#!/usr/bin/env python3
"""Executable persistence, authorization, UTC, and World-client event tests."""

import asyncio
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[2] / "app"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import world_events as policy  # noqa: E402
import world_events_api as api  # noqa: E402


NOW = 1_800_000_000_000


def run_async_test(function):
    def wrapper():
        return asyncio.run(function())
    wrapper.__name__ = function.__name__
    return wrapper


def event_record(**updates):
    record = {
        "type": "hackathon",
        "title": "Mirror resilience hackathon",
        "description": "A public UTC collaboration session.",
        "destination": "Community stage",
        "startsAt": policy.utc_iso(NOW + 60_000),
        "endsAt": policy.utc_iso(NOW + 3_600_000),
    }
    record.update(updates)
    return record


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0058_world_events.sql")
            .read_text(encoding="utf-8")
        )
        self.request_method = "GET"
        self.request_data = {}
        self.actor = ""
        self.clock = NOW
        self.ids = 0
        self.admins = {"admin"}
        self.audits = []

    def use(self, method, actor="", data=None, now=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        if now is not None:
            self.clock = now
        return self

    def method(self):
        return self.request_method

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        if not isinstance(self.request_data, dict):
            return None, "invalid_json"
        return self.request_data, ""

    async def session(self, _data):
        if not self.actor:
            return "", None
        return "bi-" + self.actor, {"name": self.actor}

    async def is_admin(self, name):
        return name in self.admins

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append({
            "actor": actor,
            "action": action,
            "target_type": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    async def d1_all(self, sql, *args):
        return [
            dict(row) for row in self.db.execute(sql, args).fetchall()
        ]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()


class CachedFakeRuntime(FakeRuntime):
    def __init__(self):
        super().__init__()
        self.env = object()
        self.list_reads = 0

    async def d1_all(self, sql, *args):
        if "FROM world_events" in sql:
            self.list_reads += 1
        return await super().d1_all(sql, *args)


def test_event_policy_requires_explicit_utc_and_bounded_future_window():
    normalized, error = policy.normalize_event(event_record(), NOW)
    assert error == ""
    assert normalized["startsAt"].endswith("Z")
    assert normalized["endsAtMs"] > normalized["startsAtMs"]

    assert policy.normalize_event(
        event_record(startsAt="2027-01-15T13:00:00-05:00"), NOW
    )[1] == "utc_times_required"
    assert policy.normalize_event(
        event_record(
            startsAt=policy.utc_iso(NOW - 120_000),
            endsAt=policy.utc_iso(NOW - 60_000),
        ),
        NOW,
    )[1] == "event_already_expired"
    assert policy.normalize_event(
        event_record(secret="not-public"), NOW
    )[1] == "unsupported_field"


@run_async_test
async def test_event_api_is_public_read_admin_write_update_cancel_and_audited():
    runtime = FakeRuntime()
    empty = await api.handle(runtime.use("GET"), "/api/world/events")
    assert empty["status"] == 200
    assert empty["data"]["events"] == []
    assert empty["cache_control"].startswith("public, max-age=30")

    denied = await api.handle(
        runtime.use("POST", "alice", event_record()),
        "/api/world/events",
    )
    assert denied["status"] == 403
    assert runtime.audits[-1]["outcome"] == "denied"

    created = await api.handle(
        runtime.use("POST", "admin", event_record()),
        "/api/world/events",
    )
    assert created["status"] == 201
    event_id = created["data"]["event"]["id"]
    assert created["data"]["event"]["title"] == (
        "Mirror resilience hackathon")
    stored = runtime.db.execute(
        "SELECT created_by_bi,status FROM world_events WHERE event_id=?",
        (event_id,),
    ).fetchone()
    assert tuple(stored) == ("bi-admin", "scheduled")

    listed = await api.handle(runtime.use("GET"), "/api/world/events")
    assert [item["id"] for item in listed["data"]["events"]] == [event_id]
    assert "created_by_bi" not in json.dumps(listed["data"])

    updated = await api.handle(
        runtime.use(
            "PATCH",
            "admin",
            {"title": "Updated mirror resilience hackathon"},
        ),
        f"/api/world/events/{event_id}",
    )
    assert updated["status"] == 200
    assert updated["data"]["event"]["title"].startswith("Updated")

    cancelled = await api.handle(
        runtime.use("DELETE", "admin"),
        f"/api/world/events/{event_id}",
    )
    assert cancelled["status"] == 200
    assert runtime.audits[-1]["action"] == "world.event.cancel"
    hidden = await api.handle(runtime.use("GET"), "/api/world/events")
    assert hidden["data"]["events"] == []


@run_async_test
async def test_expired_events_are_filtered_even_before_retention_cleanup():
    runtime = FakeRuntime()
    created = await api.handle(
        runtime.use("POST", "admin", event_record()),
        "/api/world/events",
    )
    event_id = created["data"]["event"]["id"]
    listed = await api.handle(
        runtime.use("GET", now=NOW + 3_600_001),
        "/api/world/events",
    )
    assert listed["data"]["events"] == []
    detail = await api.handle(
        runtime.use("GET"),
        f"/api/world/events/{event_id}",
    )
    assert detail["status"] == 404

    await api.cleanup_records(
        runtime.use(
            "GET",
            now=NOW + 3_600_001 + policy.EVENT_RETENTION_MS,
        )
    )
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM world_events"
    ).fetchone()[0] == 0


@run_async_test
async def test_public_event_bursts_share_one_read_and_mutations_invalidate_cache():
    runtime = CachedFakeRuntime()
    first = await api.handle(runtime.use("GET"), "/api/world/events")
    second = await api.handle(
        runtime.use("GET", now=NOW + 5_000),
        "/api/world/events",
    )
    assert first["data"] == second["data"]
    assert runtime.list_reads == 1

    created = await api.handle(
        runtime.use("POST", "admin", event_record(), now=NOW + 5_000),
        "/api/world/events",
    )
    refreshed = await api.handle(
        runtime.use("GET", now=NOW + 6_000),
        "/api/world/events",
    )
    assert refreshed["data"]["events"][0]["id"] == (
        created["data"]["event"]["id"])
    assert runtime.list_reads == 2


def test_event_schema_route_and_client_use_live_d1_records():
    migration = (
        ROOT / "migrations" / "0058_world_events.sql"
    ).read_text(encoding="utf-8")
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    entry = (
        (SRC / "entry.py").read_text(encoding="utf-8")
        + (SRC / "app_runtime.py").read_text(encoding="utf-8")
    )
    app = (
        ROOT.parent / "world" / "public" / "world" / "world.js"
    ).read_text(encoding="utf-8")
    data = (
        ROOT.parent / "world" / "public" / "world" / "world-data.js"
    ).read_text(encoding="utf-8")

    assert "CREATE TABLE IF NOT EXISTS world_events" in migration
    assert "created_by_bi TEXT NOT NULL" in migration
    assert "trg_world_events_record_limit" in migration
    assert "CREATE TABLE IF NOT EXISTS world_events" in schema
    assert 'world_events_api = _LazyModule("world_events_api")' in entry
    assert "world_events_api.cleanup_records(runtime)" in entry
    assert 'url.path == "/api/world/events"' in entry
    assert 'this.fetchJSON("/api/world/events"' in app
    assert "normalizeCommunityEvents" in app
    assert "end <= Math.max(start, now)" in app
    assert "data-world-events-refresh" in app
    assert "No seeded or demo announcement" in app
    assert "COMMUNITY_EVENTS" not in data
