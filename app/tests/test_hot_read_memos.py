#!/usr/bin/env python3
"""Write-watched memos on the clone and signed-node hot paths (2026-10-23).

The desktop's rqlite view showed every clone advertisement scanning
mirror_https_endpoints and every signed node request reading users, nodes,
account_devices and nodes again. These tests pin the per-process memos that
replace those reads and the write generations that drop them.
"""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
TREE = ast.parse(ENTRY_TEXT, filename=str(ENTRY))


def _load(names, namespace):
    selected = []
    for node in TREE.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in names:
                selected.append(node)
        elif isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if targets & names:
                selected.append(node)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Clock:
    def __init__(self):
        self.ms = 1_790_000_000_000

    def now(self):
        return self.ms


def _snapshot_harness():
    clock = _Clock()
    reads = []
    rows = [{"node_name": "mirror1", "healthy": 1}]

    async def d1_all(_env, sql, *args):
        reads.append((sql, args))
        return [dict(row) for row in rows]

    ns = _load(
        {"D1_SNAPSHOT_TTL_MS", "D1_SNAPSHOT_MAX", "_D1_SNAPSHOTS",
         "_d1_snapshot"},
        {"Date": SimpleNamespace(now=clock.now), "d1_all": d1_all,
         "_D1_WATCH_GENERATIONS": {"mirror_https_endpoints": 0}})
    return ns, clock, reads, rows


def _snapshot(ns, sql="SELECT node_name FROM mirror_https_endpoints"):
    return asyncio.run(ns["_d1_snapshot"](
        object(), "mirror_https_endpoints", sql))


def test_the_endpoint_snapshot_serves_repeat_reads_inside_its_ttl():
    ns, clock, reads, _rows = _snapshot_harness()
    for _ in range(5):
        assert _snapshot(ns) == [{"node_name": "mirror1", "healthy": 1}]
    assert len(reads) == 1
    clock.ms += ns["D1_SNAPSHOT_TTL_MS"]
    _snapshot(ns)
    assert len(reads) == 2


def test_a_local_endpoint_write_drops_the_snapshot():
    ns, _clock, reads, rows = _snapshot_harness()
    _snapshot(ns)
    rows[0]["healthy"] = 0
    ns["_D1_WATCH_GENERATIONS"]["mirror_https_endpoints"] += 1
    assert _snapshot(ns) == [{"node_name": "mirror1", "healthy": 0}]
    assert len(reads) == 2


def test_callers_cannot_mutate_the_snapshot():
    ns, _clock, reads, _rows = _snapshot_harness()
    _snapshot(ns)[0]["healthy"] = 0
    assert _snapshot(ns)[0]["healthy"] == 1
    assert len(reads) == 1


def test_the_candidate_ring_reads_endpoints_through_the_snapshot():
    start = ENTRY_TEXT.index("async def _https_mirror_candidates(")
    body = ENTRY_TEXT[start:ENTRY_TEXT.index(
        "async def _https_mirror_request_candidates", start)]
    assert 'snapshot(env, "mirror_https_endpoints", endpoints_sql)' in body


def _signing_harness():
    clock = _Clock()
    reads = []
    devices = {"mirror1": ["owner-key"]}

    async def owner_signing_pubkeys(_env, node):
        reads.append(node)
        return list(devices.get(node, []))

    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_first(_env, sql, *args):
        reads.append(sql)
        return {"pubkey": ""}

    ns = _load(
        {"CLAIMED_SIGNING_KEYS_MEMO_MS", "CLAIMED_SIGNING_KEYS_MEMO_MAX",
         "_CLAIMED_SIGNING_KEYS_MEMO", "_claimed_node_signing_pubkeys"},
        {"Date": SimpleNamespace(now=clock.now),
         "clean_string": lambda value, size: str(value or "")[:size],
         "MAX_NODE_NAME": 64,
         "valid_node_name": lambda name: bool(name),
         "valid_node_pubkey": lambda key: bool(key),
         "_owner_signing_pubkeys": owner_signing_pubkeys,
         "blind_index": blind_index,
         "d1_first": d1_first,
         "_D1_WATCH_GENERATIONS": {"signing_keys": 0}})
    return ns, clock, reads, devices


def _keys(ns, node="mirror1"):
    return asyncio.run(ns["_claimed_node_signing_pubkeys"](object(), node))


def test_signed_node_requests_reuse_the_key_set_briefly():
    ns, clock, reads, _devices = _signing_harness()
    for _ in range(4):
        assert _keys(ns) == ["owner-key"]
    assert len(reads) == 2
    clock.ms += ns["CLAIMED_SIGNING_KEYS_MEMO_MS"]
    _keys(ns)
    assert len(reads) == 4


def test_a_key_revocation_in_this_process_applies_at_once():
    ns, _clock, _reads, devices = _signing_harness()
    assert _keys(ns) == ["owner-key"]
    devices["mirror1"] = []
    ns["_D1_WATCH_GENERATIONS"]["signing_keys"] += 1
    assert _keys(ns) == []


def test_a_caller_cannot_widen_the_remembered_key_set():
    ns, _clock, _reads, _devices = _signing_harness()
    _keys(ns).append("attacker-key")
    assert _keys(ns) == ["owner-key"]
