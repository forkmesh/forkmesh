#!/usr/bin/env python3
"""Cron completion watchdog transition and runtime-safety contracts."""

import ast
import asyncio
from pathlib import Path
import re
from types import SimpleNamespace
from urllib.parse import quote, urlparse
from _maintenance_stubs import with_maintenance_off


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
WRANGLER = ROOT / "wrangler.toml"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
WRANGLER_TEXT = WRANGLER.read_text(encoding="utf-8")


def _load_watchdog():
    wanted_constants = {
        "CRON_WATCHDOG_GRACE_MS",
        "CRON_WATCHDOG_RETRY_MS",
        "STATUS_DEPLOY_GRACE_MS",
        "STATUS_DEPLOY_MAX_MS",
        "STATUS_ALERT_MANAGE_URL",
    }
    wanted_functions = {
        "_status_alert_manage_url",
        "_email_with_status_alert_manage_link",
        "_cron_watchdog_email_content",
        "_send_cron_watchdog_email",
        "_status_deploy_semaphore_active",
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
        elif isinstance(node, ast.ClassDef) and \
                node.name == "ForkMeshCronWatchdog":
            selected.append(node)

    sends = []

    class DurableObject:
        def __init__(self, ctx, env):
            self.ctx = ctx
            self.env = env

    class Date:
        value = 1_000_000

        @classmethod
        def now(cls):
            return cls.value

    async def _repository_monitor_admin_emails(_env):
        return ["admin@example.test"]

    async def _status_alert_emails_enabled(_env):
        return namespace["_alerts_enabled"]

    async def _send_email(
            _env, email, subject, text, html, **_delivery_metadata):
        sends.append((email, subject, text, html))
        return True

    async def _cloudflare_attention_log_tail(_env, _now):
        return "[999] ERROR scheduled simulated bounded log"

    async def d1_first(_env, _sql, *_args):
        return namespace.get("_deploy_row")

    def _attention_email_with_logs(text, html, log_tail):
        return (
            text + "\n\nCloudflare logs · prior 2 minutes\n" + log_tail,
            html + "Cloudflare logs · prior 2 minutes" + log_tail,
        )

    def _flagship_monitor_duration(milliseconds):
        return "%d minutes" % (int(milliseconds) // 60_000)

    def _html_escape(value):
        return str(value)

    def _forkmesh_email_card_html(heading, intro, body, footer=""):
        return heading + intro + body + footer

    def _admin_path(_env):
        return "/admin-test"

    def json_response(payload, status=200):
        return SimpleNamespace(status=status, payload=payload)

    namespace = {
        "DurableObject": DurableObject,
        "Date": Date,
        "re": re,
        "urlparse": urlparse,
        "quote": quote,
        "_repository_monitor_admin_emails":
            _repository_monitor_admin_emails,
        "_status_alert_emails_enabled": _status_alert_emails_enabled,
        # The flagship repo's admin switch, off in production until an org
        # admin opts in. The transition tests below exercise the on path.
        "_alerts_enabled": True,
        "_send_email": _send_email,
        "_cloudflare_attention_log_tail": _cloudflare_attention_log_tail,
        "d1_first": d1_first,
        "_deploy_row": None,
        "_attention_email_with_logs": _attention_email_with_logs,
        "_flagship_monitor_duration": _flagship_monitor_duration,
        "_html_escape": _html_escape,
        "_forkmesh_email_card_html": _forkmesh_email_card_html,
        "_admin_path": _admin_path,
        "json_response": json_response,
        "durable_object_traffic_note": lambda *_args, **_kwargs: None,
        "durable_object_traffic_flush":
            (lambda *_args, **_kwargs: asyncio.sleep(0)),
        "_sends": sends,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


class _Storage:
    def __init__(self):
        self.data = {}
        self.alarm_at = None

    async def get(self, key):
        return self.data.get(key)

    async def put(self, key, value):
        self.data[key] = value

    async def setAlarm(self, timestamp):
        self.alarm_at = int(timestamp)


def test_watchdog_alerts_once_and_recovers_once():
    ns = _load_watchdog()
    storage = _Storage()
    ctx = SimpleNamespace(storage=storage)
    watchdog = ns["ForkMeshCronWatchdog"](ctx, object())
    request = SimpleNamespace(
        url="https://forkmesh.internal/cron-watchdog/completed")
    Date = ns["Date"]
    grace = ns["CRON_WATCHDOG_GRACE_MS"]

    # First successful completion arms the independent alarm without email.
    response = asyncio.run(watchdog.fetch(request))
    assert response.status == 200
    assert storage.data["last_completion_at"] == Date.value
    assert storage.alarm_at == Date.value + grace
    assert ns["_sends"] == []

    # Three missed ticks produce exactly one outage message.
    Date.value += grace
    asyncio.run(watchdog.alarm())
    assert len(ns["_sends"]) == 1
    assert ns["_sends"][0][1].startswith("[ForkMesh outage]")
    assert "Cloudflare logs · prior 2 minutes" in ns["_sends"][0][2]
    assert "simulated bounded log" in ns["_sends"][0][2]
    assert storage.data["notified_state"] == "down"
    asyncio.run(watchdog.alarm())
    assert len(ns["_sends"]) == 1

    # The next fully completed cron tick produces one recovery and rearms.
    Date.value += 60_000
    asyncio.run(watchdog.fetch(request))
    assert len(ns["_sends"]) == 2
    assert ns["_sends"][1][1].startswith("[ForkMesh recovered]")
    assert "Cloudflare logs" not in ns["_sends"][1][2]
    assert storage.data["notified_state"] == "up"
    assert storage.data["outage_started_at"] == 0
    asyncio.run(watchdog.fetch(request))
    assert len(ns["_sends"]) == 2


def test_watchdog_alarm_honors_a_racing_newer_completion():
    ns = _load_watchdog()
    storage = _Storage()
    ctx = SimpleNamespace(storage=storage)
    watchdog = ns["ForkMeshCronWatchdog"](ctx, object())
    Date = ns["Date"]
    grace = ns["CRON_WATCHDOG_GRACE_MS"]
    storage.data["last_completion_at"] = Date.value

    Date.value += grace - 1
    asyncio.run(watchdog.alarm())
    assert storage.alarm_at == storage.data["last_completion_at"] + grace
    assert ns["_sends"] == []


def test_watchdog_defers_alerts_during_deploy_and_post_ready_grace():
    ns = _load_watchdog()
    storage = _Storage()
    ctx = SimpleNamespace(storage=storage)
    watchdog = ns["ForkMeshCronWatchdog"](ctx, object())
    Date = ns["Date"]
    watchdog_grace = ns["CRON_WATCHDOG_GRACE_MS"]
    deploy_grace = ns["STATUS_DEPLOY_GRACE_MS"]
    storage.data["last_completion_at"] = Date.value - watchdog_grace

    ns["_deploy_row"] = {
        "state": "deploying",
        "started_at": Date.value,
        "finished_at": 0,
    }
    asyncio.run(watchdog.alarm())
    assert ns["_sends"] == []
    assert storage.data.get("outage_started_at") is None
    assert storage.alarm_at == Date.value + watchdog_grace

    Date.value += watchdog_grace
    ns["_deploy_row"] = {
        "state": "ready",
        "started_at": Date.value - watchdog_grace,
        "finished_at": Date.value,
    }
    asyncio.run(watchdog.alarm())
    assert ns["_sends"] == []
    assert storage.data.get("outage_started_at") is None

    Date.value += deploy_grace
    asyncio.run(watchdog.alarm())
    assert len(ns["_sends"]) == 1


def test_watchdog_rearms_when_alert_delivery_is_unavailable():
    ns = _load_watchdog()
    storage = _Storage()
    ctx = SimpleNamespace(storage=storage)
    watchdog = ns["ForkMeshCronWatchdog"](ctx, object())
    Date = ns["Date"]
    grace = ns["CRON_WATCHDOG_GRACE_MS"]
    retry = ns["CRON_WATCHDOG_RETRY_MS"]
    storage.data["last_completion_at"] = Date.value - grace

    async def unavailable(_env):
        raise RuntimeError("D1 unavailable")

    ns["_repository_monitor_admin_emails"] = unavailable
    asyncio.run(watchdog.alarm())
    assert storage.data.get("notified_state") != "down"
    assert storage.alarm_at == Date.value + retry
    assert ns["_sends"] == []


def test_watchdog_sends_nothing_while_alert_mail_is_switched_off():
    ns = _load_watchdog()
    ns["_alerts_enabled"] = False
    storage = _Storage()
    ctx = SimpleNamespace(storage=storage)
    watchdog = ns["ForkMeshCronWatchdog"](ctx, object())
    request = SimpleNamespace(
        url="https://forkmesh.internal/cron-watchdog/completed")
    Date = ns["Date"]
    grace = ns["CRON_WATCHDOG_GRACE_MS"]
    retry = ns["CRON_WATCHDOG_RETRY_MS"]
    storage.data["last_completion_at"] = Date.value - grace

    # A suppressed outage still settles the transition: rearming the retry
    # alarm would spin forever on a send that is never going to happen.
    asyncio.run(watchdog.alarm())
    assert ns["_sends"] == []
    assert storage.data["notified_state"] == "down"
    assert storage.alarm_at != Date.value + retry

    # The later recovery is suppressed too, and clears the outage state.
    asyncio.run(watchdog.fetch(request))
    assert ns["_sends"] == []
    assert storage.data["notified_state"] == "up"
    assert storage.data["outage_started_at"] == 0


def test_status_alert_mail_is_off_until_a_repo_admin_enables_it():
    # Defaults stay off, and the shared send helper gates both alarm branches.
    defaults = ENTRY_TEXT.split("REPO_ALERT_SETTING_DEFAULTS = {", 1)[1] \
        .split("}", 1)[0]
    assert '"statusEmails": False' in defaults
    send = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _send_cron_watchdog_email")
        :ENTRY_TEXT.index("async def _cron_watchdog_completion")
    ]
    assert send.count("await _status_alert_emails_enabled(env)") == 1
    assert ENTRY_TEXT.count("await _send_cron_watchdog_email(") == 2
    assert "CREATE TABLE IF NOT EXISTS repo_alert_settings" in (
        ROOT / "src" / "schema.py").read_text(encoding="utf-8")


def test_cron_runtime_and_durable_object_are_wired():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    due_phases = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_cron_due_phases"
    ))
    phase_runner = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_run_cron_phase"
    ))
    runner = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef)
        and node.name == "ForkMeshCronRunner"
    )
    runner_methods = {
        node.name: ast.unparse(node) for node in runner.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    order = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_cron_tick_phase_order"
    ))
    assert "'watchdog-completion'" not in due_phases
    assert (
        "if phase == 'watchdog-completion':\n"
        "        return await _cron_watchdog_completion(env)"
    ) in phase_runner
    # The heartbeat is never "due"; it is appended last to every tick's phase
    # list so it rides the invocation that finishes the tick instead of
    # costing a Durable Object invocation of its own.
    assert "CRON_WATCHDOG_PHASE" in order
    assert order.rstrip().endswith(
        "return (chain + independent + (CRON_WATCHDOG_PHASE,), len(chain))")
    # The coordinator's alarm is the only path that runs phases, so nothing
    # can drift into sending the heartbeat for a tick that failed: the tick
    # skips it whenever anything before it failed.
    assert "_cron_tick_phase_order(minute_slot)" in runner_methods[
        "_coordinator_alarm"]
    tick = runner_methods["_run_tick_slice"]
    assert "phase == CRON_WATCHDOG_PHASE and (failed or failures)" in tick
    assert "skipped.append(phase)" in tick
    assert 'compatibility_date = "2026-09-22"' in WRANGLER_TEXT
    assert 'name = "FORKMESH_CRON_WATCHDOG"' in WRANGLER_TEXT
    assert 'class_name = "ForkMeshCronWatchdog"' in WRANGLER_TEXT
    assert 'tag = "v13"' in WRANGLER_TEXT
    assert 'new_sqlite_classes = ["ForkMeshCronWatchdog"]' in WRANGLER_TEXT
