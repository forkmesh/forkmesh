"""Repository webhooks: policy, wire format, storage, delivery and wiring.

Two halves. The pure half checks the parts a receiver depends on — URL policy,
event vocabulary, retry classification and the signature — against
``tools/forkmesh_integration.py``, which is both the published reference
implementation and what a third party will actually verify with. The runtime
half runs the module's real SQL against an in-memory SQLite built from the
shipped migration, with narrow fakes for the Worker's crypto and fetch, so a
column or placeholder mistake fails here rather than in production.
"""

import ast
import asyncio
import base64
import hashlib
import hmac
import json
import os
import re
import sqlite3
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(REPO / "tools"))

import forkmesh_integration as fm  # noqa: E402
import repository_webhooks as hooks  # noqa: E402
import schema  # noqa: E402
import urls  # noqa: E402


NOW = 1_800_000_000_000
CODEMAGIC = "https://api.codemagic.io/hooks/6aac6e1e7b35578e30ad5a76"


# --- pure policy -------------------------------------------------------------

def test_only_public_https_destinations_are_accepted():
    assert hooks.clean_url(CODEMAGIC) == CODEMAGIC
    assert hooks.clean_url("https://ci.example.org:8443/hook?x=1") == (
        "https://ci.example.org:8443/hook?x=1")
    # A public IP literal is legitimate for a self-run runner.
    assert hooks.clean_url("https://93.184.216.34/hook") == (
        "https://93.184.216.34/hook")
    # The blocklist matches names, not substrings of them.
    assert hooks.clean_url("https://notforkmesh.com/hook") == (
        "https://notforkmesh.com/hook")
    for refused in (
            "",
            "http://ci.example.org/hook",          # not https
            "https://user:pw@ci.example.org/hook",  # credentials
            "https://ci.example.org/hook#frag",     # fragment
            "https://127.0.0.1/hook",               # loopback
            "https://10.1.2.3/hook",                # private range
            "https://169.254.169.254/latest",       # link-local metadata
            "https://[::1]/hook",                   # loopback v6
            "https://localhost/hook",
            "https://runner.internal/hook",
            "https://runner.local/hook",
            "https://demo.example/hook",
            "https://a.localhost/hook",
            "https://mirror35.forkmesh.com/hook",  # a subdomain of our own
            "https://forkmesh.com/api/repositories",  # our own relay
            "https://app.forkmesh.com/api/sync",
            # Production (audit 2026-09-24 M6): Next never delivers there.
            "https://app.forkmesh.com/api/sync",
            "https://forkmesh.com/hook",
            "https://mirror35.forkmesh.com/hook",
            "https://WORLD.forkmesh.com./hook",
            "https://ci.example.org/hook\nX-Evil: 1",  # header injection
            "https://" + "a" * 700 + ".example.org/hook",
    ):
        assert hooks.clean_url(refused) == "", refused


def test_event_vocabulary_is_the_three_named_triggers_plus_release():
    assert hooks.EVENT_TYPES == (
        "repository.push", "branch.push", "pull.merged", "release.published")
    # Normalization is order-stable and drops anything unknown, so a stored
    # subscription cannot grow an event the delivery path never emits.
    assert hooks.normalize_events(
        ["pull.merged", "bogus", "REPOSITORY.PUSH", "pull.merged"]) == (
        "repository.push", "pull.merged")
    assert hooks.normalize_events("branch.push release.published") == (
        "branch.push", "release.published")
    assert hooks.normalize_events(None) == ()


def test_retry_classification_matches_the_published_policy():
    contract = json.loads(
        (REPO / "conformance" / "integrations" / "v1"
         / "contract.json").read_text("utf-8"))
    retries = contract["policies"]["retries"]
    assert set(retries["retryStatuses"]) == set(hooks.RETRY_STATUSES)
    assert retries["maxAttempts"] == hooks.MAX_ATTEMPTS
    assert contract["policies"]["signatures"]["toleranceSeconds"] == (
        hooks.TOLERANCE_SECONDS)
    for status in hooks.RETRY_STATUSES:
        assert hooks.classify(status) == "retry"
    assert hooks.classify(0) == "retry"          # no response arrived
    assert hooks.classify(200) == "delivered"
    assert hooks.classify(204) == "delivered"
    assert hooks.classify(410) == "dropped"      # Gone: stop for good
    assert hooks.classify(404) == "dropped"
    # Backoff is capped, so an endpoint that is down for hours does not push a
    # retry past the point anybody cares about it.
    assert hooks.backoff_ms(1) < hooks.backoff_ms(3) <= hooks.MAX_DELAY_MS
    assert hooks.backoff_ms(99) == hooks.MAX_DELAY_MS


