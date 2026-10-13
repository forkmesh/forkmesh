#!/usr/bin/env python3
"""A one-off D1 backend fault must not become a 500 error group.

Production surfaced `500 GET /api/accounts/<name> — D1_ERROR: internal error;
reference = <id>`: a single opaque D1 platform blip on a public profile read
escaped the router and became a Cloudflare 1101. These tests pin both halves
of the fix.

  * the read helpers replay a transient fault once, so the request succeeds;
  * a sustained fault (D1 down / overloaded) answers 503 + Retry-After marked
    expected-degraded, never a re-raised 500 — and overload is NEVER replayed,
    because retrying into an overloaded database is what turned the
    2026-07-11 free-plan incident into a death spiral.
"""

import ast
import asyncio
from pathlib import Path
from _maintenance_stubs import with_maintenance_off

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
APP_RUNTIME = ENTRY.with_name("app_runtime.py")

READ_FUNCS = {
    "_is_transient_d1_error", "_d1_read", "d1_all", "d1_first",
    "d1_bind_args", "d1_row_to_dict", "js_nullish", "_safe_error_text",
    "_cron_d1_query_charge", "_cron_subrequest_charge", "_log_d1_query",
    "_d1_query_origin",
}
ROUTER_FUNCS = {
    "_is_transient_d1_error", "_is_d1_platform_error", "_safe_error_text",
}

D1_INTERNAL = "D1_ERROR: internal error; reference = gajm603ikm069q0eol2gc9j2"
D1_OVERLOADED = "D1_ERROR: D1 DB is overloaded."


def _load(names, extra=None):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [n for n in tree.body
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                and n.name in names]
    assert {n.name for n in selected} == names, "missing functions"
    # Module-level marker tuples the predicate reads.
    wanted_consts = {
        "_D1_TRANSIENT_MARKERS", "_D1_SUSTAINED_MARKERS",
        "CRON_PHASE_D1_QUERY_LIMIT", "_CRON_D1_BUDGET_VAR",
        "D1_QUERY_LOG_MAX_CHARS", "_D1_QUERY_CALLER",
    }
    consts = [n for n in tree.body
              if isinstance(n, ast.Assign)
              and any(getattr(t, "id", "") in wanted_consts
                      for t in n.targets)]
    found_consts = {
        target.id for node in consts for target in node.targets
        if isinstance(target, ast.Name) and target.id in wanted_consts
    }
    assert found_consts == wanted_consts, "missing D1 budget constants"
    mod = ast.fix_missing_locations(
        ast.Module(body=consts + selected, type_ignores=[]))
    ns = dict(extra or {})
    exec(compile(mod, str(ENTRY), "exec"), with_maintenance_off(ns))
    return ns


class _Stmt:
    def __init__(self, db, sql):
        self._db = db
        self._sql = sql
        self._args = ()

    def bind(self, *args):
        self._args = args
        return self

    async def _run(self):
        self._db.attempts.append((self._sql, self._args))
        error = self._db.errors.pop(0) if self._db.errors else None
        if error is not None:
            raise RuntimeError(error)
        return self._db.result

    async def first(self):
        return await self._run()

    async def all(self):
        return await self._run()


class _DB:
    """env.DB whose queries fail with a scripted list of errors."""

    def __init__(self, errors=(), result=None):
        self.errors = list(errors)
        self.result = result
        self.attempts = []

    def prepare(self, sql):
        return _Stmt(self, sql)


class _Env:
    def __init__(self, db):
        self.DB = db


class _Results:
    def __init__(self, rows):
        self.results = rows


# --- the read helpers replay a transient fault -------------------------------

def test_transient_d1_fault_is_replayed_once_and_succeeds():
    ns = _load(READ_FUNCS)
    db = _DB(errors=[D1_INTERNAL], result={"name": "magnetic-mirror-8594"})
    row = asyncio.run(ns["d1_first"](
        _Env(db), "SELECT data FROM nodes WHERE node_bi=?", "bi:x"))

    # The caller sees the row, not the platform fault.
    assert row == {"name": "magnetic-mirror-8594"}
    # Exactly two attempts, and the replay rebuilt the bound statement: a
    # prepared statement that already failed is not guaranteed re-awaitable.
    assert db.attempts == [
        ("SELECT data FROM nodes WHERE node_bi=?", ("bi:x",)),
        ("SELECT data FROM nodes WHERE node_bi=?", ("bi:x",)),
    ]


