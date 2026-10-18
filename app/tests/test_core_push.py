import ast
import asyncio
import hashlib
import importlib
import json
import sqlite3
import sys
from pathlib import Path

import pytest
from protection_fakes import UNPROTECTED  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

NOW = 1_790_000_000_000
MIGRATION = ROOT / "migrations" / "0205_git_tag_moves.sql"
ENTRY = SRC / "entry.py"


def _entry_function(name, extra):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.AsyncFunctionDef, ast.FunctionDef))
        and item.name == name)
    namespace = dict(extra)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(ENTRY), "exec"),
         namespace)
    return namespace[name]


class _Date:
    @staticmethod
    def now():
        return NOW


def _permitted(org_links, roles, owned=()):
    async def d1_all(_env, sql, owner, repo):
        assert "FROM org_repos WHERE node_owner=? AND repo=?" in sql
        return [{"org_bi": org} for org in org_links.get((owner, repo), ())]

    async def _org_role(_env, org_bi, account):
        return roles.get((org_bi, account), "")

    async def _account_owns_node(_env, account, node):
        return account == node or (account, node) in owned

    return _entry_function("_core_push_permitted", {
        "d1_all": d1_all, "_org_role": _org_role,
        "_account_owns_node": _account_owns_node})


def test_only_organization_owners_may_push_an_organization_repository():
    permitted = _permitted(
        {("fmnext-n1", "forkmesh"): ("org:forkmesh",)},
        {("org:forkmesh", "jett"): "owner",
         ("org:forkmesh", "mdkaifansari04"): "owner",
         ("org:forkmesh", "maint"): "maintainer",
         ("org:forkmesh", "admin"): "admin",
         ("org:forkmesh", "member"): "member"},
        owned={("fmnext", "fmnext-n1")})
    run = lambda account: asyncio.run(
        permitted(object(), "fmnext-n1", "forkmesh", account))
    assert run("jett") and run("mdkaifansari04") and run("JETT")
    for account in ("maint", "admin", "member", "stranger", "fmnext",
                    "fmnext-n1", ""):
        assert not run(account), account


def test_a_personal_core_repository_takes_pushes_from_its_owner_only():
    permitted = _permitted({}, {}, owned={("fmnext", "fmnext-n1")})
    run = lambda account: asyncio.run(
        permitted(object(), "fmnext-n1", "tools", account))
    assert run("fmnext")
    assert not run("fmnext-n1")
    assert not run("jett")


def test_permission_errors_fail_closed():
    async def d1_all(*_args):
        raise RuntimeError("database unavailable")

    permitted = _entry_function("_core_push_permitted", {"d1_all": d1_all})
    assert not asyncio.run(permitted(object(), "fmnext-n1", "forkmesh", "jett"))


class _Headers(dict):
    def get(self, key, default=None):
        for name, value in self.items():
            if name.lower() == str(key).lower():
                return value
        return default


class _Request:
    def __init__(self, data, headers=None, method="POST"):
        self.method = method
        self.headers = _Headers(headers or {})
        self._data = data


