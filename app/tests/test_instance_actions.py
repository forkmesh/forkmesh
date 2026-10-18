import asyncio
import hashlib
import json
from pathlib import Path
import sys

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(TESTS.parent / "src"))

import edge_routing  # noqa: E402
import runner_actions  # noqa: E402
import runner_leases  # noqa: E402
from test_runner_leases import (  # noqa: E402
    FakeRuntime, Key, NOW, call, decode_grant, signed)


COMMIT_A = "a" * 40
COMMIT_B = "b" * 40
COMMIT_C = "c" * 40


def run(coroutine):
    return asyncio.run(coroutine)


def setup(trusted=True):
    runtime = FakeRuntime()
    key = Key()
    runtime.pool = {"forkmesh"}
    runtime.gateways = {"forkmesh": "https://git.example.test"}
    runtime.add_runner("box-runner", key, owners=("forkmesh",))
    runtime.db.execute("UPDATE runner_nodes SET actions_trusted=?",
                       (1 if trusted else 0,))
    runtime.db.commit()
    runtime.settings["ACTIONS_INSTANCE_RUNNER"] = "box-runner"
    return runtime, key


def lease(runtime, key):
    path = "/api/runners/claim"
    body = {"max": 4, "providers": [], "actionsMax": 4}
    return call(signed(runtime, "box-runner", key, path, body),
                path)["data"]["jobs"]


def report(runtime, key, body):
    path = "/api/runners/actions-report"
    return call(signed(runtime, "box-runner", key, path, body), path)


def enqueue(runtime, branch="main", commit=COMMIT_A, **kwargs):
    return run(runner_actions.enqueue(
        runtime, "forkmesh", "site", branch, commit, "forkmesh", "jett",
        **kwargs))


def add_secret(runtime, owner, repo, name, value):
    runtime.db.execute(
        "INSERT INTO repo_action_secrets (owner,repo,name,value,updated_by,"
        "updated_at) VALUES (?,?,?,?,'jett',?)",
        (owner, repo, name, json.dumps({"value": value}), NOW))
    runtime.db.commit()


def column(runtime, job, name):
    return runtime.db.execute(
        "SELECT %s FROM repo_action_jobs WHERE id=?" % name,
        (job,)).fetchone()[0]


def add_catalog(runtime, path=".forkmesh/deploy.yml", events=None,
                inputs=None, schedules=None):
    run(runner_actions.store_catalog(
        runtime, "forkmesh", "site", runner_actions.clean_catalog([{
            "path": path, "name": "Deploy",
            "events": events or ["push", "workflow_dispatch"],
            "schedules": schedules or [], "inputs": inputs or {}}]),
        COMMIT_A, NOW))


def test_actions_push_grants_cover_exactly_one_branch():
    good = dict(node="forkmesh", owner="forkmesh", repository="site",
                access="actions-push", ref="refs/heads/release/v1",
                runner="box-runner", grant_id="grant-000000001",
                issued_at=1000, expires_at=2000)
    assert edge_routing.runner_grant_message(**good).split("\n")[4:6] == [
        "actions-push", "refs/heads/release/v1"]
    for ref in ("refs/heads/", "refs/tags/v1", "refs/heads/a..b",
                "refs/heads/a//b", "refs/heads/a@{1}", "refs/heads/x/",
                "refs/heads/x.lock", "refs/heads/x.", "refs/heads/-x",
                "main", "refs/heads/" + "a" * 201):
        assert edge_routing.runner_grant_message(**{**good, "ref": ref}) == ""
    assert edge_routing.runner_grant_message(
        **{**good, "expires_at": 1000 + 3 * 3600_000 + 1}) == ""


def test_push_jobs_coalesce_per_branch_and_other_events_get_their_own_rows():
    runtime, _key = setup()
    main = enqueue(runtime, before="1" * 40)
    assert enqueue(runtime) == 0
    assert enqueue(runtime, commit=COMMIT_B) == main
    assert column(runtime, main, "before_oid") == "1" * 40
    topic = enqueue(runtime, branch="topic", commit=COMMIT_C)
    assert topic not in (0, main)
    first = enqueue(runtime, commit="", event="schedule",
                    workflow=".forkmesh/nightly.yml",
                    payload={"schedule": "0 3 * * *"})
    second = enqueue(runtime, commit="", event="schedule",
                     workflow=".forkmesh/nightly.yml",
                     payload={"schedule": "0 3 * * *"})
    assert 0 not in (first, second) and first != second
    assert enqueue(runtime, event="nope") == 0
    assert enqueue(runtime, event="schedule", commit="") == 0
    assert enqueue(runtime, event="schedule", workflow="../x.yml") == 0
    runtime.db.execute("UPDATE repo_action_jobs SET status='completed',"
                       "pushed_commit=? WHERE id=?", ("9" * 40, main))
    assert enqueue(runtime, commit="9" * 40) == 0


