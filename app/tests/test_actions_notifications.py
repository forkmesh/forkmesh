from pathlib import Path
import sys

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(TESTS.parent / "src"))

import runner_actions  # noqa: E402
import runner_leases  # noqa: E402
from test_instance_actions import (  # noqa: E402
    COMMIT_A, add_catalog, enqueue, lease, report, run, setup)
from test_pull_request_actions import (  # noqa: E402
    HEAD_1, HEAD_2, checks, sync, world)
from test_runner_leases import NOW  # noqa: E402


ROOT = TESTS.parent
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")


def capture(runtime):
    sent = []

    async def notify_actions(kind, owner, repo, title, body, href, dedupe,
                             meta):
        sent.append({"kind": kind, "owner": owner, "repo": repo,
                     "title": title, "body": body, "href": href,
                     "dedupe": dedupe, "meta": meta})
        return 1

    runtime.notify_actions = notify_actions
    return sent


def test_an_outside_authors_pull_request_asks_maintainers_once_per_head():
    runtime, _key, repo, _audit = world()
    sent = capture(runtime)
    repo.add(7, HEAD_1, author="bob")
    sync(runtime)
    [ping] = sent
    job = runtime.db.execute(
        "SELECT id FROM repo_action_jobs WHERE pull_number=7").fetchone()[0]
    assert ping["kind"] == "action_approval"
    assert (ping["owner"], ping["repo"]) == ("forkmesh", "site")
    assert ping["title"] == "Checks need your approval"
    assert ping["href"] == "/forkmesh/site/pull/7/checks"
    assert ping["dedupe"] == "actions-approval:forkmesh/site:7:" + HEAD_1
    assert ping["meta"] == {
        "category": "actions-approval", "number": 7, "head": HEAD_1,
        "runId": job * runner_actions.RUN_ID_STRIDE, "reason": "author",
        "author": "bob"}
    assert "ci.yml" in ping["body"]
    assert "bob is not trusted" in ping["body"]

    sync(runtime)
    assert len(sent) == 1

    repo.add(7, HEAD_2, author="bob")
    sync(runtime)
    assert len(sent) == 2
    assert sent[1]["meta"]["head"] == HEAD_2
    assert sent[1]["dedupe"].endswith(":7:" + HEAD_2)


def test_a_writers_pull_request_asks_only_when_it_changes_workflows():
    runtime, key, repo, _audit = world()
    sent = capture(runtime)
    repo.add(7, HEAD_1, author="alice")
    sync(runtime)
    assert sent == []
    [job] = lease(runtime, key)
    report(runtime, key, {
        "jobId": job["jobId"], "leaseId": job["leaseId"],
        "status": "completed", "commit": HEAD_1, "runs": [],
        "approvalRequired": "workflows"})
    [ping] = sent
    assert ping["meta"]["reason"] == "workflows"
    assert ping["meta"]["head"] == HEAD_1 and ping["meta"]["number"] == 7
    assert ping["meta"]["runId"] == job["jobId"] * 100
    assert "changes files under .forkmesh/" in ping["body"]
    assert ping["dedupe"] == "actions-approval:forkmesh/site:7:" + HEAD_1


def test_approving_from_the_phone_route_runs_the_head_the_ping_named():
    runtime, key, repo, _audit = world()
    sent = capture(runtime)
    repo.add(7, HEAD_1, author="bob")
    sync(runtime)
    meta = sent[0]["meta"]
    status, data = checks(runtime, viewer="alice", method="POST",
                          body={"action": "approve", "head": meta["head"]})
    assert status == 200 and data["approved"] == 1
    [job] = lease(runtime, key)
    assert job["jobId"] * 100 == meta["runId"]


def _run(status, workflow="Deploy", log_tail=""):
    return {"workflow": workflow, "workflowPath": ".forkmesh/deploy.yml",
            "commit": COMMIT_A, "ref": "refs/heads/main", "status": status,
            "createdAt": NOW, "startedAt": NOW, "finishedAt": NOW,
            "logTail": log_tail}


def test_a_failed_run_pings_the_maintainers_with_the_run_to_open():
    runtime, key = setup()
    add_catalog(runtime)
    sent = capture(runtime)
    job_id = enqueue(runtime)
    [job] = lease(runtime, key)
    body = {"jobId": job_id, "leaseId": job["leaseId"], "status": "running",
            "commit": COMMIT_A, "runs": [_run("running")]}
    report(runtime, key, body)
    assert sent == []
    report(runtime, key, dict(body, status="failed", runs=[
        _run("success", "Lint"), _run("failed", "Deploy")]))
    [ping] = sent
    assert ping["kind"] == "action_failed"
    assert ping["title"] == "Deploy failed"
    assert ping["body"] == "Failed on main at aaaaaaa."
    assert ping["href"] == "/forkmesh/site/actions/runs/%d" % (
        job_id * 100 + 2)
    assert ping["dedupe"] == "actions-failed:forkmesh/site:%d" % job_id
    assert ping["meta"] == {"category": "ci-failed",
                            "runId": job_id * 100 + 2, "workflow": "Deploy"}


