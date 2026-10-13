#!/usr/bin/env python3
"""Per-source-IP account-creation throttle (signup_rate_check).

A single IP may create at most SIGNUP_MAX_PER_IP accounts per rolling window;
further attempts get a 429 until the window rolls over. A missing IP (local/dev
or a proxy path with no CF header) is never throttled. AST-extraction harness,
same style as test_repo_agents.py.
"""

import ast
import asyncio
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")

FUNCS = {"signup_rate_check"}


def _load(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [n for n in tree.body
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                and n.name in FUNCS]
    assert {n.name for n in selected} == FUNCS
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


def _harness(window_ms=60 * 60 * 1000, maximum=5):
    rows = {}          # ip_bi -> {count, window_start_ts}
    now = [1_000_000_000]

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def d1_first(_env, sql, *args):
        assert sql.startswith("INSERT INTO signup_rate")
        ip_bi = args[0]
        current = rows.get(ip_bi)
        if (
            not current
            or now[0] - current["window_start_ts"] >= window_ms
        ):
            current = {"count": 1, "window_start_ts": now[0]}
        else:
            current = {
                "count": current["count"] + 1,
                "window_start_ts": current["window_start_ts"],
            }
        rows[ip_bi] = current
        return dict(current)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO signup_rate"):
            ip_bi, count, window_start = args[0], args[1], args[2]
            rows[ip_bi] = {"count": count, "window_start_ts": window_start}
            return
        raise AssertionError("unexpected d1_run: " + sql)

    ns = _load({
        "Date": _DateStub,
        "json_response": json_response,
        "d1_first": d1_first,
        "SIGNUP_RATE_WINDOW_MS": window_ms,
        "SIGNUP_MAX_PER_IP": maximum,
    })
    ns["_now"] = now
    ns["_rows"] = rows
    return ns


def test_no_ip_is_never_throttled():
    ns = _harness()
    for _ in range(50):
        assert asyncio.run(ns["signup_rate_check"](object(), None)) is None
    assert asyncio.run(ns["signup_rate_check"](object(), "")) is None


def test_ip_allowed_up_to_limit_then_429():
    ns = _harness(maximum=5)
    env = object()
    for i in range(5):
        assert asyncio.run(ns["signup_rate_check"](env, "bi:1.2.3.4")) is None, i
    blocked = asyncio.run(ns["signup_rate_check"](env, "bi:1.2.3.4"))
    assert blocked["status"] == 429
    assert blocked["data"]["error"] == "rate_limited"
    assert blocked["data"]["retryAfterMs"] > 0
    # A different IP has its own independent budget.
    assert asyncio.run(ns["signup_rate_check"](env, "bi:9.9.9.9")) is None


def test_window_rollover_resets_the_count():
    ns = _harness(window_ms=1000, maximum=2)
    env = object()
    assert asyncio.run(ns["signup_rate_check"](env, "bi:x")) is None
    assert asyncio.run(ns["signup_rate_check"](env, "bi:x")) is None
    assert asyncio.run(ns["signup_rate_check"](env, "bi:x"))["status"] == 429
    ns["_now"][0] += 1000  # window elapses
    assert asyncio.run(ns["signup_rate_check"](env, "bi:x")) is None