def test_a_trusted_lease_carries_secrets_token_push_grant_and_bot():
    runtime, key = setup()
    add_secret(runtime, "forkmesh", "site", "TOKEN", "repo-value")
    add_secret(runtime, "forkmesh", "", "TOKEN", "org-value")
    add_secret(runtime, "forkmesh", "", "ONLY_ORG", "org-only")
    add_secret(runtime, "forkmesh", "other", "OTHER", "nope")
    job_id = enqueue(runtime, before="1" * 40)
    [job] = lease(runtime, key)
    assert job["jobId"] == job_id and job["event"] == "push"
    assert job["trusted"] is True
    assert job["secrets"] == {"TOKEN": "repo-value", "ONLY_ORG": "org-only"}
    assert job["eventPayload"] == {
        "ref": "refs/heads/main", "before": "1" * 40, "after": COMMIT_A,
        "repository": {"full_name": "forkmesh/site"},
        "pusher": {"name": "jett"}}
    assert job["pushRef"] == "refs/heads/main"
    grant = decode_grant(job["pushGrant"])
    assert grant[1:7] == ["forkmesh", "forkmesh", "site", "actions-push",
                          "refs/heads/main", "box-runner"]
    assert int(grant[9]) == job["grantExpiresAt"]
    assert column(runtime, job_id, "token_hash") == hashlib.sha256(
        job["jobToken"].encode()).hexdigest()
    assert job["apiUrl"] == "https://git.example.test"
    assert job["botName"] == "forkmesh-actions"
    assert job["botEmail"] == (
        "forkmesh-actions@users.noreply.git.example.test")


def test_an_untrusted_runner_or_pull_request_gets_no_secrets_or_push():
    runtime, key = setup(trusted=False)
    add_secret(runtime, "forkmesh", "site", "TOKEN", "repo-value")
    enqueue(runtime)
    [job] = lease(runtime, key)
    assert job["trusted"] is False
    for name in ("secrets", "jobToken", "pushGrant", "pushRef", "botName"):
        assert name not in job
    runtime, key = setup()
    enqueue(runtime, commit="", event="pull_request",
            workflow=".forkmesh/ci.yml")
    [job] = lease(runtime, key)
    assert job["trusted"] is False and "pushGrant" not in job


def test_one_active_job_per_workflow():
    runtime, key = setup()
    first = enqueue(runtime, commit="", event="workflow_dispatch",
                    workflow=".forkmesh/deploy.yml", inputs={"a": "1"},
                    payload={"inputs": {"a": "1"}})
    second = enqueue(runtime, commit="", event="workflow_dispatch",
                     workflow=".forkmesh/deploy.yml")
    other = enqueue(runtime, commit="", event="schedule",
                    workflow=".forkmesh/nightly.yml")
    jobs = lease(runtime, key)
    assert [job["jobId"] for job in jobs] == [first, other]
    assert jobs[0]["inputs"] == {"a": "1"}
    assert jobs[0]["workflow"] == ".forkmesh/deploy.yml"
    assert jobs[0]["eventPayload"]["repository"] == {
        "full_name": "forkmesh/site"}
    assert column(runtime, second, "status") == "queued"


def test_a_report_stores_catalog_masked_logs_and_the_pushed_commit():
    runtime, key = setup()
    add_secret(runtime, "forkmesh", "site", "TOKEN", "hunter2-secret")
    job_id = enqueue(runtime)
    [job] = lease(runtime, key)
    run_item = {
        "workflow": "Deploy", "workflowPath": ".forkmesh/deploy.yml",
        "commit": COMMIT_A, "ref": "refs/heads/main", "status": "running",
        "createdAt": NOW, "startedAt": NOW, "finishedAt": 0,
        "logTail": "tail hunter2-secret",
        "log": "start\nhunter2-secret\n" + job["jobToken"] + "\n"}
    catalog = [
        {"path": ".forkmesh/deploy.yml", "name": "Deploy",
         "events": ["push", "workflow_dispatch", "bogus"],
         "schedules": ["17 3 * * *", "* * * * *"],
         "inputs": {"project": {"type": "choice", "options": ["all", "x"],
                                "required": True, "default": "all"},
                    "bad name": {}}},
        {"path": "../escape.yml", "name": "x"}]
    body = {"jobId": job_id, "leaseId": job["leaseId"], "status": "running",
            "commit": COMMIT_A, "runs": [run_item], "catalog": catalog}
    assert report(runtime, key, body)["data"] == {"ok": True, "cancel": False}
    log = run(runner_actions.run_log(
        runtime, "forkmesh", "site", job_id * 100 + 1))
    assert log["log"] == "start\n***\n***\n"
    [workflow] = run(runner_actions.catalog(runtime, "forkmesh", "site"))
    assert workflow["events"] == ["push", "workflow_dispatch"]
    assert workflow["schedules"] == ["17 3 * * *"]
    assert [i["name"] for i in workflow["inputs"]] == ["project"]
    assert workflow["inputs"][0]["options"] == ["all", "x"]
    snapshot = run(runner_actions.snapshot(runtime, "forkmesh", "site"))
    assert snapshot["controlAvailable"] is True
    assert snapshot["workflows"] == [workflow]
    [shown] = snapshot["runs"]
    assert shown["event"] == "push" and shown["logTail"] == "tail ***"
    done = dict(body, status="completed", pushedCommit=COMMIT_B, catalog=None,
                runs=[dict(run_item, status="success", log=None)])
    assert report(runtime, key, done)["status"] == 200
    assert column(runtime, job_id, "pushed_commit") == COMMIT_B
    assert column(runtime, job_id, "token_hash") == ""
    assert run(runner_actions.run_log(
        runtime, "forkmesh", "site", job_id * 100 + 1))["log"] == (
            "start\n***\n***\n")
    assert run(runner_actions.run_log(
        runtime, "other", "site", job_id * 100 + 1)) is None
    assert enqueue(runtime, commit=COMMIT_B) == 0


