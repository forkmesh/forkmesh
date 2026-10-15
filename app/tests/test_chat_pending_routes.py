"""Side-effect-free pending-ciphertext reads behind chat push previews.

The phone's notification code fetches the still-encrypted frame a push named
and decrypts it on the device (docs/app/chat-push-previews.md). The read must
never count as receiving, reading or acknowledging the message.
"""

import asyncio
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = Path(__file__).resolve().parent
for path in (SRC, TESTS):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import chat_channels_api as channels_api  # noqa: E402
import chat_direct_messages_api as direct_api  # noqa: E402
import test_chat_channels_api as channel_tests  # noqa: E402
import test_chat_direct_messages_api as direct_tests  # noqa: E402


REF = "0123456789abcdef0123456789abcdef01234567"
OTHER_REF = "f" * 40
FRAME = '{"kind":"cipher","v":1,"iv":"i","data":"d","ref":"' + REF + '"}'
CHAT_HISTORY_SQL = (ROOT / "migrations" / "0008_chat_history.sql").read_text(
    encoding="utf-8")


def _snapshot(db, tables):
    return {
        table: [tuple(row) for row in db.execute(
            "SELECT * FROM " + table + " ORDER BY 1,2").fetchall()]
        for table in tables
    }


class DirectRuntime(direct_tests.FakeRuntime):
    def __init__(self):
        super().__init__()
        self.db.executescript(CHAT_HISTORY_SQL)
        self.passphrases = []

    async def passphrase(self, conversation_id, key_version):
        self.passphrases.append((conversation_id, key_version))
        return f"key:{conversation_id}:{key_version}"


class ChannelRuntime(channel_tests.FakeRuntime):
    def __init__(self):
        super().__init__()
        self.db.executescript(CHAT_HISTORY_SQL)


def _store(runtime, room, msg_id, body=FRAME):
    runtime.db.execute(
        "INSERT INTO chat_history (room_key,msg_id,ts,body) VALUES (?,?,?,?)",
        (room, msg_id, 1, body))
    runtime.db.commit()


async def _direct_pending(runtime, actor, conversation_id, ref=REF,
                          method="GET"):
    return await direct_api.handle(
        runtime.use(method, actor, query={"ref": ref}),
        f"/api/chat/direct-messages/{conversation_id}/pending",
    )


async def _channel_pending(runtime, actor, channel_id, ref=REF,
                           method="GET"):
    return await channels_api.handle(
        runtime.use(method, actor, query={"ref": ref}),
        f"/api/chat/channels/{channel_id}/pending",
    )


async def _direct_setup():
    runtime = DirectRuntime()
    created = await direct_tests.start_dm(runtime, "alice", "bob")
    conversation_id = created["data"]["conversation"]["id"]
    room = f"chat-direct:{conversation_id}:v1"
    _store(runtime, room, REF + ":" + "a" * 16 + ":n")
    _store(runtime, room, OTHER_REF + ":" + "b" * 16, body="other")
    runtime.db.execute(
        "UPDATE chat_direct_conversations SET message_count=3")
    runtime.db.commit()
    runtime.account_events.clear()
    return runtime, conversation_id, room


def test_a_participant_reads_the_pending_direct_frame():
    async def run():
        runtime, conversation_id, room = await _direct_setup()
        tables = ("chat_history", "chat_direct_conversations",
                  "chat_direct_participants")
        before = _snapshot(runtime.db, tables)
        response = await _direct_pending(runtime, "bob", conversation_id)
        assert response["status"] == 200
        assert response["cache_control"].startswith("no-store")
        assert response["data"] == {
            "conversation": "direct:" + conversation_id,
            "room": room,
            "keyVersion": 1,
            "passphrase": f"key:{conversation_id}:1",
            "frame": FRAME,
        }
        upper = await _direct_pending(
            runtime, "alice", conversation_id, ref=REF.upper())
        assert upper["data"]["frame"] == FRAME
        assert _snapshot(runtime.db, tables) == before
        assert runtime.read_announcements == []
        assert runtime.account_events == []
        assert runtime.room_access_calls == []
    asyncio.run(run())


