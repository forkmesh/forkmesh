#!/usr/bin/env python3
"""The routed clone path buckets upload-packs per UTC day (migration 0174)."""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace
import time


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
NOW = 1_788_000_000_000  # 2026-08-29T10:40:00Z


def _namespace():
    wanted = {"_https_mirror_route_advance", "_utc_day_key"}
    selected = []
    for node in ast.parse(ENTRY.read_text(encoding="utf-8")).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and (
                node.name in wanted):
            selected.append(node)
    assert len(selected) == 2
    writes = []

    async def d1_run(_env, sql, *args):
        writes.append((" ".join(sql.split()), args))
        return SimpleNamespace()

    namespace = {
        "Date": SimpleNamespace(now=lambda: NOW),
        "d1_run": d1_run,
        "time": time,
        "CLONE_STICKY_MS": 60_000,
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    namespace["_writes"] = writes
    return namespace


def _context():
    return {"repoBi": "repo-bi", "owner": "Alice", "repo": "Repo"}


def test_utc_day_key_uses_the_dashboard_timezone():
    ns = _namespace()
    assert ns["_utc_day_key"](NOW) == "2026-08-29"
    midnight = 1_788_048_000_000  # 2026-08-30T00:00:00Z
    assert ns["_utc_day_key"](midnight - 1) == "2026-08-29"
    assert ns["_utc_day_key"](midnight) == "2026-08-30"
    assert ns["_utc_day_key"]("%d" % NOW) == "2026-08-29"


def test_upload_pack_bumps_the_day_bucket_after_the_serve_counter():
    ns = _namespace()
    asyncio.run(ns["_https_mirror_route_advance"](
        None, _context(), "Mirror7", "git-upload-pack"))
    sqls = [sql for sql, _args in ns["_writes"]]
    assert len(sqls) == 2
    assert "INSERT INTO mirror_serve_counters" in sqls[0]
    assert "INSERT INTO product_clone_daily(day,owner,repo,clones)" in sqls[1]
    assert "ON CONFLICT(day,owner,repo) DO UPDATE SET clones=product_clone_daily.clones+1" in sqls[1]
    assert ns["_writes"][1][1] == ("2026-08-29", "alice", "repo")


def test_website_reads_and_info_refs_do_not_touch_the_day_bucket():
    ns = _namespace()
    asyncio.run(ns["_https_mirror_route_advance"](
        None, _context(), "mirror7", "tree"))
    asyncio.run(ns["_https_mirror_route_advance"](
        None, _context(), "mirror7", "git-info-refs"))
    sqls = [sql for sql, _args in ns["_writes"]]
    assert not any("product_clone_daily" in sql for sql in sqls)
    assert any("website" in sql for sql in sqls)
    assert any("INSERT INTO clone_sticky" in sql for sql in sqls)


def test_a_failed_bucket_write_never_fails_the_served_clone():
    ns = _namespace()

    async def failing_run(_env, sql, *args):
        if "product_clone_daily" in sql:
            raise RuntimeError("D1 is busy")
        return SimpleNamespace()

    ns["d1_run"] = failing_run
    asyncio.run(ns["_https_mirror_route_advance"](
        None, _context(), "mirror7", "git-upload-pack"))