def test_a_catalog_from_another_branch_is_not_stored():
    runtime, key = setup()
    job_id = enqueue(runtime, branch="topic")
    [job] = lease(runtime, key)
    report(runtime, key, {
        "jobId": job_id, "leaseId": job["leaseId"], "status": "completed",
        "runs": [], "catalog": [{"path": ".forkmesh/a.yml"}]})
    assert run(runner_actions.catalog(runtime, "forkmesh", "site")) == []


def test_cancel_queued_and_running_jobs():
    runtime, key = setup()
    queued = enqueue(runtime, commit="", event="schedule",
                     workflow=".forkmesh/a.yml")
    running = enqueue(runtime)
    run(runner_actions.cancel(runtime, "forkmesh", "site", queued * 100))
    assert column(runtime, queued, "status") == "cancelled"
    [job] = lease(runtime, key)
    assert job["jobId"] == running
    assert run(runner_actions.cancel(
        runtime, "forkmesh", "site", running * 100)) == ""
    body = {"jobId": running, "leaseId": job["leaseId"], "status": "running",
            "runs": []}
    assert report(runtime, key, body)["data"]["cancel"] is True
    assert column(runtime, running, "status") == "cancelling"
    report(runtime, key, dict(body, status="completed"))
    assert column(runtime, running, "status") == "cancelled"
    assert run(runner_actions.cancel(
        runtime, "forkmesh", "site", running * 100)) == "not_cancellable"
    assert run(runner_actions.cancel(
        runtime, "forkmesh", "site", 999900)) == "run_not_found"


def test_rerun_copies_the_job_for_one_workflow():
    runtime, key = setup()
    job_id = enqueue(runtime)
    assert run(runner_actions.rerun(
        runtime, "forkmesh", "site", job_id * 100 + 1, "ada")) == (
            0, "run_active")
    [job] = lease(runtime, key)
    report(runtime, key, {
        "jobId": job_id, "leaseId": job["leaseId"], "status": "completed",
        "runs": [{"workflow": "Deploy", "workflowPath": ".forkmesh/d.yml",
                  "commit": COMMIT_A, "ref": "refs/heads/main",
                  "status": "failed"}]})
    new_id, reason = run(runner_actions.rerun(
        runtime, "forkmesh", "site", job_id * 100 + 1, "ada"))
    assert reason == "" and new_id > job_id
    assert column(runtime, new_id, "workflow") == ".forkmesh/d.yml"
    assert column(runtime, new_id, "commit_oid") == COMMIT_A
    assert column(runtime, new_id, "actor") == "ada"


def test_dispatch_validates_inputs_against_the_catalog():
    runtime, _key = setup()
    add_catalog(runtime, inputs={
        "project": {"type": "choice", "options": ["all", "leaf"],
                    "required": True, "default": "all", "description": "",
                    "default_": ""},
        "dry": {"type": "boolean", "default": "false", "required": False},
        "note": {"type": "string", "required": True, "default": ""}})
    dispatch = lambda **kw: run(runner_actions.dispatch(  # noqa: E731
        runtime, "forkmesh", "site", kw.get("workflow", ".forkmesh/deploy.yml"),
        kw.get("ref", "refs/heads/main"), kw.get("inputs"), "ada"))
    assert dispatch(inputs={"note": "x", "project": "nope"})[1] == (
        "input_invalid")
    assert dispatch(inputs={})[1] == "input_required"
    assert dispatch(inputs={"note": "x", "extra": 1})[1] == "input_invalid"
    assert dispatch(inputs={"note": "x", "dry": "maybe"})[1] == (
        "input_invalid")
    assert dispatch(ref="main", inputs={"note": "x"})[1] == ""
    assert dispatch(workflow=".forkmesh/none.yml")[1] == "workflow_not_found"
    assert dispatch(ref="refs/heads/a..b")[1] == "ref_not_found"
    job, reason, _ = dispatch(inputs={"note": "hi", "dry": True})
    assert reason == "" and job
    assert column(runtime, job, "event") == "workflow_dispatch"
    assert column(runtime, job, "commit_oid") == ""
    payload = json.loads(column(runtime, job, "payload"))
    assert payload["inputs"] == {"project": "all", "dry": "true",
                                 "note": "hi"}
    assert payload["event"]["sender"] == {"login": "ada"}
    add_catalog(runtime, path=".forkmesh/push.yml", events=["push"])
    assert dispatch(workflow=".forkmesh/push.yml")[1] == "not_manual"


def test_enqueue_catalog_queues_every_workflow_with_the_event():
    runtime, _key = setup()
    run(runner_actions.store_catalog(runtime, "forkmesh", "site", [
        {"path": ".forkmesh/a.yml", "name": "", "events": ["issues"],
         "schedules": [], "inputs": {}},
        {"path": ".forkmesh/b.yml", "name": "", "events": ["push"],
         "schedules": [], "inputs": {}}], COMMIT_A, NOW))
    jobs = run(runner_actions.enqueue_catalog(
        runtime, "forkmesh", "site", "issues", "main",
        {"action": "opened"}, "ada"))
    assert len(jobs) == 1
    assert column(runtime, jobs[0], "workflow") == ".forkmesh/a.yml"