def test_delivery_is_verifiable_by_the_published_reference_receiver():
    # The whole point of matching forkmesh-webhook-v1 is that a receiver using
    # the documented snippet accepts our bytes without a ForkMesh-specific
    # branch. Build one exactly as the Worker does and verify it with the tool.
    assert hooks.WEBHOOK_SCHEME == fm.WEBHOOK_SCHEME
    assert hooks.EVENT_SCHEMA == fm.EVENT_SCHEMA
    event = hooks.build_event(
        "forkmesh", "forkmesh", "release.published", "v0.9.0",
        {"tag": "v0.9.0", "author": "forkmesh"}, NOW, 7)
    body = hooks.canonical_body(event)
    assert body == json.dumps(
        event, sort_keys=True, separators=(",", ":")).encode()
    stamp = int(time.time())
    signature = hooks.sign_body("s3cret", event["id"], stamp, body)
    assert signature == fm.webhook_signature(
        "s3cret", event["id"], stamp, body)
    headers = hooks.delivery_headers(signature, event, 1, stamp)
    assert set(fm.REQUIRED_WEBHOOK_HEADERS) <= set(headers)
    verified = fm.verify_webhook("s3cret", headers, body)
    assert verified["type"] == "release.published"
    assert verified["sequence"] == 7
    assert verified["subject"] == "forkmesh/forkmesh"
    # A tampered body must not verify.
    try:
        fm.verify_webhook("s3cret", headers, body + b" ")
    except fm.WebhookVerificationError:
        pass
    else:  # pragma: no cover
        raise AssertionError("tampered body verified")


def test_event_id_is_a_stable_idempotency_key():
    first = hooks.build_event("o", "r", "branch.push", "abc", {}, NOW, 1)
    again = hooks.build_event("o", "r", "branch.push", "abc", {}, NOW + 9, 2)
    # Same repository state re-published by a retrying node is one event, so a
    # receiver deduplicating on the id does not build twice.
    assert first["id"] == again["id"]
    moved = hooks.build_event("o", "r", "branch.push", "def", {}, NOW, 3)
    assert moved["id"] != first["id"]
    assert re.fullmatch(r"[0-9a-f]{32}", first["id"])


def test_the_codemagic_hook_is_declared_for_the_flagship_repository():
    seeds = hooks.seeds_for("forkmesh", "forkmesh")
    assert len(seeds) == 1
    seed = seeds[0]
    assert seed["url"] == CODEMAGIC
    assert hooks.clean_url(seed["url"]) == CODEMAGIC
    # The three trigger kinds Codemagic is configured with, plus the release
    # that is the reason the mobile artifacts have to be rebuilt.
    assert hooks.normalize_events(seed["events"]) == (
        "repository.push", "branch.push", "pull.merged", "release.published")
    assert hooks.seeds_for("someone", "else") == ()


def test_a_stored_view_never_leaks_the_signing_secret():
    row = {"webhook_id": "a" * 32, "active": 1, "seed_key": "",
           "created_at": NOW, "updated_at": NOW}
    record = {"url": CODEMAGIC, "events": ["branch.push"], "secret": "nope"}
    view = hooks.public_view(row, record)
    assert "secret" not in view
    assert view["url"] == CODEMAGIC
    # Only the create/rotate responses pass a secret in explicitly.
    assert hooks.public_view(row, record, "shown-once")["secret"] == (
        "shown-once")
    assert hooks.redact_url(
        "https://api.codemagic.io/hooks/6aac6e1e7b35578e30ad5a76?token=x"
    ) == "https://api.codemagic.io/hooks/6aac6e1e7b35578e30ad5a76"


# --- Worker runtime fakes ----------------------------------------------------

class _Bytes:
    def __init__(self, raw):
        self.raw = bytes(raw)

    def to_py(self):
        return self.raw


class _Uint8Array:
    @staticmethod
    def new(value):
        if isinstance(value, _Bytes):
            return _Bytes(value.raw)
        if isinstance(value, int):
            return _Bytes(b"\0" * value)
        return _Bytes(value)


