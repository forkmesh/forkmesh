import ast
import asyncio
import builtins
import re
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
sys.path.insert(0, str(SRC))

import agent_branches  # noqa: E402
import branch_protection  # noqa: E402
from test_agent_branches import A, B, D, World, world  # noqa: E402,F401

ROUTES_PATH = SRC / "route_handlers.py"
ROUTES = ROUTES_PATH.read_text(encoding="utf-8")
URLS = (SRC / "urls.py").read_text(encoding="utf-8")
RUNTIME = (SRC / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
READ_ONLY = (SRC / "read_only_mirrors.py").read_text(encoding="utf-8")

NAMES = (
    "BRANCH_OPS", "MAX_PENDING_BRANCH_OPS", "BRANCH_OP_NAME_PATTERN",
    "BRANCH_OP_OID_PATTERN", "branch_op_name", "_branch_op_projection",
    "_branch_ops_pending", "_branch_ops_wake",
    "MAX_BRANCH_DELETES", "BRANCH_DELETE_AUDIT_OUTCOMES",
    "_branch_delete_refusal", "_branch_delete_wanted",
    "_branch_delete_screen", "_branch_delete_hosted", "_branch_delete_queue",
    "_branch_delete_via_node", "_branch_delete_on_cores",
    "repository_branch_delete_handler",
)

@pytest.fixture(autouse=True)
def _restore_agent_branches_namespace():
    before = set(vars(agent_branches))
    yield
    for name in set(vars(agent_branches)) - before:
        delattr(agent_branches, name)


ACCOUNTS = {"owner-session": "alice", "admin-session": "olivia",
            "member-session": "mallory"}
ORG_ADMINS = {"olivia"}


def _protection(refs=None, problem=None):
    async def push_protection(_env, _owner, _repo):
        return (None, problem) if problem else (list(refs or []), None)

    async def storage_owner(_env, owner, _repo):
        return owner

    async def default_branch(_env, _node, _repo):
        return "main"

    return SimpleNamespace(
        push_protection=push_protection,
        branch_is_protected=branch_protection.branch_is_protected,
        storage_owner=storage_owner,
        default_branch=default_branch,
    )


class Harness:
    def __init__(self, protection=None, hosted=True):
        self.rows = []
        self.audits = []
        self.wakes = []
        self.protection = protection or _protection()
        self.hosted = hosted
        self.namespace = self._load()

    def _load(self):
        tree = ast.parse(ROUTES, filename=str(ROUTES_PATH))
        selected = []
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name in NAMES:
                    selected.append(node)
            elif isinstance(node, ast.Assign) and any(
                    isinstance(target, ast.Name) and target.id in NAMES
                    for target in node.targets):
                selected.append(node)
        assert len(selected) == len(NAMES)
        harness = self

        class Clock:
            @staticmethod
            def now():
                return 1_700_000_000_000

        def json_response(data, status=200, extra_headers=None, **_kwargs):
            return {"status": status, "data": data,
                    "headers": extra_headers or {}}

        async def nothing(*_args, **_kwargs):
            return None

        async def bounded_json_request(request):
            return request.body

        async def blind_index(_env, value):
            return "bi:" + str(value)

        async def session_record(_env, request, _data):
            name = ACCOUNTS.get(request.session or "")
            return ("bi:" + name, {"name": name}) if name else ("", None)

        async def authorize(_env, owner, data, request):
            _, record = await session_record(None, request, data)
            name = (record or {}).get("name", "")
            if name and (name == owner or name in ORG_ADMINS):
                return True, None
            return False, json_response({"error": "not_authorized"},
                                        status=403)

        async def identity(_env, value):
            return value

        async def d1_all(_env, sql, *args):
            assert "FROM branch_op_inbox" in sql
            return [dict(row) for row in harness.rows
                    if row["repo_bi"] == args[0]]

        async def d1_run(_env, sql, *args):
            assert sql.startswith("INSERT INTO branch_op_inbox"), sql
            repo_bi, data, queued_at = args
            harness.rows.append({"id": len(harness.rows) + 1,
                                 "repo_bi": repo_bi, "data": data,
                                 "queued_at": queued_at})

        async def audit(_env, actor, action, kind, target, outcome, details):
            harness.audits.append((actor, action, kind, target, outcome,
                                   details))

        async def notify(_env, owner, repo, topic, event_owner=""):
            harness.wakes.append((owner, repo, topic))

        async def catalog_record(_env, _owner, _repo):
            return {"source": "hosted-web" if harness.hosted
                    else "local-node"}

        real_import = builtins.__import__

        def importer(name, *args, **kwargs):
            if name == "agent_branches":
                return agent_branches
            return real_import(name, *args, **kwargs)

        namespace = {
            "__builtins__": dict(vars(builtins), __import__=importer),
            "Date": Clock, "re": re, "parse_qs": parse_qs,
            "urlparse": urlparse, "json_response": json_response,
            "EXPECTED_DEGRADED_HEADERS": {"x-forkmesh-expected-degraded": "1"},
            "ensure_schema": nothing, "method_name": lambda r: r.method,
            "bounded_json_request": bounded_json_request,
            "blind_index": blind_index,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "encrypt_row": identity, "decrypt_row": identity,
            "d1_all": d1_all, "d1_run": d1_run, "MAX_NODE_NAME": 63,
            "_authorize_issue_manager": authorize,
            "_account_session_record": session_record,
            "_audit_sensitive_action": audit, "notify_repo_host": notify,
            "_repo_catalog_record": catalog_record,
            "_branch_protection": self.protection,
            "REPO_API_PREFIX_RE": re.compile(
                r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$"),
            "safe_segment": lambda value: str(value or ""),
            "_org_row": nothing, "valid_node_name": lambda value: True,
            "MAX_ORG_MEMBERS": 200,
        }
        exec(compile(ast.fix_missing_locations(
            ast.Module(body=selected, type_ignores=[])),
            str(ROUTES_PATH), "exec"), namespace)
        return namespace

    def post(self, branches, session="owner-session", method="POST",
             owner="alice"):
        request = SimpleNamespace(
            method=method, session=session,
            body={"branches": branches} if branches is not None else {},
            url="https://app.test/api/repo/%s/proj/branches/delete" % owner)
        return asyncio.run(self.namespace["repository_branch_delete_handler"](
            None, request, owner, "proj"))


def _add_branch(world, name, oid=D):
    for core in world.cores.values():
        core.refs[name] = oid


def _statuses(response):
    return {item["branch"]: (item["status"], item["error"])
            for item in response["data"]["results"]}


def test_the_route_is_wired_and_read_only_mirrors_refuse_it():
    assert 'r"^/api/repo/([^/]+)/([^/]+)/branches/delete$"' in URLS
    assert '"REPO_BRANCH_DELETE_RE"' in ENTRY
    assert '_route_handlers.export(\n    "repository_branch_delete_handler")' in ENTRY
    assert "REPO_BRANCH_DELETE_RE.match(url.path)" in RUNTIME
    assert "repository_branch_delete_handler(" in RUNTIME
    sys.path.insert(0, str(SRC))
    import read_only_mirrors
    assert read_only_mirrors.is_write_request(
        "POST", "/api/repo/alice/proj/branches/delete")
    assert not read_only_mirrors.is_write_request(
        "GET", "/api/repo/alice/proj/branches")


@pytest.mark.parametrize("session", [None, "member-session"])
def test_only_owners_and_organization_admins_may_delete(world, session):
    _add_branch(world, "feature")
    harness = Harness()
    response = harness.post(["feature"], session=session)
    assert response["status"] == 403
    assert response["data"]["error"] == "not_authorized"
    assert world.proxied == [] and harness.audits == []
    assert all("feature" in core.refs for core in world.cores.values())


def test_the_owner_deletes_a_branch_on_every_core_and_it_is_audited(world):
    _add_branch(world, "feature")
    harness = Harness()
    response = harness.post([{"name": "feature", "expectedOid": D}])
    assert response["status"] == 200
    assert _statuses(response) == {"feature": ("deleted", "")}
    assert response["data"]["counts"]["deleted"] == 1
    assert response["data"]["defaultBranch"] == "main"
    assert all("feature" not in core.refs for core in world.cores.values())
    sent = [item[2] for item in world.proxied if item[1] == "branch-delete"]
    assert sent and all(item["discard"] is True and item["base"] == "main"
                        and item["expectedHeadOid"] == D for item in sent)
    assert harness.audits == [(
        "alice", "repository.branch_delete", "repository", "alice/proj",
        "success", {"branch": "feature", "op": "delete", "status": "deleted",
                    "error": "", "batch": 1, "cleanupPending": 0})]
    assert world.hosted_pushes == [("alice", "proj", "fmnext-n1", "alice")]
    assert harness.rows == []


def test_an_organization_admin_may_delete_through_the_alias(world):
    _add_branch(world, "feature")
    harness = Harness()
    response = harness.post(["feature"], session="admin-session",
                            owner="acme")
    assert response["status"] == 200
    assert _statuses(response) == {"feature": ("deleted", "")}
    assert harness.audits[0][0] == "olivia"


def test_the_default_branch_is_refused(world):
    harness = Harness()
    response = harness.post(["main"])
    assert response["status"] == 200
    assert _statuses(response) == {"main": ("refused", "default_branch")}
    assert world.proxied == []
    assert world.cores["fmnext-n1"].refs["main"] == A
    assert harness.audits[0][4] == "denied"
    assert world.hosted_pushes == []


def test_protected_branches_are_refused(world):
    _add_branch(world, "release/1.0")
    _add_branch(world, "feature")
    harness = Harness(protection=_protection(
        ["refs/heads/main", "refs/heads/release/"]))
    agent_branches._branch_protection = harness.protection
    response = harness.post(["release/1.0", "feature"])
    assert _statuses(response) == {
        "release/1.0": ("refused", "branch_protected"),
        "feature": ("deleted", ""),
    }
    assert all(core.refs["release/1.0"] == D
               for core in world.cores.values())


def test_unreadable_protection_refuses_the_whole_request(world):
    _add_branch(world, "feature")
    harness = Harness(protection=_protection(
        problem="protection_unavailable"))
    agent_branches._branch_protection = harness.protection
    response = harness.post(["feature"])
    assert response["status"] == 503
    assert response["data"]["error"] == "protection_unavailable"
    assert world.proxied == []
    assert harness.audits[0][4] == "failed"


def test_a_batch_reports_every_branch_in_order(world):
    _add_branch(world, "merged/one")
    _add_branch(world, "merged/two")
    _add_branch(world, "moved", B)
    harness = Harness()
    response = harness.post([
        {"name": "merged/one", "expectedOid": D},
        "main",
        "../escape",
        "forkmesh/pulls",
        "forkmesh/meta",
        {"name": "moved", "expectedOid": D},
        "missing",
        "merged/two",
        "merged/two",
    ])
    assert response["status"] == 200
    results = response["data"]["results"]
    assert [(item["branch"], item["status"], item["error"])
            for item in results] == [
        ("../escape", "refused", "invalid_branch"),
        ("forkmesh/pulls", "refused", "invalid_branch"),
        ("forkmesh/meta", "refused", "reserved_branch"),
        ("merged/one", "deleted", ""),
        ("main", "refused", "default_branch"),
        ("moved", "refused", "stale_head"),
        ("missing", "refused", "branch_not_found"),
        ("merged/two", "deleted", ""),
    ]
    assert all(item["message"] for item in results)
    assert response["data"]["counts"] == {
        "deleted": 2, "queued": 0, "pending": 0, "refused": 6, "failed": 0}
    assert world.cores["fmnext-n2"].refs["moved"] == B
    assert [audit[5]["branch"] for audit in harness.audits] == [
        item["branch"] for item in results]
    assert [audit[4] for audit in harness.audits].count("success") == 2


def test_a_core_that_refuses_is_reported_per_branch(world):
    _add_branch(world, "feature")
    _add_branch(world, "other")
    real = world.proxy

    async def refuse_other(env, endpoint, context, request, mutation):
        if request["branch"] == "other":
            return 409, {"ok": False, "status": "failed",
                         "requestId": request["requestId"],
                         "error": "stale_base"}
        return await real(env, endpoint, context, request, mutation)

    agent_branches._https_mirror_merge_proxy = refuse_other
    response = Harness().post(["feature", "other"])
    assert _statuses(response) == {
        "feature": ("deleted", ""), "other": ("failed", "stale_base")}


def test_requests_are_bounded():
    harness = Harness()
    too_many = harness.post(["b%d" % index for index in range(101)])
    assert too_many["status"] == 400
    assert too_many["data"] == {"error": "too_many_branches", "limit": 100}
    assert harness.post([])["data"]["error"] == "branches_required"
    assert harness.post(None)["data"]["error"] == "branches_required"
    assert harness.post(["x"], method="GET")["status"] == 405


def test_a_node_owned_repository_queues_the_deletes_for_its_node(
        world, monkeypatch):
    async def unserved(env, owner, repo):
        return None

    monkeypatch.setattr(agent_branches, "_https_mirror_public_context",
                        unserved)
    harness = Harness(hosted=False)
    response = harness.post([
        {"name": "feature", "expectedOid": D}, "no-tip", "main"])
    assert response["status"] == 200
    assert _statuses(response) == {
        "feature": ("queued", ""),
        "no-tip": ("refused", "invalid_commit"),
        "main": ("refused", "default_branch"),
    }
    assert [row["data"]["branch"] for row in harness.rows] == ["feature"]
    assert harness.rows[0]["data"]["expectedOid"] == D
    assert response["data"]["ops"][0]["branch"] == "feature"
    assert ("alice", "proj", "branches") in harness.wakes
    assert harness.audits[0][4] == "requested"


def test_a_hosted_repository_without_a_core_refuses_instead_of_queueing(
        world, monkeypatch):
    async def unserved(env, owner, repo):
        return None

    monkeypatch.setattr(agent_branches, "_https_mirror_public_context",
                        unserved)
    harness = Harness(hosted=True)
    response = harness.post([{"name": "feature", "expectedOid": D}])
    assert response["status"] == 503
    assert response["data"]["error"] == "branch_delete_unavailable"
    assert response["data"]["retryable"] is True
    assert harness.rows == []


def test_a_node_owned_repository_served_by_cores_tells_its_node_too(world):
    _add_branch(world, "feature")
    harness = Harness(hosted=False)
    response = harness.post(["feature"])
    assert _statuses(response) == {"feature": ("deleted", "")}
    assert [(row["data"]["branch"], row["data"]["source"])
            for row in harness.rows] == [("feature", "cores")]


def test_deleting_counts_as_a_ref_change_for_push_mirrors():
    helper = ROUTES[ROUTES.index("async def _branch_delete_on_cores"):]
    assert "agent_branches.core_delete_branches(" in helper
    source = (SRC / "agent_branches.py").read_text(encoding="utf-8")
    body = source[source.index("async def core_delete_branches"):]
    body = body[:body.index("\nasync def ")]
    assert "record_hosted_push_state(" in body
    hosted = (SRC / "hosted_git.py").read_text(encoding="utf-8")
    pin = hosted[hosted.index("async def record_hosted_push_state"):]
    pin = pin[:pin.index("\nasync def ")]
    assert pin.index("_note_core_push(") < pin.index("record.get(\"stateHash\") == digest")
