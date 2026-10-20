import ast
import asyncio
import builtins
import json
import re
import sqlite3
import sys
import time
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
SCHEMA = (SRC / "schema.py").read_text(encoding="utf-8")
DASHBOARD = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")

NAMES = (
    "BRANCH_OPS", "MAX_PENDING_BRANCH_OPS", "BRANCH_OP_NAME_PATTERN",
    "BRANCH_OP_OID_PATTERN", "branch_op_name", "_branch_op_projection",
    "_branch_ops_pending", "_branch_ops_wake",
    "MAX_BRANCH_DELETES", "BRANCH_DELETE_AUDIT_OUTCOMES",
    "BRANCH_DELETE_JOBS_KEPT", "BRANCH_DELETE_LEASE_MS",
    "BRANCH_DELETE_YIELD_SECONDS", "BRANCH_DELETE_BACKOFF_SECONDS",
    "BRANCH_DELETE_WORKER_KEY", "BRANCH_DELETE_ITEM_FIELDS",
    "_branch_delete_refusal", "_branch_delete_wanted",
    "_branch_delete_screen", "_branch_delete_hosted", "_branch_delete_queue",
    "_branch_delete_via_node", "_branch_delete_projection", "_d1_changes",
    "_branch_delete_job_row", "_branch_delete_job_load",
    "_branch_delete_job_save", "_branch_delete_audit",
    "_branch_delete_job_finish", "_branch_delete_job_drain",
    "_branch_delete_claim", "_branch_delete_worker", "_branch_delete_kick",
    "_branch_delete_status", "_branch_delete_create",
    "repository_branch_delete_handler",
)

ACCOUNTS = {"owner-session": "alice", "admin-session": "olivia",
            "member-session": "mallory"}
ORG_ADMINS = {"olivia"}


@pytest.fixture(autouse=True)
def _restore_agent_branches_namespace():
    before = set(vars(agent_branches))
    yield
    for name in set(vars(agent_branches)) - before:
        delattr(agent_branches, name)


@pytest.fixture
def counted(world, monkeypatch):
    counts = []

    async def record_count(env, owner, repo, count):
        counts.append((owner, repo, count))
        return True

    monkeypatch.setattr(agent_branches, "record_hosted_branch_count",
                        record_count, raising=False)
    return counts


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
    def __init__(self, protection=None, hosted=True, background=False):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            "CREATE TABLE branch_op_inbox (id INTEGER PRIMARY KEY "
            "AUTOINCREMENT, repo_bi TEXT NOT NULL, data TEXT NOT NULL, "
            "queued_at INTEGER NOT NULL);")
        for statement in _schema_statements("branch_delete_jobs"):
            self.db.execute(statement)
        self.audits = []
        self.wakes = []
        self.scheduled = []
        self.protection = protection or _protection()
        self.hosted = hosted
        self.background = background
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
                return int(time.time() * 1000)

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

        async def encrypt(_env, value):
            return json.dumps(value)

        async def decrypt(_env, value):
            return json.loads(value)

        async def d1_all(_env, sql, *args):
            return [dict(row) for row in harness.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            row = harness.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_run(_env, sql, *args):
            cursor = harness.db.execute(sql, args)
            return SimpleNamespace(meta=SimpleNamespace(
                changes=cursor.rowcount))

        async def audit(_env, actor, action, kind, target, outcome, details):
            harness.audits.append((actor, action, kind, target, outcome,
                                   details))

        async def notify(_env, owner, repo, topic, event_owner=""):
            harness.wakes.append((owner, repo, topic))

        async def catalog_record(_env, _owner, _repo):
            return {"source": "hosted-web" if harness.hosted
                    else "local-node"}

        def host_background(_env, key, factory):
            if not harness.background:
                return False
            harness.scheduled.append((key, factory))
            return True

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
            "encrypt_row": encrypt, "decrypt_row": decrypt,
            "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
            "MAX_NODE_NAME": 63,
            "_authorize_issue_manager": authorize,
            "_account_session_record": session_record,
            "_audit_sensitive_action": audit, "notify_repo_host": notify,
            "_repo_catalog_record": catalog_record,
            "_host_background": host_background,
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
        namespace["BRANCH_DELETE_YIELD_SECONDS"] = 0
        namespace["BRANCH_DELETE_BACKOFF_SECONDS"] = (0,)
        return namespace

    def request(self, branches=None, session="owner-session", method="POST",
                owner="alice", job=0):
        url = "https://app.test/api/repo/%s/proj/branches/delete" % owner
        if job:
            url += "?job=%d" % job
        return SimpleNamespace(
            method=method, session=session, url=url,
            body={"branches": branches} if branches is not None else {})

    async def call(self, *args, **kwargs):
        request = self.request(*args, **kwargs)
        return await self.namespace["repository_branch_delete_handler"](
            None, request, kwargs.get("owner", "alice"), "proj")

    def post(self, branches, **kwargs):
        return asyncio.run(self.call(branches, **kwargs))

    def run_job(self, branches, **kwargs):
        async def go():
            created = await self.call(branches, **kwargs)
            job = created["data"].get("job")
            while job and job["state"] == "running":
                status = await self.call(
                    method="GET", job=job["id"],
                    owner=kwargs.get("owner", "alice"),
                    session=kwargs.get("session", "owner-session"))
                job = status["data"]["job"]
                await asyncio.sleep(0.002)
            return created, job
        return asyncio.run(go())


def _schema_statements(table):
    tree = ast.parse(SCHEMA)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name)
                and target.id == "SCHEMA_STATEMENTS"
                for target in node.targets):
            statements = ast.literal_eval(node.value)
            return [statement for statement in statements
                    if table in statement]
    raise AssertionError("SCHEMA_STATEMENTS not found")


