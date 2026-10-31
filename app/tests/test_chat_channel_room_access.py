#!/usr/bin/env python3
"""Private chat channel key, ticket, and socket-admission contracts."""

import ast
import asyncio
import hashlib
import hmac
import ipaddress
import re
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
APP_RUNTIME = ENTRY.with_name("app_runtime.py")
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
APP_RUNTIME_TEXT = APP_RUNTIME.read_text(encoding="utf-8")
FUNCTIONS = {
    "_chat_channel_passphrase",
    "_chat_channel_ticket",
    "_chat_channel_ticket_claims",
    "_desktop_office_session_id",
    "_require_data_secret",
}


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
        "CHAT_CHANNEL_TICKET_TTL_MS": 60 * 1000,
        "Date": _Date,
        "Uint8Array": _FakeU8,
        "_INSECURE_DATA_KEYS": frozenset({"", "forkmesh-dev-data-key"}),
        "_to_js": lambda value: value,
        "hashlib": hashlib,
        "hmac": hmac,
        "ipaddress": ipaddress,
        "js_crypto": _FakeCrypto(),
        "re": re,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace, clock


def _socket_harness(channel_version=1, member=True, admin=False,
                    visibility="private", account_status="active",
                    account_kind="user"):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    names = FUNCTIONS | {"_chat_channel_socket_handler", "_chat_socket_address"}
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    calls = []
    clock = [1_800_000_000_000]

    class _Date:
        @staticmethod
        def now():
            return clock[0]

    async def d1_first(_env, sql, *args):
        if "FROM chat_channels" in sql:
            return {
                "data": "sealed-channel",
                "key_version": channel_version,
            }
        if "FROM users" in sql:
            return {"data": "sealed-user", "is_admin": 1 if admin else 0}
        if "FROM chat_channel_members" in sql:
            return {"allowed": 1} if member else None
        raise AssertionError(sql)

    async def decrypt_row(_env, data):
        if data == "sealed-channel":
            return {"visibility": visibility}
        return {"status": account_status, "kind": account_kind}

    namespace = {
        "CHAT_CHANNEL_TICKET_TTL_MS": 60 * 1000,
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
        "hashlib": hashlib,
        "hmac": hmac,
        "ipaddress": ipaddress,
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
                "https://forkmesh.test/api/chat/channels/" + "a" * 32
                + "/ws?ticket=" + quote(token, safe="")
            )

    return namespace, _SocketEnv(), _Request, calls


class _Env:
    DATA_KEY = "private-production-data-key"


def _function_source(name, source=ENTRY_TEXT, path=ENTRY):
    tree = ast.parse(source, filename=str(path))
    node = next(
        item for item in ast.walk(tree)
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.unparse(node)


def test_desktop_office_session_id_is_valid_stable_and_account_scoped():
    helpers, _clock = _helpers()
    make_id = helpers["_desktop_office_session_id"]
    first = make_id("a" * 64, "Jett")
    assert re.fullmatch(r"[A-Za-z0-9_-]{24,64}", first)
    assert first == make_id("a" * 64, "jett")
    assert first != make_id("b" * 64, "jett")


def _room_key_from_path(path):
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
        "re": re,
        "safe_segment": lambda value, _limit=80: str(value or ""),
    }
    module = ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["room_key_from_path"](path)


def _room_class(namespace):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.ClassDef) and item.name == "ForkMeshRoom"
    )
    module = ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[]))
    globals_map = {"DurableObject": object, **namespace}
    exec(compile(module, str(ENTRY), "exec"), globals_map)
    return globals_map["ForkMeshRoom"]


def test_channel_passphrases_are_scoped_by_channel_and_version():
    namespace, _clock = _helpers()
    env = _Env()
    first = asyncio.run(
        namespace["_chat_channel_passphrase"](env, "a" * 32, 1))
    other_channel = asyncio.run(
        namespace["_chat_channel_passphrase"](env, "b" * 32, 1))
    rotated = asyncio.run(
        namespace["_chat_channel_passphrase"](env, "a" * 32, 2))
    assert len(first) == 64
    assert first != other_channel
    assert first != rotated
    assert env.DATA_KEY not in first


def test_ticket_is_channel_version_account_and_expiry_bound():
    namespace, clock = _helpers()
    env = _Env()
    account_bi = "b" * 64
    token = namespace["_chat_channel_ticket"](
        env, "a" * 32, 3, account_bi)
    assert namespace["_chat_channel_ticket_claims"](env, token) == {
        "channel_id": "a" * 32,
        "key_version": 3,
        "account_bi": account_bi,
    }
    assert namespace["_chat_channel_ticket_claims"](env, token + "x") is None
    assert namespace["_chat_channel_ticket_claims"](
        env, token.replace("." + account_bi + ".", "." + "c" * 64 + ".")
    ) is None
    clock[0] += 60_001
    assert namespace["_chat_channel_ticket_claims"](env, token) is None


