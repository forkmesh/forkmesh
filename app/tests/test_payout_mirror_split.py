#!/usr/bin/env python3
"""Donation node-split eligibility tests (issue #94).

The donation reward split pays the "node half" only to online nodes that
actually mirror a repo for ANOTHER node. A node that hosts only its own
single repo (nobody else mirrors it, and it mirrors nobody) provides no
redundancy and must be left out of the split — its share would otherwise be
minted out of thin air against the network's stated purpose.

These tests load the relevant functions straight out of src/entry.py (no
Workers runtime) and drive _online_payout_addresses against an in-memory D1
mock so the filter is exercised end to end, not just asserted as a substring.
"""

import ast
import asyncio
import re
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
MIRRORS = ENTRY.parent / "mirrors.py"
CATALOG = ENTRY.parent / "catalog.py"
# Mirror grouping / clone-selection helpers were extracted from entry.py into
# mirrors.py, and the catalog-record sanitization helpers into catalog.py;
# parse all sources so the AST loaders below still find them.
_WORKER_SRC = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + MIRRORS.read_text(encoding="utf-8") + "\n"
    + CATALOG.read_text(encoding="utf-8"))
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")


def _load(*names, extra_globals=None):
    tree = ast.parse(_WORKER_SRC, filename=str(ENTRY))
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
    return [namespace[name] for name in names]


def _function_source(name):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(ENTRY_TEXT, node)


# --- mirroring_owner_set: the pure grouping decision ------------------------

def _owner_set(records):
    (fn,) = _load(
        "mirroring_owner_set",
        extra_globals={"repo_mirror_group_key": _load("repo_mirror_group_key")[0]},
    )
    return fn(records)


def test_single_owner_repo_does_not_qualify():
    # carol hosts only her own repo; no other node mirrors it -> not eligible.
    owners = _owner_set([{"owner": "carol", "name": "solo", "rootCommit": "R2"}])
    assert owners == set()


def test_repo_mirrored_across_two_owners_qualifies_both():
    owners = _owner_set([
        {"owner": "alice", "name": "forkmesh", "rootCommit": "R1"},
        {"owner": "bob", "name": "forkmesh", "rootCommit": "R1"},
    ])
    assert owners == {"alice", "bob"}


def test_groups_by_root_commit_not_just_name():
    # Same root, different local names -> still the same logical repo (mirrors).
    owners = _owner_set([
        {"owner": "alice", "name": "forkmesh", "rootCommit": "R1"},
        {"owner": "bob", "name": "forkmesh-fork", "rootCommit": "R1"},
        # A genuine fork (different root) shares the name but is NOT a mirror.
        {"owner": "eve", "name": "forkmesh", "rootCommit": "Rx"},
    ])
    assert owners == {"alice", "bob"}


def test_owner_with_one_mirrored_and_one_solo_repo_still_qualifies():
    owners = _owner_set([
        {"owner": "alice", "name": "shared", "rootCommit": "R1"},
        {"owner": "bob", "name": "shared", "rootCommit": "R1"},
        {"owner": "alice", "name": "private-toy", "rootCommit": "R9"},
    ])
    assert owners == {"alice", "bob"}


def test_owner_comparison_is_case_insensitive():
    owners = _owner_set([
        {"owner": "Alice", "name": "forkmesh", "rootCommit": "R1"},
        {"owner": "alice", "name": "forkmesh", "rootCommit": "R1"},
    ])
    # Same owner under two casings is one node, not a mirror across nodes.
    assert owners == set()


# --- _online_payout_addresses: the split actually drops solo nodes ----------

class _Clock:
    @staticmethod
    def now():
        return 1_000_000_000_000


def _payout_globals(repos, accounts, presence, balances=None):
    """Build the injected globals + an in-memory D1 mock.

    repos:    list of decrypted repository records ({owner,name,rootCommit}).
    accounts: name_bi -> account record (status/name/solana).
    presence: list of name_bi currently online (most-recent first).
    """
    calls = {"deleted": False}
    balances = balances or {}
    repos = [
        dict(record, visibility=record.get("visibility", "public"))
        for record in repos
    ]

    async def d1_run(_env, sql, *_args):
        if "DELETE FROM account_presence" in sql:
            calls["deleted"] = True
        return None

    async def d1_all(_env, sql, *_args):
        if "FROM repositories" in sql:
            return [{"data": r} for r in repos]
        if "FROM account_presence" in sql:
            return [{"name_bi": bi} for bi in presence]
        return []

    async def d1_first(_env, sql, *args):
        if "FROM nodes" in sql:
            rec = accounts.get(args[0])
            return {"name": rec.get("name"), "data": rec} if rec else None
        return None

    async def decrypt_row(_env, data):
        return data

    async def _solana_balance_lamports(_env, wallet):
        return balances.get(wallet, 1_000_000)

    extra = {
        "Date": _Clock,
        "ACCOUNT_PRESENCE_STALE_MS": 600_000,
        "MAX_NODE_NAME": 63,
        "SOLANA_RE": re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$"),
        "decrypt_row": decrypt_row,
        "d1_run": d1_run,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "_solana_balance_lamports": _solana_balance_lamports,
        "_is_blocked_catalog_identity": lambda _e, _o, _n: False,
        "_is_main_relay": lambda _e: False,
    }
    return extra, calls


