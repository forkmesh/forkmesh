#!/usr/bin/env python3
"""Non-custodial community reward-pool contracts."""

import ast
import asyncio
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
APP_RUNTIME = ROOT / "src" / "app_runtime.py"
SOLANA = ROOT / "src" / "solana.py"
SOLANA_TEXT = SOLANA.read_text(encoding="utf-8")
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
    + "\n" + (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
)
APP_RUNTIME_TEXT = APP_RUNTIME.read_text(encoding="utf-8")
DEPLOY_TEXT = (
    ROOT.parent / ".forkmesh" / "deploy.yml"
).read_text(encoding="utf-8")
PRODUCTION_ENV_EXAMPLE = (
    ROOT / ".env.production.example"
).read_text(encoding="utf-8")

SOLANA_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
POOL = "Fund1111111111111111111111111111111111111111"
PAYEE_A = "A1ice1111111111111111111111111111111111111x"
PAYEE_B = "Bob2222222222222222222222222222222222222222x"


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra_globals or {})
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=selected, type_ignores=[])), str(ENTRY), "exec"), namespace)
    return namespace


def _function_source(source, path, name):
    tree = ast.parse(source, filename=str(path))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(source, node)


class _Env:
    COMMUNITY_REWARD_POOL_ADDRESS = POOL
    REWARD_SIGNER_ACCOUNT = "instance-owner"


class _Date:
    @staticmethod
    def now():
        return 2_000_000_000_000


def test_pool_record_uses_only_public_configuration():
    async def pool_verified(_env, _address):
        return True

    ns = _load(
        "_reward_pool_address", "_central_fund_record", "_save_central_fund",
        extra_globals={
            "SOLANA_RE": SOLANA_RE,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
            "_reward_network": lambda _env: "devnet",
            "_reward_pool_network_verified": pool_verified,
        },
    )
    record = asyncio.run(ns["_central_fund_record"](_Env()))
    assert record == {
        "address": POOL,
        "signer_account": "instance-owner",
        "custody": "external-local-signer",
        "network": "devnet",
    }
    assert "secret" not in json.dumps(record).lower()
    assert asyncio.run(ns["_save_central_fund"](_Env(), record)) is False
    assert "CENTRAL_FUND_SOLANA_ADDRESS" not in ENTRY_TEXT


def test_missing_public_pool_configuration_never_generates_a_wallet():
    ns = _load(
        "_reward_pool_address", "_central_fund_record",
        extra_globals={
            "SOLANA_RE": SOLANA_RE,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
            "_reward_network": lambda _env: "devnet",
        },
    )
    assert asyncio.run(ns["_central_fund_record"](object())) is None
    assert "_new_solana_keypair" not in _function_source(
        ENTRY_TEXT, ENTRY, "_central_fund_record")


def test_reward_pool_address_keeps_treasury_compatibility_with_modern_priority():
    ns = _load(
        "_reward_pool_address",
        extra_globals={
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
        },
    )
    resolve = ns["_reward_pool_address"]

    treasury_only = type("_TreasuryOnly", (), {
        "TREASURY_SOLANA_ADDRESS": PAYEE_A,
    })()
    assert resolve(treasury_only) == PAYEE_A

    explicit_override = type("_Both", (), {
        "COMMUNITY_REWARD_POOL_ADDRESS": PAYEE_B,
        "TREASURY_SOLANA_ADDRESS": PAYEE_A,
    })()
    assert resolve(explicit_override) == PAYEE_B

    blank_override = type("_BlankOverride", (), {
        "COMMUNITY_REWARD_POOL_ADDRESS": "  ",
        "TREASURY_SOLANA_ADDRESS": PAYEE_A,
    })()
    assert resolve(blank_override) == PAYEE_A
    assert (
        "TREASURY_SOLANA_ADDRESS=${{ vars.TREASURY_SOLANA_ADDRESS }}"
        in DEPLOY_TEXT
    )
    assert (
        "TREASURY_SOLANA_ADDRESS=CHANGE-ME-PUBLIC-MAINNET-ADDRESS"
        in PRODUCTION_ENV_EXAMPLE
    )