@pytest.fixture
def module():
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript(MIGRATION.read_text(encoding="utf-8"))
    audits, webhooks, pings = [], [], []
    records = {}
    state = {"session": "", "keys": {}, "now": NOW, "cores": [],
             "cores_answer": ("deleted", 200), "trash_calls": [],
             "trash": {"entries": [{"branch": "pr/304", "oid": "a" * 40}],
                       "truncated": False, "cores": ["fmnext-n1"],
                       "unanswered": ["fmnext-n2"]}}

    class Clock:
        @staticmethod
        def now():
            return state["now"]

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in database.execute(sql, args).fetchall()]

    async def d1_first(_env, sql, *args):
        rows = await d1_all(_env, sql, *args)
        return rows[0] if rows else None

    async def d1_run(_env, sql, *args):
        database.execute(sql, args)

    async def blind_index(_env, value):
        return "bi:" + value

    async def decrypt_row(_env, data):
        return data

    async def _audit_sensitive_action(_env, actor, action, target_type,
                                      target, outcome, details=None):
        audits.append({"actor": actor, "action": action, "target": target,
                       "outcome": outcome, "details": dict(details or {})})

    async def queue_repo_webhook_event(_env, owner, repo, event, data, digest,
                                       occurred_at=0):
        webhooks.append((owner, repo, event, data, digest))
        return 1

    async def notify_code_push(_env, owner, repo, **kwargs):
        pings.append((owner, repo, kwargs))
        return 1

    async def ed25519_verify(pubkey, sig, message):
        return sig == "sig:" + pubkey + ":" + hashlib.sha256(
            bytes(message)).hexdigest()

    async def _git_push_account_for_pubkey(_env, pubkey):
        return state["keys"].get(pubkey, "")

    async def _authed_account_name(_env, _request, _data):
        return state["session"]

    async def _org_repo_node(_env, org, repo):
        return "fmnext-n1" if (org, repo) == ("forkmesh", "forkmesh") else ""

    async def _core_push_permitted(_env, node, repo, account):
        return (node, repo, account) in {
            ("fmnext-n1", "forkmesh", "jett"),
            ("fmnext-n1", "forkmesh", "mdkaifansari04")}

    async def bounded_json_request(request):
        return request._data

    async def ensure_schema(_env):
        return None

    def json_response(data, status=200, extra_headers=None, **_kwargs):
        state["headers"] = extra_headers
        return {"status": status, "data": data}

    class _RouteHandlers:
        @staticmethod
        async def _branch_op_on_cores(_env, _request, owner, repo, repo_bi,
                                      target, record, data, default_branch,
                                      actor):
            state["cores"].append({
                "owner": owner, "repo": repo, "repo_bi": repo_bi,
                "target": target, "record": dict(record), "data": data,
                "default": default_branch, "actor": actor})
            if state["cores_answer"] is None:
                return None
            outcome, status = state["cores_answer"]
            return json_response(
                {"status": outcome, "branch": record["branch"]}, status=status)

    async def _branch_trash_on_cores(_env, _request, owner, repo):
        state["trash_calls"].append((owner, repo))
        return state["trash"]

    sys.modules.pop("core_push", None)
    loaded = importlib.import_module("core_push")
    loaded._bind_runtime({
        "Date": Clock, "d1_all": d1_all, "d1_first": d1_first,
        "d1_run": d1_run, "blind_index": blind_index,
        "decrypt_row": decrypt_row,
        "_audit_sensitive_action": _audit_sensitive_action,
        "queue_repo_webhook_event": queue_repo_webhook_event,
        "notify_code_push": notify_code_push,
        "ed25519_verify": ed25519_verify,
        "_git_push_account_for_pubkey": _git_push_account_for_pubkey,
        "_authed_account_name": _authed_account_name,
        "_org_repo_node": _org_repo_node,
        "_core_push_nodes": lambda _env: {"fmnext-n1", "fmnext-n2"},
        "_core_push_permitted": _core_push_permitted,
        "_route_handlers": _RouteHandlers,
        "_branch_trash_on_cores": _branch_trash_on_cores,
        "bounded_json_request": bounded_json_request,
        "ensure_schema": ensure_schema,
        "json_response": json_response,
        "EXPECTED_DEGRADED_HEADERS": {"x-forkmesh-expected-degraded": "1"},
        "method_name": lambda request: request.method,
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "safe_segment": lambda value: value if value and "/" not in value
        else "",
        "valid_node_name": lambda value: bool(value) and value.replace(
            "-", "").isalnum(),
        "valid_node_pubkey": lambda value: len(str(value or "")) == 43,
        "js_nullish": lambda value: value is None,
        "MAX_NODE_NAME": 64,
        "LOGIN_MAX_SKEW_MS": 5 * 60 * 1000,
    })
    yield loaded, {"db": database, "audits": audits, "webhooks": webhooks,
                   "pings": pings, "state": state, "records": records}
    sys.modules.pop("core_push", None)


def _sign(module, pubkey, owner, repo, ref, target, ts=NOW):
    message = module.tag_move_message(owner, repo, ref, target, ts)
    return {
        "X-ForkMesh-Pubkey": pubkey, "X-ForkMesh-Ts": str(ts),
        "X-ForkMesh-Sig": "sig:" + pubkey + ":" + hashlib.sha256(
            message).hexdigest()}


def _authorize(module, data, headers=None):
    return asyncio.run(module.tag_moves_handler(
        object(), _Request(data, headers)))


