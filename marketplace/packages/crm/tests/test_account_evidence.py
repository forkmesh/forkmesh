import ast
import asyncio
import json
import sqlite3
from pathlib import Path
from types import SimpleNamespace


ENTRY = Path(__file__).resolve().parents[4] / "app" / "src" / "entry.py"
NOW = 2_000_000_000_000
DAY = 86_400_000
CONSTANTS = ("PLUGIN_ACCOUNT_EVIDENCE_PERMISSION", "PLUGIN_ACCOUNT_EVIDENCE_LIMIT",
             "PLUGIN_MEMBER_ROLES")
MANIFEST = Path(__file__).resolve().parents[1] / "manifest.json"
GRANTED = {"crm": frozenset(item["id"] for item in json.loads(
    MANIFEST.read_text(encoding="utf-8"))["permissions"])}


def _account_evidence(db, admins):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    runtime = next(node for node in tree.body
                   if isinstance(node, ast.ClassDef) and node.name == "_PluginApiRuntime")
    method = next(node for node in runtime.body
                  if isinstance(node, ast.AsyncFunctionDef)
                  and node.name == "account_evidence")
    constants = [node for node in tree.body if isinstance(node, ast.Assign)
                 and any(getattr(target, "id", "") in CONSTANTS for target in node.targets)]

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def blind_index(_env, value):
        return "bi:" + value

    async def decrypt_row(_env, value):
        return json.loads(value)

    async def is_admin(_env, name):
        return name in admins

    namespace = {
        "json": json, "d1_all": d1_all, "blind_index": blind_index,
        "decrypt_row": decrypt_row, "_is_admin": is_admin,
        "_account_kind": lambda rec: rec.get("kind", "user"),
        "Date": SimpleNamespace(now=lambda: NOW),
        "plugin_data": SimpleNamespace(
            permissions=lambda plugin: GRANTED.get(plugin, frozenset())),
    }
    exec(compile(ast.Module(body=constants + [method], type_ignores=[]),
                 str(ENTRY), "exec"), namespace)
    return namespace["account_evidence"]


def _database():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript("""
      CREATE TABLE users(user_bi TEXT PRIMARY KEY,data TEXT,email_bi TEXT,username TEXT,
        is_admin INTEGER,ip_bi TEXT);
      CREATE TABLE role_grants(account_bi TEXT,role TEXT,scope_type TEXT,scope_bi TEXT,
        granted_by_bi TEXT,granted_at INTEGER,expires_at INTEGER,revoked_at INTEGER);
      CREATE TABLE plan_trials(account_bi TEXT PRIMARY KEY,tier TEXT,started_at INTEGER,
        ends_at INTEGER);
    """)
    for name, email, rec in (
            ("payer", "payer@example.com", {"status": "active", "created_at": 10}),
            ("trialist", "trial@example.com", {"status": "active", "created_at": 20}),
            ("lapsed", "lapsed@example.com", {"status": "active", "created_at": 30}),
            ("plain", "plain@example.com", {"status": "active", "created_at": 40}),
            ("closed", "closed@example.com", {"status": "deleted", "created_at": 50}),
            ("relay", "relay@example.com", {"status": "active", "kind": "node"})):
        db.execute("INSERT INTO users VALUES(?,?,?,?,0,'')",
                   ("u:" + name, json.dumps(rec), "bi:" + email, name))
    for account, role, scope, granted, expires, revoked in (
            ("u:payer", "supporting_member", "platform", 100, NOW + DAY, 0),
            ("u:payer", "pro_member", "platform", 200, NOW + 2 * DAY, 0),
            ("u:payer", "pro_member", "promotion", 300, NOW + DAY, 1),
            ("u:trialist", "supporting_member", "promotion", 400, NOW + DAY, 0),
            ("u:lapsed", "supporting_member", "platform", 500, NOW - DAY, 0),
            ("u:plain", "admin", "platform", 600, 0, 0)):
        db.execute("INSERT INTO role_grants VALUES(?,?,?,'','',?,?,?)",
                   (account, role, scope, granted, expires, revoked))
    db.execute("INSERT INTO plan_trials VALUES('u:trialist','supporter',400,?)",
               (NOW + DAY,))
    return db


def test_only_an_administrator_using_the_crm_learns_who_pays():
    evidence = _account_evidence(_database(), admins={"jett"})
    crm = SimpleNamespace(env=None, _plugin="crm")
    assert asyncio.run(evidence(crm, "kaif", ["payer@example.com"])) is None
    other = SimpleNamespace(env=None, _plugin="analytics")
    assert asyncio.run(evidence(other, "jett", ["payer@example.com"])) is None
    assert asyncio.run(evidence(crm, "jett", [])) == {}


def test_paid_prize_and_trial_memberships_come_back_per_address():
    evidence = _account_evidence(_database(), admins={"jett"})
    crm = SimpleNamespace(env=None, _plugin="crm")
    found = asyncio.run(evidence(crm, "jett", [
        " Payer@Example.com", "trial@example.com", "lapsed@example.com",
        "plain@example.com", "closed@example.com", "relay@example.com",
        "nobody@example.com", "not-an-address"]))
    assert set(found) == {"payer@example.com", "trial@example.com",
                          "lapsed@example.com", "plain@example.com"}
    payer = found["payer@example.com"]
    assert payer["account"] == "payer" and payer["signedUpAt"] == 10
    assert payer["paid"] == {"tier": "pro", "since": 200, "until": NOW + 2 * DAY,
                             "active": True}
    assert payer["promotion"] is None and payer["trial"] is None
    trialist = found["trial@example.com"]
    assert trialist["paid"] is None
    assert trialist["promotion"]["tier"] == "supporter" and trialist["promotion"]["active"]
    assert trialist["trial"] == {"startedAt": 400, "endsAt": NOW + DAY, "active": True}
    assert found["lapsed@example.com"]["paid"]["active"] is False
    plain = found["plain@example.com"]
    assert (plain["paid"], plain["promotion"], plain["trial"]) == (None, None, None)
    assert plain["signedUpAt"] == 40