def test_treasury_compatibility_address_is_public_only_and_non_custodial():
    class _TreasuryEnv:
        TREASURY_SOLANA_ADDRESS = POOL
        REWARD_SIGNER_ACCOUNT = "instance-owner"

    verified = []

    async def pool_verified(_env, address):
        verified.append(address)
        return True

    ns = _load(
        "_reward_pool_address", "_central_fund_record",
        extra_globals={
            "SOLANA_RE": SOLANA_RE,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
            "_reward_network": lambda _env: "mainnet-beta",
            "_reward_pool_network_verified": pool_verified,
        },
    )
    record = asyncio.run(ns["_central_fund_record"](_TreasuryEnv()))
    assert record == {
        "address": POOL,
        "signer_account": "instance-owner",
        "custody": "external-local-signer",
        "network": "mainnet-beta",
    }
    assert verified == [POOL]
    assert "private" not in json.dumps(record).lower()
    assert "seed" not in json.dumps(record).lower()

    prepare_start = ENTRY_TEXT.index(
        "async def reward_contributions_handler")
    prepare_body = ENTRY_TEXT[prepare_start:ENTRY_TEXT.index(
        "\n\ndef _chain_intent_public", prepare_start)]
    for contract in (
        '"programParticipation": "voluntary-community-support"',
        '"userWalletSignatureRequired": True',
        '"forkMeshSignsContributorTransfer": False',
        '"ownershipInterestGranted": False',
        '"financialReturnPromised": False',
    ):
        assert contract in prepare_body


def test_distribution_creates_unsigned_intent_without_signing_or_broadcasting():
    inserted = []
    audited = []
    signing_calls = []
    async def d1_first(_env, sql, *_args):
        if "status IN ('pending_signature','submitted')" in sql:
            return None
        if "FROM reward_rounds WHERE status='scheduled'" in sql:
            return {"round_id": "devnet:1999998000000", "execute_after": 0}
        raise AssertionError(sql)

    async def d1_run(_env, sql, *args):
        inserted.append((sql, args))

    async def balance(_env, address):
        assert address == POOL
        return 1_005_000

    async def audit(_env, *args):
        audited.append(args)

    async def forbidden_signer(*_args):
        signing_calls.append(True)
        raise AssertionError("Worker signing must never run")

    async def fund(_env):
        return {
            "address": POOL,
            "signer_account": "instance-owner",
            "custody": "external-local-signer",
            "network": "devnet",
        }

    ns = _load("_distribute_central_fund", extra_globals={
        "Date": _Date,
        "SOLANA_RE": SOLANA_RE,
        "REWARD_POOL_FEE_RESERVE_LAMPORTS": 5000,
        "_is_main_relay": lambda _env: True,
        "_central_fund_record": fund,
        "_reward_config": lambda _env: {
            "intervalMs": 3_600_000,
            "jitterMs": 0,
            "amountLamports": 100_000,
        },
        "_eligible_reward_snapshot": lambda _env, _now: asyncio.sleep(
            0, result={
                "eligible": [{
                    "nodeId": "alice-node",
                    "walletAddress": PAYEE_A,
                }],
                "snapshotHash": "snapshot",
            }),
        "_reward_public_entropy": lambda _env: asyncio.sleep(
            0, result="finalized-blockhash"),
        "_reward_network": lambda _env: "devnet",
        "_reward_balance_lamports": balance,
        "reward_policy": type("_Policy", (), {
            "select_candidate": staticmethod(
                lambda snapshot, **_kwargs: {
                    "candidate": snapshot["eligible"][0],
                    "eligibleCount": 1,
                    "snapshotHash": "snapshot",
                    "selectedIndex": 0,
                })
        }),
        "_solana_send_transfers": forbidden_signer,
        "_solana_broadcast_raw": forbidden_signer,
        "_random_bytes": lambda _size: bytes.fromhex(
            "0123456789abcdef0123456789abcdef"
        ),
        "_audit_sensitive_action": audit,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "hashlib": hashlib,
        "json": json,
    })
    assert asyncio.run(ns["_distribute_central_fund"](_Env())) is True
    assert signing_calls == []
    sql, args = next(
        (sql, args) for sql, args in inserted
        if "INSERT INTO chain_intents" in sql
    )
    assert "INSERT INTO chain_intents" in sql
    assert args[1] == "community_reward_random"
    plan = json.loads(args[4])
    assert plan["sourceAddress"] == POOL
    assert plan["transfers"] == [
        {
            "address": PAYEE_A,
            "lamports": 100_000,
            "nodeId": "alice-node",
        },
    ]
    assert plan["selection"]["snapshotHash"] == "snapshot"
    assert audited
    assert "secret" not in json.dumps(plan).lower()


