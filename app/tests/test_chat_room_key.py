#!/usr/bin/env python3
"""Repository-scoped room-chat key endpoint (/api/chat/room-key).

Each passphrase is derived one-way from DATA_KEY. The dedicated public World
#general scope is available to guests; every other scope is handed only to an
authenticated repository participant. Verifies the guest exception is exact,
session/node authentication remains effective elsewhere, non-default
repositories have distinct keys, and no response leaks DATA_KEY.
AST-extraction harness (test_repo_agents.py style) with WebCrypto SHA-256
stubbed by hashlib.
"""

import ast
import asyncio
import hashlib
import hmac
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
APP_RUNTIME = ENTRY.parent / "app_runtime.py"
QT_SETUP = ENTRY.parents[2] / "desktop" / "src" / "MainWindowSetup.cpp"
CATALOG = ENTRY.parent / "catalog.py"
SCHEMA = ENTRY.parent / "schema.py"
ENTRY_TEXT = (
    (
    # entry.py + its lazily-split domain modules
    APP_RUNTIME.read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
) + "\n" + CATALOG.read_text(encoding="utf-8")
    + "\n" + SCHEMA.read_text(encoding="utf-8"))

FUNCS = {
    "chat_room_key_handler", "_room_key_authorized", "_room_chat_passphrase",
    "_room_key_requester",
    "_require_data_secret", "valid_node_name", "clean_string", "_owner_pubkey",
}


def _load(extra):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [n for n in tree.body
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                and n.name in FUNCS]
    assert {n.name for n in selected} == FUNCS, \
        "missing: %s" % sorted(FUNCS - {n.name for n in selected})
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


