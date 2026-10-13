#!/usr/bin/env python3
"""Regression tests for D1 row conversion helpers.

Workers Python may return JavaScript null/undefined sentinels rather than Python
None from D1 first()/all(). Load only the small pure helpers from entry.py so
this test runs without the Workers JS runtime.
"""
import ast
import asyncio
from pathlib import Path
from _maintenance_stubs import with_maintenance_off

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
# d1_all/d1_first issue their query through _d1_read, which replays a transient
# D1 platform fault (see test_d1_transient_errors.py), so its helper chain has
# to come along for these conversion tests to execute the real code path.
_WANT_FUNCS = (
    "js_nullish", "d1_bind_args", "d1_row_to_dict", "d1_all", "d1_first",
    "d1_run", "_d1_read", "_is_transient_d1_error", "_safe_error_text",
    "_cron_d1_query_charge", "_cron_subrequest_charge", "_log_d1_query",
    "_d1_query_origin",
)
_WANT_CONSTS = (
    "_D1_TRANSIENT_MARKERS", "_D1_SUSTAINED_MARKERS",
    "CRON_PHASE_D1_QUERY_LIMIT", "_CRON_D1_BUDGET_VAR",
    "D1_QUERY_LOG_MAX_CHARS",
)
_JS_NULL = object()


def _load():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                getattr(t, "id", "") in _WANT_CONSTS for t in node.targets):
            body.append(node)
        if isinstance(node, ast.FunctionDef) and node.name in _WANT_FUNCS:
            body.append(node)
        if isinstance(node, ast.AsyncFunctionDef) and node.name in _WANT_FUNCS:
            body.append(node)
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)
    ns = {"jsnull": _JS_NULL}
    exec(compile(mod, str(ENTRY), "exec"), with_maintenance_off(ns))
    return ns


class JsNull:
    pass


class JsUndefined:
    pass


class _Stmt:
    def __init__(self, row=None, results=None):
        self.row = row
        self.results = results
        self.bound = ()

    def bind(self, *args):
        self.bound = args
        return self

    async def first(self):
        return self.row

    async def all(self):
        return type("D1Result", (), {"results": self.results})()

    async def run(self):
        return None


class _DB:
    def __init__(self, row=None, results=None):
        self.stmt = _Stmt(row=row, results=results)

    def prepare(self, sql):
        self.sql = sql
        return self.stmt


class _Env:
    def __init__(self, row=None, results=None):
        self.DB = _DB(row=row, results=results)


def test_js_nullish_detects_workers_js_nullish_sentinels():
    ns = _load()

    assert ns["js_nullish"](JsNull()) is True
    assert ns["js_nullish"](JsUndefined()) is True
    assert ns["js_nullish"](None) is True
    assert ns["js_nullish"]({}) is False


def test_d1_first_treats_js_nullish_as_no_row():
    ns = _load()

    for sentinel in (JsNull(), JsUndefined()):
        row = asyncio.run(ns["d1_first"](_Env(row=sentinel), "SELECT data FROM accounts WHERE name_bi=?", "abc"))
        assert row is None


def test_d1_first_keeps_real_rows():
    ns = _load()

    row = asyncio.run(ns["d1_first"](_Env(row={"data": "ok"}), "SELECT data FROM accounts WHERE name_bi=?", "abc"))

    assert row == {"data": "ok"}


def test_d1_all_treats_js_nullish_results_as_empty():
    ns = _load()

    for sentinel in (JsNull(), JsUndefined()):
        rows = asyncio.run(ns["d1_all"](_Env(results=sentinel), "SELECT data FROM accounts"))
        assert rows == []


def test_d1_all_skips_js_nullish_rows_and_keeps_real_rows():
    ns = _load()

    rows = asyncio.run(ns["d1_all"](_Env(results=[JsNull(), JsUndefined(), {"data": "ok"}]), "SELECT data FROM accounts"))

    assert rows == [{"data": "ok"}]


def test_d1_reads_bind_javascript_undefined_as_sql_null():
    ns = _load()
    env = _Env(row=None)

    asyncio.run(ns["d1_first"](env, "SELECT ?", JsUndefined()))

    assert env.DB.stmt.bound == (_JS_NULL,)


def test_d1_writes_bind_python_none_as_sql_null():
    ns = _load()
    env = _Env()

    asyncio.run(ns["d1_run"](env, "INSERT INTO t(v) VALUES (?)", None))

    assert env.DB.stmt.bound == (_JS_NULL,)


if __name__ == "__main__":
    for test in (
        test_js_nullish_detects_workers_js_nullish_sentinels,
        test_d1_first_treats_js_nullish_as_no_row,
        test_d1_first_keeps_real_rows,
        test_d1_all_treats_js_nullish_results_as_empty,
        test_d1_all_skips_js_nullish_rows_and_keeps_real_rows,
        test_d1_reads_bind_javascript_undefined_as_sql_null,
        test_d1_writes_bind_python_none_as_sql_null,
    ):
        test()
        print("PASS", test.__name__)
