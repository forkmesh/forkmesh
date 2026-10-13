#!/usr/bin/env python3
"""Outage pings survive alongside recovery pings.

Pings are on by default and alert mail is not, so the ping channel needs its
own delivered-transition marker. Sharing the mail channel's notified_state left
every sample after a transition re-announcing it: recoveries key off a stamp
that moves each minute, so a new "recovered" row was written every cron tick
until the per-recipient cap trimmed the outage pings out of the inbox.
"""

import ast
import asyncio
import json
from pathlib import Path
import sqlite3
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("status_monitoring.py").read_text(encoding="utf-8")
)

SYSTEMS = [("edge_api", "Edge API"), ("flagship_repository", "Flagship repo")]


def _load(pings_enabled=True):
    wanted_constants = {
        "STATUS_DEPLOY_GRACE_MS", "STATUS_ALERT_CONTINUAL_INTERVAL_MS",
        "STATUS_ALERT_DELIVERY_BATCH", "STATUS_ALERT_CONTINUAL_BATCH",
        "STATUS_ALERT_ADMIN_BATCH",
        "STATUS_MONITOR_STATE_MEMO_TTL_MS", "STATUS_MONITOR_STATE_IN_LIST_MAX",
    }
    wanted_functions = {
        "_record_status_monitor_transitions",
        "_status_monitor_prior_rows", "_status_monitor_row_changed",
        "_status_monitor_rows_remember",
        "_enqueue_operational_alert_pings",
        "_record_operational_alert_pings_sent",
        "_status_recovery_ping_body",
    }
    selected = []
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if isinstance(node, ast.Assign):
            names = {
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            }
            if names & wanted_constants:
                selected.append(node)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted_functions:
                selected.append(node)

    monitors = {}
    pings = []
    state_writes = []

    async def d1_all(_env, sql, *_args):
        if "FROM users" in sql:
            return [{"data": "admin"}]
        if "FROM repository_monitor_state" in sql:
            return [dict(row) for row in monitors.values()]
        return []

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO repository_monitor_state"):
            state_writes.append((sql, args))
            for row in json.loads(args[0]):
                monitors[row["monitorId"]] = {
                    "monitor_id": row["monitorId"], "is_up": row["isUp"],
                    "changed_at": row["changedAt"],
                    "outage_started_at": row["outageStartedAt"],
                    "checked_at": row["checkedAt"], "reason": row["reason"],
                    "notified_state": row["notifiedState"],
                    "pinged_state": row["pingedState"],
                }
            return None
        if "SET pinged_state" in sql:
            state, monitor_id, is_up = args
            row = monitors.get(monitor_id)
            if row and int(row["is_up"]) == int(is_up):
                row["pinged_state"] = state
            return None
        return None

    # Flipped by the test that models an incident taking the notification
    # write down with everything else it broke.
    control = SimpleNamespace(enqueue_ok=True)

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        if not control.enqueue_ok:
            raise RuntimeError("D1 unavailable")
        pings.append({
            "recipient": recipient, "kind": kind, "title": title,
            "dedupe": kwargs.get("dedupe", ""),
            "body": kwargs.get("body", ""),
            "ts": kwargs.get("ts", 0),
            "state": (kwargs.get("meta") or {}).get("state", ""),
        })
        return True

    async def decrypt_row(_env, _data):
        return {"name": "admin"}

    async def _status_alert_pings_enabled(_env):
        return pings_enabled

    async def _status_alert_emails_enabled(_env):
        return False

    async def _repo_alert_settings_get(_env, _owner, _repo):
        return {"statusPings": pings_enabled, "statusEmails": False,
                "statusMonitors": {}}

    def _status_monitor_alert_setting(settings, system_id, channel):
        del system_id
        if channel == "pings":
            return bool(settings.get("statusPings", True))
        if channel == "emails":
            return bool(settings.get("statusEmails", False))
        return False

    namespace = {
        "json": json,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "enqueue_notification": enqueue_notification,
        "clean_string": lambda value, size: str(value or "")[:size],
        "valid_node_name": lambda value: bool(value),
        "MAX_NODE_NAME": 40,
        "STATUS_SYSTEMS": SYSTEMS,
        "STATUS_MIRROR_PREFIX": "mirror:",
        "STATUS_SAMPLE_WINDOW_MS": 60_000,
        "FLAGSHIP_MONITOR_ID": "forkmesh/forkmesh",
        "_repo_alert_settings_get": _repo_alert_settings_get,
        "_status_monitor_alert_setting": _status_monitor_alert_setting,
        "_status_alert_pings_enabled": _status_alert_pings_enabled,
        "_status_alert_emails_enabled": _status_alert_emails_enabled,
        "_test_control": control,
        "_test_state_writes": state_writes,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace, monitors, pings


def _sample(namespace, now, down=(), reason=""):
    ok = {system: system not in down for system, _ in SYSTEMS}
    reasons = {system: reason for system in down}
    asyncio.run(namespace["_record_status_monitor_transitions"](
        SimpleNamespace(), ok, reasons, now, SYSTEMS))


def test_one_ping_per_transition_in_both_directions():
    namespace, monitors, pings = _load()
    minute = 60_000

    _sample(namespace, 10 * minute)
    assert pings == []

    _sample(namespace, 11 * minute, down=("edge_api",), reason="502 from edge")
    assert [(ping["state"], ping["title"]) for ping in pings] == [
        ("down", "Edge API needs attention")]
    assert monitors["status:edge_api"]["pinged_state"] == "down"

    # A sustained outage repeats the same sample, not a new ping.
    _sample(namespace, 12 * minute, down=("edge_api",), reason="502 from edge")
    _sample(namespace, 13 * minute, down=("edge_api",), reason="502 from edge")
    assert len(pings) == 1

    _sample(namespace, 14 * minute)
    assert [(ping["state"], ping["title"]) for ping in pings] == [
        ("down", "Edge API needs attention"),
        ("up", "Edge API recovered"),
    ]
    assert monitors["status:edge_api"]["pinged_state"] == "up"

    # The regression: every later green sample used to enqueue another
    # "recovered" ping under a fresh dedupe key, burying the outage ping.
    for tick in range(15, 25):
        _sample(namespace, tick * minute)
    assert len(pings) == 2
    assert len({ping["dedupe"] for ping in pings}) == 2


def test_transition_json_upsert_executes_as_one_sqlite_statement():
    namespace, _monitors, _pings = _load()
    _sample(namespace, 10 * 60_000)
    writes = namespace["_test_state_writes"]
    assert len(writes) == 1
    sql, args = writes[0]
    assert len(args) == 1

    database = sqlite3.connect(":memory:")
    database.execute(
        "CREATE TABLE repository_monitor_state ("
        "monitor_id TEXT PRIMARY KEY,is_up INTEGER,changed_at INTEGER,"
        "outage_started_at INTEGER,checked_at INTEGER,reason TEXT,"
        "notified_state TEXT,pinged_state TEXT)"
    )
    database.execute(sql, args)
    rows = database.execute(
        "SELECT monitor_id,is_up FROM repository_monitor_state "
        "ORDER BY monitor_id"
    ).fetchall()
    assert rows == [
        ("status:edge_api", 1),
        ("status:flagship_repository", 1),
    ]


def test_a_lost_outage_ping_is_described_by_the_recovery():
    # The failure ping rides the platform the probe just found broken. If it is
    # lost, the bounded recovery notification still carries the outage duration
    # and last failure without doubling every administrator fanout.
    namespace, monitors, pings = _load()
    control = namespace["_test_control"]
    minute = 60_000

    _sample(namespace, 10 * minute)
    control.enqueue_ok = False
    _sample(namespace, 11 * minute, down=("edge_api",), reason="502 from edge")
    assert pings == []
    assert monitors["status:edge_api"]["pinged_state"] == ""

    control.enqueue_ok = True
    _sample(namespace, 13 * minute)
    assert [(ping["state"], ping["title"]) for ping in pings] == [
        ("up", "Edge API recovered"),
    ]
    assert "502 from edge" in pings[0]["body"]
    assert "Down for 2 minutes" in pings[0]["body"]

    # And it stays a one-shot: later green samples add nothing.
    _sample(namespace, 14 * minute)
    assert len(pings) == 1


def test_a_resent_recovery_ping_does_not_report_uptime_as_downtime():
    # An undelivered recovery is retried by every later green sample. Those
    # samples are not transitions, so they know no outage window: the duration
    # must come from the flip itself, never from "time since the recovery".
    namespace, monitors, pings = _load()
    control = namespace["_test_control"]
    minute = 60_000

    _sample(namespace, 10 * minute)
    _sample(namespace, 11 * minute, down=("edge_api",), reason="502 from edge")
    assert monitors["status:edge_api"]["pinged_state"] == "down"

    control.enqueue_ok = False
    _sample(namespace, 12 * minute)
    control.enqueue_ok = True
    _sample(namespace, 20 * minute)

    recoveries = [ping for ping in pings if ping["state"] == "up"]
    assert len(recoveries) == 1
    assert "Down for" not in recoveries[0]["body"]


def test_ping_state_is_only_recorded_once_delivered():
    namespace, monitors, pings = _load(pings_enabled=False)
    _sample(namespace, 10 * 60_000)
    _sample(namespace, 11 * 60_000, down=("edge_api",), reason="502")
    assert pings == []
    # Nothing was delivered, so turning Pings on later still announces what is
    # red instead of the switch silently swallowing the current outage.
    assert monitors["status:edge_api"]["pinged_state"] == ""


def test_flagship_repository_outage_pings_on_the_first_failed_probe():
    namespace, monitors, pings = _load()
    _sample(namespace, 10 * 60_000)
    _sample(namespace, 11 * 60_000, down=("flagship_repository",))
    assert [(ping["state"], ping["title"]) for ping in pings] == [
        ("down", "Flagship repo needs attention")]
    assert monitors["status:flagship_repository"]["pinged_state"] == "down"


def test_every_down_core_is_pinged_continually_in_the_same_minute():
    namespace, monitors, pings = _load()
    cores = [("core:n%d" % index, "Core node — n%d" % index)
             for index in range(1, 4)]
    namespace["_status_monitor_alert_setting"] = (
        lambda settings, system_id, channel:
        channel == "pings"
        or (channel == "continual" and system_id.startswith("core:")))
    interval = namespace["STATUS_ALERT_CONTINUAL_INTERVAL_MS"]

    def sample(now, down):
        ok = {system: system not in down for system, _ in cores}
        reasons = {system: system + " is sealed" for system in down}
        asyncio.run(namespace["_record_status_monitor_transitions"](
            SimpleNamespace(), ok, reasons, now, cores))

    down = {system for system, _ in cores}
    start = 10 * interval + 60_000
    for minute in range(0, 3):
        sample(start + minute * 60_000, down)
    assert {ping["title"] for ping in pings} == {
        label + " needs attention" for _system, label in cores}
    before = len(pings)
    for minute in range(3, interval // 60_000 + 1):
        sample(start + minute * 60_000, down)
    repeated = pings[before:]
    assert sorted(ping["title"] for ping in repeated) == sorted(
        label + " needs attention" for _system, label in cores)
    assert len({ping["dedupe"] for ping in pings}) == len(pings)
    assert all(monitors["status:" + system]["pinged_state"] == "down"
               for system, _ in cores)