# A valid-looking devnet/mainnet base58 address per node (44 chars).
_W = {
    "alice": "A1ice1111111111111111111111111111111111111x",
    "bob": "Bob2222222222222222222222222222222222222222x",
    "carol": "Caro3333333333333333333333333333333333333333",
}


def _run_addresses(repos, accounts, presence, balances=None):
    extra, calls = _payout_globals(repos, accounts, presence, balances=balances)
    # Load the whole call chain into one namespace so cross-references resolve.
    fns = _load(
        "_online_payout_addresses",
        "_mirroring_owners",
        "mirroring_owner_set",
        "repo_mirror_group_key",
        "clean_string",
        extra_globals=extra,
    )
    return asyncio.run(fns[0](object())), calls


def test_split_excludes_node_with_no_mirrors():
    repos = [
        {"owner": "alice", "name": "forkmesh", "rootCommit": "R1"},
        {"owner": "bob", "name": "forkmesh", "rootCommit": "R1"},
        {"owner": "carol", "name": "solo", "rootCommit": "R2"},
    ]
    accounts = {
        "bi_alice": {"status": "active", "name": "alice", "solana": _W["alice"]},
        "bi_bob": {"status": "active", "name": "bob", "solana": _W["bob"]},
        "bi_carol": {"status": "active", "name": "carol", "solana": _W["carol"]},
    }
    addresses, calls = _run_addresses(
        repos, accounts, ["bi_alice", "bi_bob", "bi_carol"])
    assert addresses == [_W["alice"], _W["bob"]]
    assert _W["carol"] not in addresses
    assert calls["deleted"] is True  # stale presence still self-heals


def test_split_is_empty_when_nothing_is_mirrored():
    # Every node hosts only its own repo -> the node half collapses to nobody and
    # the sweep routes the whole donation to the treasury.
    repos = [
        {"owner": "alice", "name": "a", "rootCommit": "Ra"},
        {"owner": "bob", "name": "b", "rootCommit": "Rb"},
    ]
    accounts = {
        "bi_alice": {"status": "active", "name": "alice", "solana": _W["alice"]},
        "bi_bob": {"status": "active", "name": "bob", "solana": _W["bob"]},
    }
    addresses, _ = _run_addresses(repos, accounts, ["bi_alice", "bi_bob"])
    assert addresses == []


def test_inactive_or_walletless_nodes_still_excluded_even_if_mirroring():
    repos = [
        {"owner": "alice", "name": "forkmesh", "rootCommit": "R1"},
        {"owner": "bob", "name": "forkmesh", "rootCommit": "R1"},
    ]
    accounts = {
        "bi_alice": {"status": "suspended", "name": "alice", "solana": _W["alice"]},
        "bi_bob": {"status": "active", "name": "bob", "solana": ""},
    }
    addresses, _ = _run_addresses(repos, accounts, ["bi_alice", "bi_bob"])
    assert addresses == []


def test_payout_address_does_not_need_a_deposit_to_be_eligible():
    repos = [
        {"owner": "alice", "name": "forkmesh", "rootCommit": "R1"},
        {"owner": "bob", "name": "forkmesh", "rootCommit": "R1"},
    ]
    accounts = {
        "bi_alice": {"status": "active", "name": "alice", "solana": _W["alice"]},
        "bi_bob": {"status": "active", "name": "bob", "solana": _W["bob"]},
    }
    addresses, _ = _run_addresses(
        repos,
        accounts,
        ["bi_alice", "bi_bob"],
        balances={_W["alice"]: 999_999, _W["bob"]: 1_000_000},
    )
    assert addresses == [_W["alice"], _W["bob"]]


# --- Source contract: display filtering remains; legacy sweep is gone --------

def test_worker_has_no_legacy_wallet_sweep():
    assert "def _sweep_confirmed_donation" not in ENTRY_TEXT
    assert "per_node = node_pool // len(payees)" not in ENTRY_TEXT
    assert "_solana_send_transfers" not in _function_source(
        "_distribute_central_fund")
    assert "INSERT INTO chain_intents" in _function_source(
        "_distribute_central_fund")


def test_payout_addresses_consults_mirroring_owners():
    assert "mirroring = await _mirroring_owners(env)" in ENTRY_TEXT
    assert "if mirroring is not None and owner.lower() not in mirroring:" in ENTRY_TEXT


def test_network_payout_display_marks_no_mirrors():
    # /network/ eligibility must agree with the real split.
    assert 'reason = "no_mirrors"' in ENTRY_TEXT
    assert 'reason = "wallet_unverified"' not in ENTRY_TEXT
    assert "MIN_ACTIVE_LAMPORTS" not in ENTRY_TEXT
    assert "Funding a payout address is" in ENTRY_TEXT
    assert "mirrors_repo = mirroring is None" in ENTRY_TEXT
