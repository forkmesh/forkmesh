#!/usr/bin/env python3

import ast
import asyncio
import json
import os
import re
import subprocess
import sys
import tomllib
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
RUNTIME = ROOT / "src" / "app_runtime.py"
WORLD_DO = ROOT.parent / "world" / "src" / "world_durable_objects.py"
DEPLOY = ROOT / "deploy.sh"
WRANGLER = ROOT / "wrangler.toml"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
RUNTIME_TEXT = RUNTIME.read_text(encoding="utf-8")
ENTRY_TREE = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
RUNTIME_TREE = ast.parse(RUNTIME_TEXT, filename=str(RUNTIME))

sys.path.insert(0, str(ROOT / "tools"))
import decommission_wrangler_toml  # noqa: E402


HELPER_FUNCTIONS = {
    "js_nullish",
    "method_name",
    "_git_clone_http_request",
    "_env_flag",
    "_decommissioned",
    "_read_only",
    "_sql_is_write",
    "_d1_write_guard",
    "_read_only_response",
    "_decommissioned_response",
    "_read_only_request_allowed",
    "_maintenance_gate_response",
    "_maintenance_stop_alarm",
    "_maintenance_socket_closed",
}
HELPER_CONSTANTS = {
    "EXPECTED_DEGRADED_HEADER",
    "EXPECTED_DEGRADED_HEADERS",
    "MAINTENANCE_TRUE_VALUES",
    "READ_ONLY_RETRY_AFTER_SECONDS",
    "MAINTENANCE_SOCKET_CLOSE_CODE",
    "_SQL_LEADING_COMMENTS_RE",
    "_SQL_WRITE_VERB_RE",
    "_SQL_QUOTED_RE",
    "_SQL_CTE_WRITE_RE",
}


class _Url:

    def __init__(self, value):
        parsed = urlparse(value)
        self.pathname = parsed.path
        self.search = ("?" + parsed.query) if parsed.query else ""

    @classmethod
    def new(cls, value):
        return cls(value)