def test_an_owner_key_authorizes_one_audited_tag_move(module):
    loaded, ctx = module
    key = "K" * 43
    ctx["state"]["keys"][key] = "jett"
    body = {"owner": "forkmesh", "repo": "forkmesh", "ref": "v1.2.4",
            "target": "a" * 40}
    response = _authorize(loaded, body, _sign(
        loaded, key, "forkmesh", "forkmesh", "refs/tags/v1.2.4", "a" * 40))
    assert response["status"] == 200
    assert response["data"]["ref"] == "refs/tags/v1.2.4"
    assert ctx["audits"][-1]["action"] == "git.tag_move_authorize"
    assert ctx["audits"][-1]["outcome"] == "success"
    assert ctx["audits"][-1]["target"] == "fmnext-n1/forkmesh"
    moves = asyncio.run(loaded.pending_tag_moves(
        object(), "fmnext-n1", "forkmesh", "jett"))
    assert moves == {"refs/tags/v1.2.4": "a" * 40}
    assert asyncio.run(loaded.pending_tag_moves(
        object(), "fmnext-n1", "forkmesh", "mdkaifansari04")) == {}
    ctx["state"]["now"] = NOW + loaded.TAG_MOVE_TTL_MS + 1
    assert asyncio.run(loaded.pending_tag_moves(
        object(), "fmnext-n1", "forkmesh", "jett")) == {}


def test_tag_move_authorization_refuses_non_owners_and_bad_signatures(module):
    loaded, ctx = module
    key = "K" * 43
    ctx["state"]["keys"][key] = "maint"
    body = {"owner": "forkmesh", "repo": "forkmesh", "ref": "v1",
            "target": "a" * 40}
    signed = _sign(loaded, key, "forkmesh", "forkmesh", "refs/tags/v1",
                   "a" * 40)
    assert _authorize(loaded, body, signed)["status"] == 403
    assert ctx["audits"][-1]["outcome"] == "denied"
    ctx["state"]["keys"][key] = "jett"
    forged = dict(signed, **{"X-ForkMesh-Sig": "sig:other"})
    assert _authorize(loaded, body, forged)["status"] == 401
    other_target = dict(body, target="b" * 40)
    assert _authorize(loaded, other_target, signed)["status"] == 401
    stale = _sign(loaded, key, "forkmesh", "forkmesh", "refs/tags/v1",
                  "a" * 40, ts=NOW - 10 * 60 * 1000)
    assert _authorize(loaded, body, stale)["status"] == 401
    assert _authorize(loaded, body)["status"] == 401
    ctx["state"]["session"] = "jett"
    assert _authorize(loaded, body)["status"] == 200
    for bad in ({"ref": "../x"}, {"target": "0" * 40}, {"target": "xyz"},
                {"ref": "v1.lock"}):
        assert _authorize(loaded, dict(body, **bad))["status"] == 400
    personal = dict(body, owner="alice", repo="app")
    assert _authorize(loaded, personal)["status"] == 409


def _report(*updates):
    return {"total": len(updates), "updates": [
        {"ref": ref, "old": old, "new": new, "commits": commits}
        for ref, old, new, commits in updates]}


def test_every_accepted_ref_update_is_audited_with_pusher_and_shas(module):
    loaded, ctx = module
    report = _report(
        ("refs/heads/feature", "0" * 40, "1" * 40, 4),
        ("refs/tags/v2", "0" * 40, "2" * 40, 0))
    report["total"] = 45
    asyncio.run(loaded.record_accepted_push(
        object(), "fmnext-n1", "forkmesh", {"node": "fmnext-n2"}, "jett",
        report, core=False, credential="session"))
    pushes = [a for a in ctx["audits"] if a["action"] == "git.push"]
    assert pushes[0] == {
        "actor": "jett", "action": "git.push",
        "target": "fmnext-n1/forkmesh", "outcome": "success",
        "details": {"ref": "refs/heads/feature", "old": "0" * 40,
                    "new": "1" * 40, "node": "fmnext-n2",
                    "credential": "session", "commits": 4}}
    assert pushes[1]["details"]["ref"] == "refs/tags/v2"
    assert pushes[2]["details"] == {"node": "fmnext-n2", "refsNotListed": 43}
    assert ctx["webhooks"] == [] and ctx["pings"] == []


