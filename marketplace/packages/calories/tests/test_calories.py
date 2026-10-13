"""Calorie Counter package API, schema, reminders, challenges, and contracts."""

import asyncio
import hashlib
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
APP = REPO / "app"
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
sys.path.insert(0, str(APP / "src"))
sys.path.insert(0, str(APP / "tools"))
import calories  # noqa: E402


DAY = "2026-09-16"
YESTERDAY = "2026-09-15"
ACCOUNTS = {"alice", "charlie", "dana", "eve"}


class Runtime:
    def __init__(self, installed=("alice", "charlie"), ai=True, country=""):
        calories._SCHEMA_READY = False
        calories._LEGACY_SEALED_DONE = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
        self.db.executescript(
            sorted((APP / "migrations").glob("0*_org_plugin_installs.sql"))[-1]
            .read_text(encoding="utf-8")
        )
        self.actor = "alice"
        self.country_value = country
        self.method_value = "GET"
        self.body = {}
        self.query = {}
        # 2026-09-16T12:31:00Z
        self.clock = 1_789_561_860_000
        self.ids = 0
        self.secrets = 0
        self.audits = []
        self.pings = []
        self.outbound_calls = []
        self.outbound = {}
        self.ai_calls = []
        self.ai_result = {"ok": True, "model": "@cf/test/vision", "data": {
            "items": [
                {"name": "Grilled chicken breast", "portion": "150 g", "calories": 248,
                 "proteinG": 46, "carbsG": 0, "fatG": 5.4, "confidence": 0.8},
                {"name": "Steamed rice", "portion": "1 cup", "calories": 205,
                 "proteinG": 4.3, "carbsG": 45, "fatG": 0.4, "confidence": 0.7},
                {"name": "", "calories": 999},
            ],
            "summary": "Chicken and rice plate.",
        }}
        if not ai:
            self.ai_json = None
        for name in installed:
            response = call(self.use("POST", actor=name), "/api/calories/install")
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
    async def account_exists(self, name): return name in ACCOUNTS
    # Stand-ins for the host's WebCrypto: "sealed:" is DATA_KEY, and a
    # "sealed[<key>]:" prefix names the exact key material a row was written
    # with, so a test can prove one account's key never opens another's row.
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        if not text.startswith("sealed:"):
            return None
        return json.loads(text.removeprefix("sealed:"))
    def new_secret(self):
        self.secrets += 1
        return "key-%032x" % self.secrets
    async def seal_with(self, secret, value):
        assert len(str(secret)) >= 32, secret
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
    async def audit(self, *args, **kwargs): self.audits.append((args, kwargs))
    async def notify(self, recipient, kind, title, **kwargs):
        self.pings.append(dict(kwargs, recipient=recipient, kind=kind, title=title))
        return True
    async def outbound_json(self, url, method="GET", headers=None, body=""):
        self.outbound_calls.append((url, method, dict(headers or {}), body))
        for prefix, payload in self.outbound.items():
            if url.startswith(prefix):
                return {"ok": True, "status": 200, "data": payload}
        return {"ok": True, "status": 404, "data": {"status": 0}}
    async def ai_json(self, account_bi, system_prompt, user_prompt, schema,
                      image_data_url="", max_tokens=700):
        self.ai_calls.append((account_bi, user_prompt, bool(image_data_url), max_tokens))
        return self.ai_result


def call(runtime, path):
    return asyncio.run(calories.handle(runtime, path))


def log(runtime, actor, day, name, kcal, protein=0, carbs=0, fat=0, **extra):
    body = dict(day=day, name=name, calories=kcal, proteinG=protein, carbsG=carbs,
                fatG=fat, **extra)
    response = call(runtime.use("POST", actor=actor, body=body), "/api/calories/entries")
    assert response["status"] == 201, response
    return response["data"]["entry"]


def diary_key(runtime, account_bi):
    """The account's diary key, as the host would unwrap it."""
    row = runtime.db.execute(
        "SELECT wrapped FROM calories_keys WHERE account_bi=?", (account_bi,)).fetchone()
    return json.loads(row["wrapped"].removeprefix("sealed:"))["key"] if row else ""


def sealed_for(runtime, account_bi, stored):
    """The row's contents when opened with that account's key, else None."""
    return asyncio.run(runtime.open_with(diary_key(runtime, account_bi), stored))


def count(runtime, table, where="1=1", *args):
    return runtime.db.execute("SELECT COUNT(*) FROM " + table + " WHERE " + where, args).fetchone()[0]


def test_any_account_installs_for_itself_without_an_organization():
    runtime = Runtime(installed=())
    before = call(runtime.use("GET", actor="dana"), "/api/calories")
    assert before["status"] == 200
    assert before["data"] == {"ok": True, "installed": False, "actor": "dana"}
    blocked = call(runtime.use("POST", actor="dana", body={"day": DAY, "name": "x", "calories": 1}),
                   "/api/calories/entries")
    assert blocked["status"] == 409
    assert blocked["data"] == {"error": "plugin_not_installed", "canInstall": True}

    installed = call(runtime.use("POST", actor="dana", body={"tzOffsetMinutes": 300}),
                     "/api/calories/install")
    assert installed["data"] == {"ok": True, "installed": True, "plugin": "calories"}
    again = call(runtime.use("POST", actor="dana"), "/api/calories/install")
    assert again["status"] == 200 and count(runtime, "calories_vault_profiles") == 1
    overview = call(runtime.use("GET", actor="dana", query={"day": DAY}), "/api/calories")["data"]
    assert overview["installed"] is True and overview["actor"] == "dana"
    assert overview["profile"]["tzOffsetMinutes"] == 300
    for key in ("organization", "role", "members", "canManagePlugin"):
        assert key not in overview
    # Organizations never enter into it: no org tables, no org install rows.
    assert count(runtime, "org_plugin_installs") == 0
    assert ("dana", "plugin.install", "account_plugin", "dana:calories") == runtime.audits[0][0]


def test_diary_entries_are_private_sealed_and_summarised():
    runtime = Runtime()
    entry = log(runtime, "alice", DAY, "Oatmeal with berries", 320, 12, 55, 6,
                meal="breakfast", portion="1 bowl", notes="secret craving")
    assert (entry["meal"], entry["source"], entry["calories"]) == ("breakfast", "manual", 320)
    # The row keeps only what a query needs to find it again; the food, the
    # meal, and the numbers are inside an envelope only Alice's key opens.
    stored = runtime.db.execute("SELECT * FROM calories_vault_entries").fetchone()
    assert set(stored.keys()) == {"entry_id", "account_bi", "day", "logged_at", "data"}
    assert not stored["data"].startswith("sealed:")
    assert sealed_for(runtime, "bi:charlie", stored["data"]) is None
    opened = sealed_for(runtime, "bi:alice", stored["data"])
    assert opened["name"] == "Oatmeal with berries" and opened["calories"] == 320
    assert opened["meal"] == "breakfast" and opened["notes"] == "secret craving"
    log(runtime, "alice", DAY, "Salad", 180, 4, 12, 11, meal="lunch")
    log(runtime, "alice", YESTERDAY, "Pasta", 700, 20, 90, 22, meal="dinner")
    log(runtime, "charlie", DAY, "Charlie's burger", 900, meal="dinner")

    overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    assert overview["todayTotals"]["calories"] == 500
    # The overview opens on the whole diary, newest first, not on one day.
    assert [item["name"] for item in overview["entries"]] == [
        "Salad", "Oatmeal with berries", "Pasta"]
    assert overview["entriesTruncated"] is False
    # Clients that predate the whole-diary list still read the day-scoped one.
    assert [item["name"] for item in overview["entriesToday"]] == ["Salad", "Oatmeal with berries"]
    assert len(overview["days"]) == 30 and overview["days"][-1]["day"] == DAY
    assert overview["days"][-2]["calories"] == 700
    assert overview["streaks"] == {"logged": 2, "onGoal": 2}
    assert overview["sources"] == {"openFoodFacts": True, "usda": False, "ai": True}

    # Charlie never sees Alice's entries and cannot touch them by id.
    theirs = call(runtime.use("GET", actor="charlie", query={"day": DAY}), "/api/calories")["data"]
    assert [item["name"] for item in theirs["entries"]] == ["Charlie's burger"]
    forbidden = call(runtime.use("DELETE", actor="charlie"), f"/api/calories/entries/{entry['id']}")
    assert forbidden["status"] == 404
    peek = call(runtime.use("GET", actor="charlie"), f"/api/calories/entries/{entry['id']}")
    assert peek["status"] == 404

    updated = call(runtime.use("PATCH", body={"calories": 350, "meal": "snack"}),
                   f"/api/calories/entries/{entry['id']}")
    assert (updated["data"]["entry"]["calories"], updated["data"]["entry"]["meal"]) == (350, "snack")
    assert updated["data"]["entry"]["notes"] == "secret craving"
    deleted = call(runtime.use("DELETE"), f"/api/calories/entries/{entry['id']}")
    assert deleted["data"]["deleted"] is True
    history = call(runtime.use("GET", query={"from": YESTERDAY, "to": DAY}), "/api/calories/entries")["data"]
    assert [item["name"] for item in history["entries"]] == ["Salad", "Pasta"]


