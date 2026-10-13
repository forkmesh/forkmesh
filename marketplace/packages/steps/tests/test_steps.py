import asyncio
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import steps  # noqa: E402


DAY = "2026-09-16"
ACCOUNTS = {"alice", "bob"}


class Runtime:
    def __init__(self, installed=("alice",), country=""):
        steps._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.actor = "alice"
        self.country_value = country
        self.method_value = "GET"
        self.body = {}
        self.query = {}
        self.clock = 1_789_561_860_000
        self.secrets = 0
        self.audits = []
        self.origin_ok = True
        for name in installed:
            response = call(self.use("POST", actor=name), "/api/steps/install")
            assert response["status"] == 200, response
        self.use("GET")

    def use(self, method, actor="alice", body=None, query=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def country(self): return self.country_value
    def query_params(self): return dict(self.query)
    def same_origin(self): return self.origin_ok
    def now(self): self.clock += 1; return self.clock
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = "bi:" + self.actor if self.actor in ACCOUNTS else ""
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    def new_secret(self):
        self.secrets += 1
        return "key-%032x" % self.secrets
    async def seal_with(self, secret, value):
        return "sealed[%s]:%s" % (secret, json.dumps(value, sort_keys=True))
    async def open_with(self, secret, value):
        prefix = "sealed[%s]:" % secret
        text = str(value or "")
        return json.loads(text.removeprefix(prefix)) if text.startswith(prefix) else None
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append(args)


def call(runtime, path):
    return asyncio.run(steps.handle(runtime, path))


def sync(runtime, days, actor="alice", device="iphone"):
    response = call(runtime.use("POST", actor=actor, body={
        "source": "health", "device": device, "days": days}), "/api/steps/sync")
    assert response["status"] == 200, response
    return response["data"]


def overview(runtime, actor="alice", day=DAY):
    return call(runtime.use("GET", actor=actor, query={"day": day}), "/api/steps")["data"]


def test_install_is_per_account_and_idempotent():
    runtime = Runtime(installed=())
    assert overview(runtime) == {"ok": True, "installed": False, "actor": "alice"}
    blocked = call(runtime.use("POST", body={"days": [{"day": DAY, "steps": 5}]}), "/api/steps/sync")
    assert blocked["status"] == 409
    assert blocked["data"] == {"error": "plugin_not_installed", "canInstall": True}
    done = call(runtime.use("POST", body={"goal": 8000, "tzOffsetMinutes": 300}), "/api/steps/install")
    assert done["data"] == {"ok": True, "installed": True, "plugin": "steps"}
    call(runtime.use("POST"), "/api/steps/install")
    assert runtime.db.execute("SELECT COUNT(*) FROM steps_profiles").fetchone()[0] == 1
    data = overview(runtime)
    assert data["installed"] is True and data["profile"]["goal"] == 8000
    assert data["profile"]["tzOffsetMinutes"] == 300
    assert overview(runtime, actor="bob")["installed"] is False


def test_health_sync_replaces_the_day_and_seals_it_for_the_account():
    runtime = Runtime(installed=("alice", "bob"))
    sync(runtime, [{"day": DAY, "steps": 4200, "distanceM": 3100.4, "floors": 3}])
    result = sync(runtime, [
        {"day": DAY, "steps": 6400, "distanceM": 4700, "floors": 5, "activeKcal": 210},
        {"day": "2026-09-15", "steps": 11000, "distanceM": 8000},
    ])
    assert result["synced"] == 2
    today = overview(runtime)["today"]
    assert (today["steps"], today["distanceM"], today["floors"], today["activeKcal"]) == (6400, 4700.0, 5, 210.0)
    assert today["sources"]["health"]["device"] == "iphone"
    stored = runtime.db.execute("SELECT * FROM steps_days WHERE day=?", (DAY,)).fetchone()
    assert set(stored.keys()) == {"account_bi", "day", "updated_at", "data"}
    assert "6400" in stored["data"] and not stored["data"].startswith("sealed:")
    bob_key = json.loads(runtime.db.execute(
        "SELECT wrapped FROM steps_keys WHERE account_bi='bi:bob'").fetchone()[0]
        .removeprefix("sealed:"))["key"]
    assert asyncio.run(runtime.open_with(bob_key, stored["data"])) is None
    assert overview(runtime, actor="bob")["today"]["steps"] == 0


def test_manual_steps_add_to_health_and_can_be_removed():
    runtime = Runtime()
    sync(runtime, [{"day": DAY, "steps": 3000, "distanceM": 2000}])
    saved = call(runtime.use("PUT", body={"steps": 2500, "distanceM": 1800}), "/api/steps/days/" + DAY)
    assert saved["data"]["day"]["steps"] == 5500
    assert saved["data"]["day"]["distanceM"] == 3800.0
    assert set(saved["data"]["day"]["sources"]) == {"health", "manual"}
    removed = call(runtime.use("DELETE"), "/api/steps/days/" + DAY)
    assert removed["data"]["day"]["steps"] == 3000
    everything = call(runtime.use("DELETE", query={"source": "all"}), "/api/steps/days/" + DAY)
    assert everything["data"]["day"]["steps"] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM steps_days").fetchone()[0] == 0
    bad = call(runtime.use("DELETE", query={"source": "watch"}), "/api/steps/days/" + DAY)
    assert bad["status"] == 400


def test_overview_series_goal_streak_and_summary():
    runtime = Runtime()
    call(runtime.use("PUT", body={"goal": 5000}), "/api/steps/profile")
    sync(runtime, [
        {"day": "2026-09-13", "steps": 6000},
        {"day": "2026-09-14", "steps": 7000},
        {"day": "2026-09-15", "steps": 5200, "distanceM": 4000},
        {"day": DAY, "steps": 1200},
    ])
    data = overview(runtime)
    assert len(data["days"]) == 30 and data["days"][-1]["day"] == DAY
    assert data["days"][0]["day"] == "2026-08-18"
    summary = data["summary"]
    assert summary["streak"] == 3
    assert summary["goalDays"] == 3
    assert summary["bestDay"] == "2026-09-14" and summary["bestSteps"] == 7000
    assert summary["weekSteps"] == 19400
    assert summary["averageSteps"] == 19400 // 4


def test_sync_validation_and_bounds():
    runtime = Runtime()
    assert call(runtime.use("POST", body={"days": []}), "/api/steps/sync")["status"] == 400
    too_many = [{"day": steps._shift_day(DAY, -index), "steps": 1} for index in range(63)]
    assert call(runtime.use("POST", body={"days": too_many}), "/api/steps/sync")["status"] == 400
    assert call(runtime.use("POST", body={"source": "manual", "days": [{"day": DAY}]}),
                "/api/steps/sync")["status"] == 400
    result = sync(runtime, [{"day": DAY, "steps": 10 ** 9, "distanceM": -5}, {"day": "nope", "steps": 3}])
    assert result["synced"] == 1 and result["days"][0]["steps"] == steps.MAX_STEPS
    assert result["days"][0]["distanceM"] == 0.0
    cleared = sync(runtime, [{"day": DAY, "steps": 0}])
    assert cleared["days"][0]["steps"] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM steps_days").fetchone()[0] == 0


def test_routes_methods_origin_and_session():
    runtime = Runtime()
    assert call(runtime.use("GET"), "/api/stepsx")["status"] == 404
    assert call(runtime.use("GET"), "/api/steps/nothing")["status"] == 404
    wrong = call(runtime.use("POST"), "/api/steps")
    assert wrong["status"] == 405 and wrong["headers"]["allow"] == "GET"
    assert call(runtime.use("PUT", body={}), "/api/steps/days/2026-13-01")["status"] == 400
    runtime.origin_ok = False
    assert call(runtime.use("POST", body={"days": [{"day": DAY}]}), "/api/steps/sync")["status"] == 403
    runtime.origin_ok = True
    assert call(runtime.use("GET", actor="mallory"), "/api/steps")["status"] == 401


def test_units_follow_country_until_chosen():
    runtime = Runtime(country="US")
    assert overview(runtime)["profile"]["units"] == "imperial"
    runtime.country_value = "DE"
    assert overview(runtime)["profile"]["units"] == "metric"
    call(runtime.use("PUT", body={"unitsPreference": "imperial"}), "/api/steps/profile")
    assert overview(runtime)["profile"]["units"] == "imperial"


def test_range_export_uninstall_and_purge():
    runtime = Runtime(installed=("alice", "bob"))
    sync(runtime, [{"day": DAY, "steps": 900}, {"day": "2026-09-10", "steps": 400}])
    sync(runtime, [{"day": DAY, "steps": 50}], actor="bob")
    ranged = call(runtime.use("GET", query={"from": "2026-09-10", "to": DAY}), "/api/steps/days")["data"]
    assert len(ranged["days"]) == 7 and ranged["days"][0]["steps"] == 400
    exported = call(runtime.use("GET"), "/api/steps/export")["data"]
    assert exported["filename"] == "forkmesh-steps-alice.json"
    assert [item["steps"] for item in exported["export"]["days"]] == [400, 900]
    gone = call(runtime.use("POST"), "/api/steps/uninstall")["data"]
    assert gone["uninstalled"] is True and gone["hadData"] is True
    assert len(gone["export"]["days"]) == 2
    assert overview(runtime)["installed"] is False
    for table in ("steps_days", "steps_profiles", "steps_keys"):
        rows = runtime.db.execute("SELECT account_bi FROM " + table).fetchall()
        assert {row[0] for row in rows} == {"bi:bob"}
    asyncio.run(steps.purge_account_data(runtime, "bi:bob", "bob"))
    assert runtime.db.execute("SELECT COUNT(*) FROM steps_days").fetchone()[0] == 0


def test_manifest_matches_the_worker():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["id"] == steps.PLUGIN_ID
    assert manifest["worker"] == {"module": "steps", "api": steps.PREFIX, "outboundOrigins": []}
    assert manifest["dataLifecycle"] == {"scope": "account"}
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "steps")
    assert row["version"] == manifest["version"]
    source = (PACKAGE / "content" / "desktop" / "StepsFeature.cpp").read_text(encoding="utf-8")
    assert steps.REQUEST_PROOF in source and steps.PREFIX in source
