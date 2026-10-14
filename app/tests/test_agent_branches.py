import ast
import asyncio
import hmac
import json
from pathlib import Path
import re
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "app" / "src"
sys.path.insert(0, str(SRC))

import agent_branches  # noqa: E402


A = "a" * 40
B = "b" * 40
C = "c" * 40
D = "d" * 40
BRANCH = "agent/12-fix-login"

PATCH = (
    "diff --git a/src/login.py b/src/login.py\n"
    "index 1111111..2222222 100644\n"
    "--- a/src/login.py\n"
    "+++ b/src/login.py\n"
    "@@ -1,3 +1,4 @@\n"
    " import os\n"
    "-old = 1\n"
    "+new = 2\n"
    "+extra = 3\n"
    "diff --git a/docs/new.md b/docs/new.md\n"
    "new file mode 100644\n"
    "index 0000000..3333333\n"
    "--- /dev/null\n"
    "+++ b/docs/new.md\n"
    "@@ -0,0 +1,2 @@\n"
    "+# New\n"
    "+text\n"
    "diff --git a/gone.txt b/gone.txt\n"
    "deleted file mode 100644\n"
    "index 4444444..0000000\n"
    "--- a/gone.txt\n"
    "+++ /dev/null\n"
    "@@ -1 +0,0 @@\n"
    "-bye\n"
    "diff --git a/logo.png b/logo.png\n"
    "index 5555555..6666666 100644\n"
    "Binary files a/logo.png and b/logo.png differ\n"
)


class Response:
    def __init__(self, status, value):
        self.status = status
        self._text = json.dumps(value)

    async def text(self):
        return self._text


class Core:
    def __init__(self, name, refs):
        self.name = name
        self.refs = dict(refs)
        self.trash = {}
        self.jobs = {}


class World:
    def __init__(self):
        self.cores = {
            "fmnext-n1": Core("fmnext-n1", {"main": A, BRANCH: B}),
            "fmnext-n2": Core("fmnext-n2", {"main": A, BRANCH: B}),
        }
        self.status = "completed"
        self.record = {
            "org": "alice", "repo": "proj", "scope": "owner",
            "title": "Fix login", "runnerNode": "fmnext-r1",
            "route": "runner_pool",
            "agentInfo": {"branchName": BRANCH, "baseBranch": "main"},
            "history": [],
        }
        self.saved = []
        self.audits = []
        self.rung = []
        self.hosted_pushes = []
        self.proxied = []
        self.reads = []
        self.conflict = False
        self.owner = True
        self.lagging = set()
        self.busy = set()
        self.down = set()
        self.target = "fmnext-n1"
        self.now = 1_000
        self.signed_accounts = []
        self.signature_ok = True

    def reader(self):
        return self.cores["fmnext-n1"]

    async def routed_read(self, env, url, operation, bypass_cache=False):
        self.reads.append(url.split("?", 1)[0])
        core = self.reader()
        query = dict(
            part.split("=", 1) for part in url.split("?", 1)[1].split("&")
        ) if "?" in url else {}
        query = {key: value.replace("%2F", "/") for key, value in query.items()}
        if operation == "branches":
            return Response(200, {
                "ok": True, "defaultBranch": "main",
                "branches": [{"name": name, "commit": oid}
                             for name, oid in core.refs.items()]})
        head = core.refs.get(query.get("head"), query.get("head"))
        if head not in (B, D):
            return Response(404, {"error": "ref was not found"})
        if operation == "compare":
            return Response(200, {
                "ok": True, "baseOid": A, "headOid": head, "mergeBaseOid": A,
                "commitCount": 2, "patch": PATCH, "commits": ""})
        if operation == "merge-preview":
            return Response(200, {
                "ok": True, "baseOid": A, "headOid": head, "mergeBaseOid": A,
                "ahead": 2, "behind": 1, "clean": not self.conflict,
                "conflicts": ["src/login.py"] if self.conflict else []})
        return Response(404, {})

    async def candidates(self, env, context, country, sticky="",
                         avoid_node="", operation=""):
        return sorted(
            ({"node": name, "baseUrl": "https://" + name}
             for name in self.cores),
            key=lambda item: item["node"] != sticky)

    async def proxy(self, env, endpoint, context, request, mutation):
        core = self.cores[endpoint["node"]]
        self.proxied.append((core.name, mutation, dict(request)))
        if core.name in self.down:
            return 0, None
        key = request["requestId"]
        if key not in core.jobs or core.name in self.busy:
            core.jobs[key] = "started"
            return 202, {"ok": True, "status": "processing",
                         "requestId": key}
        branch = request["branch"]
        tip = core.refs.get(branch)
        if mutation == "branch-merge":
            if self.conflict:
                return 409, {"ok": False, "status": "failed",
                             "requestId": key, "error": "merge_conflict"}
            if core.jobs[key] == "done":
                return 200, core.jobs[key + ":result"]
            if tip != request["expectedHeadOid"]:
                return 409, {"ok": False, "status": "failed",
                             "requestId": key, "error": "stale_head"}
            core.refs[request["base"]] = C
            if request.get("deleteBranch"):
                core.trash[branch] = tip
                del core.refs[branch]
            result = {"ok": True, "status": "merged", "requestId": key,
                      "published": True, "baseBefore": A, "head": tip,
                      "baseAfter": C, "branch": branch,
                      "base": request["base"],
                      "branchDeleted": bool(request.get("deleteBranch"))}
            core.jobs[key] = "done"
            core.jobs[key + ":result"] = result
            return 200, result
        if tip is None:
            return 409, {"ok": False, "status": "failed", "requestId": key,
                         "error": "branch_not_found"}
        if tip != request["expectedHeadOid"]:
            return 409, {"ok": False, "status": "failed", "requestId": key,
                         "error": "stale_head"}
        if not request.get("discard") and (
                core.name in self.lagging or core.refs.get("main") == A):
            return 409, {"ok": False, "status": "failed", "requestId": key,
                         "error": "not_merged"}
        core.trash[branch] = tip
        del core.refs[branch]
        return 200, {"ok": True, "status": "deleted", "requestId": key,
                     "published": True, "head": tip, "branch": branch,
                     "base": request["base"], "branchDeleted": True}


