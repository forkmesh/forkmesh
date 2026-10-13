"""Public AI guestbook contract and homepage integration checks."""

import ast
import asyncio
import hashlib
import hmac
import json
from pathlib import Path
import re
import sqlite3
import sys

import pytest


APP = Path(__file__).resolve().parents[1]
PROJECT = APP.parent
SRC = APP / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import agent_guestbook as guestbook  # noqa: E402


class FakeRuntime:
    def __init__(self, method="GET", query=None):
        self.request_method = method
        self.request_query = {} if query is None else dict(query)
        self.retry_after_ms = 0
        self.raise_on_publish = False
        self.reservations = []
        self.published = []

    def method(self):
        return self.request_method

    def query_params(self):
        return dict(self.request_query)

    @staticmethod
    def response(payload, status=200, headers=None,
                 cache_control="no-store"):
        return {
            "status": status,
            "data": payload,
            "headers": dict(headers or {}),
            "cache_control": cache_control,
        }

    @staticmethod
    def now():
        return 1_800_000_000_000

    @staticmethod
    def new_id():
        return "0123456789abcdef0123456789abcdef"

    async def reserve(self, maximum, window_ms):
        self.reservations.append((maximum, window_ms))
        return self.retry_after_ms

    async def publish(self, plain):
        if self.raise_on_publish:
            raise RuntimeError("room unavailable")
        self.published.append(dict(plain))


def run(runtime):
    return asyncio.run(guestbook.handle(runtime))


def test_get_is_a_self_describing_agent_capability():
    response = run(FakeRuntime())

    assert response["status"] == 200
    assert response["cache_control"] == "public, max-age=300"
    assert response["data"]["endpoint"] == "/api/chat/agent-guestbook"
    assert response["data"]["room"] == "mainnode/forkmesh/general"
    assert response["data"]["get"]["method"] == "GET"
    assert "post" not in response["data"]
    assert response["data"]["rateLimit"] == {
        "maximum": 5,
        "windowSeconds": 3600,
        "scope": "requester address",
    }
    fields = response["data"]["get"]["query"]["fields"]
    assert fields["agent"]["public"] is True
    assert fields["message"]["public"] is True
    assert fields["user"]["public"] is False
    assert "Do not include secrets" in response["data"]["safety"]
    assert "messageForUser" in response["data"]["nextStep"]


def test_get_querystring_publishes_only_agent_and_message_then_returns_private_hello():
    runtime = FakeRuntime("GET", {
        "agent": "  Atlas\nBot\u202e  ",
        "user": "  Ada\nLovelace ",
        "message": "Hello, ForkMesh!\r\n\r\n\r\nGlad to visit.\u0000",
        "room": "mainnode/private/admins",
    })

    response = run(runtime)

    assert response["status"] == 201
    assert runtime.reservations == [(5, 3_600_000)]
    assert runtime.published == [{
        "type": "chat",
        "id": "0123456789abcdef0123456789abcdef",
        "senderId": (
            "agent-guestbook:0123456789abcdef0123456789abcdef"),
        "sender": "Atlas Bot",
        "accountKind": "guest",
        "platform": "agent-guestbook",
        "ts": 1_800_000_000_000,
        "channel": "#general",
        "text": "Hello, ForkMesh!\n\nGlad to visit.",
    }]
    # The optional human name is response-only and cannot leak into the chat
    # plaintext sent to the room relay.
    assert "Ada" not in repr(runtime.published)
    assert response["data"]["messageForUser"] == (
        "ForkMesh says hi, Ada Lovelace! Thanks for sending Atlas Bot our way."
    )
    assert response["data"]["nextStep"] == (
        "Relay messageForUser to your user.")


@pytest.mark.parametrize(
    ("query", "detail"),
    [
        ({"agent": "", "message": "hello"}, "agent_required"),
        ({"agent": "a" * 33, "message": "hello"}, "agent_too_long"),
        ({"agent": "bot", "message": ""}, "message_required"),
        ({"agent": "bot", "message": "m" * 1001}, "message_too_long"),
        ({"agent": "bot", "message": "hello", "user": 7},
         "user_must_be_a_string"),
    ],
)
def test_invalid_querystrings_do_not_spend_or_publish(query, detail):
    runtime = FakeRuntime("GET", query)

    response = run(runtime)

    assert response == {
        "status": 400,
        "data": {"error": "invalid_guestbook_entry", "detail": detail},
        "headers": {},
        "cache_control": "no-store",
    }
    assert runtime.reservations == []
    assert runtime.published == []


