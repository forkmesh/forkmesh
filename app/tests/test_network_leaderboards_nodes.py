#!/usr/bin/env python3
"""network_leaderboards per-node detail card (adhoc #213).

The website's Network page "Connected nodes" list only showed a name and an
online-minutes figure. The desktop client's Mirror nodes panel already shows
per-node commit/synced/size/issues/commits/branches/pulls/discussions/
platform/version/clones/website/artifacts (adhoc #56's catalog record
fields) -- this closes that gap for the website by having
network_leaderboards() sum those counters across every repo a node mirrors
and surface the most-recently-updated repo's commit/branch/platform/version/
sync time as the node's "latest" state.
"""

import ast
import asyncio
import json
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


NOW = 1_000_000_000_000


async def _identity(data):
    return data


async def _const(value):
    return value


def _run(repo_records):
    repo_records = [
        dict(record, visibility=record.get("visibility", "public"))
        for record in repo_records
    ]

    async def d1_all(env, sql, *args):
        if "repositories" in sql:
            # Ciphertext stands in as JSON text; the catalog memo decrypts it.
            return [{"key_bi": f"bi:{i}", "data": json.dumps(rec)}
                    for i, rec in enumerate(repo_records)]
        return []

    async def d1_first(env, sql, *args):
        return {}

    async def noop(*a, **k):
        return None

    captured = {}

    def json_response(payload, cache_seconds=None):
        captured.update(payload)
        return payload

    class FakeDate:
        @staticmethod
        def now():
            return NOW

        @staticmethod
        def parse(s):
            # Model updatedAt as sortable numeric strings ("1", "2", ...); real
            # values are ISO timestamps, but only relative ordering matters here.
            try:
                return float(s)
            except (TypeError, ValueError):
                return float("nan")

    g = _load(
        "network_leaderboards", "_catalog_updated_ms",
        "_online_hourly_node_rows", "_decrypted_public_catalog",
        "_catalog_rev_current",
        extra_globals={
            "Date": FakeDate,
            "_ONLINE_HOURLY_NODES_MEMO": {"ts": 0, "start": 0, "rows": None},
            "ONLINE_HOURLY_NODES_MEMO_TTL_MS": 120_000,
            "_PUBLIC_CATALOG_MEMO": {
                "ts": 0, "rows": None, "refresh_ts": 0, "rev": None,
                "scan_ts": 0},
            "PUBLIC_CATALOG_MEMO_TTL_MS": 30_000,
            "PUBLIC_CATALOG_MEMO_MAX_AGE_MS": 600_000,
            "_CATALOG_ROW_DECRYPT_MEMO": {},
            "CATALOG_ROW_DECRYPT_MEMO_MAX": 4096,
            "_CATALOG_REV_PROBE": {"ts": 0, "rev": None},
            "CATALOG_REV_PROBE_TTL_MS": 10_000,
            "NETWORK_LEADERBOARDS_TTL": 300,
            "MAX_NODE_NAME": 64,
            "ONLINE_HISTORY_RETAIN_MS": 1000,
            "LEADERBOARD_LIMIT": 10,
            "NETWORK_LEADERBOARDS_CACHE_KEY": "k",
            "NETWORK_STATS_TTL": 20,
            "ensure_schema": noop,
            "d1_run": noop,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "decrypt_row": (lambda env, data: _identity(json.loads(data))),
            "_is_blocked_catalog_identity": (lambda env, o, n: False),
            "_short_presence_label": (lambda kind, key: key),
            "clean_string": (lambda s, n: str(s or "").strip()[:n]),
            "_funds_received_boards": (lambda env: _const(
                {"mainnode": [], "contributor": [], "project": []})),
            "edge_cache_match": (lambda key: _const(None)),
            "edge_cache_put": noop,
            "json_response": json_response,
        },
    )
    asyncio.run(g["network_leaderboards"](None))
    return captured


