import ast
import base64
import hashlib
import hmac
import json
import re
from pathlib import Path

import test_world_backend as backend

world = backend.world
ROOT = Path(__file__).resolve().parents[2]
ROUTES = ROOT / "world" / "src" / "world_routes.py"
DURABLE = (ROOT / "world" / "src" / "world_durable_objects.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")


def _registered(peer_id, now=1000):
    state = world.default_presence(peer_id, now)
    state["accountStatus"] = "Registered"
    return state


def _rig(**extra):
    frame = {
        "type": "rig",
        "v": 1,
        "p": [120, -40, 15],
        "d": base64.b64encode(bytes(range(136))).decode(),
        "x": [10, 0, -5, 3, 40, 20, 0, 0, 0, 0],
        "h": [[1, 30, 50, 0, 10, 10, 90, 90, 4]],
        "t": 1,
        "l": 55,
        "w": 42,
    }
    frame.update(extra)
    return frame


def test_presence_carries_voice_room_mic_camera_and_words_for_accounts_only():
    guest = world.default_presence("peer_guest", 1000)
    assert guest["voice"] == "" and guest["words"] == 0
    _, guest_state = world.sanitize_message({"type": "presence", "voice": "world", "mic": True}, guest, 2000)
    assert guest_state["voice"] == "" and guest_state["mic"] is False

    state = _registered("peer_member")
    _, joined = world.sanitize_message({
        "type": "presence",
        "voice": "adhoc:0123456789abcdef",
        "voiceName": "Design jam",
        "mic": True,
        "cam": True,
        "words": 1234,
    }, state, 2000)
    assert joined["voice"] == "adhoc:0123456789abcdef"
    assert joined["voiceName"] == "Design jam"
    assert joined["mic"] is True and joined["cam"] is True
    assert joined["words"] == 1234
    public = world.public_presence(joined)
    for field in ("voice", "voiceName", "mic", "cam", "words"):
        assert field in public

    _, spaced = world.sanitize_message({"type": "presence", "voice": "space:sky-campus", "voiceName": "kept?"}, joined, 3000)
    assert spaced["voice"] == "space:sky-campus"
    assert spaced["voiceName"] == ""

    for bad in ("adhoc:XYZ", "bubble:123", "space:../../x", "general", "world;drop", "x" * 80):
        _, rejected = world.sanitize_message({"type": "presence", "voice": bad}, joined, 4000)
        assert rejected["voice"] == ""
        assert rejected["mic"] is False and rejected["cam"] is False

    _, bounded = world.sanitize_message({"type": "presence", "words": 10 ** 12}, joined, 5000)
    assert bounded["words"] == world.WORLD_WORDS_MAX
    assert world.clean_voice_room_name("<script>") == ""
    assert world.clean_voice_room_name("  Late   night  ") == "Late night"


def test_rig_frames_are_bounded_numbers_only():
    cleaned = world.sanitize_rig(_rig(text="hello", url="https://example.com"))
    assert cleaned == {
        "v": 1,
        "p": [120, -40, 15],
        "d": base64.b64encode(bytes(range(136))).decode(),
        "x": [10, 0, -5, 3, 40, 20, 0, 0, 0, 0],
        "h": [[1, 30, 50, 0, 10, 10, 90, 90, 4]],
        "t": 1,
        "l": 55,
        "w": 42,
    }
    clamped = world.sanitize_rig(_rig(p=[99999, -99999, 0], x=[500] * 10, h=[[9] * 9, [999] * 9, [1] * 9], w=-5))
    assert clamped["p"] == [1600, -1600, 0]
    assert clamped["x"] == [100] * 10
    assert clamped["h"] == [[9] * 9, [255] * 9]
    assert clamped["w"] == 0
    assert world.sanitize_rig(_rig(v=2)) is None
    assert world.sanitize_rig(_rig(p=[1, 2])) is None
    assert world.sanitize_rig(_rig(d="not base64!")) is None
    assert world.sanitize_rig(_rig(d="A" * 400)) is None
    assert world.sanitize_rig(_rig(a="<img src=x>")) is None
    assert "d" not in world.sanitize_rig(_rig(d=None))