@pytest.fixture
def world(monkeypatch):
    state = World()

    async def member_context(env, request, org, repo, data=None,
                             owner_scope=False, signed_account=None,
                             own_session=False):
        state.signed_accounts.append(signed_account)
        if signed_account is not None and not signed_account[0]:
            return None, {"error": "invalid_session", "status": 401}
        if not own_session and not state.owner:
            return None, {"error": "forbidden", "status": 403}
        return {"org": org, "orgBi": "org-bi", "repo": repo,
                "actor": "alice", "scope": "owner"}, None

    async def session_row(env, context, session_id):
        return ({"session_id": session_id, "org_bi": "org-bi",
                 "status": state.status, "target_node": "fmnext-n1",
                 "repo": "proj"}, state.record)

    async def d1_run(env, sql, *args):
        state.saved.append((sql, args))

    async def encrypt_row(env, record):
        return json.loads(json.dumps(record))

    async def audit(env, actor, action, kind, target, outcome, detail=None):
        state.audits.append((action, outcome, detail))

    async def ring(env, record):
        state.rung.append(True)

    async def nothing(*args, **kwargs):
        return None

    async def hosted_push(env, owner, repo, endpoint=None, pusher=""):
        state.hosted_pushes.append((owner, repo, endpoint["node"], pusher))
        return ""

    async def admit(env, context, endpoint, result):
        return "e" * 64

    async def pull_mutation_node(env, context, candidates, operation):
        return (candidates[0], "") if candidates else (None, "none")

    async def public_context(env, owner, repo):
        return {"owner": owner, "repo": repo, "repoBi": "bi"}

    async def org_repo_node(env, owner, repo):
        return None

    counter = iter(range(10, 10_000))

    async def signed_session(env, request, data=None):
        return ("bi:alice", {"name": "alice"}) if state.signature_ok \
            else ("", None)

    class Handlers:
        _pull_mutation_node = staticmethod(pull_mutation_node)
        _owner_agent_signed_triple = staticmethod(
            lambda request: "sig=" in request.url)
        _owner_agent_signed_session = staticmethod(signed_session)

    class Clock:
        @staticmethod
        def now():
            state.now += 1
            return state.now

    fakes = {
        "json_response": lambda payload, status=200, **kwargs: {
            "status": status, "payload": payload},
        "_org_agent_error": lambda code, message, status, **kwargs: {
            "status": status, "payload": {"ok": False, "error": code,
                                          "message": message}},
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "method_name": lambda request: "GET",
        "Date": Clock,
        "d1_run": d1_run,
        "encrypt_row": encrypt_row,
        "_org_agent_member_context": member_context,
        "_org_agent_session_row": session_row,
        "_routed_repository_read": state.routed_read,
        "_org_repo_node": org_repo_node,
        "_https_mirror_public_context": public_context,
        "_HttpsMirrorContextUnavailable": RuntimeError,
        "_https_mirror_candidates": state.candidates,
        "world_request_country": lambda request: "",
        "_route_handlers": Handlers,
        "_https_mirror_merge_proxy": state.proxy,
        "_admit_owner_merge_state": admit,
        "schedule_repo_mirror_wake": nothing,
        "notify_repo_mirrors": nothing,
        "notify_repo_host": nothing,
        "record_hosted_push_state": hosted_push,
        "_audit_sensitive_action": audit,
        "_ring_cloud_agent_readers": ring,
        "_random_bytes": lambda n: (str(next(counter)) * n).encode()[:n],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(value or ""))),
    }
    for name, value in fakes.items():
        monkeypatch.setattr(agent_branches, name, value, raising=False)
    return state


class Request:
    def __init__(self, url="https://app.test/api/repo/alice/proj/agent-sessions/s1/diff"):
        self.url = url


def run(coroutine):
    return asyncio.run(coroutine)


def action(world, name, **data):
    context = {"org": "alice", "orgBi": "org-bi", "repo": "proj",
               "actor": "alice", "scope": "owner"}
    row = {"session_id": "s1", "org_bi": "org-bi", "status": world.status,
           "target_node": world.target}
    return run(agent_branches.session_action(
        None, Request(), context, row, world.record, name, data, True))


def test_patch_files_count_lines_per_file_and_keep_each_patch():
    files = agent_branches.parse_patch_files(PATCH)
    assert [(f["path"], f["status"], f["added"], f["removed"], f["binary"])
            for f in files] == [
        ("src/login.py", "modified", 2, 1, False),
        ("docs/new.md", "added", 2, 0, False),
        ("gone.txt", "deleted", 0, 1, False),
        ("logo.png", "modified", 0, 0, True),
    ]
    assert files[0]["patch"].startswith("diff --git a/src/login.py")
    assert "+extra = 3" in files[0]["patch"]
    assert "docs/new.md" not in files[0]["patch"]


def test_projection_names_the_branch_runner_stats_and_merge_state():
    row = {"status": "completed", "target_node": "fmnext-n1"}
    record = {
        "runnerNode": "fmnext-r1", "route": "runner_pool",
        "agentInfo": {"branchName": BRANCH, "baseBranch": "main"},
        "diffStats": {"files": 3, "added": 40, "removed": 2, "known": True},
    }
    extras = agent_branches.projection_extras(row, record)
    assert extras["branch"] == BRANCH and extras["baseBranch"] == "main"
    assert extras["runner"] == "fmnext-r1"
    assert extras["diffStats"] == {"files": 3, "added": 40, "removed": 2,
                                   "ahead": 0, "behind": 0,
                                   "conflicted": False, "known": True}
    assert extras["merge"]["state"] == "" and extras["canMerge"] is True
    running = agent_branches.projection_extras(
        {"status": "running", "target_node": "fmnext-n1"}, record)
    assert running["canMerge"] is False
    merged = agent_branches.projection_extras(row, dict(
        record, agentInfo={"branchName": BRANCH, "merged": True}))
    assert merged["merge"]["state"] == "merged" and not merged["canMerge"]
    mirror = agent_branches.projection_extras(
        {"status": "completed", "target_node": "mirror35"},
        {"runnerNode": "mirror35",
         "agentInfo": {"branchName": "agent/web-7-x"}})
    assert mirror["runner"] == "" and mirror["canMerge"] is True
    desktop = agent_branches.projection_extras(
        row, {"agentInfo": {"branchName": "412-fix-login"}})
    assert desktop["canMerge"] is False


