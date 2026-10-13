#!/usr/bin/env python3
"""Legacy bounty custody must remain frozen inside the Worker."""

import ast
import asyncio
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")


def _load(*names):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    namespace = {}
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=selected, type_ignores=[])), str(ENTRY), "exec"), namespace)
    return namespace


def test_legacy_payout_and_sweep_symbols_are_removed():
    for symbol in (
        "_bounty_auto_payout",
        "_bounty_mark_paid",
        "sweep_funded_bounties",
        "_save_bounty",
    ):
        assert f"def {symbol}" not in ENTRY_TEXT


def test_public_projection_labels_legacy_rows_as_migration_required():
    ns = _load("_bounty_public")
    ns.update({
        "_amount_sol": lambda value: str(value),
        "_solana_pay_uri": lambda address, amount, **_kwargs:
            f"solana:{address}?amount={amount}",
    })
    # Re-load after injecting dependencies into the execution namespace.
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.FunctionDef) and item.name == "_bounty_public"
    )
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[node], type_ignores=[])), str(ENTRY), "exec"), ns)
    public = ns["_bounty_public"]({
        "status": "funded",
        "address": "Escrow11111111111111111111111111111111111111",
        "secret": "legacy-seed",
    })
    assert public["status"] == "legacy_frozen"
    assert public["custody"] == "migration-required"
    assert public["featureState"] == "migration-only"
    assert public["uri"] == ""
    assert "secret" not in public
    assert "offline migration" in public["nonCustodialNotice"]


def test_mutating_bounty_actions_are_explicitly_disabled():
    start = ENTRY_TEXT.index("async def bounties_handler")
    body = ENTRY_TEXT[
        start:ENTRY_TEXT.index("\n\n# Cap on collaborators", start)
    ]
    assert 'if action in ("wallet", "create", "payout"):' in body
    assert '"legacy_custody_disabled"' in body
    assert '"external-self-custodial"' in body


def test_no_scheduled_path_calls_legacy_bounty_sweep():
    # The phase dispatch table only, cut at whatever top-level definition
    # follows it, so renaming that neighbour cannot silently widen the slice.
    start = ENTRY_TEXT.index("async def _run_cron_phase")
    ends = [
        offset for offset in (
            ENTRY_TEXT.find("\n\n\ndef ", start),
            ENTRY_TEXT.find("\n\n\nasync def ", start),
        ) if offset > 0
    ]
    scheduled = ENTRY_TEXT[start:min(ends)]
    assert "async def _run_cron_phase" in scheduled
    assert "sweep_funded_bounties" not in scheduled
    assert "_solana_sign_transfers" not in scheduled
    assert "_solana_send_transfers" not in scheduled
    assert "verify_submitted_chain_intents" in scheduled