def test_overview_lists_every_logged_day_and_says_when_it_is_capped():
    runtime = Runtime()
    # Older than the 30-day chart window and newer than today: both belong on
    # the diary page, which is not scoped to a day at all.
    log(runtime, "alice", "2026-01-04", "New year roast", 800, meal="dinner")
    log(runtime, "alice", YESTERDAY, "Pasta", 700, meal="dinner")
    log(runtime, "alice", DAY, "Salad", 180, meal="lunch")
    log(runtime, "alice", "2026-09-17", "Tomorrow's porridge", 300, meal="breakfast")

    overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    assert [item["name"] for item in overview["entries"]] == [
        "Tomorrow's porridge", "Salad", "Pasta", "New year roast"]
    assert overview["entriesTruncated"] is False
    assert [item["name"] for item in overview["entriesToday"]] == ["Salad"]

    capped = calories.MAX_OVERVIEW_ENTRIES
    calories.MAX_OVERVIEW_ENTRIES = 2
    try:
        overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    finally:
        calories.MAX_OVERVIEW_ENTRIES = capped
    assert [item["name"] for item in overview["entries"]] == [
        "Tomorrow's porridge", "Salad"]
    assert overview["entriesTruncated"] is True


def test_goals_reminders_and_weights_round_trip():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={
        "dailyCalories": 1800, "proteinG": 120, "units": "imperial", "targetWeightKg": 72.5,
        "tzOffsetMinutes": -120,
        "reminders": [
            {"time": "12:30", "label": "Log lunch", "onlyIfUnlogged": True},
            {"time": "12:30", "label": "duplicate"},
            {"time": "25:00", "label": "invalid"},
            {"time": "19:00", "enabled": False},
        ],
    }), "/api/calories/profile")["data"]["profile"]
    assert (saved["dailyCalories"], saved["units"], saved["targetWeightKg"]) == (1800, "imperial", 72.5)
    assert [item["time"] for item in saved["reminders"]] == ["12:30", "19:00"]
    assert saved["reminders"][0]["onlyIfUnlogged"] is True and saved["reminders"][1]["enabled"] is False
    row = runtime.db.execute(
        "SELECT reminders_on,data FROM calories_vault_profiles WHERE account_bi='bi:alice'"
    ).fetchone()
    # Only the cron's "has any reminder" flag stays in the clear: the times,
    # the labels, the goal, and the time zone are all inside the envelope.
    assert row["reminders_on"] == 1
    sealed = sealed_for(runtime, "bi:alice", row["data"])
    assert sealed["dailyCalories"] == 1800 and sealed["tzOffsetMinutes"] == -120
    assert sealed["reminders"][0]["label"] == "Log lunch"
    fetched = call(runtime.use("GET"), "/api/calories/profile")["data"]
    assert fetched["profile"] == saved and fetched["updatedAt"] > 0

    weight = call(runtime.use("PUT", body={"weightKg": 81.3}), f"/api/calories/weights/{YESTERDAY}")
    assert weight["data"]["weight"] == {"day": YESTERDAY, "weightKg": 81.3}
    call(runtime.use("PUT", body={"weightKg": 80.9}), f"/api/calories/weights/{DAY}")
    bad = call(runtime.use("PUT", body={"weightKg": 0}), f"/api/calories/weights/{DAY}")
    assert bad["status"] == 400
    overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    assert overview["latestWeight"] == {"weightKg": 80.9, "day": DAY}
    assert [item["weightKg"] for item in overview["weights"]] == [81.3, 80.9]
    assert call(runtime.use("GET", actor="charlie", query={"day": DAY}),
                "/api/calories")["data"]["weights"] == []
    removed = call(runtime.use("DELETE"), f"/api/calories/weights/{DAY}")
    assert removed["data"]["deleted"] is True


def test_reminders_fire_once_per_local_day_and_respect_unlogged_flag():
    runtime = Runtime()
    # Alice's local time is UTC+2 (offset -120): 12:31Z is 14:31 local.
    call(runtime.use("PUT", body={"tzOffsetMinutes": -120, "reminders": [
        {"time": "14:30", "label": "Log lunch"},
        {"time": "14:29", "label": "Only if empty", "onlyIfUnlogged": True},
        {"time": "09:00", "label": "Breakfast"},
    ]}), "/api/calories/profile")
    # Charlie is at UTC (offset 0): 12:31Z with a 12:30 reminder.
    call(runtime.use("PUT", actor="charlie", body={"tzOffsetMinutes": 0, "reminders": [
        {"time": "12:30", "label": "Charlie lunch", "onlyIfUnlogged": True}]}), "/api/calories/profile")
    log(runtime, "alice", DAY, "Snack", 100)

    result = asyncio.run(calories.deliver_reminders(runtime))
    assert result == {"ok": True, "delivered": 2}
    assert sorted((ping["recipient"], ping["title"]) for ping in runtime.pings) == [
        ("alice", "Log lunch"), ("charlie", "Charlie lunch")]
    assert runtime.pings[0]["kind"] == "plugin_activity"
    assert runtime.pings[0]["href"] == "/dashboard/calories"
    assert runtime.pings[0]["dedupe"].startswith("calories:reminder:blind:")
    # The ledger names neither the day nor the time it fired.
    ledger = runtime.db.execute(
        "SELECT reminder_key FROM calories_reminder_deliveries").fetchall()
    assert ledger and all(row[0].startswith("blind:") for row in ledger)
    assert not any(DAY in row[0] or "14:30" in row[0] for row in ledger)
    # A second sweep inside the same minute delivers nothing new.
    assert asyncio.run(calories.deliver_reminders(runtime))["delivered"] == 0
    assert len(runtime.pings) == 2


def test_photo_analysis_uses_host_ai_and_returns_editable_suggestions():
    runtime = Runtime()
    image = "data:image/jpeg;base64," + "QUJD" * 100
    result = call(runtime.use("POST", body={"image": image, "hint": "half portion"}),
                  "/api/calories/analyze")
    assert result["status"] == 200, result
    assert result["data"]["source"] == "photo"
    assert [item["name"] for item in result["data"]["items"]] == [
        "Grilled chicken breast", "Steamed rice"]
    assert result["data"]["totalCalories"] == 453
    assert runtime.ai_calls[0][0] == "bi:alice" and runtime.ai_calls[0][2] is True
    assert "<hint>half portion</hint>" in runtime.ai_calls[0][1]
    assert count(runtime, "calories_vault_entries") == 0

    text = call(runtime.use("POST", body={"description": "two eggs on toast"}), "/api/calories/analyze")
    assert text["data"]["source"] == "text" and runtime.ai_calls[1][2] is False

    bad = call(runtime.use("POST", body={"image": "data:text/html;base64,QUJD"}), "/api/calories/analyze")
    assert bad["status"] == 400 and bad["data"]["error"] == "invalid_image"
    runtime.ai_result = {"ok": False, "error": "rate_limited", "retryAfterMs": 60000}
    limited = call(runtime.use("POST", body={"description": "toast"}), "/api/calories/analyze")
    assert limited["status"] == 429 and limited["data"]["retryAfterMs"] == 60000
    without = Runtime(ai=False)
    missing = call(without.use("POST", body={"description": "toast"}), "/api/calories/analyze")
    assert missing["status"] == 503


