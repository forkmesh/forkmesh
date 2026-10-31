import asyncio
import base64
import json
from pathlib import Path
import sys
from urllib.parse import parse_qs, urlparse

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(TESTS.parent / "src"))

import pull_actions  # noqa: E402
import runner_actions  # noqa: E402
from test_instance_actions import (  # noqa: E402
    add_catalog, add_secret, column, lease, report, run, setup)
from test_runner_leases import NOW  # noqa: E402


HEAD_1 = "1" * 40
HEAD_2 = "2" * 40
BASE = "b" * 40
WRITERS = {"alice"}
KEYS = {"key-alice": "alice", "key-bob": "bob", "key-carol": "carol"}


class Response:
    def __init__(self, data, status=200):
        self.data, self.status = data, status

    async def text(self):
        return json.dumps(self.data)


class Request:
    def __init__(self, method="GET", body=None, url=""):
        self.method, self.body, self.url = method, body, url

    async def text(self):
        return json.dumps(self.body or {})


class Repo:
    def __init__(self):
        self.pulls = {}
        self.reads = []

    def add(self, number, head, author="bob", key=None, status="open",
            title="Fix things"):
        self.pulls[number] = (
            "---\nschema: forkmesh-pull-v1\nnumber: %d\ntitle: %s\n"
            "base: main\nhead: pr/%s/x\nstatus: %s\nauthor: %s\n"
            "authorName: %s\nheadOid: %s\n---\n\nbody\n" % (
                number, title, author, status, key or "key-" + author,
                author, head))

    async def read(self, _env, url, operation, bypass_cache=False):
        parsed = urlparse(url)
        query = parse_qs(parsed.query)
        self.reads.append((operation, query))
        assert bypass_cache and parsed.path == "/api/repo/forkmesh/site/" + (
            operation)
        if operation == "tree" and query.get("path") == [".forkmesh/pulls"]:
            return Response({"commit": "f" * 40, "entries": [
                {"name": str(n), "type": "tree"} for n in self.pulls]})
        if operation == "tree":
            assert query["ref"] == ["main"]
            return Response({"commit": BASE, "entries": []})
        blobs = {}
        for path in query["path"]:
            number = int(path.split("/")[2])
            blobs[path] = {"ok": True, "encoding": "base64",
                           "content": base64.b64encode(
                               self.pulls[number].encode()).decode()}
        return Response({"blobs": blobs})


def bind(runtime, repo, audit, viewer=""):
    db = runtime.db

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def owns(_env, actor, node):
        return actor == node

    async def org_write(_env, _owner, _repo, account):
        return account in WRITERS

    async def account_for(_env, key):
        return KEYS.get(key, "")

    async def audit_action(_env, actor, action, _type, target, outcome,
                           details):
        audit.append((actor, action, target, outcome, details))

    async def session(_env, _request, _data):
        return None, ({"name": viewer} if viewer else None)

    async def nothing(*_args, **_kwargs):
        return False

    def json_response(data, status=200, cache_control=""):
        return Response(data, status)

    namespace = vars(pull_actions)
    namespace.update({
        "d1_first": d1_first, "d1_all": d1_all, "d1_run": d1_run,
        "_account_owns_node": owns, "_org_write_allowed": org_write,
        "_git_push_account_for_pubkey": account_for,
        "_audit_sensitive_action": audit_action,
        "_account_session_record": session,
        "_routed_repository_read": repo.read,
        "_repo_is_private": nothing, "ensure_schema": nothing,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "_runtime": lambda _env: runtime,
    })
    pull_actions._SYNC_MEMO.clear()


def world(viewer=""):
    runtime, key = setup()
    runtime.db.executescript(
        "CREATE TABLE IF NOT EXISTS repo_action_trusted_authors ("
        "owner TEXT NOT NULL, repo TEXT NOT NULL, account TEXT NOT NULL, "
        "added_by TEXT NOT NULL DEFAULT '', added_at INTEGER NOT NULL, "
        "PRIMARY KEY (owner, repo, account))")
    add_catalog(runtime, ".forkmesh/ci.yml", ["push", "pull_request"])
    add_secret(runtime, "forkmesh", "site", "DEPLOY_TOKEN", "s3cret-value")
    repo, audit = Repo(), []
    bind(runtime, repo, audit, viewer)
    return runtime, key, repo, audit


