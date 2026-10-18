import asyncio
import importlib
import json
from pathlib import Path
import re
import sqlite3
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


class Request:
    def __init__(self, method, body=None, actor="ada"):
        self.method = method
        self.body = body
        self.actor = actor


def load():
    sys.modules.pop("actions_secrets", None)
    module = importlib.import_module("actions_secrets")
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        "\n".join(part + ";" for part in (
            ROOT / "migrations" / "0272_actions_instance.sql").read_text()
            .split("INSERT INTO schema_meta")[0].split(";")
            if "CREATE TABLE IF NOT EXISTS repo_action_secrets" in part))
    db.execute("CREATE TABLE org_repos (org_bi TEXT, repo TEXT, "
               "node_owner TEXT)")
    db.execute("INSERT INTO org_repos VALUES ('org:acme','site','forkmesh')")
    roles = {("org:acme", "ada"): "admin", ("org:acme", "bob"): "member"}
    audits = []

    async def d1_all(_env, sql, *args):
        return [dict(r) for r in db.execute(sql, args).fetchall()]

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def session(_env, request, _data=None):
        return "", {"name": request.actor} if request.actor else None

    async def body(request, _limit=None):
        if not isinstance(request.body, (dict, list)):
            raise ValueError("bad")
        return request.body

    async def org_row(_env, name):
        return "org:" + name, ({"name": name} if name == "acme" else None)

    async def org_role(_env, org_bi, actor):
        return roles.get((org_bi, actor), "")

    async def owns(_env, actor, node):
        return False

    async def audit(_env, actor, action, target_type, target, outcome,
                    details=None):
        audits.append((actor, action, target, outcome, details))

    async def nothing(*_args):
        return None

    async def encrypt(_env, value):
        return "sealed:" + json.dumps(value)

    module._bind_runtime({
        "json_response": lambda data, status=200, **_: {
            "status": status, "data": data},
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", value or "")),
        "_account_session_record": session,
        "bounded_json_request": body,
        "_account_owns_node": owns,
        "_org_row": org_row,
        "_org_role": org_role,
        "_audit_sensitive_action": audit,
        "ensure_schema": nothing,
        "encrypt_row": encrypt,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "Date": type("Date", (), {"now": staticmethod(lambda: 1000)}),
    })
    return module, db, audits


def call(coroutine):
    return asyncio.run(coroutine)


def test_repo_secrets_are_set_listed_by_name_and_deleted():
    module, db, audits = load()
    put = call(module.repo_handler(
        None, Request("PUT", {"value": "s3cret"}), "forkmesh", "site",
        "DEPLOY_TOKEN"))
    assert put == {"status": 200, "data": {
        "ok": True, "name": "DEPLOY_TOKEN", "updatedAt": 1000}}
    stored = db.execute("SELECT * FROM repo_action_secrets").fetchone()
    assert stored["value"] == 'sealed:{"value": "s3cret"}'
    assert stored["updated_by"] == "ada"
    listing = call(module.repo_handler(
        None, Request("GET"), "forkmesh", "site", ""))
    assert listing["data"]["secrets"] == [
        {"name": "DEPLOY_TOKEN", "updatedAt": 1000, "updatedBy": "ada"}]
    assert "s3cret" not in json.dumps(listing)
    call(module.repo_handler(None, Request("DELETE"), "forkmesh", "site",
                             "DEPLOY_TOKEN"))
    assert db.execute("SELECT COUNT(*) FROM repo_action_secrets"
                      ).fetchone()[0] == 0
    assert [(a[1], a[3]) for a in audits] == [
        ("actions.secret_set", "success"),
        ("actions.secret_delete", "success")]
    assert all("s3cret" not in json.dumps(a) for a in audits)


def test_names_values_and_methods_are_checked():
    module, _db, _audits = load()

    def put(name, value):
        return call(module.repo_handler(
            None, Request("PUT", {"value": value}), "forkmesh", "site",
            name))["status"]

    assert put("lower", "x") == 400
    assert put("9START", "x") == 400
    assert put("A" * 65, "x") == 400
    assert put("OK", "") == 400
    assert put("OK", 5) == 400
    assert put("OK", "x" * (48 * 1024 + 1)) == 400
    assert put("OK", "x" * (48 * 1024)) == 200
    assert call(module.repo_handler(
        None, Request("POST"), "forkmesh", "site", "OK"))["status"] == 405
    assert call(module.repo_handler(
        None, Request("GET"), "forkmesh", "site", "OK"))["status"] == 404
    assert call(module.repo_handler(
        None, Request("PUT", "nope"), "forkmesh", "site", "OK"))[
            "status"] == 400


def test_only_owners_and_org_admins_may_manage_secrets():
    module, db, audits = load()
    assert call(module.repo_handler(
        None, Request("GET", actor=""), "forkmesh", "site", ""))[
            "status"] == 401
    assert call(module.repo_handler(
        None, Request("PUT", {"value": "x"}, actor="bob"), "forkmesh", "site",
        "A"))["status"] == 403
    assert audits[-1][1:4] == ("actions.secret_set", "forkmesh/site",
                               "denied")
    assert call(module.repo_handler(
        None, Request("GET", actor="forkmesh"), "forkmesh", "site", ""))[
            "status"] == 200
    assert call(module.repo_handler(
        None, Request("GET"), "forkmesh", "other", ""))["status"] == 403


def test_org_secrets_are_stored_with_an_empty_repository():
    module, db, _audits = load()
    assert call(module.org_handler(
        None, Request("PUT", {"value": "v"}), "Acme", "SHARED"))[
            "status"] == 200
    assert tuple(db.execute(
        "SELECT owner,repo,name FROM repo_action_secrets").fetchone()) == (
            "acme", "", "SHARED")
    assert call(module.org_handler(
        None, Request("GET", actor="bob"), "acme", ""))["status"] == 403
    assert call(module.org_handler(
        None, Request("GET"), "ghost", ""))["status"] == 403
    assert call(module.org_handler(
        None, Request("GET"), "Bad_Org", ""))["status"] == 404
