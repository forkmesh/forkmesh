#!/usr/bin/env python3
"""Privacy and lifecycle contract for the transient multiplayer world."""

import ast
import asyncio
import base64
import hmac
import importlib.util
import ipaddress
import json
import re
import tomllib
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
WORLD_PATH = ROOT.parent / "world" / "src" / "world.py"
WORLD_INFRASTRUCTURE_PATH = ROOT.parent / "world" / "src" / "world_infrastructure.py"
WORLD_DURABLE_OBJECTS_PATH = (
    ROOT.parent / "world" / "src" / "world_durable_objects.py"
)
WRANGLER = ROOT / "wrangler.toml"
STATIC_ROUTES = ROOT / "src" / "static_routes.py"
WORLD_WORKER = ROOT.parent / "world" / "worker.js"
WORLD_WRANGLER = ROOT.parent / "world" / "wrangler.toml"
SCHEMA = ROOT / "src" / "schema.py"
INACTIVE_MIGRATION = ROOT / "migrations" / "0051_world_presence_spaces.sql"
WORLD_JS = ROOT.parent / "world" / "public" / "world" / "world.js"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
WORLD_DURABLE_OBJECTS_TEXT = WORLD_DURABLE_OBJECTS_PATH.read_text(
    encoding="utf-8"
)
# Private handover close code: one account keeps one avatar, and the newest
# device drives it. Kept in lockstep with entry.py and world.js.
WORLD_ACCOUNT_TAKEOVER_CODE = 4009

spec = importlib.util.spec_from_file_location("forkmesh_world_protocol", WORLD_PATH)
world = importlib.util.module_from_spec(spec)
spec.loader.exec_module(world)
infrastructure_spec = importlib.util.spec_from_file_location(
    "forkmesh_world_infrastructure",
    WORLD_INFRASTRUCTURE_PATH,
)
world_infrastructure = importlib.util.module_from_spec(infrastructure_spec)
infrastructure_spec.loader.exec_module(world_infrastructure)


def _route_table():
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    for node in ast.parse(runtime).body:
        if isinstance(node, ast.ClassDef) and node.name == "Default":
            return ast.unparse(node)
    raise AssertionError("Default not found in app_runtime.py")


def _top_level_node(name):
    sources = (
        (ENTRY_TEXT, ENTRY),
        (WORLD_DURABLE_OBJECTS_TEXT, WORLD_DURABLE_OBJECTS_PATH),
    )
    for source, path in sources:
        for node in ast.parse(source, filename=str(path)).body:
            if isinstance(
                node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
            ) and node.name == name:
                return node
    raise AssertionError("%s not found in Worker sources" % name)


def _function_without_docstring(name):
    node = _top_level_node(name)
    body = list(node.body)
    if (body and isinstance(body[0], ast.Expr)
            and isinstance(body[0].value, ast.Constant)
            and isinstance(body[0].value.value, str)):
        body = body[1:]
    clone = ast.FunctionDef(
        name=node.name,
        args=node.args,
        body=body,
        decorator_list=[],
        returns=node.returns,
        type_comment=node.type_comment,
    )
    return ast.unparse(ast.fix_missing_locations(clone))


class _Headers:
    def __init__(self, values=None, allowed=None):
        self.values = {
            str(key).lower(): value for key, value in (values or {}).items()
        }
        self.allowed = set(self.values if allowed is None else allowed)
        self.read = []

    def get(self, name, default=None):
        name = str(name).lower()
        self.read.append(name)
        if name not in self.allowed:
            raise AssertionError("unexpected privacy-sensitive header read: " + name)
        return self.values.get(name, default)


