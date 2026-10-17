import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

import chat_v3_reference as ref  # noqa: E402

VECTORS = json.loads((ROOT / "conformance" / "chat" / "v3" / "vectors.json").read_text())
ROOM = VECTORS["room"]
SCOPE = VECTORS["scope"]
NOW = VECTORS["now"]


def device(name):
    secrets = VECTORS["devices"][name]
    built = ref.Device(ref.unb64(secrets["identitySeed"]))
    assert ref.b64u(built.idk) == secrets["idk"]
    assert built.kid == secrets["kid"]
    for spk in secrets["prekeys"]:
        made = ref.prekey(ref.unb64(spk["xPriv"]), ref.unb64(spk["pqSeed"]), spk["ts"])
        assert made["id"] == spk["id"]
        assert ref.b64u(made["x"]) == spk["x"]
        assert ref.b64u(made["pq"]) == spk["pq"]
        assert made["exp"] == spk["exp"]
        built.prekeys.append(made)
    return built


def bob_with_trust():
    bob = device("bob")
    bundles = VECTORS["bundles"]
    for name in ("alice", "bob"):
        assert bob.accept_bundle(SCOPE, "strict", ROOM, bundles[name], NOW) == "approved"
    assert bob.accept_bundle(SCOPE, "strict", ROOM, bundles["alice2"], NOW) == "pending"
    return bob


def test_bundles_verify():
    bundles = VECTORS["bundles"]
    for name in ("alice", "alice2", "bob", "bobOld"):
        assert ref.verify_bundle(bundles[name], ROOM, NOW), name
    for name, bundle in bundles["invalid"].items():
        assert not ref.verify_bundle(bundle, ROOM, NOW), name
    assert not ref.verify_bundle(bundles["alice"], ROOM, VECTORS["bundleChecks"]["expiredAt"])


def test_bundle_for_known_kid_cannot_change_account():
    bob = bob_with_trust()
    changed = dict(VECTORS["bundles"]["alice"], acct="mallory")
    assert bob.accept_bundle(SCOPE, "strict", ROOM, changed, NOW) is None
    assert bob.records(SCOPE)[VECTORS["devices"]["alice"]["kid"]]["acct"] == "alice"


def test_open_relay_sequence():
    bob = bob_with_trust()
    sealed = VECTORS["sealed"]
    first = bob.open(ROOM, SCOPE, sealed["first"]["frame"], NOW)
    assert first[0] == "opened"
    assert first[1] == sealed["first"]["plain"]
    assert first[2] == sealed["first"]["senderKid"]
    assert first[3] == "alice"
    assert bob.open(ROOM, SCOPE, sealed["replay"]["frame"], NOW) == ("replay",)
    second = bob.open(ROOM, SCOPE, sealed["second"]["frame"], NOW + 1000)
    assert second[0] == "opened" and second[1] == sealed["second"]["plain"]
    assert bob.open(ROOM, SCOPE, sealed["tampered"]["frame"], NOW) == ("invalid",)
    assert bob.open(ROOM, SCOPE, sealed["forgedSenderId"]["frame"], NOW + 2000) == ("invalid",)
    pending = sealed["fromPendingDevice"]
    assert [k["kid"] for k in pending["frame"]["keys"]] == pending["keys"]
    assert bob.open(ROOM, SCOPE, pending["frame"], NOW + 3000) == ("held", VECTORS["devices"]["alice2"]["kid"])
    bob.approve(SCOPE, VECTORS["devices"]["alice2"]["kid"])
    assert bob.open(ROOM, SCOPE, pending["frame"], NOW + 3000)[0] == "opened"


def test_forged_frame_does_not_burn_counter():
    bob = bob_with_trust()
    sealed = VECTORS["sealed"]
    assert bob.open(ROOM, SCOPE, sealed["tampered"]["frame"], NOW) == ("invalid",)
    assert bob.open(ROOM, SCOPE, sealed["first"]["frame"], NOW)[0] == "opened"