def test_snapshot_without_instance_runner_has_no_control():
    runtime, _key = setup()
    runtime.settings = {}
    enqueue(runtime, commit="", event="schedule", workflow=".forkmesh/a.yml")
    snapshot = run(runner_actions.snapshot(runtime, "forkmesh", "site"))
    assert snapshot["controlAvailable"] is False
    [placeholder] = snapshot["runs"]
    assert placeholder["workflow"] == ".forkmesh/a.yml"
    assert placeholder["event"] == "schedule" and placeholder["commit"] == ""


class ControlRequest:
    def __init__(self, method, body=None, actor="ada"):
        self.method = method
        self.body = body
        self.actor = actor

    async def text(self):
        return json.dumps(self.body)


def control_module(runtime, public=(), actions_off=False):
    import importlib
    from types import SimpleNamespace
    sys.modules.pop("repository_actions", None)
    module = importlib.import_module("repository_actions")
    audits = []

    async def session(_env, request, _data=None):
        return "", {"name": request.actor} if request.actor else None

    async def writer(_env, owner, repo, actor):
        return actor == "ada"

    async def no(*_args):
        return False

    async def audit(_env, actor, action, kind, target, outcome, details=None):
        audits.append((actor, action, outcome, details))

    async def private(_env, owner, repo):
        return (owner, repo.lower()) not in set(public)

    async def off(_env, owner, repo):
        return actions_off

    module._bind_runtime({
        "json_response": lambda data, status=200, **_: {
            "status": status, "data": data},
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "_account_session_record": session,
        "_account_owns_node": no,
        "_org_write_allowed": writer,
        "_audit_sensitive_action": audit,
        "_repo_is_private": private,
        "_branch_protection": SimpleNamespace(actions_off=off),
    })
    module._runner_runtime = lambda env: runtime
    return module, audits


def test_the_control_endpoint_dispatches_cancels_and_reruns_locally():
    runtime, key = setup()
    add_catalog(runtime, inputs=[{"name": "dry", "type": "boolean"}])
    module, audits = control_module(runtime)
    env = type("Env", (), {"ACTIONS_INSTANCE_RUNNER": "box-runner"})()

    def control(body, actor="ada"):
        return run(module.repository_actions_control_handler(
            env, ControlRequest("POST", body, actor), "forkmesh", "site"))

    answer = control({"action": "dispatch", "workflow": ".forkmesh/deploy.yml",
                      "ref": "main", "inputs": {"dry": True},
                      "sessionToken": "t"})
    assert answer["status"] == 202 and answer["data"]["runId"] > 0
    job = answer["data"]["runId"] // 100
    assert column(runtime, job, "ref") == "refs/heads/main"
    assert control({"action": "dispatch", "workflow": ".forkmesh/deploy.yml",
                    "ref": "main", "inputs": {"dry": "x"}})["data"][
                        "error"] == "input_invalid"
    assert control({"action": "cancel", "runId": job * 100},
                   actor="bob")["status"] == 403
    assert control({"action": "cancel", "runId": job * 100})["status"] == 202
    assert column(runtime, job, "status") == "cancelled"
    rerun = control({"action": "rerun", "runId": job * 100})
    assert rerun["status"] == 202 and rerun["data"]["runId"] > job * 100
    assert control({"action": "rerun", "runId": rerun["data"]["runId"]})[
        "data"]["error"] == "run_active"
    assert control({"action": "bogus"})["status"] == 400
    assert control({"action": "cancel", "runId": 1}, actor="")[
        "status"] == 401
    assert [a[2] for a in audits].count("success") == 3
    log = run(module.repository_actions_run_log_handler(
        env, ControlRequest("GET"), "forkmesh", "site", job * 100 + 1))
    assert log["status"] == 200 and log["data"]["log"] == ""
    assert run(module.repository_actions_run_log_handler(
        env, ControlRequest("GET", actor="bob"), "forkmesh", "site",
        job * 100 + 1))["status"] == 403


def org_setup():
    runtime = FakeRuntime()
    key = Key()
    runtime.pool = {"forkmesh"}
    runtime.gateways = {"forkmesh": "https://git.example.test"}
    runtime.add_runner("box-runner", key, owners=("acme-bolt",))
    runtime.db.executescript(
        "UPDATE runner_nodes SET actions_trusted=1;"
        "CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT);"
        "CREATE TABLE org_repos (org_bi TEXT, repo TEXT, node_owner TEXT);"
        "INSERT INTO orgs VALUES ('org-1', 'acme-bolt');"
        "INSERT INTO org_repos VALUES ('org-1', 'bolt', 'drew');"
        "INSERT INTO org_repos VALUES ('org-1', 'forkmesh', 'forkmesh');")
    runtime.db.commit()
    runtime.settings["ACTIONS_INSTANCE_RUNNER"] = "box-runner"
    return runtime, key


