#!/usr/bin/env python3

import ast
import asyncio
import hashlib
import hmac
import json
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
ENTRY_TREE = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
FUNCTIONS = {
    "chat_room_audience",
    "chat_recipient_names",
    "chat_room_recipients",
    "chat_recipients_fields",
    "chat_recipients_ack",
    "chat_stamp_recipients",
    "chat_history_replay_body",
    "chat_direct_retained_id",
    "chat_server_copies_frame",
    "chat_direct_server_copies",
    "chat_direct_received",
    "chat_direct_superseded",
    "chat_direct_refs",
    "chat_direct_delivered_frame",
    "chat_direct_read_frame",
    "chat_live_budget_frame",
    "chat_live_envelope",
}
NOW = 1_800_000_000_000
ALICE = "a" * 64
BOB = "b" * 64
CAROL = "c" * 64
DAVE = "d" * 64
NAMES = {ALICE: "alice", BOB: "bob", CAROL: "carol", DAVE: "dave"}
CHANNEL = "1" * 32
DIRECT = "2" * 32


class _Attachment:
    def __init__(self, values):
        self.__dict__.update(values)


class _Socket:
    def __init__(self, socket_id, account_bi="", channel_id="",
                 direct_id=""):
        self.sent = []
        self.closed = []
        self.attachment = {
            "id": socket_id,
            "room": (
                "chat-direct:" + direct_id + ":v1" if direct_id
                else "chat-channel:" + channel_id + ":v1" if channel_id
                else "repo:mainnode/forkmesh:room:world-general"
            ),
            "channel_id": channel_id,
            "channel_version": 1 if channel_id else 0,
            "direct_id": direct_id,
            "direct_version": 1 if direct_id else 0,
            "account_bi": account_bi,
            "last": NOW,
        }

    def deserializeAttachment(self):
        return _Attachment(self.attachment)

    def serializeAttachment(self, value):
        self.attachment = dict(value)

    def send(self, data):
        self.sent.append(data)

    def close(self, code, reason):
        self.closed.append((code, reason))


class _Context:
    def __init__(self, sockets):
        self.sockets = sockets
        self.storage = _Storage()

    def getWebSockets(self, tag):
        assert tag == "chat"
        return list(self.sockets)


class _Storage:
    def __init__(self):
        self.values = {}

    async def get(self, key):
        return self.values.get(key)

    async def put(self, key, value):
        self.values[key] = value


class _Date:
    @staticmethod
    def now():
        return NOW