def test_quick_add_lists_common_foods_and_keeps_a_sealed_personal_list():
    runtime = Runtime()
    overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    assert overview["quickFoods"] == []
    common = {item["name"]: item for item in overview["commonFoods"]}
    assert len(common) >= 20 and common["Banana"]["calories"] == 105
    assert all(item["portion"] and item["category"] for item in common.values())

    saved = call(runtime.use("PUT", body={"foods": [
        {"name": "  Oat   milk latte ", "portion": "16 oz", "calories": "180", "proteinG": 3},
        {"name": "oat milk latte", "calories": 1},
        {"name": "", "calories": 5},
        "junk",
    ]}), "/api/calories/quick-foods")
    assert saved["status"] == 200, saved
    assert saved["data"]["quickFoods"] == [{
        "name": "Oat milk latte", "brand": "", "portion": "16 oz", "calories": 180,
        "proteinG": 3.0, "carbsG": 0.0, "fatG": 0.0, "source": "saved"}]
    row = runtime.db.execute(
        "SELECT data FROM calories_vault_settings WHERE account_bi='bi:alice'").fetchone()
    assert sealed_for(runtime, "bi:alice", row["data"])["quickFoods"][0]["name"] == "Oat milk latte"
    other = call(runtime.use("GET", actor="charlie", query={"day": DAY}), "/api/calories")["data"]
    assert other["quickFoods"] == []

    quick = log(runtime, "alice", DAY, "Banana", 105, 1.3, 27, 0.4, source="quick")
    assert quick["source"] == "quick"
    exported = call(runtime.use("GET"), "/api/calories/export")["data"]["export"]
    assert exported["quickFoods"][0]["name"] == "Oat milk latte"


def test_receipt_photos_fill_the_personal_quick_add_list():
    runtime = Runtime()
    call(runtime.use("PUT", body={"foods": [{"name": "Greek yogurt", "calories": 100}]}),
         "/api/calories/quick-foods")
    runtime.ai_result = {"ok": True, "model": "@cf/test/vision", "data": {"store": "Corner Market", "items": [
        {"name": "Greek Yogurt", "calories": 90},
        {"name": "Bananas", "brand": "", "portion": "1 medium", "calories": 105, "carbsG": 27},
        {"name": "Sourdough bread", "portion": "1 slice", "calories": 120, "proteinG": 4},
        {"name": "", "calories": 3},
    ]}}
    image = "data:image/jpeg;base64," + "QUJD" * 100
    result = call(runtime.use("POST", body={"image": image}), "/api/calories/receipt")
    assert result["status"] == 200, result
    data = result["data"]
    assert data["store"] == "Corner Market"
    assert data["found"] == ["Greek Yogurt", "Bananas", "Sourdough bread"]
    assert data["added"] == ["Bananas", "Sourdough bread"]
    assert [item["name"] for item in data["quickFoods"]] == ["Bananas", "Sourdough bread", "Greek yogurt"]
    assert data["quickFoods"][0]["source"] == "receipt"
    assert runtime.ai_calls[-1][2] is True and "receipt" in runtime.ai_calls[-1][1]
    assert count(runtime, "calories_vault_entries") == 0

    overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    assert [item["name"] for item in overview["quickFoods"]][:2] == ["Bananas", "Sourdough bread"]

    bad = call(runtime.use("POST", body={"image": "data:text/html;base64,QUJD"}), "/api/calories/receipt")
    assert bad["status"] == 400 and bad["data"]["error"] == "invalid_image"
    runtime.ai_result = {"ok": True, "data": {"items": [{"name": "", "calories": 1}]}}
    empty = call(runtime.use("POST", body={"image": image}), "/api/calories/receipt")
    assert empty["status"] == 422 and empty["data"]["error"] == "no_foods_on_receipt"
    runtime.ai_result = {"ok": False, "error": "rate_limited", "retryAfterMs": 60000}
    limited = call(runtime.use("POST", body={"image": image}), "/api/calories/receipt")
    assert limited["status"] == 429 and limited["data"]["retryAfterMs"] == 60000
    wrong = call(runtime.use("GET"), "/api/calories/receipt")
    assert wrong["status"] == 405
    without = Runtime(ai=False)
    missing = call(without.use("POST", body={"image": image}), "/api/calories/receipt")
    assert missing["status"] == 503


def test_barcode_lookup_uses_open_food_facts_then_personal_usda_key_and_caches():
    runtime = Runtime()
    runtime.outbound["https://world.openfoodfacts.org/api/v2/product/3017624010701"] = {
        "status": 1, "product": {
            "code": "3017624010701", "product_name": "Hazelnut spread", "brands": "Nutty",
            "quantity": "400 g", "serving_size": "15 g", "serving_quantity": 15,
            "nutriments": {"energy-kcal_100g": 539, "proteins_100g": 6.3,
                           "carbohydrates_100g": 57.5, "fat_100g": 30.9,
                           "energy-kcal_serving": 80.9, "proteins_serving": 0.9,
                           "carbohydrates_serving": 8.6, "fat_serving": 4.6},
            "image_front_small_url": "https://images.openfoodfacts.org/x.jpg",
        }}
    found = call(runtime.use("GET"), "/api/calories/barcode/3017624010701")["data"]
    product = found["product"]
    assert (product["name"], product["brand"], product["source"]) == ("Hazelnut spread", "Nutty", "openfoodfacts")
    assert product["per100g"]["calories"] == 539 and product["perServing"]["calories"] == 80.9
    assert runtime.outbound_calls[0][2]["user-agent"].startswith("ForkMesh-CalorieCounter")
    again = call(runtime.use("GET", actor="charlie"), "/api/calories/barcode/3017624010701")["data"]
    assert again["cached"] is True and len(runtime.outbound_calls) == 1

    missing = call(runtime.use("GET"), "/api/calories/barcode/000000000000")
    assert missing["status"] == 404 and missing["data"]["checked"] == ["openfoodfacts"]

    # The USDA key is the account's own: any member saves one, and it only
    # applies to that account's lookups.
    saved = call(runtime.use("PUT", body={"usdaApiKey": "DEMO_KEY"}), "/api/calories/settings")
    assert saved["data"] == {"ok": True, "settings": {"usda": True}}
    overview = call(runtime.use("GET"), "/api/calories")["data"]
    assert overview["sources"]["usda"] is True and "DEMO_KEY" not in json.dumps(overview)
    assert call(runtime.use("GET", actor="charlie"), "/api/calories")["data"]["sources"]["usda"] is False
    runtime.outbound["https://api.nal.usda.gov/fdc/v1/foods/search?query=041196910184"] = {"foods": [{
        "description": "ALMOND MILK", "brandName": "Silk", "gtinUpc": "041196910184",
        "servingSize": 240, "servingSizeUnit": "ml", "foodNutrients": [
            {"nutrientId": 1008, "value": 25}, {"nutrientId": 1003, "value": 0.4},
            {"nutrientId": 1005, "value": 0.4}, {"nutrientId": 1004, "value": 1.0}]}]}
    theirs = call(runtime.use("GET", actor="charlie"), "/api/calories/barcode/041196910184")
    assert theirs["status"] == 404 and theirs["data"]["checked"] == ["openfoodfacts"]
    usda = call(runtime.use("GET"), "/api/calories/barcode/041196910184")["data"]["product"]
    assert usda["source"] == "usda" and usda["per100g"]["calories"] == 25
    assert usda["perServing"]["calories"] == 60
    assert "api_key=DEMO_KEY" in runtime.outbound_calls[-1][0]
    invalid = call(runtime.use("GET"), "/api/calories/barcode/12ab")
    assert invalid["status"] == 400