def test_existing_unsigned_intent_prevents_duplicate_distribution():
    async def d1_first(_env, sql, *_args):
        if "status IN ('pending_signature','submitted')" in sql:
            return {"intent_id": "a" * 32}
        raise AssertionError(sql)

    async def fund(_env):
        return {"address": POOL, "signer_account": "instance-owner"}

    ns = _load("_distribute_central_fund", extra_globals={
        "Date": _Date,
        "_is_main_relay": lambda _env: True,
        "_central_fund_record": fund,
        "_reward_config": lambda _env: {},
        "d1_first": d1_first,
        "d1_run": lambda *_args: asyncio.sleep(0),
    })
    assert asyncio.run(ns["_distribute_central_fund"](_Env())) is False


def test_central_fund_flow_never_calls_worker_signing_or_broadcasting():
    central_fund_source = "\n".join(
        _function_source(ENTRY_TEXT, ENTRY, name)
        for name in (
            "_central_fund_record",
            "_save_central_fund",
            "_distribute_central_fund",
        )
    )
    for symbol in (
        "_new_solana_keypair",
        "_solana_send_transfers",
        "_solana_sign_transfers",
        "_solana_broadcast_raw",
    ):
        assert symbol not in central_fund_source


def test_shared_solana_rpc_rejects_transaction_submission_methods():
    assert '"getAccountInfo"' in SOLANA_TEXT
    assert '"getGenesisHash"' in SOLANA_TEXT
    tree = ast.parse(SOLANA_TEXT, filename=str(SOLANA))
    nodes = {
        item.name: item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name in {"_solana_rpc", "_solana_rpc_write"}
    }
    calls = []

    async def rpc_call(_env, method, params):
        calls.append((method, params))
        return {"result": "submitted"}

    namespace = {
        "_SOLANA_READ_ONLY_METHODS": {
            "getAccountInfo", "getBalance", "getGenesisHash",
            "getLatestBlockhash", "getSignatureStatuses", "getTransaction",
        },
        "_SOLANA_WRITE_METHODS": {"sendTransaction"},
        "_solana_rpc_call": rpc_call,
    }
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=list(nodes.values()), type_ignores=[])), str(SOLANA), "exec"),
        namespace)
    assert asyncio.run(
        namespace["_solana_rpc"](
            object(), "sendTransaction", ["base64-signed-payload"]
        )
    ) is None
    assert asyncio.run(
        namespace["_solana_rpc_write"](object(), "getBalance", [POOL])
    ) is None
    assert calls == []
    assert asyncio.run(namespace["_solana_rpc_write"](
        object(), "sendTransaction", ["base64-signed-payload"]
    )) == {"result": "submitted"}
    assert calls == [("sendTransaction", ["base64-signed-payload"])]
    assert "sendTransaction" not in _function_source(
        SOLANA_TEXT, SOLANA, "_solana_rpc")
    assert "AbortSignal.timeout(SOLANA_RPC_TIMEOUT_MS)" in SOLANA_TEXT
    assert "asyncio.wait_for" not in SOLANA_TEXT


def test_legacy_treasury_aliases_fail_closed_without_an_address():
    def response(payload, **kwargs):
        return payload, kwargs

    ns = _load(
        "_legacy_treasury_disabled_response",
        "_account_treasury_address",
        "_federation_treasury_address",
        extra_globals={"json_response": response},
    )
    for function in (
        ns["_account_treasury_address"],
        ns["_federation_treasury_address"],
    ):
        payload, options = asyncio.run(function(object(), object()))
        assert options["status"] == 410
        assert payload["address"] == ""
        assert payload["error"] == "legacy_custody_disabled"
        assert payload["featureState"] == "migration-only"
        assert payload["custody"] == "non-custodial"
        assert set(payload["fundStates"]) == {
            "userOwnedFunds",
            "communityFundedRewardPool",
            "pendingRewards",
            "completedOnChainTransfers",
        }
        assert "does not hold" in payload["notice"]