def _harness(channel=None, members=(), participants=()):
    selected = [
        node for node in ENTRY_TREE.body
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCTIONS
        ) or (isinstance(node, ast.ClassDef) and node.name == "ForkMeshRoom")
    ]
    stored = []
    retained = []
    retained_handles = []

    async def d1_first(_env, sql, *args):
        if "FROM chat_channels" in sql:
            return channel
        if "COUNT(*) AS stored FROM chat_history" in sql:
            room_key, _cutoff, _pattern = args
            return {"stored": sum(
                1 for row in stored
                if row[0] == room_key and row[1].endswith(":n"))}
        raise AssertionError((sql, args))

    async def d1_run(_env, sql, *args):
        assert sql.startswith("DELETE FROM chat_history WHERE room_key=?")
        room_key, tag, *refs = args
        own = "substr(msg_id,42,16)=?" in sql
        stored[:] = [
            row for row in stored
            if not (
                row[0] == room_key
                and row[1][40:41] == ":"
                and (row[1][41:57] == tag) == own
                and row[1][:40] in refs
            )
        ]

    async def d1_all(_env, sql, *args):
        if "FROM chat_channel_members" in sql:
            return [{"member_bi": member} for member in members]
        if "FROM chat_direct_participants" in sql:
            return [{"participant_bi": value} for value in participants]
        raise AssertionError((sql, args))

    async def decrypt_row(_env, data):
        return data

    async def account_rec_by_bi(_env, account_bi):
        name = NAMES.get(account_bi)
        return {"name": name.upper()} if name else None

    async def sha256_hex(text):
        return hashlib.sha256(text.encode("utf-8")).hexdigest()

    async def chat_history_store(_env, room_key, msg_id, ts, body):
        stored.append((room_key, msg_id, ts, body))

    async def noop(*_args, **_kwargs):
        return None

    async def direct_retained(*args, **kwargs):
        retained.append(args[1:3])
        retained_handles.append(kwargs)

    async def admitted(*_args, **_kwargs):
        return True

    namespace = {
        "CHAT_HISTORY_MAX_BODY": 1_900_000,
        "CHAT_HISTORY_RETAIN_MS": 7 * 24 * 60 * 60 * 1000,
        "CHAT_DIRECT_RECEIVED_REFS_MAX": 64,
        "CHAT_RECIPIENT_NAMES_MAX": 20,
        "CHAT_RECIPIENT_NAME_CACHE_MAX": 10000,
        "_CHAT_RECIPIENT_NAMES": {},
        "_account_identity_rec_by_bi": account_rec_by_bi,
        "Date": _Date,
        "DurableObject": object,
        "MAX_TEXT_BYTES": 2_000_000,
        "ROOM_CLIENT_STALE_MS": 180_000,
        "ROOM_MSG_MAX_PER_WINDOW": 1000,
        "ROOM_MSG_WINDOW_MS": 10_000,
        "ROOM_LIVE_MAX_PER_WINDOW": 150,
        "ROOM_LIVE_MAX_BYTES": 16 * 1024,
        "ROOM_LIVE_ACCESS_RECHECK_MS": 5_000,
        "_maintenance_socket_closed": lambda *_args: False,
        "_chat_direct_message_retained": direct_retained,
        "_retained": retained,
        "_retained_handles": retained_handles,
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.deserializeAttachment(), name, default),
        "chat_history_store": chat_history_store,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "durable_object_traffic_flush": noop,
        "durable_object_traffic_note": lambda *_args, **_kwargs: None,
        "CHAT_CHANNEL_DO_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/v([1-9][0-9]*)/"
            r"(ws|revoke|webhook)$"),
        "CHAT_DIRECT_MESSAGE_DO_RE": re.compile(
            r"^/api/chat/direct-messages/([0-9a-f]{32})/"
            r"v([1-9][0-9]*)/(ws|revoke|read)$"),
        "json": json,
        "json_response": lambda data, status=200, **_kwargs: {
            "status": status, "data": data},
        "method_name": lambda request: request.method,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "push_chat_room_apns": noop,
        "re": re,
        "sha256_hex": sha256_hex,
        "to_js": lambda value: value,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    room_type = namespace["ForkMeshRoom"]
    room_type._private_room_current = admitted
    room_type._retention_ingress_admitted = admitted
    return namespace, room_type, stored


def _relay(room_type, sockets, sender, frame):
    room = room_type()
    room.env = object()
    room.ctx = _Context(sockets)
    asyncio.run(room.webSocketMessage(sender, frame))


def _chat_frame(**extra):
    return json.dumps(
        {"kind": "cipher", "v": 1, "iv": "i", "data": "d", "persist": True,
         "notify": True, **extra},
        separators=(",", ":"))


def _ref(frame):
    return hashlib.sha256(frame.encode("utf-8")).hexdigest()[:40]


def _kind(socket, kind):
    return [
        json.loads(data) for data in socket.sent
        if json.loads(data).get("kind") == kind
    ]


def _ack(socket):
    acks = [
        json.loads(data) for data in socket.sent
        if json.loads(data).get("kind") == "forkmesh-recipients"
    ]
    assert len(acks) == 1
    return acks[0]


def test_direct_message_goes_to_one_person_and_tells_the_sender():
    namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)
    frame = _chat_frame()

    _relay(room_type, [alice, bob], alice, frame)

    delivered = _kind(bob, "cipher")[0]
    assert delivered["recipients"] == 1
    assert delivered["data"] == "d"
    assert delivered["ref"] == _ref(frame)
    assert delivered["recipientNames"] == ["bob"]
    assert _ack(alice) == {
        "kind": "forkmesh-recipients", "v": 1, "ref": _ref(frame),
        "recipients": 1, "recipientNames": ["bob"],
    }
    assert len(_kind(alice, "cipher")) == 0
    room_key, msg_id, _ts, body = stored[0]
    assert room_key == "chat-direct:" + DIRECT + ":v1"
    assert msg_id == _ref(frame) + ":" + ALICE[:16] + ":n"
    assert json.loads(body)["recipients"] == 1
    assert json.loads(body)["ref"] == _ref(frame)
    replay = json.loads(namespace["chat_history_replay_body"](body))
    assert replay["recipients"] == 1
    assert replay["historyReplay"] is True


