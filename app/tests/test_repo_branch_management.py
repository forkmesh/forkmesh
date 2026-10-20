#!/usr/bin/env python3
"""Website branch management (adhoc #2098).

The relay runs no Git, so the Branches page cannot delete, update or create a
branch itself. It queues an owner-authorized request for the repository's own
node — the machine that owns the checkout every mirror is published from —
which drains it from GET /api/sync and propagates the result. These tests pin
the authorization gate, the validation, the queue bounds and the route wiring.
"""

import ast
import asyncio
import builtins
import json
import subprocess
from pathlib import Path

import pytest
from protection_fakes import UNPROTECTED  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
ROUTES_PATH = ROOT / "src" / "route_handlers.py"
ROUTES = ROUTES_PATH.read_text(encoding="utf-8")
URLS = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
DASHBOARD = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
GATEWAY = (
    ROOT.parent / "tools" / "mirror_gateway.py"
).read_text(encoding="utf-8")
DESKTOP_OPS = (
    ROOT.parent / "desktop" / "src" / "WebBranchOps.cpp"
).read_text(encoding="utf-8")
DESKTOP_SYNC = (
    ROOT.parent / "desktop" / "src" / "MainWindowPulls.cpp"
).read_text(encoding="utf-8")
DESKTOP_BRANCHES = (
    ROOT.parent / "desktop" / "src" / "MainWindowBranches.cpp"
).read_text(encoding="utf-8")


BRANCH_OP_NAMES = (
    "BRANCH_OPS",
    "MAX_PENDING_BRANCH_OPS",
    "BRANCH_OP_NAME_PATTERN",
    "BRANCH_OP_OID_PATTERN",
    "branch_op_name",
    "_branch_op_projection",
    "_branch_op_record",
    "_branch_ops_pending",
    "_branch_ops_node_drain",
    "_branch_ops_wake",
    "repository_branch_ops_handler",
    "_branch_op_on_cores",
    "MAX_BRANCH_OP_RESULTS",
    "BRANCH_OP_RESULT_STATUSES",
    "_branch_op_result_projection",
    "_branch_op_results_recent",
    "_branch_op_result_record",
    "repository_branch_op_results_handler",
)


def _load(extra):
    """Exec the branch-operation slice of route_handlers.py with stubs."""
    tree = ast.parse(ROUTES, filename=str(ROUTES_PATH))
    selected = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in BRANCH_OP_NAMES:
                selected.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in BRANCH_OP_NAMES
            for target in node.targets
        ):
            selected.append(node)
    assert len(selected) == len(BRANCH_OP_NAMES)
    namespace = {"_branch_protection": UNPROTECTED, **extra}
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])),
        str(ROUTES_PATH), "exec"), namespace)
    return namespace


class _Request:
    def __init__(self, method="POST", url=None, owner="alice", repo="widget"):
        self.method = method
        self.url = url or (
            "https://forkmesh.com/api/repo/%s/%s/branches/ops" % (owner, repo))


class _Cores:
    def __init__(self, answer=None):
        self.answer = answer
        self.calls = []

    def _bind_runtime(self, _runtime):
        return None

    async def core_branch_op(self, env, request, owner, repo, record, data,
                             default_branch="", actor=""):
        self.calls.append((record, dict(data), default_branch))
        return self.answer