def test_a_core_push_fires_webhooks_apps_events_and_pings_naming_the_pusher(
        module):
    loaded, ctx = module
    ctx["db"].execute(
        "CREATE TABLE repositories (key_bi TEXT, data TEXT)")
    main = ("refs/heads/main", "1" * 40, "3" * 40, 2)
    asyncio.run(loaded.record_accepted_push(
        object(), "fmnext-n1", "forkmesh", {"node": "fmnext-n1"},
        "mdkaifansari04", _report(main), core=True, state="e" * 64))
    events = [(event, digest) for _o, _r, event, _d, digest in ctx["webhooks"]]
    assert events[0][0] == "repository.push"
    assert events[1] == ("branch.push", "3" * 40)
    push_data = ctx["webhooks"][0][3]
    assert push_data["pusher"] == "mdkaifansari04"
    assert push_data["before"] == "1" * 40 and push_data["after"] == "3" * 40
    assert push_data["defaultBranchMoved"] is True
    assert push_data["refsDigest"] == "e" * 64
    ((owner, repo, ping),) = ctx["pings"]
    assert (owner, repo) == ("fmnext-n1", "forkmesh")
    assert ping["pusher"] == "mdkaifansari04"
    assert ping["pusher_account"] == "mdkaifansari04"
    assert ping["branch"] == "main" and ping["commit"] == "3" * 40
    assert ping["commits"] == 2

    ctx["webhooks"].clear()
    ctx["pings"].clear()
    side = ("refs/heads/topic", "0" * 40, "4" * 40, 1)
    asyncio.run(loaded.record_accepted_push(
        object(), "fmnext-n1", "forkmesh", {"node": "fmnext-n1"}, "jett",
        _report(side), core=True, state="e" * 64))
    assert [event for _o, _r, event, _d, _g in ctx["webhooks"]] == [
        "repository.push"]
    digest = ctx["webhooks"][0][4]
    assert digest == loaded.updates_digest(_report(side)["updates"])
    assert ctx["pings"][0][2]["branch"] == ""
    assert ctx["pings"][0][2]["key"] == digest


def test_an_applied_tag_move_is_consumed_once_and_audited(module):
    loaded, ctx = module
    ctx["state"]["session"] = "jett"
    _authorize(loaded, {"owner": "forkmesh", "repo": "forkmesh",
                        "ref": "v1.2.4", "target": "b" * 40})
    moves = asyncio.run(loaded.pending_tag_moves(
        object(), "fmnext-n1", "forkmesh", "jett"))
    moved = ("refs/tags/v1.2.4", "a" * 40, "b" * 40, 0)
    asyncio.run(loaded.record_accepted_push(
        object(), "fmnext-n1", "forkmesh", {"node": "fmnext-n1"}, "jett",
        _report(moved), core=False, tag_moves=moves))
    audit = [a for a in ctx["audits"] if a["action"] == "git.tag_move"]
    assert audit == [{
        "actor": "jett", "action": "git.tag_move",
        "target": "fmnext-n1/forkmesh", "outcome": "success",
        "details": {"ref": "refs/tags/v1.2.4", "old": "a" * 40,
                    "new": "b" * 40}}]
    assert asyncio.run(loaded.pending_tag_moves(
        object(), "fmnext-n1", "forkmesh", "jett")) == {}
    row = ctx["db"].execute(
        "SELECT used_at, used_from FROM git_tag_moves").fetchone()
    assert tuple(row) == (NOW, "a" * 40)


def test_webhook_payload_is_json_serializable(module):
    loaded, ctx = module
    ctx["db"].execute("CREATE TABLE repositories (key_bi TEXT, data TEXT)")
    asyncio.run(loaded.record_accepted_push(
        object(), "fmnext-n1", "forkmesh", {"node": "fmnext-n1"}, "jett",
        _report(("refs/heads/main", "0" * 40, "1" * 40, 1)), core=True))
    json.dumps(ctx["webhooks"])


def _sign_delete(module, pubkey, owner, repo, branch, expect, ts=NOW):
    message = module.branch_delete_message(owner, repo, branch, expect, ts)
    return {
        "X-ForkMesh-Pubkey": pubkey, "X-ForkMesh-Ts": str(ts),
        "X-ForkMesh-Sig": "sig:" + pubkey + ":" + hashlib.sha256(
            message).hexdigest()}


def _delete(module, data, headers=None):
    return asyncio.run(module.branch_deletes_handler(
        object(), _Request(data, headers)))