def _load_context_handler(now=17_500_000):
    nodes = [
        _top_level_node("world_request_country"),
        _top_level_node("world_context_handler"),
    ]
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))

    def json_response(data, status=200, cache_control=None, extra_headers=None,
                      **_kwargs):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    namespace = {
        "method_name": lambda request: str(request.method).upper(),
        "json_response": json_response,
        "world_protocol": world,
        "ipaddress": ipaddress,
        "Date": SimpleNamespace(now=lambda: now),
        "MAX_CONNECTIONS": 128,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["world_context_handler"]


def test_context_returns_only_country_and_shared_clock_fields():
    now = 91_234_567
    headers = _Headers({"cf-ipcountry": "us"}, allowed={"cf-ipcountry"})
    request = SimpleNamespace(method="GET", headers=headers)
    response = _load_context_handler(now)(request)

    assert response["status"] == 200
    assert response["data"] == {
        "ok": True,
        "countryCode": "US",
        "serverTimeMs": now,
        "worldTimeMs": now % world.WORLD_DAY_LENGTH_MS,
        "worldDayLengthMs": world.WORLD_DAY_LENGTH_MS,
        "worldConnections": 64,
        "worldMessagesPerSecond": 4,
        "chatConnections": 128,
    }
    assert headers.read == ["cf-ipcountry"]
    assert response["cache_control"] == "no-store, max-age=0, must-revalidate"
    assert response["headers"]["x-content-type-options"] == "nosniff"
    serialized = repr(response["data"]).lower()
    for forbidden in ("ip", "user-agent", "useragent", "latitude", "longitude",
                      "repo", "wallet", "url"):
        assert forbidden not in serialized


def test_context_rejects_non_get_without_reading_headers():
    headers = _Headers({}, allowed=set())
    request = SimpleNamespace(method="POST", headers=headers)
    response = _load_context_handler()(request)
    assert response["status"] == 405
    assert response["headers"]["allow"] == "GET"
    assert headers.read == []


def test_context_prefers_trusted_request_cf_country_without_header_read():
    headers = _Headers({"cf-ipcountry": "GB"}, allowed=set())
    request = SimpleNamespace(
        method="GET",
        headers=headers,
        cf=SimpleNamespace(country="JP"),
    )
    response = _load_context_handler()(request)
    assert response["data"]["countryCode"] == "JP"
    assert headers.read == []


def test_context_never_reflects_a_connection_card_back_to_the_visitor():
    """Connection details belong to account session management, not context.

    The World context is served to every visitor, guests included, so it reads
    no address or user-agent header at all. The owner-only sign-in address now
    lives on /api/accounts/sessions behind a live session token.
    """
    headers = _Headers(
        {
            "cf-ipcountry": "US",
            "cf-connecting-ip": "2001:0db8::0001",
            "user-agent": "  Example Browser/7.2\tLinux  ",
            "x-forwarded-for": "198.51.100.88",
        },
        allowed={"cf-ipcountry"},
    )
    response = _load_context_handler()(SimpleNamespace(
        method="GET",
        headers=headers,
    ))
    assert "securityDetails" not in response["data"]
    assert headers.read == ["cf-ipcountry"]
    assert response["cache_control"] == "no-store, max-age=0, must-revalidate"


def test_country_code_is_coarse_and_rejects_cloudflare_sentinels():
    assert world.approximate_country_code("ca") == "CA"
    assert world.approximate_country_code(" EU ") == "EU"
    for raw in ("", "XX", "T1", "USA", "1A", "<script>"):
        assert world.approximate_country_code(raw) == ""


def test_presence_message_discards_sensitive_and_unknown_fields():
    current = world.default_presence("abcd1234", 1000)
    payload = {
        "type": "presence",
        "name": "<b>Alice</b>",
        "browser": "firefox",
        "os": "linux",
        "status": "available",
        "localTime": "09:32",
        "activityCategory": "viewing-repository",
        "publicDoor": "knock",
        "shareCountry": True,
        # All of these must be ignored rather than reflected.
        "id": "attacker-picked",
        "countryCode": "GB",
        "ip": "203.0.113.10",
        "userAgent": "private-browser-build",
        "repo": "secret-owner/private-repo",
        "wallet": "private-wallet",
        "url": "https://example.test/private?q=secret",
        "formContents": "secret",
    }
    kind, updated = world.sanitize_message(
        payload, current, 2000, country_source="US")
    public = world.public_presence(updated)

    assert kind == "presence"
    assert set(public) == set(world.WORLD_PUBLIC_FIELDS)
    assert public["id"] == "abcd1234"
    assert public["countryCode"] == "US"
    assert "<" not in public["name"] and ">" not in public["name"]
    assert public["browser"] == "firefox"
    assert public["os"] == "linux"
    assert public["status"] == "available"
    assert public["localTime"] == "09:32"
    assert public["activityCategory"] == "viewing-repository"
    assert public["publicDoor"] == "knock"
    rendered = repr(public)
    for secret in (
        "203.0.113.10", "private-browser-build",
        "secret-owner/private-repo", "private-wallet",
        "https://example.test/private?q=secret",
    ):
        assert secret not in rendered


def test_movement_is_bounded_quantized_and_profile_fields_cannot_change():
    current = world.default_presence("peer", 1000)
    current["countryCode"] = "DE"
    kind, updated = world.sanitize_message({
        "type": "move",
        "x": 999999,
        "y": -999999,
        "z": 12.34567,
        "yaw": 99,
        "lookYaw": -99,
        "lookPitch": 99,
        "moving": True,
        "name": "not accepted in movement",
        "countryCode": "FR",
        "wallet": "never-relay-me",
    }, current, 2000)

    assert kind == "move"
    assert updated["x"] == world.WORLD_COORD_LIMIT
    assert updated["y"] == -world.WORLD_COORD_LIMIT
    assert updated["z"] == 12.35
    assert updated["yaw"] == 3.142
    assert updated["lookYaw"] == -3.142
    assert updated["lookPitch"] == 1.561
    assert updated["moving"] is True
    assert updated["name"] == current["name"]
    assert updated["countryCode"] == "DE"
    delta = world.movement_delta(updated)
    assert set(delta) == {
        "type", "id", "x", "y", "z", "yaw", "lookYaw", "lookPitch",
        "moving", "updatedAt",
    }
    assert "never-relay-me" not in repr(delta)


def test_presence_envelope_accepts_district_package_spaces_by_shape():
    # A district package (the aquarium Sky Office package sits 14,400 units
    # out) fits the finite envelope, and its namespaced spaces pass the relay
    # without the relay knowing which packages exist.
    assert world.WORLD_COORD_LIMIT >= 14_400 + 112
    for space in (
        "pkg-forkmesh-sky-office-campus",
        "pkg-forkmesh-sky-office-aquarium-lobby",
        "pkg-forkmesh-sky-office-drone-roof",
    ):
        assert space not in world.WORLD_SPACE_VALUES
        assert world.world_space(space) == space
    assert world.world_space("office-lobby") == "office-lobby"
    for junk in (
        "pkg-",
        "pkg-office",
        "PKG-forkmesh-sky-office-campus",
        "pkg-forkmesh-sky-office-campus/../x",
        "pkg-" + "-".join(["a"] * 12),
        "pkg-" + "a" * 70,
        "",
        None,
    ):
        assert world.world_space(junk) == "town-square"


def test_step_credit_counts_gait_distance_and_rejects_teleports():
    previous = world.default_presence("peer", 1_000)
    previous.update({"x": 0.0, "z": 0.0, "moving": True})
    current = dict(previous, x=3.2, moving=True)

    steps, carry, observed = world.movement_step_credit(
        previous, current, 1_000, 2_000)
    assert (steps, carry, observed) == (2, 0.0, 2_000)

    # A partial stride survives in the private socket attachment and becomes a
    # whole step on the next accepted sample.
    partial = dict(current, x=4.0)
    steps, carry, _ = world.movement_step_credit(
        current, partial, observed, 3_000, carry)
    assert steps == 0
    completed = dict(partial, x=4.8)
    assert world.movement_step_credit(
        partial, completed, 3_000, 4_000, carry)[:2] == (1, 0.0)

    teleported = dict(current, x=500.0)
    assert world.movement_step_credit(
        current, teleported, 2_000, 3_000, 0.5)[:2] == (0, 0.0)
    assert world.movement_step_credit(
        current, dict(current, x=9.6), 2_000, 8_001)[:2] == (0, 0.0)


def test_step_credit_ignores_vertical_and_cross_space_movement():
    previous = world.default_presence("peer", 1_000)
    previous.update({
        "x": 1.0, "y": 0.4, "z": 1.0, "moving": True,
        "space": "town-square",
    })
    vertical = dict(previous, y=100.0)
    assert world.movement_step_credit(
        previous, vertical, 1_000, 2_000)[:2] == (0, 0.0)
    other_space = dict(previous, x=4.2, space="east")
    assert world.movement_step_credit(
        previous, other_space, 1_000, 2_000)[:2] == (0, 0.0)


def test_country_privacy_toggle_hides_and_restores_only_edge_country():
    current = world.default_presence("peer", 1000)
    assert current["countryCode"] == ""
    _, hidden = world.sanitize_message({
        "type": "presence",
        "shareCountry": False,
        "countryCode": "US",
    }, current, 2000, country_source="NZ")
    assert hidden["countryCode"] == ""

    _, shown = world.sanitize_message({
        "type": "presence",
        "shareCountry": True,
        "countryCode": "US",
    }, hidden, 3000, country_source="NZ")
    assert shown["countryCode"] == "NZ"


def test_local_time_is_optional_bounded_and_never_inferred_by_server():
    current = world.default_presence("peer", 1000)
    assert current["localTime"] == ""

    _, shared = world.sanitize_message({
        "type": "presence",
        "localTime": "23:59",
    }, current, 2000)
    assert shared["localTime"] == "23:59"

    for invalid in ("9:32 AM", "24:00", "12:60", "09:32 America/New_York",
                    "<script>", None):
        _, rejected = world.sanitize_message({
            "type": "presence",
            "localTime": invalid,
        }, shared, 3000)
        assert rejected["localTime"] == ""


def test_activity_is_generalized_allowlisted_and_never_accepts_urls():
    current = world.default_presence("peer", 1000)
    assert current["activityCategory"] == "hidden"
    _, shared = world.sanitize_message({
        "type": "presence",
        "activityCategory": "browsing-code-visualization",
    }, current, 2000)
    assert shared["activityCategory"] == "browsing-code-visualization"
    for invalid in (
        "viewing /private/repository",
        "https://example.test/private?q=secret",
        "searching for wallet seed",
    ):
        _, hidden = world.sanitize_message({
            "type": "presence",
            "activityCategory": invalid,
        }, shared, 3000)
        assert hidden["activityCategory"] == "hidden"


def test_emoji_status_is_one_bounded_pictograph_and_one_bounded_word():
    for emoji in (
        "😀",
        "🧑🏽‍💻",
        "👨‍👩‍👧‍👦",
        "🏳️‍🌈",
        "🇺🇸",
        "1️⃣",
        "❤️",
    ):
        assert world.clean_status_emoji(emoji) == emoji
    for invalid in (
        "",
        "hello",
        "😀😀",
        "<script>",
        "https://example.test/",
        "🇺",
        "\u200d",
        "A😀",
        "😀 private",
    ):
        assert world.clean_status_emoji(invalid) == ""

    for note in ("coding", "on-call", "débogage", "コード", "l’équipe"):
        assert world.clean_status_note(note) == note
    for invalid in (
        "two words",
        "https://example.test",
        "<script>",
        "/private",
        "\u202esecret",
        "x" * (world.WORLD_STATUS_NOTE_MAX + 1),
    ):
        assert world.clean_status_note(invalid) == ""

    current = world.default_presence("peer", 1000)
    assert current["statusEmoji"] == ""
    assert current["statusNote"] == ""
    kind, shared = world.sanitize_message({
        "type": "presence",
        "statusEmoji": "🧑🏽‍💻",
        "statusNote": "coding",
        "statusDetail": "private repository URL",
    }, current, 2000)
    assert kind == "presence"
    public = world.public_presence(shared)
    assert public["statusEmoji"] == "🧑🏽‍💻"
    assert public["statusNote"] == "coding"
    assert "statusDetail" not in public
    assert "private repository URL" not in repr(public)

    _, rejected = world.sanitize_message({
        "type": "presence",
        "statusEmoji": "😀😀",
        "statusNote": "two words",
    }, shared, 3000)
    assert rejected["statusEmoji"] == ""
    assert rejected["statusNote"] == ""

    _, restored = world.sanitize_message({
        "type": "presence",
        "statusEmoji": "🚀",
        "statusNote": "shipping",
    }, rejected, 4000)
    _, moved = world.sanitize_message({
        "type": "move",
        "x": 3,
        "statusEmoji": "🔐",
        "statusNote": "secret",
    }, restored, 5000)
    assert moved["statusEmoji"] == "🚀"
    assert moved["statusNote"] == "shipping"


def test_coarse_activity_metadata_is_bounded_and_privacy_gated():
    assert world.WORLD_FIRST_VISIT_AGE_VALUES == {
        "this-session", "today", "this-week", "this-month", "this-year",
        "over-a-year", "hidden",
    }
    current = world.default_presence("peer", 1000)
    assert current["inputActive"] is False
    assert current["visitCount"] == 0
    assert current["firstVisitAge"] == "hidden"

    _, shared = world.sanitize_message({
        "type": "presence",
        "activityCategory": "viewing-repository",
        "inputActive": True,
        "visitCount": 27,
        "firstVisitAge": "this-month",
        "url": "https://example.test/private?q=secret",
    }, current, 2000)
    assert shared["inputActive"] is True
    assert shared["visitCount"] == 27
    assert shared["firstVisitAge"] == "this-month"
    assert "url" not in world.public_presence(shared)
    assert "example.test" not in repr(world.public_presence(shared))

    _, bounded = world.sanitize_message({
        "type": "presence",
        "activityCategory": "exploring-town-square",
        "visitCount": 5000,
        "firstVisitAge": "over-a-year",
    }, current, 3000)
    assert bounded["visitCount"] == 999
    assert bounded["firstVisitAge"] == "over-a-year"
    _, lower_bounded = world.sanitize_message({
        "type": "presence",
        "activityCategory": "exploring-town-square",
        "visitCount": -1,
    }, current, 3500)
    assert lower_bounded["visitCount"] == 0

    _, rejected = world.sanitize_message({
        "type": "presence",
        "activityCategory": "exploring-town-square",
        "inputActive": "true",
        "visitCount": "999",
        "firstVisitAge": "https://example.test/history",
    }, current, 4000)
    assert rejected["inputActive"] is False
    assert rejected["visitCount"] == 0
    assert rejected["firstVisitAge"] == "hidden"

    _, hidden = world.sanitize_message({
        "type": "presence",
        "activityCategory": "hidden",
        "inputActive": True,
        "visitCount": 42,
        "firstVisitAge": "today",
    }, shared, 5000)
    assert hidden["inputActive"] is False
    assert hidden["visitCount"] == 0
    assert hidden["firstVisitAge"] == "hidden"


def test_exact_first_seen_and_joined_ages_stay_bounded_and_gated():
    now = 1700000000000
    current = world.default_presence("peer", now)
    assert current["firstSeenMinutes"] == 0
    assert current["joinedAt"] == 0
    assert "firstSeenMinutes" in world.public_presence(current)
    assert "joinedAt" in world.public_presence(current)

    account = dict(current, accountStatus="Registered")
    _, shared = world.sanitize_message({
        "type": "presence",
        "activityCategory": "viewing-repository",
        "firstSeenMinutes": 137,
        "joinedAt": now - 90 * 24 * 60 * 60 * 1000,
    }, account, now)
    assert shared["firstSeenMinutes"] == 137
    assert shared["joinedAt"] == now - 90 * 24 * 60 * 60 * 1000

    _, bounded = world.sanitize_message({
        "type": "presence",
        "activityCategory": "viewing-repository",
        "firstSeenMinutes": 99999999,
        # A "joined tomorrow" claim, and every pre-2020 or non-integer value,
        # collapses to no joined date rather than a nonsense badge line.
        "joinedAt": now + 60000,
    }, account, now)
    assert bounded["firstSeenMinutes"] == world.WORLD_FIRST_SEEN_MAX_MINUTES
    assert bounded["joinedAt"] == 0

    _, rejected = world.sanitize_message({
        "type": "presence",
        "activityCategory": "viewing-repository",
        "firstSeenMinutes": "137",
        "joinedAt": "2020-01-01",
    }, account, now)
    assert rejected["firstSeenMinutes"] == 0
    assert rejected["joinedAt"] == 0

    _, hidden = world.sanitize_message({
        "type": "presence",
        "activityCategory": "hidden",
        "firstSeenMinutes": 137,
    }, shared, now)
    assert hidden["firstSeenMinutes"] == 0

    # A connection the routing Worker never authenticated has no joined date.
    _, guest = world.sanitize_message({
        "type": "presence",
        "activityCategory": "viewing-repository",
        "joinedAt": now - 90 * 24 * 60 * 60 * 1000,
    }, current, now)
    assert guest["joinedAt"] == 0


def test_arrival_slots_fill_unique_forward_facing_rows_of_ten():
    positions = [world.arrival_position(slot) for slot in range(21)]
    assert len({(item["x"], item["z"]) for item in positions}) == 21
    assert [item["z"] for item in positions[:10]] == [30.0] * 10
    assert [item["z"] for item in positions[10:20]] == [27.9] * 10
    assert positions[20]["z"] == 25.8
    assert all(item["yaw"] == 0.0 for item in positions)
    assert all(item["y"] == 0.38 for item in positions)
    assert world.first_available_arrival_slot([0, 2, 3]) == 1
    assert world.first_available_arrival_slot(range(63)) == 63


def test_arrival_cells_occupied_by_standing_visitors_read_as_used_slots():
    # Exact cell centres and near-misses within the clearance both block.
    assert world.arrival_slot_near_position(-8.1, 30.0) == 0
    assert world.arrival_slot_near_position(-6.3, 30.0) == 1
    assert world.arrival_slot_near_position(-7.6, 29.4) == 0
    assert world.arrival_slot_near_position(-8.1, 27.9) == 10
    # Positions away from the grid, and junk, never reserve anything.
    assert world.arrival_slot_near_position(0.0, 10.0) == -1
    assert world.arrival_slot_near_position(-8.1, 30.0 - 1.0) == -1
    assert world.arrival_slot_near_position("junk", None) == -1
    assert world.arrival_slot_near_position(float("nan"), 30.0) == -1
    assert world.arrival_slot_near_position(float("inf"), 30.0) == -1
    # A visitor standing on cell 0 with an unrelated reservation still keeps
    # a newcomer off cell 0.
    assert world.first_available_arrival_slot(
        [7, world.arrival_slot_near_position(-8.1, 30.0)]) == 1


def _world_fetch_runtime(now=50_000):
    class DurableObject:
        pass

    class _FetchSocket:
        def __init__(self):
            self.attachment = None
            self.sent = []
            self.closes = []

        def serializeAttachment(self, attachment):
            self.attachment = SimpleNamespace(**attachment)

        def deserializeAttachment(self):
            return self.attachment

        def send(self, message):
            self.sent.append(json.loads(message))

        def close(self, code=1000, reason=""):
            self.closes.append((code, reason))

    class _Ctx:
        def __init__(self):
            self.sockets = []
            self.tags = {}

        def acceptWebSocket(self, ws, tags=None):
            self.sockets.append(ws)
            self.tags[ws] = tuple(tags or ())

        def getWebSockets(self, tag=None):
            if tag is None:
                return list(self.sockets)
            return [
                socket for socket in self.sockets
                if tag in self.tags.get(socket, ())
            ]

    class _Pair:
        @staticmethod
        def new():
            client, server = _FetchSocket(), _FetchSocket()
            return SimpleNamespace(object_values=lambda: (client, server))

    clock = {"now": now}
    counter = {"n": 0}

    def peer_id():
        counter["n"] += 1
        return "peer%04d" % counter["n"]

    namespace = {
        "DurableObject": DurableObject,
        "Date": SimpleNamespace(now=lambda: clock["now"]),
        "world_protocol": world,
        "to_js": lambda value: value,
        "json": json,
        "base64": base64,
        "hmac": hmac,
        "WORLD_ACCOUNT_TAKEOVER_CODE": WORLD_ACCOUNT_TAKEOVER_CODE,
        "re": re,
        "urlparse": urlparse,
        "new_world_peer_id": peer_id,
        "method_name": lambda request: str(
            getattr(request, "method", "GET")).upper(),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "json_response": lambda data, status=200, **_kwargs: SimpleNamespace(
            status=status, data=data),
        "WebSocketPair": _Pair,
        "JsResponse": SimpleNamespace(
            new=lambda *args, **kwargs: SimpleNamespace(status=101)),
    }
    traffic_writes = []
    step_writes = []

    async def d1_run(_env, sql, *args):
        traffic_writes.append((sql, args))
        return None

    async def d1_first(_env, sql, *args):
        step_writes.append((sql, args))
        return {"total_steps": int(args[1]) + 1_000}

    namespace.update({
        "DURABLE_OBJECT_BINDING_RE": re.compile(r"[A-Z][A-Z0-9_]{0,63}"),
        "DURABLE_OBJECT_TRAFFIC_FLUSH_MS": 60_000,
        "DURABLE_OBJECT_TRAFFIC_FLUSH_BYTES": 262_144,
        "DURABLE_OBJECT_TRAFFIC_MAX": 9_007_199_254_740_991,
        "d1_run": d1_run,
        "d1_first": d1_first,
    })
    for name in (
        "durable_object_traffic_note",
        "durable_object_traffic_flush",
        "_ws_attachment",
        "_ws_attr",
        "ForkMeshWorld",
    ):
        node = _top_level_node(name)
        module = ast.fix_missing_locations(
            ast.Module(body=[node], type_ignores=[]))
        exec(compile(module, str(ENTRY), "exec"), namespace)
    instance = namespace["ForkMeshWorld"]()
    instance.ctx = _Ctx()
    instance.env = object()
    instance.traffic_writes = traffic_writes
    instance.step_writes = step_writes
    return instance, clock


class _FetchRequest:
    url = "https://forkmesh.example/api/world/ws"

    class headers:
        @staticmethod
        def get(name):
            return "websocket" if str(name).lower() == "upgrade" else None


class _StatusFetchRequest(_FetchRequest):
    url = "https://forkmesh.example/api/status/ws"


def test_status_signal_socket_is_separate_from_world_presence_capacity():
    instance, _clock = _world_fetch_runtime()
    response = asyncio.run(instance.fetch(_StatusFetchRequest()))
    assert response.status == 101
    status_socket = instance.ctx.sockets[-1]
    assert status_socket.attachment.channel == "status"
    assert status_socket.sent == [{"type": "ready"}]
    assert instance.ctx.getWebSockets("status") == [status_socket]
    assert instance.ctx.getWebSockets("world") == []

    asyncio.run(instance.fetch(_FetchRequest()))
    assert len(instance.ctx.getWebSockets("world")) == 1
    assert len(instance.ctx.getWebSockets("status")) == 1


def _signed_in_request(account, nonce, account_status="Registered"):
    """A relay-verified account claim, as the outer Worker forwards it."""
    claim = base64.urlsafe_b64encode(json.dumps({
        "name": account,
        "accountStatus": account_status,
        "nodeCount": 0,
        "ticketNonce": nonce,
    }).encode()).decode().rstrip("=")

    class _Request:
        url = "https://forkmesh.example/api/world/ws"

        class headers:
            @staticmethod
            def get(name):
                key = str(name).lower()
                if key == "upgrade":
                    return "websocket"
                if key == "x-forkmesh-world-claim":
                    return claim
                return None

    return _Request()


def test_account_presence_key_folds_only_verified_names():
    assert world.account_presence_key("Jett") == "jett"
    assert world.account_presence_key("JETT") == world.account_presence_key(
        "jett")
    assert world.account_presence_key("jett") != world.account_presence_key(
        "jetta")
    # No verified account behind the socket: guests are never combined, and a
    # name that only survives sanitizing as a generic fallback folds nothing.
    assert world.account_presence_key("") == ""
    assert world.account_presence_key(None) == ""
    assert world.account_presence_key("<<<>>>") == ""
    assert world.account_presence_key("  ") == ""
    # Surrounding whitespace is not a different person.
    assert world.account_presence_key(" jett ") == "jett"


def test_second_device_takes_over_the_one_avatar_for_the_same_account():
    instance, clock = _world_fetch_runtime()
    asyncio.run(instance.fetch(_FetchRequest()))
    onlooker = instance.ctx.sockets[-1]

    clock["now"] += 15_000
    asyncio.run(instance.fetch(_signed_in_request("jett", "nonce-aaaa1111")))
    first = instance.ctx.sockets[-1]
    assert first.attachment.account_key == "jett"
    # The private fold key stays out of every frame the world ever sends.
    assert "account_key" not in json.dumps(first.sent + onlooker.sent)

    clock["now"] += 15_000
    asyncio.run(instance.fetch(_signed_in_request("JETT", "nonce-bbbb2222")))
    second = instance.ctx.sockets[-1]
    assert second is not first

    # The earlier device is retired with the handover code, and the onlooker
    # is told to drop that avatar rather than being left with two "jett"s.
    assert first.closes[0] == (
        WORLD_ACCOUNT_TAKEOVER_CODE, "moved to your newest device")
    assert first.attachment.departed is True
    leaves = [frame for frame in onlooker.sent if frame["type"] == "leave"]
    assert leaves == [{"type": "leave", "id": first.attachment.id}]
    joins = [frame for frame in onlooker.sent if frame["type"] == "join"]
    assert [frame["peer"]["id"] for frame in joins] == [
        first.attachment.id, second.attachment.id]

    # The newest device's own snapshot holds the onlooker and no retired twin,
    # and it reuses the arrival cell the replaced socket just released.
    welcome = second.sent[0]
    assert [peer["id"] for peer in welcome["peers"]] == [onlooker.attachment.id]
    assert (welcome["self"]["x"], welcome["self"]["z"]) == (-6.3, 30.0)


def test_takeover_never_combines_guests_or_other_accounts():
    instance, clock = _world_fetch_runtime()
    asyncio.run(instance.fetch(_FetchRequest()))
    guest = instance.ctx.sockets[-1]
    assert guest.attachment.account_key == ""

    clock["now"] += 15_000
    asyncio.run(instance.fetch(_signed_in_request("jett", "nonce-aaaa1111")))
    signed_in = instance.ctx.sockets[-1]

    clock["now"] += 15_000
    asyncio.run(instance.fetch(_FetchRequest()))
    clock["now"] += 15_000
    asyncio.run(instance.fetch(_signed_in_request("nova", "nonce-cccc3333")))

    # A second guest cannot evict the first, and one account cannot evict
    # another: only the same verified account folds onto one avatar.
    assert guest.closes == []
    assert signed_in.closes == []
    assert len(instance.ctx.sockets[-1].sent[0]["peers"]) == 3


def test_takeover_close_code_is_shared_by_the_relay_and_the_browser():
    assert "WORLD_ACCOUNT_TAKEOVER_CODE = %d" % WORLD_ACCOUNT_TAKEOVER_CODE \
        in ENTRY_TEXT
    world_js = WORLD_JS.read_text(encoding="utf-8")
    assert "const SOCKET_ACCOUNT_TAKEOVER_CODE = %d;" % (
        WORLD_ACCOUNT_TAKEOVER_CODE) in world_js
    # The displaced browser stands down instead of racing the newest device.
    assert "handlePresenceTakeover()" in world_js
    assert "reclaimPresenceHere()" in world_js
    assert (
        "if (this.destroyed || document.hidden || this.presenceTakenOver) "
        "return;") in world_js


def test_connects_land_in_open_grid_cells_never_on_a_standing_visitor():
    instance, clock = _world_fetch_runtime()
    welcomes = []
    for _ in range(3):
        clock["now"] += 15_000
        asyncio.run(instance.fetch(_FetchRequest()))
        welcomes.append(instance.ctx.sockets[-1].sent[0])
    # Sequential fresh joins fill the arrival row without overlap.
    assert [(w["self"]["x"], w["self"]["z"]) for w in welcomes] == [
        (-8.1, 30.0), (-6.3, 30.0), (-4.5, 30.0)]

    # A visitor whose reconnect reserved an unrelated slot while they kept
    # standing on cell 0 still blocks cell 0 for the next newcomer.
    instance.ctx.sockets[0].attachment.arrival_slot = 9
    clock["now"] += 15_000
    asyncio.run(instance.fetch(_FetchRequest()))
    newcomer = instance.ctx.sockets[-1].sent[0]
    assert (newcomer["self"]["x"], newcomer["self"]["z"]) == (-2.7, 30.0)


def test_world_presence_allows_the_consent_aware_office_activity():
    current = world.default_presence("peer", 1000)
    _, result = world.sanitize_message({
        "type": "presence",
        "activityCategory": "visiting-office",
    }, current, 2000)

    assert result["activityCategory"] == "visiting-office"


def test_public_door_state_is_explicit_and_allowlisted():
    current = world.default_presence("peer", 1000)
    assert current["publicDoor"] == "closed"
    for value in ("closed", "knock", "open"):
        _, updated = world.sanitize_message({
            "type": "presence",
            "publicDoor": value,
        }, current, 2000)
        assert updated["publicDoor"] == value
    _, rejected = world.sanitize_message({
        "type": "presence",
        "publicDoor": "bypass-permissions",
    }, current, 2000)
    assert rejected["publicDoor"] == "closed"


def test_knock_interactions_are_targeted_text_free_and_not_impersonable():
    sender = world.default_presence("peer_sender", 1000)
    assert world.sanitize_interaction({
        "type": "interaction",
        "kind": "knock",
        "target": "peer_target",
        "from": "impersonated",
        "text": "private message",
        "url": "https://example.test/private",
    }, sender) == {
        "type": "interaction",
        "kind": "knock",
        "target": "peer_target",
    }
    for invalid in (
        {"type": "interaction", "kind": "chat", "target": "peer_target"},
        {"type": "interaction", "kind": "knock", "target": "peer_sender"},
        {"type": "interaction", "kind": "knock", "target": "../private"},
        {"type": "interaction", "kind": "knock", "target": ""},
    ):
        assert world.sanitize_interaction(invalid, sender) is None


def test_home_grants_and_declines_are_targeted_text_free_consent_frames():
    sender = world.default_presence("peer_owner", 1000)
    for kind in ("home-grant", "home-decline"):
        assert world.sanitize_interaction({
            "type": "interaction",
            "kind": kind,
            "target": "peer_visitor",
            "room": "private-name",
            "text": "ignored",
        }, sender) == {
            "type": "interaction",
            "kind": kind,
            "target": "peer_visitor",
        }
    assert '"pending_knocks": pending_knocks' in WORLD_DURABLE_OBJECTS_TEXT
    assert 'interaction["target"] not in pending' in WORLD_DURABLE_OBJECTS_TEXT
    assert '"kind": interaction["kind"]' in WORLD_DURABLE_OBJECTS_TEXT


def test_handshake_offers_are_targeted_one_use_and_only_public_when_accepted():
    sender = world.default_presence("peer_greeter", 1000)
    for kind in ("handshake-offer", "handshake-accept", "handshake-decline"):
        assert world.sanitize_interaction({
            "type": "interaction",
            "kind": kind,
            "target": "peer_neighbor",
            "from": "impersonated",
            "text": "private message",
        }, sender) == {
            "type": "interaction",
            "kind": kind,
            "target": "peer_neighbor",
        }
    for invalid in (
        {"type": "interaction", "kind": "handshake", "target": "peer_a"},
        {
            "type": "interaction",
            "kind": "handshake-offer",
            "target": "peer_greeter",
        },
        {
            "type": "interaction",
            "kind": "handshake-offer",
            "target": "../private",
        },
        {"type": "interaction", "kind": "handshake-accept", "target": ""},
    ):
        assert world.sanitize_interaction(invalid, sender) is None

    # The offer reaches exactly one live peer and parks there as a one-use
    # consent record; only that record makes an answer possible.
    assert '"kind": "handshake-offer"' in WORLD_DURABLE_OBJECTS_TEXT
    assert '"pending_handshakes": pending_handshakes' in WORLD_DURABLE_OBJECTS_TEXT
    assert 'pending_handshakes=pending[-8:]' in WORLD_DURABLE_OBJECTS_TEXT
    assert (
        '_ws_attr(ws, "pending_handshakes", []) or []'
        in WORLD_DURABLE_OBJECTS_TEXT
    )
    # A declined handshake goes back to the offerer alone; an accepted one is
    # the only frame the rest of the room ever sees, and it carries just the
    # two peer ids.
    handshake_broadcast = WORLD_DURABLE_OBJECTS_TEXT[
        WORLD_DURABLE_OBJECTS_TEXT.index('"kind": "handshake",'):
    ][:400]
    assert '"with": interaction["target"]' in handshake_broadcast
    assert 'exclude_id=state.get("id"), budgeted=True' in handshake_broadcast
    assert "text" not in handshake_broadcast


def test_emotes_are_broadcast_as_a_tiny_fixed_vocabulary():
    sender = world.default_presence("peer_sender", 1000)
    for emote in (
        "wave",
        "idea",
        "celebrate",
        "jump",
        "spin",
        "backflip",
        "dance",
        "float",
        "wobble",
        "sparkle",
    ):
        assert world.sanitize_interaction({
            "type": "interaction",
            "kind": "emote",
            "emote": emote,
            "text": "must be discarded",
        }, sender) == {
            "type": "interaction",
            "kind": "emote",
            "emote": emote,
        }
    assert world.sanitize_interaction({
        "type": "interaction",
        "kind": "emote",
        "emote": "<script>",
    }, sender) is None


def test_name_and_identity_badge_fields_are_privacy_controlled_not_claims():
    current = world.default_presence("peer1234", 1000)
    assert current["name"] == "Guest peer"
    assert current["status"] == "hidden"

    _, named = world.sanitize_message({
        "type": "presence",
        "name": "Alice",
        "browser": "firefox",
        "os": "linux",
        "status": "available",
        # Guests cannot assert privileged identity or account claims.
        "accountStatus": "organization_admin",
        "verified": True,
        "paid": True,
        "email": "alice@example.test",
    }, current, 2000)
    # Anonymous display names stay server-generated so a guest cannot create a
    # moving lookalike of a signed-in account.
    assert named["name"] == "Guest peer"
    assert named["status"] == "available"
    assert named["accountStatus"] == "Guest"
    assert named["nodeCount"] == 0

    _, private = world.sanitize_message({
        "type": "presence",
        "shareName": False,
        "browser": "hidden",
        "os": "hidden",
        "status": "hidden",
    }, named, 3000)
    assert private["name"] == "Guest peer"
    assert private["browser"] == "hidden"
    assert private["os"] == "hidden"
    assert private["status"] == "hidden"
    assert "accountStatus" in world.WORLD_PUBLIC_FIELDS
    assert "nodeCount" in world.WORLD_PUBLIC_FIELDS
    for claim in ("verified", "paid", "email"):
        assert claim not in world.WORLD_PUBLIC_FIELDS
        assert claim not in world.public_presence(private)

    trusted = world.trusted_presence_claim(
        "Alice", "Organization admin", 9)
    assert trusted == {
        "name": "Alice",
        "accountStatus": "Organization admin",
        "nodeCount": world.WORLD_NODE_BADGE_MAX,
    }
    current["accountStatus"] = trusted["accountStatus"]
    _, authenticated = world.sanitize_message(
        {
            "type": "presence",
            "name": "Mallory",
            "shareName": True,
            "shareNodes": True,
            "accountStatus": "Verified bot",
            "nodeCount": 99,
            "space": "east",
        },
        current,
        4000,
        trusted_name=trusted["name"],
        trusted_node_count=trusted["nodeCount"],
    )
    assert authenticated["name"] == "Alice"
    assert authenticated["accountStatus"] == "Organization admin"
    assert authenticated["nodeCount"] == world.WORLD_NODE_BADGE_MAX
    assert authenticated["space"] == "east"


def test_only_presence_movement_and_heartbeat_frames_are_accepted():
    current = world.default_presence("peer", 1000)
    assert world.sanitize_message({"type": "chat", "text": "secret"},
                                  current, 2000) is None
    assert world.sanitize_message({"type": "repository", "path": "private"},
                                  current, 2000) is None
    assert world.sanitize_message("not-an-object", current, 2000) is None
    kind, unchanged = world.sanitize_message({"type": "ping"}, current, 2000)
    assert kind == "ping"
    assert unchanged == current


def test_rate_window_and_stale_cleanup_are_bounded():
    assert (
        world.WORLD_RATE_MAX_PER_WINDOW
        < world.WORLD_RATE_HARD_MAX_PER_WINDOW
        <= 16
    )
    start = 10_000
    count = 0
    for _ in range(world.WORLD_RATE_MAX_PER_WINDOW):
        allowed, start, count = world.advance_rate_window(start, count, 10_500)
        assert allowed is True
    allowed, start, count = world.advance_rate_window(start, count, 10_500)
    assert allowed is False

    allowed, new_start, new_count = world.advance_rate_window(
        start, count, start + world.WORLD_RATE_WINDOW_MS)
    assert (allowed, new_start, new_count) == (
        True, start + world.WORLD_RATE_WINDOW_MS, 1)

    broadcast_start = 20_000
    broadcast_count = 0
    for _ in range(world.WORLD_BROADCAST_MAX_PER_WINDOW):
        allowed, broadcast_start, broadcast_count = (
            world.advance_broadcast_window(
                broadcast_start, broadcast_count, 20_500))
        assert allowed is True
    assert world.advance_broadcast_window(
        broadcast_start, broadcast_count, 20_500)[0] is False

    connect_start = 30_000
    connect_count = 0
    for _ in range(world.WORLD_CONNECT_MAX_PER_WINDOW):
        allowed, connect_start, connect_count = (
            world.advance_connection_window(
                connect_start, connect_count, 30_500))
        assert allowed is True
    assert world.advance_connection_window(
        connect_start, connect_count, 30_500)[0] is False
    assert world.WORLD_MAX_CONNECTIONS <= 64

    now = 1_000_000
    assert world.presence_is_stale(
        now - world.WORLD_CLIENT_STALE_MS, now) is False
    assert world.presence_is_stale(
        now - world.WORLD_CLIENT_STALE_MS - 1, now) is True
    assert world.presence_is_stale(0, now) is True


class _WorldSocket:
    def __init__(self, state, now, rate_count=0):
        self.attachment = SimpleNamespace(
            **world.public_presence(state),
            country_source="",
            trusted_name="",
            trusted_node_count=0,
            is_admin=False,
            ip_token="",
            agent_token="",
            pending_knocks=[],
            arrival_slot=0,
            last=now,
            rl_start=now,
            rl_count=rate_count,
            departed=False,
        )
        self.sent = []
        self.closed = []

    def serializeAttachment(self, attachment):
        self.attachment = SimpleNamespace(**attachment)

    def send(self, message):
        self.sent.append(message)

    def close(self, code, reason):
        self.closed.append((code, reason))


def _world_socket_runtime(now=50_000):
    class DurableObject:
        pass

    def ws_attachment(socket):
        return socket.attachment

    def ws_attr(socket, key, default=None):
        return getattr(socket.attachment, key, default)

    traffic_writes = []
    step_writes = []

    async def d1_run(_env, sql, *args):
        traffic_writes.append((sql, args))
        return None

    async def d1_first(_env, sql, *args):
        step_writes.append((sql, args))
        return {"total_steps": int(args[1]) + 1_000}

    namespace = {
        "DurableObject": DurableObject,
        "Date": SimpleNamespace(now=lambda: now),
        "world_protocol": world,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "_ws_attachment": ws_attachment,
        "_ws_attr": ws_attr,
        "to_js": lambda value: value,
        "json": json,
        "re": re,
        "DURABLE_OBJECT_BINDING_RE": re.compile(r"[A-Z][A-Z0-9_]{0,63}"),
        "DURABLE_OBJECT_TRAFFIC_FLUSH_MS": 60_000,
        "DURABLE_OBJECT_TRAFFIC_FLUSH_BYTES": 262_144,
        "DURABLE_OBJECT_TRAFFIC_MAX": 9_007_199_254_740_991,
        "d1_run": d1_run,
        "d1_first": d1_first,
        "_read_only": lambda _env: False,
        "_maintenance_socket_closed": lambda _env, _ws: False,
    }
    for name in (
        "durable_object_traffic_note",
        "durable_object_traffic_flush",
        "ForkMeshWorld",
    ):
        node = _top_level_node(name)
        module = ast.fix_missing_locations(
            ast.Module(body=[node], type_ignores=[]))
        exec(compile(module, str(ENTRY), "exec"), namespace)
    instance = namespace["ForkMeshWorld"]()
    instance.env = object()
    instance.traffic_writes = traffic_writes
    instance.step_writes = step_writes
    broadcasts = []
    instance._live_sockets = lambda cleanup=False: []
    instance._broadcast = (
        lambda frame, **_kwargs: broadcasts.append(frame)
    )
    return instance, broadcasts


def _world_socket_message(instance, socket, payload):
    asyncio.run(instance.webSocketMessage(
        socket, json.dumps(payload, separators=(",", ":"))))


def test_step_flush_persists_only_a_blind_aggregate_and_updates_owner():
    now = 50_000
    instance, _broadcasts = _world_socket_runtime(now)
    socket = _WorldSocket(world.default_presence("peer", now), now)
    instance._save_attachment(
        socket,
        instance._socket_state(socket),
        last=now,
        rate_start=now,
        rate_count=0,
        account_bi="a" * 64,
        pending_steps=world.WORLD_STEP_FLUSH_COUNT,
        step_flushed_at=now - world.WORLD_STEP_FLUSH_MS,
    )

    asyncio.run(instance._flush_steps(socket, now))

    assert len(instance.step_writes) == 1
    sql, args = instance.step_writes[0]
    assert "world_user_activity" in sql
    assert "total_steps" in sql
    assert "(account_bi,total_steps,updated_at)" in sql
    assert args[:2] == ("a" * 64, world.WORLD_STEP_FLUSH_COUNT)
    assert socket.attachment.pending_steps == 0
    assert json.loads(socket.sent[-1]) == {
        "type": "step-total",
        "totalSteps": 1_000 + world.WORLD_STEP_FLUSH_COUNT,
    }


def test_legitimate_join_presence_and_movement_burst_is_not_disconnected():
    now = 50_000
    instance, broadcasts = _world_socket_runtime(now)
    socket = _WorldSocket(world.default_presence("peer", now), now)
    frames = [
        {"type": "presence", "status": "available"},
        {"type": "move", "x": 1, "z": 1, "moving": False},
        {"type": "presence", "inputActive": True},
        {"type": "move", "x": 2, "z": 2, "moving": True},
        {"type": "move", "x": 3, "z": 3, "moving": True},
    ]
    for frame in frames:
        _world_socket_message(instance, socket, frame)

    assert socket.closed == []
    assert socket.attachment.rl_count == 5
    assert socket.attachment.x == 2
    assert socket.attachment.z == 2
    assert len(broadcasts) == world.WORLD_RATE_MAX_PER_WINDOW


def test_isolated_excess_movement_and_presence_are_dropped_without_disconnect():
    now = 50_000
    cases = (
        (
            {"type": "move", "x": 40, "z": 50, "moving": True},
            {"x": 4.0, "z": 5.0},
        ),
        (
            {"type": "presence", "status": "away"},
            {"status": "hidden"},
        ),
    )
    for payload, expected in cases:
        state = world.default_presence("peer", now)
        state.update({"x": 4.0, "z": 5.0})
        instance, broadcasts = _world_socket_runtime(now)
        socket = _WorldSocket(
            state, now, rate_count=world.WORLD_RATE_MAX_PER_WINDOW)

        _world_socket_message(instance, socket, payload)

        assert socket.closed == []
        assert socket.attachment.rl_count == (
            world.WORLD_RATE_MAX_PER_WINDOW + 1)
        for field, value in expected.items():
            assert getattr(socket.attachment, field) == value
        assert broadcasts == []


def test_sustained_disposable_frame_flood_still_closes_socket():
    now = 50_000
    instance, broadcasts = _world_socket_runtime(now)
    socket = _WorldSocket(
        world.default_presence("peer", now),
        now,
        rate_count=world.WORLD_RATE_MAX_PER_WINDOW,
    )
    excess_frames = (
        world.WORLD_RATE_HARD_MAX_PER_WINDOW
        - world.WORLD_RATE_MAX_PER_WINDOW
        + 1
    )
    for index in range(excess_frames):
        _world_socket_message(instance, socket, {
            "type": "move", "x": index, "moving": True,
        })
        if socket.closed:
            break

    assert socket.attachment.rl_count == (
        world.WORLD_RATE_HARD_MAX_PER_WINDOW + 1)
    assert socket.closed == [(1008, "sustained rate limit")]
    assert broadcasts == []


def test_world_durable_object_is_transient_and_hibernating():
    source = ast.unparse(_top_level_node("ForkMeshWorld"))
    assert "acceptWebSocket" in source
    assert "getWebSockets" in source
    assert "serializeAttachment" in source
    assert "WORLD_MESSAGE_MAX_BYTES" in source
    assert "advance_rate_window" in source
    assert "advance_connection_window" in source
    assert "advance_broadcast_window" in source
    assert "presence_is_stale" in source
    assert "sanitize_interaction" in source
    assert "_ws_attr(peer, 'publicDoor', 'closed') == 'knock'" in source
    assert "'from': state.get('id')" in source
    assert "webSocketError" in source
    assert "_live_sockets(cleanup=True)" in source
    # The one reachable write is a blind-indexed whole-step increment. No
    # coordinate, route, chat, repository, or account-record history is stored.
    assert "INSERT INTO world_user_activity" in source
    assert "total_steps" in source
    for forbidden in (
        ".storage", "ensure_schema", "chat_history",
        "account_row", "/api/repo/", "wallet",
    ):
        assert forbidden not in source


def test_world_internal_upgrade_does_not_forward_identifying_headers():
    source = _function_without_docstring("world_durable_object_request").lower()
    assert "world_request_country" in source
    assert "x-forkmesh-country" in source
    assert "x-forkmesh-world-private-agent" in source
    assert 'headers[\'user-agent\']' not in source
    assert "source_url.path" in source
    assert "sec-websocket-protocol" not in source
    for forbidden in (
        "cf-connecting-ip", "x-forwarded-for",
        "authorization", "cookie", "referer",
    ):
        assert forbidden not in source


def test_browser_world_socket_is_same_origin_only():
    node = _top_level_node("world_websocket_origin_allowed")
    module = ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[]))
    namespace = {"urlparse": urlparse}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    allowed = namespace["world_websocket_origin_allowed"]

    same = SimpleNamespace(
        url="https://forkmesh.test/api/world/ws",
        headers=_Headers(
            {"origin": "https://forkmesh.test"}, allowed={"origin"}))
    foreign = SimpleNamespace(
        url="https://forkmesh.test/api/world/ws",
        headers=_Headers(
            {"origin": "https://evil.example"}, allowed={"origin"}))
    missing = SimpleNamespace(
        url="https://forkmesh.test/api/world/ws",
        headers=_Headers({}, allowed={"origin"}))
    wrong_scheme = SimpleNamespace(
        url="https://forkmesh.test/api/world/ws",
        headers=_Headers(
            {"origin": "http://forkmesh.test"}, allowed={"origin"}))
    default_port = SimpleNamespace(
        url="https://forkmesh.test/api/world/ws",
        headers=_Headers(
            {"origin": "https://forkmesh.test:443"}, allowed={"origin"}))
    wrong_port = SimpleNamespace(
        url="https://forkmesh.test/api/world/ws",
        headers=_Headers(
            {"origin": "https://forkmesh.test:444"}, allowed={"origin"}))
    origin_with_path = SimpleNamespace(
        url="https://forkmesh.test/api/world/ws",
        headers=_Headers(
            {"origin": "https://forkmesh.test/private"},
            allowed={"origin"}))
    assert allowed(same) is True
    assert allowed(foreign) is False
    assert allowed(missing) is False
    assert allowed(wrong_scheme) is False
    assert allowed(default_port) is True
    assert allowed(wrong_port) is False
    assert allowed(origin_with_path) is False