class _Subtle:
    async def importKey(self, _fmt, key, *_rest):
        return key

    async def sign(self, _alg, key, message):
        return _Bytes(hmac.new(key.raw, message.raw, hashlib.sha256).digest())


class _Crypto:
    subtle = _Subtle()

    @staticmethod
    def getRandomValues(buffer):
        return _Bytes(os.urandom(len(buffer.raw)))


class _Date:
    now_ms = NOW

    @classmethod
    def now(cls):
        return cls.now_ms


class _Response:
    def __init__(self, body, status):
        self.body = body
        self.status = status


class _Request:
    def __init__(self, method="POST", body=None,
                 url="https://app.forkmesh.com/api/repo/o/r/webhooks"):
        self.method = method
        self.body = body or {}
        self.url = url


class Fetches:
    """Scripted outbound fetch: records every attempt, replays statuses."""

    def __init__(self, statuses=(200,)):
        self.statuses = list(statuses)
        self.calls = []

    async def __call__(self, url, init, _timeout):
        self.calls.append({
            "url": url,
            "headers": dict(init["headers"]),
            "body": init["body"],
            "event": json.loads(init["body"]),
        })
        status = self.statuses[min(len(self.calls) - 1, len(self.statuses) - 1)]
        if status == 0:
            raise RuntimeError("transport error")
        return _Response("", status)


def _db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript(
        (ROOT / "migrations" / "0188_repository_webhooks.sql").read_text(
            "utf-8"))
    return db


def _sql(query):
    # D1 uses ? placeholders exactly as sqlite3 does; the only dialect gap is
    # that the module never relies on RETURNING.
    assert "RETURNING" not in query.upper(), query
    return query


def _install(db, fetches=None, authorized=True, private=()):
    """Bind the module to fakes over real SQLite and return the fetch spy."""
    fetches = fetches or Fetches()

    async def d1_run(_env, query, *args):
        db.execute(_sql(query), args)
        db.commit()

    async def d1_first(_env, query, *args):
        row = db.execute(_sql(query), args).fetchone()
        return dict(row) if row else None

    async def d1_all(_env, query, *args):
        return [dict(row) for row in db.execute(_sql(query), args).fetchall()]

    async def blind_index(_env, value):
        return hashlib.sha256(
            ("bi:" + str(value).strip().lower()).encode()).hexdigest()

    async def encrypt_row(_env, obj):
        return base64.b64encode(json.dumps(obj).encode()).decode()

    async def decrypt_row(_env, stored):
        try:
            return json.loads(base64.b64decode(stored).decode())
        except Exception:
            return None

    async def bounded_json_request(request):
        return request.body

    async def ensure_schema(_env):
        return None

    async def authorize(_env, _request, _owner, _repo, _data):
        return authorized

    async def repo_is_private(_env, _owner, repo):
        return str(repo).lower() in private

    def json_response(payload, status=200, **_kwargs):
        return _Response(payload, status)

    hooks._bind_runtime({
        "Date": _Date,
        "Uint8Array": _Uint8Array,
        "js_crypto": _Crypto,
        "to_js": lambda value: value,
        "_to_js": lambda value: value,
        "js_fetch_with_timeout": fetches,
        "d1_run": d1_run,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "blind_index": blind_index,
        "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "bounded_json_request": bounded_json_request,
        "ensure_schema": ensure_schema,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 40,
        "_authorize_repo_owner_web": authorize,
        "_repo_is_private": repo_is_private,
    })
    # A previous test's bindings are already in globals(); _bind_runtime only
    # fills gaps, so overwrite the ones that carry per-test state.
    hooks.__dict__.update({
        "d1_run": d1_run, "d1_first": d1_first, "d1_all": d1_all,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "js_fetch_with_timeout": fetches,
        "bounded_json_request": bounded_json_request,
        "_authorize_repo_owner_web": authorize,
        "_repo_is_private": repo_is_private,
        "json_response": json_response,
    })
    return fetches


def _call(body, method="POST", owner="forkmesh", repo="relay"):
    return asyncio.run(hooks.repo_webhooks_handler(
        object(), _Request(method, body), owner, repo))


# --- runtime behaviour -------------------------------------------------------