def test_an_owner_key_deletes_a_branch_on_every_core(module):
    loaded, ctx = module
    key = "K" * 43
    ctx["state"]["keys"][key] = "jett"
    body = {"owner": "forkmesh", "repo": "forkmesh",
            "branch": "refs/heads/pr/304", "expect": "a" * 40}
    response = _delete(loaded, body, _sign_delete(
        loaded, key, "forkmesh", "forkmesh", "pr/304", "a" * 40))
    assert response == {"status": 200,
                        "data": {"status": "deleted", "branch": "pr/304"}}
    call = ctx["state"]["cores"][-1]
    assert call["owner"] == "fmnext-n1" and call["repo"] == "forkmesh"
    assert call["target"] == "fmnext-n1/forkmesh"
    assert call["repo_bi"] == "bi:fmnext-n1/forkmesh"
    assert call["actor"] == "jett"
    assert call["record"]["op"] == "delete"
    assert call["record"]["branch"] == "pr/304"
    assert call["record"]["expectedOid"] == "a" * 40
    assert call["record"]["source"] == "source"
    assert call["data"] == {}
    resumed = dict(body, requestId="r" * 32, node="fmnext-n2", pinBase="main",
                   extra="ignored")
    _delete(loaded, resumed, _sign_delete(
        loaded, key, "forkmesh", "forkmesh", "pr/304", "a" * 40))
    assert ctx["state"]["cores"][-1]["data"] == {
        "requestId": "r" * 32, "node": "fmnext-n2", "pinBase": "main"}


def test_branch_deletes_refuse_non_owners_bad_signatures_and_protected_refs(
        module):
    loaded, ctx = module
    key = "K" * 43
    ctx["state"]["keys"][key] = "maint"
    body = {"owner": "forkmesh", "repo": "forkmesh", "branch": "old",
            "expect": "a" * 40}
    signed = _sign_delete(loaded, key, "forkmesh", "forkmesh", "old", "a" * 40)
    assert _delete(loaded, body, signed)["status"] == 403
    assert ctx["audits"][-1]["action"] == "repository.branch_delete"
    assert ctx["audits"][-1]["outcome"] == "denied"
    assert ctx["state"]["cores"] == []
    ctx["state"]["keys"][key] = "jett"
    forged = dict(signed, **{"X-ForkMesh-Sig": "sig:other"})
    assert _delete(loaded, body, forged)["status"] == 401
    assert _delete(loaded, dict(body, expect="b" * 40), signed)["status"] == 401
    assert _delete(loaded, dict(body, branch="other"), signed)["status"] == 401
    stale = _sign_delete(loaded, key, "forkmesh", "forkmesh", "old", "a" * 40,
                         ts=NOW - 10 * 60 * 1000)
    assert _delete(loaded, body, stale)["status"] == 401
    tag_signature = _sign(loaded, key, "forkmesh", "forkmesh", "old", "a" * 40)
    assert _delete(loaded, body, tag_signature)["status"] == 401
    assert _delete(loaded, body)["status"] == 401
    assert ctx["state"]["cores"] == []
    for bad in ({"branch": "main"}, {"branch": "refs/heads/master"},
                {"branch": "HEAD"}, {"branch": "forkmesh/pulls"},
                {"branch": "forkmesh/pull/12"}, {"branch": "../x"},
                {"branch": "x.lock"}, {"branch": ""}, {"expect": "0" * 40},
                {"expect": "xyz"}):
        assert _delete(loaded, dict(body, **bad))["status"] == 400, bad
    ctx["state"]["session"] = "jett"
    assert _delete(loaded, dict(body, owner="alice", repo="app"))[
        "status"] == 409
    assert _delete(loaded, body)["status"] == 200
    ctx["state"]["cores_answer"] = None
    unavailable = _delete(loaded, body)
    assert unavailable["status"] == 503
    assert unavailable["data"]["error"] == "cores_unavailable"
    assert ctx["state"]["headers"] == {"x-forkmesh-expected-degraded": "1"}


def _sign_trash(module, pubkey, owner, repo, ts=NOW):
    message = module.branch_trash_message(owner, repo, ts)
    return {
        "X-ForkMesh-Pubkey": pubkey, "X-ForkMesh-Ts": str(ts),
        "X-ForkMesh-Sig": "sig:" + pubkey + ":" + hashlib.sha256(
            message).hexdigest()}


def _trash(module, data, headers=None, method="POST"):
    return asyncio.run(module.branch_trash_handler(
        object(), _Request(data, headers, method)))


