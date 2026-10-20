import json
from pathlib import Path
import sys
import threading
import time

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "app" / "src"))

import edge_routing  # noqa: E402
import mirror_gateway as gateway  # noqa: E402


OID_A = "a" * 40
OID_B = "b" * 40
OID_C = "c" * 40


def body(kind="merge", **changes):
    value = {
        "schemaVersion": 1,
        "type": "forkmesh.branch-%s-v1" % kind,
        "requestId": "branch_request_0001",
        "branch": "agent/12-fix-login",
        "base": "main",
        "expectedHeadOid": OID_B,
    }
    if kind == "merge":
        value["expectedBaseOid"] = OID_A
    for key, item in changes.items():
        if item is None:
            value.pop(key, None)
        else:
            value[key] = item
    return json.dumps(value, separators=(",", ":")).encode()


def parse(raw, operation="merge-agent-branch"):
    return gateway.GatewayApplication._merge_request(
        "fmnext-n1", "forkmesh", raw, operation)


def test_branch_merge_body_becomes_a_typed_executor_request():
    assert parse(body(deleteBranch=True)) == {
        "schemaVersion": 1,
        "type": "forkmesh.branch-merge-executor-v1",
        "owner": "fmnext-n1",
        "repository": "forkmesh",
        "requestId": "branch_request_0001",
        "branch": "agent/12-fix-login",
        "base": "main",
        "expectedBaseOid": OID_A,
        "expectedHeadOid": OID_B,
        "deleteBranch": True,
    }
    assert "deleteBranch" not in parse(body())
    assert parse(body("delete"), "delete-agent-branch") == {
        "schemaVersion": 1,
        "type": "forkmesh.branch-delete-executor-v1",
        "owner": "fmnext-n1",
        "repository": "forkmesh",
        "requestId": "branch_request_0001",
        "branch": "agent/12-fix-login",
        "base": "main",
        "expectedHeadOid": OID_B,
    }


@pytest.mark.parametrize("changes", [
    {"branch": "agent/"},
    {"branch": "agent/../main"},
    {"branch": "forkmesh/pulls"},
    {"branch": "HEAD"},
    {"branch": "main"},
    {"base": "forkmesh/pulls"},
    {"base": "forkmesh/pull/7"},
    {"expectedBaseOid": None},
    {"expectedHeadOid": "main"},
    {"expectedBaseOid": "a" * 64},
    {"pullNumber": 7},
    {"expectedPullsOid": OID_C},
    {"deleteBranch": "yes"},
    {"type": "forkmesh.pull-merge-v1"},
])
def test_branch_merge_body_is_strict(changes):
    with pytest.raises(gateway.GatewayError):
        parse(body(**changes))


@pytest.mark.parametrize("changes", [
    {"branch": "feature"},
    {"branch": "release/2.0"},
    {"base": "agent/other"},
    {"branch": "main", "base": "agent/12-fix-login"},
])
def test_any_user_branch_merges_and_updates(changes):
    parsed = parse(body(**changes))
    assert parsed["branch"] == changes.get("branch", "agent/12-fix-login")
    assert parsed["base"] == changes.get("base", "main")


def test_a_discarding_cleanup_says_so_and_a_merge_cannot():
    assert parse(body("delete", discard=True), "delete-agent-branch")[
        "discard"] is True
    assert "discard" not in parse(body("delete"), "delete-agent-branch")
    with pytest.raises(gateway.GatewayError):
        parse(body("delete", discard="yes"), "delete-agent-branch")
    with pytest.raises(gateway.GatewayError):
        parse(body(discard=True))


def test_cleanup_body_carries_no_merge_fields():
    with pytest.raises(gateway.GatewayError):
        parse(body("delete", deleteBranch=True), "delete-agent-branch")
    with pytest.raises(gateway.GatewayError):
        parse(body("delete", expectedBaseOid=OID_A), "delete-agent-branch")
    with pytest.raises(gateway.GatewayError):
        parse(body("merge"), "delete-agent-branch")


def test_pull_bodies_cannot_drive_a_branch_operation_or_the_reverse():
    pull = json.dumps({
        "schemaVersion": 1, "type": "forkmesh.pull-merge-v1",
        "pullNumber": 7, "requestId": "merge_request_0001",
        "expectedBaseOid": OID_A, "expectedHeadOid": OID_B,
        "expectedPullsOid": OID_C,
    }).encode()
    with pytest.raises(gateway.GatewayError):
        parse(pull)
    with pytest.raises(gateway.GatewayError):
        parse(body(), "merge-pull")