def _add_branch(world, name, oid=D):
    for core in world.cores.values():
        core.refs[name] = oid


def _statuses(job):
    return {item["branch"]: (item["status"], item["error"])
            for item in job["results"]}


def test_the_route_is_wired_and_the_job_table_is_migrated():
    assert 'r"^/api/repo/([^/]+)/([^/]+)/branches/delete$"' in URLS
    assert '"REPO_BRANCH_DELETE_RE"' in ENTRY
    assert "repository_branch_delete_handler(" in RUNTIME
    assert 'record_hosted_branch_count = _hosted_git.export(' in ENTRY
    migration = (ROOT / "migrations" / "0275_branch_delete_jobs.sql")
    assert "CREATE TABLE IF NOT EXISTS branch_delete_jobs" in \
        migration.read_text(encoding="utf-8")
    import read_only_mirrors
    assert read_only_mirrors.is_write_request(
        "POST", "/api/repo/alice/proj/branches/delete")


@pytest.mark.parametrize("session", [None, "member-session"])
@pytest.mark.parametrize("method", ["POST", "GET"])
def test_only_owners_and_organization_admins_may_delete(
        world, session, method):
    _add_branch(world, "feature")
    harness = Harness()
    response = harness.post(["feature"], session=session, method=method)
    assert response["status"] == 403
    assert response["data"]["error"] == "not_authorized"
    assert world.proxied == [] and harness.audits == []
    assert all("feature" in core.refs for core in world.cores.values())


def test_the_owner_deletes_a_branch_on_every_core_and_it_is_audited(
        world, counted):
    _add_branch(world, "feature")
    harness = Harness()
    created, job = harness.run_job([{"name": "feature", "expectedOid": D}])
    assert created["status"] == 202
    assert created["data"]["job"]["state"] == "running"
    assert job["state"] == "done"
    assert _statuses(job) == {"feature": ("deleted", "")}
    assert job["counts"]["deleted"] == 1 and job["done"] == 1
    assert all("feature" not in core.refs for core in world.cores.values())
    sent = [item[2] for item in world.proxied if item[1] == "branch-delete"]
    assert sent and all(item["discard"] is True and item["base"] == "main"
                        and item["expectedHeadOid"] == D for item in sent)
    assert harness.audits == [(
        "alice", "repository.branch_delete", "repository", "alice/proj",
        "success", {"branch": "feature", "op": "delete", "status": "deleted",
                    "error": "", "batch": 1, "cleanupPending": 0})]
    assert world.hosted_pushes == [("alice", "proj", "fmnext-n1", "alice")]
    assert counted == [("alice", "proj", 2)]


def test_an_organization_admin_may_delete_through_the_alias(world, counted):
    _add_branch(world, "feature")
    harness = Harness()
    _created, job = harness.run_job(
        ["feature"], session="admin-session", owner="acme")
    assert _statuses(job) == {"feature": ("deleted", "")}
    assert harness.audits[0][0] == "olivia"


def test_the_default_branch_is_refused(world):
    harness = Harness()
    response = harness.post(["main"])
    assert response["status"] == 200
    job = response["data"]["job"]
    assert job["state"] == "done"
    assert _statuses(job) == {"main": ("refused", "default_branch")}
    assert world.proxied == []
    assert harness.audits[0][4] == "denied"
    assert world.hosted_pushes == []