def test_barcode_lookup_rebuilds_codes_typed_without_edge_digits():
    # UPC-A prints its number-system and check digits outside the bars, so
    # "3575111700" is what people type for 035751117004.
    runtime = Runtime()
    runtime.outbound["https://world.openfoodfacts.org/api/v2/product/035751117004"] = {
        "status": 1, "product": {"code": "0035751117004", "product_name": "Concha French Sweetie Roll",
                                 "brands": "Buon Appetito", "nutriments": {"energy-kcal_100g": 380}}}
    found = call(runtime.use("GET"), "/api/calories/barcode/3575111700")
    assert found["status"] == 200 and found["data"]["product"]["barcode"] == "0035751117004"
    assert [url.split("?")[0].rsplit("/", 1)[1] for url, *_ in runtime.outbound_calls] == [
        "3575111700.json", "035751117004.json"]
    again = call(runtime.use("GET"), "/api/calories/barcode/3575111700")["data"]
    assert again["cached"] is True and len(runtime.outbound_calls) == 2

    assert calories._barcode_candidates("03575111700") == ["03575111700", "035751117004"]
    assert calories._barcode_candidates("301762401070") == ["301762401070", "3017624010701"]
    assert calories._barcode_candidates("425261") == ["425261", "042100005264"]
    assert calories._barcode_candidates("04252614") == ["04252614", "042100005264"]
    assert calories._barcode_candidates("3017624010701") == ["3017624010701"]
    assert calories._barcode_candidates("035751117004") == ["035751117004"]


SEARCH = "https://search.openfoodfacts.org/search?q="


def test_food_search_returns_normalised_results():
    runtime = Runtime()
    runtime.outbound[SEARCH + "greek%20yogurt&"] = {
        "hits": [
            {"code": "1", "product_name": "Greek yogurt", "brands": ["Fage", " "],
             "nutriments": {"energy-kcal_100g": 97, "proteins_100g": 9}},
            {"code": "2", "product_name": "No data"},
        ]}
    result = call(runtime.use("GET", query={"q": "greek yogurt"}), "/api/calories/foods")["data"]
    assert [(item["name"], item["brand"]) for item in result["foods"]] == [("Greek yogurt", "Fage")]
    assert all(url.startswith(SEARCH) for url, *_ in runtime.outbound_calls)
    assert all(headers["user-agent"].startswith("ForkMesh-CalorieCounter")
               for _, _, headers, _ in runtime.outbound_calls)
    short = call(runtime.use("GET", query={"q": "a"}), "/api/calories/foods")
    assert short["status"] == 400


def test_food_search_finds_branded_products_as_you_type():
    assert calories._search_queries(["trader", "joes", "buffalo", "chi"]) == [
        "trader joes buffalo chi",
        "trader joes buffalo product_name.en:chi*",
        "joes buffalo product_name.en:chi* brands:trader*",
        "buffalo product_name.en:chi* brands:trader*",
    ]
    assert calories._search_queries(["banana"]) == ["banana", "product_name.en:banana*"]
    assert calories._search_queries(["oat", "m"]) == ["oat m", "m brands:oat*"]
    assert calories._search_words("Trader Joe\u2019s  Buffalo-Chicken!") == [
        "trader", "joes", "buffalo", "chicken"]
    assert calories._url_query("crème fraîche") == "cr%C3%A8me%20fra%C3%AEche"

    runtime = Runtime()
    runtime.outbound[SEARCH + "trader%20joes%20buffalo%20chicken%20wrap&"] = {"hits": [
        {"code": "00588713", "product_name": "Trader joe's", "brands": ["Trader Joe's"],
         "nutriments": {"energy-kcal_100g": 400}},
        {"code": "0200376907990", "product_name": "Buffalo Chicken Wrap",
         "nutriments": {"energy-kcal_100g": 210}},
    ]}
    runtime.outbound[SEARCH + "buffalo%20chicken%20product_name.en%3Awrap%2A%20brands%3Atrader%2A&"] = {"hits": [
        {"code": "00645034", "product_name": "Hatch chile chicken wrap", "brands": ["Trader Joe's"],
         "nutriments": {"energy-kcal_100g": 360}},
        {"code": "00966887", "product_name": "Buffalo Style Chicken Wrap", "brands": ["Trader Joe's"],
         "nutriments": {"energy-kcal_100g": 199, "proteins_100g": 11}},
        {"code": "00588713", "product_name": "Trader joe's", "brands": ["Trader Joe's"],
         "nutriments": {"energy-kcal_100g": 400}},
    ]}
    result = call(runtime.use("GET", query={"q": "Trader Joe's buffalo chicken wrap"}),
                  "/api/calories/foods")["data"]
    assert [(item["name"], item["brand"]) for item in result["foods"]] == [
        ("Buffalo Style Chicken Wrap", "Trader Joe's"),
        ("Hatch chile chicken wrap", "Trader Joe's"),
        ("Buffalo Chicken Wrap", ""),
        ("Trader joe's", "Trader Joe's"),
    ]
    assert len(runtime.outbound_calls) == 4


def test_food_search_falls_back_to_the_legacy_search_and_reports_outages():
    runtime = Runtime()
    runtime.outbound["https://world.openfoodfacts.org/cgi/search.pl?search_terms=greek%20yogurt"] = {
        "products": [{"code": "1", "product_name": "Greek yogurt &amp; honey",
                      "nutriments": {"energy-kcal_100g": 97}}]}
    result = call(runtime.use("GET", query={"q": "greek yogurt"}), "/api/calories/foods")["data"]
    assert [item["name"] for item in result["foods"]] == ["Greek yogurt & honey"]

    down = Runtime()
    failed = call(down.use("GET", query={"q": "greek yogurt"}), "/api/calories/foods")
    assert failed["status"] == 502 and failed["data"]["error"] == "food_search_unavailable"