def test_owner_can_create_list_pause_rotate_and_delete_a_webhook():
    db = _db()
    _install(db)
    created = _call({"action": "create", "url": "https://ci.example.org/hook",
                     "description": "Example CI",
                     "events": ["branch.push", "pull.merged"]})
    assert created.status == 201
    hook = created.body["webhook"]
    secret = hook["secret"]
    assert re.fullmatch(r"[0-9a-f]{64}", secret)
    assert hook["events"] == ["branch.push", "pull.merged"]

    listed = _call({"action": "list"})
    assert listed.status == 200
    rows = [item for item in listed.body["webhooks"]
            if item["url"] == "https://ci.example.org/hook"]
    assert len(rows) == 1
    # The secret is write-once: no read-back route hands it out again.
    assert "secret" not in rows[0]
    assert [event["type"] for event in listed.body["events"]] == list(
        hooks.EVENT_TYPES)

    paused = _call({"action": "update", "id": hook["id"], "active": False,
                    "events": ["branch.push"]})
    assert paused.body["webhook"]["active"] is False
    assert paused.body["webhook"]["events"] == ["branch.push"]

    rotated = _call({"action": "rotate", "id": hook["id"]})
    assert rotated.body["webhook"]["secret"] != secret

    assert _call({"action": "delete", "id": hook["id"]}).status == 200
    remaining = _call({"action": "list"}).body["webhooks"]
    assert all(item["id"] != hook["id"] for item in remaining)


def test_bad_input_and_a_stranger_are_both_refused():
    db = _db()
    _install(db)
    assert _call({"action": "create", "url": "http://ci.example.org/hook",
                  "events": ["branch.push"]}).body["error"] == "invalid_url"
    assert _call({"action": "create", "url": "https://127.0.0.1/hook",
                  "events": ["branch.push"]}).body["error"] == "invalid_url"
    assert _call({"action": "create", "url": "https://ci.example.org/hook",
                  "events": ["nope"]}).body["error"] == "no_events"
    ok = _call({"action": "create", "url": "https://ci.example.org/hook",
                "events": ["branch.push"]})
    assert ok.status == 201
    assert _call({"action": "create", "url": "https://ci.example.org/hook",
                  "events": ["branch.push"]}).body["error"] == "duplicate_url"
    assert _call({"action": "update", "id": "f" * 32,
                  "events": ["branch.push"]}).status == 404
    assert _call({"action": "list"}, method="GET").status == 405

    _install(db, authorized=False)
    denied = _call({"action": "list"})
    assert denied.status == 403 and denied.body["error"] == "not_authorized"


def test_the_codemagic_hook_is_materialized_once_and_stays_deleted():
    db = _db()
    _install(db)
    listed = _call({"action": "list"}, owner="forkmesh", repo="forkmesh")
    seeded = [item for item in listed.body["webhooks"]
              if item["url"] == CODEMAGIC]
    assert len(seeded) == 1
    assert seeded[0]["managed"] is True
    assert seeded[0]["active"] is True
    assert seeded[0]["events"] == [
        "repository.push", "branch.push", "pull.merged", "release.published"]

    # Listing again must not add a second copy.
    again = _call({"action": "list"}, owner="forkmesh", repo="forkmesh")
    assert len([item for item in again.body["webhooks"]
                if item["url"] == CODEMAGIC]) == 1

    # A repository ForkMesh does not declare a hook for gets none.
    assert _call({"action": "list"}, owner="someone",
                 repo="else").body["webhooks"] == []

    # An owner who deletes it means it.
    assert _call({"action": "delete", "id": seeded[0]["id"]},
                 owner="forkmesh", repo="forkmesh").status == 200
    after = _call({"action": "list"}, owner="forkmesh", repo="forkmesh")
    assert [item for item in after.body["webhooks"]
            if item["url"] == CODEMAGIC] == []


def test_a_lost_seed_bookkeeping_write_still_cannot_double_register():
    db = _db()
    _install(db)
    _call({"action": "list"}, owner="forkmesh", repo="forkmesh")
    # Simulate the bookkeeping row never landing: without a real constraint the
    # next pass would happily insert a second copy under a fresh id.
    db.execute("DELETE FROM repo_webhook_seeds")
    db.commit()
    listed = _call({"action": "list"}, owner="forkmesh", repo="forkmesh")
    assert len([item for item in listed.body["webhooks"]
                if item["url"] == CODEMAGIC]) == 1