def _harness(body=None, actor="alice", authorized=True, method="POST",
             node_signed=True, org_admins=None, cores=None):
    """In-memory D1 stand-in for branch_op_inbox plus the audit/notify sinks."""
    rows = []          # {"id", "repo_bi", "data", "queued_at"}
    results = []       # {"id", "repo_bi", "data", "reported_at"}
    audits = []
    notifications = []
    next_id = {"value": 1}

    class _Date:
        @staticmethod
        def now():
            return 1_700_000_000_000

    def json_response(data, status=200, extra_headers=None, **_kwargs):
        return {"status": status, "data": data,
                "headers": extra_headers or {}}

    async def ensure_schema(_env):
        return None

    async def bounded_json_request(_request):
        return dict(body or {})

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def _authorize_issue_manager(_env, _owner, _data, _request):
        if authorized:
            return True, None
        return False, json_response({"error": "not_authorized"}, status=403)

    async def _account_session_record(_env, _request, _data):
        return "bi:" + actor, {"name": actor}

    async def encrypt_row(_env, record):
        return dict(record)

    async def decrypt_row(_env, stored):
        return dict(stored)

    async def d1_all(_env, sql, *args):
        if "FROM branch_op_results" in sql:
            repo_bi, limit = args
            matching = [dict(row) for row in results
                        if row["repo_bi"] == repo_bi]
            return sorted(matching, key=lambda row: -row["id"])[:limit]
        if "FROM org_members" in sql:
            assert args[0] == "org:acme"
            return [{"name": name} for name in org_admins or []]
        assert "FROM branch_op_inbox" in sql
        return [dict(row) for row in rows if row["repo_bi"] == args[0]]

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO branch_op_inbox"):
            repo_bi, data, queued_at = args
            if len([r for r in rows if r["repo_bi"] == repo_bi]) >= 1024:
                raise RuntimeError("branch_op_inbox_repo_limit")
            rows.append({"id": next_id["value"], "repo_bi": repo_bi,
                         "data": data, "queued_at": queued_at})
            next_id["value"] += 1
            return None
        if sql.startswith("DELETE FROM branch_op_inbox WHERE repo_bi=? AND"):
            repo_bi, *ids = args
            rows[:] = [r for r in rows
                       if not (r["repo_bi"] == repo_bi and r["id"] in ids)]
            return None
        if sql.startswith("DELETE FROM branch_op_inbox"):
            row_id, repo_bi = args
            rows[:] = [r for r in rows
                       if not (r["id"] == row_id and r["repo_bi"] == repo_bi)]
            return None
        if sql.startswith("INSERT INTO branch_op_results"):
            repo_bi, data, reported_at = args
            results.append({"id": next_id["value"], "repo_bi": repo_bi,
                            "data": data, "reported_at": reported_at})
            next_id["value"] += 1
            return None
        if sql.startswith("DELETE FROM branch_op_results WHERE repo_bi=? AND"):
            repo_bi, _same, limit = args
            keep = {row["id"] for row in sorted(
                (r for r in results if r["repo_bi"] == repo_bi),
                key=lambda row: -row["id"])[:limit]}
            results[:] = [r for r in results
                          if r["repo_bi"] != repo_bi or r["id"] in keep]
            return None
        if sql.startswith("DELETE FROM branch_op_results WHERE repo_bi=?"):
            results[:] = [r for r in results if r["repo_bi"] != args[0]]
            return None
        raise AssertionError("unexpected d1_run: " + sql)

    async def _audit_sensitive_action(_env, who, action, kind, target,
                                      outcome, details):
        audits.append((who, action, kind, target, outcome, details))

    async def notify_repo_host(_env, owner, repo, topic, event_owner=""):
        notifications.append(
            (owner, repo, topic) + ((event_owner,) if event_owner else ()))

    async def _org_row(_env, org):
        if org == "acme" and org_admins is not None:
            return "org:acme", {"name": "acme"}
        return "", None

    def method_name(request):
        return request.method

    async def _authorized_repo_inbox_signing_key(_env, _request, _owner, _repo):
        return "node-key" if node_signed else ""

    import re as real_re
    from urllib.parse import parse_qs, urlparse

    cores = cores or _Cores()
    real_import = builtins.__import__

    def _import(name, *args, **kwargs):
        if name == "agent_branches":
            return cores
        return real_import(name, *args, **kwargs)

    namespace = _load({
        "__builtins__": dict(vars(builtins), __import__=_import),
        "Date": _Date,
        "json_response": json_response,
        "EXPECTED_DEGRADED_HEADERS": {"x-forkmesh-expected-degraded": "1"},
        "ensure_schema": ensure_schema,
        "bounded_json_request": bounded_json_request,
        "blind_index": blind_index,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "method_name": method_name,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "re": real_re,
        "MAX_NODE_NAME": 63,
        "_authorize_issue_manager": _authorize_issue_manager,
        "_account_session_record": _account_session_record,
        "_audit_sensitive_action": _audit_sensitive_action,
        "notify_repo_host": notify_repo_host,
        "_authorized_repo_inbox_signing_key": _authorized_repo_inbox_signing_key,
        "_org_row": _org_row,
        "REPO_API_PREFIX_RE": real_re.compile(
            r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$"),
        "safe_segment": lambda value: str(value or ""),
        "valid_node_name": lambda value: bool(
            real_re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", value or "")),
        "MAX_ORG_MEMBERS": 200,
    })
    namespace["_results"] = results
    namespace["_cores"] = cores
    return namespace, rows, audits, notifications


def _post(body, cores=None, **kwargs):
    namespace, rows, audits, notifications = _harness(
        body, cores=cores, **kwargs)
    response = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request(kwargs.get("method", "POST")), "alice", "widget"))
    return response, rows, audits, notifications, namespace


def test_delete_request_is_queued_for_the_owning_node_and_audited():
    response, rows, audits, notifications, _ns = _post({
        "op": "delete",
        "branch": "agent/finished-work",
        "expectedOid": "a" * 40,
        "defaultBranch": "main",
    })
    assert response["status"] == 200
    assert response["data"]["queued"] is True
    assert len(rows) == 1
    queued = rows[0]["data"]
    assert queued["op"] == "delete"
    assert queued["branch"] == "agent/finished-work"
    # The tip the requester saw travels with the request: the node refuses a
    # branch that moved instead of deleting commits nobody reviewed.
    assert queued["expectedOid"] == "a" * 40
    assert queued["requestedBy"] == "alice"
    assert queued["source"] == "web"
    assert response["data"]["ops"] == [{
        "id": 1,
        "op": "delete",
        "branch": "agent/finished-work",
        "base": "",
        "expectedOid": "a" * 40,
        "startOid": "",
        "baseOid": "",
        "deleteBranch": False,
        "requestedBy": "alice",
        "queuedAt": 1_700_000_000_000,
    }]
    assert audits == [(
        "alice", "repository.branch_delete", "repository", "alice/widget",
        "requested",
        {"branch": "agent/finished-work", "op": "delete", "base": ""},
    )]
    # The node applies the queue from its next sync; wake it now.
    assert notifications == [("alice", "widget", "branches")]


def test_update_and_create_carry_their_own_required_commit():
    response, rows, _audits, _notify, _ns = _post({
        "op": "update",
        "branch": "feature/login",
        "expectedOid": "b" * 40,
        "base": "main",
    })
    assert response["status"] == 200
    assert rows[0]["data"]["base"] == "main"

    response, rows, _audits, _notify, _ns = _post({
        "op": "create",
        "branch": "release/2026-09",
        "startOid": "c" * 64,
    })
    assert response["status"] == 200
    assert rows[0]["data"]["startOid"] == "c" * 64

    # A create without its start commit is not a request the node can apply.
    response, rows, _audits, _notify, _ns = _post({
        "op": "create", "branch": "release/2026-10",
    })
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_commit"
    assert rows == []


def test_merge_request_pins_both_tips_and_names_its_target():
    response, rows, audits, notifications, _ns = _post({
        "op": "merge",
        "branch": "agent/finished-work",
        "expectedOid": "a" * 40,
        "base": "main",
        "baseOid": "b" * 40,
        "deleteBranch": True,
        "defaultBranch": "main",
    })
    assert response["status"] == 200
    queued = rows[0]["data"]
    assert queued["op"] == "merge"
    assert queued["base"] == "main"
    assert queued["baseOid"] == "b" * 40
    assert queued["deleteBranch"] is True
    op = response["data"]["ops"][0]
    assert op["op"] == "merge"
    assert op["baseOid"] == "b" * 40
    assert op["deleteBranch"] is True
    assert audits[0][1] == "repository.branch_merge"
    assert audits[0][5]["base"] == "main"
    assert notifications == [("alice", "widget", "branches")]


