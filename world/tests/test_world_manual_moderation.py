#!/usr/bin/env python3
"""Contracts for explicit, temporary World moderation.

This is intentionally not an abuse detector or quarantine subsystem.
"""

import ast
import asyncio
import hashlib
import importlib.util
import json
import re
import sqlite3
from pathlib import Path
from types import SimpleNamespace

from worker_test_helpers import json_from_request_double
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
WORLD_DURABLE_OBJECTS = (
    ROOT.parent / "world" / "src" / "world_durable_objects.py"
)
SCHEMA = ROOT / "src" / "schema.py"
MIGRATION = ROOT / "migrations" / "0069_world_manual_moderation.sql"
WORLD_PATH = ROOT.parent / "world" / "src" / "world.py"
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
WORLD_DURABLE_OBJECTS_TEXT = WORLD_DURABLE_OBJECTS.read_text(encoding="utf-8")

spec = importlib.util.spec_from_file_location(
    "forkmesh_world_manual_protocol", WORLD_PATH)
world = importlib.util.module_from_spec(spec)
spec.loader.exec_module(world)


def _load(*names, extra_globals=None):
    trees = (
        ast.parse(ENTRY_TEXT, filename=str(ENTRY)),
        ast.parse(
            WORLD_DURABLE_OBJECTS.read_text(encoding="utf-8"),
            filename=str(WORLD_DURABLE_OBJECTS),
        ),
    )
    selected = [
        node for tree in trees for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra_globals or {})
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace


def _run(value):
    return asyncio.run(value)


class _Date:
    value = 1_700_000_000_000

    @classmethod
    def now(cls):
        return cls.value


class _Headers:
    def __init__(self, values):
        self.values = {
            str(key).lower(): str(value) for key, value in values.items()
        }

    def get(self, name, default=None):
        return self.values.get(str(name).lower(), default)


class _Request:
    def __init__(self, data, headers=None, method="POST"):
        self._data = data
        self.headers = _Headers(headers or {})
        self.method = method
        self.url = "https://forkmesh.test/api/world/moderation"

    async def json(self):
        return self._data


def _response(payload, status=200, **kwargs):
    return {"payload": payload, "status": status, **kwargs}


async def _noop(*_args, **_kwargs):
    return None


def test_schema_adds_only_a_manual_temporary_block_plane():
    schema = SCHEMA.read_text(encoding="utf-8")
    migration = MIGRATION.read_text(encoding="utf-8")
    for source in (schema, migration):
        assert "CREATE TABLE IF NOT EXISTS world_manual_blocks" in source
        assert "target_type IN ('ip','agent')" in source
        assert "expires_at INTEGER NOT NULL" in source
        assert "revoked_at INTEGER NOT NULL DEFAULT 0" in source
        for retired in (
            "CREATE TABLE IF NOT EXISTS security_signals",
            "CREATE TABLE IF NOT EXISTS security_restrictions",
            "CREATE TABLE IF NOT EXISTS security_appeals",
        ):
            assert retired not in source
    assert "raw ip addresses and raw user-agent strings are never stored" in (
        migration.lower())

    db = sqlite3.connect(":memory:")
    db.executescript(migration)
    db.executescript(migration)
    columns = {
        row[1] for row in db.execute("PRAGMA table_info(world_manual_blocks)")
    }
    assert columns == {
        "block_id", "target_type", "subject_token", "created_by_bi",
        "created_at", "expires_at", "revoked_at",
    }


def _token_runtime():
    async def blind_index(_env, value):
        return hashlib.sha256(("secret:" + value).encode()).hexdigest()

    return _load(
        "_transient_client_address",
        "_world_moderation_tokens",
        extra_globals={
            "Date": _Date,
            "WORLD_MANUAL_TOKEN_DAY_MS": 24 * 60 * 60 * 1000,
            "blind_index": blind_index,
        },
    )