def test_rig_hands_may_carry_a_bounded_depth():
    deep = world.sanitize_rig(_rig(h=[[0, 20, 40, 0, 0, 0, 0, 0, 0, 60], [1, 0, 0, 0, 0, 0, 0, 0, 0, 999]]))
    assert deep["h"] == [[0, 20, 40, 0, 0, 0, 0, 0, 0, 60], [1, 0, 0, 0, 0, 0, 0, 0, 0, 255]]
    assert world.sanitize_rig(_rig(h=[[0] * 11, [0] * 8]))["h"] == []


def test_voice_signals_only_carry_call_presence_and_keys_for_the_senders_room():
    sender = _registered("peer_a")
    sender["voice"] = "bubble:0123456789abcdef"
    peer = "ab" * 16
    key = base64.b64encode(bytes(16)).decode()
    presence = {"type": "voice-signal", "room": sender["voice"], "frame": {"type": "group-call-presence", "peer": peer, "call": "in", "media": "video", "note": "x"}}
    assert world.sanitize_voice_signal(presence, sender) == {
        "room": sender["voice"],
        "frame": {"type": "group-call-presence", "peer": peer, "call": "in", "media": "video"},
    }
    keyed = {"type": "voice-signal", "room": sender["voice"], "frame": {"type": "group-call-key", "peer": peer, "keyId": 3, "key": key}}
    assert world.sanitize_voice_signal(keyed, sender)["frame"] == {"type": "group-call-key", "peer": peer, "keyId": 3, "key": key}
    assert world.sanitize_voice_signal({**keyed, "room": "world"}, sender) is None
    assert world.sanitize_voice_signal({**keyed, "frame": {**keyed["frame"], "key": "short"}}, sender) is None
    assert world.sanitize_voice_signal({**keyed, "frame": {**keyed["frame"], "peer": "PEER"}}, sender) is None
    assert world.sanitize_voice_signal({**keyed, "frame": {**keyed["frame"], "type": "call-offer"}}, sender) is None
    sender["voice"] = "channel"
    assert world.sanitize_voice_signal({**keyed, "room": "channel"}, sender) is None


def test_bubble_invites_and_reactions_are_tiny_consent_frames():
    member = _registered("peer_member")
    secret = "f" * 32
    invite = {"type": "interaction", "kind": "bubble-invite", "target": "peer_friend", "room": "bubble:0123456789abcdef", "secret": secret, "text": "hi"}
    assert world.sanitize_interaction(invite, member) == {
        "type": "interaction", "kind": "bubble-invite", "target": "peer_friend",
        "room": "bubble:0123456789abcdef", "secret": secret,
    }
    assert world.sanitize_interaction({**invite, "secret": "short"}, member) is None
    assert world.sanitize_interaction({**invite, "room": "adhoc:0123456789abcdef"}, member) is None
    guest = world.default_presence("peer_guest", 1000)
    assert world.sanitize_interaction(invite, guest) is None
    for kind in ("bubble-knock", "bubble-decline"):
        assert world.sanitize_interaction({"type": "interaction", "kind": kind, "target": "peer_friend"}, member) == {
            "type": "interaction", "kind": kind, "target": "peer_friend",
        }
    reaction = {"type": "interaction", "kind": "reaction", "reaction": "thumbs_up", "target": "peer_speaker", "text": "x"}
    assert world.sanitize_interaction(reaction, member) == {
        "type": "interaction", "kind": "reaction", "reaction": "thumbs_up", "target": "peer_speaker",
    }
    assert world.sanitize_interaction({**reaction, "reaction": "<b>"}, member) is None
    assert world.sanitize_interaction({**reaction, "target": "peer_member"}, member)["target"] == ""
    assert "state.get(\"voice\") != interaction[\"room\"]" in DURABLE


def _voice_scope():
    tree = ast.parse(ROUTES.read_text(encoding="utf-8"))
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "world_voice_scope")
    namespace = {"world_protocol": world, "hashlib": hashlib, "hmac": hmac}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(ROUTES), "exec"), namespace)
    return namespace["world_voice_scope"]


