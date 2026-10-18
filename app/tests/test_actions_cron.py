import asyncio
import calendar
import json
from pathlib import Path
import sys

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(TESTS.parent / "src"))

import actions_cron  # noqa: E402
from test_runner_leases import FakeRuntime  # noqa: E402


def slot(year, month, day, hour, minute):
    return calendar.timegm((year, month, day, hour, minute, 0)) // 60


def fires(expression, *moment):
    spec = actions_cron.parse(expression)
    assert spec is not None, expression
    return actions_cron.matches(spec, slot(*moment))


def test_fields_lists_ranges_steps_and_aliases():
    assert fires("17 3 * * *", 2026, 10, 17, 3, 17)
    assert not fires("17 3 * * *", 2026, 10, 17, 4, 17)
    assert fires("*/15 * * * *", 2026, 10, 17, 9, 45)
    assert not fires("*/15 * * * *", 2026, 10, 17, 9, 46)
    assert fires("0 9-17/4 * * 1-5", 2026, 10, 16, 13, 0)
    assert not fires("0 9-17/4 * * 1-5", 2026, 10, 17, 13, 0)
    assert fires("0 0 * * 7", 2026, 10, 18, 0, 0)
    assert fires("0,30 12 1,15 * *", 2026, 10, 15, 12, 30)
    assert fires("5/20 * * * *", 2026, 10, 15, 12, 45)
    assert fires("@hourly", 2026, 10, 15, 7, 0)
    assert fires("@daily", 2026, 10, 15, 0, 0)
    assert fires("@weekly", 2026, 10, 18, 0, 0)
    assert not fires("@weekly", 2026, 10, 17, 0, 0)
    assert fires("@monthly", 2026, 11, 1, 0, 0)
    assert fires("0 0 13 * 5", 2026, 10, 16, 0, 0)
    assert fires("0 0 13 * 5", 2026, 10, 13, 0, 0)


def test_invalid_and_too_frequent_expressions_are_refused():
    for bad in ("", "* * * *", "60 * * * *", "* 24 * * *", "0 0 0 * *",
                "0 0 * 13 *", "0 0 * * 8", "a * * * *", "5-1 * * * *",
                "*/0 * * * *", "* * * * *", "*/4 * * * *", "0,3 * * * *",
                "58,1 * * * *", "@yearly", "x" * 101):
        assert actions_cron.parse(bad) is None, bad
    assert actions_cron.valid("*/5 * * * *")
    assert actions_cron.valid("0 */2 * * *")


def test_due_skips_recent_slots_and_takes_the_first_matching_schedule():
    now = slot(2026, 10, 17, 3, 15)
    rows = [
        {"path": "a", "schedules": json.dumps(["*/5 * * * *", "15 3 * * *"]),
         "last_slot": 0},
        {"path": "b", "schedules": ["*/5 * * * *"], "last_slot": now - 4},
        {"path": "c", "schedules": "not json", "last_slot": 0},
        {"path": "d", "schedules": ["0 4 * * *"], "last_slot": 0},
    ]
    assert [(row["path"], expression)
            for row, expression in actions_cron.due(rows, now)] == [
        ("a", "*/5 * * * *")]


def test_run_claims_each_slot_once_and_enqueues_schedule_jobs():
    runtime = FakeRuntime()
    now = slot(2026, 10, 17, 3, 17)
    runtime.db.execute(
        "INSERT INTO repo_action_workflows (owner,repo,path,schedules,"
        "events,updated_at) VALUES ('forkmesh','site','.forkmesh/n.yml',?,"
        "'[\"schedule\"]',1)", (json.dumps(["17 3 * * *"]),))
    runtime.db.commit()
    assert asyncio.run(actions_cron.run(runtime, now)) == 1
    assert asyncio.run(actions_cron.run(runtime, now)) == 0
    row = runtime.db.execute(
        "SELECT branch,commit_oid,event,workflow,payload,trusted "
        "FROM repo_action_jobs").fetchone()
    assert tuple(row)[:4] == ("main", "", "schedule", ".forkmesh/n.yml")
    assert json.loads(row["payload"])["event"] == {"schedule": "17 3 * * *"}
    assert row["trusted"] == 1
    assert runtime.db.execute(
        "SELECT last_slot FROM repo_action_workflows").fetchone()[0] == now