def test_merge_defaults_to_the_default_branch_and_needs_both_tips():
    response, rows, _audits, _notify, _ns = _post({
        "op": "merge",
        "branch": "feature/x",
        "expectedOid": "a" * 40,
        "baseOid": "b" * 40,
        "defaultBranch": "main",
    })
    assert response["status"] == 200
    assert rows[0]["data"]["base"] == "main"
    assert rows[0]["data"]["deleteBranch"] is False

    response, rows, _audits, _notify, _ns = _post({
        "op": "merge", "branch": "feature/x", "expectedOid": "a" * 40,
        "base": "main",
    })
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_commit"
    assert rows == []

    response, rows, _audits, _notify, _ns = _post({
        "op": "merge", "branch": "feature/x", "expectedOid": "a" * 40,
        "baseOid": "b" * 40,
    })
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_branch"

    response, rows, _audits, _notify, _ns = _post({
        "op": "merge", "branch": "feature/x", "expectedOid": "a" * 40,
        "baseOid": "b" * 40, "base": "feature/x",
    })
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_branch"

    response, rows, _audits, _notify, _ns = _post({
        "op": "merge", "branch": "main", "expectedOid": "a" * 40,
        "baseOid": "b" * 40, "base": "develop", "defaultBranch": "main",
    })
    assert response["status"] == 409
    assert response["data"]["error"] == "default_branch"


def test_merge_into_another_branch_than_the_default_is_allowed():
    response, rows, _audits, _notify, _ns = _post({
        "op": "merge", "branch": "feature/x", "expectedOid": "a" * 40,
        "baseOid": "b" * 40, "base": "release/1.0", "defaultBranch": "main",
    })
    assert response["status"] == 200
    assert rows[0]["data"]["base"] == "release/1.0"


def _results(namespace, method="POST", url=None):
    return asyncio.run(namespace["repository_branch_op_results_handler"](
        object(),
        _Request(method, url=url or (
            "https://forkmesh.com/api/repo/alice/widget/branches/ops/results"
            "?owner=alice&ts=1&sig=x")),
        "alice", "widget"))


def test_the_signed_node_drains_its_queue_directly():
    namespace, rows, _audits, _notify = _harness({"ops": [
        {"op": "update", "branch": "feature/x", "expectedOid": "a" * 40,
         "base": "main"},
        {"op": "merge", "branch": "feature/y", "expectedOid": "b" * 40,
         "baseOid": "c" * 40, "base": "main", "deleteBranch": True},
    ]})
    asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert len(rows) == 2
    drained = asyncio.run(namespace["repository_branch_ops_handler"](
        object(),
        _Request("GET", url=(
            "https://forkmesh.com/api/repo/alice/widget/branches/ops"
            "?owner=alice&ts=1&sig=x")),
        "alice", "widget"))
    assert drained["status"] == 200
    ops = drained["data"]["ops"]
    assert [(op["id"], op["op"], op["branch"]) for op in ops] == [
        (1, "update", "feature/x"), (2, "merge", "feature/y")]
    assert ops[1]["deleteBranch"] is True
    assert rows == []

    namespace, rows, _audits, _notify = _harness(
        {"op": "delete", "branch": "feature/x", "expectedOid": "a" * 40},
        node_signed=False)
    asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    refused = asyncio.run(namespace["repository_branch_ops_handler"](
        object(),
        _Request("GET", url=(
            "https://forkmesh.com/api/repo/alice/widget/branches/ops"
            "?owner=alice&ts=1&sig=x")),
        "alice", "widget"))
    assert refused["status"] == 401
    assert len(rows) == 1


def test_an_organization_alias_wakes_the_owners_and_admins():
    namespace, rows, _audits, notifications = _harness(
        {"op": "update", "branch": "feature/x", "expectedOid": "a" * 40,
         "base": "main"},
        org_admins=["jett", "fmnext-n1", "Not A Name!"])
    response = asyncio.run(namespace["repository_branch_ops_handler"](
        object(),
        _Request("POST", url=(
            "https://forkmesh.com/api/repo/acme/widget/branches/ops")),
        "fmnext-n1", "widget"))
    assert response["status"] == 200
    assert len(rows) == 1
    assert notifications == [
        ("fmnext-n1", "widget", "branches"),
        ("jett", "widget", "branches", "acme"),
    ]


def test_node_reports_outcomes_the_page_reads_back_newest_first():
    namespace, _rows, _audits, _notify = _harness({"results": [
        {"requestId": 7, "op": "merge", "branch": "feature/x", "base": "main",
         "status": "applied", "message": "merged feature/x into main",
         "mergeOid": "C" * 40, "branchDeleted": True},
        {"requestId": 8, "op": "merge", "branch": "feature/y", "base": "main",
         "status": "refused",
         "message": "refused: merging feature/y into main conflicts"},
        {"op": "rebase", "branch": "feature/z", "status": "applied"},
        {"op": "merge", "branch": "../escape", "status": "applied"},
        {"op": "merge", "branch": "feature/w", "status": "exploded"},
        "not an object",
    ]})
    response = _results(namespace)
    assert response["status"] == 200
    assert response["data"]["recorded"] == 2
    listed = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("GET"), "alice", "widget"))
    results = listed["data"]["results"]
    assert [r["branch"] for r in results] == ["feature/y", "feature/x"]
    assert results[0]["status"] == "refused"
    assert results[1]["mergeOid"] == "c" * 40
    assert results[1]["branchDeleted"] is True
    assert results[1]["requestId"] == 7
    assert results[1]["reportedAt"] == 1_700_000_000_000


