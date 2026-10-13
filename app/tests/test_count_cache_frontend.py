#!/usr/bin/env python3
"""Counts render instantly from a cached last-known value (issue #218).

The home and network pages used to show blank placeholders ("Loading",
"Checking…", "—") until the first network fetch returned. These contract tests
pin the stale-while-revalidate behavior: the last-known counts are persisted to
localStorage and rendered immediately on load, then refreshed by one live fetch.
"""

from pathlib import Path


APP_PUBLIC = Path(__file__).resolve().parents[1] / "public"
WWW_PUBLIC = APP_PUBLIC.parents[1] / "www" / "public"
STATIC = (WWW_PUBLIC / "static-page.js").read_text(encoding="utf-8")
MIRROR_PAYOUTS = (APP_PUBLIC / "mirror-payouts.js").read_text(encoding="utf-8")


def test_static_page_defines_and_primes_cached_stats():
    assert "function readCachedStat(" in STATIC
    assert "function writeCachedStat(" in STATIC
    assert "function primeCachedStats(" in STATIC
    # Priming happens before the first live fetch kicks off.
    assert STATIC.index("primeCachedStats();") < STATIC.rindex("pollStats();")
    assert "setInterval(" not in STATIC
    assert "function startStats(" not in STATIC


def test_static_page_persists_network_counts():
    poll = STATIC[STATIC.index("async function pollStats") : STATIC.index("function primeCachedStats")]
    assert 'writeCachedStat("clients", clients);' in poll
    assert 'writeCachedStat("hosts", hosts);' in poll
    assert 'writeCachedStat("repos", repos);' in poll


def test_static_page_skips_priming_in_file_preview():
    prime = STATIC[STATIC.index("function primeCachedStats") : STATIC.index("primeCachedStats();")]
    assert 'location.protocol === "file:"' in prime


def test_mirror_payouts_loads_network_and_pool_status_once_without_interval():
    assert "loadStatus();" in MIRROR_PAYOUTS
    assert 'api("/api/network/stats")' in MIRROR_PAYOUTS
    # The pool balance is served from the relay's hourly cache; only the Refresh
    # button asks it to re-read the chain, so a plain load stays the bare path.
    assert ('api(refresh ? "/api/rewards/pool?refresh=1" : "/api/rewards/pool")'
            in MIRROR_PAYOUTS)
    for selector in (
        "#calc-nodes",
        "#calc-reward",
        "#pool-network",
        "#pool-balance",
        "#pool-transfers",
    ):
        assert selector in MIRROR_PAYOUTS
    assert "setInterval(" not in MIRROR_PAYOUTS