def test_world_route_binding_and_migration_are_registered():
    route_source = _route_table()
    assert "/api/world/context" in route_source
    assert "/api/world/ws" in route_source
    assert "town-square-v1" in route_source
    assert "world_websocket_origin_allowed" in route_source
    assert "world_durable_object_request" in route_source

    config = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))
    bindings = {
        item["name"]: item["class_name"]
        for item in config["durable_objects"]["bindings"]
    }
    # Multiplayer chat, world presence, the per-owner node event channel, and
    # the alarm-backed cron watchdog remain; the repository transport DO
    # (ForkMeshHost, which carried git bytes) is deleted and must never return
    # as a production binding.
    assert bindings == {
        "FORKMESH_MAINNODE_ROOM": "ForkMeshRoom",
        "FORKMESH_WORLD": "ForkMeshWorld",
        "FORKMESH_OFFICE_ROOM": "ForkMeshOfficeRoom",
        "FORKMESH_NODES": "ForkMeshNodes",
        "FORKMESH_NOTE_ROOM": "ForkMeshNoteRoom",
        "FORKMESH_CRON_WATCHDOG": "ForkMeshCronWatchdog",
        "FORKMESH_CRON_RUNNER": "ForkMeshCronRunner",
        "FORKMESH_DISCORD_GATE": "ForkMeshDiscordGate",
        "FORKMESH_ORG_OFFICE": "ForkMeshOrgOffice",
    }
    dev_bindings = {
        item["name"]: item["class_name"]
        for item in config["env"]["dev"]["durable_objects"]["bindings"]
    }
    assert dev_bindings == {
        "FORKMESH_MAINNODE_ROOM": "ForkMeshRoom",
        "FORKMESH_WORLD": "ForkMeshWorld",
        "FORKMESH_OFFICE_ROOM": "ForkMeshOfficeRoom",
        "FORKMESH_NODES": "ForkMeshNodes",
        "FORKMESH_NOTE_ROOM": "ForkMeshNoteRoom",
        "FORKMESH_CRON_WATCHDOG": "ForkMeshCronWatchdog",
        "FORKMESH_CRON_RUNNER": "ForkMeshCronRunner",
        "FORKMESH_DISCORD_GATE": "ForkMeshDiscordGate",
        "FORKMESH_ORG_OFFICE": "ForkMeshOrgOffice",
    }
    migrations = {item["tag"]: item for item in config["migrations"]}
    assert migrations["v9"]["new_sqlite_classes"] == ["ForkMeshWorld"]
    assert migrations["v10"]["deleted_classes"] == ["ForkMeshHost"]
    assert migrations["v11"]["new_sqlite_classes"] == [
        "ForkMeshOfficeRoom"]
    assert migrations["v12"]["new_sqlite_classes"] == ["ForkMeshNodes"]
    assert migrations["v13"]["new_sqlite_classes"] == [
        "ForkMeshCronWatchdog"]