def test_edge_tokens_are_opaque_daily_and_agent_subject_is_narrow():
    runtime = _token_runtime()
    first = _Request({}, headers={
        "cf-connecting-ip": "203.0.113.42",
        "user-agent": (
            "Mozilla/5.0 (X11; Linux x86_64) "
            "AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36"),
    })
    upgraded = _Request({}, headers={
        "cf-connecting-ip": "203.0.113.42",
        "user-agent": (
            "Mozilla/5.0 (X11; Linux x86_64) "
            "AppleWebKit/537.36 Chrome/140.9.8.7 Safari/537.36"),
    })
    now = 1_700_000_000_000
    tokens = _run(runtime["_world_moderation_tokens"](None, first, now))
    different_build = _run(
        runtime["_world_moderation_tokens"](None, upgraded, now))
    next_day = _run(runtime["_world_moderation_tokens"](
        None, first, now + 24 * 60 * 60 * 1000))

    assert set(tokens) == {"ip", "agent"}
    assert all(re.fullmatch(r"[a-f0-9]{64}", value) for value in tokens.values())
    assert "203.0.113.42" not in repr(tokens)
    assert "Chrome/120" not in repr(tokens)
    assert different_build["ip"] == tokens["ip"]
    assert different_build["agent"] != tokens["agent"]
    assert next_day["ip"] != tokens["ip"]
    assert next_day["agent"] != tokens["agent"]


def test_active_block_lookup_uses_only_tokens_and_unexpired_manual_rows():
    calls = []

    async def d1_first(_env, sql, *params):
        calls.append((sql, params))
        return {"target_type": "ip", "expires_at": 2000}

    runtime = _load("_world_active_manual_block", extra_globals={
        "Date": _Date,
        "re": re,
        "ensure_schema": _noop,
        "d1_first": d1_first,
    })
    ip_token = "a" * 64
    agent_token = "b" * 64
    result = _run(runtime["_world_active_manual_block"](
        None, {"ip": ip_token, "agent": agent_token}, 1000))
    assert result == {"targetType": "ip", "expiresAt": 2000}
    sql, params = calls[0]
    assert "world_manual_blocks" in sql
    assert "revoked_at=0" in sql
    assert "expires_at>?" in sql
    assert params == (1000, ip_token, agent_token)


