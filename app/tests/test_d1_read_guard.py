#!/usr/bin/env python3
"""D1 read guard (2026-09-14).

The D1 tab showed 3.7M of the 5M daily Free rows read by 22:00 UTC. The top
readers were table-wide COUNTs behind admin views, the users directory join,
its commit totals, and scans whose per-isolate memos never survived isolate
churn. These tests pin the shared colo copy those readers now go through, the
operator switch that makes them conserve, and the direct query fixes.
"""

import ast
import asyncio
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
from _maintenance_stubs import with_maintenance_off

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
SRC = ROOT / "src"
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
USAGE_TEXT = (SRC / "usage_reporting.py").read_text(encoding="utf-8")
ADMIN_TEXT = (SRC / "admin_console.py").read_text(encoding="utf-8")
ACCOUNTS_TEXT = (SRC / "account_users.py").read_text(encoding="utf-8")
WORLD_ROUTES_TEXT = (
    REPO / "world" / "src" / "world_routes.py").read_text(encoding="utf-8")
DAY_MS = 1_789_430_400_000  # 2026-09-15T00:00:00Z


def _clock(box):
    return SimpleNamespace(now=lambda: box["now"])


def _entry_host_helpers():
    namespace = {}
    for node in ast.parse(ENTRY_TEXT).body:
        if isinstance(node, ast.FunctionDef) and node.name in (
                "_host_background", "_host_single_flight"):
            exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
    return {name: namespace[name]
            for name in ("_host_background", "_host_single_flight")}