class _Response:
    def __init__(self, body=None, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = {str(k).lower(): v for k, v in dict(headers or {}).items()}


def _json_response(data, status=200, cache_control=None, extra_headers=None,
                   **_kwargs):
    headers = {"content-type": "application/json; charset=utf-8"}
    if cache_control is not None:
        headers["cache-control"] = cache_control
    headers.update(extra_headers or {})
    return _Response(json.dumps(data), status=status, headers=headers)


def _load(extra_classes=()):
    selected = []
    for node in ENTRY_TREE.body:
        if isinstance(node, ast.Assign):
            names = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if names & HELPER_CONSTANTS:
                selected.append(node)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in HELPER_FUNCTIONS:
                selected.append(node)
        elif isinstance(node, ast.ClassDef) and (
                node.name == "ReadOnlyModeError" or node.name in extra_classes):
            selected.append(node)

    class DurableObject:
        def __init__(self, ctx, env):
            self.ctx = ctx
            self.env = env

    namespace = {
        "re": re,
        "JsURL": _Url,
        "Response": _Response,
        "json_response": _json_response,
        "DurableObject": DurableObject,
        "urlparse": urlparse,
    }
    module = ast.Module(body=selected, type_ignores=[])
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _env(**values):
    return SimpleNamespace(**values)


def _request(url, method="GET", headers=None):
    return SimpleNamespace(url=url, method=method, headers=dict(headers or {}))


class _Storage:
    def __init__(self):
        self.alarm_at = 123
        self.deleted = 0
        self.touched = []

    async def deleteAlarm(self):
        self.deleted += 1
        self.alarm_at = None

    async def setAlarm(self, value):
        self.touched.append(("setAlarm", value))
        self.alarm_at = value

    async def get(self, key):
        self.touched.append(("get", key))
        return None

    async def put(self, key, value):
        self.touched.append(("put", key))



def test_env_flags_are_cheap_and_tolerate_missing_or_null_values():
    ns = _load()

    class JsNull:
        pass

    assert ns["_read_only"](_env()) is False
    assert ns["_read_only"](None) is False
    assert ns["_read_only"](_env(FORKMESH_READ_ONLY=JsNull())) is False
    assert ns["_read_only"](_env(FORKMESH_READ_ONLY="0")) is False
    assert ns["_read_only"](_env(FORKMESH_READ_ONLY="")) is False
    for value in ("1", "true", "YES", " on "):
        assert ns["_read_only"](_env(FORKMESH_READ_ONLY=value)) is True
    assert ns["_read_only"](_env(FORKMESH_DECOMMISSIONED="1")) is True
    assert ns["_decommissioned"](_env(FORKMESH_READ_ONLY="1")) is False

    class Exploding:
        def __getattr__(self, name):
            raise RuntimeError("binding proxy failed")

    assert ns["_read_only"](Exploding()) is False



@pytest.mark.parametrize("sql", [
    "INSERT INTO t (a) VALUES (?)",
    "insert into t (a) values (?) returning id",
    "INSERT INTO t(a) VALUES(?) ON CONFLICT(a) DO UPDATE SET a=excluded.a "
    "RETURNING a",
    "  UPDATE account_sessions SET last_seen_at=? WHERE session_id=?",
    "DELETE FROM host_presence WHERE ts < ?",
    "REPLACE INTO t VALUES (1)",
    "CREATE TABLE IF NOT EXISTS t (a)",
    "ALTER TABLE t ADD COLUMN b",
    "DROP INDEX IF EXISTS i",
    "-- keep the newest\nDELETE FROM t WHERE a < ?",
    "/* purge */ delete from t",
    "WITH doomed AS (SELECT id FROM t WHERE a < ?) "
    "DELETE FROM t WHERE id IN (SELECT id FROM doomed)",
    "with x as (select 1)\ninsert into t select * from x returning *",
    "WITH RECURSIVE r(n) AS (SELECT 1) UPDATE t SET n=(SELECT n FROM r)",
])
def test_write_statements_are_refused_while_read_only(sql):
    ns = _load()
    assert ns["_sql_is_write"](sql) is True
    with pytest.raises(ns["ReadOnlyModeError"]):
        ns["_d1_write_guard"](_env(FORKMESH_READ_ONLY="1"), sql)
    with pytest.raises(ns["ReadOnlyModeError"]):
        ns["_d1_write_guard"](_env(FORKMESH_DECOMMISSIONED="1"), sql)
    ns["_d1_write_guard"](_env(), sql)


@pytest.mark.parametrize("sql", [
    "SELECT * FROM t WHERE updated_at > ?",
    "select deleted_at, replace(name, 'a', 'b') from t",
    "-- INSERT is only in this comment\nSELECT 1",
    "/* DELETE */ SELECT 1",
    "WITH recent AS (SELECT * FROM t WHERE created_at > ?) "
    "SELECT replace(name, 'x', 'y') FROM recent ORDER BY updated_at",
    "WITH a AS (SELECT 'DELETE FROM t' AS s) SELECT s FROM a",
    "PRAGMA table_info(t)",
])
def test_reads_pass_the_guard(sql):
    ns = _load()
    assert ns["_sql_is_write"](sql) is False
    ns["_d1_write_guard"](_env(FORKMESH_READ_ONLY="1"), sql)


def _function(tree, name, cls=None):
    body = tree.body
    if cls is not None:
        body = next(
            n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == cls
        ).body
    return next(
        n for n in body
        if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
        and n.name == name
    )


def _calls(node):
    names = []
    for child in ast.walk(node):
        if isinstance(child, ast.Call):
            func = child.func
            if isinstance(func, ast.Name):
                names.append(func.id)
            elif isinstance(func, ast.Attribute):
                names.append(func.attr)
    return names


@pytest.mark.parametrize("name", [
    "_d1_read",
    "d1_first_primary",
    "d1_run",
    "d1_run_batch",
    "_contribution_run_batch",
    "invalidate_public_catalog_snapshots",
    "_publish_public_snapshot_group",
])
def test_every_d1_write_entry_point_calls_the_guard_before_preparing(name):
    function = ast.unparse(_function(ENTRY_TREE, name))
    assert "_d1_write_guard(env" in function
    guard = function.index("_d1_write_guard(")
    prepare = function.find(".prepare(")
    assert prepare == -1 or guard < prepare


def test_only_the_guarded_helpers_prepare_d1_statements():
    allowed = {
        "_d1_read", "d1_first_primary", "d1_run", "d1_run_batch",
        "_contribution_run_batch", "invalidate_public_catalog_snapshots",
        "_publish_public_snapshot_group", "_apply_schema",
        "_public_snapshot_row",
    }
    preparing = {
        node.name for node in ast.walk(ENTRY_TREE)
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and ".prepare(" in ast.unparse(node)
        and not any(
            isinstance(inner, (ast.FunctionDef, ast.AsyncFunctionDef))
            and inner is not node and ".prepare(" in ast.unparse(inner)
            for inner in ast.walk(node)
        )
    }
    assert preparing <= allowed, preparing - allowed


def test_schema_ddl_and_custody_marker_never_run_read_only():
    apply_schema = ast.unparse(_function(ENTRY_TREE, "_apply_schema"))
    assert apply_schema.index("if _read_only(env):") < apply_schema.index(
        "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
    custody = ast.unparse(
        _function(ENTRY_TREE, "_assert_legacy_wallet_custody_ready"))
    assert custody.index("if _read_only(env):") < custody.index("d1_run(")


def test_get_side_writers_are_skipped_while_read_only():
    lookup = ast.unparse(_function(ENTRY_TREE, "_account_session_lookup"))
    touch = lookup[lookup.index("if touch"):lookup.index("UPDATE account_sessions")]
    assert "not _read_only(env)" in touch
    stats = ast.unparse(_function(ENTRY_TREE, "network_stats"))
    assert "if maintain_presence and (not _read_only(env)):" in stats
    for name in ("_write_error_log", "log_durable_object_abort"):
        body = _function(ENTRY_TREE, name).body
        first = [n for n in body if not isinstance(n, ast.Expr)
                 or not isinstance(getattr(n, "value", None), ast.Constant)]
        assert "_read_only(env)" in ast.unparse(first[0]) or \
            "_read_only(env)" in ast.unparse(first[1]), name
    flush = ast.unparse(_function(ENTRY_TREE, "durable_object_traffic_flush"))
    assert flush.index("_read_only(") < flush.index("getattr(do, '_traffic'")
    inner = ast.unparse(_function(RUNTIME_TREE, "_fetch_inner", "Default"))
    for writer in ("capture_page_view", "record_api_request",
                   "record_site_referral", "record_blog_visit"):
        start = inner.index(writer)
        window = inner[max(0, inner.rfind("\n", 0, start - 120)):start]
        assert "not read_only" in window, writer



@pytest.mark.parametrize("method,url,headers,allowed", [
    ("GET", "https://app.forkmesh.com/api/repositories", {}, True),
    ("HEAD", "https://app.forkmesh.com/health", {}, True),
    ("OPTIONS", "https://app.forkmesh.com/api/login", {}, True),
    ("GET", "https://app.forkmesh.com/alice/repo/info/refs"
            "?service=git-upload-pack", {}, True),
    ("POST", "https://app.forkmesh.com/alice/repo/git-upload-pack", {}, True),
    ("POST", "https://app.forkmesh.com/alice/repo/git-receive-pack", {}, False),
    ("GET", "https://app.forkmesh.com/alice/repo/info/refs"
            "?service=git-receive-pack", {}, False),
    ("POST", "https://app.forkmesh.com/api/login", {}, False),
    ("POST", "https://app.forkmesh.com/mcp", {}, False),
    ("PUT", "https://app.forkmesh.com/api/x", {}, False),
    ("DELETE", "https://app.forkmesh.com/api/repositories", {}, False),
    ("GET", "https://app.forkmesh.com/api/status/ws",
     {"upgrade": "websocket"}, False),
    ("GET", "https://app.forkmesh.com/api/nodes/events",
     {"upgrade": "websocket"}, False),
])
def test_read_only_request_classifier(method, url, headers, allowed):
    ns = _load()
    request = _request(url, method, headers)
    assert ns["_read_only_request_allowed"](request) is allowed
    gate = ns["_maintenance_gate_response"](
        _env(FORKMESH_READ_ONLY="1"), request)
    if allowed:
        assert gate is None
    else:
        assert gate.status == 503
        assert json.loads(gate.body) == {"error": "read_only", "retryAfter": 120}
        assert gate.headers["retry-after"] == "120"
        assert gate.headers["x-forkmesh-expected-degraded"] == "1"
        assert gate.headers["cache-control"] == "no-store"
    assert ns["_maintenance_gate_response"](_env(), request) is None


def test_attribution_relay_is_dropped_silently_and_decommissioned_refuses_all():
    ns = _load()
    relay = _request(
        "https://app.forkmesh.com/api/attribution/capture", "POST")
    dropped = ns["_maintenance_gate_response"](
        _env(FORKMESH_READ_ONLY="1"), relay)
    assert dropped.status == 204
    for request in (relay, _request("https://app.forkmesh.com/health")):
        refused = ns["_maintenance_gate_response"](
            _env(FORKMESH_DECOMMISSIONED="1"), request)
        assert refused.status == 503
        assert json.loads(refused.body) == {"error": "decommissioned"}
        assert refused.headers["x-forkmesh-expected-degraded"] == "1"



def _first_statement(function):
    body = function.body
    if (body and isinstance(body[0], ast.Expr)
            and isinstance(body[0].value, ast.Constant)):
        body = body[1:]
    return body[0]


def test_app_runtime_fetch_and_fetch_inner_gate_first():
    for name in ("fetch", "_fetch_inner"):
        first = ast.unparse(
            _first_statement(_function(RUNTIME_TREE, name, "Default")))
        assert "_maintenance_gate_response(self.env, request)" in first, name
    inner = ast.unparse(_function(RUNTIME_TREE, "_fetch_inner", "Default"))
    handler = inner[inner.index("except ReadOnlyModeError"):]
    assert "_read_only_response()" in handler.split("except", 2)[1]
    assert inner.index("except ReadOnlyModeError") < inner.index(
        "except Exception as error")


def test_scheduled_and_queue_do_nothing_under_a_switch():
    ns = _load()
    scheduled = ast.unparse(_function(RUNTIME_TREE, "scheduled", "Default"))
    assert scheduled.index("if _read_only(env):") < scheduled.index(
        "FORKMESH_CRON_RUNNER")
    queue = ast.unparse(_function(RUNTIME_TREE, "queue", "Default"))
    assert queue.index("if _read_only(env):") < queue.index(
        "await process_notification_queue")
    assert "batch.ackAll()" in queue

    selected = [
        n for n in RUNTIME_TREE.body if isinstance(n, ast.ClassDef)
        and n.name == "Default"
    ]
    calls = []

    async def process_notification_queue(_env, _batch):
        calls.append("queue")

    class Batch:
        acked = 0

        def ackAll(self):
            Batch.acked += 1

    namespace = dict(ns)
    namespace.update({
        "process_notification_queue": process_notification_queue,
        "js_nullish": lambda value: value is None,
        "CRON_RUNNER_NAME": "scheduled-runner-v1",
    })
    cls_node = selected[0]
    methods = [n for n in cls_node.body
               if isinstance(n, ast.AsyncFunctionDef)
               and n.name in ("queue", "scheduled")]
    module = ast.Module(body=[ast.ClassDef(
        name="Default", bases=[], keywords=[], body=methods,
        decorator_list=[], type_params=[])], type_ignores=[])
    ast.fix_missing_locations(module)
    exec(compile(module, str(RUNTIME), "exec"), namespace)
    runtime = namespace["Default"]()
    env = _env(FORKMESH_READ_ONLY="1")
    runtime.env = env
    asyncio.run(runtime.queue(Batch(), env, None))
    assert calls == [] and Batch.acked == 1
    assert asyncio.run(runtime.scheduled(None, env, None)) is False
    decommissioned = _env(FORKMESH_DECOMMISSIONED="1")
    assert asyncio.run(runtime.scheduled(None, decommissioned, None)) is False


def _load_durable_object(name):
    ns = _load(extra_classes=(name,))
    return ns, ns[name]


@pytest.mark.parametrize("flag", ["FORKMESH_READ_ONLY", "FORKMESH_DECOMMISSIONED"])
@pytest.mark.parametrize("object_name", [
    "scheduled-runner-v1", "snapshot-republish-v1", "mirror-wake-v1", "other"])
def test_cron_runner_alarm_deletes_its_alarm_and_does_no_work(flag, object_name):
    ns, runner_cls = _load_durable_object("ForkMeshCronRunner")
    storage = _Storage()
    ctx = SimpleNamespace(storage=storage, id=SimpleNamespace(name=object_name))
    runner = runner_cls(ctx, _env(**{flag: "1"}))
    asyncio.run(runner.alarm())
    assert storage.deleted == 1
    assert storage.alarm_at is None
    assert storage.touched == []


@pytest.mark.parametrize("flag", ["FORKMESH_READ_ONLY", "FORKMESH_DECOMMISSIONED"])
def test_cron_watchdog_alarm_and_heartbeat_stop_without_mail(flag):
    ns, watchdog_cls = _load_durable_object("ForkMeshCronWatchdog")
    storage = _Storage()
    watchdog = watchdog_cls(SimpleNamespace(storage=storage), _env(**{flag: "1"}))
    asyncio.run(watchdog.alarm())
    assert storage.deleted == 1 and storage.touched == []
    response = asyncio.run(watchdog.fetch(_request(
        "https://forkmesh.internal/cron-watchdog/completed", "POST")))
    assert response.status == 503
    assert storage.deleted == 2 and storage.touched == []


def test_background_fetch_paths_refuse_under_a_switch():
    runner = ast.unparse(_function(ENTRY_TREE, "fetch", "ForkMeshCronRunner"))
    guard = runner.index("if _read_only(self.env):")
    assert runner.index("AppRuntime._fetch_inner") < guard
    for doorbell in ("_republish_fetch", "_mirror_wake_fetch",
                     "_coordinator_fetch"):
        assert guard < runner.index(doorbell), doorbell
    gate = ast.unparse(_first_statement(
        _function(ENTRY_TREE, "fetch", "ForkMeshDiscordGate")))
    assert "_decommissioned(self.env)" in gate
    for cls in ("ForkMeshCronWatchdog",):
        for method in ("fetch", "alarm"):
            first = ast.unparse(_first_statement(
                _function(ENTRY_TREE, method, cls)))
            assert "_read_only(self.env)" in first, (cls, method)


def test_every_durable_object_alarm_is_switched():
    alarms = [
        (cls.name, method) for cls in ENTRY_TREE.body
        if isinstance(cls, ast.ClassDef)
        for method in cls.body
        if isinstance(method, ast.AsyncFunctionDef) and method.name == "alarm"
    ]
    assert {name for name, _ in alarms} == {
        "ForkMeshCronWatchdog", "ForkMeshCronRunner"}
    for name, method in alarms:
        first = ast.unparse(_first_statement(method))
        assert "_read_only(self.env)" in first, name
        assert "_maintenance_stop_alarm(self)" in first, name
    world = ast.parse(WORLD_DO.read_text(encoding="utf-8"))
    assert not [
        m for c in world.body if isinstance(c, ast.ClassDef)
        for m in c.body
        if isinstance(m, ast.AsyncFunctionDef) and m.name == "alarm"
    ]


def test_every_socket_message_handler_closes_with_1012_first():
    handlers = []
    for tree, source in ((ENTRY_TREE, "entry"), (ast.parse(
            WORLD_DO.read_text(encoding="utf-8")), "world")):
        for cls in tree.body:
            if not isinstance(cls, ast.ClassDef):
                continue
            for method in cls.body:
                if (isinstance(method, ast.AsyncFunctionDef)
                        and method.name == "webSocketMessage"):
                    handlers.append((source, cls.name, method))
    assert {name for _, name, _ in handlers} == {
        "ForkMeshNoteRoom", "ForkMeshRoom", "ForkMeshNodes",
        "ForkMeshWorld", "ForkMeshOfficeRoom"}
    for source, name, method in handlers:
        first = ast.unparse(_first_statement(method))
        assert "_maintenance_socket_closed(self.env, ws)" in first, name

    ns = _load()
    closed = []
    ws = SimpleNamespace(close=lambda code, reason: closed.append((code, reason)))
    assert ns["_maintenance_socket_closed"](_env(), ws) is False
    assert ns["_maintenance_socket_closed"](
        _env(FORKMESH_READ_ONLY="1"), ws) is True
    assert closed == [(1012, "service restart")]


def test_helpers_are_bound_before_app_runtime_imports_them():
    bind = ENTRY_TEXT.index("_app_runtime_module._bind_runtime(globals())")
    for name in ("def _maintenance_gate_response", "class ReadOnlyModeError",
                 "def _read_only_response", "def _maintenance_socket_closed"):
        assert ENTRY_TEXT.index(name) < bind, name



def test_decommission_wrangler_config_keeps_data_and_drops_routes_and_work():
    original = WRANGLER.read_text(encoding="utf-8")
    variant_text = decommission_wrangler_toml.decommissioned_toml(original)
    before = tomllib.loads(original)
    after = tomllib.loads(variant_text)
    assert before["routes"], "production config is expected to claim app.forkmesh.com"
    assert "routes" not in after
    assert "app.forkmesh.com\"\ncustom_domain" not in variant_text
    assert after["triggers"]["crons"] == []
    assert "consumers" not in after.get("queues", {})
    assert after["queues"]["producers"] == before["queues"]["producers"]
    assert after["workers_dev"] is False and after["preview_urls"] is False
    for key in ("name", "main", "d1_databases", "kv_namespaces",
                "durable_objects", "migrations", "env", "assets"):
        assert after[key] == before[key], key
    assert after["vars"]["FORKMESH_DECOMMISSIONED"] == "1"
    assert after["vars"]["FORKMESH_READ_ONLY"] == "1"
    rest = {k: v for k, v in after["vars"].items()
            if not k.startswith("FORKMESH_")}
    assert rest == before["vars"]
    assert decommission_wrangler_toml.decommissioned_toml(
        variant_text) == variant_text


def test_decommission_tool_writes_a_file(tmp_path):
    output = tmp_path / "wrangler.decommissioned.toml"
    assert decommission_wrangler_toml.main(["-o", str(output)]) == 0
    assert tomllib.loads(output.read_text())["triggers"]["crons"] == []


def _extra_var_args(value):
    deploy = DEPLOY.read_text(encoding="utf-8")
    helper = deploy[deploy.index("EXTRA_VAR_ARGS=()"):deploy.index(
        "\n}\n\ndeploy_app_version()") + 3]
    script = (
        "set -euo pipefail\n" + helper
        + '\napp_extra_var_args\nprintf "%s\\n" '
        '${EXTRA_VAR_ARGS[@]+"${EXTRA_VAR_ARGS[@]}"}\n'
    )
    env = {
        key: value for key, value in os.environ.items()
        if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR",
                       "FORKMESH_EXTRA_VARS")
    }
    if value is not None:
        env["FORKMESH_EXTRA_VARS"] = value
    return subprocess.run(
        ["bash", "-c", script], env=env, text=True, capture_output=True,
        check=False)


def test_deploy_extra_vars_hook_is_opt_in_and_validated():
    unset = _extra_var_args(None)
    assert unset.returncode == 0 and unset.stdout.strip() == ""
    one = _extra_var_args("FORKMESH_READ_ONLY:1")
    assert one.returncode == 0
    assert one.stdout.split() == ["--var", "FORKMESH_READ_ONLY:1"]
    two = _extra_var_args("FORKMESH_READ_ONLY:1, FORKMESH_DECOMMISSIONED:1")
    assert two.stdout.split() == [
        "--var", "FORKMESH_READ_ONLY:1", "--var", "FORKMESH_DECOMMISSIONED:1"]
    bad = _extra_var_args("FORKMESH_READ_ONLY=1")
    assert bad.returncode != 0 and "NAME:VALUE" in bad.stderr
    deploy = DEPLOY.read_text(encoding="utf-8")
    app_deploy = deploy.split("deploy_app_version() {", 1)[1].split(
        "\n}\n\nrequire_edge_router_public_key()", 1)[0]
    assert "app_extra_var_args || return 1" in app_deploy
    assert app_deploy.index("worker_args+=(") < app_deploy.index(
        "pywrangler_deploy_with_retry")