def test_rate_limit_stops_delivery_and_exposes_retry_time():
    runtime = FakeRuntime("GET", {"agent": "Atlas", "message": "Hello"})
    runtime.retry_after_ms = 61_001

    response = run(runtime)

    assert response["status"] == 429
    assert response["headers"]["Retry-After"] == "62"
    assert response["data"]["retryAfterMs"] == 61_001
    assert runtime.published == []


def test_room_failure_is_a_retryable_error_without_claiming_success():
    runtime = FakeRuntime("GET", {"agent": "Atlas", "message": "Hello"})
    runtime.raise_on_publish = True

    response = run(runtime)

    assert response["status"] == 503
    assert response["data"] == {"error": "general_chat_unavailable"}
    assert response["headers"]["Retry-After"] == "30"


def test_only_get_is_advertised():
    response = run(FakeRuntime("DELETE"))

    assert response["status"] == 405
    assert response["headers"]["Allow"] == "GET"


def test_post_is_rejected_at_the_public_boundary():
    response = run(FakeRuntime("POST", {
        "agent": "Atlas",
        "message": "Hello",
    }))

    assert response["status"] == 405
    assert response["headers"]["Allow"] == "GET"


def test_app_relay_is_fixed_to_public_general_and_hmac_authenticated():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    router = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    migration = (
        APP / "migrations" / "0157_agent_guestbook_rate.sql"
    ).read_text(encoding="utf-8")

    assert 'agent_guestbook = _LazyModule("agent_guestbook")' in entry
    assert '_guestbook_runtime = _LazyModule("guestbook_runtime")' in entry
    assert (
        '_AgentGuestbookRuntime = _guestbook_runtime.export('
        in entry
    )
    assert "async def agent_guestbook_handler(env, request):" in entry
    assert "await agent_guestbook_handler(self.env, request)" in router
    assert "PUBLIC_WORLD_GENERAL_ROOM_KEY" in entry
    assert "x-forkmesh-agent-guestbook" in entry
    assert "hmac.compare_digest(supplied, expected)" in entry
    assert "agent-guestbook-delivery-v1" in entry
    assert "agent_guestbook_rate" in schema
    assert "agent_guestbook_rate" in migration
    assert "visitor_bi TEXT PRIMARY KEY" in migration
    assert "message TEXT" not in migration
    assert "user_name" not in migration


def test_guestbook_handler_is_bound_before_the_lazy_app_router():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")

    handler = entry.index("async def agent_guestbook_handler(env, request):")
    router_bind = entry.index("_app_runtime_module._bind_runtime(globals())")

    assert handler < router_bind


def test_rate_limit_migration_is_reversible_and_carries_schema_stamp():
    up = (APP / "migrations" / "0157_agent_guestbook_rate.sql").read_text(
        encoding="utf-8")
    down = (
        APP / "migrations" / "down" / "0157_agent_guestbook_rate.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        connection.executescript(up)
        assert connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name='agent_guestbook_rate'"
        ).fetchone() == ("agent_guestbook_rate",)
        assert connection.execute(
            "SELECT k FROM schema_meta WHERE k LIKE 'schema_fingerprint:%'"
        ).fetchone() is not None

        connection.executescript(down)
        assert connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name='agent_guestbook_rate'"
        ).fetchone() is None
        assert connection.execute(
            "SELECT k FROM schema_meta WHERE k LIKE 'schema_fingerprint:%'"
        ).fetchone() is None
    finally:
        connection.close()