def test_world_static_route_is_reserved_and_asset_first():
    worker = WORLD_WORKER.read_text(encoding="utf-8")
    assert 'return asset(request, env, "/world/index.html")' in worker
    world_config = tomllib.loads(WORLD_WRANGLER.read_text(encoding="utf-8"))
    world_routes = world_config["assets"]["run_worker_first"]
    for route in ("/", "/world", "/world/", "/api", "/api/*"):
        assert route in world_routes
    for asset in (
        "/world/world.js",
        "/world/world.css",
        "/world/world-scene.js",
        "/world/assets/beach-horizon-v1.webp",
    ):
        assert asset not in world_routes

    static_source = STATIC_ROUTES.read_text(encoding="utf-8")
    assert '"world",' in static_source
    config = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))
    assert "/world" in config["assets"]["run_worker_first"]
    assert "/world/*" in config["assets"]["run_worker_first"]


def test_world_peer_id_contains_no_timestamp_or_account_material():
    source = ast.unparse(_top_level_node("new_world_peer_id"))
    assert "getRandomValues" in source
    assert "Date.now" not in source
    assert "account" not in source.lower()
    assert "node" not in source.lower()


def test_consent_only_inactivity_records_are_bounded_and_generalized():
    assert world.sanitize_inactivity_record({
        "status": "inactive",
        "shareInactivity": True,
        "shareName": True,
        "shareNodes": False,
        "url": "https://private.example/path",
        "search": "secret",
    }) == {
        "status": "inactive",
        "shareName": True,
        "shareNodes": False,
    }
    assert world.sanitize_inactivity_record({
        "status": "inactive",
        "shareInactivity": False,
    }) is None
    assert world.sanitize_inactivity_record({
        "status": "precise browsing history",
        "shareInactivity": True,
    }) is None