def test_an_event_reaches_only_the_endpoints_subscribed_to_it():
    db = _db()
    fetches = _install(db)
    _call({"action": "create", "url": "https://ci.example.org/builds",
           "events": ["branch.push"]})
    _call({"action": "create", "url": "https://chat.example.org/notify",
           "events": ["release.published"]})
    paused = _call({"action": "create", "url": "https://old.example.org/hook",
                    "events": ["branch.push"]}).body["webhook"]
    _call({"action": "update", "id": paused["id"], "active": False,
           "events": ["branch.push"]})
    fetches.calls.clear()

    queued = asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "branch.push",
        {"branch": "main", "after": "a" * 40}, "a" * 40))
    assert queued == 1
    assert [call["url"] for call in fetches.calls] == [
        "https://ci.example.org/builds"]
    delivered = fetches.calls[0]
    assert delivered["headers"]["ForkMesh-Event-Type"] == "branch.push"
    assert delivered["event"]["subject"] == "forkmesh/relay"
    assert delivered["event"]["data"]["branch"] == "main"
    # An unknown event type is never emitted, whatever a caller passes.
    assert asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "repository.deleted", {}, "x")) == 0


def test_each_delivery_verifies_under_the_endpoints_own_secret():
    db = _db()
    fetches = _install(db)
    created = _call({"action": "create", "url": "https://ci.example.org/hook",
                     "events": ["release.published"]}).body["webhook"]
    fetches.calls.clear()
    asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "release.published",
        {"tag": "v1.2.3"}, "v1.2.3"))
    call = fetches.calls[0]
    verified = fm.verify_webhook(
        created["secret"], call["headers"], call["body"].encode(),
        now=int(call["headers"]["ForkMesh-Webhook-Timestamp"]))
    assert verified["type"] == "release.published"
    assert verified["data"]["tag"] == "v1.2.3"
    # A different endpoint's secret must not verify this delivery.
    try:
        fm.verify_webhook(
            "0" * 64, call["headers"], call["body"].encode(),
            now=int(call["headers"]["ForkMesh-Webhook-Timestamp"]))
    except fm.WebhookVerificationError:
        pass
    else:  # pragma: no cover
        raise AssertionError("foreign secret verified")


def test_each_endpoint_sees_a_gapless_sequence_of_its_own_events():
    db = _db()
    fetches = _install(db)
    # Two endpoints on one repository with different subscriptions. The
    # documented consumer treats a gap as a lost delivery and asks for a
    # resync, so neither may see the other's events as holes in its stream.
    _call({"action": "create", "url": "https://builds.example.org/hook",
           "events": ["repository.push"]})
    _call({"action": "create", "url": "https://chat.example.org/hook",
           "events": ["release.published"]})
    fetches.calls.clear()
    for index in range(3):
        asyncio.run(hooks.queue_event(
            object(), "forkmesh", "relay", "repository.push",
            {"after": str(index) * 40}, str(index) * 40))
        asyncio.run(hooks.queue_event(
            object(), "forkmesh", "relay", "release.published",
            {"tag": "v%d" % index}, "v%d" % index))
    by_url = {}
    for call in fetches.calls:
        by_url.setdefault(call["url"], []).append(call["event"]["sequence"])
    assert by_url == {
        "https://builds.example.org/hook": [1, 2, 3],
        "https://chat.example.org/hook": [1, 2, 3],
    }
    # Both endpoints agree on the id of an event they both receive, so a shared
    # receiver deduplicates it.
    _call({"action": "create", "url": "https://both.example.org/hook",
           "events": ["branch.push"]})
    _call({"action": "create", "url": "https://also.example.org/hook",
           "events": ["branch.push"]})
    fetches.calls.clear()
    asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "branch.push", {}, "b" * 40))
    ids = {call["event"]["id"] for call in fetches.calls}
    assert len(fetches.calls) == 2 and len(ids) == 1

    # A counter belongs to the endpoint, so a new endpoint starts at 1 however
    # long the repository has been publishing.
    fetches.calls.clear()
    _call({"action": "create", "url": "https://late.example.org/hook",
           "events": ["repository.push"]})
    fetches.calls.clear()
    asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "repository.push", {}, "z" * 40))
    late = [call for call in fetches.calls
            if call["url"] == "https://late.example.org/hook"]
    assert late[0]["event"]["sequence"] == 1