def test_direct_message_counts_the_other_person_even_when_offline():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)

    _relay(room_type, [alice], alice, _chat_frame())

    assert _ack(alice)["recipients"] == 1
    assert json.loads(stored[0][3])["recipients"] == 1


def test_private_room_counts_members_and_creator_minus_the_sender():
    _namespace, room_type, stored = _harness(
        channel={
            "data": {"visibility": "private"},
            "created_by_bi": CAROL,
        },
        members=(ALICE, BOB, DAVE, ALICE),
    )
    alice = _Socket("s1", ALICE, channel_id=CHANNEL)
    alice_phone = _Socket("s2", ALICE, channel_id=CHANNEL)
    bob = _Socket("s3", BOB, channel_id=CHANNEL)

    _relay(room_type, [alice, alice_phone, bob], alice, _chat_frame())

    assert json.loads(bob.sent[0])["recipients"] == 3
    assert json.loads(alice_phone.sent[0])["recipients"] == 3
    assert _ack(alice)["recipients"] == 3
    assert _ack(alice)["recipientNames"] == ["bob", "carol", "dave"]
    assert json.loads(stored[0][3])["recipients"] == 3
    assert json.loads(stored[0][3])["recipientNames"] == [
        "bob", "carol", "dave"]


def test_public_room_counts_distinct_accounts_it_was_delivered_to():
    _namespace, room_type, stored = _harness(
        channel={
            "data": {"visibility": "public"},
            "created_by_bi": DAVE,
        },
    )
    alice = _Socket("s1", ALICE, channel_id=CHANNEL)
    alice_laptop = _Socket("s2", ALICE, channel_id=CHANNEL)
    bob = _Socket("s3", BOB, channel_id=CHANNEL)
    carol = _Socket("s4", CAROL, channel_id=CHANNEL)
    carol_phone = _Socket("s5", CAROL, channel_id=CHANNEL)

    _relay(
        room_type,
        [alice, alice_laptop, bob, carol, carol_phone],
        alice,
        _chat_frame(),
    )

    for peer in (alice_laptop, bob, carol, carol_phone):
        assert json.loads(peer.sent[0])["recipients"] == 2
    assert _ack(alice)["recipients"] == 2
    assert _ack(alice)["recipientNames"] == ["bob", "carol"]
    assert json.loads(stored[0][3])["recipients"] == 2


def test_notices_channel_counts_only_its_creator():
    namespace, _room_type, _stored = _harness(
        channel={
            "data": {"visibility": "public", "kind": "notices"},
            "created_by_bi": CAROL,
        },
    )
    audience = asyncio.run(namespace["chat_room_audience"](
        object(), CHANNEL, "", ALICE, [BOB, CAROL]))
    assert audience == {CAROL}


def test_client_supplied_count_is_replaced_by_the_relay():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, bob], alice, _chat_frame(
        recipients=500, recipientNames=["mallory"]))

    assert _kind(bob, "cipher")[0]["recipients"] == 1
    assert _kind(bob, "cipher")[0]["recipientNames"] == ["bob"]
    assert json.loads(stored[0][3])["recipients"] == 1


def test_public_world_room_strips_forged_count_and_stamps_nothing():
    _namespace, room_type, stored = _harness()
    sender = _Socket("s1")
    peer = _Socket("s2")

    _relay(room_type, [sender, peer], sender, _chat_frame(
        recipients=99, recipientNames=["mallory"]))

    assert "recipients" not in json.loads(peer.sent[0])
    assert "recipientNames" not in json.loads(peer.sent[0])
    assert sender.sent == []
    assert "recipients" not in json.loads(stored[0][3])


def test_live_only_frames_are_not_counted():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)
    typing = json.dumps({"kind": "cipher", "v": 1, "data": "t"})

    _relay(room_type, [alice, bob], alice, typing)

    assert bob.sent == [typing]
    assert alice.sent == []
    assert stored == []


