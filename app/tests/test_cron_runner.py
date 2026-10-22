#!/usr/bin/env python3
"""Bounded CronRunner phase and rollout-bridge reliability contracts."""

import ast
import asyncio
import json
import re
import tomllib
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse
from _maintenance_stubs import with_maintenance_off


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
SOLANA = ROOT / "src" / "solana.py"
WRANGLER = ROOT / "wrangler.toml"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
SOLANA_TEXT = SOLANA.read_text(encoding="utf-8")
WRANGLER_TEXT = WRANGLER.read_text(encoding="utf-8")


class _Storage:
    def __init__(
            self, *, retire_after_set_alarm=False, fail_put_keys=()):
        self.data = {}
        self.alarm_at = None
        self.events = []
        self.delete_alarm_calls = 0
        self.retire_after_set_alarm = retire_after_set_alarm
        self.fail_put_keys = set(fail_put_keys)

    async def get(self, key):
        self.events.append(("get", key))
        return self.data.get(key)

    async def put(self, key, value):
        self.events.append(("put", key, value))
        if key in self.fail_put_keys:
            raise RuntimeError("transient storage handoff")
        self.data[key] = value

    async def setAlarm(self, timestamp):
        self.alarm_at = int(timestamp)
        self.events.append(("setAlarm", self.alarm_at))
        if self.retire_after_set_alarm:
            self.data["retired_v2"] = True

    async def getAlarm(self):
        self.events.append(("getAlarm",))
        return self.alarm_at

    async def deleteAlarm(self):
        self.alarm_at = None
        self.delete_alarm_calls += 1
        self.events.append(("deleteAlarm",))