def test_an_owner_key_reads_the_branches_the_cores_deleted(module):
    loaded, ctx = module
    key = "K" * 43
    ctx["state"]["keys"][key] = "jett"
    body = {"owner": "forkmesh", "repo": "forkmesh"}
    response = _trash(loaded, body, _sign_trash(
        loaded, key, "forkmesh", "forkmesh"))
    assert response["status"] == 200
    assert response["data"] == {
        "ok": True, "owner": "forkmesh", "repo": "forkmesh",
        "entries": [{"branch": "pr/304", "oid": "a" * 40}],
        "truncated": False, "cores": ["fmnext-n1"],
        "unanswered": ["fmnext-n2"], "listedAt": NOW}
    assert ctx["state"]["trash_calls"] == [("fmnext-n1", "forkmesh")]
    ctx["state"]["now"] = NOW + 10 * 1000
    again = _trash(loaded, body, _sign_trash(
        loaded, key, "forkmesh", "forkmesh", ts=NOW + 10 * 1000))
    assert again["status"] == 200
    assert ctx["state"]["trash_calls"] == [("fmnext-n1", "forkmesh")]
    ctx["state"]["now"] = NOW + 31 * 1000
    _trash(loaded, body, _sign_trash(
        loaded, key, "forkmesh", "forkmesh", ts=NOW + 31 * 1000))
    assert len(ctx["state"]["trash_calls"]) == 2
    assert ctx["audits"] == []


def test_branch_trash_refuses_everyone_but_the_owners(module):
    loaded, ctx = module
    key = "K" * 43
    ctx["state"]["keys"][key] = "maint"
    body = {"owner": "forkmesh", "repo": "forkmesh"}
    signed = _sign_trash(loaded, key, "forkmesh", "forkmesh")
    denied = _trash(loaded, body, signed)
    assert denied["status"] == 403
    assert ctx["audits"][-1]["action"] == "repository.branch_trash_read"
    assert ctx["audits"][-1]["outcome"] == "denied"
    ctx["state"]["keys"][key] = "jett"
    forged = dict(signed, **{"X-ForkMesh-Sig": "sig:other"})
    assert _trash(loaded, body, forged)["status"] == 401
    other_repo = _sign_trash(loaded, key, "forkmesh", "other")
    assert _trash(loaded, body, other_repo)["status"] == 401
    stale = _sign_trash(loaded, key, "forkmesh", "forkmesh",
                        ts=NOW - 10 * 60 * 1000)
    assert _trash(loaded, body, stale)["status"] == 401
    delete_signature = _sign_delete(
        loaded, key, "forkmesh", "forkmesh", "", "")
    assert _trash(loaded, body, delete_signature)["status"] == 401
    assert _trash(loaded, body)["status"] == 401
    assert _trash(loaded, body, signed, method="GET")["status"] == 405
    assert _trash(loaded, {"owner": "", "repo": "forkmesh"})["status"] == 400
    assert ctx["state"]["trash_calls"] == []
    ctx["state"]["session"] = "jett"
    assert _trash(loaded, {"owner": "alice", "repo": "app"})["status"] == 409
    ctx["state"]["trash"] = None
    unavailable = _trash(loaded, body)
    assert unavailable["status"] == 503
    assert unavailable["data"]["error"] == "cores_unavailable"
    assert ctx["state"]["headers"] == {"x-forkmesh-expected-degraded": "1"}
    ctx["state"]["session"] = "mdkaifansari04"
    ctx["state"]["trash"] = {"entries": [], "truncated": False,
                             "cores": ["fmnext-n1"], "unanswered": []}
    assert _trash(loaded, body)["status"] == 200