def test_only_the_signed_node_reports_and_only_managers_clear_results():
    namespace, _rows, _audits, _notify = _harness(
        {"results": [{"op": "merge", "branch": "feature/x",
                      "status": "applied"}]},
        node_signed=False)
    assert _results(namespace)["status"] == 401
    assert namespace["_results"] == []

    namespace, _rows, _audits, _notify = _harness(
        {"results": "nope"})
    assert _results(namespace)["status"] == 400

    namespace, _rows, _audits, _notify = _harness(
        {"results": [{"op": "merge", "branch": "feature/x",
                      "status": "applied"}]}, authorized=False)
    assert _results(namespace)["status"] == 200
    assert _results(namespace, "DELETE")["status"] == 403
    assert len(namespace["_results"]) == 1

    namespace, _rows, _audits, _notify = _harness(
        {"results": [{"op": "merge", "branch": "feature/x",
                      "status": "applied"}]})
    _results(namespace)
    cleared = _results(namespace, "DELETE")
    assert cleared["data"]["results"] == []
    assert namespace["_results"] == []


def test_reported_outcomes_are_bounded_per_repository():
    namespace, _rows, _audits, _notify = _harness({"results": [
        {"op": "delete", "branch": "old/%d" % index, "status": "applied"}
        for index in range(30)
    ]})
    _results(namespace)
    assert len(namespace["_results"]) == namespace["MAX_BRANCH_OP_RESULTS"]
    assert namespace["_results"][-1]["data"]["branch"] == "old/29"


def test_non_managers_cannot_queue_branch_operations():
    response, rows, audits, notifications, _ns = _post(
        {"op": "delete", "branch": "feature/x", "expectedOid": "a" * 40},
        authorized=False,
    )
    assert response["status"] == 403
    assert rows == []
    assert audits == []
    assert notifications == []


def test_default_branch_and_unsupported_operations_are_refused():
    response, rows, _audits, _notify, _ns = _post({
        "op": "delete", "branch": "main", "expectedOid": "a" * 40,
        "defaultBranch": "main",
    })
    assert response["status"] == 409
    assert response["data"]["error"] == "default_branch"
    assert rows == []

    response, rows, _audits, _notify, _ns = _post({
        "op": "rebase", "branch": "feature/x", "expectedOid": "a" * 40,
    })
    assert response["status"] == 400
    assert response["data"]["error"] == "unsupported_operation"
    assert rows == []


@pytest.mark.parametrize("branch", [
    "",
    "-delete-me",
    ".hidden",
    "with space",
    "deep/../escape",
    "double//slash",
    "trailing.lock",
    "reflog@{0}",
    "back\\slash",
    "forkmesh/pulls",
    "forkmesh/pull/12",
])
def test_unusable_branch_names_never_reach_the_queue(branch):
    response, rows, _audits, _notify, _ns = _post({
        "op": "delete", "branch": branch, "expectedOid": "a" * 40,
    })
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_branch"
    assert rows == []


def test_repeating_a_request_returns_the_existing_queue_once():
    namespace, rows, audits, _notify = _harness({
        "op": "delete", "branch": "feature/x", "expectedOid": "a" * 40,
    })
    first = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    second = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert first["data"]["queued"] is True
    assert second["data"]["queued"] is False
    assert len(rows) == 1
    assert len(audits) == 1
    assert len(second["data"]["ops"]) == 1


def test_pending_queue_is_readable_and_cancellable():
    namespace, rows, _audits, _notify = _harness({
        "op": "delete", "branch": "feature/x", "expectedOid": "a" * 40,
    })
    asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    listed = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("GET"), "alice", "widget"))
    assert [op["branch"] for op in listed["data"]["ops"]] == ["feature/x"]

    cancelled = asyncio.run(namespace["repository_branch_ops_handler"](
        object(),
        _Request(
            "DELETE",
            url="https://forkmesh.com/api/repo/alice/widget/branches/ops?id=1",
        ),
        "alice", "widget",
    ))
    assert cancelled["data"]["ops"] == []
    assert rows == []

    missing_id = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("DELETE"), "alice", "widget"))
    assert missing_id["status"] == 400


def test_queue_is_bounded_per_repository():
    namespace, rows, _audits, _notify = _harness({
        "op": "delete", "branch": "feature/new", "expectedOid": "a" * 40,
    })
    rows.extend({
        "id": 2000 + index,
        "repo_bi": "bi:alice/widget",
        "data": {"op": "delete", "branch": "kept/%d" % index,
                 "expectedOid": "a" * 40, "queuedAt": 1},
        "queued_at": 1,
    } for index in range(namespace["MAX_PENDING_BRANCH_OPS"]))
    response = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert response["status"] == 429
    assert response["data"]["error"] == "queue_full"
    assert len(rows) == namespace["MAX_PENDING_BRANCH_OPS"]


def test_bulk_request_queues_every_branch_in_one_call():
    ops = [{"op": "delete", "branch": "agent/done-%d" % index,
            "expectedOid": "a" * 40} for index in range(200)]
    response, rows, audits, notifications, namespace = _post({
        "ops": ops, "defaultBranch": "main",
    })
    assert response["status"] == 200
    assert response["data"]["queuedCount"] == 200
    assert len(rows) == 200
    assert len(response["data"]["ops"]) == 200
    assert len(audits) == 200
    assert notifications == [("alice", "widget", "branches")]
    assert namespace["MAX_PENDING_BRANCH_OPS"] == 1024


def test_bulk_request_skips_branches_already_queued():
    namespace, rows, audits, _notify = _harness({
        "ops": [
            {"op": "delete", "branch": "feature/a", "expectedOid": "a" * 40},
            {"op": "delete", "branch": "feature/b", "expectedOid": "a" * 40},
            {"op": "delete", "branch": "feature/b", "expectedOid": "a" * 40},
        ],
    })
    rows.append({
        "id": 99, "repo_bi": "bi:alice/widget",
        "data": {"op": "delete", "branch": "feature/a",
                 "expectedOid": "a" * 40, "queuedAt": 1},
        "queued_at": 1,
    })
    response = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert response["data"]["queuedCount"] == 1
    assert sorted(r["data"]["branch"] for r in rows) == [
        "feature/a", "feature/b"]
    assert len(audits) == 1