def sync(runtime):
    return run(pull_actions.sync_repo(object(), "forkmesh", "site",
                                      force=True))


def statuses(runtime):
    return [tuple(row) for row in runtime.db.execute(
        "SELECT pull_number,commit_oid,status,approval FROM repo_action_jobs "
        "WHERE event='pull_request' ORDER BY id")]


def checks(runtime, viewer="", method="GET", body=None, number=7):
    pull_actions._account_session_record = None
    namespace = vars(pull_actions)

    async def session(_env, _request, _data):
        return None, ({"name": viewer} if viewer else None)

    namespace["_account_session_record"] = session
    env = type("Env", (), {"ACTIONS_INSTANCE_RUNNER": "box-runner"})()
    response = run(pull_actions.repo_pull_checks_handler(
        env, Request(method, body), "forkmesh", "site", number))
    return response.status, response.data


def assert_untrusted_lease(job):
    assert job["trusted"] is False
    for name in ("secrets", "jobToken", "pushGrant", "pushRef", "botName",
                 "botEmail", "apiUrl"):
        assert name not in job
    assert "s3cret-value" not in json.dumps(job)


def test_an_outside_author_waits_for_approval_and_never_gets_secrets():
    runtime, key, repo, audit = world()
    repo.add(7, HEAD_1, author="bob")
    assert sync(runtime) == 1
    assert statuses(runtime) == [(7, HEAD_1, "waiting", "author")]
    assert lease(runtime, key) == []
    status, data = checks(runtime, viewer="bob")
    assert status == 200 and data["approval"]["required"] is True
    assert data["approval"]["reason"] == "author"
    assert data["canApprove"] is False and data["head"] == HEAD_1
    assert [r["status"] for r in data["runs"]] == ["awaiting-approval"]
    status, _ = checks(runtime, viewer="bob", method="POST",
                       body={"action": "approve", "head": HEAD_1})
    assert status == 403
    assert audit[-1][1:4] == ("actions.pull_approve", "forkmesh/site#7",
                              "denied")
    assert checks(runtime, method="POST",
                  body={"action": "approve", "head": HEAD_1})[0] == 401
    status, data = checks(runtime, viewer="alice", method="POST",
                          body={"action": "approve", "head": HEAD_2})
    assert status == 409 and data["error"] == "stale_head"
    status, data = checks(runtime, viewer="alice", method="POST",
                          body={"action": "approve", "head": HEAD_1})
    assert status == 200 and data["approved"] == 1
    assert audit[-1][:4] == ("alice", "actions.pull_approve",
                             "forkmesh/site#7", "success")
    assert audit[-1][4]["head"] == HEAD_1
    [job] = lease(runtime, key)
    assert_untrusted_lease(job)
    assert job["event"] == "pull_request" and job["commit"] == HEAD_1
    assert job["workflow"] == ".forkmesh/ci.yml"
    assert job["pullNumber"] == 7 and job["pullRef"] == "refs/pr/7/head"
    assert job["baseRef"] == "main" and job["baseCommit"] == BASE
    assert job["before"] == BASE and job["branch"] == "pull/7"
    assert job["gateWorkflowChanges"] is False
    assert job["eventPayload"]["pull_request"]["head"]["sha"] == HEAD_1
    assert job["eventPayload"]["sender"] == {"login": "bob"}
    assert column(runtime, job["jobId"], "token_hash") == ""
    assert column(runtime, job["jobId"], "approved_by") == "alice"


def test_a_stored_trusted_flag_still_leases_a_pull_request_without_secrets():
    runtime, key, repo, _audit = world()
    repo.add(7, HEAD_1, author="alice")
    sync(runtime)
    runtime.db.execute("UPDATE repo_action_jobs SET trusted=1")
    runtime.db.commit()
    [job] = lease(runtime, key)
    assert_untrusted_lease(job)