def test_a_core_trash_listing_is_validated_strictly():
    import re
    validate = _entry_function("_https_mirror_branch_trash_result", {
        "re": re,
        "BRANCH_TRASH_BRANCH_RE": re.compile(
            r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$"),
        "HTTPS_MIRROR_BRANCH_TRASH_MAX_ENTRIES": 2})
    good = {"ok": True, "node": "fmnext-n1", "truncated": False,
            "entries": [{"branch": "agent/7-fix", "oid": "a" * 40}]}
    assert validate(good, "fmnext-n1") == {
        "entries": [{"branch": "agent/7-fix", "oid": "a" * 40}],
        "truncated": False}
    for bad in (
            dict(good, node="fmnext-n2"), dict(good, ok=False),
            dict(good, extra=1), dict(good, truncated="no"),
            dict(good, entries=[{"branch": "x", "oid": "a" * 40}] * 3),
            dict(good, entries=[{"branch": "../x", "oid": "a" * 40}]),
            dict(good, entries=[{"branch": "x", "oid": "A" * 40}]),
            dict(good, entries=[{"branch": "x.lock", "oid": "a" * 40}]),
            dict(good, entries=[{"branch": "x", "oid": "a" * 40, "y": 1}]),
            [], None):
        assert validate(bad, "fmnext-n1") is None, bad


def test_the_cores_listing_is_the_union_of_every_core_that_answered(
        monkeypatch):
    import agent_branches

    answers = {
        "fmnext-n1": {"entries": [{"branch": "b", "oid": "1" * 40}],
                      "truncated": False},
        "fmnext-n2": None,
        "fmnext-n3": {"entries": [{"branch": "b", "oid": "2" * 40},
                                  {"branch": "a", "oid": "3" * 40}],
                      "truncated": False},
    }

    async def _served_owner(_env, owner, _repo):
        return "forkmesh"

    async def _mirror_context(_env, owner, repo):
        return {"owner": "fmnext-n1", "repo": repo}, ""

    async def _https_mirror_candidates(_env, _mirror, _country, operation):
        assert operation == "delete-agent-branch"
        return [{"node": node} for node in answers] + [{"node": "fmnext-n1"}]

    async def _https_mirror_branch_trash_proxy(_env, endpoint, _mirror):
        return answers[endpoint["node"]]

    for name, value in (
            ("_served_owner", _served_owner),
            ("_mirror_context", _mirror_context),
            ("_https_mirror_candidates", _https_mirror_candidates),
            ("_https_mirror_branch_trash_proxy",
             _https_mirror_branch_trash_proxy),
            ("world_request_country", lambda _request: "")):
        monkeypatch.setattr(agent_branches, name, value, raising=False)
    listing = asyncio.run(agent_branches.core_branch_trash(
        object(), object(), "fmnext-n1", "forkmesh"))
    assert listing == {
        "entries": [{"branch": "a", "oid": "3" * 40},
                    {"branch": "b", "oid": "1" * 40},
                    {"branch": "b", "oid": "2" * 40}],
        "truncated": False,
        "cores": ["fmnext-n1", "fmnext-n3"],
        "unanswered": ["fmnext-n2"]}
    answers["fmnext-n1"] = None
    answers["fmnext-n3"] = None
    assert asyncio.run(agent_branches.core_branch_trash(
        object(), object(), "fmnext-n1", "forkmesh")) is None


def test_a_main_push_queues_runner_actions_only_while_the_desktop_is_offline(
        module):
    loaded, ctx = module
    db = ctx["db"]
    db.execute("CREATE TABLE repositories (key_bi TEXT, data TEXT)")
    db.executescript(
        (ROOT / "migrations" / "0193_runner_nodes.sql").read_text()
        .split("-- The runtime schema text changed")[0])
    db.executescript(
        (ROOT / "migrations" / "0248_repo_action_jobs.sql").read_text()
        .split("INSERT INTO schema_meta")[0])
    db.executescript(
        (ROOT / "migrations" / "0272_actions_instance.sql").read_text()
        .split("INSERT INTO schema_meta")[0])
    online = {"forkmesh"}

    async def holds(_env, name):
        return name in online

    for name in ("runner_leases", "runner_actions"):
        sys.modules.pop(name, None)
    loaded._bind_runtime({"_node_events_object_holds_node": holds,
                          "_branch_protection": UNPROTECTED})
    env = type("Env", (), {"RUNNER_POOL_TARGET_NODES": "fmnext-n1,fmnext-n2"})()

    def push(ref, new, node="fmnext-n1", owner="forkmesh"):
        asyncio.run(loaded.record_accepted_push(
            env, owner, "forkmesh", {"node": node}, "jett",
            _report((ref, "1" * 40, new, 1)), core=True, state="e" * 64))
        return [tuple(row) for row in db.execute(
            "SELECT owner,repo,branch,commit_oid,target_node,status "
            "FROM repo_action_jobs ORDER BY id")]

    try:
        assert push("refs/heads/main", "3" * 40) == []
        db.execute(
            "INSERT INTO runner_nodes (name,public_key,registered_by,scope,"
            "allowed_owners,status,created_at,updated_at) "
            "VALUES ('fmnext-r1','k','root','pool','[\"forkmesh\"]','active',1,1)")
        assert push("refs/heads/main", "3" * 40) == []
        online.clear()
        assert push("refs/heads/topic", "4" * 40) == []
        assert push("refs/heads/main", "5" * 40, node="desk-1") == []
        assert push("refs/heads/main", "6" * 40) == [
            ("forkmesh", "forkmesh", "main", "6" * 40, "fmnext-n1", "queued")]
        assert asyncio.run(loaded.queue_runner_actions(
            env, "forkmesh", "forkmesh", "6" * 40, "fmnext-n2")) == 0
    finally:
        for name in ("runner_leases", "runner_actions"):
            sys.modules.pop(name, None)


def test_an_instance_queues_every_pushed_branch_with_before_and_skips_actions_pushes(
        module):
    loaded, ctx = module
    db = ctx["db"]
    db.execute("CREATE TABLE repositories (key_bi TEXT, data TEXT)")
    for name in ("0193_runner_nodes.sql", "0248_repo_action_jobs.sql",
                 "0272_actions_instance.sql"):
        db.executescript(
            (ROOT / "migrations" / name).read_text()
            .split("-- The runtime schema text changed")[0]
            .split("INSERT INTO schema_meta")[0])

    async def holds(_env, _name):
        return True

    for name in ("runner_leases", "runner_actions"):
        sys.modules.pop(name, None)
    loaded._bind_runtime({"_node_events_object_holds_node": holds,
                          "_branch_protection": UNPROTECTED})
    env = type("Env", (), {"RUNNER_POOL_TARGET_NODES": "forkmesh",
                           "ACTIONS_INSTANCE_RUNNER": "box-runner"})()

    def push(*updates):
        asyncio.run(loaded.record_accepted_push(
            env, "forkmesh", "site", {"node": "forkmesh"}, "jett",
            _report(*updates), core=True, state="e" * 64))
        return [tuple(row) for row in db.execute(
            "SELECT branch,commit_oid,before_oid,event,actor "
            "FROM repo_action_jobs ORDER BY id")]

    try:
        assert push(("refs/heads/topic", "1" * 40, "4" * 40, 1),
                    ("refs/tags/v1", "0" * 40, "5" * 40, 0),
                    ("refs/heads/gone", "2" * 40, "0" * 40, 0)) == [
            ("topic", "4" * 40, "1" * 40, "push", "jett")]
        db.execute("UPDATE repo_action_jobs SET status='completed',"
                   "pushed_commit=?", ("7" * 40,))
        assert len(push(("refs/heads/topic", "4" * 40, "7" * 40, 1))) == 1
    finally:
        for name in ("runner_leases", "runner_actions"):
            sys.modules.pop(name, None)


def test_an_instance_queues_only_the_default_branch_for_an_import_and_caps_bulk_pushes(
        module):
    loaded, _ctx = module
    zero = "0" * 40

    def heads(updates, default="main"):
        return [item["ref"][len("refs/heads/"):]
                for item in loaded.instance_action_updates(
                    _report(*updates)["updates"], default)]

    imported = [("refs/heads/b%02d" % i, zero, "%040x" % (i + 1), 1)
                for i in range(45)]
    imported.insert(7, ("refs/heads/main", zero, "f" * 40, 9))
    assert heads(imported) == ["main"]
    assert heads(imported[:8] + [("refs/tags/v1", zero, "e" * 40, 0)]) == [
        "main"]
    created = [("refs/heads/n%d" % i, zero, "%040x" % (i + 1), 1)
               for i in range(4)]
    assert heads(created) == []
    assert heads(created[:3]) == ["n0", "n1", "n2"]
    moved = [("refs/heads/m%d" % i, "1" * 40, "%040x" % (i + 1), 1)
             for i in range(8)]
    assert heads(moved + created + [
        ("refs/heads/main", "1" * 40, "2" * 40, 1)]) == [
            "main", "m0", "m1", "m2", "m3", "m4"]
    assert heads([("refs/heads/topic", "", "3" * 40, 1),
                  ("refs/heads/gone", "1" * 40, zero, 0)]) == ["topic"]
