#!/usr/bin/env python3
"""Server-authoritative World activity and leaderboard contracts."""

import ast
import asyncio
import base64
import hashlib
import hmac
import json
import sqlite3
from pathlib import Path
from types import SimpleNamespace
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY_PATH = ROOT / "src" / "entry.py"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = ENTRY_PATH.read_text(encoding="utf-8") + WORLD_ROUTE_SOURCE
ACCOUNT_USERS_PATH = ROOT / "src" / "account_users.py"
ACCOUNT_USERS = ACCOUNT_USERS_PATH.read_text(encoding="utf-8")
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
MIGRATION = ROOT / "migrations" / "0076_world_user_activity.sql"
STEP_MIGRATION = ROOT / "migrations" / "0137_world_user_steps.sql"
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _top_level_node(name):
    for node in ast.parse(ENTRY, filename=str(ENTRY_PATH)).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name == name:
                return node
    raise AssertionError(f"{name} not found")


def _account_users_node(name):
    for node in ast.parse(ACCOUNT_USERS, filename=str(ACCOUNT_USERS_PATH)).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name == name:
                return node
    raise AssertionError(f"{name} not found")


def _load_function(name, namespace):
    module = ast.fix_missing_locations(
        ast.Module(body=[_top_level_node(name)], type_ignores=[]))
    exec(compile(module, str(ENTRY_PATH), "exec"), namespace)
    return namespace[name]


def test_activity_migrations_are_blind_indexed_and_match_lazy_schema():
    sql = MIGRATION.read_text(encoding="utf-8")
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    db.executescript(sql)
    db.executescript(STEP_MIGRATION.read_text(encoding="utf-8"))
    columns = {
        row[1] for row in db.execute(
            "PRAGMA table_info(world_user_activity)").fetchall()
    }
    assert columns == {
        "account_bi",
        "total_active_ms",
        "total_steps",
        "last_touch_at",
        "generation",
        "last_credit_ms",
        "updated_at",
    }
    assert "CREATE TABLE IF NOT EXISTS world_user_activity" in SCHEMA
    assert "display_name" not in columns
    assert "ip_address" not in columns
    assert "duration" not in columns


def test_activity_touch_counts_one_signed_generation_only_once():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(MIGRATION.read_text(encoding="utf-8"))
    db.execute(
        "ALTER TABLE world_user_activity ADD COLUMN total_steps "
        "INTEGER NOT NULL DEFAULT 0 CHECK (total_steps >= 0)"
    )

    async def d1_first(_env, sql, *args):
        return dict(db.execute(sql, args).fetchone())

    touch = _load_function("_world_user_activity_touch", {
        "d1_first": d1_first,
        "WORLD_USER_ACTIVITY_CONTINUATION_MAX_MS": 7 * 60 * 1000,
        "WORLD_USER_ACTIVITY_MAX_TOTAL_MS": 9_000_000_000_000_000,
        "WORLD_USER_ACTIVITY_MAX_GENERATION": 9_000_000_000_000_000,
        "WORLD_USER_STEP_MAX_TOTAL": 9_000_000_000_000,
    })

    first = asyncio.run(touch(object(), "blind-account", 1_000))
    assert first == {
        "totalActiveMs": 0,
        "totalSteps": 0,
        "generation": 1,
        "creditedMs": 0,
        "observedAt": 1_000,
    }

    continued = asyncio.run(touch(
        object(),
        "blind-account",
        301_000,
        {"issuedAt": 1_000, "generation": first["generation"]},
    ))
    assert continued["totalActiveMs"] == 300_000
    assert continued["creditedMs"] == 300_000
    assert continued["generation"] == 2

    replay = asyncio.run(touch(
        object(),
        "blind-account",
        301_010,
        {"issuedAt": 1_000, "generation": first["generation"]},
    ))
    assert replay["totalActiveMs"] == 300_000
    assert replay["creditedMs"] == 0
    assert replay["generation"] == 3

    next_interval = asyncio.run(touch(
        object(),
        "blind-account",
        601_010,
        {"issuedAt": 301_010, "generation": replay["generation"]},
    ))
    assert next_interval["totalActiveMs"] == 600_000
    assert next_interval["creditedMs"] == 300_000

    too_old = asyncio.run(touch(
        object(),
        "blind-account",
        1_101_011,
        {
            "issuedAt": 601_010,
            "generation": next_interval["generation"],
        },
    ))
    assert too_old["totalActiveMs"] == 600_000
    assert too_old["creditedMs"] == 0


def test_activity_continuation_accepts_only_matching_recent_signed_ticket():
    secret = b"test-secret"

    def ticket_signature(_env, payload):
        return hmac.new(secret, payload.encode(), hashlib.sha256).hexdigest()

    validate = _load_function("_world_activity_continuation_claim", {
        "base64": base64,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "hmac": hmac,
        "json": json,
        "MAX_NODE_NAME": 80,
        "re": __import__("re"),
        "WORLD_USER_ACTIVITY_CONTINUATION_MAX_MS": 7 * 60 * 1000,
        "WORLD_USER_ACTIVITY_HEADER": "x-forkmesh-world-activity",
        "WORLD_USER_ACTIVITY_MAX_GENERATION": 9_000_000_000_000_000,
        "_world_ticket_signature": ticket_signature,
    })

    claim = {
        "name": "jett",
        "issuedAt": 1_000,
        "activityGeneration": 7,
    }
    payload = base64.urlsafe_b64encode(
        json.dumps(claim, separators=(",", ":")).encode()
    ).decode().rstrip("=")
    ticket = payload + "." + ticket_signature(None, payload)
    request = SimpleNamespace(headers={
        "x-forkmesh-world-activity": ticket,
    })

    assert validate(None, request, "jett", 301_000) == {
        "issuedAt": 1_000,
        "generation": 7,
    }
    assert validate(None, request, "alice", 301_000) is None
    assert validate(None, request, "jett", 500_000) is None
    request.headers["x-forkmesh-world-activity"] = ticket + "tampered"
    assert validate(None, request, "jett", 301_000) is None