def test_unknown_channel_leaves_the_count_out():
    _namespace, room_type, stored = _harness(channel=None)
    alice = _Socket("s1", ALICE, channel_id=CHANNEL)
    bob = _Socket("s2", BOB, channel_id=CHANNEL)

    _relay(room_type, [alice, bob], alice, _chat_frame())

    assert "recipients" not in json.loads(bob.sent[0])
    assert alice.sent == []


def test_channel_webhook_message_is_stamped_for_members():
    namespace, room_type, stored = _harness(
        channel={
            "data": {"visibility": "private"},
            "created_by_bi": CAROL,
        },
        members=(ALICE, BOB),
    )
    body = json.dumps(
        {"kind": "cipher", "v": 1, "persist": True, "recipients": 40},
        separators=(",", ":"))
    signature = hmac.new(
        b"secret:chat-webhook-delivery-v1", body.encode(), "sha256",
    ).hexdigest()

    class _Headers:
        def get(self, name, default=None):
            return signature if name == "x-forkmesh-chat-webhook" else default

    class _Request:
        headers = _Headers()

        async def text(self):
            return body

    namespace["hmac"] = hmac
    namespace["_require_data_secret"] = lambda _env: "secret"
    namespace["json_response"] = lambda data, status=200: {
        "status": status, "data": data}
    bob = _Socket("s1", BOB, channel_id=CHANNEL)
    room = room_type()
    room.env = object()
    room.ctx = _Context([bob])
    match = re.match(
        r"^/api/chat/channels/([0-9a-f]{32})/v([1-9][0-9]*)/(webhook)$",
        "/api/chat/channels/" + CHANNEL + "/v1/webhook")

    response = asyncio.run(room._channel_webhook(_Request(), match))

    assert response["status"] == 200
    assert json.loads(bob.sent[0])["recipients"] == 3
    assert json.loads(stored[0][3])["recipients"] == 3


def _copies(socket):
    return [frame["stored"] for frame in _kind(socket, "forkmesh-server-copies")]


def test_direct_message_is_deleted_once_the_other_person_receives_it():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)
    frame = _chat_frame()

    _relay(room_type, [alice, bob], alice, frame)

    assert len(stored) == 1
    assert _copies(alice) == [1]
    assert _copies(bob) == [1]
    ref = _kind(bob, "cipher")[0]["ref"]

    _relay(room_type, [alice, bob], bob, json.dumps(
        {"kind": "forkmesh-received", "v": 1, "refs": [ref]}))

    assert stored == []
    assert _copies(alice) == [1, 0]
    assert _copies(bob) == [1, 0]


def test_receiving_a_direct_message_tells_the_sender_it_was_delivered():
    _namespace, room_type, _stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    alice_phone = _Socket("s2", ALICE, direct_id=DIRECT)
    bob = _Socket("s3", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, alice_phone, bob], alice, _chat_frame())
    ref = _kind(bob, "cipher")[0]["ref"]
    _relay(room_type, [alice, alice_phone, bob], bob, json.dumps(
        {"kind": "forkmesh-received", "v": 1,
         "refs": [ref, ref, "not-a-ref", 7]}))

    expected = [{"kind": "forkmesh-delivered", "v": 1, "refs": [ref]}]
    assert _kind(alice, "forkmesh-delivered") == expected
    assert _kind(alice_phone, "forkmesh-delivered") == expected
    assert _kind(bob, "forkmesh-delivered") == []


def test_superseding_a_direct_message_is_not_a_delivery():
    _namespace, room_type, _stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, bob], alice, _chat_frame())
    ref = _kind(bob, "cipher")[0]["ref"]
    _relay(room_type, [alice, bob], alice, json.dumps(
        {"kind": "forkmesh-superseded", "v": 1, "refs": [ref]}))

    assert _kind(alice, "forkmesh-delivered") == []
    assert _kind(bob, "forkmesh-delivered") == []


