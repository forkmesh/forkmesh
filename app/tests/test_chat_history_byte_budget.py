"""Encrypted chat retention stays below D1 row and per-room byte limits."""

import ast
import asyncio
import base64
import json
import os
from pathlib import Path
import sqlite3


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
SOURCE = ENTRY.read_text(encoding="utf-8")


def _history_helpers(db):
    names = {
        "_chat_history_prune_bounds",
        "chat_history_store",
    }
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name in names
    ]
    found = {node.name for node in selected}
    assert found == names, "missing: %s" % sorted(names - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    namespace = {
        "CHAT_HISTORY_MAX_BYTES_PER_ROOM": 20,
        "CHAT_HISTORY_MAX_PER_ROOM": 3,
        "d1_run": d1_run,
        "ensure_schema": lambda _env: asyncio.sleep(0),
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _room_class(namespace):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.ClassDef) and item.name == "ForkMeshRoom"
    )
    module = ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[]))
    globals_map = {"DurableObject": object, **namespace}
    exec(compile(module, str(ENTRY), "exec"), globals_map)
    return globals_map["ForkMeshRoom"]


def _entry_constants(names):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    return {
        node.targets[0].id: eval(
            compile(ast.Expression(node.value), str(ENTRY), "eval"),
            {"__builtins__": {}},
        )
        for node in tree.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id in names
    }


def _b64(size):
    return base64.b64encode(os.urandom(size)).decode()


def _b64u(size):
    return base64.urlsafe_b64encode(os.urandom(size)).decode().rstrip("=")


def _v3_relay_frame(plain_bytes, recipients):
    sealed = {
        "type": "sealed", "v": 3, "kid": _b64u(12), "idk": _b64u(32),
        "ctr": 4_294_967_296, "ts": 1_800_000_000_000,
        "nonce": _b64(12), "tag": _b64(16), "body": _b64(plain_bytes),
        "keys": [
            {"kid": _b64u(12), "spk": _b64u(12), "eph": _b64u(32),
             "ct": _b64(1088), "nonce": _b64(12), "tag": _b64(16),
             "key": _b64(32)}
            for _ in range(recipients)
        ],
        "sig": _b64u(64),
    }
    inner = json.dumps(sealed, separators=(",", ":")).encode()
    envelope = {
        "kind": "cipher", "v": 1, "nonce": _b64(12), "tag": _b64(16),
        "body": _b64(len(inner)), "persist": True, "notify": True,
        "ref": "f" * 40, "recipients": 1, "recipientNames": ["x" * 63] * 20,
    }
    return len(json.dumps(envelope, separators=(",", ":")).encode())


def _v3_file_plain_bytes(file_bytes):
    plain = {
        "type": "chat", "id": "m" * 36, "senderId": "s" * 64,
        "sender": "n" * 63, "text": "t" * 2000, "ts": 1_800_000_000_000,
        "fileName": "f" * 120 + ".jpg", "fileMime": "image/jpeg",
        "file": _b64(file_bytes),
    }
    return len(json.dumps(plain, separators=(",", ":")).encode())


def test_attachment_frame_ceiling_bounds_retention():
    values = _entry_constants({
        "MAX_TEXT_BYTES",
        "CHAT_HISTORY_MAX_BODY",
        "CHAT_HISTORY_MAX_BYTES_PER_ROOM",
        "CHAT_HISTORY_REPLAY_MAX_MESSAGES",
        "CHAT_HISTORY_REPLAY_MAX_BYTES",
        "CHAT_HISTORY_INGRESS_MAX_BYTES",
        "CHAT_HISTORY_INGRESS_WINDOW_MS",
    })
    assert values["CHAT_HISTORY_MAX_BODY"] <= values["MAX_TEXT_BYTES"]
    assert values["CHAT_HISTORY_MAX_BYTES_PER_ROOM"] == 16 * 1024 * 1024
    assert 1 <= values["CHAT_HISTORY_REPLAY_MAX_MESSAGES"] <= 100
    assert (values["CHAT_HISTORY_MAX_BODY"]
            <= values["CHAT_HISTORY_REPLAY_MAX_BYTES"] <= 4 * 1024 * 1024)
    assert values["CHAT_HISTORY_INGRESS_MAX_BYTES"] <= 8 * 1024 * 1024
    assert values["CHAT_HISTORY_INGRESS_WINDOW_MS"] >= 10 * 1000


def test_a_v3_direct_file_to_64_devices_is_relayed_retained_and_replayed():
    values = _entry_constants({
        "MAX_TEXT_BYTES",
        "CHAT_HISTORY_MAX_BODY",
        "CHAT_HISTORY_REPLAY_MAX_BYTES",
        "CHAT_HISTORY_INGRESS_MAX_BYTES",
    })
    frame = _v3_relay_frame(_v3_file_plain_bytes(1024 * 1024), 64)
    assert frame <= values["CHAT_HISTORY_MAX_BODY"]
    assert frame <= values["CHAT_HISTORY_REPLAY_MAX_BYTES"]
    assert frame <= values["CHAT_HISTORY_INGRESS_MAX_BYTES"]
    assert frame <= values["MAX_TEXT_BYTES"]
    host = (ENTRY.parents[1] / "host" / "forkmesh_host").resolve()
    serve = (host / "serve.py").read_text(encoding="utf-8")
    assert '"ws_max_size": 4 * 1024 * 1024,' in serve
    assert 4 * 1024 * 1024 >= values["MAX_TEXT_BYTES"]
    cluster = (host / "durable_cluster.py").read_text(encoding="utf-8")
    assert "MESH_WS_MAX_SIZE = 4 * 1024 * 1024 + 64 * 1024" in cluster
    assert cluster.count("MESH_WS_MAX_SIZE") == 3