def test_a_private_repository_never_reaches_a_third_party():
    db = _db()
    fetches = _install(db, private=("relay",))
    # The endpoint is registered and active; visibility alone stops delivery,
    # so flipping a public repository to private takes effect immediately
    # without the owner having to remember to delete the webhook.
    _call({"action": "create", "url": "https://ci.example.org/hook",
           "events": list(hooks.EVENT_TYPES)})
    fetches.calls.clear()
    for event_type in hooks.EVENT_TYPES:
        assert asyncio.run(hooks.queue_event(
            object(), "forkmesh", "relay", event_type, {}, "a" * 40)) == 0
    assert fetches.calls == []
    assert db.execute(
        "SELECT COUNT(*) AS c FROM repo_webhook_outbox").fetchone()["c"] == 0


def test_a_missing_runtime_binding_fails_closed_instead_of_raising():
    # queue_event is awaited straight from the push, merge and release paths.
    # None of them may fail because this domain did.
    db = _db()
    _install(db)
    broken = hooks.__dict__.pop("d1_all")
    try:
        assert asyncio.run(hooks.queue_event(
            object(), "forkmesh", "relay", "branch.push", {}, "a" * 40)) == 0
    finally:
        hooks.__dict__["d1_all"] = broken


def test_a_failing_endpoint_is_retried_then_given_up_on():
    db = _db()
    # Every attempt answers 500: retried with growing backoff, then dropped.
    fetches = _install(db, Fetches([500]))
    _call({"action": "create", "url": "https://ci.example.org/hook",
           "events": ["branch.push"]})
    fetches.calls.clear()
    asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "branch.push", {}, "a" * 40))
    assert len(fetches.calls) == 1
    queued = db.execute(
        "SELECT attempts, next_ts FROM repo_webhook_outbox").fetchall()
    assert len(queued) == 1 and queued[0]["attempts"] == 1
    assert queued[0]["next_ts"] > _Date.now()

    # Drive the remaining attempts by moving time forward.
    for _ in range(hooks.MAX_ATTEMPTS):
        _Date.now_ms += hooks.MAX_DELAY_MS + 1
        asyncio.run(hooks.drain_outbox(object(), 10))
    _Date.now_ms = NOW
    assert db.execute(
        "SELECT COUNT(*) AS c FROM repo_webhook_outbox").fetchone()["c"] == 0
    assert len(fetches.calls) == hooks.MAX_ATTEMPTS
    # Attempt numbers are what the receiver sees, and they count up.
    assert [int(call["headers"]["ForkMesh-Webhook-Attempt"])
            for call in fetches.calls] == list(
                range(1, hooks.MAX_ATTEMPTS + 1))
    logged = db.execute(
        "SELECT outcome, attempts, status FROM repo_webhook_deliveries"
    ).fetchall()
    assert [row["outcome"] for row in logged] == ["dropped"]
    assert logged[0]["attempts"] == hooks.MAX_ATTEMPTS
    hook = db.execute(
        "SELECT failures, last_status FROM repo_webhooks").fetchone()
    assert hook["failures"] == hooks.MAX_ATTEMPTS and hook["last_status"] == 500


def test_a_gone_endpoint_stops_immediately_and_a_transport_error_retries():
    db = _db()
    fetches = _install(db, Fetches([410]))
    _call({"action": "create", "url": "https://ci.example.org/hook",
           "events": ["branch.push"]})
    fetches.calls.clear()
    asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "branch.push", {}, "a" * 40))
    assert len(fetches.calls) == 1
    assert db.execute(
        "SELECT COUNT(*) AS c FROM repo_webhook_outbox").fetchone()["c"] == 0
    assert db.execute(
        "SELECT outcome FROM repo_webhook_deliveries").fetchone()[
            "outcome"] == "dropped"

    # No response at all (DNS/TLS/timeout) is a retry, not a drop.
    db2 = _db()
    fetches = _install(db2, Fetches([0, 200]))
    _call({"action": "create", "url": "https://ci.example.org/hook",
           "events": ["branch.push"]})
    fetches.calls.clear()
    asyncio.run(hooks.queue_event(
        object(), "forkmesh", "relay", "branch.push", {}, "a" * 40))
    assert db2.execute(
        "SELECT COUNT(*) AS c FROM repo_webhook_outbox").fetchone()["c"] == 1
    _Date.now_ms += hooks.MAX_DELAY_MS + 1
    asyncio.run(hooks.drain_outbox(object(), 10))
    _Date.now_ms = NOW
    assert db2.execute(
        "SELECT COUNT(*) AS c FROM repo_webhook_outbox").fetchone()["c"] == 0
    assert db2.execute(
        "SELECT outcome FROM repo_webhook_deliveries").fetchone()[
            "outcome"] == "delivered"
    assert db2.execute(
        "SELECT failures FROM repo_webhooks").fetchone()["failures"] == 0