def test_read_announcement_reaches_only_the_other_participant():
    _namespace, room_type, _stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)
    bob_phone = _Socket("s3", BOB, direct_id=DIRECT)

    class _Request:
        method = "POST"
        url = (
            "https://forkmesh.internal/api/chat/direct-messages/" + DIRECT
            + "/v1/read?account=" + BOB
        )

    room = room_type()
    room.env = object()
    room.ctx = _Context([alice, bob, bob_phone])
    response = asyncio.run(room.fetch(_Request()))

    assert response["status"] == 200
    assert _kind(alice, "forkmesh-read") == [{"kind": "forkmesh-read", "v": 1}]
    assert bob.sent == []
    assert bob_phone.sent == []


def test_sender_cannot_clear_its_own_undelivered_message():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    alice_phone = _Socket("s2", ALICE, direct_id=DIRECT)
    frame = _chat_frame()

    _relay(room_type, [alice, alice_phone], alice, frame)
    ref = _kind(alice_phone, "cipher")[0]["ref"]
    _relay(room_type, [alice, alice_phone], alice_phone, json.dumps(
        {"kind": "forkmesh-received", "v": 1, "refs": [ref]}))

    assert len(stored) == 1
    assert _copies(alice)[-1] == 1


def test_received_refs_are_validated_and_bounded():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, bob], bob, json.dumps(
        {"kind": "forkmesh-received", "v": 1,
         "refs": ["x' OR 1=1 --", 7, None]}))

    assert alice.sent == [] and bob.sent == []


def test_client_supplied_ref_is_replaced_by_the_relay():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, bob], alice, _chat_frame(ref="f" * 40))

    assert _kind(bob, "cipher")[0]["ref"] == _ref(_chat_frame())


def test_channel_messages_carry_no_ref_and_ignore_received_frames():
    _namespace, room_type, stored = _harness(
        channel={"data": {"visibility": "private"}, "created_by_bi": CAROL},
        members=(ALICE, BOB),
    )
    alice = _Socket("s1", ALICE, channel_id=CHANNEL)
    bob = _Socket("s2", BOB, channel_id=CHANNEL)

    _relay(room_type, [alice, bob], alice, _chat_frame())
    ref = _ref(_chat_frame())
    _relay(room_type, [alice, bob], bob, json.dumps(
        {"kind": "forkmesh-received", "v": 1, "refs": [ref]}))

    assert "ref" not in _kind(bob, "cipher")[0]
    assert stored[0][1] == ref
    assert _copies(alice) == [] and _copies(bob) == []


def test_recipient_names_are_capped_and_unknown_accounts_are_left_out():
    namespace, _room_type, _stored = _harness()
    namespace["CHAT_RECIPIENT_NAMES_MAX"] = 2
    names = asyncio.run(namespace["chat_recipient_names"](
        object(), {DAVE, CAROL, BOB, "e" * 64}))
    assert names == ["bob", "carol"]
    unknown = asyncio.run(namespace["chat_recipient_names"](
        object(), {"e" * 64}))
    assert unknown == []
    assert namespace["chat_recipients_fields"]((4, [])) == {"recipients": 4}


def test_sender_can_supersede_only_its_own_stored_copies():
    _namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, bob], alice, _chat_frame(data="one"))
    _relay(room_type, [alice, bob], bob, _chat_frame(data="two"))
    alice_ref = _kind(bob, "cipher")[0]["ref"]
    bob_ref = _kind(alice, "cipher")[0]["ref"]
    assert len(stored) == 2

    _relay(room_type, [alice, bob], bob, json.dumps(
        {"kind": "forkmesh-superseded", "v": 1, "refs": [alice_ref]}))
    assert len(stored) == 2

    _relay(room_type, [alice, bob], alice, json.dumps(
        {"kind": "forkmesh-superseded", "v": 1,
         "refs": [alice_ref, bob_ref, "zz"]}))
    assert [row[1][:40] for row in stored] == [bob_ref]
    assert _copies(bob)[-1] == 1