def _load_runner(run_jobs=None, run_phase=None):
    wanted_constants = {
        "CRON_RUNNER_INTERVAL_MS",
        "CRON_RUNNER_KICK_DELAY_MS",
        "CRON_RUNNER_ALARM_OFFSET_MS",
        "CRON_TRIGGER_KICK_TIMEOUT_MS",
        "CRON_RUNNER_NAME",
        "APP_INGRESS_NAME_PREFIX",
        "APP_GIT_INGRESS_NAME_PREFIX",
        "APP_INGRESS_GIT_READ_PURPOSE",
        "APP_INGRESS_NODE_AUTH_PURPOSE",
        "APP_INGRESS_WEBSOCKET_AUTH_PURPOSE",
        "CRON_PHASE_D1_DEDUPED",
        "CRON_PHASE_REPLAY_SAFE",
        "CRON_PHASE_RETRY_DELAY_MS",
        "CRON_TICK_STATE_KEY",
        "CRON_TICK_COST_KEY",
        "CRON_TICK_DETAIL_MAX_CHARS",
        "CRON_TICK_RETAIN_MS",
        "CRON_DISPATCH_TIMEOUT_MS",
        "CRON_BATCH_SUBREQUEST_CAP",
        "CRON_BATCH_RESERVE_HEADROOM",
        "CRON_BATCH_MAX_OBJECTS",
        "CRON_BATCH_MAX_PHASES",
        "CRON_STATUS_CHAIN",
        "CRON_WATCHDOG_PHASE",
        "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES",
        "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES",
        "FLAGSHIP_CHECK_INTERVAL_MINUTES",
        "CRON_PHASE_D1_QUERY_LIMIT",
        "_CRON_D1_BUDGET_VAR",
        "WORLD_SATELLITE_REFRESH_CRON_MINUTES",
        "WORLD_SATELLITE_REFRESH_CRON_OFFSET",
        "SNAPSHOT_REPUBLISH_NAME",
        "SNAPSHOT_REPUBLISH_PATH",
        "SNAPSHOT_REPUBLISH_REQUESTED_PREFIX",
        "SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX",
        "SNAPSHOT_REPUBLISH_LEAD_MS",
        "SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS",
        "SNAPSHOT_REPUBLISH_DOORBELL_TIMEOUT_MS",
        "SNAPSHOT_REPUBLISH_STATUS_PATHS",
        "SNAPSHOT_REPUBLISH_PATHS",
        "SNAPSHOT_REPUBLISH_DOORBELL_PATHS",
        "MIRROR_WAKE_NAME",
        "MIRROR_WAKE_PATH",
        "MIRROR_WAKE_REQUESTED_PREFIX",
        "MIRROR_WAKE_LEAD_MS",
        "MIRROR_WAKE_RETRY_MS",
    }
    wanted_functions = {
        "_cron_d1_budget_start",
        "_cron_d1_budget_stop",
        "_cron_d1_query_charge",
        "_cron_subrequest_charge",
        "_cron_due_phases",
        "_cron_tick_phase_order",
        "_cron_tick_detail",
        "_cron_tick_record",
        "_republish_ready_at",
        "_republish_group",
        "js_nullish",
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
        elif (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in wanted_functions
        ):
            selected.append(node)
        elif isinstance(node, ast.ClassDef) and node.name == (
            "ForkMeshCronRunner"
        ):
            selected.append(node)

    class DurableObject:
        def __init__(self, ctx, env):
            self.ctx = ctx
            self.env = env

    class Date:
        value = 1_020_000

        @classmethod
        def now(cls):
            return cls.value

    del run_jobs

    if run_phase is None:
        async def run_phase(_env, _phase, _minute_slot):
            return True

    class Default:
        pass

    writes = []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    def json_response(payload, status=200, **_kwargs):
        return SimpleNamespace(status=status, payload=payload)

    class Scheduler:
        waits = []

        @classmethod
        async def wait(cls, value):
            cls.waits.append(value)

    namespace = {
        "DurableObject": DurableObject,
        "Date": Date,
        "AppRuntime": Default,
        "json": json,
        "json_response": json_response,
        "js_scheduler": Scheduler,
        "parse_qs": parse_qs,
        "re": re,
        "safe_segment": lambda value: (
            str(value) if re.fullmatch(r"[A-Za-z0-9._-]+", str(value))
            else None
        ),
        "clean_string": lambda value, limit: str(value).strip()[:limit],
        "urlparse": urlparse,
        "_run_cron_phase": run_phase,
        "_safe_error_text": str,
        "d1_run": d1_run,
        "_d1_writes": writes,
        "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES": 3,
        "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES": 5,
        "WORLD_SATELLITE_REFRESH_CRON_MINUTES": 180,
        "WORLD_SATELLITE_REFRESH_CRON_OFFSET": 11,
        "FLAGSHIP_CHECK_INTERVAL_MINUTES": 6 * 60,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


def _runner(ns, object_name, storage=None, env=None):
    storage = storage or _Storage()
    ctx = SimpleNamespace(
        storage=storage,
        id=SimpleNamespace(name=object_name),
    )
    return ns["ForkMeshCronRunner"](ctx, env or object()), storage


def _request(path):
    return SimpleNamespace(url="https://forkmesh.internal" + path)


def test_python_app_serializes_scheduled_work_behind_coordinator_alarm():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    default = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "Default")
    runner = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef)
        and node.name == "ForkMeshCronRunner")
    runner_methods = {
        node.name: ast.unparse(node) for node in runner.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    scheduled = ast.unparse(next(
        node for node in default.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "scheduled"))
    module_functions = {
        node.name for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    order = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_cron_tick_phase_order"))
    run_phase = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_run_cron_phase"))

    assert "_cron_runner_kick" not in module_functions
    assert "[triggers]" in WRANGLER_TEXT
    assert 'crons = ["* * * * *"]' in WRANGLER_TEXT
    assert "record_status_sample" not in scheduled
    assert "/cron-runner/kick" in scheduled
    assert "CRON_TRIGGER_KICK_TIMEOUT_MS" in scheduled
    assert "JsPromise.allSettled" not in scheduled
    assert "independent_settled_promise" not in scheduled
    assert "/cron-runner/retire" not in scheduled

    # The fan-out is gone for good: no batch, per-phase or disposable status
    # objects, and no dispatcher that would wait on them. Every phase runs
    # inside the coordinator's own alarm, which continues itself with a fresh
    # subrequest budget when one invocation cannot hold the rest.
    for retired in (
        "_cron_dispatch_tick", "_cron_batch_request", "_cron_batch_fetch",
        "_cron_trigger_status_fetch", "_cron_runner_phase_fetch",
    ):
        assert retired not in module_functions, retired
    for retired in ("_batch_fetch", "_run_batch", "_phase_fetch",
                    "_trigger_status_fetch", "_claim_phases"):
        assert retired not in runner_methods, retired
    assert "scheduled-batch-" not in ENTRY_TEXT
    assert "scheduled-phase-" not in ENTRY_TEXT
    assert "scheduled-status-" not in ENTRY_TEXT
    alarm = runner_methods["_coordinator_alarm"]
    tick = runner_methods["_run_tick_slice"]
    assert "_cron_tick_phase_order(minute_slot)" in alarm
    assert "_run_tick_slice(state)" in alarm
    assert "setAlarm(int(Date.now()))" in alarm
    assert "_cron_tick_record" in alarm
    assert "'cron phase failures: '" in alarm
    assert "_cron_d1_budget_start()" in tick
    assert "CRON_DISPATCH_TIMEOUT_MS" in tick
    assert "CRON_BATCH_MAX_OBJECTS" in tick
    assert "_phase_reserve(phase, costs)" in tick
    assert "CRON_WATCHDOG_PHASE" in tick
    # The status chain leads and the completion heartbeat rides last so it
    # never needs an invocation of its own.
    assert order.index("CRON_STATUS_CHAIN") < order.index(
        "CRON_WATCHDOG_PHASE")
    assert "_cron_due_phases(minute_slot)" in order
    assert "record_status_sample" in run_phase
    assert "allow_deep_probes=False" in run_phase
    assert "deliver_alerts=False" in run_phase
    assert "run_compaction=False" in run_phase
    assert "force_probe=True" in run_phase
    assert "include_general_chat=False" in run_phase

    assert 'name = "FORKMESH_CRON_RUNNER"' in WRANGLER_TEXT
    assert 'class_name = "ForkMeshCronRunner"' in WRANGLER_TEXT
    assert 'tag = "v14"' in WRANGLER_TEXT
    assert 'new_sqlite_classes = ["ForkMeshCronRunner"]' in WRANGLER_TEXT


def test_worker_entrypoint_owns_pyproxy_and_delegates_to_fresh_runtime():
    """The supported entrypoint owns its proxy; route state is per event."""
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    default = list(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
    )[-1]
    assert [ast.unparse(base) for base in default.bases] == [
        "WorkerEntrypoint"]
    methods = {
        node.name for node in default.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    assert methods == {"_runtime", "fetch", "scheduled", "queue"}

    calls = []

    class Receiver:
        async def fetch(self, request):
            calls.append(("fetch", request, self.env))
            return "fetched"

        async def scheduled(self, controller, env, ctx):
            calls.append((
                "scheduled", controller, env, ctx, self.env))
            return "scheduled"

        async def queue(self, batch, env, ctx):
            calls.append(("queue", batch, env, ctx, self.env))
            return "queued"

    namespace = {"AppRuntime": Receiver, "WorkerEntrypoint": object}
    module = ast.fix_missing_locations(ast.Module(
        body=[default], type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))

    async def invoke():
        worker = namespace["Default"]()
        worker.env = "env"
        worker.ctx = "ctx"
        fetch_result = await worker.fetch("request")
        cron_result = await worker.scheduled(
            "controller", "env", "ctx")
        queue_result = await worker.queue("batch", "env", "ctx")
        return fetch_result, cron_result, queue_result

    assert asyncio.run(invoke()) == ("fetched", "scheduled", "queued")
    assert calls == [
        ("fetch", "request", "env"),
        ("scheduled", "controller", "env", "ctx", "env"),
        ("queue", "batch", "env", "ctx", "env"),
    ]


def test_standard_handler_mode_uses_normal_durable_object_methods():
    # The standard handler model is the runtime default from compatibility
    # date 2025-08-14 on; spelling python_no_global_handlers out now makes
    # workerd refuse to start, so the manifest must rely on the date and
    # must never opt back into the legacy bridge.
    assert '"disable_python_no_global_handlers"' not in WRANGLER_TEXT
    assert '"python_no_global_handlers"' not in WRANGLER_TEXT
    compatibility_date = tomllib.loads(WRANGLER_TEXT)["compatibility_date"]
    assert str(compatibility_date) >= "2025-08-14"
    assert "DurableObject.on_fetch" not in ENTRY_TEXT
    assert "DurableObject.on_alarm" not in ENTRY_TEXT


def test_due_schedule_matches_every_phase_contract_for_full_cycle():
    ns = _load_runner()

    def expected(minute):
        phases = [
            "status-sample",
            "status-alert-pings",
            "mirror-health",
            "mail-log",
            "self-deploy-pings",
            "actions-schedule",
        ]
        if minute % 5 == 0:
            phases.append("status-alert-email")
        if minute % 3 == 0:
            phases.append("snapshot-status")
        elif minute % 3 == 1:
            phases.append("snapshot-mirrors")
        if minute % 60 < 5 or minute % 60 == 30:
            phases.append("status-compact")
        phases.extend((
            "probe-flagship", "probe-release-platforms"))
        if minute % 10 == 6:
            phases.append("usage-budget")
        if minute % 10 == 4:
            phases.append("discord-bugs")
        if minute % 5 == 2:
            phases.append("snapshot-catalog")
        if minute % 5 == 4:
            phases.extend(("snapshot-network", "ledger-backfill"))
        if minute % 10 == 0:
            phases.append("online-sample")
        if minute % 5 == 0:
            phases.append("ap-outbox")
        elif minute % 5 == 1:
            phases.extend((
                "reward-chain", "reward-contributions", "reward-sweeps",
                "direct-unread-email"))
        elif minute % 5 == 2:
            phases.extend(("federation", "tip-verify"))
        elif minute % 5 == 3:
            phases.extend((
                "notification-digest", "chat-digest",
                "channel-unread-email"))
        elif minute % 5 == 4:
            phases.extend(("webhook-outbox", "app-events", "provider-mirror"))
        if minute % 10 == 8:
            phases.append("ap-digest")
        if minute % 20 == 12:
            phases.append("ap-profiles")
        if minute % 180 == 11:
            phases.append("world-satellites")
        if minute % 15 == 4:
            phases.append("stale-nodes")
        elif minute % 15 == 9:
            phases.append("blocked-catalog")
        elif minute % 15 == 14:
            phases.append("central-fund")
        elif minute % 15 == 11:
            phases.append("instance-health")
        if minute % 360 == 22:
            phases.append("feedback-email")
        if minute % 10 == 9:
            phases.append("marketing-email")
        if minute % 5 == 3:
            phases.append("workflow-schedules")
        if minute % 15 == 6:
            phases.append("member-activity")
        hourly = minute % 60
        if hourly == 7:
            phases.append("microjob-payouts")
        elif hourly == 17:
            phases.append("referrer-prune")
        elif hourly == 37:
            phases.append("chat-prune")
        elif hourly == 42:
            phases.append("org-succession")
        elif hourly == 47:
            phases.append("account-device-prune")
        elif hourly == 52:
            phases.extend((
                "sensitive-cleanup", "expire-rewards",
                "world-media-cleanup"))
        elif hourly == 27:
            phases.append("attribution-maintenance")
        elif hourly == 57:
            phases.append("badge-sweep")
        elif hourly == 32:
            phases.append("instance-announce")
        if minute % 1440 == 5:
            phases.append("attribution-clone-snapshot")
        if minute % 5 == 0:
            phases.append("probe-bans")
        if minute % 2 == 0:
            phases.append("pull-attention")
        return tuple(phases)

    for minute in range(360):
        actual = ns["_cron_due_phases"](minute)
        assert actual == expected(minute), minute
        assert "watchdog-completion" not in actual


def test_exact_coordinator_and_phase_object_names_and_paths_are_required():
    calls = []

    async def run_phase(_env, phase, slot):
        calls.append((phase, slot))

    ns = _load_runner(run_phase=run_phase)
    coordinator, _ = _runner(ns, ns["CRON_RUNNER_NAME"])
    missing = asyncio.run(coordinator.fetch(_request("/cron-runner/run")))
    assert missing.status == 404

    wrong_coordinator, _ = _runner(ns, ns["CRON_RUNNER_NAME"] + "-other")
    wrong = asyncio.run(
        wrong_coordinator.fetch(_request("/cron-runner/kick")))
    assert wrong.status == 404

    # The retired fan-out receivers answer nothing on any object name: a
    # request from an edge still running the previous release cannot make
    # this namespace run a phase outside the coordinator's alarm.
    for object_name in (
        "scheduled-batch-v4:0:0", "scheduled-batch-v3:1",
        "scheduled-phase-v2:status-sample", "scheduled-status-v1:17",
        ns["CRON_RUNNER_NAME"],
    ):
        legacy, storage = _runner(ns, object_name)
        for path in (
            "/cron-runner/batch", "/cron-runner/run?phase=status-sample&slot=17",
            "/cron-runner/status-sample?slot=17",
        ):
            response = asyncio.run(legacy.fetch(_request(path)))
            assert response.status == 404, (object_name, path)
        assert storage.events == []
    assert calls == []


def test_kick_arms_bridge_and_retire_cancels_it_idempotently():
    ns = _load_runner()
    storage = _Storage()
    storage.data["retired_v2"] = True
    runner, _ = _runner(ns, ns["CRON_RUNNER_NAME"], storage)

    kick = asyncio.run(runner.fetch(_request("/cron-runner/kick")))
    assert kick.status == 200
    assert kick.payload == {"ok": True, "mode": "serialized-alarm"}
    assert storage.data["retired_v2"] is False
    assert storage.alarm_at == (
        ns["Date"].value + ns["CRON_RUNNER_KICK_DELAY_MS"])

    retire = asyncio.run(runner.fetch(_request("/cron-runner/retire")))
    assert retire.status == 200
    assert retire.payload == {"ok": True, "retired": True}
    assert storage.data["retired_v2"] is True
    assert storage.alarm_at is None
    assert storage.delete_alarm_calls == 1

    asyncio.run(runner.fetch(_request("/cron-runner/retire")))
    assert storage.delete_alarm_calls == 1

    # A tick continuation already armed for sooner than the doorbell's delay
    # is left alone: the kick is a safety net for a lost alarm, never a way
    # to push a running tick back by a second every minute.
    storage.alarm_at = ns["Date"].value
    kick = asyncio.run(runner.fetch(_request("/cron-runner/kick")))
    assert kick.status == 200
    assert storage.alarm_at == ns["Date"].value
    storage.alarm_at = ns["Date"].value + 60_000
    asyncio.run(runner.fetch(_request("/cron-runner/kick")))
    assert storage.alarm_at == (
        ns["Date"].value + ns["CRON_RUNNER_KICK_DELAY_MS"])


def test_cron_reachable_fetches_do_not_create_nested_pyodide_tasks():
    mirror_fetch = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_fetch_text"):
        ENTRY_TEXT.index("\n\n_CLOUDFLARE_EDGE_RANGES_MEMO")
    ]
    mirror_cron = ENTRY_TEXT[
        ENTRY_TEXT.index("async def https_mirror_health_cron"):
        ENTRY_TEXT.index("\n\nasync def _https_mirror_private_candidates")
    ]
    assert "js_fetch_with_timeout(" in mirror_fetch
    assert "asyncio.wait_for" not in mirror_fetch
    assert "asyncio.gather" not in mirror_cron
    assert "for row in rows:" in mirror_cron

    solana_rpc = SOLANA_TEXT[
        SOLANA_TEXT.index("async def _solana_rpc"):
    ]
    assert "AbortSignal.timeout(SOLANA_RPC_TIMEOUT_MS)" in solana_rpc
    assert "asyncio.wait_for" not in solana_rpc


# --- The inline tick (scheduled-runner-v1) ----------------------------------
#
# The coordinator's alarm runs every due phase of the minute itself and
# continues in a fresh invocation, on the same object, when one subrequest
# budget cannot hold the rest. These contracts pin that state machine with
# the phase runner and D1 replaced by fakes.

def _coordinator(ns, storage=None, env=None):
    return _runner(ns, ns["CRON_RUNNER_NAME"], storage, env)


def _tick_state(ns, storage):
    return json.loads(storage.data[ns["CRON_TICK_STATE_KEY"]])


def _measure(ns, storage, phases, cost=1):
    storage.data[ns["CRON_TICK_COST_KEY"]] = json.dumps(
        {phase: cost for phase in phases})


def _run_tick(ns, runner, storage, limit=20):
    """Fire the alarm and every continuation it arms for 'now'."""
    invocations = 0
    while True:
        storage.alarm_at = None
        asyncio.run(runner.alarm())
        invocations += 1
        if storage.alarm_at is None or storage.alarm_at > ns["Date"].value:
            return invocations
        assert invocations < limit, "tick never finished"


def _next_minute(ns, slot=17):
    return (
        (slot + 1) * ns["CRON_RUNNER_INTERVAL_MS"]
        + ns["CRON_RUNNER_ALARM_OFFSET_MS"]
    )


def test_coordinator_runs_the_whole_minute_in_one_invocation_once_measured():
    ran = []
    ns = None

    async def run_phase(_env, phase, slot):
        ran.append((phase, slot))
        ns["_cron_subrequest_charge"]()

    ns = _load_runner(run_phase=run_phase)
    ns["Date"].value = 23 * ns["CRON_RUNNER_INTERVAL_MS"]
    env = object()
    runner, storage = _coordinator(ns, env=env)
    ordered, chain_length = ns["_cron_tick_phase_order"](23)
    _measure(ns, storage, ordered)

    assert _run_tick(ns, runner, storage) == 1
    # Chain first, independent phases next, the heartbeat last, all under
    # this one alarm: no other object was addressed.
    assert [phase for phase, _slot in ran] == list(ordered)
    assert all(slot == 23 for _phase, slot in ran)
    assert ordered[:chain_length] == tuple(
        phase for phase in ns["CRON_STATUS_CHAIN"] if phase in ordered)
    assert ordered[-1] == ns["CRON_WATCHDOG_PHASE"]
    assert storage.alarm_at == _next_minute(ns, 23)
    assert storage.data["last_started_slot"] == 23
    assert storage.data["last_completed_slot"] == 23
    assert storage.data["last_failure"] == ""
    state = _tick_state(ns, storage)
    assert state["slot"] == 23 and state["pending"] == []
    assert state["invocations"] == 1
    assert state["subrequests"] == len(ordered)
    assert sorted(state["ran"]) == sorted(ordered)
    assert state["failed"] == {} and state["skipped"] == []
    assert state["completedAt"] == ns["Date"].value
    # One bounded D1 receipt for the tick, written after the budget guard
    # stopped charging, and no retention sweep on an off-slot.
    assert len(ns["_d1_writes"]) == 1
    sql, args = ns["_d1_writes"][0]
    assert "INSERT INTO cron_tick" in sql
    assert args[0] == 23 and args[2] == 1 and args[3] == len(ordered)
    assert args[5] == len(ordered)
    assert sorted(json.loads(args[-1])["ran"]) == sorted(ordered)

    # A second alarm in the same minute (the doorbell re-arming it) does
    # not run the minute twice.
    ran.clear()
    storage.alarm_at = None
    asyncio.run(runner.alarm())
    assert ran == []
    assert storage.alarm_at == _next_minute(ns, 23)
    assert len(ns["_d1_writes"]) == 1


def test_unmeasured_phase_leads_its_own_invocation_then_packs():
    """The packing the tick keeps, without ever squeezing a phase.

    A phase whose subrequest cost this object has not measured reserves the
    whole budget, so it can only lead an invocation -- exactly the isolation
    a per-phase object gave it. The cost it records then lets the next minute
    run everything under one alarm.
    """
    ns = None
    costs = {"status-sample": 6, "status-alert-pings": 2,
             "mirror-health": 4, "watchdog-completion": 1}

    async def run_phase(_env, phase, _slot):
        for _ in range(costs.get(phase, 1)):
            ns["_cron_subrequest_charge"]()

    ns = _load_runner(run_phase=run_phase)
    # A fixed schedule, so consecutive minutes owe the same phases.
    ns["_cron_due_phases"] = lambda _slot: (
        "status-sample", "status-alert-pings", "mirror-health")
    runner, storage = _coordinator(ns)
    ordered, _chain = ns["_cron_tick_phase_order"](17)
    assert ordered == (
        "status-sample", "status-alert-pings", "mirror-health",
        "watchdog-completion")

    # First minute: nothing is measured, so every phase is the head of its
    # own continuation and the alarm is re-armed for "now" between them.
    assert _run_tick(ns, runner, storage) == len(ordered)
    state = _tick_state(ns, storage)
    assert state["invocations"] == len(ordered)
    assert sorted(state["ran"]) == sorted(ordered)
    learned = json.loads(storage.data[ns["CRON_TICK_COST_KEY"]])
    assert learned == {phase: costs.get(phase, 1) for phase in ordered}
    assert storage.alarm_at == _next_minute(ns)
    assert storage.data["last_completed_slot"] == 17

    # Next minute: the learned costs pack the same work into one invocation,
    # and the cost table is only rewritten when a high-water mark moves.
    ns["Date"].value += ns["CRON_RUNNER_INTERVAL_MS"]
    storage.events = []
    assert _run_tick(ns, runner, storage) == 1
    assert _tick_state(ns, storage)["invocations"] == 1
    assert storage.data["last_completed_slot"] == 18
    assert not any(
        event[0] == "put" and event[1] == ns["CRON_TICK_COST_KEY"]
        for event in storage.events)


def test_tick_continues_with_a_fresh_budget_when_a_phase_overruns():
    ns = None
    ran = []

    async def run_phase(_env, phase, _slot):
        ran.append(phase)
        charge = (
            ns["CRON_BATCH_SUBREQUEST_CAP"] + 2
            if phase == "status-sample" else 1)
        for _ in range(charge):
            ns["_cron_subrequest_charge"]()

    ns = _load_runner(run_phase=run_phase)
    runner, storage = _coordinator(ns)
    ordered, _chain = ns["_cron_tick_phase_order"](17)
    storage.data[ns["CRON_TICK_COST_KEY"]] = json.dumps({
        **{phase: 1 for phase in ordered}, "status-sample": 2})

    storage.alarm_at = None
    asyncio.run(runner.alarm())
    # The overrun spent what this invocation could hold; the rest was
    # persisted and the alarm re-armed for now instead of being dropped.
    assert storage.alarm_at == ns["Date"].value
    resumed = _tick_state(ns, storage)
    assert resumed["pending"] == list(ordered[1:])
    assert resumed["started"] == ""
    assert list(resumed["ran"]) == ["status-sample"]
    assert ran == ["status-sample"]
    assert not resumed["failed"]

    assert _run_tick(ns, runner, storage) >= 1
    final = _tick_state(ns, storage)
    assert final["pending"] == [] and final["failed"] == {}
    assert ran == list(ordered)
    per_invocation = ns["CRON_BATCH_SUBREQUEST_CAP"] // (
        1 + ns["CRON_BATCH_RESERVE_HEADROOM"])
    assert final["invocations"] == 1 + -(-(len(ordered) - 1)
                                         // per_invocation)
    # The overrun widened its own reserve to the cap, so the next minute
    # never packs anything behind it.
    assert json.loads(storage.data[ns["CRON_TICK_COST_KEY"]])[
        "status-sample"] == ns["CRON_BATCH_SUBREQUEST_CAP"]
    assert storage.alarm_at == _next_minute(ns)
    assert storage.data["last_completed_slot"] == 17


def test_broken_chain_stops_the_chain_and_withholds_the_heartbeat():
    ran = []

    async def run_phase(_env, phase, slot):
        ran.append(phase)
        if phase == "status-alert-pings" and slot == 15:
            raise RuntimeError("ping provider down")

    ns = _load_runner(run_phase=run_phase)
    # Both chain phases behind the failing ping phase are due, and
    # independent work is due beside them, on consecutive minutes.
    ns["_cron_due_phases"] = lambda _slot: (
        "status-sample", "status-alert-pings", "status-alert-email",
        "mirror-health", "snapshot-status", "snapshot-catalog")
    runner, storage = _coordinator(ns)
    ns["Date"].value = 15 * ns["CRON_RUNNER_INTERVAL_MS"] + 5
    ordered, chain_length = ns["_cron_tick_phase_order"](15)
    chain = ordered[:chain_length]
    assert "status-alert-email" in chain and "snapshot-status" in chain
    _measure(ns, storage, ordered)

    assert _run_tick(ns, runner, storage) == 1
    state = _tick_state(ns, storage)
    assert state["failed"] == {"status-alert-pings": "ping provider down"}
    assert state["skipped"] == [
        phase for phase in chain
        if chain.index(phase) > chain.index("status-alert-pings")
    ] + ["watchdog-completion"]
    independent = [phase for phase in ordered[chain_length:-1]]
    assert ran == ["status-sample", "status-alert-pings"] + independent
    assert "last_completed_slot" not in storage.data
    assert storage.data["last_failure"] == (
        "cron phase failures: status-alert-pings: ping provider down")
    assert storage.alarm_at == _next_minute(ns, 15)
    detail = json.loads(ns["_d1_writes"][-1][1][-1])
    assert detail["failed"] == {"status-alert-pings": "ping provider down"}

    # The next minute starts clean.
    ran.clear()
    ns["Date"].value += ns["CRON_RUNNER_INTERVAL_MS"]
    assert _run_tick(ns, runner, storage) == 1
    assert storage.data["last_completed_slot"] == 16
    assert storage.data["last_failure"] == ""


def test_interrupted_phase_is_incomplete_unless_it_is_safe_to_replay():
    ran = []

    async def run_phase(_env, phase, _slot):
        ran.append(phase)

    ns = _load_runner(run_phase=run_phase)
    runner, storage = _coordinator(ns)
    now = ns["Date"].value

    def interrupted(started, pending):
        storage.data[ns["CRON_TICK_STATE_KEY"]] = json.dumps({
            "slot": 17, "pending": list(pending),
            "chain": ["status-alert-email"], "startedAt": now - 5_000,
            "invocations": 1, "ran": {"status-sample": {"ms": 1, "sub": 1}},
            "failed": {}, "skipped": [], "failures": [], "subrequests": 1,
            "started": started,
        })
        _measure(ns, storage, pending)
        ran.clear()
        storage.alarm_at = None
        asyncio.run(runner.alarm())
        return _tick_state(ns, storage)

    # A phase started by an invocation that never finished may have been
    # interrupted mid-write: report it, never replay it blindly.
    state = interrupted(
        "status-alert-email",
        ("status-alert-email", "mirror-health", "watchdog-completion"))
    assert ran == ["mirror-health"]
    assert state["failed"] == {"status-alert-email": "phase_incomplete"}
    assert state["skipped"] == ["watchdog-completion"]
    assert "last_completed_slot" not in storage.data

    # Replay-safe and D1-deduplicated phases are simply attempted again.
    storage.data.pop("last_failure", None)
    state = interrupted(
        "probe-flagship",
        ("probe-flagship", "watchdog-completion"))
    assert ran == ["probe-flagship", "watchdog-completion"]
    assert state["failed"] == {}
    state = interrupted(
        "status-sample", ("status-sample", "watchdog-completion"))
    assert ran == ["status-sample", "watchdog-completion"]
    assert state["failed"] == {}
    assert storage.data["last_completed_slot"] == 17


def test_swallowed_over_budget_subrequest_fails_closed_then_continues():
    ns = None
    ran = []

    async def run_phase(_env, phase, _slot):
        ran.append(phase)
        if phase == "status-sample":
            try:
                for _ in range(ns["CRON_PHASE_D1_QUERY_LIMIT"] + 1):
                    ns["_cron_subrequest_charge"]()
            except RuntimeError:
                # Broad maintenance helpers sometimes catch provider errors.
                # The tick must still observe the exhausted shared state.
                pass
        else:
            ns["_cron_subrequest_charge"]()

    ns = _load_runner(run_phase=run_phase)
    runner, storage = _coordinator(ns)
    ordered, _chain = ns["_cron_tick_phase_order"](17)
    _measure(ns, storage, ordered)

    storage.alarm_at = None
    asyncio.run(runner.alarm())
    state = _tick_state(ns, storage)
    assert "budget exhausted" in state["failed"]["status-sample"]
    assert ran == ["status-sample"]
    # The spent budget ends this invocation; the chain behind the sampler is
    # broken, the independent phases continue with a fresh budget.
    assert storage.alarm_at == ns["Date"].value
    _run_tick(ns, runner, storage)
    final = _tick_state(ns, storage)
    assert final["pending"] == []
    assert "watchdog-completion" in final["skipped"]
    assert all(
        phase in final["skipped"] for phase in ns["CRON_STATUS_CHAIN"]
        if phase in ordered and phase != "status-sample")
    assert "mirror-health" in final["ran"]
    assert "last_completed_slot" not in storage.data


def test_tick_deadline_marks_the_rest_undispatched():
    ns = None
    ran = []

    async def run_phase(_env, phase, _slot):
        ran.append(phase)
        if phase == "status-sample":
            ns["Date"].value += ns["CRON_DISPATCH_TIMEOUT_MS"]

    ns = _load_runner(run_phase=run_phase)
    runner, storage = _coordinator(ns)
    ordered, _chain = ns["_cron_tick_phase_order"](17)
    _measure(ns, storage, ordered)

    assert _run_tick(ns, runner, storage) == 1
    state = _tick_state(ns, storage)
    assert ran == ["status-sample"]
    assert state["pending"] == []
    assert all(
        state["failed"][phase] == "undispatched"
        for phase in ordered if phase != "status-sample")
    assert "dispatch deadline exhausted" in state["failures"]
    assert "last_completed_slot" not in storage.data
    assert "undispatched:" in storage.data["last_failure"]


def test_coordinator_checkpoint_failures_never_fail_a_finished_tick():
    calls = []
    storage = _Storage(fail_put_keys={
        "last_started_slot", "last_completed_slot", "last_completed_at",
        "last_failure",
    })

    async def run_phase(_env, phase, _slot):
        calls.append(phase)

    ns = _load_runner(run_phase=run_phase)
    ns["Date"].value = 23 * ns["CRON_RUNNER_INTERVAL_MS"]
    runner, _ = _coordinator(ns, storage)
    ordered, _chain = ns["_cron_tick_phase_order"](23)
    _measure(ns, storage, ordered)
    assert _run_tick(ns, runner, storage) == 1
    assert calls == list(ordered)
    assert storage.alarm_at == _next_minute(ns, 23)
    assert runner._coordinator_active is False
    # The completed tick state is what stops a second alarm in the same
    # minute from running it again, even without the checkpoint.
    calls.clear()
    storage.alarm_at = None
    asyncio.run(runner.alarm())
    assert calls == []


def test_coordinator_retirement_before_or_during_alarm_prevents_the_tick():
    calls = []

    async def run_phase(_env, phase, _slot):
        calls.append(phase)

    ns = _load_runner(run_phase=run_phase)
    retired_storage = _Storage()
    retired_storage.data["retired_v2"] = True
    retired, _ = _coordinator(ns, retired_storage)
    asyncio.run(retired.alarm())
    assert calls == []
    assert retired_storage.alarm_at is None

    racing_storage = _Storage(retire_after_set_alarm=True)
    racing, _ = _coordinator(ns, racing_storage)
    asyncio.run(racing.alarm())
    assert calls == []
    assert racing_storage.data["retired_v2"] is True
    assert racing_storage.alarm_at is None
    assert racing_storage.delete_alarm_calls == 1


def test_only_the_coordinator_alarm_runs_phases():
    calls = []

    async def run_phase(_env, phase, _slot):
        calls.append(phase)

    ns = _load_runner(run_phase=run_phase)
    for object_name in ("app-ingress-v1:a", "scheduled-batch-v4:0:0",
                        ns["CRON_RUNNER_NAME"] + "-other"):
        other, storage = _runner(ns, object_name)
        asyncio.run(other.alarm())
        assert calls == []
        assert storage.events == []


# --- Event-driven snapshot republish (snapshot-republish-v1) -----------------
#
# Mutations that feed a catalog snapshot ring this object; its alarm rebuilds
# the affected paths outside the mutating request. These contracts pin the
# doorbell/alarm state machine without touching D1: the row verdict and the
# publisher are injected fakes.

def _republish_ns(verdict=None, publish=None):
    ns = _load_runner()
    calls = {"verdict": [], "publish": []}

    async def default_verdict(_env, path, requested_at, now):
        calls["verdict"].append((path, requested_at, now))
        return "build", 0

    async def default_publish(_env, path, generated_at):
        calls["publish"].append((path, generated_at))
        return True

    ns["_republish_row_verdict"] = verdict or default_verdict
    ns["_republish_publish_path"] = publish or default_publish
    return ns, calls


def _doorbell(ns, runner, *paths):
    return asyncio.run(runner.fetch(_request(
        ns["SNAPSHOT_REPUBLISH_PATH"] + "?paths=" + ",".join(paths))))


def _stamp(ns, storage, prefix, path):
    return storage.data.get(ns[prefix] + path, 0)


def _fire(runner, storage):
    # The platform dequeues an alarm before running its handler: getAlarm()
    # answers null inside the handler unless it re-arms.
    storage.alarm_at = None
    asyncio.run(runner.alarm())


def test_republish_constants_pin_the_bucket_and_priority_order():
    ns = _load_runner()
    assert ns["SNAPSHOT_REPUBLISH_NAME"] == "snapshot-republish-v1"
    assert ns["SNAPSHOT_REPUBLISH_PATH"] == "/cron-runner/republish"
    # The trailing edge reuses the tombstone bucket, so a burst of edits
    # costs one rebuild per two minutes per path: the read-budget bound.
    assert ns["SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS"] == 2 * 60 * 1000
    assert 0 < ns["SNAPSHOT_REPUBLISH_LEAD_MS"] <= 10_000
    assert ns["SNAPSHOT_REPUBLISH_PATHS"] == (
        "/api/repositories",
        "/api/repo/forkmesh/forkmesh/mirrors",
        "/api/network/overview",
    )
    assert ns["SNAPSHOT_REPUBLISH_STATUS_PATHS"] == (
        "/api/status", "/api/status/world")
    assert ns["SNAPSHOT_REPUBLISH_DOORBELL_PATHS"] == (
        "/api/status", "/api/status/world",
        "/api/repositories",
        "/api/repo/forkmesh/forkmesh/mirrors",
        "/api/network/overview",
    )
    ready_at = ns["_republish_ready_at"]
    lead = ns["SNAPSHOT_REPUBLISH_LEAD_MS"]
    interval = ns["SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS"]
    # Leading edge when idle, trailing edge inside the bucket.
    assert ready_at(1_000_000, 0) == 1_000_000 + lead
    assert ready_at(1_000_000, 1_000_000 - 10) == 1_000_000 - 10 + interval
    assert ready_at(1_000_000, 1_000_000 - interval) == 1_000_000 + lead
    assert ready_at(
        1_000_000, 1_000_000 - 10, "/api/status") == 1_000_000 + lead


def test_commit_mirror_wake_is_coalesced_and_retried_until_durable():
    ns = _load_runner()
    calls = []

    async def notify(_env, owner, repo, topic, state="", generation=0):
        calls.append((owner, repo, topic, state, generation))
        return len(calls) > 1

    ns["notify_repo_mirrors"] = notify
    runner, storage = _runner(ns, ns["MIRROR_WAKE_NAME"])
    now = ns["Date"].value
    state = "a" * 64
    response = asyncio.run(runner.fetch(_request(
        ns["MIRROR_WAKE_PATH"]
        + "?owner=forkmesh&repo=forkmesh&topic=commits"
        + "&state=" + state + "&generation=1234"
    )))
    assert response.status == 202
    assert storage.alarm_at == now + ns["MIRROR_WAKE_LEAD_MS"]

    ns["Date"].value = storage.alarm_at
    _fire(runner, storage)
    assert calls == [("forkmesh", "forkmesh", "commits", state, 1234)]
    assert storage.alarm_at == (
        ns["Date"].value + ns["MIRROR_WAKE_RETRY_MS"])
    assert json.loads(storage.data[ns["MIRROR_WAKE_REQUESTED_PREFIX"]])

    ns["Date"].value = storage.alarm_at
    _fire(runner, storage)
    assert calls[-1] == ("forkmesh", "forkmesh", "commits", state, 1234)
    assert json.loads(
        storage.data[ns["MIRROR_WAKE_REQUESTED_PREFIX"]]) == {}


def test_stateless_commit_wake_keeps_a_pending_convergence_target():
    ns = _load_runner()
    calls = []

    async def notify(_env, owner, repo, topic, state="", generation=0):
        calls.append((owner, repo, topic, state, generation))
        return True

    ns["notify_repo_mirrors"] = notify
    runner, storage = _runner(ns, ns["MIRROR_WAKE_NAME"])
    state = "b" * 64
    base = (ns["MIRROR_WAKE_PATH"]
            + "?owner=forkmesh&repo=forkmesh&topic=commits")
    assert asyncio.run(runner.fetch(_request(
        base + "&state=" + state + "&generation=77"))).status == 202
    assert asyncio.run(runner.fetch(_request(base))).status == 202
    pending = json.loads(storage.data[ns["MIRROR_WAKE_REQUESTED_PREFIX"]])
    assert pending["forkmesh/forkmesh/commits"]["state"] == state

    ns["Date"].value = storage.alarm_at
    _fire(runner, storage)
    assert calls == [("forkmesh", "forkmesh", "commits", state, 77)]

    assert asyncio.run(runner.fetch(_request(base))).status == 202
    ns["Date"].value = storage.alarm_at
    _fire(runner, storage)
    assert calls[-1] == ("forkmesh", "forkmesh", "commits", "", 0)


def test_republish_doorbell_records_paths_and_arms_only_the_lead_alarm():
    ns, calls = _republish_ns()
    runner, storage = _runner(ns, ns["SNAPSHOT_REPUBLISH_NAME"])
    now = ns["Date"].value
    lead = ns["SNAPSHOT_REPUBLISH_LEAD_MS"]

    response = _doorbell(
        ns, runner, "/api/repositories", "/api/network/overview")
    assert response.status == 202
    assert response.payload == {
        "ok": True,
        "paths": ["/api/repositories", "/api/network/overview"],
        "alarmAt": now + lead,
    }
    assert storage.alarm_at == now + lead
    for path in ("/api/repositories", "/api/network/overview"):
        assert _stamp(
            ns, storage, "SNAPSHOT_REPUBLISH_REQUESTED_PREFIX", path) == now
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_REQUESTED_PREFIX",
        "/api/repo/forkmesh/forkmesh/mirrors") == 0

    # A second doorbell inside the lead window rewrites its stamp (the alarm
    # compares it with the build it just made) but never moves the alarm
    # later: the burst still costs one build.
    ns["Date"].value = now + 1_000
    again = _doorbell(ns, runner, "/api/repositories")
    assert again.status == 202
    assert again.payload["alarmAt"] == now + lead
    assert storage.alarm_at == now + lead
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_REQUESTED_PREFIX",
        "/api/repositories") == now + 1_000
    assert calls["publish"] == []

    # Unknown paths and the wrong path/object are refused without touching
    # storage.
    events_before = len(storage.events)
    refused = asyncio.run(runner.fetch(_request(
        ns["SNAPSHOT_REPUBLISH_PATH"] + "?paths=/etc/passwd")))
    assert refused.status == 400
    wrong_path = asyncio.run(runner.fetch(_request("/cron-runner/run")))
    assert wrong_path.status == 404
    assert len(storage.events) == events_before
    other, other_storage = _runner(ns, ns["CRON_RUNNER_NAME"])
    assert asyncio.run(other.fetch(_request(
        ns["SNAPSHOT_REPUBLISH_PATH"] + "?paths=/api/repositories"
    ))).status == 404
    assert other_storage.events == []


