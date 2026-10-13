import asyncio
import json
from pathlib import Path
import sqlite3
import sys
from datetime import datetime, timedelta, timezone


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import sleep  # noqa: E402


NIGHT = "2026-09-16"
ACCOUNTS = {"alice", "bob"}


class Runtime:
    def __init__(self, installed=("alice",), country=""):
        sleep._SCHEMA_READY = False
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
            response = call(self.use("POST", actor=name), "/api/sleep/install")
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
    return asyncio.run(sleep.handle(runtime, path))



def at(day, clock, tz=0):
    stamp = datetime.fromisoformat(day + "T" + clock + ":00").replace(tzinfo=timezone.utc)
    return int((stamp + timedelta(minutes=tz)).timestamp() * 1000)


def night(wake_day, bed="23:00", wake="07:00", tz=0, **extra):
    bed_day = sleep._shift_day(wake_day, -1) if bed > wake else wake_day
    return dict({"night": wake_day, "bedAt": at(bed_day, bed, tz), "wakeAt": at(wake_day, wake, tz)}, **extra)


def sync(runtime, nights, source="phone", actor="alice", device="iphone", **extra):
    response = call(runtime.use("POST", actor=actor, body=dict({
        "source": source, "device": device, "nights": nights}, **extra)), "/api/sleep/sync")
    assert response["status"] == 200, response
    return response["data"]


def overview(runtime, actor="alice", day=NIGHT):
    return call(runtime.use("GET", actor=actor, query={"night": day}), "/api/sleep")["data"]


def test_install_is_per_account_and_idempotent():
    runtime = Runtime(installed=())
    assert overview(runtime) == {"ok": True, "installed": False, "actor": "alice"}
    blocked = call(runtime.use("POST", body={"source": "phone", "nights": [night(NIGHT)]}), "/api/sleep/sync")
    assert blocked["status"] == 409
    assert blocked["data"] == {"error": "plugin_not_installed", "canInstall": True}
    done = call(runtime.use("POST", body={"goalMinutes": 450, "tzOffsetMinutes": 240, "bedtime": "22:30"}),
                "/api/sleep/install")
    assert done["data"] == {"ok": True, "installed": True, "plugin": "sleep"}
    call(runtime.use("POST"), "/api/sleep/install")
    assert runtime.db.execute("SELECT COUNT(*) FROM sleep_profiles").fetchone()[0] == 1
    profile = overview(runtime)["profile"]
    assert (profile["goalMinutes"], profile["tzOffsetMinutes"], profile["bedtime"]) == (450, 240, "22:30")
    assert profile["wakeTime"] == "07:00" and profile["scheduleMinutes"] == 510
    assert overview(runtime, actor="bob")["installed"] is False


def test_phone_detection_sync_is_sealed_for_the_account():
    runtime = Runtime(installed=("alice", "bob"))
    result = sync(runtime, [night(NIGHT, "23:10", "06:50", awakeMinutes=25, interruptions=2,
                                  asleepMinutes=420, confidence="high")])
    assert result["synced"] == 1
    last = overview(runtime)["lastNight"]
    assert last["source"] == "phone" and last["asleepMinutes"] == 420
    assert (last["awakeMinutes"], last["interruptions"], last["confidence"]) == (25, 2, "high")
    assert last["sources"]["phone"]["device"] == "iphone"
    stored = runtime.db.execute("SELECT * FROM sleep_nights WHERE night=?", (NIGHT,)).fetchone()
    assert set(stored.keys()) == {"account_bi", "night", "updated_at", "data"}
    assert "420" in stored["data"] and not stored["data"].startswith("sealed:")
    bob_key = json.loads(runtime.db.execute(
        "SELECT wrapped FROM sleep_keys WHERE account_bi='bi:bob'").fetchone()[0]
        .removeprefix("sealed:"))["key"]
    assert asyncio.run(runtime.open_with(bob_key, stored["data"])) is None
    assert overview(runtime, actor="bob")["lastNight"]["asleepMinutes"] == 0