def test_a_delivery_test_reports_the_endpoints_answer_without_queueing():
    db = _db()
    fetches = _install(db, Fetches([503]))
    created = _call({"action": "create", "url": "https://ci.example.org/hook",
                     "events": ["branch.push"]}).body["webhook"]
    fetches.calls.clear()
    result = _call({"action": "test", "id": created["id"]})
    assert result.body == {
        "ok": False, "status": 503, "outcome": "failed",
        "url": "https://ci.example.org/hook"}
    assert len(fetches.calls) == 1
    # A test is a probe, not an event: nothing is left queued for retry.
    assert db.execute(
        "SELECT COUNT(*) AS c FROM repo_webhook_outbox").fetchone()["c"] == 0


# --- wiring ------------------------------------------------------------------

def test_the_route_regex_and_schema_ship_with_the_module():
    assert urls.REPO_WEBHOOKS_RE.match(
        "/api/repo/forkmesh/forkmesh/webhooks").groups() == (
        "forkmesh", "forkmesh")
    assert not urls.REPO_WEBHOOKS_RE.match("/api/repo/forkmesh/webhooks")
    assert not urls.REPO_WEBHOOKS_RE.match(
        "/api/repo/forkmesh/forkmesh/webhooks/extra")
    tables = {
        "repo_webhooks", "repo_webhook_seeds", "repo_webhook_sequence",
        "repo_webhook_outbox", "repo_webhook_deliveries",
    }
    declared = {
        match.group(1)
        for statement in schema.SCHEMA_STATEMENTS
        for match in [re.search(
            r"CREATE TABLE IF NOT EXISTS (\w+)", statement)]
        if match
    }
    assert tables <= declared
    # The runtime DDL and the migration must create the same tables, or a fresh
    # database and a migrated one disagree.
    migration = (ROOT / "migrations" / "0188_repository_webhooks.sql").read_text(
        "utf-8")
    assert tables <= set(
        re.findall(r"CREATE TABLE IF NOT EXISTS (\w+)", migration))
    reverse = (ROOT / "migrations" / "down"
               / "0188_repository_webhooks.sql").read_text("utf-8")
    assert tables <= set(re.findall(r"DROP TABLE IF EXISTS (\w+)", reverse))


def test_entry_defers_the_module_and_purges_its_tables_with_a_repository():
    source = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert '_LazyModule("repository_webhooks")' in source
    assert 'queue_repo_webhook_event = _repository_webhooks.export(' in source
    # A repo-scoped table missing from the purge list keeps a deleted
    # repository alive in it (adhoc #196).
    scoped = source.split("_REPO_SCOPED_TABLES = (", 1)[1].split(")\n", 1)[0]
    for table in ("repo_webhooks", "repo_webhook_seeds",
                  "repo_webhook_sequence", "repo_webhook_outbox",
                  "repo_webhook_deliveries"):
        assert '("%s", "repo_bi")' % table in scoped, table
    # queue_event fails closed on a NameError, so the privacy gate it calls
    # has to be a real entry.py binding or no event would ever be delivered.
    assert "async def _repo_is_private(" in source
    # Startup memory: the module must be reached only through the proxy.
    footprint = (REPO / "world" / "tools"
                 / "build_worker_footprint.py").read_text("utf-8")
    assert '"repository_webhooks.py",' in footprint


def _free_names(tree):
    """Names a module reads without defining — its runtime contract."""
    import builtins
    bound = set(dir(builtins))
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            bound.add(node.name)
        elif isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store):
            bound.add(node.id)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                bound.add((alias.asname or alias.name).split(".")[0])
        elif isinstance(node, ast.arg):
            bound.add(node.arg)
    read = {node.id for node in ast.walk(tree)
            if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load)}
    return read - bound