def test_diff_is_paged_in_the_desktop_diff_shape(world):
    response = run(agent_branches.diff_handler(
        None, Request(Request().url + "?page=1&pageSize=3"),
        "alice", "proj", "s1", owner_scope=True))
    body = response["payload"]
    assert response["status"] == 200
    assert body["branch"] == BRANCH and body["base"] == "main"
    assert [f["path"] for f in body["files"]] == [
        "src/login.py", "docs/new.md", "gone.txt", "logo.png"]
    assert body["added"] == 4 and body["removed"] == 2
    assert body["page"] == 1 and body["pageCount"] == 2
    assert body["committed"].startswith("diff --git a/logo.png")
    assert body["truncated"] is False
    assert body["ahead"] == 2 and body["behind"] == 1 and body["clean"]
    assert body["uncommitted"] == "" and body["untracked"] == []
    first = run(agent_branches.diff_handler(
        None, Request(Request().url + "?pageSize=500"), "alice", "proj",
        "s1", owner_scope=True))["payload"]
    assert first["pageSize"] == 100 and first["truncated"] is False
    assert world.record["diffStats"] == {
        "files": 4, "added": 4, "removed": 2, "ahead": 2, "behind": 1,
        "conflicted": False, "known": True}


def test_diff_before_the_branch_exists_says_so(world):
    world.record["agentInfo"] = {}
    response = run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    assert response["status"] == 409
    assert response["payload"]["error"] == "no_branch"
    world.record["agentInfo"] = {"branchName": "agent/not-pushed"}
    response = run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    assert response["status"] == 404
    assert response["payload"]["error"] == "branch_not_found"


def _push_nothing(world):
    for core in world.cores.values():
        core.refs.pop(BRANCH, None)
    world.record["diffStats"] = {"files": 0, "added": 0, "removed": 0,
                                 "ahead": 0, "known": True}


def test_a_finished_run_that_pushed_nothing_shows_no_changes(world):
    _push_nothing(world)
    response = run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    body = response["payload"]
    assert response["status"] == 200
    assert body["branch"] == BRANCH and body["base"] == "main"
    assert body["files"] == [] and body["committed"] == ""
    assert body["merged"] is False


def test_merging_a_run_that_pushed_nothing_finishes_like_the_desktop(world):
    _push_nothing(world)
    response = action(world, "merge", cleanup=True)
    body = response["payload"]
    assert response["status"] == 200
    assert body["status"] == "merged" and body["branchDeleted"] is True
    assert body["stats"] == {"base": "main", "commit": "", "commits": 0,
                             "files": 0, "insertions": 0, "deletions": 0}
    assert body["message"] == (
        BRANCH + " had no changes, so nothing was merged into main. "
        "Cleaned up.")
    assert world.proxied == []
    assert world.record["merge"]["state"] == "merged"
    assert world.record["agentInfo"]["merged"] is True
    assert world.rung
    extras = agent_branches.projection_extras(
        {"status": "completed"}, world.record)
    assert extras["canMerge"] is False
    after = run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    assert after["status"] == 200 and after["payload"]["merged"] is True


def test_a_queued_run_that_never_reported_has_nothing_to_merge(world):
    _push_nothing(world)
    world.record.pop("diffStats")
    world.record["agentInfo"] = {}
    world.record["branch"] = BRANCH
    world.status = "stopped"
    assert action(world, "merge")["payload"]["status"] == "merged"


def test_a_missing_branch_with_work_or_a_failed_push_is_not_merged(world):
    _push_nothing(world)
    world.record["diffStats"]["files"] = 2
    response = action(world, "merge")
    assert response["status"] == 404
    assert response["payload"]["error"] == "branch_not_found"
    world.record["diffStats"]["files"] = 0
    world.record["agentInfo"]["lastError"] = "publish branch: denied"
    response = action(world, "merge")
    assert response["payload"]["error"] == "branch_not_found"
    assert "could not push it (publish branch: denied)" in response[
        "payload"]["message"]
    diff = run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    assert diff["status"] == 404
    world.record["agentInfo"].pop("lastError")
    world.status = "running"
    diff = run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    assert diff["status"] == 404
    assert "once the runner pushes them" in diff["payload"]["message"]
    assert "merge" not in world.record


