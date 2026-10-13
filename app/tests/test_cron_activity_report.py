#!/usr/bin/env python3
"""The admin cron report: published schedule, tick history, and settings.

``src/cron_activity.py`` is lazily imported, so nothing in the served Worker
exercises it until an administrator opens the desktop Cron tab. These tests
stand in for that: they prove the module binds against entry.py's real
runtime, that the schedule it publishes is the schedule ``_cron_due_phases``
actually runs, and that the summary it reports is arithmetic on the retained
rows rather than a guess.
"""

import ast
import asyncio
import importlib.util
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ACTIVITY = ROOT / "src" / "cron_activity.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)

# Everything cron_activity reads out of entry.py's globals, plus what
# _cron_due_phases itself needs to answer a minute.
WANTED_CONSTANTS = {
    "CRON_RUNNER_INTERVAL_MS",
    "CRON_RUNNER_NAME",
    "CRON_BATCH_MAX_OBJECTS",
    "CRON_BATCH_MAX_PHASES",
    "CRON_BATCH_SUBREQUEST_CAP",
    "CRON_BATCH_RESERVE_HEADROOM",
    "CRON_PHASE_D1_DEDUPED",
    "CRON_PHASE_D1_QUERY_LIMIT",
    "CRON_PHASE_REPLAY_SAFE",
    "CRON_STATUS_CHAIN",
    "CRON_TICK_RETAIN_MS",
    "CRON_WATCHDOG_GRACE_MS",
    "CRON_WATCHDOG_RETRY_MS",
    "STATUS_MIRROR_PREFIX",
    "STATUS_CORE_PREFIX",
    "STATUS_RUNNER_PREFIX",
    "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES",
    "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES",
    "WORLD_SATELLITE_REFRESH_CRON_MINUTES",
    "WORLD_SATELLITE_REFRESH_CRON_OFFSET",
    "FLAGSHIP_CHECK_INTERVAL_MINUTES",
}
WANTED_FUNCTIONS = {"_cron_due_phases", "_status_monitor_alert_setting"}