def test_only_new_messages_raise_the_direct_unread_count():
    namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, bob], alice, json.dumps(
        {"kind": "cipher", "v": 1, "iv": "k", "data": "key",
         "persist": True}, separators=(",", ":")))
    _relay(room_type, [alice, bob], alice, _chat_frame(resend=True))
    assert len(stored) == 2
    assert namespace["_retained"] == []

    _relay(room_type, [alice, bob], alice, _chat_frame(data="new"))
    assert namespace["_retained"] == [(DIRECT, ALICE)]
    ref = _kind(bob, "cipher")[-1]["ref"]
    assert namespace["_retained_handles"] == [
        {"ref": ref, "key_version": 1}]


def _live_frame(**extra):
    return json.dumps(
        {"kind": "cipher", "v": 1, "iv": "i", "data": "touch", "live": True,
         **extra},
        separators=(",", ":"))


def test_live_direct_frames_are_relayed_without_storing_or_counting():
    namespace, room_type, stored = _harness(participants=(ALICE, BOB))
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)
    frame = _live_frame(ref="f" * 40, recipients=9)

    _relay(room_type, [alice, bob], alice, frame)

    delivered = json.loads(bob.sent[0])
    assert delivered == {"kind": "cipher", "v": 1, "iv": "i",
                         "data": "touch", "live": True}
    assert alice.sent == []
    assert stored == []
    assert namespace["_retained"] == []
    assert alice.attachment["lv_count"] == 1
    assert "rl_count" not in alice.attachment


def test_live_frames_have_their_own_budget():
    namespace, room_type, _stored = _harness(participants=(ALICE, BOB))
    namespace["ROOM_MSG_MAX_PER_WINDOW"] = 2
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    for _ in range(155):
        _relay(room_type, [alice, bob], alice, _live_frame())
    assert len(bob.sent) == 150

    _relay(room_type, [alice, bob], alice, _chat_frame(data="still"))
    assert json.loads(bob.sent[-1])["data"] == "still"
    assert alice.attachment["lv_count"] == 155
    assert alice.attachment["rl_count"] == 1


def test_persisted_or_channel_live_frames_take_the_normal_path():
    _namespace, room_type, stored = _harness(
        participants=(ALICE, BOB),
        channel={"data": {"visibility": "public"}, "created_by_bi": ALICE})
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    _relay(room_type, [alice, bob], alice, _live_frame(persist=True))
    assert len(stored) == 1
    assert alice.attachment["rl_count"] == 1

    carol = _Socket("s3", CAROL, channel_id=CHANNEL)
    dave = _Socket("s4", DAVE, channel_id=CHANNEL)
    _relay(room_type, [carol, dave], carol, _live_frame())
    assert carol.attachment["rl_count"] == 1
    assert "lv_count" not in carol.attachment


def test_live_frames_recheck_room_access_every_few_seconds():
    namespace, room_type, _stored = _harness(participants=(ALICE, BOB))
    checks = []
    allowed = [True]

    async def current(_self, _ws):
        checks.append(1)
        return allowed[0]

    room_type._private_room_current = current
    clock = [NOW]

    class _Clock:
        @staticmethod
        def now():
            return clock[0]

    namespace["Date"] = _Clock
    alice = _Socket("s1", ALICE, direct_id=DIRECT)
    bob = _Socket("s2", BOB, direct_id=DIRECT)

    for _ in range(5):
        _relay(room_type, [alice, bob], alice, _live_frame())
    assert len(checks) == 1
    assert len(bob.sent) == 5

    clock[0] = NOW + 5_000
    allowed[0] = False
    _relay(room_type, [alice, bob], alice, _live_frame())
    assert len(checks) == 2
    assert len(bob.sent) == 5
    assert alice.closed and bob.closed


def test_live_budget_frame_and_envelope_rules():
    namespace, _room_type, _stored = _harness()
    assert json.loads(namespace["chat_live_budget_frame"]()) == {
        "kind": "forkmesh-live", "v": 1, "perWindow": 150,
        "windowMs": 10_000}
    envelope = namespace["chat_live_envelope"]
    assert envelope(_live_frame())["data"] == "touch"
    assert envelope(_live_frame(notify=True)) is None
    assert envelope(_live_frame(resend=True)) is None
    assert envelope(_live_frame(live=False)) is None
    assert envelope(_live_frame(data="x" * 20_000)) is None
    assert envelope(json.dumps({"kind": "forkmesh-received", "live": True})) is None
    assert envelope("not json \"live\"") is None
