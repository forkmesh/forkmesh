"""Executable contracts for direct contributions and all-node intents."""

import ast
import asyncio
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
)
SCHEMA_TEXT = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")

POOL = "11111111111111111111111111111111"
REFERENCE = "SysvarRent111111111111111111111111111111111"
SOURCE = "SysvarC1ock11111111111111111111111111111111"
WALLET_A = "Vote111111111111111111111111111111111111111"
WALLET_B = "Stake11111111111111111111111111111111111111"
SIGNATURE = "2" * 88
SOLANA_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")


def load_function(name, globals_):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    namespace = dict(globals_)
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[node], type_ignores=[])), str(ENTRY), "exec"), namespace)
    return namespace[name]


def transaction(*, reference=REFERENCE, destination=POOL, lamports=50_000):
    return {
        "result": {
            "meta": {"err": None},
            "transaction": {
                "signatures": [SIGNATURE],
                "message": {
                    "accountKeys": [
                        {"pubkey": SOURCE},
                        {"pubkey": destination},
                        {"pubkey": reference},
                    ],
                    "instructions": [{
                        "program": "system",
                        "parsed": {
                            "type": "transfer",
                            "info": {
                                "source": SOURCE,
                                "destination": destination,
                                "lamports": lamports,
                            },
                        },
                    }],
                },
            },
        },
    }


def test_contribution_requires_exact_finalized_reference_bound_transfer():
    reply = transaction()

    async def rpc(_env, method, params):
        assert method == "getTransaction"
        assert params[0] == SIGNATURE
        assert params[1]["commitment"] == "finalized"
        return reply

    verifier = load_function("_solana_contribution_details", {
        "re": re,
        "SOLANA_RE": SOLANA_RE,
        "_reward_solana_rpc": rpc,
    })
    result = asyncio.run(verifier(
        object(), SIGNATURE,
        pool_address=POOL,
        reference_address=REFERENCE,
        amount_lamports=50_000,
    ))
    assert result == {
        "sourceAddress": SOURCE,
        "destinationAddress": POOL,
        "lamports": 50_000,
    }

    reply["result"]["transaction"]["message"]["accountKeys"][2] = {
        "pubkey": WALLET_A}
    assert asyncio.run(verifier(
        object(), SIGNATURE,
        pool_address=POOL,
        reference_address=REFERENCE,
        amount_lamports=50_000,
    )) is False


def test_unfinalized_contribution_never_triggers_distribution():
    async def rpc(_env, _method, _params):
        return {"result": None}

    verifier = load_function("_solana_contribution_details", {
        "re": re,
        "SOLANA_RE": SOLANA_RE,
        "_reward_solana_rpc": rpc,
    })
    assert asyncio.run(verifier(
        object(), SIGNATURE,
        pool_address=POOL,
        reference_address=REFERENCE,
        amount_lamports=50_000,
    )) is None


def test_instant_mode_creates_unsigned_equal_all_eligible_intent():
    writes = []
    audits = []

    async def fund(_env):
        return {
            "address": POOL,
            "signer_account": "instance-owner",
            "network": "devnet",
        }

    async def snapshot(_env, _now):
        return {
            "eligible": [
                {"nodeId": "a", "walletAddress": WALLET_A},
                {"nodeId": "b", "walletAddress": WALLET_B},
            ],
            "snapshotHash": "abc",
        }

    async def write(_env, sql, *args):
        writes.append((sql, args))

    async def batch(_env, statements):
        writes.extend(statements)

    async def audit(_env, *args):
        audits.append(args)

    creator = load_function("_create_instant_distribution_intent", {
        "Date": type("Date", (), {"now": staticmethod(
            lambda: 2_000_000_000_000)}),
        "REWARD_POOL_FEE_RESERVE_LAMPORTS": 5_000,
        "REWARD_TRANSFERS_PER_INTENT": 8,
        "_central_fund_record": fund,
        "_eligible_reward_snapshot": snapshot,
        "_reward_balance_lamports": lambda _env, _address: asyncio.sleep(
            0, result=1_005_000),
        "_random_bytes": lambda _size: bytes.fromhex(
            "0123456789abcdef0123456789abcdef"),
        "_audit_sensitive_action": audit,
        "_contribution_run_batch": batch,
        "d1_all": lambda *_args: asyncio.sleep(0, result=[]),
        "json": json,
    })
    intent = asyncio.run(creator(object(), {
        "contribution_id": "c" * 32,
        "mode": "instant_all_nodes",
        "amount_lamports": 1_005_000,
        "distribution_intent_id": "",
    }))
    assert intent == ["0123456789abcdef0123456789abcdef"]
    insert = next(item for item in writes if "INSERT INTO chain_intents" in item[0])
    assert insert[1][1] == "community_reward_instant_distribution"
    plan = json.loads(insert[1][4])
    assert plan["transfers"] == [
        {"address": WALLET_A, "lamports": 500_000, "nodeId": "a"},
        {"address": WALLET_B, "lamports": 500_000, "nodeId": "b"},
    ]
    assert plan["retainedForNetworkFeesAndRemainder"] == 5_000
    assert plan["batch"] == {
        "chunkIndex": 0,
        "chunkCount": 1,
        "maximumTransfersPerTransaction": 8,
    }
    assert audits
    assert "private" not in json.dumps(plan).lower()