def test_world_ticket_returns_exact_aggregate_and_invalidates_after_credit():
    encoded_claims = []
    cache_deletes = []

    async def account_claim(_env, _request):
        return {
            "name": "jett",
            "accountStatus": "Registered",
            "nodeCount": 0,
            "isAdmin": False,
        }

    async def activity_touch(_env, account_bi, now, continuation):
        assert account_bi == "blind:jett"
        assert now == 600_000
        assert continuation == {"issuedAt": 300_000, "generation": 4}
        return {
            "totalActiveMs": 900_000,
            "totalSteps": 12_345,
            "generation": 5,
            "creditedMs": 300_000,
            "observedAt": now,
        }

    def encode(_env, claim):
        encoded_claims.append(claim)
        return "signed-next-ticket"

    async def cache_delete(key):
        cache_deletes.append(key)

    handler = _load_function("world_ticket_handler", {
        "Date": SimpleNamespace(now=lambda: 600_000),
        "USERS_DIRECTORY_CACHE_KEY": "internal:users",
        "WORLD_INACTIVE_RETAIN_MS": 123_000,
        "WORLD_TICKET_TTL_MS": 60_000,
        "_world_account_claim": account_claim,
        "_world_activity_continuation_claim": (
            lambda _env, _request, _name, _now: {
                "issuedAt": 300_000,
                "generation": 4,
            }
        ),
        "_world_system_capacity": lambda _env: None,
        "_world_ticket_encode": encode,
        "_world_user_activity_touch": activity_touch,
        "blind_index": (
            lambda _env, name: _async_value("blind:" + name)
        ),
        "d1_run": lambda *_args: _async_value(None),
        "edge_cache_delete": cache_delete,
        "ensure_schema": lambda _env: _async_value(None),
        "json_response": lambda data, **kwargs: {"data": data, **kwargs},
        "method_name": lambda request: request.method,
        "new_world_peer_id": lambda: "test-ticket-nonce",
    })
    response = asyncio.run(handler(
        object(), SimpleNamespace(method="GET", headers={})))
    assert response["data"]["totalActiveMs"] == 900_000
    assert response["data"]["totalSteps"] == 12_345
    assert response["data"]["activityObservedAt"] == 600_000
    assert response["data"]["ticket"] == "signed-next-ticket"
    assert encoded_claims[0]["activityGeneration"] == 5
    assert cache_deletes == ["internal:users"]


async def _async_value(value):
    return value


def test_public_directory_rounds_activity_down_to_whole_minutes():
    rounded = _load_function("_world_public_total_active_ms", {
        "WORLD_USER_ACTIVITY_MAX_TOTAL_MS": 9_000_000_000_000_000,
        "WORLD_USER_ACTIVITY_PUBLIC_BUCKET_MS": 60_000,
    })
    assert rounded(-1) == 0
    assert rounded(59_999) == 0
    assert rounded(119_999) == 60_000
    assert rounded(120_000) == 120_000

    steps = _load_function("_world_public_total_steps", {
        "WORLD_USER_STEP_MAX_TOTAL": 9_000_000_000_000,
    })
    assert steps(-1) == 0
    assert steps(12_345) == 12_345
    assert steps(10**20) == 9_000_000_000_000

    directory = ast.get_source_segment(
        ACCOUNT_USERS, _account_users_node("_account_users_directory"))
    assert "LEFT JOIN world_user_activity" in directory
    assert "total_active_ms" in directory
    assert "total_steps" in directory
    assert "last_touch_at" not in directory
    assert "activityObservedAt" not in directory


def test_browser_tracks_only_local_interval_over_server_base():
    assert 'WORLD_ACTIVITY_CONTINUATION_HEADER = "x-forkmesh-world-activity"' in WORLD
    assert "applyWorldActivityTicket(ticket)" in WORLD
    assert "this.worldActivityBaseMs = total;" in WORLD
    assert "performance.now() - this.worldActivityBaseAt" in WORLD
    assert "this.pauseWorldActivity();" in WORLD
    assert "keepalive: true" in WORLD
    pause_start = WORLD.index("  pauseWorldActivity() {")
    pause_source = WORLD[
        pause_start:
        WORLD.index("\n  async refreshWorldTicket()", pause_start)
    ]
    assert "durationMs" not in pause_source
    assert "this.syncCurrentWorldActivity();" in WORLD[
        WORLD.index("this.distanceTimer = window.setInterval"):
        WORLD.index(
            'document.addEventListener("visibilitychange"',
            WORLD.index("this.distanceTimer = window.setInterval"),
        )
    ]
    assert "this.leaderboardMembers()" in WORLD
    assert 'message.type === "step-total"' in WORLD
    assert "this.identity.totalSteps = totalSteps" in WORLD
    assert "toLocaleString(\"en-US\")} STEPS" in SCENE


def test_leaderboard_ranks_descending_and_can_show_active_zero_state():
    ranking_start = SCENE.index("function rankedActiveLeaderboardMembers")
    ranking = SCENE[
        ranking_start:
        SCENE.index("function activeLeaderboardTexture", ranking_start)
    ]
    assert "member?.activeNow === true" in ranking
    assert "Number(right?.totalActiveMs" in ranking
    assert "Number(left?.totalActiveMs" in ranking
    assert ".slice(0, 6)" in ranking
    assert '"ACTIVE NOW · "' in SCENE
    assert SCENE.count("rankedActiveLeaderboardMembers(") >= 3
