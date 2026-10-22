import ast
import base64
import hashlib
import hmac
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))

import test_chat_channels_api as channels  # noqa: E402

ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
SECRET = "browser-test-secret-0123456789abcdef"
CHANNEL = "0123456789abcdef0123456789abcdef"
GO_STANDBY_TICKET = (
    "eyJyb29tIjoiMjA5MDU0YzFjNjhkMzI1YzE0OWQ2NjdhZGMwZGY1ZjYwYjBmMGIzNTE0Yz"
    "Y1MWVhOGJiODhkNzE1NjQwYjk1ZSIsInBlZXIiOiJhYmFiYWJhYmFiYWJhYmFiYWJhYmFi"
    "YWJhYmFiYWJhYiIsImFjY3QiOiJkYjQ0MGY1MDE3YTc1YjNjNzM3OTA3ZTIzYzNkNzhmMS"
    "IsImV4cCI6MTgwMDAwMDA2MCwiZW5naW5lIjoiZm1tIiwic3RhbmRieSI6dHJ1ZX0."
    "PEyg510KisogiG5OcGiNFb8xW1ghm-tRorgTogp6n6g")
NODES = ["wss://n%d.next.forkmesh.com/calls/ws" % n for n in (2, 3, 4)]
HASH_A = base64.b64encode(b"a" * 32).decode()
HASH_B = base64.b64encode(b"b" * 32).decode()
NAMES = ("chat_call_room", "chat_call_ticket", "chat_call_account",
         "chat_call_pick", "chat_call_rank", "chat_call_fm_media_info",
         "chat_call_host", "chat_call_sign", "chat_call_fm_ticket",
         "chat_call_fm_media", "chat_call_nodes")


def _functions():
    namespace = {"base64": base64, "hmac": hmac, "json": json,
                 "hashlib": hashlib}
    for node in ast.parse(ENTRY).body:
        named = isinstance(node, ast.FunctionDef) and node.name in NAMES
        constant = isinstance(node, ast.Assign) and any(
            getattr(target, "id", "").startswith("CHAT_CALL_")
            for target in node.targets)
        if named or constant:
            exec(compile(ast.Module([node], []), "entry.py", "exec"), namespace)
    return namespace


def _decode(ticket):
    payload, signature = ticket.split(".")
    expected = hmac.new(SECRET.encode(),
                        ("forkmesh-call-ticket-v1\n" + payload).encode(),
                        "sha256").digest()
    assert base64.urlsafe_b64decode(signature + "=" * (-len(signature) % 4)) == expected
    return json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))


def _info():
    return {node: {"port": 4433, "hashes": [HASH_A, HASH_B]} for node in NODES}


def test_the_standby_ticket_is_the_one_the_calls_server_verifies():
    f = _functions()
    room = f["chat_call_room"](SECRET, CHANNEL)
    account = f["chat_call_account"](SECRET, "c" * 64)
    assert account == "db440f5017a75b3c737907e23c3d78f1"
    standby = f["chat_call_fm_ticket"](
        SECRET, room, "ab" * 16, account, 1_800_000_000_000, True)
    assert standby == GO_STANDBY_TICKET
    primary = _decode(f["chat_call_fm_ticket"](
        SECRET, room, "ab" * 16, account, 1_800_000_000_000, False))
    assert primary == {"room": room, "peer": "ab" * 16, "acct": account,
                       "exp": 1_800_000_060, "engine": "fmm"}


def test_fm_media_lands_on_the_webrtc_core_and_names_the_next_one_as_standby():
    f = _functions()
    rooms = [f["chat_call_room"](SECRET, "%032x" % n) for n in range(40)]
    seen = set()
    for room in rooms:
        access = f["chat_call_fm_media"](
            SECRET, room, "ab" * 16, "cd" * 16, 1_800_000_000_000,
            NODES, set(NODES), _info())
        primary = f["chat_call_pick"](room, NODES, set(NODES))
        host = f["chat_call_host"](primary)
        assert access["engine"] == "fmm"
        assert access["url"] == "https://%s:4433/calls/fmm" % host
        assert access["addr"] == host + ":4433"
        assert access["alpn"] == "fmm/1"
        assert access["hashes"] == [HASH_A, HASH_B]
        assert _decode(access["ticket"])["engine"] == "fmm"
        assert "standby" not in _decode(access["ticket"])
        standby = access["standby"]
        assert _decode(standby["ticket"])["standby"] is True
        assert _decode(standby["ticket"])["peer"] == "ab" * 16
        survivors = set(NODES) - {primary}
        moved = f["chat_call_pick"](room, NODES, survivors)
        assert standby["addr"] == f["chat_call_host"](moved) + ":4433"
        seen.add(standby["addr"])
    assert len(seen) == 3