def test_maximum_eligible_distribution_uses_three_bounded_d1_statements():
    batches = []
    ids = iter(range(1, 20))

    async def snapshot(_env, _now):
        return {
            "eligible": [
                {"nodeId": f"mirror-{index}", "walletAddress": WALLET_A}
                for index in range(100)
            ],
            "snapshotHash": "max-roster",
        }

    async def batch(_env, statements):
        batches.append(statements)

    creator = load_function("_create_instant_distribution_intent", {
        "Date": type("Date", (), {"now": staticmethod(
            lambda: 2_000_000_000_000)}),
        "REWARD_POOL_FEE_RESERVE_LAMPORTS": 5_000,
        "REWARD_TRANSFERS_PER_INTENT": 8,
        "_central_fund_record": lambda _env: asyncio.sleep(0, result={
            "address": POOL,
            "signer_account": "instance-owner",
            "network": "devnet",
        }),
        "_eligible_reward_snapshot": snapshot,
        "_reward_balance_lamports": lambda _env, _address: asyncio.sleep(
            0, result=2_000_000),
        "_random_bytes": lambda _size: next(ids).to_bytes(16, "big"),
        "_audit_sensitive_action": lambda *_args: asyncio.sleep(0),
        "_contribution_run_batch": batch,
        "d1_all": lambda *_args: asyncio.sleep(0, result=[]),
        "json": json,
    })
    intent_ids = asyncio.run(creator(object(), {
        "contribution_id": "d" * 32,
        "mode": "instant_all_nodes",
        "amount_lamports": 1_065_000,
        "distribution_intent_id": "",
    }))
    assert len(intent_ids) == 13
    assert len(batches) == 1
    statements = batches[0]
    assert len(statements) == 3
    assert len(statements[0][1]) == 13 * 7
    assert len(statements[1][1]) == 13 * 4
    assert len(statements[2][1]) == 2
    assert all(len(args) <= 100 for _sql, args in statements)


def test_routes_schema_and_public_states_cover_the_full_reward_flow():
    for route in (
        "/api/rewards/pool",
        "/api/rewards/contributions",
        "/api/rewards/pending",
        "/api/rewards/pending-awards",
        "/api/rewards/signing-jobs",
    ):
        assert route in ENTRY_TEXT
    for table in (
        "reward_node_observations",
        "reward_rounds",
        "reward_contributions",
        "pending_rewards",
        "chain_intents",
    ):
        assert f"CREATE TABLE IF NOT EXISTS {table}" in SCHEMA_TEXT
    for phrase in (
        '"userOwnedFunds"',
        '"communityFundedPool"',
        '"pendingRewards"',
        '"completedOnChainTransfers"',
        '"privateKeyStoredByWorker": False',
    ):
        assert phrase in ENTRY_TEXT
    assert "instant-all-eligible-mirrors-v1" in ENTRY_TEXT
    assert "randomized-eligible-mirror-v2" in ENTRY_TEXT
    assert "forkmesh/forkmesh refs integrity proof" in ENTRY_TEXT
    assert "REWARD_TRANSFERS_PER_INTENT = 8" in ENTRY_TEXT
    assert "reward_contribution_intents" in SCHEMA_TEXT


def test_every_successful_reward_transition_repeats_the_custody_boundary():
    contribution_start = ENTRY_TEXT.index(
        "async def reward_contributions_handler")
    contribution_body = ENTRY_TEXT[contribution_start:ENTRY_TEXT.index(
        "\n\ndef _chain_intent_public", contribution_start)]
    for state in (
        "prepared",
        "awaiting_finality",
        "public-community-pool",
    ):
        assert state in contribution_body
    assert contribution_body.count('"custody"') >= 4
    assert contribution_body.count('"notice"') >= 4

    signing_start = ENTRY_TEXT.index(
        "async def reward_signing_jobs_handler")
    signing_body = ENTRY_TEXT[signing_start:ENTRY_TEXT.index(
        "\n\nasync def verify_submitted_chain_intents", signing_start)]
    assert signing_body.count('"custody"') >= 4
    assert signing_body.count('"notice"') >= 4
    assert '"fundsState": "pending-finality"' in signing_body

    pending_start = ENTRY_TEXT.index("async def pending_rewards_handler")
    pending_body = ENTRY_TEXT[pending_start:ENTRY_TEXT.index(
        "\n\nasync def expire_pending_rewards", pending_start)]
    for state in (
        "external-source-wallet",
        "completed-on-chain-transfer",
        "ledger-allocation-only",
    ):
        assert state in pending_body
    assert pending_body.count('"custody"') >= 4
    assert pending_body.count('"notice"') >= 4


def test_signing_receipts_cannot_revive_or_replace_an_intent():
    start = ENTRY_TEXT.index("async def reward_signing_jobs_handler")
    body = ENTRY_TEXT[start:ENTRY_TEXT.index(
        "\n\nasync def verify_submitted_chain_intents", start)]
    assert 'row.get("status") not in ("pending_signature", "submitted")' in body
    assert '"error": "intent_not_signable"' in body
    assert 'int(row.get("expires_at") or 0) <= now' in body
    assert '"error": "intent_expired"' in body
    assert "existing_signature != tx_signature" in body
    assert '"error": "intent_signature_mismatch"' in body