def test_ticket_rejects_malformed_and_insecure_secret_inputs():
    namespace, _clock = _helpers()
    assert namespace["_chat_channel_ticket_claims"](_Env(), "") is None
    assert namespace["_chat_channel_ticket_claims"](
        _Env(), "v1.release-team.1.user.999.signature") is None

    class _BadEnv:
        DATA_KEY = "forkmesh-dev-data-key"

    try:
        namespace["_chat_channel_ticket"](
            _BadEnv(), "a" * 32, 1, "b" * 64)
    except RuntimeError as error:
        assert "DATA_KEY" in str(error)
    else:
        raise AssertionError("placeholder DATA_KEY must fail closed")


def test_private_socket_authorization_precedes_durable_object_lookup():
    source = _function_source("_chat_channel_socket_handler")
    assert source.index("_chat_channel_ticket_claims") < source.index(
        "idFromName")
    assert source.index("chat_channel_members") < source.index("idFromName")
    assert source.index("users") < source.index("idFromName")
    assert "chat-channel:" in source
    assert ":v" in source
    assert "_private_replica_not_found" in source


def test_private_socket_routes_only_current_authorized_members():
    namespace, env, request_type, calls = _socket_harness()
    token = namespace["_chat_channel_ticket"](
        env, "a" * 32, 1, "b" * 64)
    response = asyncio.run(namespace["_chat_channel_socket_handler"](
        env, request_type(token), "a" * 32))
    assert response["status"] == 101
    assert calls[0] == ("id", "chat-channel:" + "a" * 32 + ":v1")
    assert calls[-1][0] == "fetch"
    assert calls[-1][1]["target"] == (
        "https://forkmesh.test/api/chat/channels/" + "a" * 32
        + "/v1/ws?account=" + "b" * 64
    )

    for kwargs in (
        {"member": False},
        {"channel_version": 2},
    ):
        denied_ns, denied_env, denied_request, denied_calls = _socket_harness(
            **kwargs)
        denied_token = denied_ns["_chat_channel_ticket"](
            denied_env, "a" * 32, 1, "b" * 64)
        denied = asyncio.run(denied_ns["_chat_channel_socket_handler"](
            denied_env, denied_request(denied_token), "a" * 32))
        assert denied == {"status": 404, "data": {"error": "not_found"}}
        assert denied_calls == []


def test_private_room_path_carries_channel_version_into_history_key():
    channel_id = "a" * 32
    info = _room_key_from_path(
        f"/api/chat/channels/{channel_id}/v3/ws")
    assert info == {
        "key": f"chat-channel:{channel_id}:v3",
        "owner": "",
        "repo": "",
        "room": channel_id,
        "compat": False,
        "channel_id": channel_id,
        "channel_version": 3,
    }


def test_private_room_revocation_closes_every_accepted_socket():
    now = 1_800_000_000_000

    class _Date:
        @staticmethod
        def now():
            return now

    class _Peer:
        def __init__(self):
            self.closed = []

        def deserializeAttachment(self):
            return type("Attachment", (), {"last": now})()

        def close(self, code, reason):
            self.closed.append((code, reason))

    peers = [_Peer(), _Peer()]

    class _Context:
        def getWebSockets(self, tag):
            assert tag == "chat"
            return peers

    class _Headers:
        def get(self, _name, default=None):
            return default

    class _Request:
        method = "POST"
        headers = _Headers()
        url = (
            "https://forkmesh.internal/api/chat/channels/" + "a" * 32
            + "/v1/revoke"
        )

    room_type = _room_class({
        "CHAT_CHANNEL_DO_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/v([1-9][0-9]*)/"
            r"(ws|revoke)$"
        ),
        "Date": _Date,
        "MAX_CONNECTIONS": 128,
        "ROOM_CLIENT_STALE_MS": 180_000,
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.deserializeAttachment(), name, default),
        "json_response": lambda data, status=200, **_kwargs: {
            "status": status,
            "data": data,
        },
        "method_name": lambda request: request.method,
        "room_key_from_path": lambda _path: None,
        "urlparse": urlparse,
    })
    room = room_type()
    room.ctx = _Context()

    response = asyncio.run(room.fetch(_Request()))

    assert response["status"] == 200
    assert all(peer.closed == [(1008, "room access revoked")]
               for peer in peers)


def test_private_socket_refuses_admin_without_membership_row():
    namespace, env, request_type, calls = _socket_harness(
        member=False, admin=True)
    token = namespace["_chat_channel_ticket"](
        env, "a" * 32, 1, "b" * 64)
    response = asyncio.run(namespace["_chat_channel_socket_handler"](
        env, request_type(token), "a" * 32))
    assert response["status"] == 404
    assert calls == []