def test_no_fm_media_without_it_on_the_picked_core():
    f = _functions()
    room = f["chat_call_room"](SECRET, CHANNEL)
    ranked = f["chat_call_rank"](room, NODES, set(NODES))
    assert ranked[0] == f["chat_call_pick"](room, NODES, set(NODES))
    info = _info()
    del info[ranked[0]]
    assert f["chat_call_fm_media"](
        SECRET, room, "ab" * 16, "cd" * 16, 0, NODES, set(NODES), info) is None
    info = _info()
    del info[ranked[1]]
    access = f["chat_call_fm_media"](
        SECRET, room, "ab" * 16, "cd" * 16, 0, NODES, set(NODES), info)
    assert access and "standby" not in access
    alone = f["chat_call_fm_media"](
        SECRET, room, "ab" * 16, "cd" * 16, 0, NODES, {ranked[0]}, _info())
    assert alone and "standby" not in alone
    assert f["chat_call_fm_media"](
        SECRET, room, "ab" * 16, "cd" * 16, 0, [], set(), {}) is None


def test_health_info_is_checked_before_it_reaches_a_client():
    parse = _functions()["chat_call_fm_media_info"]
    good = {"ok": True, "fmm": {"port": 4433, "path": "/calls/fmm",
                                "alpn": "fmm/1", "hashes": [HASH_A, HASH_B]}}
    assert parse(good) == {"port": 4433, "hashes": [HASH_A, HASH_B]}
    assert parse({"ok": True}) is None
    assert parse(None) is None
    for key, value in (("port", True), ("port", 0), ("port", 70000),
                       ("port", "4433"), ("path", "/other"), ("alpn", "h3"),
                       ("hashes", []), ("hashes", HASH_A),
                       ("hashes", [HASH_A] * 5),
                       ("hashes", [base64.b64encode(b"x" * 31).decode()]),
                       ("hashes", ["not base64!"]), ("hashes", [7])):
        bad = json.loads(json.dumps(good))
        bad["fmm"][key] = value
        assert parse(bad) is None, (key, value)


class CallRuntime(channels.FakeRuntime):
    def __init__(self):
        super().__init__()
        self.fm_calls = []

    async def call_ticket(self, channel_id, account_bi):
        return {"ticket": "webrtc-ticket", "room": "r" * 64, "peer": "p" * 32,
                "url": NODES[0]}

    async def call_fm_media(self, channel_id, account_bi, ticket):
        self.fm_calls.append((channel_id, account_bi, ticket["peer"]))
        return {"engine": "fmm", "url": "https://n2.next.forkmesh.com:4433/calls/fmm",
                "ticket": "fmm-ticket"}


@channels.run_async_test
async def test_old_clients_get_the_webrtc_response_unchanged():
    runtime = CallRuntime()
    created = await channels.create_channel(runtime, "call-room",
                                            members=["alice"])
    channel = created["data"]["channel"]["id"]
    path = "/api/chat/channels/%s/call-ticket" % channel
    plain = await channels.api.handle(runtime.use("POST", "alice", {}), path)
    assert plain["status"] == 200
    assert plain["data"] == {"ticket": "webrtc-ticket", "room": "r" * 64,
                             "peer": "p" * 32, "url": NODES[0]}
    webrtc_only = await channels.api.handle(
        runtime.use("POST", "alice", {"engines": ["webrtc"]}), path)
    assert "fmm" not in webrtc_only["data"]
    junk = await channels.api.handle(
        runtime.use("POST", "alice", {"engines": "fmm"}), path)
    assert "fmm" not in junk["data"]
    assert runtime.fm_calls == []
    both = await channels.api.handle(
        runtime.use("POST", "alice", {"engines": ["fmm", "webrtc"]}), path)
    assert both["status"] == 200
    assert both["data"]["ticket"] == "webrtc-ticket"
    assert both["data"]["url"] == NODES[0]
    assert both["data"]["fmm"]["ticket"] == "fmm-ticket"
    assert runtime.fm_calls == [(channel, "bi:alice", "p" * 32)]


def test_runtime_reuses_the_probe_and_the_webrtc_peer():
    runtime = ENTRY[ENTRY.index("    async def call_fm_media(self, channel_id, account_bi, ticket):"):]
    runtime = runtime[:runtime.index("\n\n")]
    assert "await _chat_call_healthy(nodes)" in runtime
    assert 'ticket["room"], ticket["peer"]' in runtime
    assert "_chat_call_fm_info(nodes)" in runtime
    probe = ENTRY[ENTRY.index("async def _chat_call_probe(node):"):]
    probe = probe[:probe.index("\n\n\n")]
    assert "chat_call_fm_media_info(" in probe
    assert "CHAT_CALL_HEALTH_TIMEOUT_SECONDS" in probe