def test_identity_maps_an_organization_repository_both_ways():
    runtime, _key = org_setup()
    for owner in ("drew", "acme-bolt", "Acme-Bolt"):
        assert run(runner_actions.identity(runtime, owner, "Bolt")) == (
            "drew", "acme-bolt")
    assert run(runner_actions.identity(runtime, "acme-bolt", "forkmesh")) == (
        "forkmesh", "acme-bolt")
    assert run(runner_actions.identity(runtime, "alice", "Bolt")) == (
        "alice", "alice")
    assert run(runner_actions.identity(runtime, "drew", "other")) == (
        "drew", "drew")


def test_an_organization_repository_job_is_leased_as_the_organization():
    runtime, key = org_setup()
    add_secret(runtime, "acme-bolt", "", "CLOUDFLARE_API_TOKEN", "org-token")
    add_secret(runtime, "acme-bolt", "", "SHARED", "org-shared")
    add_secret(runtime, "acme-bolt", "Bolt", "SHARED", "alias-repo")
    add_secret(runtime, "drew", "Bolt", "OWN", "repo-own")
    add_secret(runtime, "drew", "", "PERSONAL", "nope")
    job_id = run(runner_actions.enqueue(
        runtime, "drew", "Bolt", "main", COMMIT_A, "forkmesh", "drew",
        before="1" * 40))
    assert column(runtime, job_id, "owner") == "drew"
    [job] = lease(runtime, key)
    assert job["jobId"] == job_id
    assert job["repositoryOwner"] == "acme-bolt"
    assert job["organization"] == "acme-bolt"
    assert job["cloneUrl"] == (
        "https://git.example.test/v1/runner/acme-bolt/Bolt.git")
    assert decode_grant(job["readGrant"])[1:5] == [
        "forkmesh", "acme-bolt", "Bolt", "read"]
    assert decode_grant(job["pushGrant"])[1:6] == [
        "forkmesh", "acme-bolt", "Bolt", "actions-push", "refs/heads/main"]
    assert job["secrets"] == {"CLOUDFLARE_API_TOKEN": "org-token",
                              "OWN": "repo-own", "SHARED": "alias-repo"}
    assert job["eventPayload"]["repository"] == {"full_name": "acme-bolt/Bolt"}
    assert job["botName"] == "acme-bolt-actions"
    body = {"jobId": job_id, "leaseId": job["leaseId"], "status": "completed",
            "commit": COMMIT_A,
            "runs": [{"workflow": "CI", "workflowPath": ".forkmesh/ci.yml",
                      "commit": COMMIT_A, "ref": "refs/heads/main",
                      "status": "success", "log": "org-token\n"}],
            "catalog": [{"path": ".forkmesh/ci.yml", "name": "CI",
                         "events": ["push", "workflow_dispatch"]}]}
    assert report(runtime, key, body)["status"] == 200
    assert run(runner_actions.run_log(
        runtime, "acme-bolt", "Bolt", job_id * 100 + 1))["log"] == "***\n"
    for owner in ("acme-bolt", "drew"):
        snapshot = run(runner_actions.snapshot(runtime, owner, "Bolt"))
        assert snapshot["source"] == "runner"
        assert snapshot["controlAvailable"] is True
        assert [w["path"] for w in snapshot["workflows"]] == [
            ".forkmesh/ci.yml"]
        assert [r["status"] for r in snapshot["runs"]] == ["success"]
    dispatched, reason, _ = run(runner_actions.dispatch(
        runtime, "acme-bolt", "Bolt", ".forkmesh/ci.yml", "main", {}, "ada"))
    assert reason == "" and column(runtime, dispatched, "owner") == "drew"
    assert run(runner_actions.cancel(
        runtime, "acme-bolt", "Bolt", dispatched * 100)) == ""
    assert column(runtime, dispatched, "status") == "cancelled"


def test_a_job_for_an_owner_the_runner_does_not_serve_is_not_leased():
    runtime, key = org_setup()
    run(runner_actions.enqueue(
        runtime, "alice", "site", "main", COMMIT_A, "forkmesh", "alice"))
    assert lease(runtime, key) == []


def test_a_failed_job_without_runs_shows_its_note_in_the_snapshot():
    runtime, _key = org_setup()
    job_id = run(runner_actions.enqueue(
        runtime, "drew", "Bolt", "main", COMMIT_A, "forkmesh", "jett"))
    runtime.db.execute(
        "UPDATE repo_action_jobs SET status='failed',note=?,updated_at=? "
        "WHERE id=?", ("This runner does not accept jobs for that owner.",
                       NOW + 5, job_id))
    runtime.db.commit()
    [shown] = run(runner_actions.snapshot(runtime, "acme-bolt", "Bolt"))["runs"]
    assert shown["status"] == "failed" and shown["finishedAt"] == NOW + 5
    assert shown["logTail"] == (
        "This runner does not accept jobs for that owner.\n")
    assert shown["ref"] == "refs/heads/main" and shown["commit"] == COMMIT_A