def test_a_node_owned_session_reads_the_repository_its_organization_serves(
        world, monkeypatch):
    async def d1_all(env, sql, *args):
        assert "org_repos" in sql and args == ("fmnext-n1", "proj")
        return [{"name": "Forkmesh"}]

    async def org_repo_node(env, owner, repo):
        return "fmnext-n1" if (owner, repo) == ("forkmesh", "proj") else ""

    contexts = []

    async def candidates(env, context, country, sticky="", avoid_node="",
                         operation=""):
        contexts.append(dict(context))
        return await world.candidates(env, context, country, sticky,
                                      avoid_node, operation)

    monkeypatch.setattr(agent_branches, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(agent_branches, "_org_repo_node", org_repo_node,
                        raising=False)
    monkeypatch.setattr(agent_branches, "_https_mirror_candidates",
                        candidates, raising=False)
    monkeypatch.setattr(agent_branches, "valid_node_name",
                        lambda name: bool(re.fullmatch(r"[a-z0-9-]+", name)),
                        raising=False)
    response = run(agent_branches.diff_handler(
        None, Request(), "fmnext-n1", "proj", "s1", owner_scope=True))
    assert response["status"] == 200
    assert world.reads and all(
        url.startswith("https://forkmesh.internal/api/repo/forkmesh/proj/")
        for url in world.reads)
    world.reads.clear()
    context = {"org": "fmnext-n1", "orgBi": "org-bi", "repo": "proj",
               "actor": "alice", "scope": "owner"}
    row = {"session_id": "s1", "org_bi": "org-bi", "status": world.status,
           "target_node": world.target}
    run(agent_branches.session_action(
        None, Request(), context, row, world.record, "merge",
        {"cleanup": True}, True))
    assert world.reads and all(
        "/api/repo/forkmesh/proj/" in url for url in world.reads)
    assert context["org"] == "fmnext-n1"
    assert contexts and all(
        item["owner"] == "fmnext-n1" and item["routeOwner"] == "forkmesh"
        for item in contexts)
    assert world.proxied


def test_an_unlinked_owner_keeps_its_own_name(world, monkeypatch):
    async def d1_all(env, sql, *args):
        return [{"name": "one"}, {"name": "two"}]

    monkeypatch.setattr(agent_branches, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(agent_branches, "valid_node_name", lambda name: True,
                        raising=False)
    run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    assert world.reads and all(
        "/api/repo/alice/proj/" in url for url in world.reads)


def test_merge_and_clean_up_polls_then_removes_the_branch_everywhere(world):
    started = action(world, "merge", cleanup=True)
    assert started["status"] == 202
    request_id = started["payload"]["requestId"]
    assert world.record["merge"]["state"] == "merging"
    sent = world.proxied[0][2]
    assert sent == {
        "schemaVersion": 1, "type": "forkmesh.branch-merge-v1",
        "requestId": request_id, "branch": BRANCH, "base": "main",
        "expectedBaseOid": A, "expectedHeadOid": B, "deleteBranch": True}
    world.cores["fmnext-n2"].refs["main"] = C
    done = action(world, "merge", cleanup=True, requestId=request_id)
    assert done["status"] == 200
    body = done["payload"]
    assert body["status"] == "merged" and body["branchDeleted"] is True
    assert body["mergeOid"] == C and body["cleanupPending"] == []
    assert body["stats"] == {"base": "main", "commit": C, "commits": 2,
                             "files": 4, "insertions": 4, "deletions": 2}
    assert body["message"] == (
        "Merged " + BRANCH + " into main and cleaned up.")
    assert BRANCH not in world.cores["fmnext-n1"].refs
    assert BRANCH not in world.cores["fmnext-n2"].refs
    assert world.cores["fmnext-n2"].trash[BRANCH] == B
    assert world.record["merge"]["state"] == "merged"
    assert world.record["agentInfo"]["merged"] is True
    assert [a[1] for a in world.audits] == ["requested", "success"]
    assert world.rung
    assert world.hosted_pushes == [
        ("alice", "proj", world.proxied[0][0], "alice")]
    again = action(world, "merge", cleanup=True)
    assert again["status"] == 409 or again["payload"]["status"] == "cleaned"


def test_a_mirror_session_merges_on_the_mirror_that_ran_it(world):
    world.target = "fmnext-n2"
    world.record.update(runnerNode="fmnext-n2", route="owner_node")
    first = action(world, "merge", cleanup=False)
    assert first["status"] == 202
    assert world.record["merge"]["node"] == "fmnext-n2"
    done = action(world, "merge", cleanup=False,
                  requestId=first["payload"]["requestId"])
    assert done["payload"]["status"] == "merged"
    assert {name for name, _, _ in world.proxied} == {"fmnext-n2"}
    assert world.cores["fmnext-n2"].refs["main"] == C


def test_a_runner_session_has_no_home_node():
    assert agent_branches.session_home(
        {"target_node": "fmnext-n1"},
        {"runnerNode": "fmnext-r1", "route": "runner_pool"}) == ""
    assert agent_branches.session_home(
        {"target_node": "mirror35"}, {"runnerNode": "mirror35"}) == "mirror35"


def test_a_second_tap_resumes_the_merge_already_running(world):
    started = action(world, "merge", cleanup=True,
                     requestId="phone_request_0001")
    assert started["payload"]["requestId"] == "phone_request_0001"
    retry = action(world, "merge", cleanup=True,
                   requestId="phone_request_0002")
    assert retry["status"] == 200
    assert retry["payload"]["requestId"] == "phone_request_0001"
    merges = [item for item in world.proxied if item[1] == "branch-merge"]
    assert {item[2]["requestId"] for item in merges} == {"phone_request_0001"}
    assert agent_branches.projection_extras(
        {"status": "completed"}, world.record)["canMerge"] is False


def test_a_merge_left_merging_is_finished_by_a_later_tap(world):
    action(world, "merge", cleanup=True, requestId="phone_request_0001")
    world.now += agent_branches.PENDING_MERGE_MS + 1
    later = action(world, "merge", cleanup=True,
                   requestId="phone_request_0002")
    assert later["status"] == 200
    assert later["payload"]["status"] == "merged"
    assert later["payload"]["requestId"] == "phone_request_0001"
    assert world.record["merge"]["state"] == "merged"
    merges = [item for item in world.proxied if item[1] == "branch-merge"]
    assert {item[2]["requestId"] for item in merges} == {"phone_request_0001"}


def test_a_stale_merge_on_a_core_that_left_starts_over(world):
    action(world, "merge", cleanup=True, requestId="phone_request_0001")
    first = world.record["merge"]["node"]
    world.cores.pop(first)
    world.reader = lambda: next(iter(world.cores.values()))
    world.now += agent_branches.PENDING_MERGE_MS + 1
    later = action(world, "merge", cleanup=True,
                   requestId="phone_request_0002")
    assert later["status"] == 202
    assert later["payload"]["requestId"] == "phone_request_0002"
    assert world.record["merge"]["node"] != first


def test_a_lagging_core_stays_pending_until_the_next_clean_up(world):
    world.lagging.add("fmnext-n2")
    action(world, "merge", cleanup=True)
    request_id = world.record["merge"]["requestId"]
    done = action(world, "merge", cleanup=True, requestId=request_id)
    assert done["payload"]["cleanupPending"] == ["fmnext-n2"]
    assert "fmnext-n2 still has it" in done["payload"]["message"]
    assert BRANCH in world.cores["fmnext-n2"].refs
    world.lagging.clear()
    world.cores["fmnext-n2"].refs["main"] = C
    cleaned = action(world, "cleanup")
    assert cleaned["status"] == 200
    assert cleaned["payload"]["cleanupPending"] == []
    assert BRANCH not in world.cores["fmnext-n2"].refs


def test_a_conflict_changes_nothing_and_lists_the_files(world):
    world.conflict = True
    action(world, "merge", cleanup=True)
    request_id = world.record["merge"]["requestId"]
    failed = action(world, "merge", cleanup=True, requestId=request_id)
    assert failed["status"] == 409
    assert failed["payload"]["error"] == "merge_conflict"
    assert failed["payload"]["conflicts"] == ["src/login.py"]
    assert world.record["merge"]["state"] == "conflict"
    assert world.cores["fmnext-n1"].refs[BRANCH] == B
    assert world.cores["fmnext-n1"].refs["main"] == A
    world.conflict = False
    retried = action(world, "merge", cleanup=True)
    assert retried["status"] == 202
    assert retried["payload"]["requestId"] != request_id


def test_clean_up_refuses_a_branch_that_is_not_merged(world):
    refused = action(world, "cleanup")
    assert refused["status"] == 409
    assert refused["payload"]["error"] == "not_merged"
    assert world.cores["fmnext-n1"].refs[BRANCH] == B
    assert world.cores["fmnext-n2"].refs[BRANCH] == B


def test_discard_deletes_a_branch_that_is_not_merged(world):
    deleted = action(world, "cleanup", discard=True,
                     requestId="phone_request_0001")
    assert deleted["status"] == 200
    assert deleted["payload"]["branchDeleted"] is True
    assert deleted["payload"]["cleanupPending"] == []
    for core in world.cores.values():
        assert BRANCH not in core.refs
        assert core.trash[BRANCH] == B
        assert core.refs["main"] == A
    sent = [item[2] for item in world.proxied if item[1] == "branch-delete"]
    assert sent and all(item["discard"] is True for item in sent)
    assert all(item["requestId"].startswith("phone_request_0001-d")
               for item in sent)
    assert world.record["merge"].get("state", "") != "merged"


def test_discard_keeps_the_run_when_no_core_deleted_the_branch(world):
    stale_reader = Core("reader", {"main": A, BRANCH: D})
    world.reader = lambda: stale_reader
    refused = action(world, "cleanup", discard=True)
    assert refused["status"] == 409
    assert refused["payload"]["error"] == "stale_head"
    for core in world.cores.values():
        assert core.refs[BRANCH] == B


def test_discard_a_core_is_still_running_is_polled_not_failed(world):
    world.busy.update(world.cores)
    first = action(world, "cleanup", discard=True,
                   requestId="phone_request_0001")
    assert first["status"] == 202
    assert first["payload"]["status"] == "processing"
    assert first["payload"]["requestId"] == "phone_request_0001"
    assert not [item for item in world.audits if item[1] == "failed"]
    world.busy.clear()
    done = action(world, "cleanup", discard=True,
                  requestId="phone_request_0001")
    assert done["status"] == 200
    assert done["payload"]["branchDeleted"] is True
    sent = {item[2]["requestId"] for item in world.proxied
            if item[1] == "branch-delete"}
    assert len(sent) == len(world.cores)
    assert all(item.startswith("phone_request_0001-d") for item in sent)
    for core in world.cores.values():
        assert BRANCH not in core.refs


def test_discard_no_core_answering_keeps_the_run(world):
    world.down.update(world.cores)
    kept = action(world, "cleanup", discard=True)
    assert kept["status"] == 503
    assert kept["payload"]["error"] == "branch_delete_pending"
    assert ("owner.agent_branch_discard", "failed") in {
        item[:2] for item in world.audits}
    for core in world.cores.values():
        assert core.refs[BRANCH] == B


def test_a_cleanup_without_discard_never_sends_it(world):
    action(world, "cleanup")
    sent = [item[2] for item in world.proxied if item[1] == "branch-delete"]
    assert sent and all("discard" not in item for item in sent)


def test_merge_waits_for_a_running_agent_and_needs_an_agent_branch(world):
    world.status = "running"
    assert action(world, "merge")["payload"]["error"] == "session_running"
    assert action(world, "cleanup")["payload"]["error"] == "session_running"
    world.status = "completed"
    world.record["agentInfo"] = {"branchName": "412-fix-login"}
    assert action(world, "merge")["payload"]["error"] == "no_branch"
    assert world.proxied == []


def test_a_stale_head_clears_the_attempt_so_the_next_one_reads_again(world):
    action(world, "merge", cleanup=False)
    request_id = world.record["merge"]["requestId"]
    world.cores["fmnext-n1"].refs[BRANCH] = D
    stale = action(world, "merge", cleanup=False, requestId=request_id)
    assert stale["status"] == 409
    assert stale["payload"]["error"] == "stale_head"
    assert world.record["merge"] == {}


def test_next_branch_name_numbers_like_the_desktop(monkeypatch):
    records = [
        {"agentInfo": {"branchName": "agent/7-old"}},
        {"branch": "agent/12-fix-login"},
        {"agentInfo": {"branchName": "agent/web-3-x"}},
        {},
    ]

    async def d1_all(env, sql, *args):
        assert "org_agent_sessions" in sql
        return [{"data": record} for record in records]

    async def decrypt_row(env, value):
        return value

    monkeypatch.setattr(agent_branches, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(agent_branches, "decrypt_row", decrypt_row,
                        raising=False)
    name = run(agent_branches.next_branch_name(
        None, "org-bi", "proj", "Fix the login page's redirect loop!"))
    assert name == "agent/13-fix-the-login-page-s-redirect"
    assert run(agent_branches.next_branch_name(
        None, "org-bi", "proj", "   ")) == ""
    assert agent_branches.valid_base("main") == "main"
    assert agent_branches.valid_base("agent/x") == ""
    assert agent_branches.valid_base("forkmesh/pulls") == ""
    assert agent_branches.valid_base("../main") == ""


def _entry_function(name, constants=("HTTPS_MIRROR_PULL_MUTATIONS",)):
    source = (SRC / "entry.py").read_text("utf-8")
    tree = ast.parse(source)
    nodes = [node for node in tree.body
             if isinstance(node, ast.Assign)
             and isinstance(node.targets[0], ast.Name)
             and node.targets[0].id in constants]
    nodes += [node for node in tree.body
              if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
              and node.name == name]
    namespace = {"re": re, "hmac": hmac}
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), "entry.py", "exec"),
         namespace)
    return namespace