def test_world_ticket_and_inactive_routes_keep_auth_out_of_the_socket():
    route_source = _route_table()
    assert "/api/world/ticket" in route_source
    assert "/api/world/inactive" in route_source
    assert "_world_ticket_decode" in route_source

    bridge = _function_without_docstring("world_durable_object_request").lower()
    assert "x-forkmesh-world-claim" in bridge
    assert "x-forkmesh-world-private-agent" in bridge
    assert 'headers[\'user-agent\']' not in bridge
    assert "trusted_presence_claim" in bridge
    assert "source_url.path" in bridge
    assert "source_url.query" not in bridge
    for forbidden in (
        "authorization", "cookie", "sessiontoken", "cf-connecting-ip",
        "x-forwarded-for",
    ):
        assert forbidden not in bridge

    handler = ast.unparse(_top_level_node("world_inactive_handler"))
    assert "sanitize_inactivity_record" in handler
    assert "WORLD_INACTIVE_RETAIN_MS" in handler
    assert "WORLD_INACTIVE_DELAY_MS" in handler
    assert "Private contributor" in handler
    assert "updated_at<=?" in handler
    assert "visibleAfter" in handler
    assert "world_request_country" not in handler
    assert "request.url" not in handler

    ticket = ast.unparse(_top_level_node("world_ticket_handler"))
    assert "UPDATE world_inactive_presence SET updated_at" in ticket

    claims = ast.unparse(_top_level_node("_world_account_claim"))
    assert "revoked_at=0" in claims
    assert "expires_at=0 OR expires_at>?" in claims

    schema = SCHEMA.read_text(encoding="utf-8")
    migration = INACTIVE_MIGRATION.read_text(encoding="utf-8")
    for source in (schema, migration):
        assert "world_inactive_presence" in source
        assert "expires_at" in source


