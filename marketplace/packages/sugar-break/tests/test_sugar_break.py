import asyncio
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import sugar_break  # noqa: E402


DAY = "2026-10-02"
ACCOUNTS = {"alice", "bob"}


class Runtime:
    def __init__(self, installed=("alice",)):
        sugar_break._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.query = {}
        self.clock = 1_791_000_000_000
        self.ids = 0
        self.secrets = 0
        self.audits = []
        self.origin_ok = True
        for name in installed:
            response = call(self.use("POST", actor=name), "/api/sugar-break/install")
            assert response["status"] == 200, response
        self.use("GET")

    def use(self, method, actor="alice", body=None, query=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def query_params(self): return dict(self.query)
    def same_origin(self): return self.origin_ok
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
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
    return asyncio.run(sugar_break.handle(runtime, path))


def day(offset):
    return sugar_break._shift_day(DAY, offset)


def overview(runtime, actor="alice", today=DAY):
    return call(runtime.use("GET", actor=actor, query={"day": today}), "/api/sugar-break")["data"]


def start(runtime, actor="alice", today=DAY, **body):
    response = call(runtime.use("POST", actor=actor, body=body, query={"day": today}),
                    "/api/sugar-break/breaks")
    assert response["status"] == 201, response
    return response["data"]["break"]


def log(runtime, kind="skip", actor="alice", today=DAY, **body):
    return call(runtime.use("POST", actor=actor, body=dict(body, kind=kind), query={"day": today}),
                "/api/sugar-break/events")


def test_install_is_per_account_and_idempotent():
    runtime = Runtime(installed=())
    assert overview(runtime) == {"ok": True, "installed": False, "actor": "alice"}
    blocked = log(runtime)
    assert blocked["status"] == 409
    assert blocked["data"] == {"error": "plugin_not_installed", "canInstall": True}
    done = call(runtime.use("POST"), "/api/sugar-break/install")
    assert done["data"] == {"ok": True, "installed": True, "plugin": "sugar-break"}
    call(runtime.use("POST"), "/api/sugar-break/install")
    assert runtime.db.execute("SELECT COUNT(*) FROM sugar_break_profiles").fetchone()[0] == 1
    data = overview(runtime)
    assert data["current"] is None and data["days"] == [] and data["history"] == []
    assert data["profile"] == {"defaultLengthDays": 30, "tzOffsetMinutes": 0}
    assert data["lengths"] == [7, 14, 21, 30, 60, 90]
    assert data["today"] == {"day": DAY, "skips": 0, "slips": 0, "status": "open", "inBreak": False}
    assert overview(runtime, actor="bob")["installed"] is False


def test_a_break_lasts_one_month_by_default():
    runtime = Runtime()
    item = start(runtime)
    assert (item["startDay"], item["endDay"], item["lengthDays"]) == (DAY, day(29), 30)
    assert item["status"] == "active" and item["endedAt"] == 0
    data = overview(runtime)
    assert data["current"]["id"] == item["id"]
    assert len(data["days"]) == 30 and data["days"][0] == {"day": DAY, "status": "open", "skips": 0, "slips": 0}
    assert {entry["status"] for entry in data["days"][1:]} == {"upcoming"}
    assert data["today"]["inBreak"] is True
    summary = data["summary"]
    assert (summary["dayNumber"], summary["lengthDays"], summary["remainingDays"]) == (1, 30, 29)
    short = start(runtime, lengthDays=7)
    assert short["lengthDays"] == 7 and short["endDay"] == day(6)
    assert start(runtime, lengthDays=5000)["lengthDays"] == sugar_break.MAX_LENGTH_DAYS


def test_plus_one_counts_every_skip_and_a_slip_marks_the_day():
    runtime = Runtime(installed=("alice", "bob"))
    start(runtime)
    for _ in range(3):
        created = log(runtime)
        assert created["status"] == 201 and created["data"]["event"]["kind"] == "skip"
    data = overview(runtime)
    assert data["today"]["skips"] == 3 and data["today"]["status"] == "sugar-free"
    assert data["summary"]["skips"] == 3 and data["summary"]["sugarFreeDays"] == 1
    assert data["summary"]["streak"] == 1 and data["summary"]["percent"] == 3
    assert [event["kind"] for event in data["events"]] == ["skip", "skip", "skip"]
    slip = log(runtime, "slip", note="birthday cake")["data"]["event"]
    data = overview(runtime)
    assert data["today"]["status"] == "slip" and data["summary"]["slipDays"] == 1
    assert data["summary"]["sugarFreeDays"] == 0 and data["summary"]["streak"] == 0
    stored = runtime.db.execute("SELECT data FROM sugar_break_events WHERE event_id=?", (slip["id"],)).fetchone()
    assert "birthday cake" in stored[0] and not stored[0].startswith("sealed:")
    undone = call(runtime.use("DELETE", query={"day": DAY}), "/api/sugar-break/events/" + slip["id"])
    assert undone["data"] == {"ok": True, "deleted": True}
    assert overview(runtime)["today"]["status"] == "sugar-free"
    assert call(runtime.use("DELETE", actor="bob"), "/api/sugar-break/events/" + slip["id"])["status"] == 404
    assert overview(runtime, actor="bob")["current"] is None


def test_streaks_progress_and_days_outside_the_break():
    runtime = Runtime()
    start(runtime, startDay=day(-5), lengthDays=10)
    for offset in (-5, -4, -2, -1):
        assert log(runtime, day=day(offset))["status"] == 201
    log(runtime, "slip", day=day(-3))
    data = overview(runtime)
    statuses = [entry["status"] for entry in data["days"][:7]]
    assert statuses == ["sugar-free", "sugar-free", "slip", "sugar-free", "sugar-free", "open", "upcoming"]
    summary = data["summary"]
    assert summary["streak"] == 2 and summary["bestStreak"] == 2
    assert summary["dayNumber"] == 6 and summary["remainingDays"] == 4
    assert summary["sugarFreeDays"] == 4 and summary["percent"] == 40 and summary["openDays"] == 1
    assert log(runtime, day=day(1))["data"] == {"error": "day_outside_break"}
    assert log(runtime, day=day(-6))["status"] == 400
    assert log(runtime, "cake")["data"] == {"error": "invalid_kind"}
    assert overview(runtime, today=day(1))["summary"]["streak"] == 0


def test_completed_and_ended_breaks_move_to_history():
    runtime = Runtime()
    first = start(runtime, startDay=day(-10), lengthDays=7)
    assert log(runtime, today=day(-9))["status"] == 201
    assert log(runtime)["data"] == {"error": "no_active_break"}
    data = overview(runtime)
    assert data["current"]["status"] == "completed" and data["today"]["inBreak"] is False
    assert data["summary"]["remainingDays"] == 0 and data["summary"]["sugarFreeDays"] == 1
    second = start(runtime)
    log(runtime)
    ended = call(runtime.use("PATCH", body={"end": True}, query={"day": DAY}),
                 "/api/sugar-break/breaks/" + second["id"])["data"]["break"]
    assert ended["status"] == "ended" and ended["endedDay"] == DAY and ended["endedAt"] > 0
    third = start(runtime, lengthDays=14)
    data = overview(runtime)
    assert data["current"]["id"] == third["id"]
    assert [item["id"] for item in data["history"]] == [second["id"], first["id"]]
    assert data["history"][0]["summary"]["sugarFreeDays"] == 1
    fourth = start(runtime)
    assert overview(runtime)["history"][0]["status"] == "ended"
    assert overview(runtime)["history"][0]["id"] == third["id"]
    removed = call(runtime.use("DELETE"), "/api/sugar-break/breaks/" + second["id"])
    assert removed["data"] == {"ok": True, "deleted": True}
    assert runtime.db.execute("SELECT COUNT(*) FROM sugar_break_events WHERE break_id=?",
                              (second["id"],)).fetchone()[0] == 0
    assert overview(runtime)["current"]["id"] == fourth["id"]
    longer = call(runtime.use("PATCH", body={"lengthDays": 60}), "/api/sugar-break/breaks/" + fourth["id"])
    assert longer["data"]["break"]["endDay"] == day(59)


def test_profile_default_length_and_time_zone():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={"defaultLengthDays": 21, "tzOffsetMinutes": 300}),
                 "/api/sugar-break/profile")["data"]["profile"]
    assert saved == {"defaultLengthDays": 21, "tzOffsetMinutes": 300}
    assert start(runtime)["lengthDays"] == 21
    bad = call(runtime.use("PUT", body={"defaultLengthDays": "x"}), "/api/sugar-break/profile")["data"]
    assert bad["profile"]["defaultLengthDays"] == 21
    runtime.clock = 1_791_000_000_000
    assert call(runtime.use("GET"), "/api/sugar-break")["data"]["day"] == sugar_break._day_from_ms(
        runtime.clock, 300)