def test_relay_result_check_pins_the_branch_and_its_tips():
    namespace = _entry_function("_https_mirror_merge_result")
    check = namespace["_https_mirror_merge_result"]
    profiles = namespace["HTTPS_MIRROR_PULL_MUTATIONS"]
    assert profiles["branch-merge"]["operation"] == "merge-agent-branch"
    assert profiles["branch-delete"]["operation"] == "delete-agent-branch"
    request = {"requestId": "branch_request_0001", "branch": BRANCH,
               "base": "main", "expectedBaseOid": A, "expectedHeadOid": B}
    result = {"ok": True, "status": "merged",
              "requestId": "branch_request_0001", "published": True,
              "baseBefore": A, "head": B, "baseAfter": C, "branch": BRANCH,
              "base": "main", "branchDeleted": True}
    checked = check(result, request, "branch-merge")
    assert checked["baseAfter"] == C and checked["branchDeleted"] is True
    assert "pullsBefore" not in checked
    assert check(dict(result, head=D), request, "branch-merge") is None
    assert check(dict(result, branch="agent/other"), request,
                 "branch-merge") is None
    deleted = {"ok": True, "status": "deleted",
               "requestId": "branch_request_0001", "published": True,
               "head": B, "branch": BRANCH, "base": "main",
               "branchDeleted": True}
    cleanup_request = dict(request)
    cleanup_request.pop("expectedBaseOid")
    assert check(deleted, cleanup_request, "branch-delete")["status"] == \
        "deleted"
    assert check({"ok": False, "status": "failed",
                  "requestId": "branch_request_0001",
                  "error": "not_merged"}, cleanup_request,
                 "branch-delete")["error"] == "not_merged"
    assert check({"ok": False, "status": "failed",
                  "requestId": "branch_request_0001",
                  "error": "not_merged"}, request, "branch-merge") is None


