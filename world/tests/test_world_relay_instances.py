#!/usr/bin/env python3
"""Privacy-safe approved relay projection for the World map."""

import ast
import asyncio
import hashlib
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
SOURCE = (
    ENTRY.read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
NOW = 1_700_000_000_000


class Request:
    method = "GET"


def _load(rows):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    wanted = {"_world_relay_public_origin", "world_relay_instances_handler"}
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in wanted
    ]
    assert {node.name for node in nodes} == wanted

    async def d1_all(_env, sql, *args):
        assert "WHERE r.status='approved'" in sql
        assert "LEFT JOIN federated_presence" in sql
        assert args == (
            NOW - 10 * 60 * 1000,
            NOW - 10 * 60 * 1000,
        )
        return rows

    def response(data, status=200, cache_control=None, extra_headers=None):
        return {
            "data": data,
            "status": status,
            "cacheControl": cache_control,
            "headers": extra_headers or {},
        }

    namespace = {
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "urlparse": urlparse,
        "method_name": lambda request: request.method,
        "json_response": response,
        "ensure_schema": _none,
        "Date": SimpleNamespace(now=lambda: NOW),
        "WORLD_RELAY_HEALTH_FRESH_MS": 10 * 60 * 1000,
        "d1_all": d1_all,
        "hashlib": hashlib,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace


async def _none(*_args):
    return None


def run(value):
    return asyncio.run(value)


def test_only_approved_query_rows_with_public_https_origins_are_projected():
    module = _load([
        {
            "relay_bi": "approved-relay-blind-index",
            "label": "Community Relay",
            "base_url": "https://relay.example/world/private/path",
            "approved_at": NOW - 10_000,
            "last_seen": NOW - 1_000,
            "fresh_verified": 1,
            # A fake adapter returning extra columns must not make them public.
            "pubkey": "relay-public-key",
            "wallet": "wallet-address",
            "health_sig": "secret-signature-value",
            "token": "private-token",
        },
        {
            "relay_bi": "bad-origin",
            "label": "Bad",
            "base_url": "https://token@example.invalid/",
            "approved_at": NOW,
            "last_seen": NOW,
            "fresh_verified": 1,
        },
    ])
    response = run(module["world_relay_instances_handler"](
        object(), Request()))
    assert response["status"] == 200
    assert response["data"]["count"] == 1
    instance = response["data"]["instances"][0]
    assert instance == {
        "id": hashlib.sha256(
            b"forkmesh-world-relay-v1\0approved-relay-blind-index"
        ).hexdigest()[:24],
        "label": "Community Relay",
        "origin": "https://relay.example",
        "approved": True,
        "joinedAt": NOW - 10_000,
        "health": "online",
        "online": True,
        "healthEvidence": "approved-relay-signed-fresh-node-health",
    }
    serialized = repr(response["data"])
    for private in (
        "relay-public-key", "wallet-address", "secret-signature-value",
        "private-token",
        "approved-relay-blind-index",
    ):
        assert private not in serialized


def test_health_never_claims_online_without_fresh_verified_evidence():
    module = _load([
        {
            "relay_bi": "stale",
            "label": "Stale relay",
            "base_url": "https://stale.example",
            "approved_at": 1,
            "last_seen": NOW - 11 * 60 * 1000,
            "fresh_verified": 0,
        },
        {
            "relay_bi": "new",
            "label": "New relay",
            "base_url": "https://new.example",
            "approved_at": 2,
            "last_seen": 0,
            "fresh_verified": 0,
        },
    ])
    response = run(module["world_relay_instances_handler"](
        object(), Request()))
    assert [
        (item["health"], item["online"])
        for item in response["data"]["instances"]
    ] == [("offline", False), ("awaiting_verified_health", False)]


def test_world_instances_route_is_exact_and_tokens_are_not_accepted():
    assert '"/api/world/instances"' in SOURCE
    assert "world_relay_instances_handler(self.env, request)" in SOURCE
    handler = ast.get_source_segment(
        SOURCE,
        next(
            node for node in ast.parse(SOURCE).body
            if isinstance(node, ast.AsyncFunctionDef)
            and node.name == "world_relay_instances_handler"
        ),
    )
    assert "request.json" not in handler
    assert "authorization" not in handler.lower()
    assert "fp.wallet" not in handler
    assert "r.pubkey" not in handler
    assert "health_message" not in handler
    assert "health_sig" not in handler