def test_a_writer_runs_at_once_but_workflow_changes_need_approval():
    runtime, key, repo, _audit = world()
    repo.add(7, HEAD_1, author="alice")
    sync(runtime)
    assert statuses(runtime) == [(7, HEAD_1, "queued", "")]
    [job] = lease(runtime, key)
    assert_untrusted_lease(job)
    assert job["gateWorkflowChanges"] is True
    answer = report(runtime, key, {
        "jobId": job["jobId"], "leaseId": job["leaseId"],
        "status": "completed", "commit": HEAD_1, "runs": [],
        "approvalRequired": "workflows"})
    assert answer["data"] == {"ok": True, "cancel": False}
    assert statuses(runtime) == [(7, HEAD_1, "waiting", "workflows")]
    assert column(runtime, job["jobId"], "lease_id") == ""
    assert lease(runtime, key) == []
    status, data = checks(runtime, viewer="alice")
    assert data["approval"]["reason"] == "workflows"
    assert data["approval"]["authorTrust"] == "writer"
    checks(runtime, viewer="alice", method="POST",
           body={"action": "approve", "head": HEAD_1})
    [again] = lease(runtime, key)
    assert again["jobId"] == job["jobId"]
    assert again["gateWorkflowChanges"] is False


def test_the_runner_cannot_downgrade_an_approved_run():
    runtime, key, repo, _audit = world()
    repo.add(7, HEAD_1, author="bob")
    sync(runtime)
    checks(runtime, viewer="alice", method="POST",
           body={"action": "approve", "head": HEAD_1})
    [job] = lease(runtime, key)
    report(runtime, key, {
        "jobId": job["jobId"], "leaseId": job["leaseId"],
        "status": "completed", "commit": HEAD_1, "runs": [],
        "approvalRequired": "workflows"})
    assert statuses(runtime) == [(7, HEAD_1, "completed", "approved")]


def test_a_new_commit_needs_a_new_approval_and_supersedes_the_old_one():
    runtime, key, repo, _audit = world()
    repo.add(7, HEAD_1, author="bob")
    sync(runtime)
    checks(runtime, viewer="alice", method="POST",
           body={"action": "approve", "head": HEAD_1})
    repo.add(7, HEAD_2, author="bob")
    sync(runtime)
    assert statuses(runtime) == [
        (7, HEAD_1, "cancelled", "approved"),
        (7, HEAD_2, "waiting", "author")]
    assert lease(runtime, key) == []
    sync(runtime)
    assert len(statuses(runtime)) == 2
    _status, data = checks(runtime, viewer="bob")
    assert data["head"] == HEAD_2 and data["previousHeads"] == 1
    assert data["approval"]["required"] is True


def test_always_run_for_an_author_trusts_only_the_verified_author():
    runtime, key, repo, audit = world()
    repo.add(7, HEAD_1, author="bob")
    sync(runtime)
    status, data = checks(runtime, viewer="alice", method="POST",
                          body={"action": "trust", "account": "carol"})
    assert status == 409 and data["error"] == "author_unknown"
    status, data = checks(runtime, viewer="alice", method="POST",
                          body={"action": "trust", "account": "@Bob"})
    assert status == 200 and data == {"ok": True, "trusted": "bob",
                                      "approved": 1}
    assert audit[-1][:4] == ("alice", "actions.trust_author",
                             "forkmesh/site#7", "success")
    assert runtime.db.execute(
        "SELECT owner,repo,account,added_by FROM "
        "repo_action_trusted_authors").fetchall()[0][:] == (
            "forkmesh", "site", "bob", "alice")
    repo.add(7, HEAD_2, author="bob")
    sync(runtime)
    assert statuses(runtime)[-1] == (7, HEAD_2, "queued", "")
    _status, data = checks(runtime, viewer="bob")
    assert data["approval"]["authorTrust"] == "allowlist"
    [job] = [j for j in lease(runtime, key) if j["commit"] == HEAD_2]
    assert_untrusted_lease(job)
    assert job["gateWorkflowChanges"] is True
    status, _ = checks(runtime, viewer="alice", method="POST",
                       body={"action": "untrust", "account": "bob"})
    assert status == 200 and audit[-1][1] == "actions.untrust_author"
    repo.add(7, "3" * 40, author="bob")
    sync(runtime)
    assert statuses(runtime)[-1] == (7, "3" * 40, "waiting", "author")