def test_bulk_request_is_validated_before_anything_is_queued():
    response, rows, _audits, notifications, _ns = _post({
        "ops": [
            {"op": "delete", "branch": "feature/a", "expectedOid": "a" * 40},
            {"op": "delete", "branch": "main", "expectedOid": "a" * 40},
        ],
        "defaultBranch": "main",
    })
    assert response["status"] == 409
    assert rows == []
    assert notifications == []

    response, rows, _audits, _notify, _ns = _post({"ops": []})
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_request"

    response, rows, _audits, _notify, _ns = _post({"ops": ["delete"]})
    assert response["status"] == 400
    assert rows == []


def test_bulk_request_that_does_not_fit_queues_nothing():
    namespace, rows, _audits, _notify = _harness({
        "ops": [{"op": "delete", "branch": "new/%d" % index,
                 "expectedOid": "a" * 40} for index in range(5)],
    })
    limit = namespace["MAX_PENDING_BRANCH_OPS"]
    rows.extend({
        "id": 5000 + index, "repo_bi": "bi:alice/widget",
        "data": {"op": "delete", "branch": "kept/%d" % index,
                 "expectedOid": "a" * 40, "queuedAt": 1},
        "queued_at": 1,
    } for index in range(limit - 3))
    response = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert response["status"] == 429
    assert response["data"] == {
        "error": "queue_full", "room": 3, "limit": limit}
    assert len(rows) == limit - 3


def test_route_is_wired_end_to_end_and_the_queue_is_bounded_in_the_schema():
    assert 'REPO_BRANCH_OPS_RE = re.compile(' in URLS
    assert r'r"^/api/repo/([^/]+)/([^/]+)/branches/ops$")' in URLS
    assert '"REPO_BRANCH_OPS_RE",' in ENTRY
    assert (
        'repository_branch_ops_handler = _route_handlers.export('
        '"repository_branch_ops_handler")' in ENTRY
    )
    assert "branch_ops_match = REPO_BRANCH_OPS_RE.match(url.path)" in RUNTIME
    assert "return await repository_branch_ops_handler(" in RUNTIME
    assert r'r"^/api/repo/([^/]+)/([^/]+)/branches/ops/results$")' in URLS
    assert '"REPO_BRANCH_OP_RESULTS_RE"' in ENTRY
    assert "return await repository_branch_op_results_handler(" in RUNTIME
    assert "CREATE TABLE IF NOT EXISTS branch_op_results" in SCHEMA
    # The read route the page renders must not swallow the write route.
    assert "|branches|" in URLS
    assert "CREATE TABLE IF NOT EXISTS branch_op_inbox" in SCHEMA
    assert "trg_branch_op_inbox_repo_bound" in SCHEMA
    assert "branch_op_inbox_repo_limit" in SCHEMA


def test_mirror_publishes_divergence_with_every_branch():
    # The desktop app computes "behind / ahead" locally. The website cannot,
    # so the mirror's branch listing carries it — in one for-each-ref walk,
    # with a graceful listing when the Git running the mirror is too old for
    # the atom.
    assert 'row_format + "%00%(ahead-behind:" + default_ref + ")"' in GATEWAY
    assert '"defaultBranch": default_ref[len("refs/heads/"):]' in GATEWAY
    assert "except GitError:\n            output = listing(row_format)" in GATEWAY
    # Divergence is a property of the listing, not a per-branch request: the
    # read stays one cacheable operation with no query.
    assert '"branches": frozenset(),' in GATEWAY


def test_branches_page_shows_divergence_and_offers_the_owner_the_actions():
    # Divergence, and the conflict verdict of a real merge trial, both come
    # from the mirror; the write controls only exist for a viewer the relay's
    # own gate would accept.
    assert "function repoBranchDivergenceText(branch, defaultBranch)" in DASHBOARD
    assert "${formatCount(branch.behind)} behind" in DASHBOARD
    assert "${formatCount(branch.ahead)} ahead" in DASHBOARD
    assert "return `merged into ${defaultBranch}`;" in DASHBOARD
    assert "async function probeRepoBranchTrials(repo)" in DASHBOARD
    assert "const REPO_BRANCH_TRIAL_LIMIT = 8;" in DASHBOARD
    assert "/merge-preview?base=${encodeURIComponent(defaultBranch)}" in DASHBOARD
    assert "data-repo-branch-conflicts" in DASHBOARD

    assert "async function loadRepoBranchManageAuthorization(repo)" in DASHBOARD
    assert "await loadRepoIssueMutationAuthorization(repo)" in DASHBOARD
    assert "if (!repoBranchManageAllowed(repo)) {" in DASHBOARD

    for marker in (
        "async function deleteRepoBranches(repo, names)",
        "async function updateRepoBranchFromDefault(repo, branch, commit)",
        "async function createRepoBranch(repo)",
        "function toggleRepoBranchMergedFilter(repo)",
        "async function cancelRepoBranchOp(repo, id)",
        "async function queueRepoBranchOp(repo, request, message)",
        '`${repoApiBase(repo)}/branches/ops`',
        'data-repo-branch-delete',
        'data-repo-branch-update',
        'data-repo-branch-create',
        'data-repo-branches-filter-merged',
        'data-repo-branch-select',
        'data-repo-branches-delete-selected',
        'data-repo-branch-update-all',
        "async function updateAllRepoBranchesFromDefault(repo)",
        'data-repo-branch-op-cancel',
        'data-repo-branches-refresh',
    ):
        assert marker in DASHBOARD, marker

    # Deleting asks inline in the row or the selection bar, never with a
    # browser dialog, and sends the exact tip the page showed.
    assert "data-repo-branch-delete-confirmation" in DASHBOARD
    assert "data-repo-branches-delete-summary" in DASHBOARD
    assert "`${repoApiBase(repo)}/branches/delete`" in DASHBOARD
    assert '({ name, expectedOid: String(listed.get(name)?.commit || "") })' in DASHBOARD
    delete_code = DASHBOARD[DASHBOARD.index("const REPO_BRANCH_DELETE_LIMIT"):
                            DASHBOARD.index("async function updateRepoBranchFromDefault(")]
    assert "window.confirm" not in delete_code