def test_the_instance_status_endpoint_answers_for_the_organization_address():
    runtime, _key = org_setup()
    module, audits = control_module(runtime)
    asked = []

    async def writer(_env, owner, repo, actor):
        asked.append(owner)
        return owner == "drew" and actor == "ada"

    module._org_write_allowed = writer
    env = type("Env", (), {"ACTIONS_INSTANCE_RUNNER": "box-runner"})()

    def status(owner, actor="ada"):
        return run(module.repository_actions_instance_status_handler(
            env, ControlRequest("GET", actor=actor), owner, "Bolt"))

    empty = status("acme-bolt")
    assert empty["status"] == 200
    assert empty["data"]["source"] == "runner"
    assert empty["data"]["controlAvailable"] is True
    assert empty["data"]["runs"] == [] and empty["data"]["workflows"] == []
    job_id = run(runner_actions.enqueue(
        runtime, "acme-bolt", "Bolt", "main", COMMIT_A, "forkmesh", "ada"))
    assert column(runtime, job_id, "owner") == "drew"
    for owner in ("acme-bolt", "drew"):
        answer = status(owner)
        assert answer["status"] == 200
        assert [r["id"] for r in answer["data"]["runs"]] == [job_id * 100]
    assert set(asked) == {"drew"}
    assert status("acme-bolt", actor="bob")["status"] == 403
    assert status("acme-bolt", actor="")["status"] == 401
    assert [a[2] for a in audits] == ["success"] * 3 + ["denied"]


def public_setup():
    runtime = FakeRuntime()
    key = Key()
    runtime.pool = {"forkmesh"}
    runtime.gateways = {"forkmesh": "https://git.example.test"}
    runtime.add_runner("box-runner", key, owners=("acme",))
    runtime.db.executescript(
        "UPDATE runner_nodes SET actions_trusted=1;"
        "CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT);"
        "CREATE TABLE org_repos (org_bi TEXT, repo TEXT, node_owner TEXT);"
        "INSERT INTO orgs VALUES ('org-1', 'acme');"
        "INSERT INTO org_repos VALUES ('org-1', 'web', 'builder');")
    runtime.db.commit()
    runtime.settings["ACTIONS_INSTANCE_RUNNER"] = "box-runner"
    add_secret(runtime, "acme", "", "DEPLOY_TOKEN", "deploy-secret-value")
    job_id = run(runner_actions.enqueue(
        runtime, "acme", "web", "main", COMMIT_A, "forkmesh", "ada"))
    [job] = lease(runtime, key)
    body = {"jobId": job_id, "leaseId": job["leaseId"], "status": "running",
            "commit": COMMIT_A,
            "runs": [{"workflow": "CI", "workflowPath": ".forkmesh/ci.yml",
                      "commit": COMMIT_A, "ref": "refs/heads/main",
                      "status": "running", "createdAt": NOW,
                      "startedAt": NOW, "finishedAt": 0,
                      "logTail": "using deploy-secret-value",
                      "log": "step 1\ndeploy-secret-value\n"
                             + job["jobToken"] + "\n"}],
            "catalog": [{"path": ".forkmesh/ci.yml", "name": "CI",
                         "events": ["push", "workflow_dispatch"],
                         "inputs": [{"name": "target", "type": "string",
                                     "default": "staging"}]}]}
    assert report(runtime, key, body)["status"] == 200
    return runtime, job_id, job


def status_module(runtime, public=()):
    module, audits = control_module(runtime, public)
    module._ACTIONS_PUBLIC_MEMO.clear()
    responses = []

    def respond(data, status=200, cache_control=None, extra_headers=None):
        responses.append((cache_control, extra_headers))
        return {"status": status, "data": data, "cache": cache_control,
                "headers": extra_headers or {}}

    module.json_response = respond
    env = type("Env", (), {"ACTIONS_INSTANCE_RUNNER": "box-runner"})()

    def status(owner="acme", actor=""):
        return run(module.repository_actions_instance_status_handler(
            env, ControlRequest("GET", actor=actor), owner, "web"))

    def log(run_id, actor=""):
        return run(module.repository_actions_run_log_handler(
            env, ControlRequest("GET", actor=actor), "acme", "web", run_id))

    return module, audits, status, log


def test_a_public_repository_shows_a_run_summary_to_anyone():
    runtime, job_id, job = public_setup()
    module, audits, status, log = status_module(
        runtime, public={("builder", "web")})
    for actor in ("", "bob"):
        answer = status(actor=actor)
        assert answer["status"] == 200
        assert answer["cache"].startswith("public, max-age=5")
        assert answer["headers"]["Vary"] == "Cookie, Authorization"
        data = answer["data"]
        assert data["access"] == "public" and data["source"] == "runner"
        assert data["controlAvailable"] is False and data["node"] == ""
        [shown] = data["runs"]
        assert shown == {
            "id": job_id * 100 + 1, "workflow": "CI",
            "workflowPath": ".forkmesh/ci.yml", "event": "push",
            "commit": COMMIT_A, "ref": "refs/heads/main",
            "status": "running", "createdAt": NOW, "startedAt": NOW,
            "finishedAt": 0, "actor": "ada"}
        assert data["workflows"] == [{"path": ".forkmesh/ci.yml",
                                      "name": "CI",
                                      "events": ["push", "workflow_dispatch"]}]
        text = json.dumps(data)
        for secret in ("deploy-secret-value", job["jobToken"], "box-runner",
                       "logTail", "inputs", "staging", "controls"):
            assert secret not in text
    assert audits == []
    assert log(job_id * 100 + 1)["status"] == 401
    assert log(job_id * 100 + 1, actor="bob")["status"] == 403