def test_direct_pending_is_participant_only_and_404s_a_gone_row():
    async def run():
        runtime, conversation_id, room = await _direct_setup()
        for actor, target in (("carol", conversation_id),
                              ("admin", conversation_id),
                              ("bob", "e" * 32)):
            denied = await _direct_pending(runtime, actor, target)
            assert denied["status"] == 404
            assert denied["data"] == {"error": "not_found"}
        gone = await _direct_pending(
            runtime, "bob", conversation_id, ref="c" * 40)
        assert gone["status"] == 404
        assert gone["data"] == {"error": "not_found"}
        runtime.db.execute("DELETE FROM chat_history WHERE room_key=?",
                           (room,))
        received = await _direct_pending(runtime, "bob", conversation_id)
        assert received["status"] == 404
        assert runtime.passphrases == []
        for bad in ("", "xyz", REF + "0", REF[:-1] + "%"):
            invalid = await _direct_pending(
                runtime, "bob", conversation_id, ref=bad)
            assert invalid["status"] == 400
            assert invalid["data"] == {"error": "invalid_ref"}
        unauthenticated = await _direct_pending(
            runtime, "nobody", conversation_id)
        assert unauthenticated["status"] == 401
        posted = await _direct_pending(
            runtime, "bob", conversation_id, method="POST")
        assert posted["status"] == 405
        assert posted["headers"]["allow"] == "GET"
    asyncio.run(run())


def test_direct_pending_reads_only_the_current_key_version():
    async def run():
        runtime, conversation_id, _room = await _direct_setup()
        runtime.db.execute(
            "UPDATE chat_direct_conversations SET key_version=2")
        runtime.db.commit()
        missing = await _direct_pending(runtime, "bob", conversation_id)
        assert missing["status"] == 404
        _store(runtime, f"chat-direct:{conversation_id}:v2", REF + ":x")
        found = await _direct_pending(runtime, "bob", conversation_id)
        assert found["data"]["keyVersion"] == 2
        assert found["data"]["room"] == f"chat-direct:{conversation_id}:v2"
    asyncio.run(run())


async def _channel_setup(visibility="private"):
    runtime = ChannelRuntime()
    created = await channel_tests.create_channel(
        runtime, "design", visibility=visibility, members=(
            ["alice"] if visibility == "private" else None))
    channel_id = created["data"]["channel"]["id"]
    room = f"chat-channel:{channel_id}:v1"
    _store(runtime, room, REF)
    _store(runtime, room, OTHER_REF + ":" + "a" * 16, body="other")
    runtime.account_events.clear()
    return runtime, channel_id, room


def test_a_member_reads_the_pending_channel_frame_without_marking_it_read():
    async def run():
        runtime, channel_id, room = await _channel_setup()
        tables = ("chat_history", "chat_channel_reads")
        before = _snapshot(runtime.db, tables)
        response = await _channel_pending(runtime, "alice", channel_id)
        assert response["status"] == 200
        assert response["cache_control"].startswith("no-store")
        assert response["data"] == {
            "channel": channel_id,
            "room": room,
            "keyVersion": 1,
            "passphrase": f"key:{channel_id}:1",
            "frame": FRAME,
        }
        assert _snapshot(runtime.db, tables) == before
        assert runtime.account_events == []
        assert runtime.revoked_rooms == []
    asyncio.run(run())


def test_channel_pending_follows_the_readable_channel_rule():
    async def run():
        runtime, channel_id, _room = await _channel_setup()
        for actor, target in (("bob", channel_id), ("alice", "e" * 32)):
            denied = await _channel_pending(runtime, actor, target)
            assert denied["status"] == 404
            assert denied["data"] == {"error": "not_found"}
        runtime.admins.add("bob")
        assert (await _channel_pending(
            runtime, "bob", channel_id))["status"] == 404
        gone = await _channel_pending(
            runtime, "alice", channel_id, ref="c" * 40)
        assert gone["status"] == 404
        assert gone["data"] == {"error": "not_found"}
        prefixed = await _channel_pending(
            runtime, "alice", channel_id, ref=OTHER_REF)
        assert prefixed["status"] == 404
        invalid = await _channel_pending(
            runtime, "alice", channel_id, ref="nothex")
        assert invalid["status"] == 400
        posted = await _channel_pending(
            runtime, "alice", channel_id, method="POST")
        assert posted["status"] == 405
        unauthenticated = await _channel_pending(
            runtime, "nobody", channel_id)
        assert unauthenticated["status"] == 401

        public, public_id, _ = await _channel_setup(visibility="public")
        assert (await _channel_pending(
            public, "bob", public_id))["data"]["frame"] == FRAME
    asyncio.run(run())