def test_every_runtime_name_the_module_reads_is_one_entry_supplies():
    # _bind_runtime copies entry.py's globals in by name and silently skips
    # what is not there; a typo or a renamed helper would only surface as a
    # NameError on a live push. The module deliberately imports nothing from
    # entry, so this list IS the contract between them.
    module = ast.parse((ROOT / "src" / "repository_webhooks.py").read_text(
        "utf-8"))
    entry = ast.parse((ROOT / "src" / "entry.py").read_text("utf-8"))
    supplied = set()
    for node in entry.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            supplied.add(node.name)
        elif isinstance(node, ast.Assign):
            supplied.update(target.id for target in node.targets
                            if isinstance(target, ast.Name))
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                supplied.add((alias.asname or alias.name).split(".")[0])
    needed = _free_names(module)
    assert needed, "the module should depend on the runtime, not nothing"
    assert needed <= supplied, sorted(needed - supplied)
    # And the fake runtime in this file has to supply the same set, or these
    # tests would be exercising a shape production never sees.
    db = _db()
    _install(db)
    assert needed <= set(hooks.__dict__), sorted(
        needed - set(hooks.__dict__))


def test_the_three_event_points_queue_their_events():
    catalog = (ROOT / "src" / "catalog_routes.py").read_text("utf-8")
    assert '"repository.push"' in catalog and '"branch.push"' in catalog
    # Only a publication that actually moved refs may fire.
    assert catalog.index("catalog_mirror_update_needed(") < catalog.index(
        '"repository.push"')
    merges = (ROOT / "src" / "route_handlers.py").read_text("utf-8")
    assert '"pull.merged"' in merges
    assert 'if mutation == "merge":' in merges
    releases = (ROOT / "src" / "fediverse_routes.py").read_text("utf-8")
    assert '"release.published"' in releases
    assert 'kind == "release" and event_type == "publish"' in releases
    # A web-created repository is pushed to over plain git; its state is pinned
    # in hosted_git, not through a node's signed catalog publication, so a push
    # to one would otherwise be the one push that raises nothing.
    hosted = (ROOT / "src" / "hosted_git.py").read_text("utf-8")
    assert '"repository.push"' in hosted


def test_the_settings_tab_ships_the_webhook_surface_in_the_bundle():
    # Source-text contract for the client half: the dashboard's behaviour is a
    # single closure concatenated from ordered fragments, so a fragment edit
    # that was never rebuilt into public/dashboard.js ships nothing.
    public = ROOT / "public"
    fragments = public / "dashboard" / "js"
    detail = (fragments / "08-repo-detail-network.js").read_text("utf-8")
    content = (fragments / "06-repo-content.js").read_text("utf-8")
    explorer = (fragments / "05-repo-list-explorer.js").read_text("utf-8")
    bundle = (public / "dashboard.js").read_text("utf-8")

    # The card lives inside the owner-only Settings panel, never in the public
    # repository chrome.
    settings_panel = detail.split("const settingsPanel = ", 1)[1].split(
        "detail.innerHTML = ", 1)[0]
    assert "data-repo-webhooks-card" in settings_panel
    assert "data-repo-webhook-form" in settings_panel
    for attribute in ("data-repo-webhooks-list", "data-repo-webhook-url",
                      "data-repo-webhook-events", "data-repo-webhook-status",
                      "data-repo-webhook-secret-value"):
        assert attribute in settings_panel, attribute

    # Every action the handler answers is reachable from the page.
    for action in ("create", "update", "rotate", "delete", "test"):
        assert 'action: "%s"' % action in (detail + content), action
    assert "loadRepoWebhooks(state.selectedRepo)" in explorer
    # The secret is shown from the create/rotate response only; the page must
    # not pretend it can read one back.
    assert "showRepoWebhookSecret(body?.webhook?.secret" in detail
    assert "webhook.secret" not in content

    # Escaping: endpoint URLs and descriptions are owner-supplied text.
    render = content.split("function renderRepoWebhooks(", 1)[1].split(
        "\n  function ", 1)[0]
    assert "escapeHtml(hook.url)" in render
    assert "escapeHtml(hook.description)" in render

    for fragment in (detail, content, explorer):
        marker = fragment.split("\n", 1)[0]
        assert marker in bundle
    assert "data-repo-webhooks-card" in bundle
    assert "renderRepoWebhooks" in bundle


def test_the_drain_phase_is_published_on_the_schedule_it_runs():
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert 'phases.append("webhook-outbox")' in entry
    assert 'if phase == "webhook-outbox":' in entry
    activity = (ROOT / "src" / "cron_activity.py").read_text("utf-8")
    assert '("webhook-outbox", 5, (4,), "integrations",' in activity