def test_d1_all_replays_the_same_transient_fault():
    ns = _load(READ_FUNCS)
    db = _DB(errors=[D1_INTERNAL], result=_Results([{"k": "v"}]))
    rows = asyncio.run(ns["d1_all"](_Env(db), "SELECT k FROM t"))
    assert rows == [{"k": "v"}]
    assert len(db.attempts) == 2


def test_replay_is_bounded_at_one_retry():
    ns = _load(READ_FUNCS)
    db = _DB(errors=[D1_INTERNAL, D1_INTERNAL, D1_INTERNAL])
    try:
        asyncio.run(ns["d1_first"](_Env(db), "SELECT 1"))
        raise AssertionError("a persistent D1 fault must still propagate")
    except RuntimeError as exc:
        assert "internal error" in str(exc)
    # Two attempts total — never an unbounded retry loop against a sick DB.
    assert len(db.attempts) == 2


def test_overloaded_d1_is_never_replayed():
    ns = _load(READ_FUNCS)
    db = _DB(errors=[D1_OVERLOADED, D1_OVERLOADED])
    try:
        asyncio.run(ns["d1_first"](_Env(db), "SELECT 1"))
        raise AssertionError("an overloaded D1 must fail fast")
    except RuntimeError as exc:
        assert "overloaded" in str(exc)
    # Replaying reads into an overloaded database is the amplification that
    # turned the 2026-07-11 free-plan incident into a death spiral.
    assert len(db.attempts) == 1


def test_our_own_bad_query_is_not_replayed():
    ns = _load(READ_FUNCS)
    db = _DB(errors=["D1_ERROR: no such table: nodes", "unreachable"])
    try:
        asyncio.run(ns["d1_first"](_Env(db), "SELECT 1 FROM nodes"))
        raise AssertionError("a schema error must propagate immediately")
    except RuntimeError as exc:
        assert "no such table" in str(exc)
    assert len(db.attempts) == 1


def test_writes_are_never_replayed():
    """d1_run must keep failing fast — not every write is idempotent."""
    source = ENTRY.read_text(encoding="utf-8")
    # Anchor on the module-level helper, not the request-context method that
    # merely forwards to it.
    body = source.split("\nasync def d1_run(", 1)[1].split("\nasync def ", 1)[0]
    assert "_d1_read" not in body
    assert "await stmt.run()" in body


# --- the router turns an escaped D1 outage into backpressure ------------------

def test_platform_faults_are_classified_but_our_bugs_are_not():
    ns = _load(ROUTER_FUNCS)
    is_platform = ns["_is_d1_platform_error"]
    # Both the one-off blip and the sustained outage are D1's failure, so both
    # become a 503 rather than a re-raised 500.
    assert is_platform(RuntimeError(D1_INTERNAL))
    assert is_platform(RuntimeError(D1_OVERLOADED))
    # Our own broken SQL and unrelated bugs must keep their 500 + Sentry stack.
    assert not is_platform(RuntimeError("D1_ERROR: no such column: bogus"))
    assert not is_platform(KeyError("name"))
    assert not is_platform(RuntimeError("internal error; reference = x"))


def test_router_answers_503_degraded_instead_of_re_raising():
    source = APP_RUNTIME.read_text(encoding="utf-8")
    handler = source.split("            if str(error) == "
                           "\"legacy_custody_migration_required\":", 1)[1]
    handler = handler.split("        try:\n            status = int(", 1)[0]
    assert "_is_d1_platform_error(error)" in handler
    # 503 + Retry-After so clients back off, and the degraded marker keeps the
    # outer 5xx logger from double-logging what log_d1_unavailable recorded.
    assert "status=503" in handler
    assert "\"Retry-After\": \"2\"" in handler
    assert "EXPECTED_DEGRADED_HEADERS" in handler
    assert "log_d1_unavailable(" in handler
    # The generic 1101 capture stays reachable for every other exception.
    assert "capture_worker_exception" in handler