def test_hand_entry_beats_apple_health_which_beats_the_phone():
    runtime = Runtime()
    sync(runtime, [night(NIGHT, "23:00", "07:00", awakeMinutes=30)])
    assert overview(runtime)["lastNight"]["asleepMinutes"] == 450
    sync(runtime, [night(NIGHT, "23:30", "06:30", asleepMinutes=390, deepMinutes=60, remMinutes=80)],
         source="health", device="watch")
    last = overview(runtime)["lastNight"]
    assert last["source"] == "health" and last["asleepMinutes"] == 390 and last["deepMinutes"] == 60
    assert set(last["sources"]) == {"health", "phone"}
    saved = call(runtime.use("PUT", body=night(NIGHT, "22:00", "06:00", awakeMinutes=15)),
                 "/api/sleep/nights/" + NIGHT)
    assert saved["data"]["night"]["source"] == "manual"
    assert saved["data"]["night"]["asleepMinutes"] == 465
    assert "deepMinutes" not in saved["data"]["night"]
    removed = call(runtime.use("DELETE"), "/api/sleep/nights/" + NIGHT)
    assert removed["data"]["night"]["source"] == "health"
    phone_only = call(runtime.use("DELETE", query={"source": "health"}), "/api/sleep/nights/" + NIGHT)
    assert phone_only["data"]["night"]["source"] == "phone"
    everything = call(runtime.use("DELETE", query={"source": "all"}), "/api/sleep/nights/" + NIGHT)
    assert everything["data"]["night"]["asleepMinutes"] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM sleep_nights").fetchone()[0] == 0
    assert call(runtime.use("DELETE", query={"source": "watch"}), "/api/sleep/nights/" + NIGHT)["status"] == 400


def test_a_night_with_no_sleep_clears_only_that_source():
    runtime = Runtime()
    sync(runtime, [night(NIGHT)])
    sync(runtime, [night(NIGHT)], source="health")
    cleared = sync(runtime, [{"night": NIGHT, "asleepMinutes": 0}])
    assert set(cleared["nights"][0]["sources"]) == {"health"}
    sync(runtime, [{"night": NIGHT}], source="health")
    assert runtime.db.execute("SELECT COUNT(*) FROM sleep_nights").fetchone()[0] == 0


def test_sync_validation_and_bounds():
    runtime = Runtime()
    assert call(runtime.use("POST", body={"source": "phone", "nights": []}), "/api/sleep/sync")["status"] == 400
    too_many = [night(sleep._shift_day(NIGHT, -index)) for index in range(63)]
    assert call(runtime.use("POST", body={"source": "phone", "nights": too_many}), "/api/sleep/sync")["status"] == 400
    assert call(runtime.use("POST", body={"source": "manual", "nights": [night(NIGHT)]}),
                "/api/sleep/sync")["status"] == 400
    assert call(runtime.use("POST", body={"nights": [night(NIGHT)]}), "/api/sleep/sync")["status"] == 400
    long = night(NIGHT, "02:00", "21:00", asleepMinutes=10 ** 6, awakeMinutes=-4, interruptions=10 ** 6)
    result = sync(runtime, [long, {"night": "nope", "bedAt": 1, "wakeAt": 2}])
    stored = result["nights"][0]
    assert result["synced"] == 1 and stored["asleepMinutes"] == 19 * 60
    assert stored["awakeMinutes"] == 0 and stored["interruptions"] == sleep.MAX_INTERRUPTIONS
    too_long = night(NIGHT, "01:00", "23:30")
    too_long["bedAt"] -= 24 * 3_600_000
    assert sync(runtime, [too_long])["nights"][0]["asleepMinutes"] == 0
    backwards = dict(night(NIGHT), wakeAt=night(NIGHT)["bedAt"] - 1)
    assert call(runtime.use("PUT", body=backwards), "/api/sleep/nights/" + NIGHT)["status"] == 400


def test_overview_summary_streak_debt_and_average_clock_times():
    runtime = Runtime()
    call(runtime.use("PUT", body={"goalMinutes": 420, "tzOffsetMinutes": 300}), "/api/sleep/profile")
    sync(runtime, [
        night("2026-09-13", "23:30", "07:30", tz=300),
        night("2026-09-14", "00:30", "08:00", tz=300),
        night("2026-09-15", "23:50", "06:20", tz=300),
        night(NIGHT, "00:10", "07:40", tz=300),
    ])
    data = overview(runtime)
    assert len(data["nights"]) == 30 and data["nights"][-1]["night"] == NIGHT
    assert data["nights"][0]["night"] == "2026-08-18"
    summary = data["summary"]
    assert summary["loggedNights"] == 4
    assert summary["streak"] == 1
    assert summary["goalNights"] == 3
    assert summary["bestNight"] == "2026-09-13" and summary["bestMinutes"] == 480
    assert summary["averageMinutes"] == (480 + 450 + 390 + 450) // 4
    assert summary["debtMinutes"] == 30
    assert summary["averageBedtimeMinutes"] == 0
    assert summary["averageWakeMinutes"] in (442, 443)
    assert summary["bedtimeSpreadMinutes"] == 20
    before = overview(runtime, day=sleep._shift_day(NIGHT, 1))
    assert before["summary"]["streak"] == 1 and before["lastNight"]["asleepMinutes"] == 0