def test_the_owning_node_applies_the_queue_and_refuses_the_dangerous_cases():
    # The relay hands each request over once; the node that owns the checkout
    # is what actually runs Git, and it re-checks everything.
    assert 'entry.value("branchOps").toArray()' in DESKTOP_SYNC
    assert "applyWebBranchOps(repo," in DESKTOP_SYNC
    assert "drainWebBranchOpsFor(uncovered);" in DESKTOP_SYNC
    assert "/api/repo/%1/%2/branches/ops\").arg(owner, name)" in (
        DESKTOP_BRANCHES)
    assert "if (!isSourceOfTruthRepo(repo))" in DESKTOP_BRANCHES
    assert "propagateRepoUpdate(index);" in DESKTOP_BRANCHES

    for marker in (
        "refused: %1 is the default branch",
        "refused: %1 is checked out here",
        "refused: %1 is checked out in a worktree",
        "refused: %1 moved to %2 since the request",
        "refused: merging %1 into %2 conflicts",
        # Exact-tip compare-and-swap on every ref write, never a plain move.
        'QStringLiteral("update-ref"), QStringLiteral("-d"),',
        "bool usableBranchName(const QString &branch)",
    ):
        assert marker in DESKTOP_OPS, marker
    assert 'branch != QLatin1String("forkmesh/pulls")' in DESKTOP_OPS


MERGE_BODY = {
    "op": "merge", "branch": "feature", "base": "main",
    "expectedOid": "b" * 40, "baseOid": "a" * 40, "deleteBranch": True,
    "defaultBranch": "main",
}


def test_a_single_merge_runs_on_the_cores_when_they_serve_the_repository():
    cores = _Cores((200, {
        "ok": True, "executed": True, "op": "merge", "status": "merged",
        "branch": "feature", "base": "main", "mergeOid": "c" * 40,
        "branchDeleted": True, "cleanupPending": [],
        "message": "Merged feature into main and cleaned up."}))
    response, rows, audits, _notes, namespace = _post(
        dict(MERGE_BODY), cores=cores)
    assert response["status"] == 200
    assert response["data"]["executed"] is True
    assert response["data"]["results"][0]["status"] == "applied"
    assert response["data"]["results"][0]["mergeOid"] == "c" * 40
    assert cores.calls[0][0]["branch"] == "feature"
    assert cores.calls[0][2] == "main"
    assert len(rows) == 1
    follow_up = rows[0]
    assert '"op": "delete"' in follow_up["data"] or "delete" in str(
        follow_up["data"])
    assert audits and audits[-1][4] == "success"


def test_a_core_still_working_answers_202_and_queues_nothing():
    cores = _Cores((202, {
        "ok": True, "executed": True, "status": "processing",
        "requestId": "ab-123456789012", "node": "fmnext-n1",
        "pinBase": "main"}))
    response, rows, _audits, _notes, _ns = _post(dict(MERGE_BODY),
                                                 cores=cores)
    assert response["status"] == 202
    assert response["data"]["status"] == "processing"
    assert rows == []

    assert response["headers"] == {}


def test_a_result_still_pending_is_a_quiet_retryable_503():
    cores = _Cores((503, {
        "ok": False, "executed": True, "status": "failed",
        "error": "merge_status_pending", "op": "merge", "branch": "feature",
        "retryable": True, "message": "The result is not known yet."}))
    response, rows, _audits, _notes, _ns = _post(dict(MERGE_BODY),
                                                 cores=cores)
    assert response["status"] == 503
    assert response["data"]["error"] == "merge_status_pending"
    assert response["headers"] == {"x-forkmesh-expected-degraded": "1"}
    assert rows == []


def test_without_cores_a_single_op_is_queued_for_the_desktop():
    response, rows, _audits, _notes, namespace = _post(dict(MERGE_BODY))
    assert response["data"]["queued"] is True and len(rows) == 1
    assert len(namespace["_cores"].calls) == 1


def test_a_batch_or_an_explicit_queue_request_goes_to_the_desktop():
    cores = _Cores((200, {"ok": True, "status": "merged"}))
    response, rows, _a, _n, _ns = _post(
        {"ops": [dict(MERGE_BODY)], "defaultBranch": "main"}, cores=cores)
    assert response["data"]["queued"] is True and cores.calls == []
    response, rows, _a, _n, _ns = _post(
        dict(MERGE_BODY, queue=True), cores=cores)
    assert response["data"]["queued"] is True and cores.calls == []


BRANCH_UI = (
    ROOT / "public" / "dashboard" / "js" / "07-repo-compose-branch.js"
).read_text(encoding="utf-8")

BRANCH_UI_FUNCTIONS = (
    "postRepoBranchOp", "repoBranchResumeKey", "repoBranchResumeBody",
    "repoBranchRunning", "repoBranchOutcomeExtrasHtml",
    "paintRepoBranchOutcome", "setRepoBranchOutcome", "repoBranchOpOutcome",
    "refreshRepoBranchesAfterCores", "runRepoBranchOpOnCores",
    "sendRepoBranchOp", "queueRepoBranchOp", "runRepoBranchBatch",
)


def _js_function(source, name):
    start = source.index(f"function {name}(")
    if source[max(0, start - 6):start] == "async ":
        start -= 6
    cursor = source.index("{", source.index(")", start))
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(f"unterminated JavaScript function: {name}")


def _js_constant(name):
    text = BRANCH_UI[BRANCH_UI.index(f"const {name}"):]
    return text[:text.index(";\n") + 1]