def test_challenges_invite_friends_by_username_and_protect_diaries():
    runtime = Runtime(installed=("alice", "charlie", "eve"))
    call(runtime.use("PUT", body={"weightKg": 90}), f"/api/calories/weights/{YESTERDAY}")
    call(runtime.use("PUT", actor="charlie", body={"weightKg": 80}), f"/api/calories/weights/{YESTERDAY}")
    created = call(runtime.use("POST", body={
        "kind": "weight_loss_percent", "title": "Spring shred", "startsOn": YESTERDAY,
        "endsOn": "2026-10-15", "invite": ["@Charlie", "dana", "nobody", "alice", "Bad Name"],
    }), "/api/calories/challenges")
    assert created["status"] == 201, created
    assert created["data"]["invited"] == ["charlie", "dana"]
    assert created["data"]["skipped"] == ["nobody", "bad name"]
    challenge = created["data"]["challenge"]
    assert challenge["status"] == "active" and challenge["myStatus"] == "accepted"
    assert challenge["participants"] == 1 and challenge["invited"] == 2
    assert sorted(ping["recipient"] for ping in runtime.pings) == ["charlie", "dana"]
    assert runtime.pings[0]["href"] == "/dashboard/calories?challenge=" + challenge["id"]

    # Someone who was not invited cannot see or open it.
    assert call(runtime.use("GET", actor="eve"), "/api/calories")["data"]["challenges"] == []
    hidden = call(runtime.use("GET", actor="eve"), f"/api/calories/challenges/{challenge['id']}")
    assert hidden["status"] == 404

    seen = call(runtime.use("GET", actor="charlie"), "/api/calories")["data"]["challenges"]
    assert seen[0]["myStatus"] == "invited" and seen[0]["isCreator"] is False
    accepted = call(runtime.use("POST", actor="charlie", body={"accept": True}),
                    f"/api/calories/challenges/{challenge['id']}/respond")["data"]["challenge"]
    assert accepted["participants"] == 2
    # Dana has not installed Calorie Counter yet, so she installs before answering.
    uninstalled = call(runtime.use("POST", actor="dana", body={"accept": False}),
                       f"/api/calories/challenges/{challenge['id']}/respond")
    assert uninstalled["status"] == 409
    call(runtime.use("POST", actor="dana"), "/api/calories/install")
    declined = call(runtime.use("POST", actor="dana", body={"accept": False}),
                    f"/api/calories/challenges/{challenge['id']}/respond")["data"]
    assert declined == {"ok": True, "left": True}
    assert call(runtime.use("GET", actor="dana"), "/api/calories")["data"]["challenges"] == []

    call(runtime.use("PUT", body={"weightKg": 87.3}), f"/api/calories/weights/{DAY}")
    call(runtime.use("PUT", actor="charlie", body={"weightKg": 79}), f"/api/calories/weights/{DAY}")
    board = call(runtime.use("GET", actor="charlie", query={"day": DAY}),
                 f"/api/calories/challenges/{challenge['id']}")["data"]["challenge"]
    leaders = [(item["rank"], item["name"], item["score"]) for item in board["leaderboard"]]
    assert leaders == [(1, "alice", 3.0), (2, "charlie", 1.25)]
    assert board["leaderboard"][0]["isMe"] is False and board["leaderboard"][1]["isMe"] is True
    assert [item["name"] for item in board["pending"]] == ["dana"]
    assert "weightKg" not in json.dumps(board["leaderboard"])

    outsider = call(runtime.use("PATCH", actor="charlie", body={"title": "Hijack"}),
                    f"/api/calories/challenges/{challenge['id']}")
    assert outsider["status"] == 403
    invite_denied = call(runtime.use("POST", actor="charlie", body={"members": ["eve"]}),
                         f"/api/calories/challenges/{challenge['id']}/invite")
    assert invite_denied["status"] == 403
    invited = call(runtime.use("POST", body={"members": ["eve", "ghost"]}),
                   f"/api/calories/challenges/{challenge['id']}/invite")["data"]
    assert (invited["invited"], invited["skipped"]) == (["eve"], ["ghost"])
    closed = call(runtime.use("PATCH", body={"title": "Spring shred", "status": "closed", "endsOn": DAY}),
                  f"/api/calories/challenges/{challenge['id']}")["data"]["challenge"]
    assert closed["status"] == "closed" and closed["winner"] == "alice"
    late = call(runtime.use("POST", actor="eve", body={"accept": True}),
                f"/api/calories/challenges/{challenge['id']}/respond")
    assert late["status"] == 409

    streak = call(runtime.use("POST", body={"kind": "log_streak", "title": "Log every day",
                                            "startsOn": YESTERDAY, "endsOn": "2026-09-30",
                                            "invite": ["charlie"]}), "/api/calories/challenges")["data"]["challenge"]
    log(runtime, "alice", YESTERDAY, "Eggs", 200)
    log(runtime, "alice", DAY, "Toast", 150)
    board = call(runtime.use("GET", query={"day": DAY}), f"/api/calories/challenges/{streak['id']}")["data"]["challenge"]
    assert board["leaderboard"][0]["detail"] == "2 days logged"
    bad = call(runtime.use("POST", body={"kind": "nope", "title": "x", "endsOn": DAY}), "/api/calories/challenges")
    assert bad["status"] == 400
    not_mine = call(runtime.use("DELETE", actor="charlie"), f"/api/calories/challenges/{streak['id']}")
    assert not_mine["status"] == 403
    deleted = call(runtime.use("DELETE"), f"/api/calories/challenges/{streak['id']}")
    assert deleted["data"]["deleted"] is True
    assert count(runtime, "calories_vault_challenge_members", "challenge_id=?",
                 streak["id"]) == 0


def test_day_totals_follow_edits_because_sql_cannot_sum_a_sealed_column():
    runtime = Runtime()
    first = log(runtime, "alice", DAY, "Oatmeal", 320, 12, 55, 6)
    log(runtime, "alice", DAY, "Salad", 180, 4, 12, 11)

    def totals(day=DAY):
        rows = call(runtime.use("GET", query={"from": YESTERDAY, "to": DAY}),
                    "/api/calories/entries")["data"]["days"]
        return next((item for item in rows if item["day"] == day), None)

    assert totals() == {"day": DAY, "entries": 2, "calories": 500,
                        "proteinG": 16.0, "carbsG": 67.0, "fatG": 17.0}
    # Correcting an entry re-totals its day.
    call(runtime.use("PATCH", body={"calories": 420}), f"/api/calories/entries/{first['id']}")
    assert totals()["calories"] == 600
    # Moving one to another day re-totals both.
    call(runtime.use("PATCH", body={"day": YESTERDAY}), f"/api/calories/entries/{first['id']}")
    assert totals()["entries"] == 1 and totals()["calories"] == 180
    assert totals(YESTERDAY)["calories"] == 420
    # A day with nothing left keeps no rollup row at all.
    call(runtime.use("DELETE"), f"/api/calories/entries/{first['id']}")
    assert totals(YESTERDAY) is None
    assert count(runtime, "calories_vault_days", "day=?", YESTERDAY) == 0
    assert count(runtime, "calories_vault_days", "day=?", DAY) == 1
    # And the rollup itself is sealed with the account's key, like every row.
    stored = runtime.db.execute(
        "SELECT data FROM calories_vault_days WHERE day=?", (DAY,)).fetchone()["data"]
    assert sealed_for(runtime, "bi:alice", stored)["calories"] == 180
    assert asyncio.run(runtime.open(stored)) is None


# Every table that holds a diary, and the column naming the account whose key
# must open it. calories_keys and calories_products are deliberately absent:
# the wrapped keys are the host's to hold, and the product cache is public
# food data shared by everyone.
VAULT_TABLES = (
    ("calories_vault_profiles", "account_bi"),
    ("calories_vault_entries", "account_bi"),
    ("calories_vault_days", "account_bi"),
    ("calories_vault_weights", "account_bi"),
    ("calories_vault_settings", "account_bi"),
    ("calories_vault_challenges", "created_by_bi"),
)


def test_every_diary_row_is_sealed_with_its_own_accounts_key():
    runtime = Runtime()
    log(runtime, "alice", DAY, "Oatmeal with berries", 320, 12, 55, 6)
    call(runtime.use("PUT", body={"weightKg": 70.5}), f"/api/calories/weights/{DAY}")
    call(runtime.use("PUT", body={"usdaApiKey": "DEMO_KEY"}), "/api/calories/settings")
    call(runtime.use("POST", body={"kind": "weight_loss_kg", "title": "Spring shred",
                                   "endsOn": "2026-12-31", "invite": ["charlie"]}),
         "/api/calories/challenges")
    log(runtime, "charlie", DAY, "Burger", 900)
    call(runtime.use("PUT", actor="charlie", body={"weightKg": 90.0}),
         f"/api/calories/weights/{DAY}")

    alice, charlie = diary_key(runtime, "bi:alice"), diary_key(runtime, "bi:charlie")
    assert alice and charlie and alice != charlie
    for table, owner in VAULT_TABLES:
        rows = runtime.db.execute(
            f"SELECT {owner} AS owner,data FROM {table}").fetchall()
        assert rows, table
        for row in rows:
            # The host key on its own opens nothing but the wrapped keys.
            assert asyncio.run(runtime.open(row["data"])) is None, table
            assert sealed_for(runtime, row["owner"], row["data"]) is not None, table
            stranger = "bi:charlie" if row["owner"] == "bi:alice" else "bi:alice"
            assert sealed_for(runtime, stranger, row["data"]) is None, table

    # A member's start weight is the member's own row, sealed with their key.
    joined = runtime.db.execute(
        "SELECT account_bi,data FROM calories_vault_challenge_members "
        "WHERE status='accepted'").fetchone()
    assert sealed_for(runtime, joined["account_bi"], joined["data"]) == {
        "startWeightKg": 70.5}
    # An invitation carries nothing to seal until it is accepted.
    assert runtime.db.execute(
        "SELECT data FROM calories_vault_challenge_members "
        "WHERE status='invited'").fetchone()["data"] == ""

    # The wrapped keys, and the shared product cache, are the host's rows.
    wrapped = runtime.db.execute("SELECT wrapped FROM calories_keys").fetchall()
    assert len(wrapped) == 2
    for row in wrapped:
        opened = asyncio.run(runtime.open(row["wrapped"]))
        assert opened["version"] == calories.KEY_VERSION and len(opened["key"]) >= 32
    asyncio.run(calories._cache_product(runtime, "1234567890", {"name": "Oats"}))
    assert asyncio.run(runtime.open(runtime.db.execute(
        "SELECT data FROM calories_products").fetchone()["data"])) == {"name": "Oats"}