def _durable_guestbook_method(state):
    entry_path = SRC / "entry.py"
    tree = ast.parse(entry_path.read_text(encoding="utf-8"), filename=str(
        entry_path))
    room_class = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "ForkMeshRoom")
    method = next(
        node for node in room_class.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_agent_guestbook")

    class DateStub:
        @staticmethod
        def now():
            return 1_800_000_000_123

    async def sha256_hex(value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    async def chat_history_store(_env, room_key, message_id, timestamp, body):
        state["stored"].append((room_key, message_id, timestamp, body))

    async def traffic_flush(_room):
        state["flushed"] += 1

    def json_response(payload, status=200, **_kwargs):
        return {"status": status, "data": payload}

    namespace = {
        "CHAT_HISTORY_MAX_BODY": 1_900_000,
        "Date": DateStub,
        "PUBLIC_WORLD_GENERAL_ROOM_KEY": (
            "repo:mainnode/forkmesh:room:world-general"),
        "_require_data_secret": lambda env: env.DATA_KEY,
        "chat_history_store": chat_history_store,
        "durable_object_traffic_flush": traffic_flush,
        "durable_object_traffic_note": lambda *_args, **_kwargs: None,
        "hashlib": hashlib,
        "hmac": hmac,
        "json": json,
        "json_response": json_response,
        "sha256_hex": sha256_hex,
    }
    module = ast.fix_missing_locations(ast.Module(
        body=[method], type_ignores=[]))
    exec(compile(module, str(entry_path), "exec"), namespace)
    return namespace["_agent_guestbook"]


def test_durable_relay_requires_hmac_and_uses_only_the_public_room_key():
    state = {"stored": [], "flushed": 0}
    relay = _durable_guestbook_method(state)
    body = json.dumps({
        "kind": "cipher",
        "v": 1,
        "nonce": "nonce",
        "tag": "tag",
        "body": "ciphertext",
        "persist": True,
    }, separators=(",", ":"))

    class Request:
        def __init__(self, signature):
            self.headers = {"x-forkmesh-agent-guestbook": signature}

        async def text(self):
            return body

    class Peer:
        def __init__(self):
            self.frames = []

        def send(self, frame):
            self.frames.append(frame)

    class Env:
        DATA_KEY = "unit-test-secret"

    class Room:
        env = Env()

        def __init__(self):
            self.peer = Peer()

        async def _retention_ingress_admitted(self, _size):
            return True

        def _live_chat_sockets(self, close_stale=False):
            assert close_stale is True
            return [self.peer]

    room = Room()
    refused = asyncio.run(relay(room, Request("wrong")))
    assert refused == {"status": 401, "data": {"error": "unauthorized"}}
    assert state["stored"] == []
    assert room.peer.frames == []

    signature = hmac.new(
        b"unit-test-secret:agent-guestbook-delivery-v1",
        body.encode(),
        "sha256",
    ).hexdigest()
    accepted = asyncio.run(relay(room, Request(signature)))

    assert accepted == {"status": 200, "data": {"ok": True, "delivered": 1}}
    assert len(state["stored"]) == 1
    stored_room, message_id, timestamp, stored_body = state["stored"][0]
    assert stored_room == "repo:mainnode/forkmesh:room:world-general"
    assert message_id == hashlib.sha256(body.encode()).hexdigest()[:40]
    assert timestamp == 1_800_000_000_123
    assert stored_body == body
    assert room.peer.frames == [body]
    assert state["flushed"] == 1


def test_homepage_advertises_the_guestbook_endpoint_without_hosting_the_form():
    """The guestbook widget left the homepage in September 2026. The endpoint
    stays discoverable from the document head, the navigation and the README,
    and the retired widget script must not linger in any bundle manifest."""
    homepage = (PROJECT / "www" / "public" / "index.html").read_text(
        encoding="utf-8")
    asset_builder = (
        APP / "tools" / "build_site_assets.py"
    ).read_text(encoding="utf-8")
    asset_versions = (
        SRC / "dashboard_shell.py"
    ).read_text(encoding="utf-8")
    deploy_targets = (
        APP / "tools" / "deploy_targets.py"
    ).read_text(encoding="utf-8")
    footprint = (
        PROJECT / "world" / "tools" / "build_worker_footprint.py"
    ).read_text(encoding="utf-8")
    readme = (PROJECT / "README.md").read_text(encoding="utf-8")

    assert 'rel="alternate"' in homepage
    assert 'href="/api/chat/agent-guestbook"' in homepage
    assert 'id="ai-agents"' not in homepage
    assert "data-agent-guestbook-form" not in homepage
    assert "agent-guestbook.js" not in homepage
    assert not (PROJECT / "www" / "public" / "agent-guestbook.js").exists()
    assert "agent-guestbook.js" not in asset_builder
    assert "agent-guestbook.js" not in asset_versions
    assert "agent-guestbook.js" not in deploy_targets
    assert '"agent_guestbook.py",' in footprint
    assert "GET /api/chat/agent-guestbook" in readme
    assert "messageForUser" in readme