def test_pending_routes_are_wired_and_their_paths_redacted():
    import re
    urls = (SRC / "urls.py").read_text(encoding="utf-8")
    assert ('CHAT_DIRECT_MESSAGE_PENDING_RE = re.compile(\n'
            '    r"^/api/chat/direct-messages/([0-9a-f]{32})/pending/?$")'
            in urls)
    assert ('CHAT_CHANNEL_PENDING_RE = re.compile(\n'
            '    r"^/api/chat/channels/([0-9a-f]{32})/pending/?$")' in urls)
    router = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert "or CHAT_DIRECT_MESSAGE_PENDING_RE.match(url.path)" in router
    assert "or CHAT_CHANNEL_PENDING_RE.match(url.path)" in router
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    exports = entry[entry.index("_URL_EXPORT_NAMES = ("):]
    exports = exports[:exports.index(")")]
    assert '"CHAT_DIRECT_MESSAGE_PENDING_RE"' in exports
    assert '"CHAT_CHANNEL_PENDING_RE"' in exports
    runtime = entry[entry.index("class _ChatDirectMessagesRuntime("):]
    runtime = runtime[:runtime.index("\nasync def ")]
    assert "async def passphrase(self, conversation_id, key_version):" in (
        runtime)
    redact = entry[entry.index("async def _privacy_safe_log_path"):]
    redact = redact[:redact.index("    match = REPO_API_PREFIX_RE")]
    for prefix in ("/api/chat/channels/", "/api/chat/direct-messages/"):
        assert re.search(
            r'if path\.startswith\("' + re.escape(prefix) + r'"\):\n'
            r'        return "' + re.escape(prefix) + r'\[opaque\]"', redact)


async def _direct_delivered(runtime, actor, conversation_id, ref=REF,
                            method="POST"):
    return await direct_api.handle(
        runtime.use(method, actor, query={"ref": ref}),
        f"/api/chat/direct-messages/{conversation_id}/delivered",
    )


def test_a_push_delivery_receipt_tells_the_sender_and_keeps_the_copy():
    async def run():
        runtime, conversation_id, _room = await _direct_setup()
        tables = ("chat_history", "chat_direct_conversations",
                  "chat_direct_participants")
        before = _snapshot(runtime.db, tables)
        response = await _direct_delivered(runtime, "bob", conversation_id)
        assert response["status"] == 200
        assert response["data"] == {"ok": True}
        assert runtime.delivered_announcements == [
            (conversation_id, 1, "bi:bob", REF)]
        assert _snapshot(runtime.db, tables) == before
        assert runtime.read_announcements == []
        assert runtime.account_events == []
        assert runtime.passphrases == []
    asyncio.run(run())


def test_a_delivery_receipt_needs_a_waiting_frame_from_the_other_person():
    async def run():
        runtime, conversation_id, room = await _direct_setup()
        _store(runtime, room, "d" * 40 + ":" + "bi:bob")
        own = await _direct_delivered(
            runtime, "bob", conversation_id, ref="d" * 40)
        assert own["status"] == 404
        gone = await _direct_delivered(
            runtime, "bob", conversation_id, ref="c" * 40)
        assert gone["status"] == 404
        for actor, target in (("carol", conversation_id),
                              ("admin", conversation_id),
                              ("bob", "e" * 32)):
            denied = await _direct_delivered(runtime, actor, target)
            assert denied["status"] == 404
        for bad in ("", "xyz", REF + "0"):
            invalid = await _direct_delivered(
                runtime, "bob", conversation_id, ref=bad)
            assert invalid["status"] == 400
            assert invalid["data"] == {"error": "invalid_ref"}
        unauthenticated = await _direct_delivered(
            runtime, "nobody", conversation_id)
        assert unauthenticated["status"] == 401
        fetched = await _direct_delivered(
            runtime, "bob", conversation_id, method="GET")
        assert fetched["status"] == 405
        assert fetched["headers"]["allow"] == "POST"
        assert runtime.delivered_announcements == []
    asyncio.run(run())