def test_sync_updates_the_time_zone_and_the_default_night_follows_it():
    runtime = Runtime()
    sync(runtime, [night(NIGHT)], tzOffsetMinutes=-600)
    assert overview(runtime)["profile"]["tzOffsetMinutes"] == -600
    runtime.clock = at("2026-09-16", "20:00")
    data = call(runtime.use("GET"), "/api/sleep")["data"]
    assert data["night"] == "2026-09-17"


def test_profile_rejects_bad_clock_times():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={"bedtime": "25:00", "wakeTime": "06:15", "goalMinutes": 5000}),
                 "/api/sleep/profile")["data"]["profile"]
    assert saved["bedtime"] == "23:00" and saved["wakeTime"] == "06:15"
    assert saved["goalMinutes"] == sleep.MAX_GOAL_MINUTES


def test_routes_methods_origin_and_session():
    runtime = Runtime()
    assert call(runtime.use("GET"), "/api/sleepx")["status"] == 404
    assert call(runtime.use("GET"), "/api/sleep/nothing")["status"] == 404
    wrong = call(runtime.use("POST"), "/api/sleep")
    assert wrong["status"] == 405 and wrong["headers"]["allow"] == "GET"
    assert call(runtime.use("PUT", body={}), "/api/sleep/nights/2026-13-01")["status"] == 400
    runtime.origin_ok = False
    assert call(runtime.use("POST", body={"source": "phone", "nights": [night(NIGHT)]}),
                "/api/sleep/sync")["status"] == 403
    runtime.origin_ok = True
    assert call(runtime.use("GET", actor="mallory"), "/api/sleep")["status"] == 401


def test_range_export_uninstall_and_purge():
    runtime = Runtime(installed=("alice", "bob"))
    sync(runtime, [night(NIGHT), night("2026-09-10", awakeMinutes=60)])
    sync(runtime, [night(NIGHT)], actor="bob")
    ranged = call(runtime.use("GET", query={"from": "2026-09-10", "to": NIGHT}), "/api/sleep/nights")["data"]
    assert len(ranged["nights"]) == 7 and ranged["nights"][0]["asleepMinutes"] == 420
    exported = call(runtime.use("GET"), "/api/sleep/export")["data"]
    assert exported["filename"] == "forkmesh-sleep-alice.json"
    assert exported["export"]["format"] == "forkmesh-sleep-export-v1"
    assert [item["asleepMinutes"] for item in exported["export"]["nights"]] == [420, 480]
    gone = call(runtime.use("POST"), "/api/sleep/uninstall")["data"]
    assert gone["uninstalled"] is True and gone["hadData"] is True
    assert len(gone["export"]["nights"]) == 2
    assert overview(runtime)["installed"] is False
    for table in ("sleep_nights", "sleep_profiles", "sleep_keys"):
        rows = runtime.db.execute("SELECT account_bi FROM " + table).fetchall()
        assert {row[0] for row in rows} == {"bi:bob"}
    asyncio.run(sleep.purge_account_data(runtime, "bi:bob", "bob"))
    assert runtime.db.execute("SELECT COUNT(*) FROM sleep_nights").fetchone()[0] == 0


def test_manifest_matches_the_worker():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["id"] == sleep.PLUGIN_ID
    assert manifest["worker"] == {"module": "sleep", "api": sleep.PREFIX, "outboundOrigins": []}
    assert manifest["dataLifecycle"] == {"scope": "account"}
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "sleep")
    assert row["version"] == manifest["version"]
    source = (PACKAGE / "content" / "desktop" / "SleepFeature.cpp").read_text(encoding="utf-8")
    assert sleep.REQUEST_PROOF in source and sleep.PREFIX in source
