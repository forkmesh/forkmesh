import asyncio
import base64
import hashlib
import importlib
import json
from pathlib import Path
import sqlite3
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(ROOT / "src"))

from forkmesh_host import ffi, webcrypto  # noqa: E402
from forkmesh_host import js as host_js  # noqa: E402
from forkmesh_host import instance_bootstrap  # noqa: E402

SEED = "22" * 32
TOKEN = "ab" * 32
NOW = 2_000_000_000_000


def unb64(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def content(event):
    kind = event["type"]
    attachments = ",".join(event.get("attachments") or [])
    if kind == "open":
        return "\x00".join([event["title"], event["body"], attachments])
    if kind == "comment":
        return "\x00".join([event["body"], attachments])
    if kind == "status":
        return event["status"]
    if kind == "labels":
        return ",".join(event["labels"])
    if kind == "claim":
        return "\x00".join([event["assignee"], event["claimState"]])
    return ""


class Request:
    def __init__(self, method="GET", body=None, token=TOKEN):
        self.method = method
        self.body = body
        self.headers = {"authorization": "Bearer " + token} if token else {}


class World:
    def __init__(self, issue=None):
        sys.modules.pop("actions_job_api", None)
        sys.modules.pop("runner_actions", None)
        self.module = importlib.import_module("actions_job_api")
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for name in ("0248_repo_action_jobs.sql", "0272_actions_instance.sql",
                     "0280_actions_pull_requests.sql"):
            text = (ROOT / "migrations" / name).read_text()
            self.db.executescript(
                "\n".join(part + ";" for part in text.split(
                    "INSERT INTO schema_meta")[0].split(";")
                    if "runner_nodes" not in part and part.strip()))
        self.submitted = []
        self.audits = []
        self.accounts = {"alice", "bob", "carol"}
        self.accept_status = 201
        self.issue = issue
        self.feature_off = None
        self.env = type("Env", (), {"ACTIONS_BOT_SEED": SEED,
                                    "ACTIONS_INSTANCE_RUNNER": "r"})()
        world = self

        async def d1_first(_env, sql, *args):
            row = world.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_all(_env, sql, *args):
            return [dict(r) for r in world.db.execute(sql, args).fetchall()]

        async def d1_run(_env, sql, *args):
            world.db.execute(sql, args)
            world.db.commit()

        async def accept(env, request, owner, repo, repo_bi, data, repo_keys,
                         actions_job=False):
            world.submitted.append((owner, repo, repo_bi, data, repo_keys,
                                    actions_job))
            if world.accept_status != 201:
                return {"status": world.accept_status,
                        "data": {"error": "author_quota"}}
            return {"status": 201, "data": {"ok": True, "number": 7}}

        async def account_row(_env, name):
            return ("bi:" + name,
                    {"name": name} if name in world.accounts else None)

        async def audit(_env, actor, action, target_type="", target="",
                        outcome="success", details=None):
            world.audits.append((actor, action, target_type, target, outcome,
                                 dict(details or {})))

        async def gateway(_env, owner, repo, action):
            world.gateway_calls = (owner, repo, action)
            if world.issue is None:
                return None
            return {"blobs": {".forkmesh/issues/open/3/issue-3.json": {
                "ok": True, "content": json.dumps(world.issue)}}}

        async def sign(public_key, private_key, message):
            key = Ed25519PrivateKey.from_private_bytes(unb64(private_key))
            return base64.urlsafe_b64encode(key.sign(message)).decode(
            ).rstrip("=")

        async def sha256_hex(text):
            return hashlib.sha256(text.encode()).hexdigest()

        async def nothing(*_args):
            return None

        async def feature_off(*_args):
            return world.feature_off

        async def decrypt(_env, value):
            return json.loads(value)

        async def blind_index(_env, value):
            return "bi:" + value

        self.module._bind_runtime({
            "js_crypto": webcrypto.crypto, "Uint8Array": host_js.Uint8Array,
            "_to_js": ffi.to_js, "to_js": ffi.to_js,
            "json_response": lambda data, status=200, **_: {
                "status": status, "data": data},
            "method_name": lambda request: request.method,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 63,
            "d1_first": d1_first, "d1_all": d1_all, "d1_run": d1_run,
            "ensure_schema": nothing,
            "_accept_issue_submission": accept,
            "_account_row": account_row,
            "_audit_sensitive_action": audit,
            "_forkbot_repo_gateway_json": gateway,
            "_forkbot_issue_json_candidates": lambda n: [
                ".forkmesh/issues/%s/%d/issue-%d.json" % (d, n, n)
                for d in ("open", "closed")],
            "_forkbot_blob_text": lambda blob: (blob or {}).get("content", ""),
            "ed25519_sign": sign, "sha256_hex": sha256_hex,
            "issue_event_content": content,
            "_b64url_encode": lambda raw: base64.urlsafe_b64encode(raw)
            .decode().rstrip("="),
            "_random_bytes": lambda n: b"\x01" * n,
            "_repo_feature_off": feature_off,
            "bounded_json_request": self.body,
            "decrypt_row": decrypt, "blind_index": blind_index,
            "Date": type("Date", (), {"now": staticmethod(lambda: NOW)}),
        })
        self.module._runtime = lambda env: Runtime(world)

    @staticmethod
    async def body(request, _limit=None):
        if not isinstance(request.body, dict):
            raise ValueError("bad")
        return request.body

    def add_job(self, status="running", token=TOKEN, payload="", trusted=1):
        self.db.execute(
            "INSERT INTO repo_action_jobs (owner,repo,branch,commit_oid,status,"
            "created_at,updated_at,event,workflow,ref,actor,payload,"
            "token_hash,trusted) VALUES ('forkmesh','site','main',?,?,1,1,"
            "'workflow_dispatch','.forkmesh/a.yml','refs/heads/main','ada',?,"
            "?,?)", ("c" * 40, status, payload,
                     hashlib.sha256(token.encode()).hexdigest(), trusted))
        self.db.commit()

    def call(self, path, request):
        return asyncio.run(self.module.route(self.env, request, path))


class Runtime:
    def __init__(self, world):
        self.world = world

    async def d1_all(self, sql, *args):
        return [dict(r) for r in self.world.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.world.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.world.db.execute(sql, args)
        self.world.db.commit()

    def now(self):
        return NOW

    async def seal(self, value):
        return json.dumps(value)

    async def default_branch(self, _owner, _repo):
        return "main"


def verify(event, owner="forkmesh", repo="site", number=0):
    body = "\n".join((event["type"], str(number), event["author"],
                      str(event["ts"]),
                      hashlib.sha256(content(event).encode()).hexdigest()))
    message = ("forkmesh-issue-event-v2\n%s/%s\n%s" % (owner, repo, body))
    Ed25519PublicKey.from_public_bytes(unb64(event["author"])).verify(
        unb64(event["sig"]), message.encode())


def test_the_token_must_belong_to_a_live_job():
    world = World()
    assert world.call("/api/actions/job/event", Request())["status"] == 401
    world.add_job(status="completed")
    assert world.call("/api/actions/job/event", Request())["status"] == 401
    world = World()
    world.add_job()
    assert world.call("/api/actions/job/event",
                      Request(token="x" * 64))["status"] == 401
    assert world.call("/api/actions/job/event",
                      Request(token=""))["status"] == 401


def test_the_event_payload_is_the_job_payload():
    world = World()
    world.add_job(payload=json.dumps({"event": {
        "inputs": {"a": "1"}, "ref": "refs/heads/main",
        "sender": {"login": "ada"}}}))
    answer = world.call("/api/actions/job/event", Request())
    assert answer["status"] == 200
    assert answer["data"] == {
        "inputs": {"a": "1"}, "ref": "refs/heads/main",
        "sender": {"login": "ada"},
        "repository": {"full_name": "forkmesh/site"}}


def test_opening_commenting_and_closing_are_signed_by_the_bot():
    world = World()
    world.add_job()
    bot_key = instance_bootstrap.bot_public_key(SEED)
    answer = world.call("/api/actions/job/issues", Request(
        "POST", {"title": "Nightly failed", "body": "see log",
                 "labels": ["ci", {"name": "bug"}]}))
    assert answer == {"status": 201, "data": {"ok": True, "number": 7}}
    owner, repo, repo_bi, data, keys, from_job = world.submitted[-1]
    assert (owner, repo, repo_bi, keys, from_job) == (
        "forkmesh", "site", "bi:forkmesh/site", ("forkmesh/site",), True)
    event = data["event"]
    assert event["author"] == bot_key
    assert event["authorName"] == "forkmesh-actions"
    assert data["meta"] == {"labels": ["ci", "bug"]}
    verify(event)
    world.call("/api/actions/job/issues/3/comments",
               Request("POST", {"body": "Thanks!"}))
    verify(world.submitted[-1][3]["event"], number=3)
    world.call("/api/actions/job/issues/3/state",
               Request("POST", {"state": "closed"}))
    event = world.submitted[-1][3]["event"]
    assert event["status"] == "closed"
    verify(event, number=3)
    assert world.call("/api/actions/job/issues/3/state", Request(
        "POST", {"state": "gone"}))["status"] == 400
    assert world.call("/api/actions/job/issues", Request(
        "POST", {"body": "no title"}))["status"] == 400
    world.env.ACTIONS_BOT_SEED = ""
    assert world.call("/api/actions/job/issues/3/comments", Request(
        "POST", {"body": "x"}))["status"] == 503


def test_reading_an_issue_and_changing_its_labels():
    world = World(issue={"number": 3, "title": "Bug", "status": "open",
                         "labels": ["bug", "hackathon"],
                         "events": [{"type": "open", "body": "It broke",
                                     "authorName": "bob"}]})
    world.add_job()
    answer = world.call("/api/actions/job/issues/3", Request())
    assert answer["data"] == {
        "number": 3, "title": "Bug", "body": "It broke", "state": "open",
        "labels": ["bug", "hackathon"], "assignees": [], "author": "bob"}
    assert world.gateway_calls[:2] == ("forkmesh", "site")
    world.call("/api/actions/job/issues/3/labels", Request(
        "POST", {"add": ["triaged", "bug"], "remove": ["hackathon"]}))
    event = world.submitted[-1][3]["event"]
    assert event["labels"] == ["bug", "triaged"]
    verify(event, number=3)
    assert world.call("/api/actions/job/issues/4", Request())[
        "status"] == 404
    assert world.call("/api/actions/job/issues/3", Request("POST", {}))[
        "status"] == 405
    assert world.call("/api/actions/job/nope", Request())["status"] == 404
    world.feature_off = {"status": 404, "data": {"error": "disabled"}}
    assert world.call("/api/actions/job/issues/3/comments", Request(
        "POST", {"body": "x"}))["status"] == 404


def queue(world, event, item=None, number=3):
    return asyncio.run(world.module.queue_issue_event(
        world.env, "forkmesh", "site", number, event, item or {}))


def jobs(world):
    return [(row["event"], row["workflow"], json.loads(row["payload"]))
            for row in world.db.execute(
                "SELECT event,workflow,payload FROM repo_action_jobs "
                "WHERE status='queued' ORDER BY id")]


def test_accepted_issue_events_queue_matching_workflows_with_github_payloads():
    world = World(issue={"number": 3, "title": "Bug", "status": "open",
                         "labels": ["bug"], "authorName": "bob",
                         "events": [{"type": "open", "body": "It broke"}]})
    for path, events in ((".forkmesh/triage.yml", ["issues"]),
                         (".forkmesh/reply.yml", ["issue_comment"]),
                         (".forkmesh/ci.yml", ["push"])):
        world.db.execute(
            "INSERT INTO repo_action_workflows (owner,repo,path,events,"
            "updated_at) VALUES ('forkmesh','site',?,?,1)",
            (path, json.dumps(events)))
    queue(world, {"type": "open", "title": "New", "body": "b",
                  "authorName": "carol", "authorVerified": True},
          {"meta": {"labels": ["x"]}}, number=9)
    [(event, workflow, payload)] = jobs(world)
    assert (event, workflow) == ("issues", ".forkmesh/triage.yml")
    assert payload["event"] == {
        "action": "opened", "sender": {"login": "carol", "verified": True},
        "issue": {"number": 9, "title": "New", "body": "b", "state": "open",
                  "labels": [{"name": "x"}], "user": {"login": "carol"}}}
    world.db.execute("DELETE FROM repo_action_jobs")
    queue(world, {"type": "labels", "labels": ["bug", "hackathon"],
                  "authorName": "ada"})
    queue(world, {"type": "status", "status": "closed", "authorName": "ada"})
    queue(world, {"type": "comment", "id": "c1", "body": "hi",
                  "authorName": "dan", "authorVerified": True})
    queue(world, {"type": "labels", "labels": ["bug"], "authorName": "ada"})
    queue(world, {"type": "comment", "body": "bot",
                  "authorName": "forkmesh-actions"})
    queue(world, {"type": "vote", "authorName": "ada"})
    found = jobs(world)
    assert [(e, p["event"]["action"]) for e, _w, p in found] == [
        ("issues", "labeled"), ("issues", "closed"),
        ("issue_comment", "created")]
    labeled = found[0][2]["event"]
    assert labeled["label"] == {"name": "hackathon"}
    assert labeled["issue"]["labels"] == [{"name": "bug"},
                                          {"name": "hackathon"}]
    assert labeled["issue"]["user"] == {"login": "bob"}
    assert found[1][2]["event"]["issue"]["state"] == "closed"
    assert found[2][2]["event"]["comment"] == {
        "id": "c1", "body": "hi", "user": {"login": "dan", "verified": True},
        "verified": True}
    assert found[2][2]["event"]["sender"] == {"login": "dan", "verified": True}
    assert found[2][1] == ".forkmesh/reply.yml"


def test_an_unverified_comment_author_reaches_actions_as_its_key_only():
    world = World(issue={"number": 3, "title": "Bug", "status": "open",
                         "labels": [], "events": []})
    world.db.execute(
        "INSERT INTO repo_action_workflows (owner,repo,path,events,"
        "updated_at) VALUES ('forkmesh','site','.forkmesh/a.yml',?,1)",
        (json.dumps(["issue_comment"]),))
    queue(world, {"type": "comment", "id": "c2", "body": "/assign",
                  "author": "webkey", "authorName": "alice"})
    queue(world, {"type": "comment", "id": "c3", "body": "/assign",
                  "author": "webkey2", "authorName": "",
                  "authorVerified": True})
    first, second = [p["event"] for _e, _w, p in jobs(world)]
    assert first["comment"]["user"] == {"login": "webkey", "verified": False}
    assert first["comment"]["verified"] is False
    assert first["sender"] == {"login": "webkey", "verified": False}
    assert second["comment"]["user"] == {"login": "webkey2",
                                         "verified": False}


ASSIGN = "/api/actions/job/issues/3/assignees"


def assign_world(assignees=()):
    world = World(issue={"number": 3, "title": "Bug", "status": "open",
                         "labels": [], "assignees": list(assignees),
                         "events": [{"type": "open", "body": "b",
                                     "authorName": "bob"}]})
    world.add_job()
    return world


def test_assigning_signs_one_claim_per_account_as_the_bot():
    world = assign_world(["Carol"])
    bot_key = instance_bootstrap.bot_public_key(SEED)
    answer = world.call(ASSIGN, Request(
        "POST", {"add": ["@Alice", "bob"], "remove": ["carol"]}))
    assert answer == {"status": 200, "data": {
        "ok": True, "number": 3, "assignees": ["alice", "bob"],
        "assigned": ["alice", "bob"], "unassigned": ["carol"],
        "changed": True}}
    events = []
    for owner, repo, repo_bi, data, keys, from_job in world.submitted:
        assert (owner, repo, repo_bi, keys, from_job) == (
            "forkmesh", "site", "bi:forkmesh/site", ("forkmesh/site",), True)
        assert data["number"] == 3
        event = data["event"]
        assert event["author"] == bot_key
        assert event["authorName"] == "forkmesh-actions"
        verify(event, number=3)
        events.append((event["type"], event["assignee"],
                       event["claimState"]))
    assert events == [("claim", "alice", "assigned"),
                      ("claim", "bob", "assigned"),
                      ("claim", "carol", "released")]
    [(actor, action, kind, target, outcome, details)] = world.audits
    assert (actor, action, kind, target, outcome) == (
        "forkmesh-actions", "actions.issue_assignees", "issue",
        "forkmesh/site#3", "success")
    assert details["assigned"] == "alice,bob"
    assert details["unassigned"] == "carol"
    assert details["job"] == 1


def test_assigning_is_idempotent():
    world = assign_world(["alice"])
    answer = world.call(ASSIGN, Request(
        "POST", {"add": ["alice"], "remove": ["bob"]}))
    assert answer["status"] == 200
    assert answer["data"]["changed"] is False
    assert answer["data"]["assignees"] == ["alice"]
    assert world.submitted == []
    assert world.audits == []


def test_only_existing_accounts_can_be_assigned():
    world = assign_world()
    answer = world.call(ASSIGN, Request("POST", {"add": ["alice", "mallory"]}))
    assert answer["status"] == 422
    assert answer["data"]["error"] == "unknown_account"
    assert world.submitted == []
    assert world.audits[0][4] == "denied"
    assert world.audits[0][5]["accounts"] == "mallory"


def test_assignee_requests_are_validated():
    world = assign_world()
    for body in ({}, {"add": []}, {"add": "not a name"},
                 {"add": ["alice"], "remove": ["alice"]},
                 {"add": ["a%d" % i for i in range(11)]},
                 {"add": {"alice": 1}}):
        assert world.call(ASSIGN, Request("POST", body))["status"] == 400
    assert world.call(ASSIGN, Request("GET"))["status"] == 405
    assert world.submitted == []


def test_assigning_needs_a_live_job_token_and_an_existing_issue():
    world = assign_world()
    assert world.call(ASSIGN, Request("POST", {"add": ["alice"]},
                                      token="cd" * 32))["status"] == 401
    world = World()
    world.add_job(status="completed")
    assert world.call(ASSIGN, Request("POST", {"add": ["alice"]}))[
        "status"] == 401
    world = World(issue=None)
    world.add_job()
    answer = world.call(ASSIGN, Request("POST", {"add": ["alice"]}))
    assert answer["status"] == 404
    assert answer["data"]["error"] == "issue_not_found"
    world = assign_world()
    world.feature_off = {"status": 404, "data": {"error": "disabled"}}
    assert world.call(ASSIGN, Request("POST", {"add": ["alice"]}))[
        "status"] == 404
    assert world.submitted == []


def test_an_inbox_refusal_stops_the_change_and_is_audited():
    world = assign_world()
    world.accept_status = 429
    answer = world.call(ASSIGN, Request("POST", {"add": ["alice", "bob"]}))
    assert answer == {"status": 429, "data": {"error": "author_quota"}}
    assert len(world.submitted) == 1
    assert world.audits[0][4] == "failed"


def test_bot_events_on_an_org_repository_are_signed_under_the_org_address():
    world = World()
    world.add_job()

    async def public_owner(_env, _owner, _repo):
        return "acme"

    world.module.public_owner = public_owner
    world.call("/api/actions/job/issues/3/comments",
               Request("POST", {"body": "Thanks!"}))
    owner, repo, repo_bi, data, keys, from_job = world.submitted[-1]
    assert keys == ("acme/site", "forkmesh/site")
    assert data["event"]["authorName"] == "acme-actions"
    verify(data["event"], owner="acme", number=3)


def _trusted_flags(world):
    return [row["trusted"] for row in world.db.execute(
        "SELECT trusted FROM repo_action_jobs WHERE status='queued' "
        "ORDER BY id")]


def test_only_issue_events_by_repository_writers_queue_trusted_jobs():
    world = World(issue={"number": 3, "title": "Bug", "status": "open",
                         "labels": [], "authorName": "bob", "events": []})
    world.db.execute(
        "INSERT INTO repo_action_workflows (owner,repo,path,events,"
        "updated_at) VALUES ('forkmesh','site','.forkmesh/reply.yml',?,1)",
        (json.dumps(["issue_comment"]),))

    async def owns(_env, account, node):
        return (account, node) == ("ada", "forkmesh")

    async def org_write(_env, owner, repo, account):
        return account == "wes"

    async def key_account(_env, pubkey):
        return {"key-of-wes": "wes", "key-of-out": "outsider"}.get(pubkey, "")

    world.module._bind_runtime({
        "_account_owns_node": owns, "_org_write_allowed": org_write,
        "_git_push_account_for_pubkey": key_account})
    comment = {"type": "comment", "id": "c", "body": "$(curl evil)"}
    # A claimed name proves nothing; an outsider stays untrusted.
    queue(world, dict(comment, authorName="ada"))
    queue(world, dict(comment, authorName="mallory", authorVerified=True))
    queue(world, dict(comment, author="key-of-out", authorName="outsider"))
    # The session-verified owner, and an org writer by registered key.
    queue(world, dict(comment, authorName="ada", authorVerified=True))
    queue(world, dict(comment, author="key-of-wes", authorName="wes"))
    assert _trusted_flags(world) == [0, 0, 0, 1, 1]


def _outside_job(world, verified=True):
    world.add_job(trusted=0, payload=json.dumps({"event": {
        "action": "created", "sender": {"login": "bob", "verified": verified},
        "issue": {"number": 3}, "comment": {"body": "/assign"}}}))


def test_an_outside_issue_job_acts_only_on_its_own_issue():
    world = assign_world()
    world.db.execute("DELETE FROM repo_action_jobs")
    _outside_job(world)
    assert world.call("/api/actions/job/event", Request())["status"] == 200
    assert world.call("/api/actions/job/issues/3", Request())["status"] == 200
    for path, body in (
            ("/api/actions/job/issues/4", None),
            ("/api/actions/job/issues/4/comments", {"body": "hi"}),
            ("/api/actions/job/issues", {"title": "spam"}),
            ("/api/actions/job/issues/3/state", {"state": "closed"})):
        method = "GET" if body is None else "POST"
        answer = world.call(path, Request(method, body))
        assert answer["status"] == 403, path
    assert world.submitted == []
    answer = world.call(ASSIGN, Request("POST", {"add": ["carol"]}))
    assert answer["status"] == 403
    answer = world.call(ASSIGN, Request("POST", {"add": ["bob"]}))
    assert answer["status"] == 200
    assert answer["data"]["assigned"] == ["bob"]
    answer = world.call("/api/actions/job/issues/3/comments",
                        Request("POST", {"body": "Assigned to you."}))
    assert answer["status"] in (200, 201)


def test_an_unverified_outside_sender_cannot_be_assigned():
    world = assign_world()
    world.db.execute("DELETE FROM repo_action_jobs")
    _outside_job(world, verified=False)
    answer = world.call(ASSIGN, Request("POST", {"add": ["bob"]}))
    assert answer["status"] == 403
