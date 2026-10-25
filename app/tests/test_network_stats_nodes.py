#!/usr/bin/env python3
"""network_stats headline "Nodes" count contract (adhoc #93).

The headline "Nodes" figure (stats.hosts) must equal the number of DISTINCT,
publicly known nodes that are online, the same set as stats.onlineNodes, so the
Network panel agrees with the Mirror nodes list on who is live. A node seen
through both its signed HTTPS endpoint and its account heartbeat counts once.
The retired host_presence table is never read.
"""

import ast
import asyncio
from pathlib import Path
from _maintenance_stubs import with_maintenance_off


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
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


NOW = 1_000_000_000_000
STALE_MS = 90_000


def _run(endpoint_nodes, account_nodes):
    """Exercise the real network_stats + _live_online_nodes over stubbed D1 rows.

    endpoint_nodes: (node_bi, name) of healthy signed HTTPS endpoints.
    account_nodes: (name_bi, name) of fresh account heartbeats.
    """
    queries = []

    async def d1_all(env, sql, *args):
        queries.append(sql)
        if "FROM mirror_https_endpoints" in sql:
            return [{"node_bi": key, "name": name}
                    for key, name in endpoint_nodes]
        if "FROM account_presence" in sql:
            return [{"name_bi": key, "name": name, "data": None}
                    for key, name in account_nodes]
        return []

    async def d1_first(env, sql, *args):
        queries.append(sql)
        return {}

    async def noop(*a, **k):
        return None

    captured = {}

    def json_response(payload, cache_seconds=None):
        captured.update(payload)
        return payload

    g = _load(
        "network_stats", "_live_online_nodes", "_short_presence_label",
        "_repositories_count", "_catalog_rev_current",
        extra_globals={
            "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
            "_REPOSITORIES_COUNT_MEMO": {"ts": 0, "count": None, "rev": None},
            "REPOSITORIES_COUNT_MEMO_MAX_AGE_MS": 600_000,
            "_CATALOG_REV_PROBE": {"ts": 0, "rev": None},
            "CATALOG_REV_PROBE_TTL_MS": 10_000,
            "ONLINE_SAMPLE_WINDOW_MS": STALE_MS,
            "MAX_NODE_NAME": 64,
            "LAMPORTS_PER_SOL": 1_000_000_000,
            "NETWORK_STATS_CACHE_KEY": "k",
            "NETWORK_STATS_TTL": 20,
            "ensure_schema": noop,
            "d1_run": noop,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "decrypt_row": (lambda env, data: _identity(data)),
            "_is_blocked_catalog_identity": (lambda env, o, n: False),
            "clean_string": (lambda s, n: str(s or "").strip()[:n]),
            "_flagship_client_count": (lambda env: _const(0)),
            "_amount_sol": (lambda v: "0"),
            "edge_cache_match": (lambda key: _const(None)),
            "edge_cache_put": noop,
            "json_response": json_response,
        },
    )
    asyncio.run(g["network_stats"](None))
    return captured, queries


async def _identity(data):
    return data


async def _const(value):
    return value


def test_hosts_counts_distinct_online_nodes_not_sources():
    out, _queries = _run(
        [("bi:newnewnode", "newnewnode"), ("bi:vm1", "vm1")],
        [("bi:newnewnode", "newnewnode")],
    )
    assert sorted(out["onlineNodes"]) == ["newnewnode", "vm1"]
    assert out["hosts"] == 2
    assert out["hosts"] == len(out["onlineNodes"])


def test_network_stats_never_reads_the_retired_host_presence_table():
    out, queries = _run([("bi:vm1", "vm1")], [])
    assert out["hosts"] == 1
    assert queries
    assert not any("host_presence" in sql for sql in queries)


if __name__ == "__main__":
    test_hosts_counts_distinct_online_nodes_not_sources()
    test_network_stats_never_reads_the_retired_host_presence_table()
    print("ok")
