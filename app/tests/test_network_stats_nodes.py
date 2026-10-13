#!/usr/bin/env python3
"""network_stats headline "Nodes" count contract (adhoc #93).

The dashboard Network panel showed a "Nodes" figure (stats.hosts) that came from
a raw COUNT of host_presence rows, while the node *list* it sits above came from
stats.onlineNodes. Those disagreed: a raw row count double-counts a node hosting
several repos and inflates the total with ad-hoc/private tunnels that have no
public catalog record and are never named. The count must equal the number of
DISTINCT, publicly known nodes that are online — the same set as onlineNodes — so
the Network panel agrees with the Mirror nodes list on who is live.
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
STALE_MS = 90_000  # HOST_PRESENCE_STALE_MS stand-in for the test


def _run(host_rows):
    """Exercise the real network_stats + _live_online_nodes over stubbed D1 rows.

    host_rows: list of (repo_bi, owner_bi, is_private, owner_name) describing the
    fresh host_presence rows joined to repositories. owner_name=None models an
    ad-hoc tunnel with no public catalog record (LEFT JOIN misses -> owner_bi None,
    data None), which must not be counted or named.
    """
    async def d1_all(env, sql, *args):
        if "host_presence hp" in sql:
            rows = []
            for repo_bi, owner_bi, is_private, owner in host_rows:
                rows.append({
                    "repo_bi": repo_bi,
                    "owner_bi": owner_bi if owner is not None else None,
                    "is_private": is_private,
                    "data": {
                        "owner": owner,
                        "name": "repo",
                        "visibility": "private" if is_private else "public",
                    } if owner is not None else None,
                })
            return rows
        return []  # account_presence query -> none online via account presence

    async def d1_first(env, sql, *args):
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
            "HOST_PRESENCE_STALE_MS": STALE_MS,
            "ONLINE_SAMPLE_WINDOW_MS": STALE_MS,
            "MAX_NODE_NAME": 64,
            "LAMPORTS_PER_SOL": 1_000_000_000,
            "NETWORK_STATS_CACHE_KEY": "k",
            "NETWORK_STATS_TTL": 20,
            "ensure_schema": noop,
            "notify_stale_hosts_offline": noop,
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
    return captured


async def _identity(data):
    return data


async def _const(value):
    return value


def test_hosts_counts_distinct_online_nodes_not_tunnels():
    # newnewnode hosts two repos (two presence rows), plus one distinct mirror.
    out = _run([
        ("bi:newnewnode/a", "bi:newnewnode", 0, "newnewnode"),
        ("bi:newnewnode/b", "bi:newnewnode", 0, "newnewnode"),
        ("bi:vm1/a", "bi:vm1", 0, "vm1"),
    ])
    assert sorted(out["onlineNodes"]) == ["newnewnode", "vm1"]
    # Two distinct nodes -> 2, even though there are three host_presence rows.
    assert out["hosts"] == 2
    assert out["hosts"] == len(out["onlineNodes"])


def test_adhoc_and_private_tunnels_are_not_counted_or_named():
    # A public mirror, an ad-hoc tunnel (no catalog record), and a private repo.
    out = _run([
        ("bi:newnewnode/a", "bi:newnewnode", 0, "newnewnode"),
        ("bi:adhoc/x", None, 0, None),          # no public catalog record
        ("bi:secret/y", "bi:secret", 1, "secret"),  # private repo
    ])
    assert out["onlineNodes"] == ["newnewnode"]
    assert out["hosts"] == 1
    assert out["hosts"] == len(out["onlineNodes"])


if __name__ == "__main__":
    test_hosts_counts_distinct_online_nodes_not_tunnels()
    test_adhoc_and_private_tunnels_are_not_counted_or_named()
    print("ok")
