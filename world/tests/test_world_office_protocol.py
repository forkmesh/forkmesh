#!/usr/bin/env python3
"""Privacy, movement, and seat contracts for Office meeting presence."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD_PATH = ROOT.parent / "world" / "src" / "world.py"

spec = importlib.util.spec_from_file_location(
    "forkmesh_world_office_protocol", WORLD_PATH)
world = importlib.util.module_from_spec(spec)
spec.loader.exec_module(world)

VALID_P256_JWK = {
    "kty": "EC",
    "crv": "P-256",
    "x": "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE",
    "y": "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI",
}


def test_default_office_presence_is_bounded_and_standing():
    state = world.default_office_presence("participant-1", 1000)

    assert set(state) == set(world.OFFICE_PUBLIC_FIELDS)
    assert state == {
        "id": "participant-1",
        "name": "Guest part",
        "accountStatus": "Guest",
        "x": 0.0,
        "y": 0.38,
        "z": 0.0,
        "yaw": 0.0,
        "moving": False,
        "pose": "standing",
        "chairId": "",
        "bindingKey": None,
        "updatedAt": 1000,
    }


def test_office_presence_drops_chat_and_private_room_fields():
    current = world.default_office_presence("participant-1", 1000)
    kind, updated = world.sanitize_office_message({
        "type": "presence",
        "name": "Mallory",
        "bindingKey": VALID_P256_JWK,
        "text": "secret-message",
        "channelId": "a" * 32,
        "privateChannelId": "b" * 32,
        "roomKey": "secret-key",
        "passphrase": "secret-passphrase",
        "attachment": {"name": "private.pdf"},
        "token": "session-secret",
    }, current, 2000, trusted_name="Alice")

    assert kind == "presence"
    assert updated["name"] == "Alice"
    assert updated["bindingKey"] == VALID_P256_JWK
    public = world.public_office_presence(updated)
    assert set(public) == set(world.OFFICE_PUBLIC_FIELDS)
    serialized = repr(public)
    for forbidden in (
        "secret-message",
        "channelId",
        "privateChannelId",
        "secret-key",
        "secret-passphrase",
        "private.pdf",
        "session-secret",
    ):
        assert forbidden not in serialized


def test_office_binding_key_accepts_only_public_p256_jwk():
    current = world.default_office_presence("participant", 1000)
    invalid_keys = [
        {**VALID_P256_JWK, "d": VALID_P256_JWK["x"]},
        {**VALID_P256_JWK, "crv": "P-384"},
        {**VALID_P256_JWK, "kty": "RSA"},
        {**VALID_P256_JWK, "x": "not base64url!"},
        {**VALID_P256_JWK, "extra": "field"},
        "not-an-object",
    ]

    for binding_key in invalid_keys:
        _, updated = world.sanitize_office_message({
            "type": "presence",
            "bindingKey": binding_key,
        }, current, 2000)
        assert updated["bindingKey"] is None


def test_office_movement_is_bounded_but_ignored_while_seated():
    standing = world.default_office_presence("participant", 1000)
    kind, moved = world.sanitize_office_message({
        "type": "move",
        "x": 99999,
        "y": -99999,
        "z": 12.3456,
        "yaw": 99,
        "moving": True,
        "text": "not-presence",
    }, standing, 2000)

    assert kind == "move"
    assert moved["x"] == world.OFFICE_COORD_LIMIT
    assert moved["y"] == -world.OFFICE_COORD_LIMIT
    assert moved["z"] == 12.35
    assert moved["yaw"] == 3.142
    assert moved["moving"] is True
    assert "text" not in moved

    granted, seated = world.allocate_office_seat(moved, "chair-1", {})
    assert granted == "granted"
    _, unchanged = world.sanitize_office_message({
        "type": "move",
        "x": -20,
        "z": -20,
        "moving": True,
    }, seated, 3000)
    assert unchanged["x"] == seated["x"]
    assert unchanged["z"] == seated["z"]
    assert unchanged["moving"] is False


def test_two_participants_cannot_claim_the_same_chair():
    first = world.default_office_presence("first", 1000)
    second = world.default_office_presence("second", 1000)

    granted, first = world.allocate_office_seat(first, "chair-1", {})
    denied, second = world.allocate_office_seat(
        second, "chair-1", {"chair-1": "first"})

    assert granted == "granted"
    assert first["chairId"] == "chair-1"
    assert first["pose"] == "seated"
    assert first["moving"] is False
    assert denied == "denied"
    assert second["chairId"] == ""
    assert second["pose"] == "standing"


def test_seat_release_and_invalid_chair_fail_closed():
    state = world.default_office_presence("participant", 1000)
    _, seated = world.allocate_office_seat(state, "chair-8", {})

    released, standing = world.allocate_office_seat(seated, "", {})
    invalid, unchanged = world.allocate_office_seat(
        standing, "chair-9", {})

    assert released == "released"
    assert standing["chairId"] == ""
    assert standing["pose"] == "standing"
    assert invalid == "invalid"
    assert unchanged == standing


def test_office_protocol_accepts_only_presence_move_and_ping():
    current = world.default_office_presence("participant", 1000)
    assert world.sanitize_office_message({
        "type": "chat", "text": "secret"}, current, 2000) is None
    assert world.sanitize_office_message({
        "type": "seat-request", "chairId": "chair-1"},
        current,
        2000,
    ) is None
    kind, pinged = world.sanitize_office_message(
        {"type": "ping"}, current, 2000)
    assert kind == "ping"
    assert pinged == current


def test_office_rate_and_stale_helpers_are_independent_from_global_world():
    allowed = []
    start = 0
    count = 0
    for _index in range(world.OFFICE_RATE_MAX_PER_WINDOW + 1):
        result, start, count = world.advance_office_rate_window(
            start, count, 1000)
        allowed.append(result)

    assert allowed[:-1] == [True] * world.OFFICE_RATE_MAX_PER_WINDOW
    assert allowed[-1] is False
    reset, start, count = world.advance_office_rate_window(
        start, count, 1000 + world.OFFICE_RATE_WINDOW_MS)
    assert reset is True
    assert count == 1
    assert world.office_presence_is_stale(
        1000, 1000 + world.OFFICE_CLIENT_STALE_MS) is False
    assert world.office_presence_is_stale(
        1000, 1001 + world.OFFICE_CLIENT_STALE_MS) is True
