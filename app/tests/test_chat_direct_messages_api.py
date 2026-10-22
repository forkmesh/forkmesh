"""Executable persistence and authorization tests for direct messages."""

import asyncio
import hashlib
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import chat_direct_messages_api as api  # noqa: E402


def run_async_test(function):
    def wrapper():
        return asyncio.run(function())

    wrapper.__name__ = function.__name__
    return wrapper


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0079_chat_direct_messages.sql")
            .read_text(encoding="utf-8")
        )
        self.db.executescript(
            (ROOT / "migrations" / "0084_chat_direct_message_limits.sql")
            .read_text(encoding="utf-8")
        )
        self.db.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        self.db.executescript(
            (ROOT / "migrations" / "0286_chat_blocks.sql")
            .read_text(encoding="utf-8")
        )
        self.actor = ""
        self.request_method = "GET"
        self.request_data = {}
        self.request_query = {}
        self.clock = 1_800_000_000_000
        self.accounts = {"admin", "alice", "bob", "carol"}
        self.private_accounts = set()
        self.ids = 0
        self.fail_batch_at = None
        self.room_access_calls = []
        self.account_events = []
        self.read_announcements = []
        self.delivered_announcements = []

    async def announce_read(self, conversation_id, key_version, account_bi):
        self.read_announcements.append(
            (conversation_id, key_version, account_bi))

    async def announce_delivered(
            self, conversation_id, key_version, account_bi, ref):
        self.delivered_announcements.append(
            (conversation_id, key_version, account_bi, ref))

    async def notify_account(self, owner, topic, started_by=""):
        # Payload-free push to one account's clients (adhoc #1604).
        self.account_events.append((owner, topic))
        self.started_by = started_by

    def use(self, method, actor="", data=None, query=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        self.request_query = {} if query is None else dict(query)
        return self

    def method(self):
        return self.request_method

    def query_params(self):
        return dict(self.request_query)

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        if not isinstance(self.request_data, dict):
            return None, "invalid_json"
        return self.request_data, ""

    async def session(self, _data=None):
        if self.actor not in self.accounts:
            return "", None
        return f"bi:{self.actor}", {
            "name": self.actor,
            "status": "active",
        }

    async def account(self, name):
        normalized = str(name or "").strip().lower()
        if normalized not in self.accounts:
            return "", ""
        return f"bi:{normalized}", normalized

    async def search_accounts(self, query, limit):
        query = str(query or "").strip().lower()
        matches = [
            name for name in self.accounts
            if query in name and name != self.actor
        ]
        matches.sort(key=lambda name: (
            0 if name == query else 1,
            0 if name.startswith(query) else 1,
            name,
        ))
        return matches[:limit]

    async def blind(self, value):
        digest = hashlib.sha256(str(value).encode()).hexdigest()
        return "blind:" + digest

    async def seal(self, value):
        return json.dumps(value, sort_keys=True)

    async def open(self, value):
        return json.loads(value)

    async def room_access(self, conversation_id, key_version, account_bi):
        self.room_access_calls.append(
            (conversation_id, key_version, account_bi)
        )
        return {
            "room": f"chat-direct:{conversation_id}:v{key_version}",
            "passphrase": f"key:{conversation_id}:{key_version}",
            "webSocketUrl": (
                f"/api/chat/direct-messages/{conversation_id}/ws?ticket="
                f"ticket:{account_bi}"
            ),
        }

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def batch(self, statements):
        self.db.execute("BEGIN")
        try:
            for index, (sql, args) in enumerate(statements):
                if self.fail_batch_at == index:
                    raise RuntimeError("forced_batch_failure")
                self.db.execute(sql, args)
            self.db.commit()
        except Exception:
            self.db.rollback()
            raise

    def scalar(self, sql):
        return self.db.execute(sql).fetchone()[0]


async def start_dm(runtime, actor, username):
    return await api.handle(
        runtime.use("POST", actor, {"username": username}),
        "/api/chat/direct-messages",
    )


async def list_dms(runtime, actor):
    return await api.handle(
        runtime.use("GET", actor),
        "/api/chat/direct-messages",
    )


async def room_access(runtime, actor, conversation_id):
    return await api.handle(
        runtime.use("GET", actor),
        f"/api/chat/direct-messages/{conversation_id}/room-access",
    )


async def mark_read(runtime, actor, conversation_id):
    return await api.handle(
        runtime.use("POST", actor),
        f"/api/chat/direct-messages/{conversation_id}/read",
    )


@run_async_test
async def test_pair_creation_is_atomic_and_reverse_order_is_idempotent():
    runtime = FakeRuntime()
    created = await start_dm(runtime, "alice", "bob")
    reopened = await start_dm(runtime, "bob", "alice")

    assert created["status"] == 201
    assert reopened["status"] == 200
    assert reopened["data"]["conversation"]["id"] == (
        created["data"]["conversation"]["id"]
    )
    assert created["data"]["conversation"]["otherUser"] == "bob"
    assert reopened["data"]["conversation"]["otherUser"] == "alice"
    assert runtime.scalar(
        "SELECT COUNT(*) FROM chat_direct_conversations"
    ) == 1
    assert runtime.scalar(
        "SELECT COUNT(*) FROM chat_direct_participants"
    ) == 2
    # bob got a conversation he did not open, and his chat page reads that list
    # once when it opens rather than every 30s, so he is told (adhoc #1604).
    # Reopening an existing pair creates nothing and pushes nothing.
    assert runtime.account_events == [("bob", "direct-messages")]
    # ...and his phone banner says who opened it.
    assert runtime.started_by == "alice"


@run_async_test
async def test_pair_creation_rolls_back_every_row_when_batch_fails():
    runtime = FakeRuntime()
    runtime.fail_batch_at = 1

    try:
        await start_dm(runtime, "alice", "bob")
    except RuntimeError as error:
        assert str(error) == "forced_batch_failure"
    else:
        raise AssertionError("batch failure must escape the API boundary")

    assert runtime.scalar(
        "SELECT COUNT(*) FROM chat_direct_conversations"
    ) == 0
    assert runtime.scalar(
        "SELECT COUNT(*) FROM chat_direct_participants"
    ) == 0


@run_async_test
async def test_only_participants_list_and_access_a_direct_message():
    runtime = FakeRuntime()
    created = await start_dm(runtime, "alice", "bob")
    conversation_id = created["data"]["conversation"]["id"]

    alice = await list_dms(runtime, "alice")
    bob = await list_dms(runtime, "bob")
    admin = await list_dms(runtime, "admin")
    assert alice["data"]["conversations"] == [{
        "id": conversation_id,
        "otherUser": "bob",
        "updatedAt": runtime.clock,
        "keyVersion": 1,
        "unreadCount": 0,
    }]
    assert bob["data"]["conversations"][0]["otherUser"] == "alice"
    assert admin["data"]["conversations"] == []

    allowed = await room_access(runtime, "alice", conversation_id)
    denied = await room_access(runtime, "admin", conversation_id)
    missing = await room_access(runtime, "admin", "f" * 32)
    assert allowed["status"] == 200
    assert allowed["data"]["conversation"]["otherUser"] == "bob"
    assert allowed["cache_control"].startswith("no-store")
    assert denied["status"] == missing["status"] == 404
    assert denied["data"] == missing["data"] == {"error": "not_found"}
    assert runtime.room_access_calls == [(conversation_id, 1, "bi:alice")]


@run_async_test
async def test_self_missing_and_invalid_sessions_are_rejected():
    runtime = FakeRuntime()

    self_message = await start_dm(runtime, "alice", "alice")
    missing_user = await start_dm(runtime, "alice", "missing")
    unauthenticated = await list_dms(runtime, "guest")

    assert self_message["status"] == 400
    assert self_message["data"] == {"error": "cannot_message_self"}
    assert missing_user["status"] == 404
    assert missing_user["data"] == {"error": "user_not_found"}
    assert unauthenticated["status"] == 401
    assert unauthenticated["data"] == {"error": "invalid_session"}
    assert runtime.scalar(
        "SELECT COUNT(*) FROM chat_direct_conversations"
    ) == 0


@run_async_test
async def test_collection_validates_json_and_methods():
    runtime = FakeRuntime()
    invalid_json = await api.handle(
        runtime.use("POST", "alice", []),
        "/api/chat/direct-messages",
    )
    wrong_method = await api.handle(
        runtime.use("DELETE", "alice"),
        "/api/chat/direct-messages",
    )
    wrong_resource_method = await api.handle(
        runtime.use("POST", "alice", {"username": "bob"}),
        f"/api/chat/direct-messages/{'a' * 32}/room-access",
    )

    assert invalid_json["status"] == 400
    assert invalid_json["data"] == {"error": "invalid_json"}
    assert wrong_method["status"] == 405
    assert wrong_method["headers"]["allow"] == "GET, POST"
    assert wrong_resource_method["status"] == 405
    assert wrong_resource_method["headers"]["allow"] == "GET"


@run_async_test
async def test_authenticated_search_finds_private_active_users():
    runtime = FakeRuntime()
    runtime.private_accounts.add("bob")

    found = await api.handle(
        runtime.use("GET", "alice", query={"query": "bob"}),
        "/api/chat/direct-messages/users",
    )
    unauthenticated = await api.handle(
        runtime.use("GET", "guest", query={"query": "bob"}),
        "/api/chat/direct-messages/users",
    )

    assert found["status"] == 200
    assert found["data"] == {"users": [{"name": "bob"}]}
    assert unauthenticated["status"] == 401


@run_async_test
async def test_conversation_listing_is_cursor_paginated():
    runtime = FakeRuntime()
    runtime.accounts.update({"dave", "erin"})
    await start_dm(runtime, "alice", "bob")
    runtime.clock += 1
    await start_dm(runtime, "alice", "carol")
    runtime.clock += 1
    await start_dm(runtime, "alice", "dave")

    first = await api.handle(
        runtime.use("GET", "alice", query={"limit": "2"}),
        "/api/chat/direct-messages",
    )
    second = await api.handle(
        runtime.use(
            "GET",
            "alice",
            query={"limit": "2", "cursor": first["data"]["nextCursor"]},
        ),
        "/api/chat/direct-messages",
    )

    assert [
        item["otherUser"] for item in first["data"]["conversations"]
    ] == ["dave", "carol"]
    assert first["data"]["nextCursor"]
    assert [
        item["otherUser"] for item in second["data"]["conversations"]
    ] == ["bob"]
    assert second["data"]["nextCursor"] == ""


@run_async_test
async def test_new_conversation_creation_is_rate_limited_but_reopen_is_not():
    runtime = FakeRuntime()
    targets = [f"user{index}" for index in range(
        api.DIRECT_MESSAGE_CREATE_MAX_PER_WINDOW + 1
    )]
    runtime.accounts.update(targets)

    first = None
    for target in targets[:-1]:
        response = await start_dm(runtime, "alice", target)
        assert response["status"] == 201
        first = first or response

    blocked = await start_dm(runtime, "alice", targets[-1])
    reopened = await start_dm(runtime, "alice", targets[0])
    runtime.clock += api.DIRECT_MESSAGE_CREATE_WINDOW_MS
    created_later = await start_dm(runtime, "alice", targets[-1])

    assert blocked["status"] == 429
    assert blocked["data"]["error"] == "rate_limited"
    assert blocked["data"]["retryAfterMs"] > 0
    assert reopened["status"] == 200
    assert reopened["data"]["conversation"]["id"] == (
        first["data"]["conversation"]["id"]
    )
    assert created_later["status"] == 201


@run_async_test
async def test_unread_count_is_durable_and_marked_read_by_participant():
    runtime = FakeRuntime()
    created = await start_dm(runtime, "alice", "bob")
    conversation_id = created["data"]["conversation"]["id"]
    runtime.db.execute(
        "UPDATE chat_direct_conversations "
        "SET message_count=3,updated_at=? WHERE conversation_id=?",
        (runtime.clock + 10, conversation_id),
    )
    runtime.db.execute(
        "UPDATE chat_direct_participants SET last_read_count=3 "
        "WHERE conversation_id=? AND participant_bi=?",
        (conversation_id, "bi:alice"),
    )
    runtime.db.commit()

    before = await list_dms(runtime, "bob")
    marked = await mark_read(runtime, "bob", conversation_id)
    after = await list_dms(runtime, "bob")

    assert before["data"]["conversations"][0]["unreadCount"] == 3
    assert marked["status"] == 200
    assert after["data"]["conversations"][0]["unreadCount"] == 0
    assert runtime.read_announcements == [(conversation_id, 1, "bi:bob")]


@run_async_test
async def test_reading_announces_to_the_sender_only_when_the_cursor_moves():
    runtime = FakeRuntime()
    created = await start_dm(runtime, "alice", "bob")
    conversation_id = created["data"]["conversation"]["id"]

    await mark_read(runtime, "bob", conversation_id)
    await room_access(runtime, "bob", conversation_id)
    assert runtime.read_announcements == []

    runtime.db.execute(
        "UPDATE chat_direct_conversations SET message_count=1 "
        "WHERE conversation_id=?",
        (conversation_id,),
    )
    runtime.db.commit()
    accessed = await room_access(runtime, "bob", conversation_id)
    await mark_read(runtime, "bob", conversation_id)

    assert accessed["status"] == 200
    assert runtime.read_announcements == [(conversation_id, 1, "bi:bob")]


@run_async_test
async def test_message_action_reuses_one_conversation_per_pair():
    runtime = FakeRuntime()
    first = await start_dm(runtime, "alice", "bob")
    conversation_id = first["data"]["conversation"]["id"]
    for actor, target in (
        ("alice", "bob"), ("alice", "Bob"), ("bob", "alice"),
        ("alice", " bob "),
    ):
        again = await start_dm(runtime, actor, target)
        assert again["status"] == 200
        assert again["data"]["conversation"]["id"] == conversation_id
    assert runtime.scalar(
        "SELECT COUNT(*) FROM chat_direct_conversations") == 1
    assert (await list_dms(runtime, "alice"))["data"]["conversations"][0][
        "id"] == conversation_id


@run_async_test
async def test_non_participant_gets_not_found_from_every_direct_endpoint():
    runtime = FakeRuntime()
    created = await start_dm(runtime, "alice", "bob")
    conversation_id = created["data"]["conversation"]["id"]
    for call in (room_access, mark_read):
        hidden = await call(runtime, "admin", conversation_id)
        absent = await call(runtime, "admin", "f" * 32)
        assert hidden["status"] == absent["status"] == 404
        assert hidden["data"] == absent["data"] == {"error": "not_found"}
    listed = await list_dms(runtime, "admin")
    assert conversation_id not in str(listed["data"])