class Runtime:
    """Enough of entry.py for d1_read_guard: D1 rows, Cache API, responses."""

    def __init__(self, guard_row=None, now=DAY_MS + 3_600_000):
        self.clock = {"now": now}
        self.guard_row = guard_row
        self.cache = {}
        self.guard_reads = 0
        self.writes = []
        self.admin_ok = True
        self.audits = []

    def module(self):
        spec = importlib.util.spec_from_file_location(
            "d1_read_guard_under_test", SRC / "d1_read_guard.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        runtime = self

        async def d1_first(_env, sql, *args):
            assert "d1_read_guard" in sql and args == ("d1-rows-read",)
            runtime.guard_reads += 1
            return runtime.guard_row

        async def d1_run(_env, sql, *args):
            runtime.writes.append((sql, args))

        async def edge_cache_match(key):
            return runtime.cache.get(key)

        async def edge_cache_put(key, response):
            runtime.cache[key] = response

        async def edge_cache_delete(key):
            runtime.cache.pop(key, None)

        async def response_json(response):
            return json.loads(response.body)

        async def admin_authorized(_env, node, ts, sig, canonical):
            runtime.canonical = canonical
            return {"name": node} if runtime.admin_ok else None

        async def ensure_schema(_env):
            return None

        async def bounded_json_request(request):
            return request.body

        async def audit(*args):
            runtime.audits.append(args)

        module._bind_runtime({
            "Date": _clock(self.clock),
            "time": __import__("time"),
            "d1_first": d1_first,
            "d1_run": d1_run,
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "edge_cache_delete": edge_cache_delete,
            "_response_json": response_json,
            "json_response": lambda data, status=200, cache_control=None:
                SimpleNamespace(body=json.dumps(data), status=status,
                                cache_control=cache_control),
            "_admin_authorized": admin_authorized,
            "ensure_schema": ensure_schema,
            "bounded_json_request": bounded_json_request,
            "_audit_sensitive_action": audit,
            "method_name": lambda request: request.method,
            "clean_string": lambda value, size: str(value or "")[:size],
            "MAX_NODE_NAME": 64,
            **_entry_host_helpers(),
        })
        return module


def _run(coro):
    return asyncio.run(coro)


def _loader(calls, value):
    async def load():
        calls.append(1)
        return value
    return load


def _sample(percent, day="2026-09-15", mode=None, threshold=None):
    return {"mode": mode, "threshold": threshold, "day": day,
            "percent": percent, "sampled_at": DAY_MS}


def _new_isolate(guard):
    guard._READ_GUARD_LOCAL.clear()
    guard._READ_GUARD_STATE.update({"ts": 0, "state": None})


# --- shared copies ----------------------------------------------------------

def test_a_cold_isolate_reuses_the_colo_copy_instead_of_rescanning():
    runtime = Runtime()
    guard = runtime.module()
    calls = []
    load = _loader(calls, {"u1": 4})
    assert _run(guard.shared_read(object(), "commit-totals", load)) == {"u1": 4}
    for _ in range(3):
        _new_isolate(guard)
        runtime.clock["now"] += 60_000
        assert _run(guard.shared_read(
            object(), "commit-totals", load)) == {"u1": 4}
    assert len(calls) == 1
    # Past the reader's normal TTL the next caller scans once more.
    runtime.clock["now"] += 30 * 60_000
    _new_isolate(guard)
    _run(guard.shared_read(object(), "commit-totals", load))
    assert len(calls) == 2


def test_variants_keep_separate_copies_and_forget_drops_one():
    runtime = Runtime()
    guard = runtime.module()
    calls = []
    for rev in ("7", "8", "7"):
        _run(guard.shared_read(
            object(), "repositories-count", _loader(calls, {"n": int(rev)}),
            rev))
    assert len(calls) == 2
    _run(guard.shared_forget("repositories-count", "7"))
    assert _run(guard.shared_read(
        object(), "repositories-count", _loader(calls, {"n": 70}), "7")) == {
            "n": 70}
    assert len(calls) == 3


def test_conserving_stretches_the_ttl_and_serves_the_fallback_on_a_miss():
    runtime = Runtime(guard_row=_sample(75.0))
    guard = runtime.module()
    calls = []
    load = _loader(calls, [{"name": "users", "rowCount": 9}])
    _run(guard.shared_read(object(), "users-directory", load))
    # 50 minutes is past the 5-minute normal TTL, inside the 1 h conserve one.
    runtime.clock["now"] += 50 * 60_000
    _new_isolate(guard)
    _run(guard.shared_read(object(), "users-directory", load))
    assert len(calls) == 1
    # An inventory with no copy yet serves its fallback instead of counting.
    assert _run(guard.shared_read(
        object(), "table-inventory", load, fallback=[])) == []
    assert len(calls) == 1
    # Without a fallback a miss still has to be answered from D1.
    _run(guard.shared_read(object(), "admin-stats", load))
    assert len(calls) == 2


def test_the_copy_is_publicly_cacheable_for_the_longest_ttl():
    runtime = Runtime()
    guard = runtime.module()
    _run(guard.shared_read(object(), "admin-stats", _loader([], {"repos": 3})))
    (stored,) = runtime.cache.values()
    # The Cache API keeps public responses; freshness comes from the ts.
    assert stored.cache_control == "public, max-age=3600"
    assert json.loads(stored.body)["value"] == {"repos": 3}


# --- the switch ---------------------------------------------------------------

def test_auto_engages_at_the_threshold_for_today_only():
    for row, conserving in (
            (None, False),
            (_sample(59.9), False),
            (_sample(60.0), True),
            (_sample(95.0, day="2026-09-14"), False),
            (_sample(30.0, mode="auto", threshold=25), True),
            (_sample(5.0, mode="on"), True),
            (_sample(99.0, mode="off"), False)):
        runtime = Runtime(guard_row=row)
        state = _run(runtime.module().read_guard_state(object()))
        assert state["conserving"] is conserving, (row, state)
        assert state["mode"] in ("auto", "on", "off")


def test_the_switch_is_read_once_a_minute_per_isolate():
    runtime = Runtime(guard_row=_sample(10.0))
    guard = runtime.module()
    for _ in range(5):
        _run(guard.read_guard_state(object()))
    assert runtime.guard_reads == 1
    runtime.clock["now"] += 61_000
    _run(guard.read_guard_state(object()))
    assert runtime.guard_reads == 2


def test_a_database_behind_the_migration_reads_the_defaults():
    runtime = Runtime()
    guard = runtime.module()

    async def missing_table(*_args):
        raise RuntimeError("no such table: d1_read_guard")

    guard.d1_first = missing_table
    state = _run(guard.read_guard_state(object()))
    assert (state["mode"], state["threshold"], state["conserving"]) == (
        "auto", 60, False)


def test_admin_post_sets_the_switch_and_returns_the_report():
    runtime = Runtime()
    guard = runtime.module()
    request = SimpleNamespace(method="POST", body={
        "node": "root", "ts": "123", "sig": "s", "mode": "on",
        "threshold": "70"})
    response = _run(guard.admin_d1_read_guard(object(), request))
    payload = json.loads(response.body)
    assert payload["ok"] is True
    assert runtime.canonical == (
        b"forkmesh-admin-d1-read-guard-v1\nroot\n123\non\n70")
    ((sql, args),) = runtime.writes
    assert "INSERT INTO d1_read_guard" in sql
    assert args[:2] == ("on", 70) and args[3] == "root"
    assert [reader["name"] for reader in payload["readGuard"]["readers"]] == \
        list(guard.READ_GUARD_READERS)
    assert runtime.audits


def test_admin_post_refuses_bad_signatures_and_values():
    runtime = Runtime()
    guard = runtime.module()
    runtime.admin_ok = False
    request = SimpleNamespace(method="POST", body={
        "node": "root", "ts": "1", "sig": "s", "mode": "on",
        "threshold": "60"})
    assert _run(guard.admin_d1_read_guard(object(), request)).status == 401
    runtime.admin_ok = True
    for mode, threshold in (("sometimes", "60"), ("on", "0"), ("on", "101")):
        request.body.update({"mode": mode, "threshold": threshold})
        assert _run(guard.admin_d1_read_guard(
            object(), request)).status == 400
    assert runtime.writes == []
    get = SimpleNamespace(method="GET", body={})
    assert _run(guard.admin_d1_read_guard(object(), get)).status == 405


def test_the_2026_09_14_top_statements_name_their_guarded_readers():
    guard = Runtime().module()
    assert guard.reader_for_query(
        'SELECT COUNT(*) AS row_count FROM "api_metrics_minute"') == \
        "table-inventory"
    assert guard.reader_for_query(
        'SELECT ? AS t, COUNT(*) AS n FROM "sensitive_audit_log"') == \
        "admin-table-counts"
    assert guard.reader_for_query(
        "SELECT u.user_bi,a.total_active_ms,a.total_steps,c.issues,") == \
        "users-directory"
    assert guard.reader_for_query(
        "WITH ranked_sources AS ( SELECT projects.source_repo_bi, "
        "projects.active_generation_bi, ROW_NUMBER() OVER (") == \
        "commit-totals"
    # The per-profile language query is a different statement.
    assert guard.reader_for_query(
        "WITH ranked_sources AS ( SELECT projects.source_repo_bi, "
        "projects.project_bi,") == ""
    assert guard.reader_for_query(
        "WITH resolved AS (SELECT c.issues,c.pulls,c.commits,c.total,") == \
        "contributor-board"
    assert guard.reader_for_query(
        "SELECT COUNT(*) AS n FROM repositories") == "repositories-count"
    assert guard.reader_for_query(
        "SELECT COUNT(DISTINCT +run) AS n FROM install_diag WHERE step=? "
        "AND ts >= ?") == "admin-stats"
    assert guard.reader_for_query("SELECT 1") == ""


# --- wiring -----------------------------------------------------------------

def test_heavy_readers_go_through_the_shared_copy():
    for text, needle in (
            (ENTRY_TEXT, 'shared(env, "repositories-count", scan, str(rev))'),
            (ENTRY_TEXT, 'shared(env, "contributor-board", contrib_scan)'),
            (ENTRY_TEXT, 'shared(env, "commit-totals", scan)'),
            (ENTRY_TEXT, 'shared(env, "admin-stats", compute)'),
            (ACCOUNTS_TEXT, 'shared(env, "users-directory", scan)'),
            (ADMIN_TEXT, 'd1_shared_read(env, "admin-table-counts", count_all)'),
            (WORLD_ROUTES_TEXT, 'env, "table-inventory", lambda: inventory(')):
        assert needle in text, needle
    assert "_USERS_DIRECTORY_ROWS_MEMO" not in ENTRY_TEXT + ACCOUNTS_TEXT
    assert "_CONTRIBUTION_COMMIT_TOTALS_MEMO" not in ENTRY_TEXT
    assert "ADMIN_COUNTS_CACHE_KEY" not in ADMIN_TEXT
    route = '"/api/accounts/admin-d1-read-guard" and method == "POST"'
    assert route in ENTRY_TEXT
    footprint = (REPO / "world" / "tools" / "build_worker_footprint.py"
                 ).read_text(encoding="utf-8")
    assert '"d1_read_guard.py",' in footprint


def test_roster_invalidation_drops_the_shared_rows_too():
    forget = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _users_directory_cache_forget("):]
    forget = forget[:forget.index("\n# ")]
    assert "await edge_cache_delete(USERS_DIRECTORY_CACHE_KEY)" in forget
    assert 'await forget("users-directory")' in forget
    for text in (ENTRY_TEXT, ACCOUNTS_TEXT):
        # The helper's own body is the one direct delete left.
        assert text.count(
            "await edge_cache_delete(USERS_DIRECTORY_CACHE_KEY)") == (
                1 if text is ENTRY_TEXT else 0)


def test_admin_stats_install_count_walks_the_ts_range_not_the_run_index():
    import sqlite3
    db = sqlite3.connect(":memory:")
    db.executescript(
        "CREATE TABLE install_diag (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts INTEGER NOT NULL, run TEXT NOT NULL, step TEXT NOT NULL,"
        " ok INTEGER NOT NULL);"
        "CREATE INDEX idx_install_diag_ts ON install_diag(ts);"
        "CREATE INDEX idx_install_diag_run ON install_diag(run);")
    sql = ("SELECT COUNT(DISTINCT +run) AS n FROM install_diag "
           "WHERE step='start' AND ts >= ?")
    assert '"SELECT COUNT(DISTINCT +run) AS n FROM install_diag "' in ENTRY_TEXT
    plan = " ".join(
        str(row[-1]) for row in db.execute("EXPLAIN QUERY PLAN " + sql, (0,)))
    assert "idx_install_diag_ts" in plan and "idx_install_diag_run" not in plan


def _entry_function(name, extra):
    tree = ast.parse(ENTRY_TEXT)
    (node,) = [n for n in tree.body
               if isinstance(n, ast.AsyncFunctionDef) and n.name == name]
    namespace = dict(extra)
    exec(compile(ast.Module(body=[node], type_ignores=[]), name, "exec"),
         with_maintenance_off(namespace))
    return namespace[name]


def test_error_log_trim_is_a_primary_key_range_from_the_new_id():
    for last_id, expected in (
            (12_000, [("DELETE FROM error_log WHERE id <= ?", (7_000,))]),
            (4_000, []),
            (None, ["NOT IN"])):
        statements = []

        async def d1_run(_env, sql, *args, last_id=last_id):
            statements.append((" ".join(sql.split()), args))
            if sql.strip().startswith("INSERT"):
                if last_id is None:
                    return None
                return SimpleNamespace(meta=SimpleNamespace(last_row_id=last_id))

        async def d1_first(*_args):
            return {"hit": 1}

        async def ensure_schema(_env):
            return None

        write = _entry_function("_write_error_log", {
            "d1_run": d1_run, "d1_first": d1_first,
            "ensure_schema": ensure_schema,
            "_privacy_safe_error_text": lambda _path, message: message,
            "clean_string": lambda value, size: str(value or "")[:size],
            "Date": SimpleNamespace(now=lambda: 1), "MAX_ERROR_LOG": 5000,
            "MAX_NODE_NAME": 64,
        })
        _run(write(object(), 500, "GET", "/x", "boom"))
        trims = [s for s in statements if not s[0].startswith("INSERT")]
        if expected == ["NOT IN"]:
            assert len(trims) == 1 and "NOT IN" in trims[0][0]
        else:
            assert trims == expected


def test_notification_counts_read_only_unread_and_skip_total_on_a_short_page():
    handler = ENTRY_TEXT[ENTRY_TEXT.index("async def notifications_handler("):]
    handler = handler[:handler.index('"pauseUntil"')]
    assert "SUM(CASE WHEN read_at=0" not in handler
    assert '"WHERE recipient_bi=? AND read_at=0"' in handler
    assert "if len(rows) < limit and (rows or offset == 0):" in handler
    assert handler.index("rows = await d1_all(") < handler.index(
        "if len(rows) < limit")


def test_the_usage_sampler_records_d1_rows_read_with_a_ladder_from_half():
    sampler = USAGE_TEXT[
        USAGE_TEXT.index("async def record_usage_budget_sample("):]
    assert '"d1-rows-read", "D1 rows read", D1_FREE_ROWS_READ_PER_DAY' in sampler
    assert 'metric == "d1-rows-read" and threshold < 50' in sampler
    usage = USAGE_TEXT[USAGE_TEXT.index("async def _cloudflare_usage_today("):]
    usage = usage[:usage.index("def _safe_d1_query_text(")]
    assert "d1Rows: d1AnalyticsAdaptiveGroups" in usage
    assert '"d1-rows-read": first_sum(account.get("d1Rows")' in usage
    report = USAGE_TEXT[USAGE_TEXT.index("async def _admin_d1_usage_report("):]
    assert report.count("await _d1_read_guard_fields(env,") == 2


class _HostBackground:
    def __init__(self, accept=True):
        self.accept = accept
        self.scheduled = []
        self.joins = []

    def __call__(self, key, factory):
        if not self.accept:
            return False
        self.scheduled.append((key, factory))
        return True

    async def join(self, key, factory):
        self.joins.append(key)
        return await factory()


def test_a_fallback_reader_never_scans_on_the_request_when_the_host_can():
    runtime = Runtime()
    guard = runtime.module()
    env = SimpleNamespace(HOST_BACKGROUND=_HostBackground())
    calls = []
    load = _loader(calls, [{"table": "users", "rows": 3}])
    assert _run(guard.shared_read(
        env, "table-inventory", load, fallback=[])) == []
    assert calls == []
    key, factory = env.HOST_BACKGROUND.scheduled[0]
    assert key.startswith("shared-read:") and key.endswith("table-inventory")
    assert _run(factory()) == [{"table": "users", "rows": 3}]
    assert _run(guard.shared_read(
        env, "table-inventory", load, fallback=[])) == [
            {"table": "users", "rows": 3}]
    assert calls == [1]
    runtime.clock["now"] += 7 * 3_600_000
    _new_isolate(guard)
    assert _run(guard.shared_read(
        env, "table-inventory", load, fallback=[])) == [
            {"table": "users", "rows": 3}]
    assert calls == [1] and len(env.HOST_BACKGROUND.scheduled) == 2


def test_a_reader_without_fallback_joins_one_host_flight():
    runtime = Runtime()
    guard = runtime.module()
    env = SimpleNamespace(HOST_BACKGROUND=_HostBackground())
    calls = []
    load = _loader(calls, {"u1": 4})
    assert _run(guard.shared_read(env, "commit-totals", load)) == {"u1": 4}
    assert env.HOST_BACKGROUND.joins == [
        "shared-read:" + guard._cache_key("commit-totals", "")]
    assert env.HOST_BACKGROUND.scheduled == []
    assert calls == [1]


def test_without_a_host_the_fallback_reader_still_scans_inline():
    runtime = Runtime()
    guard = runtime.module()
    calls = []
    load = _loader(calls, [{"table": "users", "rows": 3}])
    env = SimpleNamespace(HOST_BACKGROUND=_HostBackground(accept=False))
    for target in (object(), env):
        _new_isolate(guard)
        runtime.cache.clear()
        assert _run(guard.shared_read(
            target, "table-inventory", load, fallback=[])) == [
                {"table": "users", "rows": 3}]
    assert calls == [1, 1]

