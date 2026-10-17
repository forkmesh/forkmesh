import json
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

import chat_v4_reference as ref  # noqa: E402

VECTORS = json.loads((ROOT / "conformance" / "chat" / "v4" / "vectors.json").read_text())
ROOM = VECTORS["room"]
SCOPE = VECTORS["scope"]
NOW = VECTORS["now"]
PARTICIPANTS = VECTORS["participants"]
DAY = ref.DAY_MS


def device(name, state=None):
    secrets = VECTORS["devices"][name]
    built = ref.Device(ref.unb64(secrets["identitySeed"]))
    assert built.kid == secrets["kid"]
    for spk in secrets["prekeys"]:
        made = ref.v3.prekey(ref.unb64(spk["xPriv"]), ref.unb64(spk["pqSeed"]), spk["ts"])
        assert made["id"] == spk["id"]
        built.prekeys.append(made)
    for bundle in VECTORS["bundles"].values():
        assert built.accept_bundle(SCOPE, "strict", ROOM, bundle, NOW, PARTICIPANTS) == "approved"
    if state is not None:
        built.import_ratchets(ROOM, VECTORS["states"][state])
    return built


def test_kdf_vectors():
    k = VECTORS["kdf"]
    s = k["sessionId"]
    assert ref.session_id(ref.unb64u(s["dh"]), ref.unb64(s["ct"])) == s["sid"]
    i = k["init"]
    rk, ck = ref.kdf_init(i["room"], i["sender"], i["recipient"], i["spk"], i["sid"], ref.unb64(i["ss1"]),
                          ref.unb64(i["ss2"]))
    assert (ref.b64(rk), ref.b64(ck)) == (i["rk"], i["ck"])
    r = k["rootStep"]
    rk, ck = ref.kdf_rk(ref.unb64(r["rk"]), ref.unb64(r["dhOut"]), ref.unb64(r["ss"]))
    assert (ref.b64(rk), ref.b64(ck)) == (r["nextRk"], r["ck"])
    c = k["chainStep"]
    mk, ck = ref.kdf_ck(ref.unb64(c["ck"]))
    assert (ref.b64(mk), ref.b64(ck)) == (c["mk"], c["nextCk"])


def test_vector_sequence():
    devices = {}
    for step in VECTORS["sequence"]:
        name = step["device"]
        if "state" in step:
            devices[name] = device(name, step["state"])
        dev = devices[name]
        if step.get("clearSeen"):
            dev.seen.clear()
        result = dev.open4(ROOM, SCOPE, VECTORS["frames"][step["frame"]], step["at"])
        assert result[0] == step["expect"], (step, result)
        if step["expect"] == "opened":
            assert result[1] == VECTORS["plain"][step["plain"]]
            assert result[2] == VECTORS["devices"]["bob" if name == "alice" else "alice"]["kid"]
        if step["frame"] == "m1" and step["expect"] == "opened":
            session = dev.export_ratchets(ROOM)["peers"][VECTORS["devices"]["alice"]["kid"]]["sessions"][0]
            want = VECTORS["bobAfterM1"]
            assert (session["rk"], session["ckr"], session["nr"], len(session["skipped"])) == (
                want["rk"], want["ckr"], want["nr"], want["skipped"])


def test_headers_follow_the_ratchet():
    h = VECTORS["headers"]
    assert h["m1"]["sid"] == h["m2"]["sid"] == h["m3"]["sid"] == h["r1"]["sid"]
    assert h["m1"]["dh"] == h["m2"]["dh"] != h["m3"]["dh"] == h["m4"]["dh"]
    assert (h["m1"]["n"], h["m2"]["n"], h["m3"]["n"], h["m4"]["n"]) == (0, 1, 0, 1)
    assert h["m3"]["pn"] == 2
    assert h["r1"]["dh"] not in (h["m1"]["dh"], h["m3"]["dh"])
    frame = VECTORS["frames"]["m1"]
    assert frame["v"] == 4 and [k["kid"] for k in frame["keys"]] == [VECTORS["devices"]["bob"]["kid"]]
    assert VECTORS["frames"]["empty"]["keys"] == []


def pair():
    alice, bob = device("alice"), device("bob")
    return alice, bob


def chat(text):
    return {"type": "chat", "id": text, "text": text}


def test_conversation_with_drops_and_reordering():
    alice, bob = pair()
    rng = random.Random(4)
    devices = {"a": (alice, bob), "b": (bob, alice)}
    at = NOW
    for turn in range(6):
        sender, receiver = devices["a" if turn % 2 == 0 else "b"]
        frames = [sender.seal4(ROOM, SCOPE, chat("%d-%d" % (turn, i)), at + i) for i in range(5)]
        rng.shuffle(frames)
        for frame in frames:
            result = receiver.open4(ROOM, SCOPE, frame, at + 10)
            assert result[0] == "opened", result
        at += 100