def test_status_transition_doorbell_publishes_atomic_pair_without_bucket_wait():
    ns, calls = _republish_ns()
    runner, storage = _runner(ns, ns["SNAPSHOT_REPUBLISH_NAME"])
    now = ns["Date"].value
    lead = ns["SNAPSHOT_REPUBLISH_LEAD_MS"]
    for path in ns["SNAPSHOT_REPUBLISH_STATUS_PATHS"]:
        storage.data[ns["SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX"] + path] = (
            now - 10)

    response = _doorbell(ns, runner, *ns["SNAPSHOT_REPUBLISH_STATUS_PATHS"])
    assert response.status == 202
    assert response.payload["alarmAt"] == now + lead
    ns["Date"].value = now + lead
    _fire(runner, storage)

    # The first status path represents one atomic full/World publication. The
    # state machine stamps both paths, so it neither builds a mismatched second
    # generation nor re-arms a trailing alarm for the pair.
    assert calls["publish"] == [("/api/status", now + lead)]
    for path in ns["SNAPSHOT_REPUBLISH_STATUS_PATHS"]:
        assert _stamp(
            ns, storage, "SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX", path
        ) == now + lead
    assert storage.alarm_at is None


def test_republish_alarm_builds_one_path_per_invocation_and_refires():
    ns, calls = _republish_ns()
    runner, storage = _runner(ns, ns["SNAPSHOT_REPUBLISH_NAME"])
    now = ns["Date"].value
    lead = ns["SNAPSHOT_REPUBLISH_LEAD_MS"]
    interval = ns["SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS"]
    _doorbell(
        ns, runner, "/api/network/overview", "/api/repositories",
        "/api/repo/forkmesh/forkmesh/mirrors")

    # Before the lead delay the alarm only re-arms itself.
    ns["Date"].value = now + lead - 1
    _fire(runner, storage)
    assert calls["publish"] == []
    assert storage.alarm_at == now + lead

    # Ready: exactly one build (the catalog, highest priority), its published
    # stamp, and an immediate re-fire for the remaining pending paths.
    ns["Date"].value = now + lead
    _fire(runner, storage)
    assert calls["publish"] == [("/api/repositories", now + lead)]
    assert calls["verdict"] == [("/api/repositories", now, now + lead)]
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX",
        "/api/repositories") == now + lead
    assert storage.alarm_at == now + lead

    ns["Date"].value = now + lead + 5
    _fire(runner, storage)
    assert calls["publish"][-1] == (
        "/api/repo/forkmesh/forkmesh/mirrors", now + lead + 5)
    assert storage.alarm_at == now + lead + 5

    ns["Date"].value = now + lead + 10
    _fire(runner, storage)
    assert calls["publish"][-1] == ("/api/network/overview", now + lead + 10)
    # Nothing left pending: no alarm is re-armed.
    assert storage.alarm_at is None
    assert len(calls["publish"]) == 3

    # A quiet alarm with nothing pending builds nothing and arms nothing.
    _fire(runner, storage)
    assert len(calls["publish"]) == 3
    assert storage.alarm_at is None

    # A new doorbell right after a build is throttled to the bucket edge; the
    # earlier catalog build fixes when the next one may start.
    ns["Date"].value = now + lead + 20
    burst = _doorbell(ns, runner, "/api/repositories")
    assert burst.payload["alarmAt"] == now + lead + interval
    assert storage.alarm_at == now + lead + interval
    ns["Date"].value = now + lead + interval
    _fire(runner, storage)
    assert calls["publish"][-1] == ("/api/repositories", now + lead + interval)
    assert len(calls["publish"]) == 4