def test_world_system_capacity_is_bounded_content_free_and_identifier_safe():
    calls = {"list": [], "count": []}

    async def d1_all(_env, sql, *args):
        calls["list"].append((sql, args))
        return [
            {"name": "accounts"},
            {"name": "one_row"},
            {"name": "empty_table"},
            {"name": "sqlite_sequence"},
            {"name": "_cf_KV"},
            {"name": 'bad" UNION SELECT secret FROM accounts--'},
            {"name": "space padded "},
            {"name": "too_large"},
            {"name": "dropped_during_migration"},
        ]

    async def d1_first(_env, sql, *args):
        assert args == ()
        calls["count"].append(sql)
        table = sql.split('"')[1]
        if table == "dropped_during_migration":
            raise RuntimeError("no such table")
        return {
            "row_count": {
                "accounts": 37,
                "one_row": 1,
                "empty_table": 0,
                "too_large": 9_007_199_254_740_992,
            }[table]
        }

    namespace = {
        "d1_all": d1_all,
        "d1_first": d1_first,
        "_world_infrastructure_module": lambda: world_infrastructure,
    }
    node = _top_level_node("_world_system_capacity")
    module = ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    result = asyncio.run(namespace["_world_system_capacity"](object()))

    # Empty and single-row tables are part of the inventory; only an
    # unrepresentable count, an unsafe name, or a vanished table is skipped.
    assert result == [
        {"name": "accounts", "rowCount": 37},
        {"name": "one_row", "rowCount": 1},
        {"name": "empty_table", "rowCount": 0},
    ]
    assert set(result[0]) == {"name", "rowCount"}
    assert len(calls["list"]) == 1
    list_sql, list_args = calls["list"][0]
    assert "sqlite_master" in list_sql
    assert "ORDER BY name LIMIT ?" in list_sql
    assert list_args == (256,)
    assert calls["count"] == [
        'SELECT COUNT(*) AS row_count FROM "accounts"',
        'SELECT COUNT(*) AS row_count FROM "one_row"',
        'SELECT COUNT(*) AS row_count FROM "empty_table"',
        'SELECT COUNT(*) AS row_count FROM "too_large"',
        'SELECT COUNT(*) AS row_count FROM "dropped_during_migration"',
    ]