def _app(executor):
    app = object.__new__(gateway.GatewayApplication)
    app.merge_executor = executor
    app._merge_lock = threading.Lock()
    app._merge_threads = {}
    app._merge_results = {}
    app._merge_payload_digests = {}
    app._prewarm_lock = threading.Lock()
    app._prewarm_due = None
    app._prewarm_thread = None
    app.reload_from_disk = lambda **_: None
    return app


def _settle(app, raw, operation):
    deadline = time.time() + 2
    while time.time() < deadline:
        response = app._dispatch_merge_pull(
            "fmnext-n1", "forkmesh", raw, operation)
        if response.status != 202:
            return response
        time.sleep(0.01)
    raise AssertionError("the job never finished")


def test_merge_job_publishes_the_branch_result():
    class Executor:
        def call(self, request, timeout=20):
            del timeout
            if request["action"] == "status":
                return {"ok": True, "status": "missing",
                        "requestId": request["requestId"],
                        "generationReady": False}
            if request["action"] == "execute":
                assert request["type"] == "forkmesh.branch-merge-executor-v1"
                return {"ok": True, "status": "merged",
                        "requestId": request["requestId"],
                        "baseBefore": OID_A, "head": OID_B,
                        "baseAfter": OID_C, "branch": request["branch"],
                        "base": request["base"], "branchDeleted": True,
                        "trashRef": "refs/forkmesh/trash/agent-branch/x",
                        "generationReady": True}
            return {"ok": True, "status": "registered",
                    "requestId": request["requestId"]}

    app = _app(Executor())
    first = app._dispatch_merge_pull(
        "fmnext-n1", "forkmesh", body(deleteBranch=True),
        "merge-agent-branch")
    assert first.status == 202
    done = _settle(app, body(deleteBranch=True), "merge-agent-branch")
    assert done.status == 200
    result = json.loads(done.body)
    assert result == {
        "ok": True, "status": "merged", "requestId": "branch_request_0001",
        "published": True, "baseBefore": OID_A, "head": OID_B,
        "baseAfter": OID_C, "branch": "agent/12-fix-login", "base": "main",
        "branchDeleted": True,
    }


def test_a_slow_cache_warmup_does_not_hold_the_merge_result():
    class Executor:
        def call(self, request, timeout=20):
            del timeout
            if request["action"] == "status":
                return {"ok": True, "status": "missing",
                        "requestId": request["requestId"],
                        "generationReady": False}
            if request["action"] == "execute":
                return {"ok": True, "status": "merged",
                        "requestId": request["requestId"],
                        "baseBefore": OID_A, "head": OID_B,
                        "baseAfter": OID_C, "branch": request["branch"],
                        "base": request["base"], "branchDeleted": False,
                        "generationReady": True}
            return {"ok": True, "status": "registered",
                    "requestId": request["requestId"]}

    reloads = []
    release = threading.Event()
    app = _app(Executor())
    app.reload_from_disk = lambda **kwargs: reloads.append(kwargs)
    app.prewarm_memory_caches = lambda: release.wait(5)
    try:
        app._dispatch_merge_pull(
            "fmnext-n1", "forkmesh", body(), "merge-agent-branch")
        done = _settle(app, body(), "merge-agent-branch")
        assert done.status == 200
        assert json.loads(done.body)["status"] == "merged"
        assert reloads == [{"prewarm": False}]
    finally:
        release.set()


def test_branch_failures_are_bounded_per_operation():
    def executor_failing(error):
        class Executor:
            def call(self, request, timeout=20):
                del timeout
                return {"ok": False, "status": "failed",
                        "requestId": request["requestId"], "error": error}
        return Executor()

    conflict = _settle(_app(executor_failing("merge_conflict")), body(),
                       "merge-agent-branch")
    assert conflict.status == 409
    assert json.loads(conflict.body)["error"] == "merge_conflict"
    unmerged = _settle(_app(executor_failing("not_merged")),
                       body("delete"), "delete-agent-branch")
    assert unmerged.status == 409
    assert json.loads(unmerged.body)["error"] == "not_merged"
    with pytest.raises(gateway.GatewayError):
        gateway.GatewayApplication._merge_response({
            "ok": False, "status": "failed",
            "requestId": "branch_request_0001", "error": "not_merged",
        }, "merge-agent-branch")