def _drive_branch_ops(call, replies):
    script = "\n".join([
        "const posted = [];",
        "const announced = [];",
        "const watched = [];",
        f"const replies = {json.dumps(replies)};",
        "globalThis.fetch = async (url, init) => {"
        " posted.push({url, body: JSON.parse(init.body)});"
        " const [status, data] = replies.shift() || [500, {}];"
        " return {status, ok: status >= 200 && status < 300,"
        " json: async () => data}; };",
        "const state = {session: {sessionToken: 'tok'}, repoBranchBusy: {},"
        " repoBranchRunning: {}, repoBranchOutcome: {}, repoBranchResume: {},"
        " repoBranchOps: {}, repoCompare: null, selectedRepo: null};",
        "const repoKey = (repo) => `${repo.owner}/${repo.name}`;",
        "const repoDefaultBranch = () => 'main';",
        "const repoApiBase = (repo) => `/api/repo/${repo.owner}/${repo.name}`;",
        "const repaintRepoBranchOps = () => {};",
        "const announceRepoBranchOp = (message, bad = false) =>"
        " announced.push({message, bad});",
        "const watchRepoBranchOps = (repo, ids) => watched.push(ids);",
        "const setRepoBranchOpsState = (repo, data) => {"
        " state.repoBranchOps[repoKey(repo)] = {ops: data.ops || [],"
        " results: []}; };",
        "const repoBranchOps = (repo) =>"
        " state.repoBranchOps[repoKey(repo)]?.ops || [];",
        "const loadRepoBranchOps = async () => {};",
        "const fetchRepoBranches = async () => {};",
        "const renderRepoCompareMergePanel = () => {};",
        "const loadRepoCompareMergePanel = async () => {};",
        "const orgErrorText = (code) => code;",
        "const escapeHtml = (text) => String(text);",
        "const $ = () => null;",
        "const window = {};",
        "const REPO_BRANCH_OPS_POLL_MS = 0;",
        "const REPO_BRANCH_OPS_POLL_ROUNDS = 90;",
        _js_constant("REPO_BRANCH_OP_ERRORS"),
        _js_constant("REPO_BRANCH_OP_RETRYABLE"),
        *(_js_function(BRANCH_UI, name) for name in BRANCH_UI_FUNCTIONS),
        "const repo = {owner: 'alice', name: 'proj'};",
        f"const result = await {call};",
        "console.log(JSON.stringify({result, posted, announced, watched,"
        " outcome: state.repoBranchOutcome['alice/proj'] || null,"
        " busy: state.repoBranchBusy, running: state.repoBranchRunning}));",
    ])
    output = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True)
    return json.loads(output.stdout)


PROCESSING = {
    "ok": True, "executed": True, "status": "processing",
    "requestId": "ab-123456789012", "node": "fmnext-n1", "pinBase": "main",
    "message": "Merging feature into main…",
}


def test_an_executed_merge_is_followed_with_the_same_pinned_request():
    merge = {"op": "merge", "branch": "feature", "expectedOid": "b" * 40,
             "base": "main", "baseOid": "a" * 40, "deleteBranch": True}
    run = _drive_branch_ops(
        f"queueRepoBranchOp(repo, {json.dumps(merge)}, 'Queued.')",
        [[202, PROCESSING],
         [503, {"ok": False, "executed": True, "status": "failed",
                "error": "merge_status_pending", "retryable": True}],
         [200, {"ok": True, "executed": True, "status": "merged",
                "branch": "feature", "base": "main", "mergeOid": "c" * 40,
                "branchDeleted": True, "message": "Merged feature into main.",
                "results": [{"id": 1, "status": "applied"}]}]])
    assert run["result"] is True
    first, *resumed = [item["body"] for item in run["posted"]]
    assert all(item["url"] == "/api/repo/alice/proj/branches/ops"
               for item in run["posted"])
    assert "requestId" not in first and first["defaultBranch"] == "main"
    assert len(resumed) == 2
    for body in resumed:
        assert body["requestId"] == "ab-123456789012"
        assert body["node"] == "fmnext-n1"
        assert body["pinBase"] == "main"
        assert body["op"] == "merge" and body["branch"] == "feature"
        assert body["expectedOid"] == "b" * 40
    outcome = run["outcome"]
    assert outcome["message"] == "Merged feature into main."
    assert outcome["actions"] is True and not outcome["bad"]
    assert outcome["mergeOid"] == "c" * 40
    assert {"message": "Merging feature into main…", "bad": False} in run["announced"]
    assert run["watched"] == [] and run["busy"] == {} and run["running"] == {}


def test_an_executed_update_resumes_with_the_pinned_tip():
    processing = dict(PROCESSING, pinOid="d" * 40,
                      message="Updating feature from main…")
    update = {"op": "update", "branch": "feature", "expectedOid": "b" * 40,
              "base": "main"}
    run = _drive_branch_ops(
        f"queueRepoBranchOp(repo, {json.dumps(update)}, 'Queued.')",
        [[202, processing],
         [200, {"ok": True, "executed": True, "status": "updated",
                "branch": "feature", "base": "main",
                "message": "Updated feature from main."}]])
    resumed = run["posted"][1]["body"]
    assert resumed["pinOid"] == "d" * 40 and resumed["pinBase"] == "main"
    assert resumed["requestId"] == "ab-123456789012"
    assert run["outcome"]["message"] == "Updated feature from main."
    assert run["outcome"]["actions"] is False