def test_world_ticket_capacity_is_queried_and_returned_only_for_admins():
    async def exercise(claim):
        capacity_calls = []

        async def account_claim(_env, _request):
            return claim

        async def ensure_schema(_env):
            return None

        async def blind_index(_env, value):
            return "bi:" + value

        async def d1_run(_env, _sql, *_args):
            return None

        async def system_capacity(_env):
            capacity_calls.append(True)
            return [{"name": "repositories", "rowCount": 12}]

        async def durable_objects(_env):
            capacity_calls.append("durable")
            return [{
                "id": "FORKMESH_WORLD",
                "binding": "FORKMESH_WORLD",
                "name": "World",
                "bytesIn": 30,
                "bytesOut": 70,
                "bytesTotal": 100,
                "messages": 4,
                "updatedAt": 50_000,
            }]

        async def d1_storage(_env):
            capacity_calls.append("storage")
            return {
                "bytes": 14_811_136,
                "freeDatabaseLimitBytes": 500_000_000,
                "paidDatabaseLimitBytes": 10_000_000_000,
                "includedAccountStorageBytes": 5_000_000_000,
            }

        def json_response(data, **kwargs):
            return {"data": data, **kwargs}

        namespace = {
            "method_name": lambda request: request.method,
            "json_response": json_response,
            "_world_account_claim": account_claim,
            "Date": SimpleNamespace(now=lambda: 50_000),
            "ensure_schema": ensure_schema,
            "blind_index": blind_index,
            "d1_run": d1_run,
            "WORLD_INACTIVE_RETAIN_MS": 100_000,
            "WORLD_TICKET_TTL_MS": 60_000,
            "_world_ticket_encode": lambda _env, _claim: "signed-ticket",
            "_world_system_capacity": system_capacity,
            "_world_durable_objects": durable_objects,
            "_world_d1_storage": d1_storage,
            "new_world_peer_id": lambda: "a" * 32,
        }
        node = _top_level_node("world_ticket_handler")
        module = ast.fix_missing_locations(
            ast.Module(body=[node], type_ignores=[]))
        exec(compile(module, str(ENTRY), "exec"), namespace)
        response = await namespace["world_ticket_handler"](
            object(), SimpleNamespace(method="GET"))
        return response["data"], capacity_calls

    guest_data, guest_calls = asyncio.run(exercise(None))
    assert "systemCapacity" not in guest_data
    assert guest_calls == []

    user_data, user_calls = asyncio.run(exercise({
        "name": "alice",
        "accountStatus": "Registered",
        "nodeCount": 0,
        "isAdmin": False,
    }))
    assert "systemCapacity" not in user_data
    assert user_calls == []

    admin_data, admin_calls = asyncio.run(exercise({
        "name": "root",
        "accountStatus": "Registered",
        "nodeCount": 0,
        "isAdmin": True,
    }))
    assert admin_data["systemCapacity"] == {
        "tables": [{"name": "repositories", "rowCount": 12}],
        "durableObjects": [{
            "id": "FORKMESH_WORLD",
            "binding": "FORKMESH_WORLD",
            "name": "World",
            "bytesIn": 30,
            "bytesOut": 70,
            "bytesTotal": 100,
            "messages": 4,
            "updatedAt": 50_000,
        }],
        "d1Storage": {
            "bytes": 14_811_136,
            "freeDatabaseLimitBytes": 500_000_000,
            "paidDatabaseLimitBytes": 10_000_000_000,
            "includedAccountStorageBytes": 5_000_000_000,
        },
    }
    assert admin_calls == [True, "durable", "storage"]