def _verification_harness(rpc, memo=None, now=2_000_000_000_000, shared=None):
    """Load the real _reward_pool_network_verified with a scripted RPC.

    ``shared`` stands in for the schema_meta table the fleet-wide proof row
    lives in: hand the same dict to two harnesses and they model two isolates
    of one Worker, each with its own in-memory memo.
    """
    calls = []
    rows = shared if shared is not None else {}

    async def d1_first(_env, sql, *args):
        assert "schema_meta" in sql
        return {"v": rows[args[0]]} if args[0] in rows else None

    async def d1_run(_env, sql, *args):
        assert "schema_meta" in sql
        if sql.lstrip().upper().startswith("DELETE"):
            rows.pop(args[0], None)
        else:
            rows[args[0]] = args[1]

    async def reward_rpc(_env, method, _params):
        calls.append(method)
        return rpc.get(method)

    class _FrozenDate:
        @staticmethod
        def now():
            return now

    ns = _load(
        "_reward_pool_network_verified",
        "_stored_reward_pool_proof",
        "_remember_reward_pool_proof",
        "_forget_reward_pool_proof",
        extra_globals={
            "SOLANA_RE": SOLANA_RE,
            "Date": _FrozenDate,
            "hmac": __import__("hmac"),
            "json": json,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "REWARD_CLUSTER_GENESIS_HASHES": {"devnet": "GENESIS"},
            "REWARD_POOL_VERIFY_FRESH_MS": 60 * 60 * 1000,
            "REWARD_POOL_VERIFY_STALE_MAX_MS": 24 * 60 * 60 * 1000,
            "REWARD_POOL_VERIFY_SHARE_RECHECK_MS": 60 * 1000,
            "REWARD_POOL_VERIFY_STATE_KEY": "reward_pool_verified",
            "_REWARD_POOL_VERIFY_MEMO": memo
            if memo is not None else {"key": None, "ts": 0, "read": 0},
            "_reward_network": lambda _env: "devnet",
            "_reward_rpc_url": lambda _env: "https://rpc.example",
            "_reward_solana_rpc": reward_rpc,
        },
    )
    return ns["_reward_pool_network_verified"], calls


def test_pool_verification_memoizes_the_stable_network_proof():
    # The proof (genesis hash + pool account existence) is stable
    # configuration, so a verified triple must not re-run two Solana RPC
    # round trips on every /api/accounts/central-fund view — public RPC rate
    # limits turned that into recurring 503s (2026-07-24 error log).
    memo = {"key": None, "ts": 0}
    verify, calls = _verification_harness({
        "getGenesisHash": {"result": "GENESIS"},
        "getAccountInfo": {"result": {"value": {"lamports": 5}}},
    }, memo=memo)
    assert asyncio.run(verify(_Env(), POOL)) is True
    assert calls == ["getGenesisHash", "getAccountInfo"]
    assert asyncio.run(verify(_Env(), POOL)) is True
    assert calls == ["getGenesisHash", "getAccountInfo"]  # fresh hit: no RPC


def test_pool_verification_serves_bounded_stale_through_rpc_outages():
    hour = 60 * 60 * 1000
    verified_at = 2_000_000_000_000
    memo = {"key": ("devnet", "https://rpc.example", POOL), "ts": verified_at}
    # Two hours later (past the fresh TTL) the RPC answers nothing at all:
    # the previously proven configuration keeps serving.
    verify, calls = _verification_harness(
        {}, memo=memo, now=verified_at + 2 * hour)
    assert asyncio.run(verify(_Env(), POOL)) is True
    assert calls == ["getGenesisHash"]
    # Past the stale bound a dead RPC fails closed again.
    verify, _ = _verification_harness(
        {}, memo=memo, now=verified_at + 25 * hour)
    assert asyncio.run(verify(_Env(), POOL)) is False