def test_the_public_summary_is_cached_briefly():
    runtime, job_id, _job = public_setup()
    _module, _audits, status, _log = status_module(
        runtime, public={("builder", "web")})
    assert len(status()["data"]["runs"]) == 1
    second = run(runner_actions.enqueue(
        runtime, "acme", "web", "main", COMMIT_B, "forkmesh", "ada"))
    assert len(status()["data"]["runs"]) == 1
    runtime.clock += 6000
    assert [r["id"] for r in status()["data"]["runs"]] == [
        second * 100, job_id * 100 + 1]


def test_writers_get_the_full_answer_with_masked_logs():
    runtime, job_id, job = public_setup()
    _module, audits, status, log = status_module(
        runtime, public={("builder", "web")})
    answer = status(actor="ada")
    assert answer["status"] == 200
    assert answer["cache"].startswith("private, no-store")
    data = answer["data"]
    assert data["access"] == "member" and data["controlAvailable"] is True
    assert data["node"] == "box-runner"
    [shown] = data["runs"]
    assert shown["logTail"] == "using ***" and shown["actor"] == "ada"
    assert data["workflows"][0]["inputs"][0]["name"] == "target"
    full = log(job_id * 100 + 1, actor="ada")
    assert full["status"] == 200
    assert full["data"]["log"] == "step 1\n***\n***\n"
    assert [a[2] for a in audits] == ["success"]


def test_logs_are_masked_with_secrets_added_after_the_report():
    runtime, job_id, _job = public_setup()
    _module, _audits, status, log = status_module(runtime)
    add_secret(runtime, "builder", "web", "LATE", "step 1")
    assert log(job_id * 100 + 1, actor="ada")["data"]["log"].startswith(
        "***\n")


def test_a_private_repository_shows_nothing_to_outsiders():
    runtime, job_id, _job = public_setup()
    _module, audits, status, log = status_module(runtime)
    anonymous = status()
    assert anonymous["status"] == 401 and "runs" not in anonymous["data"]
    outsider = status(actor="bob")
    assert outsider["status"] == 403 and "runs" not in outsider["data"]
    assert [a[2] for a in audits] == ["denied"]
    assert status(actor="ada")["data"]["access"] == "member"
    assert log(job_id * 100 + 1)["status"] == 401


def test_the_instance_runner_is_shown_as_this_server():
    runtime, _key = setup()
    runtime.runner_app_url = lambda: "https://code.example.org"
    job_id = enqueue(runtime)
    runtime.db.execute(
        "UPDATE repo_action_jobs SET status='completed',runner='box-runner',"
        "updated_at=? WHERE id=?", (NOW + 5, job_id))
    runtime.db.commit()
    full = run(runner_actions.snapshot(runtime, "forkmesh", "site"))
    assert full["local"] is True and full["host"] == "code.example.org"
    summary = runner_actions.public_summary(full)
    assert summary["local"] is True and summary["host"] == "code.example.org"
    assert summary["node"] == ""


def test_another_runner_is_not_shown_as_this_server():
    runtime, _key = setup()
    runtime.runner_app_url = lambda: "https://code.example.org"
    job_id = enqueue(runtime)
    runtime.db.execute(
        "UPDATE repo_action_jobs SET status='completed',runner='elsewhere',"
        "updated_at=? WHERE id=?", (NOW + 5, job_id))
    runtime.db.commit()
    full = run(runner_actions.snapshot(runtime, "forkmesh", "site"))
    assert full["local"] is False and full["host"] == ""
    assert runner_actions.public_summary(full)["host"] == ""


def test_a_finished_trusted_job_admits_the_state_its_push_left():
    runtime, key = setup()
    admitted = []

    async def admit(owner, repo, node):
        admitted.append((owner, repo, node))
        return "e" * 64

    runtime.admit_push_state = admit
    job_id = enqueue(runtime)
    [job] = lease(runtime, key)
    body = {"jobId": job_id, "leaseId": job["leaseId"], "status": "running",
            "runs": []}
    assert report(runtime, key, body)["status"] == 200
    assert admitted == []
    assert report(runtime, key, dict(body, status="completed"))[
        "status"] == 200
    assert admitted == [("forkmesh", "site", "forkmesh")]


def test_an_untrusted_job_admits_nothing_and_a_failed_admission_is_ignored():
    runtime, key = setup(trusted=False)
    admitted = []

    async def admit(owner, repo, node):
        admitted.append(node)
        return ""

    runtime.admit_push_state = admit
    job_id = enqueue(runtime)
    [job] = lease(runtime, key)
    report(runtime, key, {"jobId": job_id, "leaseId": job["leaseId"],
                          "status": "completed", "runs": []})
    assert admitted == []
    runtime, key = setup()

    async def broken(owner, repo, node):
        raise RuntimeError("proof unavailable")

    runtime.admit_push_state = broken
    job_id = enqueue(runtime)
    [job] = lease(runtime, key)
    assert report(runtime, key, {
        "jobId": job_id, "leaseId": job["leaseId"], "status": "completed",
        "runs": []})["data"] == {"ok": True, "cancel": False}
    assert column(runtime, job_id, "status") == "completed"