def _entry_runtime(rows):
    """entry.py's real cron constants, with D1 replaced by fixed rows."""
    selected = []
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if isinstance(node, ast.Assign):
            names = {
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            }
            if names & WANTED_CONSTANTS:
                selected.append(node)
        elif (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in WANTED_FUNCTIONS
        ):
            selected.append(node)
    namespace = {}
    exec(
        compile(
            ast.fix_missing_locations(
                ast.Module(body=selected, type_ignores=[])),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    missing = (WANTED_CONSTANTS | WANTED_FUNCTIONS) - set(namespace)
    assert not missing, "entry.py no longer defines: " + ", ".join(
        sorted(missing))

    calls = []

    async def d1_all(_env, sql, *binds):
        calls.append((sql, binds))
        return list(rows)

    namespace["d1_all"] = d1_all
    namespace["json"] = json
    # The report is admin-only in a Worker that has already built the same
    # roster for /status; the module must not rebuild it.
    namespace["STATUS_ALERT_ADMIN_SYSTEMS"] = [
        ("website", "Website"),
        ("mirror:*", "All mirror nodes"),
        ("core:*", "All core nodes"),
    ]
    return namespace, calls


def _activity(rows=()):
    spec = importlib.util.spec_from_file_location("cron_activity", ACTIVITY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    runtime, calls = _entry_runtime(rows)
    module._bind_runtime(runtime)
    return module, runtime, calls


def _tick(slot, ts, *, ran, failed=None, skipped=(), batches=2, subrequests=17):
    detail = {
        "ran": ran,
        "failed": failed or {},
        "skipped": list(skipped),
    }
    return {
        "slot": slot,
        "ts": ts,
        "batches": batches,
        "phases": len(ran),
        "failures": len(failed or {}),
        "subrequests": subrequests,
        "ms": 900,
        "data": json.dumps(detail),
    }


def test_published_catalog_is_exactly_what_the_scheduler_runs():
    # The tab's whole claim is that it shows the real schedule. 180 minutes is
    # the full cycle: the slowest cadence (world-satellites) is three hours,
    # and every other period divides it.
    module, runtime, _calls = _activity()
    catalog = {
        phase: (period, offsets)
        for phase, period, offsets, _group, _description
        in module.CRON_PHASE_CATALOG
    }
    assert len(catalog) == len(module.CRON_PHASE_CATALOG), "duplicate phase"
    for minute in range(180 * 4):
        due = set(runtime["_cron_due_phases"](minute))
        published = {
            phase for phase, (period, offsets) in catalog.items()
            if minute % period in offsets
        }
        assert due == published, (
            "minute %d: scheduler %s, published %s"
            % (minute, sorted(due - published), sorted(published - due)))


def test_every_catalog_phase_is_one_the_runner_can_actually_run():
    # A published phase the dispatcher would reject as unknown is a lie in the
    # tab; the watchdog heartbeat is the one phase that is never "due".
    module, runtime, _calls = _activity()
    reachable = set()
    for minute in range(180):
        reachable.update(runtime["_cron_due_phases"](minute))
    published = {row[0] for row in module.CRON_PHASE_CATALOG}
    assert published == reachable
    assert "watchdog-completion" not in published


def test_cadence_labels_read_as_the_arithmetic_they_describe():
    module, _runtime, _calls = _activity()
    assert module._cadence_label(1, (0,)) == "every minute"
    assert module._cadence_label(5, (2,)) == "every 5 minutes"
    assert module._cadence_label(60, (7,)) == "hourly at :07"
    assert module._cadence_label(180, (11,)) == "every 3 hours"
    assert module._cadence_label(60, (0, 1, 30)) == "3 minutes of every 60"
    # Next-due is a subtraction, not a scan, and never returns the slot the
    # reader is already standing on.
    assert module._next_due_slot(100, 1, (0,)) == 101
    assert module._next_due_slot(100, 10, (5,)) == 105
    assert module._next_due_slot(105, 10, (5,)) == 115
    assert module._next_due_slot(100, 60, (0, 30)) == 120


def test_report_folds_retained_ticks_into_per_phase_state():
    rows = [
        _tick(
            1001, 60_060_000,
            ran={
                "status-sample": {"ms": 40, "sub": 6, "batch": 0},
                "mirror-health": {"ms": 90, "sub": 11, "batch": 0},
            },
            failed={"ap-outbox": "delivery timeout"},
            skipped=["watchdog-completion"],
            batches=2, subrequests=17),
        _tick(
            1000, 60_000_000,
            ran={"status-sample": {"ms": 25, "sub": 9, "batch": 0}},
            batches=1, subrequests=9),
    ]
    module, _runtime, calls = _activity(rows)
    report = asyncio.run(
        module.cron_activity_report(object(), 60_120_000, 120, {}))

    assert report["ok"] is True
    assert calls and "FROM cron_tick" in calls[0][0]
    assert calls[0][1] == (120,), "the report must bind its own row limit"

    summary = report["summary"]
    assert summary["ticks"] == 2
    assert summary["batches"] == 3 and summary["phases"] == 3
    assert summary["failedTicks"] == 1
    assert summary["subrequests"] == 26
    assert summary["objectsPerTick"] == 1.5
    # What the same phases would have cost as one object each, which is what
    # the loop did before batching: three phases plus the watchdog hop.
    assert summary["unbatchedObjectsPerTick"] == 2.5
    assert summary["lastTickAt"] == 60_060_000
    assert summary["gapMs"] == 60_000

    phases = {row["phase"]: row for row in report["phases"]}
    sample = phases["status-sample"]
    assert sample["runs"] == 2
    assert sample["lastRunAt"] == 60_060_000 and sample["lastMs"] == 40
    # High water, not the last sample: the older tick was the expensive one.
    assert sample["lastSubrequests"] == 6 and sample["maxSubrequests"] == 9
    assert "status chain" in sample["notes"]
    assert phases["ap-outbox"]["failures"] == 1
    assert phases["ap-outbox"]["lastFailure"] == "delivery timeout"
    assert phases["mirror-health"]["runs"] == 1
    # Untouched phases still appear with their cadence, so the tab shows the
    # whole schedule rather than only what happened to run.
    assert phases["chat-prune"]["runs"] == 0
    assert phases["chat-prune"]["cadence"] == "hourly at :37"

    ticks = report["ticks"]
    assert [tick["slot"] for tick in ticks] == [1001, 1000]
    assert ticks[0]["ran"] == ["mirror-health", "status-sample"]
    assert ticks[0]["failed"] == ["ap-outbox"]
    assert ticks[0]["skipped"] == ["watchdog-completion"]
    assert ticks[0]["detail"]["status-sample"]["sub"] == 6


def test_report_survives_an_unreadable_or_empty_tick_row():
    # A truncated detail body (the dispatcher's overflow shape) carries names
    # only, and a row whose JSON never parses must not take the tab down.
    rows = [
        {
            "slot": 9, "ts": 500, "batches": 1, "phases": 1, "failures": 0,
            "subrequests": 3, "ms": 10, "data": "{not json",
        },
        _tick(8, 400, ran=["status-sample"], batches=1),
    ]
    module, _runtime, _calls = _activity(rows)
    report = asyncio.run(module.cron_activity_report(object(), 600, 10, None))
    assert report["summary"]["ticks"] == 2
    phases = {row["phase"]: row for row in report["phases"]}
    # The truncated list shape still counts the run; it just has no timings.
    assert phases["status-sample"]["runs"] == 1
    assert phases["status-sample"]["lastMs"] == 0


def test_report_with_no_history_still_publishes_schedule_and_settings():
    module, _runtime, _calls = _activity([])
    report = asyncio.run(module.cron_activity_report(object(), 60_000, 5, {}))
    assert report["ticks"] == []
    assert report["summary"]["ticks"] == 0
    assert report["summary"]["objectsPerTick"] == 0.0
    assert len(report["phases"]) == len(module.CRON_PHASE_CATALOG)
    assert report["trigger"]["cron"] == "* * * * *"
    assert report["settings"]


def test_settings_rows_report_the_live_alert_channels():
    module, _runtime, _calls = _activity()
    rows = {row["key"]: row for row in module.cron_settings_rows({
        "statusPings": True,
        "statusEmails": False,
        "statusMonitors": {"website": {"emails": True, "continual": True}},
    })}
    assert rows["trigger"]["value"] == "* * * * *"
    assert "phases per" in rows["batching"]["value"]
    assert rows["alerts:website"]["value"] == "ping, email, continual"
    # A monitor with no override falls back to the account defaults, and a
    # channel that is off is simply absent rather than reported as enabled.
    assert rows["alerts:mirror:*"]["value"] == "ping"
    off = {row["key"]: row for row in module.cron_settings_rows({
        "statusPings": False, "statusEmails": False,
    })}
    assert off["alerts:mirror:*"]["value"] == "off"
    assert rows["alerts:core:*"]["value"] == "ping, continual"
    assert off["alerts:core:*"]["value"] == "continual"


def _report_namespace(authorized, report=None, error=None):
    """`_admin_cron_report` with its collaborators replaced by fakes."""
    seen = {}

    async def fake_authorized(_env, node, ts, _sig, canonical):
        seen["canonical"] = canonical
        seen["node"] = node
        seen["ts"] = ts
        return {"name": "admin"} if authorized else None

    async def fake_ensure_schema(_env):
        seen["ensured"] = True

    async def fake_alert_settings(_env, owner, repo):
        seen["settings_for"] = (owner, repo)
        return {"statusPings": True}

    async def fake_report(_env, now, limit, settings=None):
        seen["report"] = (now, limit, settings)
        if error is not None:
            raise error
        return dict(report or {"ok": True})

    responses = []

    def fake_json_response(body, status=200, cache_control=None):
        responses.append(
            {"body": body, "status": status, "cache_control": cache_control})
        return responses[-1]

    selected = []
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if isinstance(node, ast.Assign):
            names = {
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            }
            if names & {"CRON_TICK_REPORT_LIMIT", "FLAGSHIP_MONITOR_ID"}:
                selected.append(node)
        elif (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == "_admin_cron_report"
        ):
            selected.append(node)
    parse = importlib.import_module("urllib.parse")
    namespace = {
        "parse_qs": parse.parse_qs,
        "urlparse": parse.urlparse,
        "clean_string": lambda value, _max: str(value or ""),
        "MAX_NODE_NAME": 64,
        "_admin_authorized": fake_authorized,
        "ensure_schema": fake_ensure_schema,
        "_repo_alert_settings_get": fake_alert_settings,
        "cron_activity_report": fake_report,
        "cron_settings_rows": lambda settings: [
            {"key": "trigger", "value": "* * * * *", "settings": settings}],
        "json_response": fake_json_response,
        "_safe_error_text": str,
        "Date": type("Date", (), {"now": staticmethod(lambda: 1_000)}),
    }
    exec(
        compile(
            ast.fix_missing_locations(
                ast.Module(body=selected, type_ignores=[])),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    request = type("Request", (), {
        "url": "https://app.forkmesh.com/api/accounts/admin-cron-report"
               "?node=admin&ts=1&sig=x",
    })()
    return namespace, request, responses, seen


def test_cron_report_endpoint_requires_an_admin_signature():
    namespace, request, responses, seen = _report_namespace(False)
    asyncio.run(namespace["_admin_cron_report"](object(), request))
    assert responses[-1]["status"] == 401
    # Its own canonical string, so an admin signature minted for one report
    # cannot be replayed against another.
    assert seen["canonical"].startswith(b"forkmesh-admin-cron-report-v1\n")
    assert seen["canonical"] == b"forkmesh-admin-cron-report-v1\nadmin\n1"
    assert "report" not in seen, "an unsigned caller must not reach D1"


def test_cron_report_answers_with_the_bounded_history_window():
    namespace, request, responses, seen = _report_namespace(
        True, report={"ok": True, "ticks": []})
    asyncio.run(namespace["_admin_cron_report"](object(), request))
    assert responses[-1]["status"] == 200
    assert responses[-1]["body"]["ok"] is True
    assert seen["ensured"] is True
    assert seen["settings_for"] == ("forkmesh", "forkmesh")
    assert seen["report"][1] == namespace["CRON_TICK_REPORT_LIMIT"]
    assert seen["report"][2] == {"statusPings": True}
    assert "no-store" in responses[-1]["cache_control"]


def test_cron_report_still_renders_when_the_tick_table_is_missing():
    # A database that has not applied the migration yet must still answer with
    # the schedule and the settings rather than an empty tab.
    namespace, request, responses, _seen = _report_namespace(
        True, error=RuntimeError("no such table: cron_tick"))
    asyncio.run(namespace["_admin_cron_report"](object(), request))
    body = responses[-1]["body"]
    assert responses[-1]["status"] == 200
    assert body["ok"] is True
    assert body["error"] == "history_unavailable"
    assert "cron_tick" in body["detail"]
    assert body["ticks"] == [] and body["settings"]


def test_cron_report_route_is_dispatched():
    assert ('if url.path == "/api/accounts/admin-cron-report" '
            'and method == "GET":') in ENTRY_TEXT
    assert "await _admin_cron_report(env, request)" in ENTRY_TEXT