def test_operations_are_write_only_routed_and_need_the_executor():
    source = (ROOT / "tools" / "mirror_gateway.py").read_text("utf-8")
    for operation in ("merge-agent-branch", "delete-agent-branch"):
        assert operation in gateway.PUBLIC_OPERATIONS
        assert operation not in gateway.DEFAULT_PUBLIC_OPERATIONS
        assert operation not in gateway.DIRECT_PUBLIC_READ_OPERATIONS
        assert gateway.OPERATION_QUERY_FIELDS[operation] == frozenset()
        assert operation in edge_routing.PUBLIC_OPERATIONS
        assert operation in edge_routing.MERGE_EXECUTOR_OPERATIONS
    assert '" requires an operator-owned mergeExecutorCommand"' in source
    schema = json.loads(
        (ROOT / "www" / "docs" / "mirror-gateway-config.schema.json")
        .read_text("utf-8"))
    assert "merge-agent-branch" in json.dumps(schema)
    go = (ROOT / "server" / "merge_branch.go").read_text("utf-8")
    assert 'agentBranchMergeOp       = "merge-agent-branch"' in go
    assert 'agentBranchDeleteOp      = "delete-agent-branch"' in go


def test_a_delete_accepts_every_branch_name_git_allows():
    plus = "team/feature/researchPapersFeature+"
    parsed = parse(body("delete", branch=plus, discard=True),
                   "delete-agent-branch")
    assert parsed["branch"] == plus
    for name in ("has space", "a..b", "-x", "x.lock", "forkmesh/pulls",
                 "HEAD", "a@{1}", "q?"):
        with pytest.raises(gateway.GatewayError):
            parse(body("delete", branch=name, discard=True),
                  "delete-agent-branch")
    with pytest.raises(gateway.GatewayError):
        parse(body(branch=plus), "merge-agent-branch")


def test_back_to_back_deletes_share_one_low_priority_cache_warmup(
        monkeypatch):
    monkeypatch.setattr(gateway, "PREWARM_AFTER_MERGE_QUIET_SECONDS", 0.2)
    priorities = []
    monkeypatch.setattr(gateway, "_lower_thread_priority",
                        lambda increment=10: priorities.append(
                            threading.current_thread().name))
    warmups = []
    app = _app(None)
    app.prewarm_memory_caches = lambda: warmups.append(time.monotonic())
    for _ in range(5):
        app._schedule_prewarm()
        time.sleep(0.05)
    deadline = time.time() + 3
    while time.time() < deadline and (
            not warmups or app._prewarm_thread is not None):
        time.sleep(0.02)
    assert len(warmups) == 1
    assert priorities == ["forkmesh-merge-prewarm"]
    app._schedule_prewarm()
    deadline = time.time() + 3
    while time.time() < deadline and len(warmups) < 2:
        time.sleep(0.02)
    assert len(warmups) == 2


def test_the_delete_job_thread_runs_at_low_priority(monkeypatch):
    lowered = []
    monkeypatch.setattr(gateway, "_lower_thread_priority",
                        lambda increment=10: lowered.append(
                            threading.current_thread().name))

    class Executor:
        def call(self, request, timeout=20):
            del timeout
            if request["action"] == "status":
                return {"ok": True, "status": "missing",
                        "requestId": request["requestId"],
                        "generationReady": False}
            if request["action"] == "execute":
                return {"ok": True, "status": "deleted",
                        "requestId": request["requestId"], "head": OID_B,
                        "branch": request["branch"],
                        "base": request["base"], "branchDeleted": True,
                        "generationReady": True}
            return {"ok": True, "status": "registered",
                    "requestId": request["requestId"]}

    app = _app(Executor())
    app._schedule_prewarm = lambda: None
    plus = "team/feature/x+"
    raw = body("delete", branch=plus, discard=True)
    app._dispatch_merge_pull("fmnext-n1", "forkmesh", raw,
                             "delete-agent-branch")
    done = _settle(app, raw, "delete-agent-branch")
    assert done.status == 200
    assert json.loads(done.body)["branch"] == plus
    assert lowered == ["forkmesh-pull-merge"]