def test_d1_outage_is_logged_to_d1_only_never_to_sentry():
    """No stack-traced Sentry ERROR for an expected dependency outage, or
    real bugs drown in the noise."""
    source = ENTRY.read_text(encoding="utf-8")
    body = source.split("async def log_d1_unavailable(", 1)[1]
    body = body.split("\nasync def ", 1)[0]
    assert "_write_error_log(" in body
    assert "capture_sentry_error" not in body
    assert "log_error(" not in body


# --- every statement reaches the Worker log ----------------------------------

def test_every_d1_attempt_logs_its_sql_but_never_the_bound_values(capsys):
    # The desktop cloud log monitor renders the Worker's console lines, so a
    # line per statement (replays included) is how D1 traffic shows up there.
    ns = _load(READ_FUNCS)
    db = _DB(errors=[D1_INTERNAL], result={"name": "magnetic-mirror-8594"})
    asyncio.run(ns["d1_first"](
        _Env(db), "SELECT data\n  FROM nodes WHERE node_bi=?", "bi:secret"))

    lines = capsys.readouterr().out.splitlines()
    assert lines == [
        "D1 query: SELECT data FROM nodes WHERE node_bi=? [1 arg]",
    ] * 2
    assert "bi:secret" not in "\n".join(lines)


def test_long_d1_statements_are_elided_under_the_desktop_line_cap(capsys):
    ns = _load(READ_FUNCS)
    db = _DB(result=_Results([]))
    asyncio.run(ns["d1_all"](_Env(db), "SELECT " + "x," * 400 + "y FROM t"))

    (line,) = capsys.readouterr().out.splitlines()
    assert line.startswith("D1 query: SELECT x,x,")
    assert line.endswith("...")
    assert len(line) <= len("D1 query: ") + ns["D1_QUERY_LOG_MAX_CHARS"]


def test_d1_lines_name_the_worker_module_and_function_that_issued_them(capsys):
    # The helpers are walked past, so the line names the handler that asked
    # for the statement; a caller outside app/src (this test) names nothing,
    # which is why the lines above carry no origin clause.
    ns = _load(READ_FUNCS)
    db = _DB(result=_Results([]))
    caller = compile(
        "async def list_pulls(env):\n"
        "    return await d1_all(env, 'SELECT id FROM pulls WHERE repo=?', 'r')\n",
        str(ENTRY.with_name("repositories.py")), "exec")
    exec(caller, with_maintenance_off(ns))
    asyncio.run(ns["list_pulls"](_Env(db)))

    (line,) = capsys.readouterr().out.splitlines()
    assert line == ("D1 query: SELECT id FROM pulls WHERE repo=? [1 arg] "
                    "(from repositories.py in list_pulls)")


class _Caller:
    def __init__(self, text):
        self.text = text

    def clause(self):
        return self.text


def test_d1_lines_name_the_caller_the_host_reports_before_the_origin(capsys):
    ns = _load(READ_FUNCS)
    db = _DB(result=_Results([]))
    caller = compile(
        "async def list_pulls(env):\n"
        "    return await d1_all(env, 'SELECT id FROM pulls WHERE repo=?', 'r')\n",
        str(ENTRY.with_name("repositories.py")), "exec")
    exec(caller, ns)
    ns["_D1_QUERY_CALLER"] = _Caller(
        "GET /api/pulls, IP 203.0.113.9, user alice, session abcd1234")
    asyncio.run(ns["list_pulls"](_Env(db)))
    ns["_D1_QUERY_CALLER"] = _Caller("")
    asyncio.run(ns["list_pulls"](_Env(db)))

    assert capsys.readouterr().out.splitlines() == [
        "D1 query: SELECT id FROM pulls WHERE repo=? [1 arg] "
        "(for GET /api/pulls, IP 203.0.113.9, user alice, session abcd1234) "
        "(from repositories.py in list_pulls)",
        "D1 query: SELECT id FROM pulls WHERE repo=? [1 arg] "
        "(from repositories.py in list_pulls)",
    ]