def test_a_conflicting_merge_lists_the_files_and_offers_the_compare_page():
    merge = {"op": "merge", "branch": "feature", "expectedOid": "b" * 40,
             "base": "main", "baseOid": "a" * 40}
    run = _drive_branch_ops(
        f"queueRepoBranchOp(repo, {json.dumps(merge)}, 'Queued.')",
        [[409, {"ok": False, "executed": True, "status": "failed",
                "error": "merge_conflict", "conflicts": ["src/a.py"],
                "message": "feature conflicts with main, so nothing changed."}]])
    assert run["result"] is False
    outcome = run["outcome"]
    assert outcome["bad"] and outcome["conflicts"] == ["src/a.py"]
    assert outcome["message"].startswith("feature conflicts with main")
    html = _drive_branch_ops(
        "repoBranchOutcomeExtrasHtml({conflicts: ['src/a.py'],"
        " compareBase: 'main', compareHead: 'feature'})", [])["result"]
    assert "src/a.py" in html and "data-repo-branch-outcome-compare" in html
    actions = _drive_branch_ops(
        "repoBranchOutcomeExtrasHtml({actions: true, base: 'main',"
        " mergeOid: 'cccccccc'})", [])["result"]
    assert "data-repo-branch-open-actions" in actions and "Open Actions" in actions


def test_a_queued_answer_keeps_the_desktop_path():
    delete = {"op": "delete", "branch": "old", "expectedOid": "b" * 40}
    run = _drive_branch_ops(
        f"queueRepoBranchOp(repo, {json.dumps(delete)}, 'Queued the deletion.')",
        [[200, {"ok": True, "queued": True, "queuedCount": 1,
                "ops": [{"id": 7, "op": "delete", "branch": "old"}]}]])
    assert run["result"] is True and len(run["posted"]) == 1
    assert run["announced"] == [{"message": "Queued the deletion.", "bad": False}]
    assert run["watched"] == [[7]]


def test_a_batch_runs_one_by_one_on_the_cores_and_stops_at_the_first_error():
    ops = [{"op": "delete", "branch": name, "expectedOid": "b" * 40}
           for name in ("one", "two", "three")]
    run = _drive_branch_ops(
        f"runRepoBranchBatch(repo, {json.dumps(ops)}, (n) => `Queued ${{n}}.`, 'Deleted')",
        [[200, {"ok": True, "executed": True, "status": "deleted",
                "message": "Deleted one."}],
         [409, {"ok": False, "executed": True, "status": "failed",
                "error": "stale_head", "message": "A branch moved."}]])
    assert [item["body"]["branch"] for item in run["posted"]] == ["one", "two"]
    assert all("ops" not in item["body"] for item in run["posted"])
    assert run["outcome"]["message"] == "Stopped after 1 of 3: A branch moved."
    assert run["outcome"]["bad"] is True


def test_a_batch_without_cores_queues_the_rest_in_one_request():
    ops = [{"op": "update", "branch": name, "expectedOid": "b" * 40,
            "base": "main"} for name in ("one", "two")]
    run = _drive_branch_ops(
        f"runRepoBranchBatch(repo, {json.dumps(ops)}, (n) => `Queued ${{n}}.`, 'Updated')",
        [[200, {"ok": True, "queued": True, "queuedCount": 1,
                "ops": [{"id": 1, "op": "update", "branch": "one"}]}],
         [200, {"ok": True, "queued": True, "queuedCount": 1,
                "ops": [{"id": 1, "op": "update", "branch": "one"},
                        {"id": 2, "op": "update", "branch": "two"}]}]])
    assert run["posted"][0]["body"]["branch"] == "one"
    assert [op["branch"] for op in run["posted"][1]["body"]["ops"]] == ["two"]
    assert run["announced"][-1] == {"message": "Queued 2.", "bad": False}


def test_the_bundle_carries_the_executed_branch_path():
    for name in BRANCH_UI_FUNCTIONS:
        assert f"function {name}(" in DASHBOARD, name
    assert "data-repo-branch-open-actions" in DASHBOARD
    assert 'activateRepoTab("actions");' in DASHBOARD
    assert "data-repo-branch-running" in DASHBOARD


def test_queue_answers_carry_the_limit():
    namespace, rows, _audits, _notify = _harness({
        "op": "delete", "branch": "feature/new", "expectedOid": "a" * 40,
    })
    limit = namespace["MAX_PENDING_BRANCH_OPS"]
    queued = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert queued["data"]["limit"] == limit
    repeated = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert repeated["data"]["queuedCount"] == 0
    assert repeated["data"]["limit"] == limit
    listed = asyncio.run(namespace["repository_branch_ops_handler"](
        object(), _Request("GET"), "alice", "widget"))
    assert listed["data"]["limit"] == limit
    full_namespace, full_rows, _audits, _notify = _harness({
        "op": "delete", "branch": "feature/other", "expectedOid": "a" * 40,
    })
    full_rows.extend({
        "id": 3000 + index,
        "repo_bi": "bi:alice/widget",
        "data": {"op": "delete", "branch": "kept/%d" % index,
                 "expectedOid": "a" * 40, "queuedAt": 1},
        "queued_at": 1,
    } for index in range(limit))
    full = asyncio.run(full_namespace["repository_branch_ops_handler"](
        object(), _Request("POST"), "alice", "widget"))
    assert full["status"] == 429
    assert full["data"] == {"error": "queue_full", "room": 0, "limit": limit}

def test_branches_page_holds_what_the_queue_cannot_take_yet():
    for marker in (
        "error.data = data;",
        'if (error?.code === "queue_full") {',
        "holdRepoBranchOps(repo, Array.isArray(request.ops) ? request.ops : [request]);",
        "announceRepoBranchHeld(repo, await sendRepoBranchHeld(repo, error.data?.room));",
        "async function sendRepoBranchHeld(repo, room)",
        "const batch = repoBranchHeld(repo).slice(0, space);",
        'if (code === "queue_full" && left > 0 && left < batch.length) {',
        "function scheduleRepoBranchHeld(repo)",
        "const REPO_BRANCH_HELD_RETRY_MS = 20000;",
        "|| repoBranchHeld(repo).find((op) => op.branch === branch)",
        'if (String(id).startsWith("held:")) {',
        'delete: "deletion waiting",',
        "(pending.held ? REPO_BRANCH_HELD_LABELS : REPO_BRANCH_OP_LABELS)",
    ):
        assert marker in DASHBOARD, marker