def test_protected_branches_are_refused(world, counted):
    _add_branch(world, "release/1.0")
    _add_branch(world, "feature")
    harness = Harness(protection=_protection(
        ["refs/heads/main", "refs/heads/release/"]))
    agent_branches._branch_protection = harness.protection
    _created, job = harness.run_job(["release/1.0", "feature"])
    assert _statuses(job) == {
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


def test_names_follow_git_rules_not_a_narrow_pattern(world, counted):
    plus = "team-tools/feature/researchPapersFeature+"
    _add_branch(world, plus)
    _add_branch(world, "fix#12")
    harness = Harness()
    _created, job = harness.run_job(
        [plus, "fix#12", "has space", "a..b", "-x", "x.lock", "HEAD"])
    assert _statuses(job) == {
        plus: ("deleted", ""),
        "fix#12": ("deleted", ""),
        "has space": ("refused", "invalid_branch"),
        "a..b": ("refused", "invalid_branch"),
        "-x": ("refused", "invalid_branch"),
        "x.lock": ("refused", "invalid_branch"),
        "HEAD": ("refused", "invalid_branch"),
    }
    assert all(plus not in core.refs for core in world.cores.values())
    for name, valid in {
        plus: True, "naïve": True, "a/b.c": True, "x@y": True,
        "ends.": False, "a//b": False, "@": False, "a@{1}": False,
        "tilde~": False, "q?": False, "star*": False, "b[": False,
        "c:": False, "x/.y": False, "back\\slash": False, "ctl\x07": False,
        "a" * 256: False,
    }.items():
        assert bool(agent_branches.git_branch_name(name)) is valid, name


def test_a_batch_reports_every_branch_and_publishes_once(world, counted):
    _add_branch(world, "merged/one")
    _add_branch(world, "merged/two")
    _add_branch(world, "moved", B)
    harness = Harness()
    _created, job = harness.run_job([
        {"name": "merged/one", "expectedOid": D},
        "main",
        "../escape",
        "forkmesh/pulls",
        {"name": "moved", "expectedOid": D},
        "missing",
        "merged/two",
        "merged/two",
    ])
    assert [(item["branch"], item["status"], item["error"])
            for item in job["results"]] == [
        ("../escape", "refused", "invalid_branch"),
        ("forkmesh/pulls", "refused", "reserved_branch"),
        ("merged/one", "deleted", ""),
        ("main", "refused", "default_branch"),
        ("moved", "refused", "stale_head"),
        ("missing", "refused", "branch_not_found"),
        ("merged/two", "deleted", ""),
    ]
    assert all(item["message"] for item in job["results"])
    assert job["counts"]["deleted"] == 2 and job["counts"]["refused"] == 5
    assert world.cores["fmnext-n2"].refs["moved"] == B
    assert sorted(audit[5]["branch"] for audit in harness.audits) == sorted(
        item["branch"] for item in job["results"])
    assert len(world.hosted_pushes) == 1
    assert counted == [("alice", "proj", 3)]


def test_a_core_that_refuses_is_reported_per_branch(world, counted):
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
    _created, job = Harness().run_job(["feature", "other"])
    assert _statuses(job) == {
        "feature": ("deleted", ""), "other": ("failed", "stale_base")}


def test_a_core_that_never_answers_keeps_the_branch_after_retries(
        world, counted):
    _add_branch(world, "feature")
    calls = []

    async def silent(env, endpoint, context, request, mutation):
        calls.append(request["requestId"])
        return 0, None

    agent_branches._https_mirror_merge_proxy = silent
    _created, job = Harness().run_job(["feature"])
    assert _statuses(job) == {"feature": ("failed", "merge_node_unavailable")}
    assert len(calls) == agent_branches.BRANCH_DELETE_MAX_TRIES
    assert len(set(calls)) == 1
    assert all(core.refs["feature"] == D for core in world.cores.values())


def test_requests_are_bounded():
    harness = Harness()
    too_many = harness.post(["b%d" % index for index in range(201)])
    assert too_many["status"] == 400
    assert too_many["data"] == {"error": "too_many_branches", "limit": 200}
    assert harness.post([])["data"]["error"] == "branches_required"
    assert harness.post(["x"], method="PUT")["status"] == 405


def test_a_second_request_while_one_runs_is_told_to_wait(world, counted):
    _add_branch(world, "feature")
    _add_branch(world, "other")
    harness = Harness(background=True)
    first = harness.post(["feature"])
    assert first["status"] == 202 and len(harness.scheduled) == 1
    second = harness.post(["other"])
    assert second["status"] == 409
    assert second["data"]["error"] == "delete_running"
    assert second["data"]["job"]["id"] == first["data"]["job"]["id"]
    asyncio.run(harness.scheduled[0][1]())
    latest = harness.post(None, method="GET")["data"]["job"]
    assert latest["state"] == "done"
    assert _statuses(latest) == {"feature": ("deleted", "")}


def test_fifty_deletes_run_one_at_a_time_and_pages_stay_responsive(
        world, counted):
    names = ["old/%02d" % index for index in range(50)]
    for name in names:
        _add_branch(world, name)
    real = world.proxy
    state = {"inflight": 0, "peak": 0, "order": []}

    async def slow(env, endpoint, context, request, mutation):
        state["inflight"] += 1
        state["peak"] = max(state["peak"], state["inflight"])
        try:
            await asyncio.sleep(0.004)
            state["order"].append((request["branch"], endpoint["node"]))
            return await real(env, endpoint, context, request, mutation)
        finally:
            state["inflight"] -= 1

    agent_branches._https_mirror_merge_proxy = slow
    harness = Harness(background=True)

    async def scenario():
        created = await harness.call(names)
        assert created["status"] == 202
        assert len(harness.scheduled) == 1
        worker = asyncio.ensure_future(harness.scheduled[0][1]())
        latencies, progress = [], []
        while not worker.done():
            started = time.perf_counter()
            status = await harness.call(method="GET")
            latencies.append(time.perf_counter() - started)
            progress.append(status["data"]["job"]["done"])
            await asyncio.sleep(0.01)
        await worker
        final = await harness.call(method="GET")
        return latencies, progress, final["data"]["job"]

    latencies, progress, job = asyncio.run(scenario())
    assert job["state"] == "done" and job["counts"]["deleted"] == 50
    assert state["peak"] == 1
    finished = []
    for branch, _node in state["order"]:
        if not finished or finished[-1] != branch:
            assert branch not in finished, branch
            finished.append(branch)
    assert finished == names
    assert progress == sorted(progress) and progress[-1] < 50
    assert len(latencies) > 10 and max(latencies) < 0.05
    assert all(name not in core.refs
               for core in world.cores.values() for name in names)
    assert len(world.hosted_pushes) == 1
    assert counted == [("alice", "proj", 2)]


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
    job = response["data"]["job"]
    assert job["state"] == "done"
    assert _statuses(job) == {
        "feature": ("queued", ""),
        "no-tip": ("refused", "invalid_commit"),
        "main": ("refused", "default_branch"),
    }
    rows = harness.db.execute("SELECT data FROM branch_op_inbox").fetchall()
    assert [json.loads(row["data"])["branch"] for row in rows] == ["feature"]
    assert response["data"]["ops"][0]["branch"] == "feature"
    assert ("alice", "proj", "branches") in harness.wakes


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
    assert harness.db.execute(
        "SELECT COUNT(*) FROM branch_delete_jobs").fetchone()[0] == 0


def test_a_node_owned_repository_served_by_cores_tells_its_node_too(
        world, counted):
    _add_branch(world, "feature")
    harness = Harness(hosted=False)
    _created, job = harness.run_job(["feature"])
    assert _statuses(job) == {"feature": ("deleted", "")}
    rows = harness.db.execute("SELECT data FROM branch_op_inbox").fetchall()
    assert [(json.loads(row["data"])["branch"],
             json.loads(row["data"])["source"]) for row in rows] == [
        ("feature", "cores")]


def test_the_page_follows_the_job_and_reads_the_list_past_every_cache():
    assert "async function followRepoBranchDeleteJob(repo, job, ops)" in DASHBOARD
    assert "async function resumeRepoBranchDeleteJob(repo)" in DASHBOARD
    assert "Deleted ${formatCount(deleting.deleted)} of ${formatCount(deleting.total)}" in DASHBOARD
    assert "dropRepoBranchesFromList(repo, results.filter(" in DASHBOARD
    assert "rememberRepoBranchCount(repo, listing.branches.length);" in DASHBOARD
    assert "localStorage.setItem(REPOSITORY_CATALOG_STORAGE_KEY, JSON.stringify(stored));" in DASHBOARD
    assert "fetchRepoJson(`${repoApiBase(repo)}/branches`, { canonical: refresh })" in DASHBOARD
    assert 'const direct = options.canonical ? [] : directRepoRequestCandidates(path);' in DASHBOARD
    assert 'cache: options.canonical ? "no-store" : "no-cache",' in DASHBOARD
