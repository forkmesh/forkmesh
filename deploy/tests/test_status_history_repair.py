import json

import status_history_repair as shr
from next_common import Runner

H = shr.HOUR_MS
D = shr.DAY_MS
DAY = 1790208000000
RETIRED = frozenset({"mirror35"})


def minute(ts, **systems):
    return {"minute_ts": ts, "data": json.dumps({"schemaVersion": 1, "systems": {
        k.replace("_", ":", 1) if k.startswith("mirror_") else k: v for k, v in systems.items()}})}


def batch(ts_key, ts, systems):
    return {ts_key: ts, "data": shr.encode(systems)}


def test_retired_list_is_read_from_entry(tmp_path):
    entry = tmp_path / "entry.py"
    entry.write_text('X = 1\nSTATUS_RETIRED_MIRRORS = frozenset((\n    "mirror2", "mirror35",\n))\n')
    assert shr.retired_mirrors(entry) == frozenset({"mirror2", "mirror35"})


def test_hour_rebuilt_from_minutes_drops_retired_and_keeps_last_reason():
    rows = [minute(DAY + i * 60000, website={"ok": True, "reason": None},
                   mirror_mirror35={"ok": True, "reason": None}) for i in range(59)]
    rows.append(minute(DAY + 59 * 60000, website={"ok": False, "reason": "boom"}))
    hour = shr.hour_from_minutes(rows, DAY, RETIRED)
    assert hour == {"website": {"checks": 60, "failures": 1, "reason": "boom"}}


def test_plan_repairs_a_lossy_hour_with_compare_and_swap_and_never_lowers_coverage():
    minutes = [minute(DAY + i * 60000, website={"ok": True, "reason": None},
                      mirror_mirror35={"ok": True, "reason": None}) for i in range(60)]
    minutes += [minute(DAY + H + i * 60000, website={"ok": True, "reason": None}) for i in range(30)]
    lossy = batch("hour_ts", DAY, {"website": {"checks": 23, "failures": 0, "reason": None}})
    fuller = batch("hour_ts", DAY + H, {"website": {"checks": 60, "failures": 0, "reason": None}})
    changes = shr.plan(minutes, [lossy, fuller], [], DAY + 3 * H, RETIRED)
    assert [(c[0], c[1], c[4], c[5]) for c in changes] == [("hour", DAY, 23, 60)]
    stmt = shr.statements(changes)[0]
    assert stmt[0].startswith("UPDATE system_status_hourly_batch SET data=? WHERE hour_ts=? AND data=?")
    assert stmt[3] == lossy["data"]
    assert "mirror35" not in stmt[1]


def test_plan_inserts_missing_hours_only_with_insert_or_ignore():
    minutes = [minute(DAY + i * 60000, website={"ok": True, "reason": None}) for i in range(10)]
    changes = shr.plan(minutes, [], [], DAY + 2 * H, RETIRED)
    stmt = shr.statements(changes)
    assert len(stmt) == 1 and stmt[0][0].startswith("INSERT OR IGNORE INTO system_status_hourly_batch")


def test_day_rebuilt_only_when_every_hour_is_present_and_coverage_grows():
    hours = [batch("hour_ts", DAY + i * H, {"website": {"checks": 60, "failures": 0, "reason": None}})
             for i in range(24)]
    lossy_day = batch("day_ts", DAY, {"website": {"checks": 300, "failures": 0}})
    changes = shr.plan([], hours, [lossy_day], DAY + D + H, RETIRED)
    assert [(c[0], c[4], c[5]) for c in changes] == [("day", 300, 1440)]
    assert shr.plan([], hours[:23], [lossy_day], DAY + D + H, RETIRED) == []
    full_day = batch("day_ts", DAY, {"website": {"checks": 1440, "failures": 0},
                                     "mirror:mirror35": {"checks": 1000, "failures": 0}})
    assert shr.plan([], hours, [full_day], DAY + D + H, RETIRED) == []


def test_current_hour_and_day_are_left_to_the_app():
    minutes = [minute(DAY + i * 60000, website={"ok": True, "reason": None}) for i in range(10)]
    assert shr.plan(minutes, [], [], DAY + 30 * 60000, RETIRED) == []


class FakeDb:
    def __init__(self, tables):
        self.tables = tables
        self.writes = []

    def request(self, statements, transaction=False, level="strong"):
        if statements and statements[0][0].startswith("SELECT"):
            table = statements[0][0].split("FROM ")[1].strip()
            return [{"rows": self.tables.get(table, [])}]
        self.writes.append(statements)
        return [{"rows_affected": 1} for _ in statements]


def test_run_is_a_dry_run_without_execute():
    minutes = [minute(DAY + i * 60000, website={"ok": True, "reason": None}) for i in range(10)]
    db = FakeDb({"system_status_minute_batch": minutes})
    changes = shr.run(db, Runner(execute=False), now_ms=DAY + 2 * H, retired=RETIRED)
    assert changes and db.writes == []
    shr.run(db, Runner(execute=True), now_ms=DAY + 2 * H, retired=RETIRED)
    assert len(db.writes) == 1