def test_a_v3_live_frame_to_64_devices_fits_the_live_lane():
    values = _entry_constants({"ROOM_LIVE_MAX_BYTES", "MAX_TEXT_BYTES"})
    frame = _v3_relay_frame(11 * 1024, 64)
    assert frame <= values["ROOM_LIVE_MAX_BYTES"] <= 256 * 1024


def test_store_prunes_oldest_frames_by_count_and_total_bytes():
    db = sqlite3.connect(":memory:")
    db.execute(
        "CREATE TABLE chat_history ("
        "room_key TEXT NOT NULL,msg_id TEXT NOT NULL,ts INTEGER NOT NULL,"
        "body TEXT NOT NULL,PRIMARY KEY(room_key,msg_id))"
    )
    namespace = _history_helpers(db)
    store = namespace["chat_history_store"]

    for number in range(1, 5):
        asyncio.run(store(
            object(), "room-a", str(number), number, str(number) * 8))

    rows = db.execute(
        "SELECT msg_id,body FROM chat_history WHERE room_key='room-a' "
        "ORDER BY ts"
    ).fetchall()
    assert rows == [("3", "3" * 8), ("4", "4" * 8)]

    asyncio.run(store(object(), "room-b", "other", 1, "x" * 8))
    other = db.execute(
        "SELECT msg_id FROM chat_history WHERE room_key='room-b'"
    ).fetchall()
    assert other == [("other",)]


def test_room_retention_ingress_budget_is_byte_based_and_resets_by_window():
    clock = [1_800_000_000_000]

    class _JsValue:
        def __init__(self, value):
            self.value = dict(value)

        def to_py(self):
            return dict(self.value)

    class _Date:
        @staticmethod
        def now():
            return clock[0]

    room_type = _room_class({
        "CHAT_HISTORY_INGRESS_MAX_BYTES": 10,
        "CHAT_HISTORY_INGRESS_WINDOW_MS": 1_000,
        "Date": _Date,
        "to_js": lambda value: _JsValue(value),
    })

    class _Storage:
        def __init__(self):
            self.value = None

        async def get(self, key):
            assert key == "chat_history_ingress"
            return self.value

        async def put(self, key, value):
            assert key == "chat_history_ingress"
            assert isinstance(value, _JsValue)
            self.value = value

    room = room_type()
    room.ctx = type("Context", (), {"storage": _Storage()})()

    assert asyncio.run(room._retention_ingress_admitted(6)) is True
    assert asyncio.run(room._retention_ingress_admitted(5)) is False
    assert asyncio.run(room._retention_ingress_admitted(4)) is True
    clock[0] += 1_000
    assert asyncio.run(room._retention_ingress_admitted(10)) is True


def test_retention_requires_boolean_true_and_budgets_before_d1_write():
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    room = next(
        item for item in tree.body
        if isinstance(item, ast.ClassDef) and item.name == "ForkMeshRoom"
    )
    method = next(
        item for item in room.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "_maybe_retain"
    )
    source = ast.unparse(method)

    assert "envelope.get('persist') is True" in source
    assert source.index("_retention_ingress_admitted") < source.index(
        "chat_history_store")
    assert source.index("chat_history_store") < source.index(
        "_chat_direct_message_retained")


def test_replayed_cipher_is_transport_marked_and_terminated():
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    helper = next(
        item for item in tree.body
        if isinstance(item, ast.FunctionDef)
        and item.name == "chat_history_replay_body"
    )
    module = ast.fix_missing_locations(
        ast.Module(body=[helper], type_ignores=[]))
    namespace = {"json": json}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    mark = namespace["chat_history_replay_body"]

    original = {
        "kind": "cipher",
        "v": 1,
        "nonce": "n",
        "tag": "t",
        "body": "encrypted",
        "persist": True,
    }
    replay = json.loads(mark(json.dumps(original)))
    assert replay == {**original, "historyReplay": True}
    assert mark('{"kind":"presence"}') == '{"kind":"presence"}'

    room = next(
        item for item in tree.body
        if isinstance(item, ast.ClassDef) and item.name == "ForkMeshRoom"
    )
    fetch = next(
        item for item in room.body
        if isinstance(item, ast.AsyncFunctionDef) and item.name == "fetch"
    )
    source = ast.unparse(fetch)
    assert "chat_history_replay_body(body)" in source
    assert "'kind': 'forkmesh-history-end'" in source
    # Retention is bounded on every store and expiry is swept by cron. Doing
    # two DELETE queries on every join delayed the 101 handshake and amplified
    # a busy room's reconnect storm.
    assert "chat_history_prune" not in source

    recent = next(
        item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "chat_history_recent"
    )
    recent_source = ast.unparse(recent)
    assert "CHAT_HISTORY_REPLAY_MAX_MESSAGES" in recent_source
    assert "CHAT_HISTORY_REPLAY_MAX_BYTES" in recent_source
    assert "ROW_NUMBER() OVER" in recent_source
    assert "SUM(length(body)) OVER" in recent_source

    socket_message = next(
        item for item in room.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "webSocketMessage"
    )
    source = ast.unparse(socket_message)
    assert "envelope.get('kind') != 'cipher'" in source
    assert "envelope.pop('historyReplay', None)" in source
    assert source.index("envelope.pop('historyReplay', None)") < source.index(
        "peer.send(message)")