def _handler_runtime(*, admin=True, disconnects=2):
    writes = []
    audits = []

    async def account_session(_env, _request, _data):
        return "bi:admin", {"name": "jett"}

    async def has_role(_env, actor, role):
        assert actor == "jett"
        assert role == "platform_administrator"
        return admin

    async def d1_run(_env, sql, *params):
        writes.append((sql, params))

    async def audit(_env, actor, action, target_type="", target="",
                    outcome="success", details=None):
        audits.append({
            "actor": actor,
            "action": action,
            "targetType": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    async def disconnect(*_args):
        return disconnects

    runtime = _load("world_moderation_handler", extra_globals={
        "method_name": lambda request: request.method,
        "json_response": _response,
        "_account_session_record": account_session,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 64,
        "_has_role": has_role,
        "_audit_sensitive_action": audit,
        "WORLD_MANUAL_BLOCK_DEFAULT_MS": 15 * 60 * 1000,
        "WORLD_MANUAL_BLOCK_MIN_MS": 60 * 1000,
        "WORLD_MANUAL_BLOCK_MAX_MS": 24 * 60 * 60 * 1000,
        "WORLD_MANUAL_BLOCK_RETAIN_MS": 30 * 24 * 60 * 60 * 1000,
        "WORLD_MANUAL_TOKEN_DAY_MS": 24 * 60 * 60 * 1000,
        "Date": _Date,
        "new_world_peer_id": lambda: "c" * 16,
        "ensure_schema": _noop,
        "d1_run": d1_run,
        "_world_disconnect_manual_block": disconnect,
        "re": re,
    })
    return runtime["world_moderation_handler"], writes, audits


def test_admin_manual_block_is_bounded_persisted_audited_and_disconnects():
    handler, writes, audits = _handler_runtime()
    handle = "d" * 64
    response = _run(handler(None, _Request({
        "targetType": "ip",
        "handle": handle,
        "durationMs": 5 * 60 * 1000,
    })))
    assert response["status"] == 200
    assert response["payload"]["disconnected"] == 2
    assert response["payload"]["targetType"] == "ip"
    assert "automatic abuse detection and quarantine remain off" in (
        response["payload"]["notice"])
    insert = next(row for row in writes if "INSERT INTO world_manual_blocks" in row[0])
    assert insert[1][1:4] == ("ip", handle, "bi:admin")
    assert insert[1][5] == response["payload"]["expiresAt"]
    assert audits[-1]["action"] == "world.manual_block"
    assert audits[-1]["outcome"] == "success"
    assert audits[-1]["details"]["manual"] is True
    assert audits[-1]["details"]["disconnectedCount"] == 2


def test_non_admin_and_invalid_duration_cannot_create_blocks():
    handle = "e" * 64
    handler, writes, audits = _handler_runtime(admin=False)
    response = _run(handler(None, _Request({
        "targetType": "agent", "handle": handle, "durationMs": 60_000,
    })))
    assert response["status"] == 403
    assert writes == []
    assert audits[-1]["outcome"] == "denied"

    handler, writes, _audits = _handler_runtime()
    for duration in (True, "60000", 59_999, 24 * 60 * 60 * 1000 + 1):
        response = _run(handler(None, _Request({
            "targetType": "agent",
            "handle": handle,
            "durationMs": duration,
        })))
        assert response["status"] == 400
        assert response["payload"]["error"] == "invalid_duration"
    assert writes == []


def test_durable_object_exposes_handles_only_to_verified_admin_viewer():
    class DurableObject:
        pass

    runtime = _load("ForkMeshWorld", extra_globals={
        "DurableObject": DurableObject,
        "world_protocol": world,
        "_ws_attachment": lambda socket: SimpleNamespace(**socket),
        "_ws_attr": lambda socket, key, default=None: socket.get(key, default),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "re": re,
    })
    instance = runtime["ForkMeshWorld"]()
    subject = {
        **world.default_presence("peer", 1000),
        "ip_token": "a" * 64,
        "agent_token": "b" * 64,
        "client_ip": "203.0.113.42",
        "client_user_agent": (
            "Mozilla/5.0 (X11; Linux x86_64) "
            "AppleWebKit/537.36 Chrome/140.0.0.0 Safari/537.36"
        ),
    }
    regular = instance._presence_for_viewer({"is_admin": False}, subject)
    admin = instance._presence_for_viewer({"is_admin": True}, subject)
    assert "moderationHandles" not in regular
    assert "adminGuestNetwork" not in regular
    assert "203.0.113.42" not in repr(regular)
    assert "Chrome/140" not in repr(regular)
    assert "a" * 64 not in repr(regular)
    assert admin["moderationHandles"] == {
        "ip": "a" * 64,
        "agent": "b" * 64,
    }
    assert admin["adminGuestNetwork"] == {
        "ipAddress": "203.0.113.42",
        "userAgent": subject["client_user_agent"],
    }

    verified_member = {
        **subject,
        "accountStatus": "Member",
        "trusted_name": "alice",
    }
    member_admin_view = instance._presence_for_viewer(
        {"is_admin": True}, verified_member)
    assert "adminGuestNetwork" not in member_admin_view
    assert "203.0.113.42" not in repr(member_admin_view)


def test_guest_network_detail_is_ephemeral_admin_only_frontend_data():
    app = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(
        encoding="utf-8")
    scene = world_scene_source()
    bridge = ast.unparse(next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "world_durable_object_request"
    ))
    assert "x-forkmesh-world-private-ip" in bridge
    assert "x-forkmesh-world-private-agent" in bridge
    assert "client_ip=client_ip" in WORLD_DURABLE_OBJECTS_TEXT
    assert "client_user_agent=client_user_agent" in WORLD_DURABLE_OBJECTS_TEXT
    assert "adminGuestNetwork" in WORLD_DURABLE_OBJECTS_TEXT
    assert "adminGuestNetwork" in app
    assert "sanitizedAdminGuestNetwork" in scene
    assert 'identity?.isAdmin === true' in scene
    assert 'String(remote?.accountStatus || "Guest") !== "Guest"' in scene
    assert "CLICK TO COPY" in scene
    assert "navigator.clipboard.writeText" in app
    # Raw network strings are never columns in the persistent moderation table.
    migration = MIGRATION.read_text(encoding="utf-8").lower()
    assert "client_ip" not in migration
    assert "user_agent" not in migration


def test_route_checks_manual_block_before_world_admission():
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    source = ast.unparse(next(
        node for node in ast.parse(runtime).body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
    ))
    moderation = source.index("world_moderation_handler")
    socket = source.index("world_websocket_origin_allowed")
    tokens = source.index("_world_moderation_tokens", socket)
    active = source.index("_world_active_manual_block", tokens)
    fetch = source.index("world_object.fetch", active)
    assert moderation < socket < tokens < active < fetch
    assert "world_temporarily_blocked" in source
    assert "automated abuse or quarantine" in source


def test_world_ticket_admin_bit_is_server_verified_and_not_public_presence():
    claim = ast.unparse(next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_world_account_claim"
    ))
    decode = ast.unparse(next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_world_ticket_decode"
    ))
    bridge = ast.unparse(next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "world_durable_object_request"
    ))
    assert "_has_role(env, name, 'platform_administrator')" in claim
    assert "claim.get('isAdmin') is True" in decode
    assert "claim.get('isAdmin') is True" in bridge
    assert "isAdmin" not in world.WORLD_PUBLIC_FIELDS