def test_route_handlers_send_merge_and_cleanup_to_the_owner_only_path():
    text = (SRC / "route_handlers.py").read_text("utf-8")
    handler = text[text.index("async def org_agent_bot_handler"):]
    handler = handler[:handler.index("\nasync def ")]
    assert ('if branch_action not in ("merge", "cleanup", "link-pull", '
            '"update"):' in handler)
    assert "own_session=method in (\"GET\", \"DELETE\") or stopping)" in \
        handler
    assert "agent_branches.session_action(" in handler
    assert 'job["branch"] = session_branch["branch"]' in handler
    assert 'record["lastMerge"] = record.pop("merge")' in handler
    start = text[text.index("async def org_agent_bots_handler"):]
    assert 'data.get("baseBranch")' in start
    assert "agent_branches.next_branch_name(" in start
    urls = (SRC / "urls.py").read_text("utf-8")
    assert "agent-sessions/([^/]+)/diff$" in urls
    runtime = (SRC / "app_runtime.py").read_text("utf-8")
    assert "agent_branches.diff_handler(" in runtime


def _bot_handler(extra):
    text = (SRC / "route_handlers.py").read_text("utf-8")
    tree = ast.parse(text)
    nodes = [node for node in tree.body
             if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
             and node.name == "org_agent_bot_handler"]
    namespace = dict(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), "route_handlers.py",
        "exec"), namespace)
    return namespace["org_agent_bot_handler"]


def _bot_harness(body, owner=True, status="completed", record=None,
                 private_block=None):
    calls = {"own_session": [], "batches": []}

    async def member_context(env, request, org, repo, data, owner_scope=False,
                             signed_account=None, own_session=False):
        calls["own_session"].append(own_session)
        if not owner and not own_session:
            return None, {"error": "forbidden"}
        return ({"org": "alice", "orgBi": "org-bi", "repo": "proj",
                 "actor": "alice", "scope": "owner"}, None)

    async def session_row(env, context, session_id):
        return ({"session_id": "s1", "status": status,
                 "target_node": "fmnext-n1", "provider": "claude-code"},
                {"localAgentId": 7, "history": [], "title": "Fix login",
                 "runnerNode": "fmnext-r1", "route": "runner_pool",
                 "merge": {"state": "merged"},
                 "agentInfo": {"branchName": BRANCH, "baseBranch": "main"},
                 **(record or {})})

    async def request_json(request):
        return dict(body), None

    async def batch(env, statements):
        calls["batches"].append(statements)

    async def encrypt_row(env, record):
        return record

    async def nothing(*args, **kwargs):
        return None

    class Clock:
        @staticmethod
        def now():
            return 5_000

    handler = _bot_handler({
        "ensure_schema": nothing,
        "method_name": lambda request: "POST",
        "_owner_agent_signed_triple": lambda request: False,
        "_request_origin_allowed": lambda env, request: True,
        "_org_agent_request_json": request_json,
        "_org_agent_member_context": member_context,
        "_org_agent_session_row": session_row,
        "_org_agent_parse_attachments": lambda value: ([], None),
        "_org_agent_security_envelope": lambda context, scope: (
            {"state": "not_required"}, {"required": False}, "queued"),
        "_contribution_run_batch": batch,
        "_audit_sensitive_action": nothing,
        "notify_repo_host": nothing,
        "encrypt_row": encrypt_row,
        "Date": Clock,
        "ORG_AGENT_MAX_HISTORY": 50,
        "ORG_AGENT_MAX_PROMPT": 8000,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "json_response": lambda payload, **kwargs: {
            "payload": payload, "status": kwargs.get("status", 200)},
        "_org_agent_error": lambda code, *args, **kwargs: {"error": code},
        "_org_agent_private_runner_block": private_block or nothing,
    })
    return handler, calls


def test_only_an_owner_reaches_merge_and_cleanup():
    for name in ("merge", "cleanup", "link-pull"):
        handler, calls = _bot_harness({"action": name}, owner=False)
        response = run(handler(object(), object(), "alice", "proj", "s1",
                               owner_scope=True))
        assert response == {"error": "forbidden"}
        assert calls["own_session"] == [False]


def test_a_follow_up_on_a_runner_session_carries_its_branch():
    handler, calls = _bot_harness({"prompt": "Continue where you left off."})
    response = run(handler(object(), object(), "alice", "proj", "s1",
                           owner_scope=True))
    assert response["status"] == 202
    (statements,) = calls["batches"]
    session_update, insert = statements
    job = insert[1][5]
    assert job["kind"] == "steer"
    assert job["branch"] == BRANCH and job["baseBranch"] == "main"
    assert job["title"] == "Fix login"
    record = session_update[1][1]
    assert "merge" not in record and record["lastMerge"]["state"] == "merged"


def test_a_follow_up_on_a_runner_session_names_the_runner_owner():
    for extra, owner in (({"runnerOwner": "jett"}, "jett"),
                         ({"createdBy": "Jett"}, "jett")):
        handler, calls = _bot_harness({"prompt": "continue lets go with A"},
                                      record=extra)
        response = run(handler(object(), object(), "fmnext-n1", "proj", "s1",
                               owner_scope=True))
        assert response["status"] == 202
        job = calls["batches"][0][1][1][5]
        assert job["organization"] == owner


