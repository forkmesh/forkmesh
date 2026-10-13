#!/usr/bin/env python3
"""The "Top tip recipients" board, exercised rather than pattern-matched.

The board folds the public ledger's ``member-tip`` receipts per recipient, so
the things that can quietly go wrong are all behaviour rather than text: a
devnet lamport outranking every real tip, a member who has since gone private
still being ranked, or the profile gate silently shortening a top ten because
ranking happens in SQL before it. Each of those gets a case here.

AST-extraction harness in the style of test_leaderboards_partial_failure.py.
"""

import ast
import asyncio
import sqlite3
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")

FUNCS = {
    "tip_recipient_leaderboard",
    "_is_public_roster_member",
    "_account_kind",
}
CONSTANTS = {
    "TIP_RECIPIENT_LEADERBOARD_CACHE_KEY",
    "TIP_RECIPIENT_LEADERBOARD_TTL",
    "TIP_RECIPIENT_LEADERBOARD_LIMIT",
    "TIP_RECIPIENT_SCAN_LIMIT",
    "LAMPORTS_PER_SOL",
    "MAX_NODE_NAME",
}


def _selected():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    nodes = []
    found = set()
    for node in tree.body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in FUNCS):
            nodes.append(node)
            found.add(node.name)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in CONSTANTS
            for target in node.targets
        ):
            nodes.append(node)
            found.update(
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            )
    missing = (FUNCS | CONSTANTS) - found
    assert not missing, "missing: %s" % sorted(missing)
    return nodes


