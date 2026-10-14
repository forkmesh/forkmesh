#!/usr/bin/env python3
"""Direct-message key, ticket, socket, and live-admission contracts."""

import ast
import asyncio
import hashlib
import hmac
import re
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
FUNCTIONS = {
    "_chat_direct_passphrase",
    "_chat_direct_message_retained",
    "_chat_direct_message_recipient",
    "_chat_direct_message_parties",
    "_chat_direct_ticket",
    "_chat_direct_ticket_claims",
    "_require_data_secret",
}


def _apns_stub():
    import types
    return types.SimpleNamespace(
        direct_message_alert=(
            lambda sender="", started=False, conversation_id="", ref="",
            key_version=0: {
                "title": "Direct message from " + sender,
                **({"ref": ref, "keyVersion": key_version} if ref else {})}))


class _FakeSubtle:
    async def digest(self, _algorithm, data):
        return hashlib.sha256(bytes(data)).digest()


class _FakeCrypto:
    subtle = _FakeSubtle()


class _FakeU8:
    @staticmethod
    def new(data):
        class _Array:
            def __init__(self, value):
                self.value = bytes(value)

            def to_py(self):
                return self.value

            def __bytes__(self):
                return self.value

        return _Array(data)


class _Env:
    DATA_KEY = "private-production-data-key"


def _helpers():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCTIONS
    ]
    found = {node.name for node in selected}
    assert found == FUNCTIONS, "missing: %s" % sorted(FUNCTIONS - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    clock = [1_800_000_000_000]

    class _Date:
        @staticmethod
        def now():
            return clock[0]

    namespace = {
        "CHAT_DIRECT_TICKET_TTL_MS": 60 * 1000,
        "Date": _Date,
        "Uint8Array": _FakeU8,
        "_INSECURE_DATA_KEYS": frozenset({"", "forkmesh-dev-data-key"}),
        "_to_js": lambda value: value,
        "hmac": hmac,
        "js_crypto": _FakeCrypto(),
        "re": re,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace, clock


def _socket_harness(participant=True, version=1, account_status="active"):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    names = FUNCTIONS | {"_chat_direct_socket_handler"}
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in selected}
    assert found == names, "missing: %s" % sorted(names - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    calls = []
    clock = [1_800_000_000_000]

    class _Date:
        @staticmethod
        def now():
            return clock[0]

    async def d1_first(_env, sql, *args):
        if "FROM chat_direct_conversations" in sql:
            return {"data": "sealed-direct", "key_version": version}
        if "FROM users" in sql:
            return {"data": "sealed-user", "is_admin": 1}
        if "FROM chat_direct_participants" in sql:
            return {"allowed": 1} if participant else None
        raise AssertionError(sql)

    async def decrypt_row(_env, data):
        if data == "sealed-direct":
            return {"participants": ["alice", "bob"]}
        return {"status": account_status, "kind": "user"}

    namespace = {
        "CHAT_DIRECT_TICKET_TTL_MS": 60 * 1000,
        "Date": _Date,
        "Uint8Array": _FakeU8,
        "_INSECURE_DATA_KEYS": frozenset({"", "forkmesh-dev-data-key"}),
        "_account_kind": lambda record: record.get("kind", "user"),
        "_private_replica_not_found": lambda: {
            "status": 404, "data": {"error": "not_found"}},
        "_to_js": lambda value: value,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "durable_object_request": lambda request, target_url=None: asyncio.sleep(
            0, result={"request": request, "target": target_url}),
        "ensure_schema": lambda _env: asyncio.sleep(0),
        "hmac": hmac,
        "js_crypto": _FakeCrypto(),
        "json_response": lambda data, status=200: {
            "status": status, "data": data},
        "log_durable_object_abort": lambda *_args: asyncio.sleep(0),
        "method_name": lambda request: request.method,
        "parse_qs": parse_qs,
        "quote": quote,
        "re": re,
        "urlparse": urlparse,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)

    class _Room:
        def idFromName(self, name):
            calls.append(("id", name))
            return "room-id"

        def get(self, room_id):
            calls.append(("get", room_id))

            class _Stub:
                async def fetch(self, request):
                    calls.append(("fetch", request))
                    return {"status": 101}

            return _Stub()

    class _SocketEnv(_Env):
        FORKMESH_MAINNODE_ROOM = _Room()

    class _Headers:
        def get(self, name, default=None):
            return {"upgrade": "websocket"}.get(str(name).lower(), default)

    class _Request:
        method = "GET"
        headers = _Headers()

        def __init__(self, token):
            self.url = (
                "https://forkmesh.test/api/chat/direct-messages/"
                + "a" * 32
                + "/ws?ticket="
                + quote(token, safe="")
            )

    return namespace, _SocketEnv(), _Request, calls


def _room_key_from_path(path, account_bi):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.FunctionDef)
        and item.name == "room_key_from_path"
    )
    namespace = {
        "MAX_ROOM_NAME": 80,
        "REPO_ROOM_RE": re.compile(
            r"^/api/repo/([^/]+)/([^/]+)/rooms/([^/]+)/(?:ws|clients)$"
        ),
        "ROOM_RE": re.compile(r"^/api/room/([^/]+)/(?:ws|clients)$"),
        "CHAT_CHANNEL_DO_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/v([1-9][0-9]*)/"
            r"(ws|revoke)$"
        ),
        "CHAT_DIRECT_MESSAGE_DO_RE": re.compile(
            r"^/api/chat/direct-messages/([0-9a-f]{32})/v([1-9][0-9]*)/"
            r"(ws|revoke)$"
        ),
        "re": re,
        "safe_segment": lambda value, _limit=80: str(value or ""),
    }
    module = ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["room_key_from_path"](path, account_bi)