def test_a_follow_up_on_someone_elses_personal_runner_is_refused():
    async def refuse(env, row, record, actor):
        assert actor == "alice" and record["runnerNode"] == "fmnext-r1"
        return {"error": "runner_private"}

    handler, calls = _bot_harness({"prompt": "go on"}, private_block=refuse)
    response = run(handler(object(), object(), "fmnext-n1", "proj", "s1",
                           owner_scope=True))
    assert response == {"error": "runner_private"}
    assert calls["batches"] == []


def _route_functions(*names, **extra):
    tree = ast.parse((SRC / "route_handlers.py").read_text("utf-8"))
    nodes = [node for node in tree.body
             if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
             and node.name in names]
    namespace = dict(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), "route_handlers.py",
        "exec"), namespace)
    return [namespace[name] for name in names]


def test_only_the_runner_owner_may_follow_up_on_a_personal_runner_session():
    runners = {"alice-box": {"scope": "personal", "registered_by": "alice"},
               "pool-1": {"scope": "pool", "registered_by": "root"}}

    async def d1_first(env, sql, name):
        return runners.get(name)

    [block] = _route_functions(
        "_org_agent_private_runner_block", d1_first=d1_first,
        _org_agent_error=lambda code, *args, **kwargs: {"error": code})
    row = {"target_node": "fmnext-n1"}

    def check(runner, actor):
        record = {"runnerNode": runner, "route": "runner_pool"}
        return run(block(object(), row, record, actor))

    assert check("alice-box", "alice") is None
    assert check("alice-box", "bob") == {"error": "runner_private"}
    assert check("pool-1", "bob") is None
    assert run(block(object(), row, {"runnerNode": ""}, "bob")) is None


def test_a_personal_runner_is_hidden_from_other_members_of_the_session():
    [hide] = _route_functions("_org_agent_hide_private_runner")
    row = {"target_node": "fmnext-n1"}
    record = {"runnerPrivate": True, "runnerOwner": "alice",
              "runnerNode": "alice-box"}

    def projection():
        return {"runner": "alice-box", "targetNode": "alice-box",
                "queue": {"jobId": 1, "runner": {"name": "alice-box"}}}

    assert hide(projection(), row, record, "alice") == projection()
    hidden = hide(projection(), row, record, "bob")
    assert hidden == {"runner": "", "targetNode": "fmnext-n1",
                      "queue": {"jobId": 1}}
    shared = dict(record, runnerPrivate=False)
    assert hide(projection(), row, shared, "bob") == projection()


def test_a_follow_up_on_a_mirror_session_keeps_the_organization():
    handler, calls = _bot_harness(
        {"prompt": "go on"},
        record={"route": "owner_node", "runnerNode": "", "createdBy": "jett"})
    run(handler(object(), object(), "alice", "proj", "s1", owner_scope=True))
    assert calls["batches"][0][1][1][5]["organization"] == "alice"


def test_projection_offers_a_pull_request_like_a_desktop_agent():
    row = {"status": "completed", "target_node": "fmnext-n1"}
    record = {
        "runnerNode": "fmnext-r1", "route": "runner_pool",
        "agentInfo": {"branchName": BRANCH, "baseBranch": "main",
                      "prNumber": 0},
        "diffStats": {"files": 3, "added": 40, "removed": 2, "ahead": 2,
                      "known": True},
    }
    extras = agent_branches.projection_extras(row, record)
    assert extras["prNumber"] == 0 and extras["canOpenPull"] is True
    linked = agent_branches.projection_extras(
        row, dict(record, pull={"number": 57}))
    assert linked["prNumber"] == 57 and linked["canOpenPull"] is False
    reported = agent_branches.projection_extras(row, dict(
        record, agentInfo=dict(record["agentInfo"], prNumber=9)))
    assert reported["prNumber"] == 9 and not reported["canOpenPull"]
    for blocked in (
        ({"status": "running", "target_node": "fmnext-n1"}, record),
        (row, dict(record, merge={"state": "merged"})),
        (row, dict(record, merge={"state": "merging"})),
        (row, dict(record, diffStats={"files": 0, "ahead": 0,
                                      "known": True})),
        (row, dict(record, agentInfo={"branchName": "412-fix-login"})),
    ):
        assert agent_branches.projection_extras(*blocked)[
            "canOpenPull"] is False


def test_linking_a_pull_request_survives_the_runner_report(world):
    response = action(world, "link-pull", number=57)
    assert response["status"] == 200
    assert response["payload"]["prNumber"] == 57
    assert world.record["pull"]["number"] == 57
    assert world.record["pull"]["branch"] == BRANCH
    assert world.rung and world.audits[-1][0] == "owner.agent_branch_pull"
    world.record["agentInfo"] = {"branchName": BRANCH, "prNumber": 0}
    row = {"status": "completed", "target_node": "fmnext-n1"}
    assert agent_branches.projection_extras(
        row, world.record)["prNumber"] == 57
    saved = len(world.saved)
    assert action(world, "link-pull", number=57)["status"] == 200
    assert len(world.saved) == saved
    refused = action(world, "link-pull", number=58)
    assert refused["payload"]["error"] == "pull_exists"
    assert world.record["pull"]["number"] == 57


def test_a_pull_link_needs_a_number_and_an_agent_branch(world):
    for number in (0, -1, "57", True, None, 10_000_001):
        response = action(world, "link-pull", number=number)
        assert response["payload"]["error"] == "invalid_pull"
    world.record["agentInfo"] = {"branchName": "412-fix-login"}
    assert action(world, "link-pull", number=3)["payload"][
        "error"] == "no_branch"
    assert "pull" not in world.record


def test_a_signed_desktop_reads_the_diff_with_its_proof(world):
    url = ("https://app.test/api/repo/alice/proj/agent-sessions/s1/diff"
           "?page=0&user=alice&ts=1&sig=x")
    response = run(agent_branches.diff_handler(
        None, Request(url), "alice", "proj", "s1", owner_scope=True))
    assert response["status"] == 200
    assert world.signed_accounts[-1] == ("bi:alice", {"name": "alice"})
    world.signature_ok = False
    refused = run(agent_branches.diff_handler(
        None, Request(url), "alice", "proj", "s1", owner_scope=True))
    assert refused["error"] == "invalid_session"
    plain = run(agent_branches.diff_handler(
        None, Request(), "alice", "proj", "s1", owner_scope=True))
    assert plain["status"] == 200 and world.signed_accounts[-1] is None


def core_op(world, record, data=None):
    return run(agent_branches.core_branch_op(
        None, Request(), "alice", "proj", record, data or {}, "main"))