V2_SCHEMA = """
CREATE TABLE calories_profiles (account_bi TEXT PRIMARY KEY, account_name TEXT NOT NULL,
  reminders_on INTEGER NOT NULL DEFAULT 0, tz_offset INTEGER NOT NULL DEFAULT 0,
  daily_calories INTEGER NOT NULL DEFAULT 2000, installed_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL, data TEXT NOT NULL);
CREATE TABLE calories_entries (entry_id TEXT PRIMARY KEY, account_bi TEXT NOT NULL,
  day TEXT NOT NULL, meal TEXT NOT NULL, source TEXT NOT NULL, calories INTEGER NOT NULL,
  protein_g REAL NOT NULL, carbs_g REAL NOT NULL, fat_g REAL NOT NULL,
  logged_at INTEGER NOT NULL, data TEXT NOT NULL);
CREATE TABLE calories_weights (account_bi TEXT NOT NULL, day TEXT NOT NULL,
  weight_kg REAL NOT NULL, logged_at INTEGER NOT NULL, PRIMARY KEY(account_bi,day));
CREATE TABLE calories_settings (account_bi TEXT PRIMARY KEY, updated_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE TABLE calories_challenges (challenge_id TEXT PRIMARY KEY, kind TEXT NOT NULL,
  status TEXT NOT NULL, starts_on TEXT NOT NULL, ends_on TEXT NOT NULL,
  created_by_bi TEXT NOT NULL, created_by_name TEXT NOT NULL, created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL, data TEXT NOT NULL);
CREATE TABLE calories_challenge_members (challenge_id TEXT NOT NULL, account_name TEXT NOT NULL,
  account_bi TEXT NOT NULL DEFAULT '', status TEXT NOT NULL, invited_by_name TEXT NOT NULL,
  start_weight_kg REAL NOT NULL DEFAULT 0, joined_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(challenge_id,account_name));
INSERT INTO calories_profiles VALUES('bi:alice','alice',1,-120,1900,1,2,
  'sealed:{"dailyCalories": 1900, "reminders": [{"time": "12:30", "label": "Log lunch",
   "enabled": true, "onlyIfUnlogged": false, "id": "1230"}]}');
INSERT INTO calories_profiles VALUES('bi:charlie','charlie',0,0,2400,1,2,
  'sealed:{"dailyCalories": 2400}');
INSERT INTO calories_entries VALUES('00000000000000000000000000000abc','bi:alice','2026-09-16',
  'lunch','barcode',640,30,70,20,9,'sealed:{"name": "Burrito", "notes": "with extra salsa"}');
INSERT INTO calories_entries VALUES('00000000000000000000000000000abd','bi:charlie','2026-09-16',
  'dinner','manual',900,10,20,30,9,'sealed:{"name": "Burger"}');
INSERT INTO calories_weights VALUES('bi:alice','2026-09-16',82.5,4);
INSERT INTO calories_settings VALUES('bi:alice',5,'sealed:{"usdaApiKey": "DEMO_KEY"}');
INSERT INTO calories_challenges VALUES('000000000000000000000000000000c1','weight_loss_kg',
  'open','2026-09-01','2026-12-31','bi:alice','alice',1,2,'sealed:{"title": "Spring shred"}');
INSERT INTO calories_challenge_members VALUES('000000000000000000000000000000c1','alice',
  'bi:alice','accepted','alice',83.0,3);
INSERT INTO calories_challenge_members VALUES('000000000000000000000000000000c1','charlie',
  '','invited','alice',0,0);
"""


def test_two_zero_rows_are_re_encrypted_under_each_accounts_own_key():
    runtime = Runtime(installed=())
    runtime.db.executescript(V2_SCHEMA)

    # The caller's own rows are re-encrypted before anything reads them.
    overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    assert overview["installed"] is True
    assert overview["profile"]["dailyCalories"] == 1900
    assert overview["profile"]["tzOffsetMinutes"] == -120
    assert overview["profile"]["reminders"][0]["label"] == "Log lunch"
    assert [item["name"] for item in overview["entries"]] == ["Burrito"]
    assert overview["entries"][0]["meal"] == "lunch"
    assert overview["entries"][0]["notes"] == "with extra salsa"
    assert overview["todayTotals"] == {"day": DAY, "entries": 1, "calories": 640,
                                       "proteinG": 30.0, "carbsG": 70.0, "fatG": 20.0}
    assert overview["latestWeight"] == {"weightKg": 82.5, "day": DAY}
    assert overview["sources"]["usda"] is True
    assert overview["challenges"][0]["title"] == "Spring shred"
    entry = runtime.db.execute(
        "SELECT data FROM calories_vault_entries WHERE account_bi='bi:alice'").fetchone()
    assert sealed_for(runtime, "bi:alice", entry["data"])["calories"] == 640
    assert count(runtime, "calories_profiles", "account_bi='bi:alice'") == 0
    assert count(runtime, "calories_entries", "account_bi='bi:alice'") == 0
    # The challenge went with her, invitation and start weight included.
    member = runtime.db.execute(
        "SELECT account_bi,data FROM calories_vault_challenge_members "
        "WHERE account_name='alice'").fetchone()
    assert sealed_for(runtime, "bi:alice", member["data"]) == {"startWeightKg": 83.0}
    assert count(runtime, "calories_vault_challenge_members",
                 "account_name='charlie' AND status='invited'") == 1
    assert count(runtime, "calories_challenges") == 0

    # Charlie has not been back, so the per-minute sweep drains what is left
    # and drops the 2.0.0 tables once nothing is waiting.
    assert count(runtime, "calories_profiles") == 1
    asyncio.run(calories.deliver_reminders(runtime))
    theirs = call(runtime.use("GET", actor="charlie", query={"day": DAY}),
                  "/api/calories")["data"]
    assert [item["name"] for item in theirs["entries"]] == ["Burger"]
    assert theirs["profile"]["dailyCalories"] == 2400
    assert diary_key(runtime, "bi:charlie") != diary_key(runtime, "bi:alice")
    asyncio.run(calories.deliver_reminders(runtime))
    assert not runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name IN "
        "('calories_profiles','calories_entries','calories_weights','calories_settings',"
        "'calories_challenges','calories_challenge_members')").fetchall()
    assert calories._LEGACY_SEALED_DONE is True


def test_rejects_uninstalled_access_bad_days_and_unknown_routes():
    runtime = Runtime()
    missing = call(runtime.use("GET", actor="dana"), "/api/calories/profile")
    assert missing["status"] == 409 and missing["data"]["canInstall"] is True
    bad_day = call(runtime.use("POST", body={"day": "2026-13-40", "name": "x", "calories": 1}),
                   "/api/calories/entries")
    assert bad_day["status"] == 400 and bad_day["data"]["error"] == "day_required"
    for path in ("/api/calories/nope", "/api/calories/acme/entries", "/api/caloriesx",
                 "/api/calories/barcode"):
        assert call(runtime.use("GET"), path)["status"] == 404, path
    wrong = call(runtime.use("DELETE"), "/api/calories/analyze")
    assert wrong["status"] == 405 and wrong["headers"]["allow"] == "POST"
    install_get = call(runtime.use("GET"), "/api/calories/install")
    assert install_get["status"] == 405
    guest = call(runtime.use("GET", actor="mallory"), "/api/calories")
    assert guest["status"] == 401