def test_a_claimed_author_name_is_not_trusted():
    runtime, _key, repo, _audit = world()
    repo.add(8, HEAD_1, author="alice", key="key-bob")
    repo.add(9, HEAD_2, author="alice", key="key-unknown")
    sync(runtime)
    assert statuses(runtime) == [(9, HEAD_2, "waiting", "author"),
                                 (8, HEAD_1, "waiting", "author")]
    assert column(runtime, 1, "actor") == ""
    assert column(runtime, 2, "actor") == "bob"


def test_only_the_base_catalog_decides_and_closed_pulls_do_not_run():
    runtime, _key, repo, _audit = world()
    runtime.db.execute("DELETE FROM repo_action_workflows")
    runtime.db.commit()
    add_catalog(runtime, ".forkmesh/ci.yml", ["push"])
    repo.add(7, HEAD_1, author="alice")
    assert sync(runtime) == 0 and repo.reads == []
    add_catalog(runtime, ".forkmesh/ci.yml", ["pull_request"])
    repo.add(7, HEAD_1, author="alice", status="closed")
    assert sync(runtime) == 0 and statuses(runtime) == []


def test_waiting_runs_show_in_the_actions_tab_and_expire():
    runtime, key, repo, _audit = world()
    repo.add(7, HEAD_1, author="bob")
    sync(runtime)
    snapshot = run(runner_actions.snapshot(runtime, "forkmesh", "site"))
    [item] = snapshot["runs"]
    assert item["status"] == "awaiting-approval" and item["pullNumber"] == 7
    assert item["event"] == "pull_request"
    assert item["workflowPath"] == ".forkmesh/ci.yml"
    public = runner_actions.public_summary(snapshot)
    assert public["runs"][0]["pullNumber"] == 7
    assert "logTail" not in public["runs"][0]
    runtime.clock = NOW + runner_actions.WAITING_TTL_MS + 1
    lease(runtime, key)
    assert statuses(runtime) == [(7, HEAD_1, "expired", "author")]


def test_approve_and_reject_from_the_actions_tab():
    runtime, _key, repo, _audit = world()
    repo.add(7, HEAD_1, author="bob")
    repo.add(8, HEAD_2, author="bob")
    sync(runtime)
    ids = {row[0]: row[1] for row in runtime.db.execute(
        "SELECT pull_number,id FROM repo_action_jobs")}
    stride = runner_actions.RUN_ID_STRIDE
    assert run(runner_actions.approve_run(
        runtime, "forkmesh", "site", ids[7] * stride, "alice")) == (
            "", 7, HEAD_1)
    assert run(runner_actions.approve_run(
        runtime, "forkmesh", "site", ids[7] * stride, "alice"))[0] == (
            "not_awaiting_approval")
    assert run(runner_actions.cancel(
        runtime, "forkmesh", "site", ids[8] * stride)) == ""
    assert dict((row[0], row[2]) for row in statuses(runtime)) == {
        7: "queued", 8: "cancelled"}


def test_a_rerun_of_a_pull_request_run_keeps_it_a_pull_request_run():
    runtime, key, repo, _audit = world()
    repo.add(7, HEAD_1, author="bob")
    sync(runtime)
    checks(runtime, viewer="alice", method="POST",
           body={"action": "approve", "head": HEAD_1})
    [job] = lease(runtime, key)
    report(runtime, key, {"jobId": job["jobId"], "leaseId": job["leaseId"],
                          "status": "completed", "commit": HEAD_1,
                          "runs": []})
    new_id, reason = run(runner_actions.rerun(
        runtime, "forkmesh", "site", job["jobId"] * 100, "alice"))
    assert not reason
    [again] = lease(runtime, key)
    assert again["jobId"] == new_id
    assert_untrusted_lease(again)
    assert again["pullNumber"] == 7 and again["baseCommit"] == BASE


def test_enqueue_refuses_a_head_ref_for_another_pull():
    runtime, _key, _repo, _audit = world()
    for ref in ("refs/pr/8/head", "refs/heads/../x", "refs/tags/v1", ""):
        assert run(runner_actions.enqueue_pull(
            runtime, "forkmesh", "site", 7, HEAD_1, ref, "main", BASE, "bob",
            "author")) == []
    assert run(runner_actions.enqueue_pull(
        runtime, "forkmesh", "site", 7, HEAD_1, "refs/pr/7/head", "main",
        BASE, "bob", "trusted")) == []