def test_the_worker_admits_the_signed_state_of_the_job_target_node(
        monkeypatch):
    env = object()
    calls = []
    endpoints = [{"node": "mirror-a"}, {"node": "forkmesh"}]

    async def context_of(given, owner, repo):
        assert given is env
        return {"owner": owner, "repo": repo, "repoBi": "bi"} if (
            repo == "site") else None

    async def candidates(given, context, region):
        return endpoints

    async def admit(given, owner, repo, endpoint):
        calls.append((owner, repo, endpoint["node"]))
        return "e" * 64

    for name, value in (("_https_mirror_public_context", context_of),
                        ("_https_mirror_request_candidates", candidates),
                        ("_admit_core_push_state", admit)):
        monkeypatch.setattr(runner_leases, name, value, raising=False)
    worker = runner_leases.WorkerRuntime(env, None)
    assert run(worker.admit_push_state("forkmesh", "site", "ForkMesh")) == (
        "e" * 64)
    assert calls == [("forkmesh", "site", "forkmesh")]
    assert run(worker.admit_push_state("forkmesh", "site", "absent")) == ""
    assert run(worker.admit_push_state("forkmesh", "gone", "forkmesh")) == ""
    assert calls == [("forkmesh", "site", "forkmesh")]


def test_a_trusted_job_marks_its_target_node_as_pushing_until_it_ends():
    runtime, key = setup()
    pushing = runner_actions.pushing
    assert run(pushing(runtime, "forkmesh", "site", "forkmesh")) is False
    job_id = enqueue(runtime)
    assert run(pushing(runtime, "forkmesh", "site", "forkmesh")) is False
    [job] = lease(runtime, key)
    assert run(pushing(runtime, "forkmesh", "site", "forkmesh")) is True
    assert run(pushing(runtime, "forkmesh", "Site", "ForkMesh")) is True
    assert run(pushing(runtime, "forkmesh", "site", "mirror-a")) is False
    assert run(pushing(runtime, "forkmesh", "other", "forkmesh")) is False
    assert run(pushing(runtime, "someone", "site", "forkmesh")) is False
    report(runtime, key, {"jobId": job_id, "leaseId": job["leaseId"],
                          "status": "completed", "runs": []})
    assert run(pushing(runtime, "forkmesh", "site", "forkmesh")) is False


def test_an_untrusted_or_expired_job_is_not_pushing():
    runtime, key = setup(trusted=False)
    enqueue(runtime)
    lease(runtime, key)
    assert run(runner_actions.pushing(
        runtime, "forkmesh", "site", "forkmesh")) is False
    runtime, key = setup()
    enqueue(runtime)
    lease(runtime, key)
    runtime.clock += runner_actions.JOB_TIMEOUT_MS + 1
    assert run(runner_actions.pushing(
        runtime, "forkmesh", "site", "forkmesh")) is False
def test_a_repository_with_actions_off_queues_and_leases_nothing():
    runtime, key = setup()
    queued = enqueue(runtime)
    off = {"value": True}

    async def actions_off(owner, repo):
        return off["value"] and (owner, repo) == ("forkmesh", "site")
    runtime.actions_off = actions_off
    assert enqueue(runtime, commit=COMMIT_B) == 0
    assert enqueue(runtime, branch="topic", commit=COMMIT_C) == 0
    assert lease(runtime, key) == []
    assert column(runtime, queued, "status") == "cancelled"
    off["value"] = False
    assert enqueue(runtime, branch="topic", commit=COMMIT_C) != 0


def test_a_protected_branch_gets_no_actions_push_grant():
    runtime, key = setup()

    async def branch_protected(owner, repo, branch):
        return branch == "main"
    runtime.branch_protected = branch_protected
    enqueue(runtime)
    [job] = lease(runtime, key)
    assert job["trusted"] is True and job["jobToken"]
    assert "pushGrant" not in job and "pushRef" not in job
    runtime.db.execute("UPDATE repo_action_jobs SET status='completed'")
    enqueue(runtime, branch="topic", commit=COMMIT_C)
    [job] = lease(runtime, key)
    assert job["pushRef"] == "refs/heads/topic"


def test_the_control_endpoint_refuses_runs_when_actions_are_off():
    runtime, _key = setup()
    add_catalog(runtime)
    module, _audits = control_module(runtime, actions_off=True)
    env = type("Env", (), {"ACTIONS_INSTANCE_RUNNER": "box-runner"})()
    answer = run(module.repository_actions_control_handler(
        env, ControlRequest("POST", {
            "action": "dispatch", "workflow": ".forkmesh/deploy.yml",
            "ref": "main", "sessionToken": "t"}, "ada"), "forkmesh", "site"))
    assert answer["status"] == 403
    assert answer["data"]["error"] == "actions_disabled"


def test_the_instance_runner_log_header_names_this_server():
    runtime, _key = setup()
    runtime.runner_app_url = lambda: "https://code.example.org"
    header = "==> ci at 0123456789ab on main (runner box-runner; event push)\n"
    assert runner_actions._show_runner(runtime, header) == (
        "==> ci at 0123456789ab on main (runner code.example.org; event push)\n")
    other = "==> ci at 0123456789ab on main (runner elsewhere; event push)\n"
    assert runner_actions._show_runner(runtime, other) == other
    runtime.runner_app_url = lambda: ""
    assert runner_actions._show_runner(runtime, header) == header