def core_op_until_done(world, record):
    status, answer = core_op(world, record)
    assert status == 202 and answer["status"] == "processing"
    resume = {key: answer[key] for key in ("requestId", "node", "pinBase")}
    if "pinOid" in answer:
        resume["pinOid"] = answer["pinOid"]
    return core_op(world, record, resume)


def test_any_branch_merges_on_the_cores_without_a_desktop(world):
    for core in world.cores.values():
        core.refs["feature"] = D
    status, answer = core_op_until_done(world, {
        "op": "merge", "branch": "feature", "base": "main",
        "expectedOid": D, "baseOid": A, "deleteBranch": True})
    assert status == 200 and answer["status"] == "merged"
    assert answer["executed"] is True and answer["mergeOid"] == C
    assert answer["branchDeleted"] is True
    assert answer["cleanupPending"] == ["fmnext-n2"]
    merges = [item for item in world.proxied if item[1] == "branch-merge"]
    assert merges[0][2]["branch"] == "feature"
    assert merges[0][2]["expectedBaseOid"] == A
    assert merges[0][2]["expectedHeadOid"] == D
    assert merges[0][2]["deleteBranch"] is True
    assert world.cores["fmnext-n1"].refs["main"] == C
    assert "feature" not in world.cores["fmnext-n1"].refs


def test_updating_a_branch_merges_its_base_into_it_on_the_cores(world):
    status, answer = core_op_until_done(world, {
        "op": "update", "branch": BRANCH, "base": "", "expectedOid": B})
    assert status == 200 and answer["status"] == "updated"
    assert answer["base"] == "main" and answer["pinOid"] == A
    sent = [item[2] for item in world.proxied if item[1] == "branch-merge"]
    assert sent[0]["branch"] == "main" and sent[0]["base"] == BRANCH
    assert sent[0]["expectedBaseOid"] == B and sent[0]["expectedHeadOid"] == A
    assert world.cores["fmnext-n1"].refs[BRANCH] == C
    assert world.cores["fmnext-n1"].refs["main"] == A


def test_deleting_a_branch_on_the_cores_removes_it_everywhere(world):
    for core in world.cores.values():
        core.refs["feature"] = D
    status, answer = core_op_until_done(world, {
        "op": "delete", "branch": "feature", "expectedOid": D})
    assert status == 200 and answer["status"] == "deleted"
    assert answer["branchDeleted"] is True and answer["cleanupPending"] == []
    sent = [item[2] for item in world.proxied if item[1] == "branch-delete"]
    assert all(item["discard"] is True for item in sent)
    assert all("feature" not in core.refs for core in world.cores.values())


def test_a_delete_the_first_core_already_did_still_reaches_the_others(world):
    world.cores["fmnext-n2"].refs["feature"] = D
    status, answer = core_op(world, {
        "op": "delete", "branch": "feature", "expectedOid": D})
    if status == 202:
        resume = {key: answer[key] for key in ("requestId", "node", "pinBase")}
        status, answer = core_op(world, {
            "op": "delete", "branch": "feature", "expectedOid": D}, resume)
    assert status == 200 and answer["status"] == "already"
    assert answer["cleanupPending"] == []
    assert all("feature" not in core.refs for core in world.cores.values())
    assert world.cores["fmnext-n2"].trash["feature"] == D


def test_a_branch_moved_since_the_page_read_it_is_left_alone(world):
    for core in world.cores.values():
        core.refs["feature"] = D
    status, answer = core_op_until_done(world, {
        "op": "merge", "branch": "feature", "base": "main",
        "expectedOid": B, "baseOid": A, "deleteBranch": False})
    assert status == 409 and answer["error"] == "stale_head"
    assert world.cores["fmnext-n1"].refs["main"] == A


def test_a_repository_the_cores_do_not_serve_falls_back_to_the_desktop(
        world, monkeypatch):
    async def unavailable(env, owner, repo):
        raise RuntimeError("private")

    monkeypatch.setattr(agent_branches, "_https_mirror_public_context",
                        unavailable)
    assert core_op(world, {"op": "delete", "branch": "feature",
                           "expectedOid": D}) is None
    assert core_op(world, {"op": "create", "branch": "feature",
                           "startOid": D}) is None
    assert world.proxied == []


def test_an_older_core_that_refuses_the_branch_falls_back(world):
    async def refuse(env, endpoint, context, request, mutation):
        world.proxied.append((endpoint["node"], mutation, dict(request)))
        return 400, None

    agent_branches._https_mirror_merge_proxy = refuse
    for core in world.cores.values():
        core.refs["feature"] = D
    assert core_op(world, {"op": "merge", "branch": "feature",
                           "base": "main", "expectedOid": D, "baseOid": A,
                           "deleteBranch": False}) is None


def test_a_runner_branch_is_updated_from_its_base(world):
    first = action(world, "update")
    assert first["status"] == 202
    assert world.record["update"]["state"] == "updating"
    done = action(world, "update")
    assert done["status"] == 200 and done["payload"]["status"] == "updated"
    assert done["payload"]["message"] == "Updated " + BRANCH + " from main."
    assert world.cores["fmnext-n1"].refs[BRANCH] == C
    assert world.record["update"]["state"] == "updated"
    assert world.record["diffStats"]["known"] is False


def test_a_conflicting_update_says_which_files(world):
    world.conflict = True
    action(world, "update")
    response = action(world, "update")
    assert response["status"] == 409
    assert response["payload"]["error"] == "merge_conflict"
    assert response["payload"]["conflicts"] == ["src/login.py"]
    assert world.record["merge"]["state"] == "conflict"


def test_branch_ops_try_the_cores_before_the_desktop_queue():
    text = (SRC / "route_handlers.py").read_text("utf-8")
    handler = text[text.index("async def repository_branch_ops_handler"):]
    handler = handler[:handler.index("\nPROFILE_CONTRIBUTION")]
    assert '"ops" not in data and data.get("queue") is not True' in handler
    assert "_branch_op_on_cores(" in handler
    assert handler.index("_branch_op_on_cores(") < handler.index(
        "INSERT INTO branch_op_inbox")
    helper = text[text.index("async def _branch_op_on_cores"):]
    helper = helper[:helper.index("\nasync def ")]
    assert "agent_branches.core_branch_op(" in helper
    assert '"source": "cores"' in helper
