#!/usr/bin/env python3
"""Performance budget for the public repository catalog."""

import ast
import asyncio
import os
import re
import time
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, unquote, urlparse

import pytest


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
WORKER_SOURCE = "\n".join(
    path.read_text(encoding="utf-8")
    for path in (
        ENTRY.parent / "catalog_routes.py",
        ENTRY,
        ENTRY.parent / "mirrors.py",
        ENTRY.parent / "catalog.py",
    )
)
NOW = 1_700_000_000_000

pytestmark = pytest.mark.skipif(
    os.environ.get("FORKMESH_SKIP_PERF_TESTS") == "1",
    reason="performance budget tests disabled",
)


def _load(*names, extra_globals=None):
    tree = ast.parse(WORKER_SOURCE)
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == set(names)
    namespace = dict(extra_globals or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    return namespace


def _run_catalog_get(count):
    repositories = [{
        "owner": f"owner{index}",
        "name": f"repo{index}",
        "visibility": "public",
        "rootCommit": f"root{index}",
        "updatedAt": str(NOW - index),
    } for index in range(count)]

    async def d1_all(_env, sql, *_args):
        if "FROM repositories" in sql:
            return [
                {
                    "key_bi": f"bi{index}",
                    "owner_bi": f"owner-bi{index}",
                    "data": record,
                }
                for index, record in enumerate(repositories)
            ]
        return []

    async def noop(*_args, **_kwargs):
        return None

    async def identity(_env, value):
        return value

    async def none(*_args, **_kwargs):
        return None

    def json_response(payload, status=200, cache_control=None, **_kwargs):
        return {**payload, "_status": status, "_cacheControl": cache_control}

    namespace = _load(
        "catalog_handler", "method_name", "safe_segment", "clean_string",
        "served_mirror_groups", "repo_clone_online", "repo_mirror_group_key",
        "_catalog_logical_owners",
        extra_globals={
            "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
            "HOST_PRESENCE_STALE_MS": 600_000,
            "HTTPS_MIRROR_STATUS_FRESH_MS": 600_000,
            "MAX_NODE_NAME": 80,
            "MAX_REPO_SEGMENT": 80,
            "MAX_CATALOG_REPOS": 200,
            "CATALOG_TTL": 30,
            "CATALOG_CACHE_KEY": "catalog",
            "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._:-]+$"),
            "unquote": unquote,
            "ensure_schema": noop,
            "purge_blocked_catalog": noop,
            "verify_catalog_view_token": none,
            "edge_cache_match": none,
            "edge_cache_put": noop,
            "decrypt_row": identity,
            "_is_blocked_catalog_identity": lambda _env, _o, _n: False,
            "valid_node_name": lambda value: bool(str(value or "").strip()),
            "_admin_query": lambda value: "admin=" + value if value else "",
            "_ssh_gateway_settings": lambda _env: {
                "configured": False, "host": "", "port": 0,
                "repositories": {},
            },
            "d1_all": d1_all,
            "json_response": json_response,
            "urlparse": urlparse,
            "parse_qs": parse_qs,
        },
    )
    request = SimpleNamespace(
        method="GET", url="https://forkmesh.internal/api/repositories")
    started = time.perf_counter()
    result = asyncio.run(namespace["catalog_handler"](None, request))
    return result, time.perf_counter() - started


def test_catalog_get_is_linear_and_bounded_at_scale():
    result, elapsed = _run_catalog_get(3000)
    assert result["ok"] is True
    assert len(result["repositories"]) == 200
    assert elapsed < 0.5