def _load(namespace):
    module = ast.fix_missing_locations(
        ast.Module(body=_selected(), type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _member(name, **overrides):
    record = {"name": name, "kind": "user", "status": "active"}
    record.update(overrides)
    return record


def _tip(name, lamports, tips=1, senders=1, last_ts=1_700_000_000_000,
         **overrides):
    """One grouped SQL row: the recipient's encrypted blob plus its totals."""
    return {
        "data": name,
        "lamports": lamports,
        "tips": tips,
        "senders": senders,
        "last_ts": last_ts,
        **overrides,
    }


def _run(grouped_rows, records=None):
    """Call the board against a fake D1 and return (payload, queries, cache)."""
    queries = []
    cache = {}
    records = records if records is not None else {}

    async def d1_all(_env, sql, *binds):
        queries.append((sql, binds))
        return list(grouped_rows)

    async def decrypt_row(_env, blob):
        # The fake blob is the account name; the fixture owns its record.
        return records.get(blob, _member(str(blob)))

    async def edge_cache_put(key, response):
        cache[key] = response

    captured = {}

    def json_response(data, cache_seconds=None, **_kwargs):
        captured["data"] = data
        captured["cache_seconds"] = cache_seconds
        return captured

    namespace = {
        "clean_string": lambda value, limit=240: str(value or "")[:limit],
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "edge_cache_match": _noop,
        "edge_cache_put": edge_cache_put,
        "ensure_schema": _noop,
        "json_response": json_response,
    }
    board = _load(namespace)["tip_recipient_leaderboard"]
    return asyncio.run(board(object())), queries, cache


async def _noop(*_args, **_kwargs):
    return None


def test_the_board_ranks_the_ledgers_tip_receipts_per_recipient():
    payload, _queries, _cache = _run([
        _tip("ada", 3_000_000_000, tips=4, senders=3),
        _tip("grace", 500_000_000, tips=1, senders=1),
    ])
    board = payload["data"]["board"]
    assert [row["name"] for row in board] == ["ada", "grace"]
    assert board[0]["lamports"] == 3_000_000_000
    assert board[0]["sol"] == 3.0
    # How the total was reached travels with it, so the hub can say "4 tips ·
    # 3 senders" beside the amount instead of one unexplained figure.
    assert board[0]["tips"] == 4
    assert board[0]["senders"] == 3
    assert board[0]["lastTs"] == 1_700_000_000_000
    assert payload["data"]["recipients"] == 2
    assert payload["data"]["ok"] is True


def test_only_mainnet_receipts_can_enter_the_ranking():
    # Devnet and testnet lamports are free to mint, so a single faucet airdrop
    # would otherwise sit above every real tip on the board forever.
    _payload, queries, _cache = _run([])
    sql = queries[0][0]
    assert "t.network='mainnet'" in sql
    assert "tip_notice" in sql


def test_contributor_rewards_are_not_counted_as_member_tips():
    # These notices exist so the recipient gets an email, but the public
    # ledger represents the paid reward as a contributor reward rather than a
    # member-to-member tip. Run the actual aggregate query against SQLite so
    # this verifies the ledger's canonical-record exclusion, not just its SQL
    # spelling.
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript("""
        CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE tip_notice (
          signature TEXT PRIMARY KEY, from_bi TEXT NOT NULL, to_bi TEXT NOT NULL,
          lamports INTEGER NOT NULL, network TEXT NOT NULL, ts INTEGER NOT NULL
        );
        CREATE TABLE contributor_reward_estimates (
          tx_signature TEXT, status TEXT NOT NULL
        );
        INSERT INTO users VALUES ('ada-bi', 'ada'), ('grace-bi', 'grace');
        INSERT INTO tip_notice VALUES
          ('plain-ada', 'sender-a', 'ada-bi', 2000000000, 'mainnet', 10),
          ('reward-ada', 'sender-b', 'ada-bi', 8000000000, 'mainnet', 11),
          ('cancelled-ada', 'sender-c', 'ada-bi', 4000000000, 'mainnet', 12),
          ('devnet-ada', 'sender-d', 'ada-bi', 9000000000, 'devnet', 13),
          ('plain-grace', 'sender-e', 'grace-bi', 5000000000, 'mainnet', 14);
        INSERT INTO contributor_reward_estimates VALUES
          ('reward-ada', 'paid'), ('cancelled-ada', 'cancelled');
    """)

    async def d1_all(_env, sql, *binds):
        return [dict(row) for row in database.execute(sql, binds)]

    async def decrypt_row(_env, blob):
        return _member(str(blob))

    namespace = {
        "clean_string": lambda value, limit=240: str(value or "")[:limit],
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "edge_cache_match": _noop,
        "edge_cache_put": _noop,
        "ensure_schema": _noop,
        "json_response": lambda data, **_kwargs: data,
    }
    board = _load(namespace)["tip_recipient_leaderboard"]
    payload = asyncio.run(board(object()))
    assert [
        (row["name"], row["lamports"], row["tips"], row["senders"])
        for row in payload["board"]
    ] == [
        ("ada", 6_000_000_000, 2, 2),
        ("grace", 5_000_000_000, 1, 1),
    ]


def test_a_recipient_who_has_gone_private_or_closed_is_not_ranked():
    payload, _queries, _cache = _run(
        [
            _tip("ada", 9_000_000_000),
            _tip("mallory", 8_000_000_000),
            _tip("closed", 7_000_000_000),
            _tip("relay-node", 6_000_000_000),
            _tip("grace", 1_000_000_000),
        ],
        records={
            "ada": _member("ada"),
            "mallory": _member("mallory", profile_private=True),
            "closed": _member("closed", status="closed"),
            "relay-node": _member("relay-node", kind="node"),
            "grace": _member("grace"),
        },
    )
    assert [row["name"] for row in payload["data"]["board"]] == ["ada", "grace"]
    assert payload["data"]["recipients"] == 2


def test_the_scan_reaches_past_the_board_depth():
    # SQL ranks before the profile gate can drop anyone, so a board that only
    # scanned its own depth would publish a short top ten whenever one of the
    # leaders had gone private.
    namespace = _load({
        "clean_string": lambda value, limit=240: str(value or "")[:limit],
        "d1_all": _noop,
        "decrypt_row": _noop,
        "edge_cache_match": _noop,
        "edge_cache_put": _noop,
        "ensure_schema": _noop,
        "json_response": lambda *a, **k: {},
    })
    assert (namespace["TIP_RECIPIENT_SCAN_LIMIT"]
            > namespace["TIP_RECIPIENT_LEADERBOARD_LIMIT"])
    _payload, queries, _cache = _run([])
    assert queries[0][1] == (namespace["TIP_RECIPIENT_SCAN_LIMIT"],)


def test_the_board_is_trimmed_to_a_top_ten_and_edge_cached():
    rows = [_tip("member%02d" % index, (40 - index) * 1_000_000_000)
            for index in range(30)]
    payload, _queries, cache = _run(rows)
    assert len(payload["data"]["board"]) == 10
    # The tail is still counted, so ten rows read as a top ten rather than as
    # everyone who has ever been tipped.
    assert payload["data"]["recipients"] == 30
    assert payload["cache_seconds"] == 300
    assert list(cache) == [
        "https://forkmesh.internal/api/leaderboards/tip-recipients"]


def test_the_board_never_presents_a_reported_amount_as_a_verified_one():
    payload, _queries, _cache = _run([_tip("ada", 1_000_000_000)])
    notice = payload["data"]["notice"]
    assert "reported" in notice
    assert "authoritative" in notice
    # ForkMesh is not a party to the transfer and the board must not read as
    # though a ranking were a payout.
    assert "not a payout" in notice
    assert payload["data"]["custody"] == "none-peer-to-peer-self-custodial"