def test_pool_verification_proof_survives_isolate_recycling():
    # adhoc #176: a crawler GET /api/accounts/central-fund answered 503 (cpu
    # 2ms, wall 988ms — no work, just a public RPC that never replied) while
    # the pool was configured and funded. The memo above is per isolate, so
    # every recycled isolate paid the two round trips again and a single rate
    # limit fell straight through to "unavailable". The proof is now shared,
    # so a cold isolate serves without touching the RPC at all.
    shared = {}
    verify, calls = _verification_harness({
        "getGenesisHash": {"result": "GENESIS"},
        "getAccountInfo": {"result": {"value": {"lamports": 5}}},
    }, shared=shared)
    assert asyncio.run(verify(_Env(), POOL)) is True
    assert calls == ["getGenesisHash", "getAccountInfo"]
    assert json.loads(shared["reward_pool_verified"])["key"] == [
        "devnet", "https://rpc.example", POOL]

    # A fresh isolate half an hour later, with every RPC endpoint refusing.
    cold, cold_calls = _verification_harness(
        {}, now=2_000_000_000_000 + 30 * 60 * 1000, shared=shared)
    assert asyncio.run(cold(_Env(), POOL)) is True
    assert cold_calls == []


def test_shared_proof_is_adopted_at_its_own_age_not_renewed():
    hour = 60 * 60 * 1000
    proved_at = 2_000_000_000_000
    shared = {"reward_pool_verified": json.dumps(
        {"key": ["devnet", "https://rpc.example", POOL], "ts": proved_at})}
    # Two hours on, a cold isolate inherits the proof past its fresh TTL: it
    # re-proves, and serves stale only because the RPC answered nothing.
    verify, calls = _verification_harness(
        {}, now=proved_at + 2 * hour, shared=shared)
    assert asyncio.run(verify(_Env(), POOL)) is True
    assert calls == ["getGenesisHash"]
    # Adopting must not restamp the proof, or a dead RPC would be smoothed
    # over forever by isolates handing the same row back and forth.
    assert json.loads(shared["reward_pool_verified"])["ts"] == proved_at
    stale, _ = _verification_harness(
        {}, now=proved_at + 25 * hour, shared=shared)
    assert asyncio.run(stale(_Env(), POOL)) is False


def test_shared_proof_ignores_a_different_configuration():
    # The row is keyed by (network, rpc_url, address). Repointing any of them
    # must re-prove rather than inherit the previous pool's verification.
    shared = {"reward_pool_verified": json.dumps(
        {"key": ["devnet", "https://rpc.example", PAYEE_A], "ts":
         2_000_000_000_000})}
    verify, calls = _verification_harness({}, shared=shared)
    assert asyncio.run(verify(_Env(), POOL)) is False
    assert calls == ["getGenesisHash"]


def test_wrong_cluster_answer_revokes_the_shared_proof_fleet_wide():
    hour = 60 * 60 * 1000
    proved_at = 2_000_000_000_000
    shared = {"reward_pool_verified": json.dumps(
        {"key": ["devnet", "https://rpc.example", POOL], "ts": proved_at})}
    memo = {"key": ("devnet", "https://rpc.example", POOL), "ts": proved_at,
            "read": 0}
    verify, _ = _verification_harness(
        {"getGenesisHash": {"result": "MAINNET-HASH"}},
        memo=memo, now=proved_at + 2 * hour, shared=shared)
    assert asyncio.run(verify(_Env(), POOL)) is False
    # Clearing only this isolate's memo would let the next cold one read the
    # revoked proof back out of D1 and keep serving.
    assert shared == {}
    cold, _ = _verification_harness(
        {}, now=proved_at + 2 * hour, shared=shared)
    assert asyncio.run(cold(_Env(), POOL)) is False


def test_pool_verification_wrong_cluster_answer_clears_the_fallback():
    memo = {"key": ("devnet", "https://rpc.example", POOL),
            "ts": 2_000_000_000_000}
    # A real ANSWER naming the wrong cluster is disqualifying — it must not
    # be smoothed over by the stale window, and it revokes the fallback.
    verify, _ = _verification_harness(
        {"getGenesisHash": {"result": "MAINNET-HASH"}},
        memo=memo, now=2_000_000_000_000 + 2 * 60 * 60 * 1000)
    assert asyncio.run(verify(_Env(), POOL)) is False
    assert memo["key"] is None


