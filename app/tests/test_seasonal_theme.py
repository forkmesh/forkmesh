"""The Halloween theme setting: one platform-wide value that only platform
administrators and moderators can change and that always switches itself
off."""

import asyncio
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import schema  # noqa: E402
import seasonal_theme as season  # noqa: E402
import urls  # noqa: E402

NOW = 1_790_000_000_000
DAY = 24 * 60 * 60 * 1000
ROLES = {"alice": "platform_administrator", "mo": "moderator"}


def run(coro):
    return asyncio.run(coro)


@pytest.fixture
def rt(monkeypatch):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    for statement in schema.SCHEMA_STATEMENTS:
        if re.search(r"instance_settings", statement):
            db.execute(statement)
    clock = SimpleNamespace(now=NOW)
    audit = []

    async def d1_first(env, sql, *args):
        rows = [dict(row) for row in db.execute(sql, args).fetchall()]
        return rows[0] if rows else None

    async def d1_run(env, sql, *args):
        db.execute(sql, args)

    async def session(env, request, data=None):
        return ("bi", {"name": request.account}) if request.account else ("", None)

    async def has_role(env, name, role, scope_type="platform", scope=""):
        return ROLES.get(name) == role

    async def record(env, actor, action, *args, **kwargs):
        audit.append((actor, action, args))

    async def body(request, max_bytes=None):
        return request.body

    async def nothing(*_args, **_kwargs):
        return None

    def json_response(data, status=200, cache_control=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control)

    for name, value in {
        "d1_first": d1_first, "d1_run": d1_run,
        "_account_session_record": session, "_has_role": has_role,
        "_audit_sensitive_action": record, "bounded_json_request": body,
        "json_response": json_response, "ensure_schema": nothing,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, n=240: value.strip()[:n] if isinstance(value, str) else "",
        "MAX_NODE_NAME": 63, "Date": SimpleNamespace(now=lambda: clock.now),
    }.items():
        monkeypatch.setattr(season, name, value, raising=False)
    season.forget()
    return SimpleNamespace(db=db, clock=clock, audit=audit)


def call(rest="", method="GET", account="", body=None):
    request = SimpleNamespace(method=method, account=account, body=body)
    return run(season.handler(SimpleNamespace(), request, rest))


def turn_on(account="alice", until=NOW + 7 * DAY):
    return call("settings", "POST", account, {"season": "halloween", "until": until})


def test_off_by_default_and_public(rt):
    reply = call()
    assert reply.status == 200
    assert reply.data == {"ok": True, "season": None, "until": 0}
    assert reply.cache_control.startswith("public")


def test_only_administrators_and_moderators_manage_it(rt):
    assert call("settings").status == 401
    assert call("settings", account="bob").status == 403
    assert turn_on("bob").status == 403
    assert turn_on("").status == 401
    assert call("settings", account="alice").data["season"] is None
    assert call("settings", account="mo").status == 200
    assert turn_on("mo").status == 200
    public = call()
    assert public.data == {"ok": True, "season": "halloween", "until": NOW + 7 * DAY}
    assert "by" not in public.data
    settings = call("settings", account="alice").data
    assert settings["by"] == "mo" and settings["season"] == "halloween"
    assert rt.audit[-1][:2] == ("mo", "admin.season")


def test_turning_on_needs_a_close_auto_off_time(rt):
    for until in (0, None, "soon", NOW, NOW + 60 * 1000, NOW + 61 * DAY):
        reply = call("settings", "POST", "alice", {"season": "halloween", "until": until})
        assert reply.data["error"] == "invalid_until", until
    assert call("settings", "POST", "alice", {"season": "christmas", "until": NOW + DAY}).data["error"] == "invalid_season"
    assert turn_on(until=NOW + 60 * DAY).status == 200


def test_it_switches_itself_off(rt):
    turn_on(until=NOW + DAY)
    assert call().data["season"] == "halloween"
    rt.clock.now = NOW + DAY + 1
    season.forget()
    assert call().data == {"ok": True, "season": None, "until": 0}
    assert call("settings", account="alice").data["season"] is None


def test_turning_off(rt):
    turn_on()
    reply = call("settings", "POST", "mo", {"season": ""})
    assert reply.status == 200 and reply.data["season"] is None
    stored = json.loads(rt.db.execute("SELECT doc FROM instance_settings WHERE k='season'").fetchone()[0])
    assert stored["season"] == "" and stored["until"] == 0 and stored["by"] == "mo"
    assert call().data["season"] is None


def test_methods_and_paths(rt):
    assert call("", "POST").status == 405
    assert call("settings", "DELETE", "alice").status == 405
    assert call("nope").status == 404
    assert urls.SEASON_RE.match("/api/season").group(1) is None
    assert urls.SEASON_RE.match("/api/season/settings").group(1) == "settings"
    assert not urls.SEASON_RE.match("/api/season/other")
