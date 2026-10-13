import asyncio
import importlib.util
import sqlite3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
NOW = 1_800_000_000_000
TODAY = "2027-01-15"
SLOTS = 288


def _load(now=NOW):
    spec = importlib.util.spec_from_file_location(
        "account_activity_under_test", ROOT / "src" / "account_activity.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT);"
        + (ROOT / "migrations" / "0235_account_activity_days.sql").read_text(
            encoding="utf-8"))
    clock = {"now": now}

    class _Date:
        @staticmethod
        def now():
            return clock["now"]

    async def d1_all(env, sql, *params):
        return [dict(row) for row in db.execute(sql, params).fetchall()]

    async def d1_run(env, sql, *params):
        db.execute(sql, params)

    async def ensure_schema(env):
        return None

    def json_response(payload, status=200, cache_control=None):
        return status, payload

    module._bind_runtime({
        "Date": _Date,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "ensure_schema": ensure_schema,
        "json_response": json_response,
    })
    return module, db, clock


def _post(module, body, account=("alice-bi", {"name": "alice"})):
    async def _bounded_json_request(request):
        return body

    async def _session(env, request, data=None):
        return account

    module.bounded_json_request = _bounded_json_request
    module._account_session_record = _session
    return asyncio.run(module.account_activity_today_route(None, object()))


def _counts(**slots):
    counts = [0] * SLOTS
    for key, value in slots.items():
        counts[int(key[1:])] = value
    return counts


def test_entry_routes_activity_today_and_deletes_it_with_the_account():
    assert '"/api/accounts/activity-today" and method == "POST"' in ENTRY
    assert "_account_activity.account_activity_today_route(" in ENTRY
    assert '"account_activity_days",' in ENTRY


def test_utc_day_matches_the_test_clock():
    module, _, _ = _load()
    assert module._utc_day(NOW).isoformat() == TODAY


def test_each_device_sees_the_other_devices_counts():
    module, _, _ = _load()
    status, desktop = _post(module, {
        "day": TODAY, "device": "desktop-0001", "kind": "desktop",
        "counts": _counts(s100=5, s101=2)})
    assert status == 200
    assert desktop["others"] == [0] * SLOTS
    assert desktop["counts"] == _counts(s100=5, s101=2)
    status, phone = _post(module, {
        "day": TODAY, "device": "iphone-0001", "kind": "ios",
        "counts": _counts(s100=1, s200=4)})
    assert status == 200
    assert phone["others"] == _counts(s100=5, s101=2)
    assert phone["counts"] == _counts(s100=6, s101=2, s200=4)
    assert [(s["kind"], s["total"], s["self"]) for s in phone["sources"]] == [
        ("desktop", 7, False), ("ios", 5, True)]
    status, web = _post(module, {"day": TODAY, "device": "web-browser-1"})
    assert status == 200
    assert web["others"] == _counts(s100=6, s101=2, s200=4)
    assert web["nodeName"] == "alice"


def test_a_read_writes_nothing_and_a_rewrite_replaces_the_row():
    module, db, _ = _load()
    _post(module, {"day": TODAY, "device": "web-browser-1"})
    assert db.execute("SELECT COUNT(*) FROM account_activity_days").fetchone()[0] == 0
    _post(module, {"day": TODAY, "device": "desktop-0001",
                   "counts": _counts(s1=1)})
    _post(module, {"day": TODAY, "device": "desktop-0001",
                   "counts": _counts(s1=3)})
    rows = db.execute("SELECT device_key,counts FROM account_activity_days").fetchall()
    assert len(rows) == 1
    assert rows[0]["counts"].split(",")[1] == "3"


def test_accounts_do_not_see_each_other():
    module, _, _ = _load()
    _post(module, {"day": TODAY, "device": "desktop-0001",
                   "counts": _counts(s5=9)})
    status, other = _post(
        module, {"day": TODAY, "device": "desktop-0002"},
        account=("bob-bi", {"name": "bob"}))
    assert status == 200
    assert other["others"] == [0] * SLOTS
    assert other["sources"] == []


def test_rejects_bad_input_and_signed_out_requests():
    module, _, _ = _load()
    good = {"day": TODAY, "device": "desktop-0001", "counts": _counts()}
    assert _post(module, good, account=("", None))[0] == 401
    assert _post(module, {**good, "day": "2027-01-18"})[1]["error"] == "invalid_day"
    assert _post(module, {**good, "day": "2027-02-30"})[1]["error"] == "invalid_day"
    assert _post(module, {**good, "device": "short"})[1]["error"] == "invalid_device"
    assert _post(module, {**good, "device": "bad device!"})[1]["error"] == "invalid_device"
    assert _post(module, {**good, "counts": [1] * 10})[1]["error"] == "invalid_counts"
    assert _post(module, {**good, "counts": [True] * SLOTS})[1]["error"] == "invalid_counts"
    assert _post(module, {**good, "counts": [1.5] * SLOTS})[1]["error"] == "invalid_counts"
    status, payload = _post(module, {**good, "counts": [-4] + [10 ** 9] * (SLOTS - 1)})
    assert status == 200
    assert payload["counts"][0] == 0
    assert payload["counts"][1] == module.ACTIVITY_SLOT_MAX


def test_a_neighbouring_local_day_is_accepted():
    module, _, _ = _load()
    assert _post(module, {"day": "2027-01-14", "device": "desktop-0001"})[0] == 200
    assert _post(module, {"day": "2027-01-16", "device": "desktop-0001"})[0] == 200


def test_old_days_are_pruned_when_a_device_first_writes_a_day():
    module, db, clock = _load()
    _post(module, {"day": TODAY, "device": "desktop-0001",
                   "counts": _counts(s1=1)})
    clock["now"] = NOW + 4 * 24 * 60 * 60 * 1000
    _post(module, {"day": "2027-01-19", "device": "desktop-0001",
                   "counts": _counts(s2=1)})
    days = [row[0] for row in db.execute("SELECT day FROM account_activity_days")]
    assert days == ["2027-01-19"]


def test_device_count_per_day_is_capped():
    module, _, _ = _load()
    for index in range(module.ACTIVITY_DEVICES_PER_DAY):
        status, _ = _post(module, {"day": TODAY, "device": "device-%04d" % index,
                                   "counts": _counts(s1=1)})
        assert status == 200
    status, payload = _post(module, {"day": TODAY, "device": "device-extra",
                                     "counts": _counts(s1=1)})
    assert status == 429
    assert payload["error"] == "too_many_activity_devices"
    status, _ = _post(module, {"day": TODAY, "device": "device-0003",
                               "counts": _counts(s1=2)})
    assert status == 200