def test_uninstall_exports_then_purges_only_the_callers_rows():
    runtime = Runtime()
    log(runtime, "alice", DAY, "Soup", 120)
    log(runtime, "charlie", DAY, "Stew", 400)
    call(runtime.use("PUT", body={"weightKg": 70}), f"/api/calories/weights/{DAY}")
    call(runtime.use("PUT", body={"usdaApiKey": "k"}), "/api/calories/settings")
    mine = call(runtime.use("POST", body={"kind": "log_streak", "title": "Streak", "endsOn": "2026-12-31",
                                          "invite": ["charlie"]}), "/api/calories/challenges")["data"]["challenge"]
    theirs = call(runtime.use("POST", actor="charlie", body={
        "kind": "log_streak", "title": "Charlie's", "endsOn": "2026-12-31", "invite": ["alice"]}),
        "/api/calories/challenges")["data"]["challenge"]
    call(runtime.use("POST", body={"accept": True}), f"/api/calories/challenges/{theirs['id']}/respond")

    exported = call(runtime.use("GET", actor="charlie"), "/api/calories/export")["data"]
    assert exported["filename"] == "forkmesh-calories-charlie.json"
    assert exported["export"]["format"] == "forkmesh-calories-export-v2"
    assert exported["export"]["account"] == "charlie"
    assert [item["name"] for item in exported["export"]["entries"]] == ["Stew"]

    removed = call(runtime.use("POST"), "/api/calories/uninstall")["data"]
    assert removed["uninstalled"] is True and removed["hadData"] is True
    assert removed["filename"] == "forkmesh-calories-alice.json"
    assert [item["name"] for item in removed["export"]["entries"]] == ["Soup"]
    assert sorted(item["title"] for item in removed["export"]["challenges"]) == ["Charlie's", "Streak"]
    for table in ("calories_vault_entries", "calories_vault_days", "calories_vault_weights",
                  "calories_vault_settings", "calories_vault_profiles", "calories_keys"):
        assert count(runtime, table, "account_bi='bi:alice'") == 0, table
    assert count(runtime, "calories_vault_challenges", "challenge_id=?", mine["id"]) == 0
    assert count(runtime, "calories_vault_challenge_members", "account_name='alice'") == 0
    # Charlie keeps his diary and his own challenge.
    assert count(runtime, "calories_vault_entries", "account_bi='bi:charlie'") == 1
    assert count(runtime, "calories_vault_challenges", "challenge_id=?", theirs["id"]) == 1
    after = call(runtime.use("GET"), "/api/calories")["data"]
    assert after["installed"] is False
    assert call(runtime.use("POST"), "/api/calories/uninstall")["status"] == 409

    # Account deletion removes the same rows through the host hook.
    asyncio.run(calories.purge_account_data(runtime, "bi:charlie", "charlie"))
    for table in ("calories_vault_entries", "calories_vault_profiles",
                  "calories_vault_challenges", "calories_vault_challenge_members",
                  "calories_keys"):
        assert count(runtime, table) == 0, table
    for statement in calories.DATA_SCHEMA_DOWN:
        runtime.db.execute(statement)
    assert not runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'calories_%'").fetchall()


LEGACY_SCHEMA = """
CREATE TABLE calorie_profiles (org_bi TEXT NOT NULL, account_bi TEXT NOT NULL, org_name TEXT NOT NULL,
  account_name TEXT NOT NULL, reminders_on INTEGER NOT NULL DEFAULT 0, tz_offset INTEGER NOT NULL DEFAULT 0,
  daily_calories INTEGER NOT NULL DEFAULT 2000, updated_at INTEGER NOT NULL, data TEXT NOT NULL,
  PRIMARY KEY(org_bi,account_bi));
CREATE TABLE calorie_entries (entry_id TEXT PRIMARY KEY, org_bi TEXT NOT NULL, account_bi TEXT NOT NULL,
  day TEXT NOT NULL, meal TEXT NOT NULL, source TEXT NOT NULL, calories INTEGER NOT NULL,
  protein_g REAL NOT NULL, carbs_g REAL NOT NULL, fat_g REAL NOT NULL, logged_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE TABLE calorie_weights (org_bi TEXT NOT NULL, account_bi TEXT NOT NULL, day TEXT NOT NULL,
  weight_kg REAL NOT NULL, logged_at INTEGER NOT NULL, PRIMARY KEY(org_bi,account_bi,day));
CREATE TABLE calorie_challenges (challenge_id TEXT PRIMARY KEY, org_bi TEXT, data TEXT);
CREATE TABLE calorie_challenge_members (challenge_id TEXT, account_bi TEXT);
CREATE TABLE calorie_reminder_deliveries (org_bi TEXT, account_bi TEXT, reminder_key TEXT);
CREATE TABLE calorie_products (barcode TEXT PRIMARY KEY, fetched_at INTEGER, data TEXT);
CREATE TABLE calorie_settings (org_bi TEXT PRIMARY KEY, updated_at INTEGER, data TEXT);
INSERT INTO org_plugin_installs VALUES('org:acme','calories','acme','bi:alice','alice',1);
INSERT INTO org_plugin_installs VALUES('org:acme','crm','acme','bi:alice','alice',1);
INSERT INTO calorie_profiles VALUES('org:acme','bi:alice','acme','alice',1,-120,1800,5,
  'sealed:{"dailyCalories": 1800, "reminders": [{"time": "12:30"}]}');
INSERT INTO calorie_profiles VALUES('org:beta','bi:alice','beta','alice',0,0,2500,2,'sealed:{"dailyCalories": 2500}');
INSERT INTO calorie_entries VALUES('00000000000000000000000000000abc','org:acme','bi:alice','2026-09-16',
  'lunch','manual',640,30,70,20,9,'sealed:{"name": "Burrito"}');
INSERT INTO calorie_weights VALUES('org:acme','bi:alice','2026-09-16',82.5,4);
INSERT INTO calorie_weights VALUES('org:beta','bi:alice','2026-09-16',82.0,3);
"""


def test_organization_installed_diaries_move_onto_their_accounts_once():
    runtime = Runtime(installed=())
    runtime.db.executescript(LEGACY_SCHEMA)
    overview = call(runtime.use("GET", query={"day": DAY}), "/api/calories")["data"]
    assert overview["installed"] is True
    assert overview["profile"]["dailyCalories"] == 1800
    assert [item["name"] for item in overview["entries"]] == ["Burrito"]
    assert overview["latestWeight"] == {"weightKg": 82.5, "day": DAY}
    assert overview["challenges"] == []
    assert not runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'calorie\\_%' ESCAPE '\\'").fetchall()
    assert [row[0] for row in runtime.db.execute("SELECT plugin FROM org_plugin_installs")] == ["crm"]
    # Charlie never used 1.0.0, so he still has to install.
    assert call(runtime.use("GET", actor="charlie"), "/api/calories")["data"]["installed"] is False