def test_sums_counters_across_a_nodes_repos_and_keeps_latest_state():
    out = _run([
        {
            "owner": "newnewnode", "name": "repoA", "sizeBytes": 100,
            "issueCount": 3, "commitCount": 10, "branchCount": 2,
            "pullCount": 1, "discussionCount": 0, "artifactCount": 0,
            "clonesServed": 5, "websiteServed": 7,
            "commit": "aaaaaaaaaaaa", "branch": "main", "lastSync": "1",
            "platform": "linux", "version": "0.5.1", "updatedAt": "1",
        },
        {
            "owner": "newnewnode", "name": "repoB", "sizeBytes": 50,
            "issueCount": 1, "commitCount": 4, "branchCount": 1,
            "pullCount": 0, "discussionCount": 2, "artifactCount": 1,
            "clonesServed": 0, "websiteServed": 3,
            "commit": "bbbbbbbbbbbb", "branch": "dev", "lastSync": "2",
            "platform": "linux", "version": "0.5.2", "updatedAt": "2",
        },
        {
            "owner": "vm1", "name": "repoC", "sizeBytes": 20,
            "issueCount": 0, "commitCount": 1, "branchCount": 1,
            "pullCount": 0, "discussionCount": 0, "artifactCount": 0,
            "clonesServed": 0, "websiteServed": 0,
            "commit": "cccccccccccc", "branch": "main", "lastSync": "1",
            "platform": "windows", "version": "0.5.1", "updatedAt": "1",
        },
    ])
    nodes = {n["name"]: n for n in out["nodes"]}
    assert set(nodes) == {"newnewnode", "vm1"}

    newnewnode = nodes["newnewnode"]
    assert newnewnode["repositoryCount"] == 2
    assert newnewnode["sizeBytes"] == 150
    assert newnewnode["issueCount"] == 4
    assert newnewnode["commitCount"] == 14
    assert newnewnode["branchCount"] == 3
    assert newnewnode["pullCount"] == 1
    assert newnewnode["discussionCount"] == 2
    assert newnewnode["artifactCount"] == 1
    assert newnewnode["clonesServed"] == 5
    assert newnewnode["websiteServed"] == 10
    # repoB has the later updatedAt ("2" > "1"), so its state wins.
    assert newnewnode["commit"] == "bbbbbbbbbbbb"
    assert newnewnode["branch"] == "dev"
    assert newnewnode["version"] == "0.5.2"
    assert "_updatedMs" not in newnewnode

    vm1 = nodes["vm1"]
    assert vm1["repositoryCount"] == 1
    assert vm1["sizeBytes"] == 20
    assert vm1["platform"] == "windows"


def test_missing_counters_remain_unknown_while_reported_zero_stays_zero():
    out = _run([
        {
            "owner": "mirror2", "name": "forkmesh", "sizeBytes": 100,
            "issueCount": "0", "commitCount": "",
            "pullCount": None, "clonesServed": -1,
            "commit": "a" * 40, "branch": "main", "updatedAt": "1",
        },
    ])
    node = out["nodes"][0]
    assert node["issueCount"] == 0
    assert node["commitCount"] is None
    assert node["pullCount"] is None
    assert node["clonesServed"] is None
    assert node["websiteServed"] is None


def test_partial_multi_repo_counter_totals_remain_unknown():
    out = _run([
        {
            "owner": "mirror2", "name": "forkmesh", "sizeBytes": 100,
            "issueCount": "0", "commitCount": "10",
            "pullCount": "2", "updatedAt": "1",
        },
        {
            "owner": "mirror2", "name": "another-repo", "sizeBytes": 50,
            "issueCount": "", "commitCount": "5",
            "pullCount": None, "updatedAt": "2",
        },
    ])
    node = out["nodes"][0]
    # A partial sum is not a truthful node total. Both repositories reported
    # commitCount, so that one aggregate remains known.
    assert node["issueCount"] is None
    assert node["pullCount"] is None
    assert node["commitCount"] == 15


if __name__ == "__main__":
    test_sums_counters_across_a_nodes_repos_and_keeps_latest_state()
    test_missing_counters_remain_unknown_while_reported_zero_stays_zero()
    test_partial_multi_repo_counter_totals_remain_unknown()
    print("ok")


def test_daemon_epoch_ms_updated_at_picks_the_latest_state():
    out = _run([
        {"owner": "fmnext-n1", "name": "old", "version": "next",
         "platform": "linux", "updatedAt": 1_790_000_000_000},
        {"owner": "fmnext-n1", "name": "new", "version": "1.2.4",
         "platform": "linux", "updatedAt": 1_790_000_000_500},
        {"owner": "fmnext-n2", "name": "text", "version": "1.2.4",
         "updatedAt": "1790000000900"},
        {"owner": "fmnext-n3", "name": "flag", "version": "1.2.4",
         "updatedAt": True},
    ])
    nodes = {node["name"]: node for node in out["nodes"]}
    assert nodes["fmnext-n1"]["version"] == "1.2.4"
    assert nodes["fmnext-n1"]["platform"] == "linux"
    assert nodes["fmnext-n2"]["version"] == "1.2.4"
    assert nodes["fmnext-n3"]["version"] == ""


def test_catalog_updated_ms_with_the_host_date():
    import sys
    sys.path.insert(0, str(ENTRY.parents[1] / "host"))
    from forkmesh_host.js import Date as HostDate
    g = _load("_catalog_updated_ms", extra_globals={"Date": HostDate})
    updated = g["_catalog_updated_ms"]
    assert updated({"updatedAt": 1_790_280_890_669}) == 1_790_280_890_669
    assert updated({"updatedAt": "1790280890669"}) == 1_790_280_890_669
    assert updated({"updatedAt": "2026-09-24T20:00:00Z"}) == 1_790_280_000_000
    assert updated({"updatedAt": True}) == -1
    assert updated({"updatedAt": ""}) == -1
