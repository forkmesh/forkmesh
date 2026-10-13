#!/usr/bin/env python3
"""Name-tailored outfits and the Supporting-member outfit/face perks."""

import importlib.util
from pathlib import Path
from world_scene_source import world_scene_source

ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD_PATH = ROOT.parent / "world" / "src" / "world.py"
SCENE = world_scene_source()
APP = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
DATA = (ROOT.parent / "world" / "public" / "world" / "world-data.js").read_text(
    encoding="utf-8")

spec = importlib.util.spec_from_file_location(
    "forkmesh_world_protocol", WORLD_PATH)
world = importlib.util.module_from_spec(spec)
spec.loader.exec_module(world)


def _presence(payload, status="Supporting member"):
    state = world.default_presence("peer1", 1000)
    state["accountStatus"] = status
    result = world.sanitize_message(
        {"type": "presence", **payload}, state, 2000)
    assert result is not None
    return result[1]


def test_outfit_cut_ids_stay_in_lockstep_across_protocol_data_and_scene():
    cuts = sorted(world.WORLD_OUTFIT_STYLE_VALUES - {""})
    assert cuts
    for cut in cuts:
        # The picker option in world-data.js...
        assert 'id: "%s"' % cut in DATA
        # ...and the painter that actually draws that cut in world-scene.js.
        assert "%s(context, rng, kit)" % cut in SCENE


def test_supporting_member_may_pin_an_outfit_cut():
    assert _presence({"outfitStyle": "circuit"})["outfitStyle"] == "circuit"


def test_guest_and_registered_outfit_cut_requests_reset_to_seeded():
    for status in ("Guest", "Registered"):
        assert _presence(
            {"outfitStyle": "circuit"}, status=status)["outfitStyle"] == ""


def test_unknown_outfit_cut_resets_to_seeded():
    assert _presence({"outfitStyle": "tuxedo"})["outfitStyle"] == ""


def test_face_image_is_a_boolean_for_authenticated_members_only():
    assert _presence({"faceImage": True})["faceImage"] is True
    assert _presence({"faceImage": "yes"})["faceImage"] is False
    assert _presence({"faceImage": 1})["faceImage"] is False
    assert _presence({"faceImage": True}, status="Guest")["faceImage"] is False
    assert _presence(
        {"faceImage": True}, status="Registered")["faceImage"] is True


def test_outfit_and_face_fields_are_public_presence_fields():
    for field in ("outfitStyle", "faceImage"):
        assert field in world.WORLD_PUBLIC_FIELDS
        assert field in world.default_presence("peer1", 1000)


def test_presence_frames_never_carry_image_bytes():
    # Only the opt-in boolean travels over presence; peers resolve the actual
    # image from the already-public, edge-cached account lookup.
    assert "faceImage:" in APP
    assert "syncWorldFaceImages" in APP
    assert "/api/accounts/" in APP
    assert "avatarPng" not in SCENE
    assert "base64" not in SCENE


def test_every_identity_has_a_stable_generated_face_and_verified_pin():
    assert "function proceduralAvatarFaceTexture" in SCENE
    assert "faceIdentityKey: identity.id || identity.name" in SCENE
    assert 'verifiedPin.name = "forkmesh-verified-email-pin"' in SCENE
    assert "identity?.emailVerified === true" in SCENE


def test_signed_in_members_can_upload_a_compact_face_photo():
    assert "function compactWorldAvatar" in APP
    assert "WORLD_AVATAR_MAX_BYTES = 64 * 1024" in APP
    assert "for (const size of [128, 112, 96, 80, 64, 48])" in APP
    assert "data-world-avatar-upload" in APP
    assert "Change and save avatar" in APP
    assert '"/api/accounts/profile"' in APP
    assert "storeWorldSession({" in APP
    assert "this.updateIdentityUI();" in APP
    assert "this.identity.accountStatus === \"Guest\"" in APP


def test_account_avatar_photo_is_the_default_face_but_can_be_disabled():
    defaults = APP.split("function defaultSettings()", 1)[1].split(
        "function mergeSettings", 1)[0]
    merged = APP.split("function mergeSettings(stored)", 1)[1].split(
        "function publicIdentity", 1)[0]
    assert "faceImage: true," in defaults
    assert 'faceImagePreference: "photo",' in defaults
    assert 'stored?.faceImagePreference === "generated"' in merged
    assert 'faceImage: faceImagePreference === "photo",' in merged
    assert 'enabled === true ? "photo" : "generated"' in APP
    assert "setFaceImage(faceImage.checked)" in APP