def test_world_d1_storage_reports_provider_size_and_documented_limits():
    calls = []

    class Statement:
        async def run(self):
            calls.append("run")
            return SimpleNamespace(
                meta=SimpleNamespace(size_after=14_811_136)
            )

    class Database:
        def prepare(self, sql):
            calls.append(sql)
            return Statement()

    result = asyncio.run(
        world_infrastructure.d1_storage_usage(
            SimpleNamespace(DB=Database())
        )
    )
    assert result["bytes"] == 14_811_136
    assert result["freeDatabaseLimitBytes"] == 500_000_000
    assert result["paidDatabaseLimitBytes"] == 10_000_000_000
    assert result["includedAccountStorageBytes"] == 5_000_000_000
    assert calls == ["SELECT 1 AS capacity_probe", "run"]


def test_arrival_counter_keeps_only_fixed_size_unique_sketches():
    # Arrival identity is derived in the outer Worker, never persisted in the
    # storage-free ForkMeshWorld object. D1 receives only fixed-size HLL
    # register/rank updates for an all-time and a short-lived temporal sketch.
    route_source = _route_table()
    assert "world_visitors_handler" in route_source
    assert "/api/world/visitors" in route_source

    counter = _function_without_docstring("record_world_visit")
    assert "world_visit_unique_hll" in counter
    assert "WORLD_VISIT_BUCKET_MS" in counter
    assert "hll_register" in counter
    assert "MAX(world_visit_unique_hll.rank,excluded.rank)" in counter
    lowered = counter.lower()
    for forbidden in (
        "country", "account", "cookie", "peer", "user-agent",
    ):
        assert forbidden not in lowered

    identity = ast.unparse(_top_level_node("_world_unique_visitor_tokens"))
    assert "headers.get('cf-connecting-ip')" in identity
    assert "headers.get('user-agent')" in identity
    assert "canonical_edge_address" in identity
    assert "generalized_user_agent" in identity
    assert "blind_index" in identity
    assert "x-forwarded-for" not in identity.lower()
    assert "bounded_json_request" not in identity

    handler = ast.unparse(_top_level_node("world_visitors_handler"))
    assert "edge_cache_match" in handler
    assert "edge_cache_put" in handler
    assert "approximate_unique_visitors" in handler
    assert "unique_visit_summary" in handler

    schema = SCHEMA.read_text(encoding="utf-8")
    migration = (
        ROOT / "migrations" / "0077_world_unique_visitors.sql"
    ).read_text(
        encoding="utf-8")
    for source in (schema, migration):
        assert "world_visit_unique_hll" in source
        assert "bucket_start" in source
        table = source.split("world_visit_unique_hll", 1)[1].split(")")[0]
        assert "TEXT" not in table
        for forbidden in ("ip_address", "user_agent", "visitor_token"):
            assert forbidden not in table.lower()


def test_arrival_identity_reads_only_edge_headers_and_skips_missing_ip():
    node = _top_level_node("_world_unique_visitor_tokens")
    module = ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[]))
    calls = []

    async def blind_index(_env, value):
        calls.append(value)
        return "a" * 64

    metrics = SimpleNamespace(
        canonical_edge_address=lambda value: (
            "203.0.113.9" if value == "203.0.113.9" else ""),
        generalized_user_agent=lambda _value: "chromium:linux:desktop",
    )
    namespace = {
        "world_visitor_metrics": metrics,
        "blind_index": blind_index,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    derive = namespace["_world_unique_visitor_tokens"]
    headers = _Headers(
        {
            "cf-connecting-ip": "203.0.113.9",
            "user-agent": "Mozilla/5.0 private build 123",
        },
        allowed={"cf-connecting-ip", "user-agent"},
    )
    tokens = asyncio.run(derive(
        None, SimpleNamespace(headers=headers), 3 * 24 * 60 * 60 * 1000))
    assert tokens == {"allTime": "a" * 64, "daily": "a" * 64}
    assert headers.read == ["cf-connecting-ip", "user-agent"]
    assert calls == [
        (
            "world-unique-visitor-all-time-v1\n"
            "203.0.113.9\nchromium:linux:desktop"
        ),
        (
            "world-unique-visitor-day-v1:3\n"
            "203.0.113.9\nchromium:linux:desktop"
        ),
    ]
    assert all("private build" not in value for value in calls)

    calls.clear()
    missing = _Headers(
        {"user-agent": "Mozilla/5.0"},
        allowed={"cf-connecting-ip", "user-agent"},
    )
    assert asyncio.run(
        derive(None, SimpleNamespace(headers=missing), 0)) == ""
    assert calls == []


def test_arrival_record_upserts_all_time_and_utc_bucket_idempotently():
    node = _top_level_node("record_world_visit")
    module = ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[]))
    writes = []

    async def unique_tokens(_env, _request, _now):
        return {"allTime": "a" * 64, "daily": "b" * 64}

    async def ensure_schema(_env):
        return None

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    metrics = SimpleNamespace(
        HLL_ALL_TIME_BUCKET=-1,
        hll_register=lambda token: (
            (17, 4) if token == "a" * 64 else (29, 2)),
    )
    namespace = {
        "_world_unique_visitor_tokens": unique_tokens,
        "world_visitor_metrics": metrics,
        "ensure_schema": ensure_schema,
        "d1_run": d1_run,
        "WORLD_VISIT_BUCKET_MS": 10 * 60 * 1000,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    counted = asyncio.run(namespace["record_world_visit"](
        None, SimpleNamespace(), now=1_234_567))
    assert counted is True
    assert [args for _sql, args in writes] == [
        (-1, 17, 4),
        (1_200_000, 29, 2),
    ]
    assert all(
        "ON CONFLICT(bucket_start,register_id) DO UPDATE" in sql
        for sql, _args in writes
    )