def test_republish_alarm_defers_to_tombstones_and_skips_reflected_rows():
    verdicts = {}

    async def verdict(_env, path, _requested_at, _now):
        return verdicts[path]

    ns, calls = _republish_ns(verdict=verdict)
    runner, storage = _runner(ns, ns["SNAPSHOT_REPUBLISH_NAME"])
    now = ns["Date"].value
    lead = ns["SNAPSHOT_REPUBLISH_LEAD_MS"]
    deadline = now + 90_000
    _doorbell(ns, runner, "/api/repositories", "/api/network/overview")
    verdicts["/api/repositories"] = ("tombstone", deadline)
    verdicts["/api/network/overview"] = ("reflected", now + 2_500)

    ns["Date"].value = now + lead
    _fire(runner, storage)
    # A privacy-transition barrier is authoritative until its deadline: no
    # wasted scan, and the alarm heals it right after the deadline instead of
    # leaving that to the next Cron slot.
    assert calls["publish"] == []
    assert storage.alarm_at == deadline + lead
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_REQUESTED_PREFIX",
        "/api/repositories") == now
    # A Cron generation that started after the mutation already carries it:
    # the path is marked published at that generation and never rebuilt.
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX",
        "/api/network/overview") == now + 2_500

    verdicts["/api/repositories"] = ("build", 0)
    ns["Date"].value = deadline + lead
    _fire(runner, storage)
    assert calls["publish"] == [("/api/repositories", deadline + lead)]
    assert storage.alarm_at is None


