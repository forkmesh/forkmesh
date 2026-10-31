#!/usr/bin/env python3
"""Server side of docs/app/chat-relay-hardening.md (R3-R6)."""

import ast
import asyncio
import base64
import hashlib
import hmac
import ipaddress
import json
import re
import sqlite3
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)
from cryptography.hazmat.primitives.serialization import (
    Encoding, PublicFormat)

import test_chat_recipient_count as relay


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
ENTRY_TREE = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
APP_RUNTIME_TEXT = (ROOT / "src" / "app_runtime.py").read_text(
    encoding="utf-8")
URLS_TEXT = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import chat_direct_messages_api as dm_api  # noqa: E402
from test_chat_direct_messages_api import FakeRuntime as DmRuntime  # noqa: E402

NOW = relay.NOW
ALICE = relay.ALICE
BOB = relay.BOB
WORLD_GENERAL = "repo:mainnode/forkmesh:room:world-general"
GENERAL = "repo:mainnode/forkmesh:room:general"


def _b64u(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _b64u_decode(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def _node_key(seed=1):
    private = Ed25519PrivateKey.from_private_bytes(bytes([seed]) * 32)
    public = private.public_key().public_bytes(
        Encoding.Raw, PublicFormat.Raw)
    return private, _b64u(public)


async def _ed25519_verify(pubkey, sig, data):
    try:
        Ed25519PublicKey.from_public_bytes(_b64u_decode(pubkey)).verify(
            _b64u_decode(sig), data)
    except Exception:
        return False
    return True


def _functions(names, namespace):
    selected = [
        node for node in ENTRY_TREE.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    return namespace


def _cipher(**extra):
    return json.dumps(
        {"kind": "cipher", "v": 1, "iv": "i", "data": "d", "persist": True,
         **extra}, separators=(",", ":"))


def test_the_relay_strips_a_client_srv_mark_from_every_lane():
    _namespace, room_type, stored = relay._harness(
        channel={"data": {"visibility": "public"}, "created_by_bi": ALICE})
    sender = relay._Socket("s", account_bi=ALICE, channel_id=relay.CHANNEL)
    reader = relay._Socket("r", account_bi=BOB, channel_id=relay.CHANNEL)
    relay._relay(room_type, [sender, reader], sender, _cipher(srv=True))
    delivered = [json.loads(frame) for frame in reader.sent]
    assert delivered and all("srv" not in frame for frame in delivered)
    assert stored and all('"srv"' not in row[3] for row in stored)

    _namespace, room_type, stored = relay._harness(
        participants=(ALICE, BOB))
    sender = relay._Socket("s", account_bi=ALICE, direct_id=relay.DIRECT)
    reader = relay._Socket("r", account_bi=BOB, direct_id=relay.DIRECT)
    live = json.dumps({"kind": "cipher", "v": 1, "live": True, "srv": True,
                       "data": "d"}, separators=(",", ":"))
    relay._relay(room_type, [sender, reader], sender, live)
    assert [json.loads(frame) for frame in reader.sent] == [
        {"kind": "cipher", "v": 1, "live": True, "data": "d"}]


def test_server_injected_webhook_frames_carry_the_srv_mark():
    namespace, room_type, stored = relay._harness(
        channel={"data": {"visibility": "public"}, "created_by_bi": ALICE})
    reader = relay._Socket("r", account_bi=BOB, channel_id=relay.CHANNEL)
    body = json.dumps(
        {"kind": "cipher", "v": 1, "nonce": "n", "tag": "t", "body": "b",
         "persist": True, "srv": False}, separators=(",", ":"))

    class _Request:
        headers = {"x-forkmesh-chat-webhook": hmac.new(
            b"secret:chat-webhook-delivery-v1", body.encode(),
            "sha256").hexdigest()}

        async def text(self):
            return body

    namespace["hmac"] = hmac
    namespace["_require_data_secret"] = lambda _env: "secret"
    room = room_type()
    room.env = object()
    room.ctx = relay._Context([reader])
    match = re.match(
        r"^/api/chat/channels/([0-9a-f]{32})/v([1-9][0-9]*)/(webhook)$",
        "/api/chat/channels/" + relay.CHANNEL + "/v1/webhook")
    response = asyncio.run(room._channel_webhook(_Request(), match))
    assert response["status"] == 200
    assert json.loads(reader.sent[0])["srv"] is True
    assert json.loads(stored[0][3])["srv"] is True


def test_the_new_member_notice_is_marked_as_a_server_frame():
    source = ast.unparse(next(
        node for node in ast.walk(ENTRY_TREE)
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_new_member"))
    assert "'srv': True" in source


def test_an_invalid_frame_neither_relays_nor_refreshes_last_seen():
    _namespace, room_type, stored = relay._harness()
    sender = relay._Socket("s")
    reader = relay._Socket("r")
    sender.attachment["last"] = NOW - 100_000
    for junk in ("not json", "[1,2]", '{"kind":"presence"}', '"cipher"'):
        relay._relay(room_type, [sender, reader], sender, junk)
    assert sender.attachment["last"] == NOW - 100_000
    assert reader.sent == [] and stored == []
    relay._relay(room_type, [sender, reader], sender, _cipher())
    assert sender.attachment["last"] == NOW
    assert len(reader.sent) == 1


def test_history_outside_world_general_needs_an_account():
    _namespace, room_type, stored = relay._harness()
    guest = relay._Socket("g")
    guest.attachment["room"] = GENERAL
    reader = relay._Socket("r")
    reader.attachment["room"] = GENERAL
    relay._relay(room_type, [guest, reader], guest, _cipher())
    assert len(reader.sent) == 1
    assert stored == []

    member = relay._Socket("m", account_bi=ALICE)
    member.attachment["room"] = GENERAL
    relay._relay(room_type, [member, reader], member, _cipher(n=1))
    assert [row[0] for row in stored] == [GENERAL]

    world_guest = relay._Socket("w")
    relay._relay(room_type, [world_guest], world_guest, _cipher(n=2))
    assert [row[0] for row in stored] == [GENERAL, WORLD_GENERAL]


class _OpenSocket(relay._Socket):
    def __init__(self, socket_id, address, state=1):
        super().__init__(socket_id)
        self.attachment["addr"] = address
        self.readyState = state


def _address_room(sockets):
    namespace, room_type, _stored = relay._harness()
    namespace.update({
        "ROOM_SOCKETS_PER_ADDRESS": 8,
        "CHAT_SOCKETS_PER_ADDRESS": 32,
        "CHAT_SOCKET_ADDRESS_MEMO_MAX": 4096,
        "_CHAT_SOCKET_ADDRESSES": {},
    })
    _functions({"_chat_socket_open"}, namespace)
    room = room_type()
    room.env = object()
    room.ctx = relay._Context(sockets)
    return namespace, room


def test_one_address_holds_at_most_eight_sockets_in_a_room():
    address = "a" * 32
    sockets = [_OpenSocket(str(i), address) for i in range(7)]
    namespace, room = _address_room(sockets)
    assert room._address_admitted(address) is True
    assert room._address_admitted("") is True
    sockets.append(_OpenSocket("7", address))
    assert room._address_admitted(address) is False
    assert room._address_admitted("b" * 32) is True
    sockets[0].attachment["last"] = NOW - 10 * 60 * 1000
    assert room._address_admitted(address) is True
    assert namespace["_CHAT_SOCKET_ADDRESSES"] == {}


def test_one_address_holds_at_most_thirty_two_sockets_per_isolate():
    address = "c" * 32
    namespace, room = _address_room([])
    elsewhere = [_OpenSocket(str(i), address) for i in range(32)]
    namespace["_CHAT_SOCKET_ADDRESSES"][address] = list(elsewhere)
    assert room._address_admitted(address) is False
    elsewhere[0].readyState = 3
    assert room._address_admitted(address) is True
    assert len(namespace["_CHAT_SOCKET_ADDRESSES"][address]) == 31


def test_the_socket_address_is_a_keyed_token_and_ipv6_is_cut_to_its_64():
    namespace = _functions({"_chat_socket_address", "_require_data_secret"}, {
        "hmac": hmac, "ipaddress": ipaddress,
        "_INSECURE_DATA_KEYS": frozenset({""}),
    })

    class _Env:
        DATA_KEY = "unit-test-secret"

    class _Request:
        def __init__(self, ip):
            self.headers = {"cf-connecting-ip": ip}

    token = namespace["_chat_socket_address"]
    first = token(_Env(), _Request("2001:db8:1:2::5"))
    assert re.fullmatch(r"[0-9a-f]{32}", first)
    assert first == token(_Env(), _Request("2001:db8:1:2:ffff::9"))
    assert first != token(_Env(), _Request("2001:db8:1:3::5"))
    assert token(_Env(), _Request("192.0.2.1")) != token(
        _Env(), _Request("192.0.2.2"))
    assert "192.0.2.1" not in token(_Env(), _Request("192.0.2.1"))
    assert token(_Env(), _Request("::ffff:192.0.2.1")) == token(
        _Env(), _Request("192.0.2.1"))
    assert token(_Env(), _Request("")) == ""
    assert token(_Env(), _Request("not-an-ip")) == ""


def _room_key_from_path():
    namespace = {
        "CHAT_DIRECT_MESSAGE_DO_RE": relay._harness()[0][
            "CHAT_DIRECT_MESSAGE_DO_RE"],
        "CHAT_CHANNEL_DO_RE": relay._harness()[0]["CHAT_CHANNEL_DO_RE"],
        "REPO_ROOM_RE": re.compile(
            r"^/api/repo/([^/]+)/([^/]+)/rooms/([^/]+)/(?:ws|clients)$"),
        "CHAT_RELAY_OWNER": "mainnode",
        "CHAT_RELAY_REPO": "forkmesh",
        "CHAT_RELAY_ROOMS": frozenset(
            {"world-general", "general", "world-sky-campus"}),
        "MAX_ROOM_NAME": 80,
        "re": re,
        "safe_segment": lambda value, _limit=80: (
            str(value or "") if re.fullmatch(
                r"[A-Za-z0-9._-]+", str(value or "")) else ""),
    }
    return _functions({"room_key_from_path"}, namespace)["room_key_from_path"]


def test_the_relay_admits_only_the_rooms_it_serves():
    room_key = _room_key_from_path()
    for name in ("world-general", "general", "world-sky-campus"):
        info = room_key(
            "/api/repo/mainnode/forkmesh/rooms/" + name + "/ws", ALICE)
        assert info["key"] == "repo:mainnode/forkmesh:room:" + name
        assert info["account_bi"] == ALICE
    assert "account_bi" not in room_key(
        "/api/repo/mainnode/forkmesh/rooms/general/ws", "forged")
    for path in (
        "/api/repo/mainnode/forkmesh/rooms/secret-team/ws",
        "/api/repo/mainnode/forkmesh/rooms/General/ws",
        "/api/repo/alice/demo/rooms/general/ws",
        "/api/repo/MainNode/forkmesh/rooms/general/ws",
        "/api/room/general/ws",
        "/api/room/world-general/ws",
    ):
        assert room_key(path) is None, path
    assert room_key(
        "/api/chat/channels/" + "1" * 32 + "/v1/ws")["channel_id"] == "1" * 32


def test_the_legacy_room_route_is_gone_and_unknown_rooms_404():
    constants = {
        node.value
        for node in ast.walk(ENTRY_TREE)
        if isinstance(node, ast.Constant) and isinstance(node.value, str)
    }
    assert "ROOM_RE" not in constants
    assert "\nROOM_RE = " not in URLS_TEXT
    assert "/api/room/" not in URLS_TEXT
    assert "/api/room/" not in APP_RUNTIME_TEXT
    assert "compatWebsocket" not in APP_RUNTIME_TEXT
    route = APP_RUNTIME_TEXT[APP_RUNTIME_TEXT.index(
        "room = room_key_from_path(url.path)"):]
    route = route[:route.index("log_durable_object_abort")]
    assert route.index("REPO_ROOM_RE.match(url.path)") < route.index(
        "idFromName")
    assert '{"error": "not_found"}, status=404' in route
    assert "_room_key_requester(" in route
    assert "target_url=socket_target" in route


def test_the_handoff_accepts_only_server_built_socket_queries():
    namespace = _functions({"_chat_socket_query_ok"}, {"re": re})
    ok = namespace["_chat_socket_query_ok"]
    assert ok({}) is True
    assert ok({"account": [ALICE], "addr": ["a" * 32]}) is True
    assert ok({"addr": ["a" * 32]}, account_required=True) is False
    assert ok({"ticket": ["x"]}) is False
    assert ok({"node": ["alice"], "sig": ["x"]}) is False
    assert ok({"account": [ALICE, BOB]}) is False
    assert ok({"addr": ["A" * 32]}) is False


def test_client_query_strings_never_reach_the_room_object():
    namespace = _functions({"_chat_socket_target"}, {
        "re": re, "urlparse": urlparse})

    class _Request:
        url = ("https://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/"
               "general/ws?account=" + BOB + "&addr=" + "f" * 32
               + "&node=alice&sig=x")

    target = namespace["_chat_socket_target"](
        _Request(), account_bi=ALICE, address="a" * 32)
    parsed = urlparse(target)
    assert parsed.path == "/api/repo/mainnode/forkmesh/rooms/general/ws"
    assert parse_qs(parsed.query) == {"account": [ALICE], "addr": ["a" * 32]}
    bare = urlparse(namespace["_chat_socket_target"](_Request()))
    assert bare.query == ""


class _Db:
    def __init__(self):
        self.conn = sqlite3.connect(":memory:")
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript("""
            CREATE TABLE account_devices (
                device_bi TEXT PRIMARY KEY, account_bi TEXT, pubkey TEXT,
                kind TEXT, label TEXT, capabilities TEXT, enabled INTEGER,
                created_at INTEGER, last_seen INTEGER, revoked_at INTEGER);
            CREATE TABLE nodes (
                node_bi TEXT PRIMARY KEY, user_bi TEXT, pubkey TEXT);
        """)

    async def d1_all(self, _env, sql, *args):
        return [dict(row) for row in self.conn.execute(sql, args)]

    async def d1_first(self, _env, sql, *args):
        row = self.conn.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, _env, sql, *args):
        self.conn.execute(sql, args)


NODE_FUNCTIONS = {
    "chat_node_owner_account", "chat_node_owner_handler",
    "chat_node_bind_handler", "_chat_node_rate_ok", "_chat_node_bind_ts",
    "_register_account_device", "_account_device_for_pubkey",
    "_account_devices_list",
}


def _node_harness(accounts=None):
    db = _Db()
    clock = [NOW]
    audits = []
    accounts = accounts or {
        "alice": {"name": "alice", "status": "active"},
        "bob": {"name": "bob", "status": "active"},
        "bot": {"name": "bot", "status": "active", "kind": "bot"},
    }
    sessions = {"alice-token": "alice", "bob-token": "bob"}

    class _Date:
        @staticmethod
        def now():
            return clock[0]

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def account_rec(_env, account_bi):
        return accounts.get(str(account_bi).removeprefix("bi:"))

    async def session_lookup(_env, token, touch=True):
        name = sessions.get(token)
        if not name:
            return "", None, ""
        return "bi:" + name, accounts[name], "session-" + name

    async def bounded_json_request(request):
        return json.loads(request.body)

    async def audit(*args, **_kwargs):
        audits.append(args)

    def json_response(data, status=200, **kwargs):
        return {"status": status, "data": data, **kwargs}

    namespace = {
        "Date": _Date,
        "CHAT_NODE_DEVICE_KIND": "chat_node",
        "CHAT_NODE_CAPABILITIES": "chat_node",
        "CHAT_NODE_BIND_PROOF": "forkmesh-chat-node-bind-v1",
        "CHAT_NODE_MAX_DEVICES": 20,
        "CHAT_NODE_OWNER_WINDOW_MS": 60_000,
        "CHAT_NODE_OWNER_MAX_PER_WINDOW": 3,
        "CHAT_NODE_OWNER_RATE_MEMO_MAX": 4096,
        "_CHAT_NODE_OWNER_RATE": {},
        "DESKTOP_NODE_CAPABILITIES": "owner_sign",
        "MOBILE_NODE_CAPABILITIES": "owner_sign",
        "BROWSER_CONTROL_CAPABILITIES": "desktop_control",
        "CLIENT_CAPABILITIES": "browse",
        "CONTRIBUTION_KEY_PROOF_CAPABILITY": "contribution_key_proof",
        "MAX_NODE_NAME": 63,
        "d1_all": db.d1_all,
        "d1_first": db.d1_first,
        "d1_run": db.d1_run,
        "blind_index": blind_index,
        "_account_identity_rec_by_bi": account_rec,
        "_account_kind": lambda rec: rec.get("kind", "user"),
        "_account_session_lookup": session_lookup,
        "_account_session_invalid_error": (
            lambda _env, _token: asyncio.sleep(0, result="invalid_session")),
        "_request_account_session_token": lambda request, data: (
            request.headers.get("authorization", "")[7:], False),
        "_request_origin_allowed": lambda _env, _request: True,
        "_audit_sensitive_action": audit,
        "_chat_socket_address": lambda _env, request: request.headers.get(
            "addr", ""),
        "_ts_ok": lambda ts: abs(clock[0] - int(ts)) <= 5 * 60 * 1000,
        "bounded_json_request": bounded_json_request,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "ed25519_verify": _ed25519_verify,
        "ensure_schema": lambda _env: asyncio.sleep(0),
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "valid_node_name": lambda value: bool(re.fullmatch(
            r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", str(value or ""))),
        "valid_node_pubkey": lambda value: bool(re.fullmatch(
            r"[A-Za-z0-9_-]{43}", str(value or ""))),
    }
    _functions(NODE_FUNCTIONS, namespace)
    return namespace, db, clock, audits


class _HttpRequest:
    def __init__(self, method="GET", url="https://app.test/", body="",
                 headers=None):
        self.method = method
        self.url = url
        self.body = body
        self.headers = dict(headers or {})


def _owner(namespace, node, addr="x"):
    return asyncio.run(namespace["chat_node_owner_handler"](
        object(), _HttpRequest(
            url="https://app.test/api/chat/node-owner?node=" + node,
            headers={"addr": addr})))


def _bind(namespace, account, private, node, ts, token=None, sig=None):
    canonical = (
        "forkmesh-chat-node-bind-v1\n" + account + "\n" + node + "\n"
        + str(ts)).encode()
    body = json.dumps({
        "node": node, "ts": ts,
        "sig": sig if sig is not None else _b64u(private.sign(canonical)),
    })
    return asyncio.run(namespace["chat_node_bind_handler"](
        object(), _HttpRequest(
            method="POST", url="https://app.test/api/chat/node-bind",
            body=body, headers={
                "authorization": "Bearer " + (token or account + "-token")})))


def test_node_owner_answers_from_phone_desktop_and_browser_binds():
    namespace, db, _clock, _audits = _node_harness()
    _phone_key, phone = _node_key(1)
    _desktop_key, desktop = _node_key(2)
    _other_key, unbound = _node_key(3)
    db.conn.execute(
        "INSERT INTO account_devices VALUES (?,?,?,?,?,?,?,?,?,?)",
        ("d1", "bi:alice", phone, "mobile_node", "", "owner_sign", 1, 1, 1, 0))
    db.conn.execute("INSERT INTO nodes VALUES (?,?,?)",
                    ("bi:bob", "", desktop))
    assert _owner(namespace, phone) == {
        "status": 200, "data": {"account": "alice"},
        "cache_control": "private, max-age=600"}
    assert _owner(namespace, desktop, "y")["data"] == {"account": "bob"}
    assert _owner(namespace, unbound, "z")["data"] == {"account": ""}
    assert _owner(namespace, "short", "z")["status"] == 400

    db.conn.execute("UPDATE account_devices SET revoked_at=5")
    assert _owner(namespace, phone, "w")["data"] == {"account": ""}

    db.conn.execute(
        "INSERT INTO account_devices VALUES (?,?,?,?,?,?,?,?,?,?)",
        ("d2", "bi:alice", desktop, "desktop_node", "", "", 1, 1, 1, 0))
    assert _owner(namespace, desktop, "v")["data"] == {"account": ""}

    db.conn.execute(
        "INSERT INTO account_devices VALUES (?,?,?,?,?,?,?,?,?,?)",
        ("d3", "bi:bot", unbound, "mobile_node", "", "", 1, 1, 1, 0))
    assert _owner(namespace, unbound, "u")["data"] == {"account": ""}
    _control_key, control = _node_key(7)
    db.conn.execute(
        "INSERT INTO account_devices VALUES (?,?,?,?,?,?,?,?,?,?)",
        ("d4", "bi:alice", control, "browser_control", "", "", 1, 1, 1, 0))
    assert _owner(namespace, control, "t")["data"] == {"account": ""}


def test_node_owner_is_rate_limited_per_address():
    namespace, _db, clock, _audits = _node_harness()
    _key, node = _node_key(4)
    for _ in range(3):
        assert _owner(namespace, node, "same")["status"] == 200
    limited = _owner(namespace, node, "same")
    assert limited["status"] == 429
    assert _owner(namespace, node, "other")["status"] == 200
    clock[0] += 60_000
    assert _owner(namespace, node, "same")["status"] == 200


def test_a_browser_binds_its_node_key_with_a_fresh_signature():
    namespace, db, clock, audits = _node_harness()
    private, node = _node_key(5)
    bound = _bind(namespace, "alice", private, node, NOW)
    assert bound["status"] == 200
    assert bound["data"] == {"ok": True, "account": "alice", "node": node}
    row = db.conn.execute(
        "SELECT account_bi,kind,capabilities,enabled FROM account_devices"
    ).fetchone()
    assert tuple(row) == ("bi:alice", "chat_node", "chat_node", 1)
    assert len(audits) == 1
    assert _owner(namespace, node)["data"] == {"account": "alice"}
    assert _bind(namespace, "alice", private, node, NOW)["status"] == 200
    assert len(audits) == 1

    other, other_node = _node_key(6)
    assert _bind(namespace, "bob", other, other_node, NOW - 6 * 60 * 1000)[
        "data"] == {"error": "stale_request"}
    assert _bind(namespace, "bob", other, other_node, str(NOW))[
        "status"] == 200
    assert _bind(namespace, "bob", other, other_node, True)[
        "data"] == {"error": "stale_request"}
    forged = _bind(namespace, "bob", private, other_node, NOW)
    assert forged["data"] == {"error": "invalid_signature"}
    signed_for_alice = (
        "forkmesh-chat-node-bind-v1\nalice\n" + node + "\n" + str(NOW))
    replayed = _bind(namespace, "bob", private, node, NOW, sig=_b64u(
        private.sign(signed_for_alice.encode())))
    assert replayed["data"] == {"error": "invalid_signature"}
    conflict = _bind(namespace, "bob", private, node, NOW)
    assert conflict["status"] == 409
    assert conflict["data"] == {"error": "node_key_conflict"}
    anonymous = _bind(namespace, "alice", private, node, NOW, token="none")
    assert anonymous["status"] == 401
    clock[0] += 1


def test_a_browser_bind_keeps_at_most_twenty_chat_nodes():
    namespace, db, clock, _audits = _node_harness()
    nodes = []
    for seed in range(10, 32):
        private, node = _node_key(seed)
        clock[0] += 1000
        assert _bind(namespace, "alice", private, node, clock[0])[
            "status"] == 200
        nodes.append(node)
    live = [row[0] for row in db.conn.execute(
        "SELECT pubkey FROM account_devices WHERE enabled=1 "
        "ORDER BY last_seen")]
    assert len(live) == 20
    assert live == nodes[-20:]
    assert _owner(namespace, nodes[0])["data"] == {"account": ""}


class _BlockRuntime(DmRuntime):
    def __init__(self):
        super().__init__()
        self.request_body = {}


def _blocks(runtime, method, actor, data=None):
    return asyncio.run(dm_api.handle_blocks(
        runtime.use(method, actor, data), "/api/chat/blocks"))


def test_the_block_list_is_kept_sealed_per_account():
    runtime = _BlockRuntime()
    assert _blocks(runtime, "GET", "alice")["data"] == {"accounts": []}
    saved = _blocks(runtime, "PUT", "alice", {
        "accounts": [" Bob ", "carol", "bob", "alice", "nobody-here"]})
    assert saved["status"] == 200
    assert saved["data"] == {
        "ok": True, "accounts": ["bob", "carol", "nobody-here"]}
    row = runtime.db.execute(
        "SELECT account_bi,data FROM chat_blocks").fetchone()
    assert row["account_bi"] == "bi:alice"
    assert json.loads(row["data"]) == {
        "accounts": ["bob", "carol", "nobody-here"]}
    assert _blocks(runtime, "GET", "alice")["data"] == {
        "accounts": ["bob", "carol", "nobody-here"]}
    assert _blocks(runtime, "GET", "bob")["data"] == {"accounts": []}

    assert _blocks(runtime, "PUT", "alice", {"accounts": []})["data"] == {
        "ok": True, "accounts": []}
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_blocks").fetchone()[0] == 0

    for bad in ({"accounts": "bob"}, {"accounts": ["bad name"]},
                {"accounts": [7]}, {}):
        refused = _blocks(runtime, "PUT", "alice", bad)
        assert refused["status"] == 400
        assert refused["data"] == {"error": "invalid_accounts"}
    too_many = _blocks(runtime, "PUT", "alice", {
        "accounts": ["user%d" % index for index in range(501)]})
    assert too_many["data"] == {"error": "too_many_accounts"}
    full = _blocks(runtime, "PUT", "alice", {
        "accounts": ["user%d" % index for index in range(500)]})
    assert len(full["data"]["accounts"]) == 500
    assert _blocks(runtime, "GET", "")["status"] == 401
    assert _blocks(runtime, "POST", "alice")["status"] == 405


def test_someone_who_blocked_you_looks_like_a_missing_account():
    runtime = _BlockRuntime()
    _blocks(runtime, "PUT", "bob", {"accounts": ["alice"]})
    blocked = asyncio.run(dm_api.handle(
        runtime.use("POST", "alice", {"username": "bob"}),
        "/api/chat/direct-messages"))
    missing = asyncio.run(dm_api.handle(
        runtime.use("POST", "alice", {"username": "zed"}),
        "/api/chat/direct-messages"))
    assert blocked == missing
    assert blocked["status"] == 404
    assert blocked["data"] == {"error": "user_not_found"}

    opened = asyncio.run(dm_api.handle(
        runtime.use("POST", "bob", {"username": "alice"}),
        "/api/chat/direct-messages"))
    assert opened["status"] == 201
    again = asyncio.run(dm_api.handle(
        runtime.use("POST", "alice", {"username": "bob"}),
        "/api/chat/direct-messages"))
    assert again["status"] == 404


def _push_harness(blocks):
    pushes = []

    async def d1_first(_env, sql, *args):
        if "FROM chat_blocks" in sql:
            accounts = blocks.get(args[0])
            return {"data": json.dumps({"accounts": accounts})} if (
                accounts is not None) else None
        return {"data": json.dumps({"participants": ["alice", "bob"]})}

    async def decrypt_row(_env, data):
        return json.loads(data)

    async def blind_index(_env, name):
        return {"alice": ALICE, "bob": BOB}.get(name, "bi:" + name)

    async def notify(_env, owner, topic, alert=None):
        pushes.append((owner, topic, alert))

    async def batch(_env, _statements):
        return None

    class _Apns:
        @staticmethod
        def direct_message_alert(**kwargs):
            return {"kind": "message", **kwargs}

        @staticmethod
        def direct_call_alert(**kwargs):
            return {"kind": "call", **kwargs}

    namespace = _functions({
        "_chat_direct_message_retained", "_chat_direct_call_ring",
        "_chat_direct_message_parties", "_chat_blocked_by",
    }, {
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "blind_index": blind_index,
        "notify_account_event": notify,
        "_contribution_run_batch": batch,
        "_apns_module": lambda: _Apns,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(value),
        "MAX_NODE_NAME": 63,
        "_CHAT_DIRECT_PEER_MEMO": {},
        "CHAT_DIRECT_PEER_MEMO_MAX": 256,
    })
    return namespace, pushes


def test_no_message_push_call_ring_or_voip_from_a_blocked_account():
    namespace, pushes = _push_harness({BOB: ["alice"]})
    asyncio.run(namespace["_chat_direct_message_retained"](
        object(), relay.DIRECT, ALICE, NOW, ref="r" * 40))
    asyncio.run(namespace["_chat_direct_call_ring"](
        object(), relay.DIRECT, ALICE, call_id="c"))
    assert pushes == []

    namespace, pushes = _push_harness({BOB: ["carol"]})
    asyncio.run(namespace["_chat_direct_message_retained"](
        object(), relay.DIRECT, ALICE, NOW, ref="r" * 40))
    asyncio.run(namespace["_chat_direct_call_ring"](
        object(), relay.DIRECT, ALICE, call_id="c"))
    assert [(owner, topic) for owner, topic, _alert in pushes] == [
        ("bob", "direct-messages"), ("bob", "calls")]
    assert "caller" not in pushes[1][2]

    namespace, pushes = _push_harness({ALICE: ["bob"]})
    asyncio.run(namespace["_chat_direct_call_ring"](
        object(), relay.DIRECT, ALICE, call_id="c"))
    assert [owner for owner, _topic, _alert in pushes] == ["bob"]


def test_desktops_sign_their_block_list_reads_and_writes():
    source = ast.unparse(next(
        node for node in ENTRY_TREE.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_chat_direct_signed_session"))
    assert "CHAT_BLOCKS_LIST_PROOF" in source
    assert "chat_direct_messages_api.blocks_digest(accounts)" in source
    assert 'CHAT_BLOCKS_LIST_PROOF = "forkmesh-chat-blocks-list-v1"' in (
        ENTRY_TEXT)
    assert 'CHAT_BLOCKS_PUT_PROOF = "forkmesh-chat-blocks-put-v1"' in (
        ENTRY_TEXT)
    assert dm_api.blocks_digest(["bob", "carol"]) == hashlib.sha256(
        b"bob\ncarol").hexdigest()


def test_chat_routes_are_wired():
    for path in ("/api/chat/node-owner", "/api/chat/node-bind"):
        assert '"' + path + '"' in APP_RUNTIME_TEXT
    assert "CHAT_BLOCKS_RE.match(url.path)" in APP_RUNTIME_TEXT
    assert "chat_direct_messages_api.handle_blocks(" in APP_RUNTIME_TEXT
    assert 'CHAT_BLOCKS_RE = re.compile(r"^/api/chat/blocks/?$")' in URLS_TEXT
    assert '"CHAT_BLOCKS_RE"' in ENTRY_TEXT


def test_capability_tokens_travel_only_where_the_access_log_redacts():
    render = (ROOT.parent / "deploy" / "core" / "render.py").read_text(
        encoding="utf-8")
    keys = next(
        ast.literal_eval(node.value)
        for node in ast.parse(render).body
        if isinstance(node, ast.Assign)
        and any(getattr(target, "id", "") == "ACCESS_LOG_QUERY_KEYS"
                for target in node.targets))
    assert "token" in keys
    source = ast.unparse(next(
        node for node in ast.walk(ENTRY_TREE)
        if isinstance(node, ast.FunctionDef) and node.name == "webhook_url"))
    assert "'/api/chat/webhooks?token='" in source
    urls = URLS_TEXT[URLS_TEXT.index("CHAT_WEBHOOK_DELIVERY_RE"):]
    assert "[0-9a-f]{64})/" not in urls[:urls.index("CHAT_CHANNEL_WS_RE")]
