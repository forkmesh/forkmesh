import asyncio
import json
import math
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import stretch  # noqa: E402


DAY = "2026-10-02"
ACCOUNTS = {"alice", "bob"}


class Runtime:
    def __init__(self, installed=("alice",)):
        stretch._SCHEMA_READY = False
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
            response = call(self.use("POST", actor=name), "/api/stretch/install")
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
    return asyncio.run(stretch.handle(runtime, path))


def day(offset):
    return stretch._shift_day(DAY, offset)


def overview(runtime, actor="alice", today=DAY):
    return call(runtime.use("GET", actor=actor, query={"day": today}), "/api/stretch")["data"]


def log(runtime, actor="alice", today=DAY, **body):
    body.setdefault("routine", "desk")
    body.setdefault("seconds", 300)
    return call(runtime.use("POST", actor=actor, body=body, query={"day": today}), "/api/stretch/sessions")


def test_install_is_per_account_and_idempotent():
    runtime = Runtime(installed=())
    data = overview(runtime)
    assert data["installed"] is False and data["actor"] == "alice"
    assert [item["id"] for item in data["library"]["routines"]] == ["desk", "lymph", "full"]
    blocked = log(runtime)
    assert blocked["status"] == 409
    assert blocked["data"] == {"error": "plugin_not_installed", "canInstall": True}
    done = call(runtime.use("POST"), "/api/stretch/install")
    assert done["data"] == {"ok": True, "installed": True, "plugin": "stretch"}
    call(runtime.use("POST"), "/api/stretch/install")
    assert runtime.db.execute("SELECT COUNT(*) FROM stretch_profiles").fetchone()[0] == 1
    data = overview(runtime)
    assert data["profile"] == {"voice": True, "routine": "desk", "tzOffsetMinutes": 0}
    assert data["today"] == {"day": DAY, "sessions": 0, "seconds": 0}
    assert data["summary"] == {"streak": 0, "bestStreak": 0, "sessions": 0, "weekSeconds": 0, "weekSessions": 0}
    assert len(data["days"]) == stretch.CHART_DAYS and data["days"][-1]["day"] == DAY
    assert data["recent"] == []
    assert overview(runtime, actor="bob")["installed"] is False


def test_library_is_public_and_every_motion_is_well_formed():
    runtime = Runtime(installed=())
    library = call(runtime.use("GET", actor="mallory"), "/api/stretch/library")["data"]
    assert library["ok"] is True and library["figure"]["width"] == 200
    fields = library["poseFields"]
    motions = library["motions"]
    used = set()
    for routine in library["routines"]:
        assert routine["seconds"] == sum(step["seconds"] for step in routine["steps"])
        for step in routine["steps"]:
            assert step["motion"] in motions, step["id"]
            assert step["say"] and step["title"] and step["seconds"] >= 10
            used.add(step["motion"])
    assert used == set(motions)
    for name, motion in motions.items():
        frames = motion["frames"]
        assert frames[0]["t"] == 0 and frames[-1]["t"] == 1, name
        assert [frame["t"] for frame in frames] == sorted(frame["t"] for frame in frames)
        assert {key: value for key, value in frames[-1].items() if key != "t"} == \
            {key: value for key, value in frames[0].items() if key != "t"}
        assert motion["tap"] in ("", "left", "right", "both") and motion["period"] > 0
        for frame in frames:
            assert set(frame) == {"t", *fields}
            assert all(isinstance(frame[key], (int, float)) for key in fields)
    desk = {step["id"] for step in library["routines"][0]["steps"]}
    assert {"hip-circles", "arm-swings", "side-bend-left", "side-bend-right", "twist-left",
            "twist-right", "neck-rolls"} <= desk
    lymph = {step["id"] for step in library["routines"][1]["steps"]}
    assert {"tap-arm-left", "tap-arm-right", "tap-eyes", "tap-head"} <= lymph


def _hand(side, arm, elbow):
    sx, sy = stretch._shoulder(side)
    sign = -1 if side == "l" else 1
    upper, fore = stretch.FIGURE["upperArm"], stretch.FIGURE["forearm"]
    ex = sx + sign * math.sin(math.radians(arm)) * upper
    ey = sy + math.cos(math.radians(arm)) * upper
    return (ex + sign * math.sin(math.radians(arm + elbow)) * fore,
            ey + math.cos(math.radians(arm + elbow)) * fore)


def test_tapping_hands_reach_their_targets():
    base = stretch.FIGURE["hip"][1] - 150
    for side, target in (("l", (126, 94)), ("r", (74, 94)), ("l", (92, 32)), ("r", (108, 80))):
        arm, elbow = stretch._reach(side, target[0], target[1] + base)
        x, y = _hand(side, arm, elbow)
        assert abs(x - target[0]) < 0.5 and abs(y - target[1] - base) < 0.5
    left = stretch.MOTIONS["tap-arm-left"]
    assert left["tap"] == "right" and all(frame["lArm"] == stretch.HELD_ARM[0] for frame in left["frames"])
    wrist = _hand("l", *stretch.HELD_ARM)
    nearest = min(math.dist(_hand("r", frame["rArm"], frame["rElbow"]), wrist) for frame in left["frames"])
    assert nearest < 8


