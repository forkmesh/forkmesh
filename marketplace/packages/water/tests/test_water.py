import asyncio
import hashlib
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import water  # noqa: E402


DAY = "2026-09-16"
ACCOUNTS = {"alice", "bob"}


class Runtime:
    def __init__(self, installed=("alice",), country=""):
        water._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.actor = "alice"
        self.country_value = country
        self.method_value = "GET"
        self.body = {}
        self.query = {}
        self.clock = 1_789_561_860_000
        self.ids = 0
        self.secrets = 0
        self.audits = []
        for name in installed:
            response = call(self.use("POST", actor=name), "/api/water/install")
            assert response["status"] == 200, response
        self.use("GET")

    def use(self, method, actor="alice", body=None, query=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def country(self): return self.country_value
    def query_params(self): return dict(self.query)
    def same_origin(self): return True
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
    async def blind(self, value):
        return "blind:" + hashlib.sha256(str(value).encode()).hexdigest()[:48]
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append(args)


def call(runtime, path):
    return asyncio.run(water.handle(runtime, path))


def drink(runtime, amount, actor="alice", day=DAY, **extra):
    response = call(runtime.use("POST", actor=actor, body=dict(day=day, amountMl=amount, **extra)),
                    "/api/water/entries")
    assert response["status"] in (200, 201), response
    return response["data"]


def overview(runtime, actor="alice", day=DAY):
    return call(runtime.use("GET", actor=actor, query={"day": day}), "/api/water")["data"]


def test_install_and_empty_overview():
    runtime = Runtime(installed=())
    assert overview(runtime) == {"ok": True, "installed": False, "actor": "alice"}
    assert call(runtime.use("POST", body={"amountMl": 250}), "/api/water/entries")["status"] == 409
    call(runtime.use("POST", body={"goalMl": 2500}), "/api/water/install")
    data = overview(runtime)
    assert data["installed"] is True and data["profile"]["goalMl"] == 2500
    assert data["today"] == {"day": DAY, "totalMl": 0, "entries": 0}
    assert data["profile"]["presetsMl"] == [250, 500, 750]
    assert len(data["days"]) == 30


def test_entries_total_seal_and_stay_private():
    runtime = Runtime(installed=("alice", "bob"))
    first = drink(runtime, 250, note="morning glass")
    assert first["entry"]["amountMl"] == 250 and first["entry"]["drink"] == "water"
    drink(runtime, 500, drink="tea")
    drink(runtime, 330, day="2026-09-15")
    drink(runtime, 1000, actor="bob")
    data = overview(runtime)
    assert data["today"] == {"day": DAY, "totalMl": 750, "entries": 2}
    assert [entry["amountMl"] for entry in data["entries"]] == [250, 500]
    assert data["days"][-2]["totalMl"] == 330
    stored = runtime.db.execute("SELECT * FROM water_entries WHERE entry_id=?",
                                (first["entry"]["id"],)).fetchone()
    assert "morning glass" in stored["data"] and not stored["data"].startswith("sealed:")
    assert stored["external"] == ""
    assert overview(runtime, actor="bob")["today"]["totalMl"] == 1000


def test_update_and_delete_entries():
    runtime = Runtime(installed=("alice", "bob"))
    entry = drink(runtime, 250)["entry"]
    updated = call(runtime.use("PATCH", body={"amountMl": 400, "drink": "juice"}),
                   "/api/water/entries/" + entry["id"])["data"]["entry"]
    assert (updated["amountMl"], updated["drink"], updated["source"]) == (400, "juice", "manual")
    assert call(runtime.use("PATCH", actor="bob", body={"amountMl": 1}),
                "/api/water/entries/" + entry["id"])["status"] == 404
    assert call(runtime.use("PATCH", body={"amountMl": 0}),
                "/api/water/entries/" + entry["id"])["status"] == 400
    assert call(runtime.use("DELETE"), "/api/water/entries/" + entry["id"])["data"]["deleted"] is True
    assert overview(runtime)["today"]["totalMl"] == 0
    assert call(runtime.use("GET"), "/api/water/entries/nothex")["status"] == 404


def test_health_ids_deduplicate_entries_and_imports():
    runtime = Runtime()
    created = drink(runtime, 300, source="health", externalId="8F1C-AA")
    assert created["duplicate"] is False and created["entry"]["externalId"] == "8F1C-AA"
    again = drink(runtime, 300, source="health", externalId="8F1C-AA")
    assert again["duplicate"] is True and again["entry"]["id"] == created["entry"]["id"]
    imported = call(runtime.use("POST", body={"entries": [
        {"day": DAY, "amountMl": 200, "externalId": "8F1C-AA"},
        {"day": DAY, "amountMl": 350, "externalId": "B2", "at": 1_789_560_000_000},
        {"day": DAY, "amountMl": 350},
        {"day": DAY, "amountMl": 0, "externalId": "C3"},
        "junk",
    ]}), "/api/water/import")["data"]
    assert (imported["imported"], imported["skipped"]) == (1, 4)
    assert imported["entries"][0]["source"] == "health"
    assert overview(runtime)["today"]["totalMl"] == 650
    stored = runtime.db.execute("SELECT external FROM water_entries WHERE external!=''").fetchall()
    assert len(stored) == 2 and all("8F1C" not in row[0] for row in stored)


def test_validation_limits_and_units():
    runtime = Runtime(country="US")
    assert call(runtime.use("POST", body={"amountMl": -1}), "/api/water/entries")["status"] == 400
    big = drink(runtime, 99999)["entry"]
    assert big["amountMl"] == water.MAX_AMOUNT_ML
    profile = overview(runtime)["profile"]
    assert profile["units"] == "imperial" and profile["presetsMl"] == [237, 473, 710]
    call(runtime.use("PUT", body={"unitsPreference": "metric", "goalMl": 99999}), "/api/water/profile")
    profile = overview(runtime)["profile"]
    assert profile["units"] == "metric" and profile["goalMl"] == water.MAX_GOAL_ML
    call(runtime.use("PUT", body={"unitsPreference": "Cups"}), "/api/water/profile")
    profile = overview(runtime)["profile"]
    assert profile["unitsPreference"] == profile["units"] == "cups" and profile["presetsMl"] == [237, 473, 710]
    many = {"entries": [{"amountMl": 1, "externalId": str(index)} for index in range(201)]}
    assert call(runtime.use("POST", body=many), "/api/water/import")["status"] == 400


def test_goal_streak_and_summary():
    runtime = Runtime()
    call(runtime.use("PUT", body={"goalMl": 1000}), "/api/water/profile")
    for day, amount in (("2026-09-13", 1200), ("2026-09-14", 1000), ("2026-09-15", 1500), (DAY, 200)):
        drink(runtime, amount, day=day)
    summary = overview(runtime)["summary"]
    assert summary == {"averageMl": 975, "goalDays": 3, "streak": 3}


def test_range_export_uninstall_and_purge():
    runtime = Runtime(installed=("alice", "bob"))
    drink(runtime, 250)
    drink(runtime, 500, day="2026-09-12")
    drink(runtime, 100, actor="bob")
    ranged = call(runtime.use("GET", query={"from": "2026-09-12", "to": DAY}), "/api/water/entries")["data"]
    assert [item["totalMl"] for item in ranged["days"]] == [500, 0, 0, 0, 250]
    exported = call(runtime.use("GET"), "/api/water/export")["data"]
    assert exported["filename"] == "forkmesh-water-alice.json"
    assert len(exported["export"]["entries"]) == 2
    gone = call(runtime.use("POST"), "/api/water/uninstall")["data"]
    assert gone["hadData"] is True and len(gone["export"]["entries"]) == 2
    assert overview(runtime)["installed"] is False
    assert asyncio.run(water.account_installed(runtime, "bi:bob")) is True
    asyncio.run(water.purge_account_data(runtime, "bi:bob", "bob"))
    for table in ("water_entries", "water_profiles", "water_keys"):
        assert runtime.db.execute("SELECT COUNT(*) FROM " + table).fetchone()[0] == 0


def test_manifest_matches_the_worker():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["id"] == water.PLUGIN_ID
    assert manifest["worker"] == {"module": "water", "api": water.PREFIX, "outboundOrigins": []}
    assert manifest["dataLifecycle"] == {"scope": "account"}
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "water")
    assert row["version"] == manifest["version"]
    source = (PACKAGE / "content" / "desktop" / "WaterFeature.cpp").read_text(encoding="utf-8")
    assert water.REQUEST_PROOF in source and water.PREFIX in source