def test_package_is_account_scoped_across_manifest_host_and_web_page():
    import build_app_plugins
    import plugin_data

    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    schema = json.loads((REPO / "marketplace" / "manifest.schema.json").read_text(encoding="utf-8"))
    page = (PACKAGE / "content/web/page.js").read_text(encoding="utf-8")
    view = (PACKAGE / "content/web/view.html").read_text(encoding="utf-8")
    entry = (APP / "src" / "entry.py").read_text(encoding="utf-8")
    assert manifest["worker"]["module"] == "calories"
    assert manifest["worker"]["api"] == "/api/calories"
    assert manifest["worker"]["outboundOrigins"] == [
        "https://world.openfoodfacts.org", "https://search.openfoodfacts.org",
        "https://api.nal.usda.gov"]
    assert manifest["dataLifecycle"] == {"scope": "account"}
    assert "account" in schema["properties"]["dataLifecycle"]["properties"]["scope"]["enum"]
    assert manifest["install"] == {"bundled": True, "default": False, "sync": "account"}
    assert not any(perm["id"].startswith("organization.") for perm in manifest["permissions"])
    assert {perm["id"] for perm in manifest["permissions"]} >= {
        "account.calories.read", "account.calories.write",
        "ai.vision.analyze", "external.food.lookup", "camera.capture", "notifications.ping"}
    for marker in ("data-calories-install", "data-calories-uninstall", "data-calories-export",
                   "data-calories-photo-video", "data-calories-scan-video", "data-calories-chart=\"weight\"",
                   "data-calories-table-toggle", "data-calories-reminders", "data-calories-board",
                   "data-calories-challenge-invite"):
        assert marker in view, marker
    assert 'registerPage("calories"' in page
    assert 'apiPath("/install")' in page and 'apiPath("/uninstall")' in page
    assert "/api/plugins/" not in page and "organization" not in page.lower()
    assert "BarcodeDetector" in page and "getUserMedia" in page
    assert "http://" not in page

    # The generated registry marks the package account-scoped, so the shared
    # organization lifecycle neither lists nor installs it.
    staged = {}
    exec(build_app_plugins.registry_source(), staged)
    registry = staged["PLUGINS"]
    assert registry["calories"]["accountData"] is True
    assert registry["calories"]["organizationData"] is False
    original = plugin_data.REGISTRY
    plugin_data.REGISTRY = registry
    try:
        assert plugin_data._data_module("calories") is None
        assert plugin_data._account_module("calories") is calories
        runtime = Runtime(installed=())
        runtime.db.executescript("""
          CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT,data TEXT,created_at INTEGER);
          CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,created_at INTEGER);
          INSERT INTO orgs VALUES('org:acme','acme','sealed:{}',1);
          INSERT INTO org_members VALUES('org:acme','bi:alice','owner','alice',1);
        """)

        async def organization(name, account_bi):
            return ("org:acme", "owner") if (name, account_bi) == ("acme", "bi:alice") else ("", "")
        runtime.organization = organization
        state = asyncio.run(plugin_data.handle(runtime.use("GET"), "/api/plugins/acme"))
        assert "calories" not in [item["id"] for item in state["data"]["plugins"]]
        refused = asyncio.run(plugin_data.handle(runtime.use("POST"), "/api/plugins/acme/calories/install"))
        assert refused["status"] == 409 and refused["data"]["error"] == "plugin_installs_per_account"
        assert count(runtime, "org_plugin_installs") == 0

        call(runtime.use("POST", actor="dana"), "/api/calories/install")
        asyncio.run(plugin_data.purge_account(runtime, "bi:dana", "dana"))
        assert count(runtime, "calories_vault_profiles") == 0
    finally:
        plugin_data.REGISTRY = original

    # Host capabilities the package relies on.
    assert "async def ai_json(" in entry and "async def _plugin_ai_json(" in entry
    assert "await deliver_plugin_reminders(env)" in entry
    assert "await plugin_data.purge_account(_PluginPurgeRuntime(env), name_bi, name)" in entry
    assert calories.DATA_SCHEMA_DOWN[::-1][0].endswith("calories_keys")
    assert "async def seal_with(" in entry and "async def open_with(" in entry
    assert "def new_secret(" in entry


def units_of(runtime, actor="alice"):
    profile = call(runtime.use("GET", actor=actor), "/api/calories/profile")["data"]["profile"]
    return profile["unitsPreference"], profile["units"], profile["autoUnits"]


def test_weights_follow_the_country_the_request_came_from():
    # Nobody picked anything, so the unit follows the member: pounds for a
    # request out of the United States, kilograms for one out of Germany.
    american = Runtime(installed=("alice",), country="US")
    assert units_of(american) == ("auto", "imperial", "imperial")
    german = Runtime(installed=("alice",), country="DE")
    assert units_of(german) == ("auto", "metric", "metric")
    # The same account travelling: the resolution is per request, not frozen
    # at install, and nothing is rewritten to say so.
    american.country_value = "DE"
    assert units_of(american) == ("auto", "metric", "metric")
    american.country_value = "GB"
    assert units_of(american) == ("auto", "imperial", "imperial")
    # A host with no country at all (a cron sweep, or an unresolved address)
    # falls back to the answer stored on the row — here the one written at
    # install — rather than snapping everyone to metric.
    american.country_value = ""
    assert units_of(american) == ("auto", "imperial", "imperial")


def test_a_pinned_unit_beats_the_country_and_a_round_trip_does_not_pin_one():
    runtime = Runtime(installed=("alice",), country="US")
    pinned = call(runtime.use("PUT", body={"unitsPreference": "metric"}),
                  "/api/calories/profile")["data"]["profile"]
    assert (pinned["unitsPreference"], pinned["units"]) == ("metric", "metric")
    assert units_of(runtime) == ("metric", "metric", "imperial")
    # Back to following the member.
    call(runtime.use("PUT", body={"unitsPreference": "auto"}), "/api/calories/profile")
    assert units_of(runtime) == ("auto", "imperial", "imperial")
    # An older client knows only the resolved unit and echoes it back on every
    # save; that is not a choice, so the preference stays on auto.
    echoed = call(runtime.use("PUT", body={"units": "imperial", "dailyCalories": 1700}),
                  "/api/calories/profile")["data"]["profile"]
    assert (echoed["unitsPreference"], echoed["dailyCalories"]) == ("auto", 1700)
    # The same client asking for the other unit is a choice, and it pins.
    changed = call(runtime.use("PUT", body={"units": "metric"}),
                   "/api/calories/profile")["data"]["profile"]
    assert (changed["unitsPreference"], changed["units"]) == ("metric", "metric")


def test_a_profile_written_before_the_preference_existed_keeps_its_choice():
    runtime = Runtime(installed=("alice", "charlie"), country="US")
    for actor, stored in (("alice", "imperial"), ("charlie", "metric")):
        row = runtime.db.execute(
            "SELECT data FROM calories_vault_profiles WHERE account_bi=?",
            ("bi:" + actor,)).fetchone()
        legacy = sealed_for(runtime, "bi:" + actor, row["data"])
        for key in ("unitsPreference", "autoUnits"):
            legacy.pop(key, None)
        legacy["units"] = stored
        runtime.db.execute(
            "UPDATE calories_vault_profiles SET data=? WHERE account_bi=?",
            (asyncio.run(runtime.seal_with(diary_key(runtime, "bi:" + actor), legacy)),
             "bi:" + actor))
    # An explicit imperial row was a real choice and survives as one; the old
    # blanket metric default was never a choice, so it starts following.
    assert units_of(runtime, "alice") == ("imperial", "imperial", "imperial")
    assert units_of(runtime, "charlie") == ("auto", "imperial", "imperial")


def test_a_shared_leaderboard_reads_in_each_viewer_s_own_unit():
    runtime = Runtime(installed=("alice", "charlie"), country="DE")
    challenge = call(runtime.use("POST", body={
        "kind": "weight_loss_kg", "title": "Spring shred",
        "startsOn": YESTERDAY, "endsOn": "2026-10-31", "invite": ["charlie"],
    }), "/api/calories/challenges")["data"]["challenge"]
    call(runtime.use("POST", actor="charlie", body={"accept": True}),
         f"/api/calories/challenges/{challenge['id']}/respond")
    for actor in ("alice", "charlie"):
        call(runtime.use("PUT", actor=actor, body={"weightKg": 90.0}),
             f"/api/calories/weights/{YESTERDAY}")
        call(runtime.use("PUT", actor=actor, body={"weightKg": 87.3}),
             f"/api/calories/weights/{DAY}")

    def detail(actor):
        board = call(runtime.use("GET", actor=actor, query={"day": DAY}),
                     f"/api/calories/challenges/{challenge['id']}")["data"]["challenge"]
        return board["leaderboard"][0]["detail"], board["leaderboard"][0]["score"]

    assert detail("alice") == ("2.7 kg lost", 2.7)
    # The same row, read from the United States: pounds on the label, and the
    # ranking score stays in kilograms so both readers see the same order.
    runtime.country_value = "US"
    assert detail("charlie") == ("5.95 lb lost", 2.7)