def test_sessions_today_streak_and_week():
    runtime = Runtime(installed=("alice", "bob"))
    created = log(runtime, routine="lymph", seconds=270, steps=11, stepsTotal=11, completed=True,
                  source="ios", health=True)
    assert created["status"] == 201
    session = created["data"]["session"]
    assert (session["routine"], session["seconds"], session["completed"], session["source"],
            session["health"]) == ("lymph", 270, True, "ios", True)
    for offset in (-1, -2, -4, -5, -6, -20):
        assert log(runtime, day=day(offset), seconds=60)["status"] == 201
    data = overview(runtime)
    assert data["today"] == {"day": DAY, "sessions": 1, "seconds": 270}
    assert data["summary"]["streak"] == 3 and data["summary"]["bestStreak"] == 3
    assert data["summary"]["sessions"] == 7
    assert data["summary"]["weekSessions"] == 6 and data["summary"]["weekSeconds"] == 270 + 5 * 60
    assert data["days"][-1] == {"day": DAY, "sessions": 1, "seconds": 270}
    assert data["recent"][0]["id"] == session["id"]
    assert overview(runtime, today=day(1))["summary"]["streak"] == 3
    assert overview(runtime, today=day(2))["summary"]["streak"] == 0
    stored = runtime.db.execute("SELECT data FROM stretch_sessions WHERE session_id=?", (session["id"],)).fetchone()
    assert "lymph" in stored[0] and not stored[0].startswith("sealed:")
    assert overview(runtime, actor="bob")["summary"]["sessions"] == 0
    assert call(runtime.use("DELETE", actor="bob"), "/api/stretch/sessions/" + session["id"])["status"] == 404
    gone = call(runtime.use("DELETE", query={"day": DAY}), "/api/stretch/sessions/" + session["id"])
    assert gone["data"] == {"ok": True, "deleted": True}
    assert overview(runtime)["today"]["sessions"] == 0


def test_session_validation():
    runtime = Runtime()
    assert log(runtime, routine="yoga")["data"] == {"error": "invalid_routine"}
    assert log(runtime, seconds=0)["data"] == {"error": "invalid_seconds"}
    assert log(runtime, seconds=stretch.MAX_SESSION_SECONDS + 1)["status"] == 400
    assert log(runtime, day=day(1))["data"] == {"error": "invalid_day"}
    odd = log(runtime, steps=500, stepsTotal=3, source="toaster", health="yes")["data"]["session"]
    assert (odd["steps"], odd["stepsTotal"], odd["source"], odd["health"]) == (3, 3, "web", False)


def test_profile_voice_routine_and_time_zone():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={"voice": False, "routine": "full", "tzOffsetMinutes": 300}),
                 "/api/stretch/profile")["data"]["profile"]
    assert saved == {"voice": False, "routine": "full", "tzOffsetMinutes": 300}
    kept = call(runtime.use("PUT", body={"voice": "no", "routine": "yoga"}), "/api/stretch/profile")["data"]
    assert kept["profile"] == saved
    runtime.clock = 1_791_000_000_000
    assert call(runtime.use("GET"), "/api/stretch")["data"]["day"] == stretch._day_from_ms(runtime.clock, 300)


def test_routes_methods_origin_and_session():
    runtime = Runtime()
    assert call(runtime.use("GET"), "/api/stretchy")["status"] == 404
    assert call(runtime.use("GET"), "/api/stretch/nothing")["status"] == 404
    wrong = call(runtime.use("POST"), "/api/stretch")
    assert wrong["status"] == 405 and wrong["headers"]["allow"] == "GET"
    assert call(runtime.use("DELETE"), "/api/stretch/sessions/nope")["status"] == 404
    runtime.origin_ok = False
    assert log(runtime)["status"] == 403
    runtime.origin_ok = True
    assert call(runtime.use("GET", actor="mallory"), "/api/stretch")["status"] == 401


def test_export_uninstall_and_purge():
    runtime = Runtime(installed=("alice", "bob"))
    log(runtime)
    log(runtime, routine="lymph")
    log(runtime, actor="bob")
    exported = call(runtime.use("GET"), "/api/stretch/export")["data"]
    assert exported["filename"] == "forkmesh-stretch-alice.json"
    assert exported["export"]["format"] == "forkmesh-stretch-export-v1"
    assert sorted(item["routine"] for item in exported["export"]["sessions"]) == ["desk", "lymph"]
    gone = call(runtime.use("POST"), "/api/stretch/uninstall")["data"]
    assert gone["uninstalled"] is True and gone["hadData"] is True
    assert len(gone["export"]["sessions"]) == 2
    assert overview(runtime)["installed"] is False
    for table in ("stretch_sessions", "stretch_profiles", "stretch_keys"):
        rows = runtime.db.execute("SELECT account_bi FROM " + table).fetchall()
        assert {row[0] for row in rows} == {"bi:bob"}
    asyncio.run(stretch.purge_account_data(runtime, "bi:bob", "bob"))
    assert runtime.db.execute("SELECT COUNT(*) FROM stretch_sessions").fetchone()[0] == 0


def test_manifest_matches_the_worker():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["id"] == stretch.PLUGIN_ID
    assert manifest["category"] == "Health"
    assert manifest["worker"] == {"module": "stretch", "api": stretch.PREFIX, "outboundOrigins": []}
    assert manifest["dataLifecycle"] == {"scope": "account"}
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "stretch")
    assert row["version"] == manifest["version"]
    source = (PACKAGE / "content" / "desktop" / "StretchFeature.cpp").read_text(encoding="utf-8")
    assert stretch.REQUEST_PROOF in source and stretch.PREFIX in source
    script = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
    assert stretch.PREFIX in script and "speechSynthesis" in script