def test_blocked_sender_is_held_not_opened():
    bob = bob_with_trust()
    bob.block(SCOPE, VECTORS["devices"]["alice"]["kid"])
    assert bob.open(ROOM, SCOPE, VECTORS["sealed"]["first"]["frame"], NOW)[0] == "held"


def test_wiped_prekey_cannot_open():
    bob = bob_with_trust()
    later = VECTORS["afterBobWipesOldPrekey"]["now"]
    bob.wipe_expired(later)
    assert bob.open(ROOM, SCOPE, VECTORS["sealed"]["first"]["frame"], NOW) == ("notForMe",)


def test_other_room_cannot_open():
    bob = bob_with_trust()
    assert bob.open("another-room", SCOPE, VECTORS["sealed"]["first"]["frame"], NOW) == ("invalid",)


def test_kek_and_mlkem():
    k = VECTORS["kek"]
    ct = ref.unb64(k["ct"])
    bob = device("bob")
    spk = next(p for p in bob.prekeys if p["id"] == k["spkId"])
    assert ref.mlkem_decaps(spk["pq_seed"], ct) == ref.unb64(k["ss2"])
    assert ref.x25519(ref.unb64(k["ephPriv"]), spk["x"]) == ref.unb64(k["ss1"])
    assert ref.x25519(spk["x_priv"], ref.unb64u(k["ephPub"])) == ref.unb64(k["ss1"])
    derived = ref.kek(k["room"], k["sender"], k["recipient"], k["spkId"], ref.unb64u(k["ephPub"]), ct,
                      ref.unb64(k["ss1"]), ref.unb64(k["ss2"]))
    assert ref.b64(derived) == k["kek"]


@pytest.mark.parametrize("name", ["twoDevices", "threeDevices"])
def test_safety_numbers(name):
    s = VECTORS["safety"][name]
    digits, qr = ref.safety_number(s["scope"], [ref.unb64u(i) for i in s["idks"]])
    assert digits == s["digits"] and len(digits) == 60
    assert qr == s["qr"]
    reversed_digits, _ = ref.safety_number(s["scope"], [ref.unb64u(i) for i in reversed(s["idks"])])
    assert reversed_digits == digits


def test_safety_number_changes_with_devices():
    assert VECTORS["safety"]["twoDevices"]["digits"] != VECTORS["safety"]["threeDevices"]["digits"]


def test_link_handshake():
    link = VECTORS["link"]
    transcript = ref.link_transcript(link["initiatorHello"], link["responderHello"])
    assert ref.b64u(transcript) == link["transcript"]
    assert ref.ed_verify(ref.unb64u(link["initiatorHello"]["idk"]), ref.unb64u(link["initiatorAuthSig"]),
                         ref.link_auth_message("i", transcript))
    assert ref.ed_verify(ref.unb64u(link["responderHello"]["idk"]), ref.unb64u(link["responderAuthSig"]),
                         ref.link_auth_message("r", transcript))
    assert not ref.ed_verify(ref.unb64u(link["initiatorHello"]["idk"]), ref.unb64u(link["initiatorAuthSig"]),
                             ref.link_auth_message("r", transcript))
    i_eph = ref.unb64(link["initiatorEphPriv"])
    r_eph = ref.unb64(link["responderEphPriv"])
    keys = ref.link_keys(i_eph, ref.x25519_pub(r_eph), transcript)
    assert keys == ref.link_keys(r_eph, ref.x25519_pub(i_eph), transcript)
    assert ref.b64(keys[0]) == link["keyI2R"] and ref.b64(keys[1]) == link["keyR2I"]
    for frame in ("frame0", "frame1"):
        data = ref.unb64(link[frame]["data"])
        assert ref.link_open(keys[0], link[frame]["counter"], data) == link[frame]["plain"]
    with pytest.raises(Exception):
        ref.link_open(keys[0], 1, ref.unb64(link["frame0"]["data"]))