def test_message_lost_in_old_chain_opens_after_new_chain():
    alice, bob = pair()
    late = alice.seal4(ROOM, SCOPE, chat("late"), NOW)
    assert bob.open4(ROOM, SCOPE, alice.seal4(ROOM, SCOPE, chat("a2"), NOW + 1), NOW + 2)[0] == "opened"
    assert alice.open4(ROOM, SCOPE, bob.seal4(ROOM, SCOPE, chat("b1"), NOW + 3), NOW + 4)[0] == "opened"
    assert bob.open4(ROOM, SCOPE, alice.seal4(ROOM, SCOPE, chat("a3"), NOW + 5), NOW + 6)[0] == "opened"
    assert bob.open4(ROOM, SCOPE, late, NOW + 7)[1]["text"] == "late"
    assert bob.open4(ROOM, SCOPE, late, NOW + 7) == ("replay",)


def test_simultaneous_first_messages_converge():
    alice, bob = pair()
    from_alice = alice.seal4(ROOM, SCOPE, chat("a"), NOW)
    from_bob = bob.seal4(ROOM, SCOPE, chat("b"), NOW)
    assert bob.open4(ROOM, SCOPE, from_alice, NOW + 1)[0] == "opened"
    assert alice.open4(ROOM, SCOPE, from_bob, NOW + 1)[0] == "opened"
    for i in range(3):
        assert bob.open4(ROOM, SCOPE, alice.seal4(ROOM, SCOPE, chat("a%d" % i), NOW + 10 * i), NOW + 10 * i)[0] == "opened"
        assert alice.open4(ROOM, SCOPE, bob.seal4(ROOM, SCOPE, chat("b%d" % i), NOW + 10 * i + 5), NOW + 10 * i + 5)[0] == "opened"


def test_reset_recovers_a_lost_session():
    alice, bob = pair()
    assert bob.open4(ROOM, SCOPE, alice.seal4(ROOM, SCOPE, chat("a1"), NOW), NOW)[0] == "opened"
    assert alice.open4(ROOM, SCOPE, bob.seal4(ROOM, SCOPE, chat("b1"), NOW + 1), NOW + 1)[0] == "opened"
    bob.ratchets.clear()
    broken = alice.seal4(ROOM, SCOPE, chat("a2"), NOW + 2)
    assert bob.open4(ROOM, SCOPE, broken, NOW + 3) == ("noSession", alice.kid)
    bob.reset(ROOM, alice.kid)
    reset = bob.seal4(ROOM, SCOPE, {"type": ref.RESET_TYPE}, NOW + 4, only=[alice.kid])
    assert reset["keys"][0]["sid"] == ref.session_id(ref.unb64u(reset["keys"][0]["dh"]),
                                                      ref.unb64(reset["keys"][0]["ct"]))
    opened = alice.open4(ROOM, SCOPE, reset, NOW + 5)
    assert opened[0] == "opened" and opened[1]["type"] == ref.RESET_TYPE
    assert bob.open4(ROOM, SCOPE, alice.seal4(ROOM, SCOPE, chat("a2 again"), NOW + 6), NOW + 7)[0] == "opened"


def test_peek_does_not_advance():
    alice, bob = pair()
    frame = alice.seal4(ROOM, SCOPE, chat("a1"), NOW)
    before = bob.export_ratchets(ROOM)
    assert bob.open4(ROOM, SCOPE, frame, NOW, commit=False)[0] == "opened"
    assert bob.export_ratchets(ROOM) == before
    assert bob.open4(ROOM, SCOPE, frame, NOW)[0] == "opened"
    assert bob.open4(ROOM, SCOPE, frame, NOW) == ("replay",)


def test_too_many_skipped_needs_a_reset():
    alice, bob = pair()
    for i in range(ref.MAX_SKIP + 1):
        alice.seal4(ROOM, SCOPE, chat("lost"), NOW)
    far = alice.seal4(ROOM, SCOPE, chat("far"), NOW)
    assert bob.open4(ROOM, SCOPE, far, NOW)[0] == "noSession"


def test_expired_prekey_cannot_start_a_session():
    alice, bob = pair()
    frame = alice.seal4(ROOM, SCOPE, chat("a1"), NOW)
    later = bob.prekeys[0]["exp"] + 1
    bob.wipe_expired(later)
    assert bob.open4(ROOM, SCOPE, frame, NOW)[0] == "noSession"


def test_other_room_and_tampered_header_fail():
    alice, bob = pair()
    frame = alice.seal4(ROOM, SCOPE, chat("a1"), NOW)
    assert bob.open4("another-room", SCOPE, frame, NOW) == ("invalid",)
    with_self = json.loads(json.dumps(frame))
    with_self["keys"].append(dict(with_self["keys"][0], kid=alice.kid))
    with_self["keys"].sort(key=lambda k: k["kid"])
    assert bob.open4(ROOM, SCOPE, with_self, NOW) == ("invalid",)


def test_sessions_are_bounded():
    alice, bob = pair()
    for i in range(ref.SESSIONS_KEPT + 3):
        alice.reset(ROOM, bob.kid)
        assert bob.open4(ROOM, SCOPE, alice.seal4(ROOM, SCOPE, chat(str(i)), NOW + i), NOW + i)[0] == "opened"
    assert len(bob.export_ratchets(ROOM)["peers"][alice.kid]["sessions"]) == ref.SESSIONS_KEPT
