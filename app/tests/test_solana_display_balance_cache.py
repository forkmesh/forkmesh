#!/usr/bin/env python3
"""Hourly shared cache for *displayed* Solana balances (adhoc #80).

A public getBalance is a Worker subrequest against a keyless public RPC, and the
surfaces that merely show an amount re-render far more often than a balance
moves: the /network payout table priced every listed wallet on every 20s
rebuild, per colo, and the heartbeat probed once per wallet per warm isolate.
That is what made solana-rpc.publicnode.com the busiest thing the relay calls.

Every display read now goes through _display_balance_lamports, which serves
from the D1 wallet_balances table until the reading is an hour old. The manual
refresh buttons the UIs carry set refresh=1, which forces a live read against a
one-minute floor. Money-gating reads must keep bypassing the cache entirely.

These drive the real functions out of src/entry.py against an in-memory D1 mock,
so the throttle is exercised rather than asserted as a substring.
"""

import ast
import asyncio
import re
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
# world_wallet_badge_handler lives in world/src/world_routes.py (kept out of
# the Worker's startup source) but still reads through the same cache.
ENTRY_TEXT = (
    (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
    + "\n\n\n"
    + (ENTRY.parents[2] / "world" / "src" / "world_routes.py").read_text(
        encoding="utf-8")
)

WALLET = "A1ice1111111111111111111111111111111111111x"
OTHER = "Bob2222222222222222222222222222222222222222x"

HOUR_MS = 60 * 60 * 1000
MINUTE_MS = 60 * 1000


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
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
    namespace["__ns__"] = namespace
    return namespace


class _Clock:
    """Test clock: entry.py reads wall time through Date.now()."""

    now_ms = 1_000_000_000_000

    @classmethod
    def now(cls):
        return cls.now_ms


def _harness(rows=None, balances=None):
    """One namespace holding the cache helpers over an in-memory D1."""
    table = dict(rows or {})
    reads = []

    async def d1_first(_env, sql, *args):
        assert "FROM wallet_balances" in sql
        row = table.get(args[0])
        if not row:
            return None
        return {"lamports": row[0], "checked_at": row[1]}

    async def d1_all(_env, sql, *_args):
        assert "FROM wallet_balances" in sql
        return [
            {"wallet": wallet, "lamports": value[0], "checked_at": value[1]}
            for wallet, value in table.items()
        ]

    async def d1_run(_env, _sql, *args):
        # (wallet, name, lamports, checked_at) — the upsert's bind order.
        table[args[0]] = (int(args[2]), int(args[3]))

    async def _solana_balance_lamports(_env, address):
        reads.append(address)
        return (balances or {}).get(address, 7_000_000)

    namespace = _load(
        "_display_balance_lamports",
        "_stored_wallet_balances",
        "_store_wallet_balance",
        "_wants_balance_refresh",
        extra_globals={
            "Date": _Clock,
            "SOLANA_RE": re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$"),
            "SOLANA_DISPLAY_BALANCE_TTL_MS": HOUR_MS,
            "SOLANA_DISPLAY_BALANCE_FORCE_MIN_MS": MINUTE_MS,
            "SOLANA_DISPLAY_REFRESH_MAX_PER_REQUEST": 8,
            # Module-level state the AST loader does not carry over.
            "_BALANCE_REFRESH_CLAIMS": {},
            "d1_first": d1_first,
            "d1_all": d1_all,
            "d1_run": d1_run,
            "_solana_balance_lamports": _solana_balance_lamports,
            "parse_qs": __import__("urllib.parse", fromlist=["parse_qs"]).parse_qs,
            "urlparse": __import__(
                "urllib.parse", fromlist=["urlparse"]).urlparse,
        },
    )
    return namespace, table, reads


def _display(namespace, address, **kwargs):
    return asyncio.run(
        namespace["_display_balance_lamports"](object(), address, **kwargs))


# --- The hour-long window ----------------------------------------------------

def test_first_read_hits_the_rpc_and_is_stored():
    namespace, table, reads = _harness()
    lamports, checked_at = _display(namespace, WALLET)
    assert lamports == 7_000_000
    assert checked_at == _Clock.now_ms
    assert reads == [WALLET]
    # Stored, so every other colo and surface can answer from D1.
    assert table[WALLET] == (7_000_000, _Clock.now_ms)


def test_second_read_within_the_hour_costs_no_subrequest():
    namespace, _table, reads = _harness(
        rows={WALLET: (5_000_000, _Clock.now_ms - 59 * MINUTE_MS)})
    lamports, _ = _display(namespace, WALLET)
    assert lamports == 5_000_000
    assert reads == []  # this is the whole point


def test_read_past_the_hour_refreshes():
    stale_at = _Clock.now_ms - HOUR_MS - 1
    namespace, table, reads = _harness(rows={WALLET: (5_000_000, stale_at)})
    lamports, checked_at = _display(namespace, WALLET)
    assert reads == [WALLET]
    assert lamports == 7_000_000
    assert checked_at == _Clock.now_ms
    assert table[WALLET] == (7_000_000, _Clock.now_ms)


# --- The manual refresh buttons ---------------------------------------------

def test_refresh_button_forces_a_live_read_inside_the_hour():
    namespace, _table, reads = _harness(
        rows={WALLET: (5_000_000, _Clock.now_ms - 30 * MINUTE_MS)})
    lamports, _ = _display(namespace, WALLET, force=True)
    assert reads == [WALLET]
    assert lamports == 7_000_000


def test_held_down_refresh_button_still_floors_at_a_minute():
    namespace, _table, reads = _harness(
        rows={WALLET: (5_000_000, _Clock.now_ms - 30 * 1000)})
    lamports, _ = _display(namespace, WALLET, force=True)
    assert reads == []
    assert lamports == 5_000_000


class _Request:
    def __init__(self, url):
        self.url = url


def test_refresh_query_param_is_what_the_buttons_send():
    namespace, _table, _reads = _harness()
    wants = namespace["_wants_balance_refresh"]
    assert wants(_Request("https://forkmesh.com/api/rewards/pool?refresh=1"))
    assert wants(_Request("https://forkmesh.com/api/network/stats?payouts=1&refresh=1"))
    assert not wants(_Request("https://forkmesh.com/api/network/stats?payouts=1"))
    assert not wants(_Request("https://forkmesh.com/api/rewards/pool"))
    assert not wants(_Request("https://forkmesh.com/api/rewards/pool?refresh=0"))


def test_a_scoped_refresh_cannot_rebuild_the_aggregate_on_a_loop():
    # Honouring a press skips the edge-cached copy, so the whole /network/stats
    # aggregate is rebuilt behind it. A scripted button must fall back to the
    # cached response rather than re-run that every request.
    namespace, _table, _reads = _harness()
    wants = namespace["_wants_balance_refresh"]
    request = _Request("https://forkmesh.com/api/network/stats?payouts=1&refresh=1")
    assert wants(request, scope="network-stats")
    assert not wants(request, scope="network-stats")
    # A different surface keeps its own allowance.
    assert wants(request, scope="wallet-leaderboard")


def test_a_scoped_refresh_comes_back_after_the_floor():
    namespace, _table, _reads = _harness()
    wants = namespace["_wants_balance_refresh"]
    request = _Request("https://forkmesh.com/api/network/stats?refresh=1")
    assert wants(request, scope="network-stats")
    _Clock.now_ms += MINUTE_MS + 1
    try:
        assert wants(request, scope="network-stats")
    finally:
        _Clock.now_ms -= MINUTE_MS + 1


# --- Failure and bounding behaviour -----------------------------------------

def test_a_dead_rpc_keeps_the_last_reading_rather_than_publishing_zero():
    stale_at = _Clock.now_ms - 2 * HOUR_MS
    namespace, table, _reads = _harness(
        rows={WALLET: (5_000_000, stale_at)}, balances={WALLET: None})
    lamports, checked_at = _display(namespace, WALLET)
    assert lamports == 5_000_000
    assert checked_at == stale_at  # its age is reported honestly
    assert table[WALLET] == (5_000_000, stale_at)  # not overwritten


def test_an_address_never_read_reports_none_not_zero():
    namespace, _table, _reads = _harness(balances={WALLET: None})
    lamports, checked_at = _display(namespace, WALLET)
    # None lets a caller say "unavailable" instead of claiming an empty wallet.
    assert lamports is None
    assert checked_at == 0


def test_a_bad_address_never_reaches_the_rpc():
    namespace, _table, reads = _harness()
    assert _display(namespace, "not-an-address") == (None, 0)
    assert _display(namespace, "") == (None, 0)
    assert reads == []


def test_one_request_cannot_burst_more_reads_than_its_budget():
    # A list page pressing refresh must not turn into unbounded subrequests.
    wallets = [
        "W" + str(index) + "1111111111111111111111111111111111111"
        for index in range(6)
    ]
    namespace, _table, reads = _harness()
    budget = {"left": 2}
    stored = {}
    for wallet in wallets:
        _display(namespace, wallet, force=True, stored=stored, budget=budget)
    assert len(reads) == 2
    assert budget["left"] == 0


def test_a_shared_stored_map_prices_a_list_off_one_query():
    namespace, _table, reads = _harness()
    stored = {}
    first = _display(namespace, WALLET, stored=stored)
    # The same address twice in one render costs one read, not two.
    second = _display(namespace, WALLET, stored=stored)
    assert reads == [WALLET]
    assert first == second


# --- Source guarantees -------------------------------------------------------

def _body(name):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(ENTRY_TEXT, node)


def test_the_window_is_an_hour_and_the_force_floor_a_minute():
    assert "SOLANA_DISPLAY_BALANCE_TTL_MS = 60 * 60 * 1000" in ENTRY_TEXT
    assert "SOLANA_DISPLAY_BALANCE_FORCE_MIN_MS = 60 * 1000" in ENTRY_TEXT
    # The heartbeat's own per-isolate gate must not undercut the shared window.
    assert "HEARTBEAT_SOLANA_BALANCE_MIN_MS = 60 * 60 * 1000" in ENTRY_TEXT


def test_money_gating_reads_never_come_from_the_cache():
    # An hour-old number must never authorize a transfer, unlock a purchase,
    # release a bounty, or confirm a signup. Every one of these stays on the
    # live read — _body() raises if a name goes stale, so this cannot quietly
    # stop checking anything.
    for name in (
        "_account_donation_status",           # signup donation confirmation
        "_federation_donation_status",        # ditto, via a federated relay
        "_element_deposit_status",            # world-element purchase unlock
        "_sweep_element_deposit",             # moves the deposit on-chain
        "bounties_handler",                   # marks a bounty funded
        "_distribute_central_fund",           # signs reward transfers
        "_create_instant_distribution_intent",
        "_create_element_store_split_intent",
    ):
        body = _body(name)
        assert "_display_balance_lamports" not in body, name
        assert ("_solana_balance_lamports" in body
                or "_reward_balance_lamports" in body), name


def test_display_surfaces_all_read_through_the_cache():
    for name in (
        "_network_payout_nodes",      # /network payout wallet table
        "wallet_leaderboard",         # member SOL board
        "world_wallet_badge_handler", # avatar chest chip
        "_central_fund_public",       # community pool card
        "_account_heartbeat",         # desktop rail chip
    ):
        assert "_display_balance_lamports" in _body(name), name