def test_a_passing_or_cancelled_run_pings_nobody():
    runtime, key = setup()
    add_catalog(runtime)
    sent = capture(runtime)
    job_id = enqueue(runtime)
    [job] = lease(runtime, key)
    report(runtime, key, {
        "jobId": job_id, "leaseId": job["leaseId"], "status": "completed",
        "commit": COMMIT_A, "runs": [_run("success")]})
    assert sent == []

    runtime, key = setup()
    add_catalog(runtime)
    sent = capture(runtime)
    other = enqueue(runtime)
    [job] = lease(runtime, key)
    run(runner_actions.cancel(runtime, "forkmesh", "site", other * 100))
    report(runtime, key, {
        "jobId": other, "leaseId": job["leaseId"], "status": "failed",
        "commit": COMMIT_A, "runs": [_run("failed")]})
    assert sent == []


def test_a_failed_pull_request_run_names_the_pull_request():
    runtime, key, repo, _audit = world()
    sent = capture(runtime)
    repo.add(7, HEAD_1, author="alice")
    sync(runtime)
    [job] = lease(runtime, key)
    failing = dict(_run("failed", "CI"), commit=HEAD_1,
                   ref="refs/pr/7/head")
    report(runtime, key, {
        "jobId": job["jobId"], "leaseId": job["leaseId"], "status": "failed",
        "commit": HEAD_1, "runs": [failing]})
    [ping] = sent
    assert ping["body"] == "Failed on pull request #7 at 1111111."
    assert ping["meta"]["number"] == 7


def test_notifications_without_the_hook_or_with_a_failing_hook_are_skipped():
    runtime, key = setup()
    add_catalog(runtime)

    async def broken(*_args):
        raise RuntimeError("relay down")

    runtime.notify_actions = broken
    job_id = enqueue(runtime)
    [job] = lease(runtime, key)
    answer = report(runtime, key, {
        "jobId": job_id, "leaseId": job["leaseId"], "status": "failed",
        "commit": COMMIT_A, "runs": [_run("failed")]})
    assert answer["data"] == {"ok": True, "cancel": False}
    assert run(runner_actions._notify(
        object(), "action_failed", "a", "b", "t", "", "/", "d", {})) == 0


def test_the_worker_runtime_fans_out_to_the_repository_maintainers():
    calls = []

    async def fanout(env, owner, repo):
        calls.append(("fanout", owner, repo))
        return ["forkmesh", "alice"]

    async def enqueue_notification(env, recipient, kind, title, **kwargs):
        calls.append((recipient, kind, title, kwargs))
        return recipient != "alice"

    class _Date:
        @staticmethod
        def now():
            return NOW

    namespace = vars(runner_leases)
    saved = {name: namespace.get(name) for name in (
        "_org_fanout_recipients", "enqueue_notification", "Date")}
    namespace.update({"_org_fanout_recipients": fanout,
                      "enqueue_notification": enqueue_notification,
                      "Date": _Date})
    try:
        pings = run(runner_leases.WorkerRuntime(object(), None).notify_actions(
            "action_failed", "forkmesh", "site", "Deploy failed",
            "Failed on main.", "/forkmesh/site/actions/runs/101",
            "actions-failed:forkmesh/site:1", {"runId": 101}))
    finally:
        for name, value in saved.items():
            if value is None:
                namespace.pop(name, None)
            else:
                namespace[name] = value
    assert pings == 1
    assert calls[0] == ("fanout", "forkmesh", "site")
    recipient, kind, title, kwargs = calls[1]
    assert (recipient, kind, title) == ("forkmesh", "action_failed",
                                        "Deploy failed")
    assert kwargs == {"body": "Failed on main.", "repo": "forkmesh/site",
                      "href": "/forkmesh/site/actions/runs/101",
                      "source": "actions",
                      "dedupe": "actions-failed:forkmesh/site:1",
                      "ts": NOW, "meta": {"runId": 101}}
    assert calls[2][0] == "alice"


def test_kinds_are_registered_and_the_banner_carries_what_the_phone_needs():
    assert '    "action_approval",\n    "action_failed",\n' in ENTRY
    source = ENTRY.split("async def _apns_ping_alert(")[1].split(
        "\nasync def ")[0]
    assert '("action_approval", "action_failed")' in source
    for field in ("category", "runId", "number", "head", "repo"):
        assert 'alert["%s"]' % field in source