def test_republish_failed_build_is_left_to_cron_but_still_throttles():
    async def failing(_env, path, generated_at):
        return False

    ns, _calls = _republish_ns(publish=failing)
    runner, storage = _runner(ns, ns["SNAPSHOT_REPUBLISH_NAME"])
    now = ns["Date"].value
    lead = ns["SNAPSHOT_REPUBLISH_LEAD_MS"]
    interval = ns["SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS"]
    _doorbell(ns, runner, "/api/repositories")
    ns["Date"].value = now + lead
    _fire(runner, storage)
    # The attempt is stamped as published: no retry loop, the Cron cadence
    # is the safety net, and a following doorbell waits for the bucket edge.
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX",
        "/api/repositories") == now + lead
    assert storage.alarm_at is None
    ns["Date"].value = now + lead + 1
    assert _doorbell(ns, runner, "/api/repositories").payload["alarmAt"] == (
        now + lead + interval)


def test_republish_doorbell_during_a_build_keeps_the_path_pending():
    holder = {}

    async def publish(_env, path, generated_at):
        # A mutation commits while the first build is reading D1 and rings
        # the bell; that build may have missed it.
        ns, runner = holder["ns"], holder["runner"]
        if holder.setdefault("builds", 0) == 0:
            ns["Date"].value = generated_at + 700
            response = await runner._republish_fetch(
                urlparse("https://forkmesh.internal"
                         + ns["SNAPSHOT_REPUBLISH_PATH"] + "?paths=" + path))
            assert response.status == 202
        holder["builds"] += 1
        return True

    ns, _calls = _republish_ns(publish=publish)
    runner, storage = _runner(ns, ns["SNAPSHOT_REPUBLISH_NAME"])
    holder.update(ns=ns, runner=runner)
    now = ns["Date"].value
    lead = ns["SNAPSHOT_REPUBLISH_LEAD_MS"]
    interval = ns["SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS"]
    _doorbell(ns, runner, "/api/repositories")
    ns["Date"].value = now + lead
    _fire(runner, storage)
    generated_at = now + lead
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX",
        "/api/repositories") == generated_at
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_REQUESTED_PREFIX",
        "/api/repositories") == generated_at + 700
    # Still pending. The mid-build doorbell armed its own lead alarm before
    # the build's stamp landed; the handler never moves an alarm later, so
    # that early fire is a cheap no-op that re-arms at the bucket edge — the
    # mutation is rebuilt then rather than dropped.
    assert storage.alarm_at == generated_at + 700 + lead
    ns["Date"].value = generated_at + 700 + lead
    _fire(runner, storage)
    assert storage.alarm_at == generated_at + interval
    ns["Date"].value = generated_at + interval
    _fire(runner, storage)
    assert _stamp(
        ns, storage, "SNAPSHOT_REPUBLISH_PUBLISHED_PREFIX",
        "/api/repositories") == generated_at + interval
    assert holder["builds"] == 2
    assert storage.alarm_at is None


def test_republish_object_ignores_phase_and_coordinator_alarms():
    ns, calls = _republish_ns()
    runner, storage = _runner(ns, ns["SNAPSHOT_REPUBLISH_NAME"])
    kick = asyncio.run(runner.fetch(_request("/cron-runner/kick")))
    assert kick.status == 404
    assert asyncio.run(runner.fetch(
        _request("/cron-runner/run?phase=snapshot-catalog&slot=17")
    )).status == 404
    assert storage.alarm_at is None
    # And the coordinator/phase alarm paths never run the republish machine.
    coordinator, coordinator_storage = _runner(ns, ns["CRON_RUNNER_NAME"])
    _run_tick(ns, coordinator, coordinator_storage)
    assert calls["publish"] == [] and calls["verdict"] == []
    assert coordinator_storage.data.get("last_completed_slot") == 17