def test_private_socket_refuses_non_member_with_a_valid_ticket():
    namespace, env, request_type, calls = _socket_harness(member=False)
    token = namespace["_chat_channel_ticket"](
        env, "a" * 32, 1, "b" * 64)
    response = asyncio.run(namespace["_chat_channel_socket_handler"](
        env, request_type(token), "a" * 32))
    assert response == {"status": 404, "data": {"error": "not_found"}}
    assert calls == []


def test_live_private_socket_recheck_has_no_admin_bypass():
    source = ENTRY_TEXT[ENTRY_TEXT.index("class ForkMeshRoom(DurableObject)"):]
    recheck = source[:source.index("async def _retention_ingress_admitted")]
    tail = recheck[recheck.rindex('"SELECT data,created_by_bi,key_version '
                                  'FROM chat_channels "'):]
    assert "is_admin" not in tail.split("FROM users")[1]


def test_public_socket_allows_active_registered_user_without_membership_row():
    namespace, env, request_type, calls = _socket_harness(
        member=False, visibility="public")
    token = namespace["_chat_channel_ticket"](
        env, "a" * 32, 1, "b" * 64)

    response = asyncio.run(namespace["_chat_channel_socket_handler"](
        env, request_type(token), "a" * 32))

    assert response["status"] == 101
    assert calls[0] == ("id", "chat-channel:" + "a" * 32 + ":v1")


def test_live_private_socket_rechecks_current_account_membership():
    async def d1_first(_env, sql, *args):
        if "FROM chat_channels" in sql:
            return {"data": "sealed-channel", "key_version": 1}
        if "FROM users" in sql:
            return {"data": "sealed-user", "is_admin": 0}
        if "FROM chat_channel_members" in sql:
            return None
        raise AssertionError((sql, args))

    async def decrypt_row(_env, data):
        if data == "sealed-channel":
            return {"visibility": "private"}
        return {"status": "active", "kind": "user"}

    attachment = type("Attachment", (), {
        "channel_id": "a" * 32,
        "channel_version": 1,
        "account_bi": "b" * 64,
    })()
    ws = type("Socket", (), {
        "deserializeAttachment": lambda self: attachment,
    })()
    room_type = _room_class({
        "_account_kind": lambda record: record.get("kind", "user"),
        "_ws_attr": lambda socket, name, default=None: getattr(
            socket.deserializeAttachment(), name, default),
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "ensure_schema": lambda _env: asyncio.sleep(0),
    })
    room = room_type()
    room.env = object()

    assert asyncio.run(room._private_room_current(ws)) is False


def test_live_socket_attachment_carries_ticket_account_identity():
    socket_source = _function_source("_chat_channel_socket_handler")
    room_source = _function_source("room_key_from_path")
    room_class_source = ast.unparse(next(
        item for item in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body
        if isinstance(item, ast.ClassDef) and item.name == "ForkMeshRoom"
    ))

    assert "account=" in socket_source
    assert "account_bi" in room_source
    assert "account_bi" in room_class_source


def test_runtime_adapter_and_routes_use_private_channel_gates():
    assert 'chat_channels_api = _LazyModule("chat_channels_api")' in ENTRY_TEXT
    for route_name in (
        "CHAT_CHANNELS_RE",
        "CHAT_CHANNEL_MEMBERS_RE",
        "CHAT_CHANNEL_ROOM_ACCESS_RE",
        "CHAT_CHANNEL_WS_RE",
    ):
        assert route_name in ENTRY_TEXT
    assert "class _ChatChannelsRuntime(_WorldCommunityRuntime)" in ENTRY_TEXT
    runtime_source = ast.unparse(next(
        node for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body
        if isinstance(node, ast.ClassDef)
        and node.name == "_ChatChannelsRuntime"
    ))
    world_runtime_source = ENTRY_TEXT[
        ENTRY_TEXT.index("class _WorldCommunityRuntime"):
        ENTRY_TEXT.index("class _ChatChannelsRuntime")
    ]
    assert "async def batch(self, statements)" in world_runtime_source
    assert "_contribution_run_batch(self.env, statements)" in (
        world_runtime_source
    )
    assert "_chat_channel_passphrase" in runtime_source
    assert "_chat_channel_ticket" in runtime_source
    assert "_office_meeting_ticket" in runtime_source
    assert "meetingWebSocketUrl" in runtime_source
    assert "_revoke_office_channel_room" in runtime_source
    assert "quote(" in runtime_source

    cleanup_source = _function_source("_delete_chat_channel_memberships")
    assert "_revoke_chat_channel_room" in cleanup_source
    assert "_revoke_office_channel_room" in cleanup_source

    # Request dispatch lives in the lazy runtime, while the private channel
    # primitives above remain module-level entry helpers.
    route = _function_source("_route", APP_RUNTIME_TEXT, APP_RUNTIME)
    api_at = route.index("chat_channels_api.handle")
    socket_at = route.index("_chat_channel_socket_handler")
    generic_room_at = route.index("room_key_from_path")
    assert api_at < generic_room_at
    assert socket_at < generic_room_at