def test_voice_tickets_name_one_world_room_and_bubbles_need_their_secret():
    scope = _voice_scope()
    assert scope("world") == "world"
    assert scope("space:sky-campus") == "space:sky-campus"
    assert scope("space:not-a-space") == ""
    assert scope("adhoc:0123456789abcdef") == "adhoc:0123456789abcdef"
    assert scope("channel") == ""
    secret = "0" * 32
    tag = hashlib.sha256(("forkmesh-world-bubble-v1\n" + secret).encode()).hexdigest()[:16]
    assert scope("bubble:" + tag, secret) == "bubble:" + secret
    assert scope("bubble:" + tag, "1" * 32) == ""
    assert scope("bubble:" + tag) == ""
    source = ROUTES.read_text(encoding="utf-8")
    handler = source[source.index("async def world_voice_ticket_handler"):]
    assert "_request_origin_allowed(env, request)" in handler
    assert '"sign_in_required"' in handler
    assert '"world-voice:" + scope' in handler
    assert '"/api/world/voice/ticket"' in APP_RUNTIME
    assert 'world_voice_ticket_handler = _world_routes.export(' in ENTRY


def _relay_runtime(now):
    instance, broadcasts = backend._world_socket_runtime(now)
    return instance, broadcasts


def _place(state, x=0.0, z=0.0, space="town-square", voice=""):
    state.update({"x": x, "z": z, "space": space, "voice": voice})
    return state


def test_rig_frames_reach_nearby_people_in_the_same_space_without_touching_the_rate_budget():
    now = 50_000
    instance, broadcasts = _relay_runtime(now)
    sender = backend._WorldSocket(_place(_registered("peer_talker", now), 0, 0), now)
    near = backend._WorldSocket(_place(_registered("peer_near", now), 10, 5), now)
    far = backend._WorldSocket(_place(_registered("peer_far", now), 500, 0), now)
    elsewhere = backend._WorldSocket(_place(_registered("peer_away", now), 2, 2, "sky-campus"), now)
    sockets = [sender, near, far, elsewhere]
    instance._live_sockets = lambda cleanup=False: sockets
    backend._world_socket_message(instance, sender, _rig())
    assert len(near.sent) == 1
    frame = json.loads(near.sent[0])
    assert frame["type"] == "rig" and frame["id"] == "peer_talker" and frame["w"] == 42
    assert not far.sent and not elsewhere.sent and not sender.sent
    assert sender.attachment.rl_count == 0
    for _ in range(world.WORLD_RIG_RATE_MAX_PER_WINDOW + 4):
        backend._world_socket_message(instance, sender, _rig())
    assert len(near.sent) == world.WORLD_RIG_RATE_MAX_PER_WINDOW
    assert not sender.closed
    for _ in range(world.WORLD_RIG_RATE_HARD_MAX_PER_WINDOW):
        backend._world_socket_message(instance, sender, _rig())
    assert sender.closed and sender.closed[-1][0] == 1008
    assert not broadcasts


def test_voice_signals_reach_only_people_in_the_same_voice_room():
    now = 60_000
    instance, _ = _relay_runtime(now)
    room = "adhoc:0123456789abcdef"
    sender = backend._WorldSocket(_place(_registered("peer_a", now), voice=room), now)
    sender.attachment.name = "alice"
    friend = backend._WorldSocket(_place(_registered("peer_b", now), voice=room), now)
    stranger = backend._WorldSocket(_place(_registered("peer_c", now), voice="world"), now)
    instance._live_sockets = lambda cleanup=False: [sender, friend, stranger]
    signal = {"type": "voice-signal", "room": room, "frame": {"type": "group-call-presence", "peer": "cd" * 16, "call": "in", "media": "audio"}}
    backend._world_socket_message(instance, sender, signal)
    assert [json.loads(m) for m in friend.sent] == [{
        "type": "voice-signal", "room": room, "from": "peer_a", "sender": "alice",
        "frame": {"type": "group-call-presence", "peer": "cd" * 16, "call": "in", "media": "audio"},
    }]
    assert not stranger.sent and not sender.sent
    backend._world_socket_message(instance, sender, {**signal, "room": "world"})
    assert len(friend.sent) == 1 and not stranger.sent


def test_rig_frames_must_lead_with_their_type_to_take_the_fast_path():
    assert re.search(r'"type":"%s"', DURABLE)
    assert "message[:40]" in DURABLE