def _private_room_current():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    room = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "ForkMeshRoom"
    )
    node = next(
        child for child in room.body
        if isinstance(child, ast.AsyncFunctionDef)
        and child.name == "_private_room_current"
    )
    module = ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[]))
    return module


def _source_for(name, class_name=""):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = tree.body
    if class_name:
        parent = next(
            node for node in body
            if isinstance(node, ast.ClassDef) and node.name == class_name
        )
        body = parent.body
    node = next(
        item for item in body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(ENTRY_TEXT, node)


def test_direct_key_is_versioned_and_domain_separated():
    namespace, _clock = _helpers()
    env = _Env()
    direct_id = "a" * 32

    first = asyncio.run(namespace["_chat_direct_passphrase"](env, direct_id, 1))
    rotated = asyncio.run(namespace["_chat_direct_passphrase"](env, direct_id, 2))

    assert first != rotated
    assert len(first) == 64
    assert re.fullmatch(r"[0-9a-f]{64}", first)


def test_direct_ticket_round_trip_and_expiry():
    namespace, clock = _helpers()
    env = _Env()
    direct_id = "a" * 32
    account_bi = "b" * 64

    token = namespace["_chat_direct_ticket"](env, direct_id, 3, account_bi)
    assert namespace["_chat_direct_ticket_claims"](env, token) == {
        "conversation_id": direct_id,
        "key_version": 3,
        "account_bi": account_bi,
    }
    clock[0] += 61_000
    assert namespace["_chat_direct_ticket_claims"](env, token) is None


def test_retained_direct_message_atomically_advances_sender_cursor():
    namespace, clock = _helpers()
    calls = []
    pushes = []

    async def run_batch(env, statements):
        calls.append((env, statements))

    # The retained counter is what raises the OTHER participant's unreadCount,
    # so it is also where their clients get told (adhoc #1604). Their chat page
    # reads the conversation list once on open and never on a timer.
    async def d1_first(_env, _sql, *_params):
        return {"data": "row"}

    async def decrypt_row(_env, _value):
        return {"participants": ["alice", "bob"]}

    async def blind_index(_env, name):
        return {"alice": "b" * 64, "bob": "c" * 64}[name]

    async def notify_account_event(_env, owner, topic, push_alert=None):
        pushes.append((owner, topic))
        alerts.append(push_alert)

    alerts = []

    namespace["_contribution_run_batch"] = run_batch
    namespace["d1_first"] = d1_first
    namespace["decrypt_row"] = decrypt_row
    namespace["blind_index"] = blind_index
    namespace["notify_account_event"] = notify_account_event
    namespace["_apns_module"] = lambda: _apns_stub()
    namespace["clean_string"] = lambda value, limit: str(value or "")[:limit]
    namespace["valid_node_name"] = lambda value: bool(value)
    namespace["MAX_NODE_NAME"] = 63
    namespace["_CHAT_DIRECT_PEER_MEMO"] = {}
    namespace["CHAT_DIRECT_PEER_MEMO_MAX"] = 256
    env = _Env()
    conversation_id = "a" * 32
    account_bi = "b" * 64
    asyncio.run(namespace["_chat_direct_message_retained"](
        env,
        conversation_id,
        account_bi,
        clock[0],
    ))

    # alice sent it, so bob is the one whose unread count moved, and his
    # phone banner names her.
    assert pushes == [("bob", "direct-messages")]
    assert alerts == [{"title": "Direct message from alice"}]

    assert len(calls) == 1
    statements = calls[0][1]
    assert len(statements) == 2
    assert "message_count=message_count+1" in statements[0][0]
    assert "last_read_count=(SELECT message_count" in statements[1][0]
    assert statements[0][1] == (clock[0], conversation_id)
    assert statements[1][1] == (
        conversation_id,
        conversation_id,
        account_bi,
    )


def test_direct_socket_requires_participant_even_for_an_admin():
    namespace, env, request_type, calls = _socket_harness(participant=True)
    direct_id = "a" * 32
    account_bi = "b" * 64
    token = namespace["_chat_direct_ticket"](env, direct_id, 1, account_bi)
    allowed = asyncio.run(
        namespace["_chat_direct_socket_handler"](
            env, request_type(token), direct_id))
    assert allowed["status"] == 101
    assert ("id", f"chat-direct:{direct_id}:v1") in calls

    denied_namespace, denied_env, denied_request, denied_calls = (
        _socket_harness(participant=False)
    )
    denied_token = denied_namespace["_chat_direct_ticket"](
        denied_env, direct_id, 1, account_bi)
    denied = asyncio.run(
        denied_namespace["_chat_direct_socket_handler"](
            denied_env, denied_request(denied_token), direct_id))
    assert denied == {"status": 404, "data": {"error": "not_found"}}
    assert not denied_calls


def test_direct_socket_rejects_stale_version_and_inactive_account():
    namespace, env, request_type, _calls = _socket_harness(version=2)
    direct_id = "a" * 32
    account_bi = "b" * 64
    token = namespace["_chat_direct_ticket"](env, direct_id, 1, account_bi)
    stale = asyncio.run(
        namespace["_chat_direct_socket_handler"](
            env, request_type(token), direct_id))
    assert stale["status"] == 404

    inactive_ns, inactive_env, inactive_request, _inactive_calls = (
        _socket_harness(account_status="disabled")
    )
    inactive_token = inactive_ns["_chat_direct_ticket"](
        inactive_env, direct_id, 1, account_bi)
    inactive = asyncio.run(
        inactive_ns["_chat_direct_socket_handler"](
            inactive_env, inactive_request(inactive_token), direct_id))
    assert inactive["status"] == 404


def test_internal_direct_room_path_is_isolated_and_carries_identity():
    direct_id = "a" * 32
    account_bi = "b" * 64
    assert _room_key_from_path(
        f"/api/chat/direct-messages/{direct_id}/v1/ws",
        account_bi,
    ) == {
        "key": f"chat-direct:{direct_id}:v1",
        "owner": "",
        "repo": "",
        "room": direct_id,
        "compat": False,
        "direct_id": direct_id,
        "direct_version": 1,
        "account_bi": account_bi,
    }


def test_live_direct_socket_rechecks_participant_membership():
    module = _private_room_current()
    state = {"participant": False}

    async def d1_first(_env, sql, *args):
        if "FROM chat_direct_conversations" in sql:
            return {"data": "sealed-direct", "key_version": 1}
        if "FROM users" in sql:
            return {"data": "sealed-user", "is_admin": 1}
        if "FROM chat_direct_participants" in sql:
            return {"allowed": 1} if state["participant"] else None
        raise AssertionError(sql)

    async def decrypt_row(_env, data):
        if data == "sealed-direct":
            return {"participants": ["alice", "bob"]}
        return {"status": "active", "kind": "user"}

    namespace = {
        "_account_kind": lambda record: record.get("kind", "user"),
        "_ws_attr": lambda ws, key, default=None: ws.get(key, default),
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "ensure_schema": lambda _env: asyncio.sleep(0),
        "re": re,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    self = type("Room", (), {"env": _Env()})()
    socket = {
        "direct_id": "a" * 32,
        "direct_version": 1,
        "account_bi": "b" * 64,
    }

    assert asyncio.run(namespace["_private_room_current"](self, socket)) is False
    state["participant"] = True
    assert asyncio.run(namespace["_private_room_current"](self, socket)) is True


def test_worker_routes_direct_api_and_socket_before_generic_rooms():
    source = _source_for("_route", class_name="Default")
    api_at = source.index("chat_direct_messages_api.handle")
    socket_at = source.index("_chat_direct_socket_handler")
    generic_room_at = source.index("room_key_from_path")
    assert api_at < generic_room_at
    assert socket_at < generic_room_at


def test_account_removal_revokes_and_deletes_direct_message_state():
    cleanup = _source_for("_delete_chat_direct_conversations")
    revoke_at = cleanup.index("_revoke_chat_direct_room")
    participants_at = cleanup.index(
        '"DELETE FROM chat_direct_participants WHERE conversation_id=?"'
    )
    conversations_at = cleanup.index(
        '"DELETE FROM chat_direct_conversations WHERE conversation_id=?"'
    )
    history_at = cleanup.index('"DELETE FROM chat_history WHERE room_key=?"')
    assert revoke_at < participants_at < conversations_at < history_at

    rename = _source_for("_rename_account_namespace")
    delete = _source_for("_delete_account_namespace")
    assert "_delete_chat_direct_conversations(env, name_bi)" in rename
    assert "_delete_chat_direct_conversations(env, name_bi)" in delete


def test_direct_message_peer_lookup_is_memoized_per_conversation():
    # A conversation's two participants never change, so resolving the
    # recipient of the unread-count push must not cost a D1 read plus a row
    # decrypt on every message a busy conversation retains.
    namespace, _clock = _helpers()
    reads = []

    async def d1_first(_env, sql, *_params):
        reads.append(sql)
        return {"data": "row"}

    async def decrypt_row(_env, _value):
        return {"participants": ["alice", "bob"]}

    async def blind_index(_env, name):
        return {"alice": "b" * 64, "bob": "c" * 64}[name]

    namespace["d1_first"] = d1_first
    namespace["decrypt_row"] = decrypt_row
    namespace["blind_index"] = blind_index
    namespace["clean_string"] = lambda value, limit: str(value or "")[:limit]
    namespace["valid_node_name"] = lambda value: bool(value)
    namespace["MAX_NODE_NAME"] = 63
    namespace["_CHAT_DIRECT_PEER_MEMO"] = {}
    namespace["CHAT_DIRECT_PEER_MEMO_MAX"] = 256

    env = _Env()
    conversation_id = "a" * 32
    for _ in range(4):
        assert asyncio.run(namespace["_chat_direct_message_recipient"](
            env, conversation_id, "b" * 64)) == "bob"
    assert len(reads) == 1

    # A transient read failure must not be remembered as "nobody to notify".
    async def failing_d1_first(_env, _sql, *_params):
        raise RuntimeError("D1 unavailable")

    namespace["d1_first"] = failing_d1_first
    other = "c" * 32
    assert asyncio.run(namespace["_chat_direct_message_recipient"](
        env, other, "b" * 64)) == ""
    namespace["d1_first"] = d1_first
    assert asyncio.run(namespace["_chat_direct_message_recipient"](
        env, other, "b" * 64)) == "bob"
