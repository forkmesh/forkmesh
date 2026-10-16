import ast
import asyncio
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

PACKAGE = Path(__file__).resolve().parents[1]
ENTRY = PACKAGE.parents[2] / "app" / "src" / "entry.py"
CONSTANTS = ("PLUGIN_MEMBER_EMAILS_PERMISSION", "PLUGIN_MEMBER_EMAILS_LIMIT")
GRANTED = {"crm": frozenset(item["id"] for item in json.loads(
    (PACKAGE / "manifest.json").read_text(encoding="utf-8"))["permissions"])}
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import crm  # noqa: E402


def _member_emails(db):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    runtime = next(node for node in tree.body
                   if isinstance(node, ast.ClassDef) and node.name == "_PluginApiRuntime")
    method = next(node for node in runtime.body
                  if isinstance(node, ast.AsyncFunctionDef)
                  and node.name == "organization_member_emails")
    constants = [node for node in tree.body if isinstance(node, ast.Assign)
                 and any(getattr(target, "id", "") in CONSTANTS for target in node.targets)]

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def decrypt_row(_env, value):
        return json.loads(value)

    namespace = {
        "d1_all": d1_all, "d1_first": d1_first, "decrypt_row": decrypt_row,
        "_account_kind": lambda rec: rec.get("kind", "user"),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_account_email": lambda value: bool(re.fullmatch(r"[^@\s]+@[^@\s]+\.[a-z]+", value)),
        "plugin_data": SimpleNamespace(
            permissions=lambda plugin: GRANTED.get(plugin, frozenset())),
    }
    exec(compile(ast.Module(body=constants + [method], type_ignores=[]),
                 str(ENTRY), "exec"), namespace)
    return namespace["organization_member_emails"]


def _database():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript("""
      CREATE TABLE users(user_bi TEXT PRIMARY KEY,data TEXT);
      CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,
        PRIMARY KEY(org_bi,member_bi));
      CREATE TABLE org_plugin_installs(org_bi TEXT,plugin TEXT);
      INSERT INTO org_plugin_installs VALUES('org:acme','crm');
    """)
    for name, rec in (
            ("alice", {"status": "active", "email": "alice@acme.test", "email_verified": True}),
            ("kim", {"status": "active", "email": " Kim@Acme.test", "email_verified": True}),
            ("lou", {"status": "active", "email": "lou@acme.test", "email_verified": True}),
            ("unverified", {"status": "active", "email": "new@acme.test"}),
            ("closed", {"status": "deleted", "email": "gone@acme.test", "email_verified": True}),
            ("relay", {"status": "active", "kind": "node", "email": "relay@acme.test",
                       "email_verified": True}),
            ("outsider", {"status": "active", "email": "out@other.test", "email_verified": True})):
        db.execute("INSERT INTO users VALUES(?,?)", ("bi:" + name, json.dumps(rec)))
        if name != "outsider":
            db.execute("INSERT INTO org_members VALUES('org:acme',?,?,?)",
                       ("bi:" + name, "owner" if name == "alice" else "member", name))
    db.execute("INSERT INTO org_members VALUES('org:other','bi:outsider','owner','outsider')")
    return db


def _runtime(plugin="crm"):
    db = _database()

    async def plugin_installed(org_bi, name):
        return bool(db.execute("SELECT 1 FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
                               (org_bi, name)).fetchone())

    return SimpleNamespace(env=None, _plugin=plugin, plugin_installed=plugin_installed), db


def test_a_member_gets_the_other_members_verified_addresses():
    runtime, db = _runtime()
    emails = _member_emails(db)
    assert asyncio.run(emails(runtime, "org:acme", "bi:alice")) == [
        "kim@acme.test", "lou@acme.test"]
    assert asyncio.run(emails(runtime, "org:acme", "bi:kim")) == [
        "alice@acme.test", "lou@acme.test"]


def test_outsiders_and_packages_without_the_permission_get_nothing():
    runtime, db = _runtime()
    emails = _member_emails(db)
    assert asyncio.run(emails(runtime, "org:acme", "bi:outsider")) == []
    assert asyncio.run(emails(runtime, "org:other", "bi:outsider")) == []
    other, _ = _runtime("analytics")
    assert asyncio.run(emails(other, "org:acme", "bi:alice")) == []


def test_the_crm_summary_offers_them_for_cc():
    payload = {}

    class Runtime:
        async def organization_member_emails(self, org_bi, account_bi):
            payload["asked"] = (org_bi, account_bi)
            return ["Kim@Acme.test", "kim@acme.test", "not-an-address", "lou@acme.test"]

    found = asyncio.run(crm._member_emails(Runtime(), "org:acme", "bi:alice"))
    assert found == ["kim@acme.test", "lou@acme.test"]
    assert payload["asked"] == ("org:acme", "bi:alice")
    assert asyncio.run(crm._member_emails(SimpleNamespace(), "org:acme", "bi:alice")) == []
    source = (PACKAGE / "content" / "worker" / "crm.py").read_text(encoding="utf-8")
    summary = source.split("async def _summary(", 1)[1].split("\nasync def ", 1)[0]
    assert '"memberEmails": await _member_emails(runtime, org_bi, account_bi)' in summary