def test_public_directory_avatar_bytes_are_reused_for_world_faces():
    directory = APP.split("function memberDirectoryAvatar", 1)[1].split(
        "function normalizeInactiveWorldPlayers", 1)[0]
    lounge = SCENE.split("const memberAvatar = String(member.avatar", 1)[1].split(
        "syncAvatarActivity(THREE, figure", 1)[0]
    assert "user?.avatarPng" in directory
    assert "data:image/png;base64,${avatarPng}" in directory
    assert "applyAvatarFaceImage(THREE, figure, memberAvatar)" in lounge


def test_every_signed_in_figure_wears_its_public_face_at_all_times():
    # The account avatar is public directory data, so a walking peer, an
    # office attendee and a persisted contributor row all wear it from the
    # moment their figure exists. Opening a chest card is for the richer
    # profile, never for revealing a face that was already public.
    dresser = SCENE.split("function dressMemberFace(avatar, identity = {})", 1)[1].split(
        "function setRemotePlayers", 1)[0]
    assert "if (identity.faceImage === false) return;" in dresser
    assert 'String(identity.accountStatus || "Guest") === "Guest"' in dresser
    assert "applyAvatarFaceImage(THREE, avatar, photo)" in dresser
    # The directory row the lounge already paints from is kept as a badge fact
    # so every other figure kind can read the same bytes.
    assert 'avatar: String(member?.avatar || ""),' in SCENE
    remote = SCENE.split("function setRemotePlayers(players = [])", 1)[1].split(
        "function setRemotePlayerMovement", 1)[0]
    assert "dressMemberFace(avatar, badgeIdentity);" in remote
    office = SCENE.split(
        "function setOfficeParticipants(participants = [])", 1)[1].split(
        "officeLobbyPlayer.visible =", 1)[0]
    assert "dressMemberFace(avatar, participantIdentity);" in office


def test_a_figure_without_a_presence_flag_still_wears_its_photo():
    # Only an explicit opt-out keeps the generated face: an identity that
    # never carried the flag at all is a persisted contributor row or an
    # office attendee, not a member who asked for the drawn portrait.
    remote = SCENE.split("function setRemotePlayers(players = [])", 1)[1].split(
        "function setRemotePlayerMovement", 1)[0]
    assert "faceImage: remote.faceImage !== false," in remote
    badge = SCENE.split("function updateAvatarBadge(", 1)[1].split(
        "function startAvatarWave", 1)[0]
    assert (
        "if (identity.faceImage === false && avatar.userData.faceImageUrl)"
        in badge
    )
    wearers = APP.split("syncWorldFaceImages(players) {", 1)[1].split(
        "setLightLevel(value) {", 1)[0]
    assert "peer?.faceImage !== false &&" in wearers
    assert 'String(peer.accountStatus || "Guest") !== "Guest"' in wearers


def test_the_face_dresser_reaches_office_meeting_figures():
    dresser = SCENE.split("function setAvatarFaceImage(peerId, url)", 1)[1].split(
        "function handleChestControl", 1)[0]
    assert "officeParticipants.get(id)" in dresser
    assert "applyAvatarFaceImage(THREE, officeParticipant, url)" in dresser


def test_chest_textures_use_mipmaps_and_anisotropic_filtering():
    sharpener = SCENE.split("function sharpenCanvasTextTexture", 1)[1].split(
        "function repaintCanvasTexture", 1)[0]
    badge = SCENE.split("function badgeTexture(", 1)[1].split(
        "const BADGE_FOLLOW_PILL", 1)[0]
    assert "THREE.LinearMipmapLinearFilter" in sharpener
    assert "texture.anisotropy = 8" in sharpener
    assert "return sharpenCanvasTextTexture(THREE, texture);" in badge


def test_outfits_are_tailored_from_the_public_name():
    assert "function outfitRandom" in SCENE
    assert "function countryShirtTexture" in SCENE
    assert "function tailorOutfitDetails" in SCENE
    assert "OUTFIT_CUTS" in SCENE
    # The tailor keys off the same public identity fields the badge shows.
    assert "identity.outfitStyle" in SCENE
    assert "identity.outfitColor" in SCENE


def test_directory_members_never_cost_a_profile_request():
    wearers = APP.split("syncWorldFaceImages(players) {", 1)[1].split(
        "setLightLevel(value) {", 1)[0]
    assert ".filter(([name]) => name)," in wearers
    assert "if (directoryAvatars.has(account)) {" in wearers
    assert "if (!directoryLoaded) return;" in wearers
    assert wearers.index("if (directoryAvatars.has(account)) {") < wearers.index(
        "if (!directoryLoaded) return;") < wearers.index("/api/accounts/")