def test_account_central_fund_is_only_a_canonical_public_pool_alias():
    start = ENTRY_TEXT.index("async def _account_central_fund")
    body = ENTRY_TEXT[start:ENTRY_TEXT.index(
        "\n\nasync def accounts_handler", start)]
    # The public payload, optionally re-reading the on-chain balance when the
    # caller pressed a refresh button rather than on every page load.
    assert "_central_fund_public(\n        env, refresh=_wants_balance_refresh(request))" in body
    assert "_treasury_address" not in body
    assert "central_fund WHERE" not in body
    public_start = ENTRY_TEXT.index("async def _central_fund_public")
    public_body = ENTRY_TEXT[public_start:ENTRY_TEXT.index(
        "\n\nasync def _distribute_central_fund", public_start)]
    for state in (
        "userOwnedFunds",
        "communityFundedPool",
        "pendingRewards",
        "completedOnChainTransfers",
    ):
        assert state in public_body
    assert "privateKeyStoredByWorker" in public_body


def test_network_stats_no_longer_advertises_a_paid_join_deposit():
    start = ENTRY_TEXT.index("async def network_stats")
    body = ENTRY_TEXT[start:ENTRY_TEXT.index(
        "\n\nasync def _network_payout_nodes", start)]
    for legacy_field in (
        "minLamports", "minSol", "minUsd", "_min_join_lamports",
    ):
        assert legacy_field not in body
    for field in (
        "payoutCustody",
        "payoutFundStates",
        "payoutNotice",
    ):
        assert field in body
    for state in (
        "userOwnedFunds",
        "communityFundedPool",
        "pendingRewards",
        "completedOnChainTransfers",
    ):
        assert state in body


def test_active_public_wallet_and_reporting_apis_repeat_custody_notices():
    for name, following, fields in (
        (
            "async def network_leaderboards",
            "\n\ndef _short_wallet",
            ("fundsState", "fundsCustody", "fundsNotice"),
        ),
        (
            "async def _account_public_payload",
            "\n\ndef _account_chat_user_payload",
            ("payoutCustody", "payoutNotice"),
        ),
        (
            "async def _account_heartbeat",
            "\n\nasync def _mirroring_owners",
            (
                "payoutCustody",
                "payoutNotice",
                "balanceFundsState",
                "balanceIncreased",
            ),
        ),
    ):
        start = ENTRY_TEXT.index(name)
        body = ENTRY_TEXT[start:ENTRY_TEXT.index(following, start)]
        for field in fields:
            assert f'"{field}"' in body
    assert '"donationReceived"' not in ENTRY_TEXT


def test_legacy_federated_deposit_views_repeat_migration_only_notice():
    for name, following in (
        ("async def _federation_donation_address",
         "\nasync def _federation_donation_status"),
        ("async def _federation_donation_status",
         "\nasync def _federation_nodes"),
        ("async def _federated_donation_address",
         "\n\nasync def _federated_donation_status"),
        ("async def _federated_donation_status",
         "\n\n# --- Federation cron"),
    ):
        start = ENTRY_TEXT.index(name)
        body = ENTRY_TEXT[start:ENTRY_TEXT.index(following, start)]
        assert '"custody"' in body
        assert '"notice"' in body
        assert (
            "legacy_custody_disabled" in body
            or "legacy_custody_frozen" in body
        )


def test_source_contract_is_read_only_and_external_signer_only():
    assert '"/api/accounts/central-fund"' in ENTRY_TEXT
    # The public control-plane routes are dispatched by the lazily loaded
    # runtime; their non-custodial implementation remains in entry.py.
    assert '"/api/rewards/pool"' in APP_RUNTIME_TEXT
    assert '"/api/rewards/signing-jobs"' in APP_RUNTIME_TEXT
    assert "CREATE TABLE IF NOT EXISTS chain_intents" in ENTRY_TEXT
    assert "privateKeyStoredByWorker" in ENTRY_TEXT
    assert '"external-local-signer"' in ENTRY_TEXT
    import_block = ENTRY_TEXT[
        ENTRY_TEXT.index('_solana = _LazyModule("solana")'):
        ENTRY_TEXT.index("MAX_ROOM_NAME =", ENTRY_TEXT.index('_solana ='))
    ]
    assert "_solana_sign_message" not in import_block
    assert "_solana_send_transaction" not in import_block