class _Headers:
    def __init__(self, m=None):
        self._m = {str(k).lower(): v for k, v in (m or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="GET", url="https://forkmesh.test/api/chat/room-key",
                 headers=None):
        self.method = method
        self.url = url
        self.headers = _Headers(headers)


# --- WebCrypto SHA-256 digest stub (hashlib) --------------------------------
class _FakeSubtle:
    async def digest(self, _algo, data):
        return hashlib.sha256(bytes(data)).digest()


class _FakeCrypto:
    subtle = _FakeSubtle()


class _FakeU8:
    @staticmethod
    def new(data):
        class _Arr:
            def __init__(self, d):
                self._d = bytes(d)

            def to_py(self):
                return self._d

            def __bytes__(self):
                return self._d
        return _Arr(data)


def _harness(accounts, data_key="a-real-secret-data-key"):
    now = [1_000_000_000]

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def json_response(data, status=200, **_k):
        return {"status": status, "data": data}

    async def ensure_schema(_e):
        return None

    async def _account_row(_e, name):
        key = str(name or "").strip().lower()
        rec = accounts.get(key)
        if rec is None:
            return "bi:" + key, None
        rec = dict(rec)
        rec.setdefault("name", key)
        return "bi:" + key, rec

    async def ed25519_verify(pubkey, sig, _canonical):
        # A node "signs" by presenting sig == "sig-for:<pubkey>".
        return bool(pubkey) and sig == "sig-for:" + pubkey

    async def _repository_access_context(
            _env, _request, owner, repo, viewer=""):
        if owner == "missing":
            return None
        return {
            "visibility": "public",
            "viewer": viewer,
            "record": {"owner": owner, "name": repo, "visibility": "public"},
        }

    def method_name(request):
        return getattr(request, "method", "GET")

    def _ts_ok(_ts):
        return True

    def _account_session_token(_env, name):
        return "test-session:" + str(name or "").strip().lower()

    async def _authed_account_name(_env, request, data=None):
        del data
        auth = request.headers.get("authorization") or ""
        token = auth[7:] if auth.lower().startswith("bearer ") else ""
        prefix = "test-session:"
        name = token[len(prefix):] if token.startswith(prefix) else ""
        rec = accounts.get(name)
        return name if rec and rec.get("status") == "active" else ""

    class _Env:
        DATA_KEY = data_key

    ns = _load({
        "Date": _DateStub,
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "_account_row": _account_row,
        "_account_session_token": _account_session_token,
        "_authed_account_name": _authed_account_name,
        "ed25519_verify": ed25519_verify,
        "_repository_access_context": _repository_access_context,
        "_private_replica_not_found": lambda: {
            "status": 404, "data": {"error": "not_found"}},
        "safe_segment": lambda value, *_a: (
            str(value or "") if re.fullmatch(
                r"[A-Za-z0-9._-]+", str(value or "")) else ""),
        "method_name": method_name,
        "_ts_ok": _ts_ok,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "hmac": hmac,
        "js_crypto": _FakeCrypto(),
        "_to_js": lambda x: x,
        "Uint8Array": _FakeU8,
        "_room_key_cache": {"secret": None, "value": None},
        "_INSECURE_DATA_KEYS": frozenset({"", "forkmesh-dev-data-key"}),
        "MAX_NODE_NAME": 63,
        "MAX_ROOM_NAME": 80,
        "PUBLIC_WORLD_GENERAL_ROOM": "world-general",
        "NODE_NAME_RE": re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"),
        "ADMIN_SESSION_TTL_MS": 12 * 60 * 60 * 1000,
    })
    return ns, _Env()


def _user():
    return {"pubkey": "PK-alice", "status": "active", "pass_hash": "h",
            "pass_salt": "s", "is_admin": False}


def test_unauthenticated_is_rejected():
    ns, env = _harness({"alice": _user()})
    resp = asyncio.run(ns["chat_room_key_handler"](env, _Request()))
    assert resp["status"] == 401
    assert "passphrase" not in resp["data"]


def test_guest_gets_only_the_public_world_general_passphrase():
    ns, env = _harness({"alice": _user()}, data_key="super-secret-data-key")
    public = asyncio.run(ns["chat_room_key_handler"](
        env,
        _Request(
            url=(
                "https://forkmesh.test/api/chat/room-key"
                "?owner=mainnode&repo=forkmesh&room=world-general"
            ),
        ),
    ))
    assert public["status"] == 200
    assert public["data"]["scope"] == "mainnode/forkmesh"
    assert public["data"]["room"] == "world-general"
    assert public["data"]["access"] == "public-world-general"
    assert public["data"]["passphrase"] == hashlib.sha256(
        b"super-secret-data-key:room-chat-passphrase-public-world-general-v1"
    ).hexdigest()

    for url in (
        "https://forkmesh.test/api/chat/room-key",
        (
            "https://forkmesh.test/api/chat/room-key"
            "?owner=mainnode&repo=forkmesh&room=general"
        ),
        (
            "https://forkmesh.test/api/chat/room-key"
            "?owner=mainnode&repo=forkmesh&room=space-station"
        ),
        (
            "https://forkmesh.test/api/chat/room-key"
            "?owner=alice&repo=secret&room=world-general"
        ),
    ):
        denied = asyncio.run(ns["chat_room_key_handler"](
            env, _Request(url=url)))
        assert denied["status"] == 401
        assert "passphrase" not in denied["data"]


def test_public_world_socket_bypasses_only_the_exact_repository_acl_gate():
    assert (
        'PUBLIC_WORLD_GENERAL_ROOM_KEY = ('
        in ENTRY.read_text(encoding="utf-8")
    )
    tree = ast.parse(
        APP_RUNTIME.read_text(encoding="utf-8"), filename=str(APP_RUNTIME))
    route = next(
        node for node in ast.walk(tree)
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "_route"
    )
    source = ast.unparse(route)
    room_block = source[source.index("room = room_key_from_path(url.path)"):]
    durable_at = room_block.index("FORKMESH_MAINNODE_ROOM.idFromName")
    gate = room_block[:durable_at]
    assert (
        "room.get('key') == PUBLIC_WORLD_GENERAL_ROOM_KEY"
        in gate
    )
    assert "and (not public_world_general)" in gate
    assert "_repository_access_context" in gate


def test_session_token_gets_the_passphrase():
    ns, env = _harness({"alice": _user()})
    token = ns["_account_session_token"](env, "alice")
    resp = asyncio.run(ns["chat_room_key_handler"](
        env, _Request(headers={"authorization": "Bearer " + token})))
    assert resp["status"] == 200
    assert isinstance(resp["data"]["passphrase"], str)
    assert len(resp["data"]["passphrase"]) == 64  # sha256 hex
    assert resp["data"]["room"] == "general"
    assert resp["data"]["access"] == "authenticated-repository"


def test_node_signature_gets_the_passphrase():
    ns, env = _harness({"alice": _user()})
    url = ("https://forkmesh.test/api/chat/room-key"
           "?node=alice&ts=1000000000&sig=sig-for:PK-alice")
    resp = asyncio.run(ns["chat_room_key_handler"](env, _Request(url=url)))
    assert resp["status"] == 200
    assert len(resp["data"]["passphrase"]) == 64
    # A wrong signature is rejected.
    bad = asyncio.run(ns["chat_room_key_handler"](env, _Request(
        url="https://forkmesh.test/api/chat/room-key?node=alice&ts=1&sig=nope")))
    assert bad["status"] == 401


def test_passphrase_is_deterministic_and_key_derived_not_raw_data_key():
    ns, env = _harness({"alice": _user()}, data_key="super-secret-data-key")
    token = ns["_account_session_token"](env, "alice")
    a = asyncio.run(ns["chat_room_key_handler"](
        env, _Request(headers={"authorization": "Bearer " + token})))["data"]["passphrase"]
    b = asyncio.run(ns["chat_room_key_handler"](
        env, _Request(headers={"authorization": "Bearer " + token})))["data"]["passphrase"]
    assert a == b  # every client must derive the same room key
    # The passphrase must be a one-way digest, never the DATA_KEY itself.
    assert a != "super-secret-data-key"
    assert "super-secret-data-key" not in a
    expected = hashlib.sha256(
        b"super-secret-data-key:room-chat-passphrase-v1").hexdigest()
    assert a == expected


def test_non_default_repository_passphrase_is_scoped():
    ns, env = _harness({"alice": _user()}, data_key="super-secret-data-key")
    token = ns["_account_session_token"](env, "alice")
    headers = {"authorization": "Bearer " + token}
    public = asyncio.run(ns["chat_room_key_handler"](
        env, _Request(headers=headers)))["data"]
    scoped = asyncio.run(ns["chat_room_key_handler"](
        env,
        _Request(
            url=("https://forkmesh.test/api/chat/room-key"
                 "?owner=alice&repo=secret"),
            headers=headers,
        ),
    ))["data"]
    assert public["scope"] == "mainnode/forkmesh"
    assert scoped["scope"] == "alice/secret"
    assert scoped["passphrase"] != public["passphrase"]
    assert scoped["passphrase"] == hashlib.sha256(
        b"super-secret-data-key:room-chat-passphrase-v2:alice/secret"
    ).hexdigest()


def test_qt_signature_binds_the_requested_repository_scope():
    setup = QT_SETUP.read_text(encoding="utf-8")
    assert '"forkmesh-room-key-v2\\n" + node + "\\n" + roomOwner + "\\n"' in setup
    assert 'query.addQueryItem("owner", roomOwner)' in setup
    assert 'query.addQueryItem("repo", roomRepo)' in setup
    assert 'query.addQueryItem("room", kDefaultRoomName)' in setup
    assert 'resp.value("room").toString() == kDefaultRoomName' in setup


def test_qt_default_room_matches_the_public_world_room_and_migrates_safely():
    room_header = (
        ENTRY.parents[2] / "desktop" / "src" / "MainnodeRoom.h"
    ).read_text(encoding="utf-8")
    assert 'kDefaultRoomName = QStringLiteral("world-general")' in room_header
    assert (
        '"/api/repo/mainnode/forkmesh/rooms/world-general/ws"'
        in room_header
    )
    assert "url.path() == kLegacyRoomPath || url.path() == kRoomPath" in room_header


def test_missing_or_unauthorized_scope_is_normalized_to_not_found():
    ns, env = _harness({"alice": _user()})
    token = ns["_account_session_token"](env, "alice")
    resp = asyncio.run(ns["chat_room_key_handler"](
        env,
        _Request(
            url=("https://forkmesh.test/api/chat/room-key"
                 "?owner=missing&repo=secret"),
            headers={"authorization": "Bearer " + token},
        ),
    ))
    assert resp == {"status": 404, "data": {"error": "not_found"}}


def test_placeholder_data_key_fails_closed():
    ns, env = _harness({"alice": _user()}, data_key="")
    token = ns["_account_session_token"](env, "alice")
    try:
        asyncio.run(ns["chat_room_key_handler"](
            env, _Request(headers={"authorization": "Bearer " + token})))
    except RuntimeError:
        return  # _require_data_secret refuses a placeholder key
    raise AssertionError("expected RuntimeError on placeholder DATA_KEY")