def test_routes_methods_origin_and_session():
    runtime = Runtime()
    assert call(runtime.use("GET"), "/api/sugar-breakx")["status"] == 404
    assert call(runtime.use("GET"), "/api/sugar-break/nothing")["status"] == 404
    wrong = call(runtime.use("POST"), "/api/sugar-break")
    assert wrong["status"] == 405 and wrong["headers"]["allow"] == "GET"
    assert call(runtime.use("PATCH", body={}), "/api/sugar-break/breaks/nope")["status"] == 404
    assert call(runtime.use("DELETE"), "/api/sugar-break/events/nope")["status"] == 404
    runtime.origin_ok = False
    assert log(runtime)["status"] == 403
    runtime.origin_ok = True
    assert call(runtime.use("GET", actor="mallory"), "/api/sugar-break")["status"] == 401


def test_export_uninstall_and_purge():
    runtime = Runtime(installed=("alice", "bob"))
    start(runtime)
    log(runtime)
    log(runtime, "slip")
    start(runtime, actor="bob")
    exported = call(runtime.use("GET"), "/api/sugar-break/export")["data"]
    assert exported["filename"] == "forkmesh-sugar-break-alice.json"
    assert exported["export"]["format"] == "forkmesh-sugar-break-export-v1"
    assert [event["kind"] for event in exported["export"]["breaks"][0]["events"]] == ["skip", "slip"]
    gone = call(runtime.use("POST"), "/api/sugar-break/uninstall")["data"]
    assert gone["uninstalled"] is True and gone["hadData"] is True
    assert len(gone["export"]["breaks"]) == 1
    assert overview(runtime)["installed"] is False
    for table in ("sugar_break_breaks", "sugar_break_profiles", "sugar_break_keys"):
        rows = runtime.db.execute("SELECT account_bi FROM " + table).fetchall()
        assert {row[0] for row in rows} == {"bi:bob"}
    asyncio.run(sugar_break.purge_account_data(runtime, "bi:bob", "bob"))
    assert runtime.db.execute("SELECT COUNT(*) FROM sugar_break_breaks").fetchone()[0] == 0


def test_manifest_matches_the_worker():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["id"] == sugar_break.PLUGIN_ID
    assert manifest["category"] == "Health"
    assert manifest["worker"] == {"module": "sugar_break", "api": sugar_break.PREFIX, "outboundOrigins": []}
    assert manifest["dataLifecycle"] == {"scope": "account"}
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "sugar-break")
    assert row["version"] == manifest["version"]
    source = (PACKAGE / "content" / "desktop" / "SugarBreakFeature.cpp").read_text(encoding="utf-8")
    assert sugar_break.REQUEST_PROOF in source and sugar_break.PREFIX in source
