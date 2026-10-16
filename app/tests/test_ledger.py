#!/usr/bin/env python3
"""Contracts for the public financial ledger and its source linkage."""

from __future__ import annotations

import asyncio
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
SRC = ROOT / "src"
PUBLIC = ROOT / "public"

sys.path.insert(0, str(SRC))
import ledger  # noqa: E402


def _run(coroutine):
    return asyncio.run(coroutine)


def test_exact_sol_and_usd_math_does_not_use_binary_float_rounding():
    assert ledger._sol_text(1_000_000_001) == "1.000000001"
    assert ledger._sol_text(50_000_000) == "0.05"
    assert ledger._usd_from_lamports(50_000_000, 100_000_000) == (
        "5.000000", 5_000_000)
    assert ledger._usd_from_lamports(1, 123_456_789) == (
        "0.000000", 0)
    usd = ledger._entry(
        "ad:1", "advertising-revenue", "Receipt", "recorded", 1,
        currency="USD", amount_minor=12345,
    )
    assert usd["amountUsd"] == "123.45"
    assert usd["amountUsdMicros"] == 123_450_000
    paid = ledger._entry(
        "reward:1", "contributor-reward", "Paid", "paid", 10,
        lamports=3_000_000, price_micros=999_000_000,
        recorded_usd_micros=450_000,
        recorded_price_micros=150_000_000,
        recorded_valued_at=9,
    )
    assert paid["amountUsd"] == "0.450000"
    assert paid["valuationBasis"] == "recorded-at-payment-sol-usd"
    assert paid["valuationObservedAt"] == 9


def test_solana_explorer_links_preserve_the_selected_network():
    signature = "A signature with punctuation/+"
    assert ledger._explorer_url(signature, "mainnet") == (
        "https://explorer.solana.com/tx/A%20signature%20with%20punctuation%2F%2B"
    )
    assert ledger._explorer_url(signature, "devnet").endswith(
        "?cluster=devnet")
    assert ledger._wallet_link("wallet/address", "recipient", "testnet")[
        "url"
    ].endswith("wallet%2Faddress?cluster=testnet")


def test_backfill_parses_only_successful_system_transfers_for_enrolled_address():
    observed = "A" * 32
    destination = "B" * 32
    unrelated = "C" * 32
    signature = "5" * 88
    response = {
        "result": {
            "blockTime": 123,
            "meta": {"err": None},
            "transaction": {
                "signatures": [signature],
                "message": {"instructions": [
                    {
                        "program": "system",
                        "parsed": {"type": "transfer", "info": {
                            "source": observed,
                            "destination": destination,
                            "lamports": 42,
                        }},
                    },
                    {
                        "program": "system",
                        "parsed": {"type": "transfer", "info": {
                            "source": unrelated,
                            "destination": destination,
                            "lamports": 99,
                        }},
                    },
                    {
                        "program": "spl-token",
                        "parsed": {"type": "transfer", "info": {
                            "source": observed,
                            "destination": destination,
                            "lamports": 100,
                        }},
                    },
                ]},
            },
        },
    }
    assert ledger._parsed_system_transfers(
        response, signature, observed) == [{
            "instructionIndex": 0,
            "sourceAddress": observed,
            "destinationAddress": destination,
            "lamports": 42,
            "blockTime": 123_000,
        }]
    failed = dict(response)
    failed["result"] = dict(response["result"], meta={"err": "failed"})
    assert ledger._parsed_system_transfers(failed, signature, observed) == []


def test_backfill_scan_persists_receipt_and_repairs_legacy_sweep(monkeypatch):
    observed = "A" * 32
    destination = "B" * 32
    signature = "5" * 88
    writes = []

    async def rpc(_env, _network, method, _params):
        if method == "getSignaturesForAddress":
            return {"result": [{"signature": signature, "err": None}]}
        assert method == "getTransaction"
        return {"result": {
            "blockTime": 123,
            "meta": {"err": None},
            "transaction": {
                "signatures": [signature],
                "message": {"instructions": [{
                    "program": "system",
                    "parsed": {"type": "transfer", "info": {
                        "source": observed,
                        "destination": destination,
                        "lamports": 495_000,
                    }},
                }]},
            },
        }}

    async def d1_run(_env, sql, *arguments):
        writes.append((sql, arguments))

    monkeypatch.setattr(ledger, "_backfill_rpc", rpc)
    monkeypatch.setattr(ledger, "d1_run", d1_run, raising=False)
    discovered = _run(ledger._scan_backfill_source(None, {
        "source_id": "world-deposit:purchase-1",
        "address": observed,
        "network": "mainnet-beta",
        "before_signature": "",
    }, 999))

    assert discovered == 1
    insert = next(call for call in writes
                  if "INSERT OR IGNORE INTO ledger_backfill_transactions"
                  in call[0])
    assert insert[1][1:4] == ("mainnet-beta", signature, 0)
    sweep = next(call for call in writes
                 if "SET sweep_lamports=?" in call[0])
    assert sweep[1] == (495_000, signature, observed)
    cursor = next(call for call in writes
                  if "SET before_signature=?" in call[0])
    assert cursor[1] == (signature, 1, 999, "world-deposit:purchase-1")


def test_backfill_source_enrollment_executes_against_sqlite(monkeypatch):
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript(
        """
        CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
        CREATE TABLE issue_bounty (
          bounty_bi TEXT PRIMARY KEY, data TEXT NOT NULL
        );
        CREATE TABLE legacy_custody_reconciliation (
          record_id TEXT PRIMARY KEY, source_table TEXT,
          public_address TEXT, network TEXT
        );
        CREATE TABLE world_element_purchases (
          purchase_id TEXT PRIMARY KEY, deposit_address TEXT,
          sweep_signature TEXT
        );
        CREATE TABLE ledger_backfill_sources (
          source_id TEXT PRIMARY KEY, source_kind TEXT NOT NULL,
          address TEXT NOT NULL, network TEXT NOT NULL,
          subject_repo_bi TEXT NOT NULL DEFAULT '',
          subject_kind TEXT NOT NULL DEFAULT '',
          subject_number INTEGER NOT NULL DEFAULT 0,
          before_signature TEXT NOT NULL DEFAULT '',
          completed INTEGER NOT NULL DEFAULT 0,
          updated_at INTEGER NOT NULL DEFAULT 0,
          UNIQUE (network, address)
        );
        INSERT INTO issue_bounty VALUES ('bounty-bi', 'sealed-bounty');
        INSERT INTO legacy_custody_reconciliation VALUES (
          'legacy-row', 'central_fund',
          'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA', 'mainnet-beta'
        );
        INSERT INTO world_element_purchases VALUES (
          'purchase-1', 'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB', 'sweep'
        );
        """
    )

    async def d1_all(_env, sql, *arguments):
        return [dict(row) for row in database.execute(sql, arguments).fetchall()]

    async def d1_first(_env, sql, *arguments):
        row = database.execute(sql, arguments).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *arguments):
        database.execute(sql, arguments)
        database.commit()

    async def decrypt_row(_env, stored):
        assert stored == "sealed-bounty"
        return {
            "owner": "forkmesh",
            "repo": "app",
            "number": 7,
            "address": "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
        }

    async def blind_index(_env, value):
        assert value == "forkmesh/app"
        return "repo-bi"

    monkeypatch.setattr(ledger, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(ledger, "d1_first", d1_first, raising=False)
    monkeypatch.setattr(ledger, "d1_run", d1_run, raising=False)
    monkeypatch.setattr(ledger, "decrypt_row", decrypt_row, raising=False)
    monkeypatch.setattr(ledger, "blind_index", blind_index, raising=False)
    try:
        _run(ledger._seed_backfill_sources(SimpleNamespace(
            COMMUNITY_REWARD_POOL_ADDRESS=("D" * 32),
            COMMUNITY_REWARD_NETWORK="mainnet-beta",
        ), 999))
        sources = [
            dict(row) for row in database.execute(
                "SELECT * FROM ledger_backfill_sources ORDER BY source_kind"
            ).fetchall()
        ]
    finally:
        database.close()

    assert {row["source_kind"] for row in sources} == {
        "community-pool",
        "legacy-bounty",
        "legacy-central_fund",
        "world-deposit",
    }
    bounty = next(row for row in sources
                  if row["source_kind"] == "legacy-bounty")
    assert bounty["subject_repo_bi"] == "repo-bi"
    assert bounty["subject_kind"] == "issue"
    assert bounty["subject_number"] == 7


def test_microjob_subjects_link_directly_to_the_issue_or_pull_request():
    pull = ledger._microjob_subject({
        "record": {
            "owner": "ForkMesh", "repo": "App", "number": 17,
            "title": "Ship the ledger title links",
        },
    }, "merged_pull")
    issue = ledger._microjob_subject({
        "record": {
            "owner": "ForkMesh", "repo": "App", "number": 23,
            "title": "Ledger rows do not link",
        },
    }, "issue_report")
    assert pull["type"] == "pull"
    assert pull["url"] == "/forkmesh/app/pulls/17"
    assert pull["title"] == "Ship the ledger title links"
    assert issue["type"] == "issue"
    assert issue["url"] == "/forkmesh/app/issues/23"
    assert issue["label"] == "forkmesh/app issue #23"
    assert issue["title"] == "Ledger rows do not link"


def test_legacy_world_sweep_amount_is_explicitly_unknown():
    entry = ledger._entry(
        "world-sweep:old", "world-element-sweep", "Deposit sweep",
        "completed", 1, signature="receipt", lamports=0,
        amount_known=False, price_micros=100_000_000,
    )
    assert entry["transactionId"] == "receipt"
    assert entry["amountKnown"] is False
    assert entry["amountSol"] is None
    assert entry["amountUsd"] is None


def test_public_handler_projects_canonical_sources_and_links_paid_work(monkeypatch):
    signature = "5" * 88
    queries = []

    async def d1_all(_env, sql, *_args):
        queries.append(sql)
        if "FROM ledger_backfill_transactions AS history" in sql:
            return [{
                "entry_id": "chain-backfill:devnet:historic:000",
                "network": "devnet", "signature": "6" * 88,
                "instruction_index": 0, "block_time": 700,
                "source_address": "historic-source",
                "destination_address": "historic-recipient",
                "amount_lamports": 25_000_000,
                "observed_address": "historic-source",
                "discovered_from": "legacy-reconciliation:old",
                "source_kind": "legacy-central_fund",
                "subject_repo_bi": "", "subject_kind": "",
                "subject_number": 0, "repo_data": "",
            }]
        if "FROM tip_notice AS tips" in sql:
            return [{
                "signature": "4" * 88,
                "from_bi": "from", "to_bi": "to",
                "lamports": 50_000_000, "network": "devnet", "ts": 950,
                "sender_name": "alice", "recipient_name": "bob",
            }]
        if "FROM reward_contributions" in sql:
            return []
        if "FROM pending_rewards" in sql:
            return []
        if "FROM chain_intents" in sql:
            return []
        if "FROM microjob_claims" in sql:
            return [{
                "claim_id": "claim-1", "job": "merged_pull",
                "status": "paid", "amount_lamports": 50_000_000,
                "data": "sealed-claim", "created_at": 900,
                "updated_at": 1_000, "intent_id": "intent-1",
                "tx_signature": signature, "account_name": "alice",
            }]
        if "FROM world_element_purchases" in sql:
            return []
        if "FROM community_ad_revenue" in sql:
            return [{
                "entry_id": "ad-1", "instance_id": "main",
                "proposal_id": "proposal-1",
                "ledger_class": "advertising-revenue",
                "amount_minor": 2500, "currency": "USD",
                "destination_snapshot": "project operations",
                "external_reference": "invoice-42", "recorded_at": 800,
                "proposer_name": "bob",
            }]
        if "FROM operating_expenses" in sql:
            return [{
                "expense_id": "cursor-e45e60f2-0007",
                "vendor": "Cursor",
                "description": "Cursor development subscription",
                "paid_by_name": "mdkaifansari04",
                "amount_minor": 2360,
                "currency": "USD",
                "original_amount_minor": 234359,
                "original_currency": "INR",
                "external_reference": "E45E60F2-0007",
                "payment_method_summary": "private card detail",
                "incurred_at": 850,
                "recorded_at": 1200,
                "reimbursement_status": "outstanding",
                "reimbursed_at": 0,
                "reimbursement_reference": "",
            }]
        raise AssertionError(sql)

    async def d1_first(_env, sql, *_args):
        assert "ledger_backfill_sources" in sql
        return {
            "sources": 2,
            "completed_sources": 1,
            "transfers": 1,
        }

    async def decrypt_row(_env, stored):
        assert stored == "sealed-claim"
        return {
            "account": "alice",
            "subject": "forkmesh/app pull #17",
            "record": {"owner": "forkmesh", "repo": "app", "number": 17},
        }

    async def quote():
        return {
            "priceMicros": 100_000_000,
            "observedAt": 777,
            "expiresAt": 999,
        }

    async def ensure_schema(_env):
        return None

    class Date:
        @staticmethod
        def now():
            return 1_234

    def json_response(payload, status=200, cache_control="", extra_headers=None):
        return {
            "payload": payload,
            "status": status,
            "cacheControl": cache_control,
            "headers": extra_headers or {},
        }

    monkeypatch.setattr(ledger, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(ledger, "d1_first", d1_first, raising=False)
    monkeypatch.setattr(ledger, "decrypt_row", decrypt_row, raising=False)
    monkeypatch.setattr(ledger, "_sol_usd_quote", quote)
    monkeypatch.setattr(ledger, "ensure_schema", ensure_schema, raising=False)
    monkeypatch.setattr(ledger, "Date", Date, raising=False)
    monkeypatch.setattr(
        ledger, "method_name", lambda request: request.method, raising=False)
    monkeypatch.setattr(ledger, "json_response", json_response, raising=False)

    request = SimpleNamespace(
        method="GET", url="https://app.forkmesh.com/api/ledger?limit=20")
    response = _run(ledger.ledger_handler(
        SimpleNamespace(SOLANA_NETWORK="devnet"), request))
    payload = response["payload"]

    assert response["status"] == 200
    assert response["cacheControl"].startswith("public, max-age=30")
    assert payload["schemaVersion"] == 1
    assert payload["network"] == "devnet"
    assert payload["pageTotals"] == {
        "records": 6,
        "withTransactionId": 3,
        "solLamports": 125_000_000,
    }
    by_id = {entry["id"]: entry for entry in payload["entries"]}

    payout = by_id["microjob:claim-1"]
    assert payout["sourceTable"] == "microjob_claims"
    assert payout["transactionId"] == signature
    assert payout["amountSol"] == "0.05"
    assert payout["amountUsd"] == "5.000000"
    assert payout["explorerUrl"].endswith("?cluster=devnet")
    assert any(
        link["type"] == "user" and link["url"] == "/@alice"
        for link in payout["links"])
    assert any(
        link["type"] == "pull"
        and link["url"] == "/forkmesh/app/pulls/17"
        for link in payout["links"])

    advertising = by_id["advertising-revenue:ad-1"]
    assert advertising["amountUsd"] == "25.00"
    assert advertising["transactionId"] == ""
    assert any(link["url"] == "/@bob" for link in advertising["links"])
    expense = by_id["operating-expense:cursor-e45e60f2-0007"]
    debt = by_id["reimbursement-debt:cursor-e45e60f2-0007"]
    assert expense["direction"] == "expense"
    assert expense["amountUsd"] == "23.60"
    assert expense["sourceTable"] == "operating_expenses"
    assert expense["sourceId"] == debt["sourceId"]
    assert debt["direction"] == "debt"
    assert debt["status"] == "outstanding"
    assert debt["amountUsdMicros"] == expense["amountUsdMicros"] == 23_600_000
    assert any(link["url"] == "/@mdkaifansari04" for link in debt["links"])
    assert "INR 2343.59" in expense["note"]
    assert "private card detail" not in expense["note"]
    assert "payment_method_summary" not in " ".join(queries)
    historical = by_id["chain-backfill:devnet:historic:000"]
    assert historical["amountSol"] == "0.025"
    assert historical["verification"] == "finalized-read-only-chain-backfill"
    assert historical["explorerUrl"].endswith("?cluster=devnet")
    assert len(queries) == 12
    assert payload["backfill"] == {
        "sourceAddresses": 2,
        "completedSourceAddresses": 1,
        "discoveredTransfers": 1,
        "running": True,
        "scope": (
            "configured community pool, temporary World deposit addresses, "
            "and public legacy-custody reconciliation addresses"
        ),
        "memberWalletHistoriesScanned": False,
    }
    assert payload["scope"]["projection"] == "canonical source-of-truth records"
    assert payload["scope"]["privateRepositoryDetailsExposed"] is False
    assert payload["scope"]["payoutKeysStoredByWorker"] is False
    assert payload["scope"]["temporaryWorldDepositKeysExposed"] is False
    assert payload["backfill"]["memberWalletHistoriesScanned"] is False


def test_projection_sql_orders_events_and_paginates_equal_timestamps(monkeypatch):
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript(
        """
        CREATE TABLE users (user_bi TEXT PRIMARY KEY, username TEXT);
        CREATE TABLE tip_notice (
          signature TEXT, from_bi TEXT, to_bi TEXT, lamports INTEGER,
          network TEXT, ts INTEGER
        );
        CREATE TABLE reward_contributions (
          contribution_id TEXT, mode TEXT, amount_lamports INTEGER,
          status TEXT, created_at INTEGER, tx_signature TEXT,
          source_address TEXT, confirmed_at INTEGER,
          distribution_intent_id TEXT
        );
        CREATE TABLE pending_rewards (
          reward_id TEXT, recipient_bi TEXT, recipient_name TEXT,
          source_address TEXT, amount_lamports INTEGER, wallet_address TEXT,
          status TEXT, created_at INTEGER, expires_at INTEGER,
          intent_id TEXT, tx_signature TEXT
        );
        CREATE TABLE chain_intents (
          intent_id TEXT, kind TEXT, source_address TEXT, status TEXT,
          data TEXT, created_at INTEGER, tx_signature TEXT,
          completed_at INTEGER
        );
        CREATE TABLE microjob_claims (
          claim_id TEXT, account_bi TEXT, job TEXT, status TEXT,
          amount_lamports INTEGER, data TEXT, created_at INTEGER,
          updated_at INTEGER, intent_id TEXT, tx_signature TEXT
        );
        CREATE TABLE world_element_purchases (
          purchase_id TEXT, account_bi TEXT, element_id TEXT,
          amount_lamports INTEGER, status TEXT, created_at INTEGER,
          tx_signature TEXT, source_address TEXT, confirmed_at INTEGER,
          distribution_intent_id TEXT, deposit_address TEXT,
          sweep_lamports INTEGER, sweep_signature TEXT, sweep_at INTEGER
        );
        CREATE TABLE community_ad_proposals (
          proposal_id TEXT, proposer_bi TEXT
        );
        CREATE TABLE community_ad_revenue (
          entry_id TEXT, instance_id TEXT, proposal_id TEXT,
          ledger_class TEXT, amount_minor INTEGER, currency TEXT,
          destination_snapshot TEXT, external_reference TEXT,
          recorded_at INTEGER
        );
        CREATE TABLE operating_expenses (
          expense_id TEXT PRIMARY KEY, vendor TEXT, description TEXT,
          paid_by_name TEXT, amount_minor INTEGER, currency TEXT,
          original_amount_minor INTEGER, original_currency TEXT,
          external_reference TEXT, payment_method_summary TEXT,
          incurred_at INTEGER, recorded_at INTEGER,
          reimbursement_status TEXT, reimbursed_at INTEGER,
          reimbursement_reference TEXT
        );
        CREATE TABLE repositories (
          key_bi TEXT PRIMARY KEY, data TEXT, is_private INTEGER
        );
        CREATE TABLE contributor_reward_estimates (
          reward_id TEXT PRIMARY KEY, recipient_bi TEXT,
          recipient_name TEXT, repo_owner TEXT, repo_name TEXT,
          source_kind TEXT, subject_number INTEGER, subject_title TEXT,
          subject_summary TEXT,
          amount_lamports INTEGER, condition TEXT, status TEXT,
          created_at INTEGER, updated_at INTEGER, source_address TEXT,
          destination_address TEXT, tx_signature TEXT, submitted_at INTEGER,
          paid_at INTEGER,
          payment_usd_micros INTEGER, payment_sol_usd_micros INTEGER,
          payment_valued_at INTEGER
        );
        CREATE TABLE ledger_backfill_sources (
          source_id TEXT PRIMARY KEY, source_kind TEXT, address TEXT,
          network TEXT, subject_repo_bi TEXT, subject_kind TEXT,
          subject_number INTEGER, before_signature TEXT,
          completed INTEGER, updated_at INTEGER
        );
        CREATE TABLE ledger_backfill_transactions (
          entry_id TEXT PRIMARY KEY, network TEXT, signature TEXT,
          instruction_index INTEGER, block_time INTEGER,
          source_address TEXT, destination_address TEXT,
          amount_lamports INTEGER, observed_address TEXT,
          discovered_from TEXT
        );

        INSERT INTO users VALUES ('alice-bi', 'alice');
        INSERT INTO pending_rewards VALUES (
          'reward-old', 'alice-bi', 'alice', 'source-wallet', 5000000,
          'recipient-wallet', 'completed', 100, 5000,
          'reward-intent', 'reward-signature'
        );
        INSERT INTO chain_intents VALUES (
          'reward-intent', 'pending_reward', 'source-wallet', 'completed',
          '{}', 110, 'reward-signature', 900
        );
        INSERT INTO world_element_purchases VALUES (
          'purchase-1', 'alice-bi', 'lamp', 500000, 'confirmed', 200,
          'purchase-signature', 'alice-wallet', 800, '', 'deposit-wallet',
          495000, 'sweep-signature', 1000
        );
        INSERT INTO ledger_backfill_sources VALUES (
          'legacy:one', 'legacy-central_fund', 'legacy-wallet',
          'mainnet-beta', '', '', 0, '', 1, 1
        );
        INSERT INTO ledger_backfill_transactions VALUES (
          'chain-backfill:mainnet-beta:historic:000', 'mainnet-beta',
          'historic-signature', 0, 700, 'legacy-wallet', 'recipient-wallet',
          125000, 'legacy-wallet', 'legacy:one'
        );
        INSERT INTO ledger_backfill_transactions VALUES (
          'chain-backfill:mainnet-beta:duplicate:000', 'mainnet-beta',
          'reward-signature', 0, 950, 'source-wallet', 'recipient-wallet',
          5000000, 'source-wallet', 'legacy:one'
        );
        """
    )

    async def d1_all(_env, sql, *arguments):
        return [dict(row) for row in database.execute(sql, arguments).fetchall()]

    async def d1_first(_env, sql, *arguments):
        row = database.execute(sql, arguments).fetchone()
        return dict(row) if row else None

    async def quote():
        return {"priceMicros": 100_000_000, "observedAt": 700}

    async def ensure_schema(_env):
        return None

    class Date:
        @staticmethod
        def now():
            return 2_000

    monkeypatch.setattr(ledger, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(ledger, "d1_first", d1_first, raising=False)
    monkeypatch.setattr(ledger, "_sol_usd_quote", quote)
    monkeypatch.setattr(ledger, "ensure_schema", ensure_schema, raising=False)
    monkeypatch.setattr(ledger, "Date", Date, raising=False)
    monkeypatch.setattr(
        ledger, "method_name", lambda request: request.method, raising=False)
    monkeypatch.setattr(
        ledger, "json_response", lambda payload, **_kwargs: payload,
        raising=False)

    try:
        payload = _run(ledger.ledger_handler(
            SimpleNamespace(SOLANA_NETWORK="mainnet-beta"),
            SimpleNamespace(method="GET", url="https://example/api/ledger"),
        ))
        database.executemany(
            "INSERT INTO tip_notice VALUES (?,?,?,?,?,?)",
            [
                ("same-time-%03d" % index, "", "", 1, "mainnet-beta", 1200)
                for index in range(90)
            ],
        )
        paged_tip_ids = []
        before_at = 9_007_199_254_740_991
        before_id = "\uffff"
        while True:
            candidates = _run(ledger._tip_entries(
                None, before_at, before_id, 41, 0))
            candidates = [
                entry for entry in candidates
                if ledger._after_cursor(entry, before_at, before_id)
            ]
            candidates.sort(
                key=lambda entry: (entry["occurredAt"], entry["id"]),
                reverse=True,
            )
            page = candidates[:40]
            paged_tip_ids.extend(entry["id"] for entry in page)
            if len(candidates) <= 40:
                break
            before_at = page[-1]["occurredAt"]
            before_id = page[-1]["id"]
    finally:
        database.close()

    assert [entry["id"] for entry in payload["entries"]] == [
        "world-purchase-sweep:purchase-1",
        "pending-reward:reward-old",
        "world-purchase:purchase-1",
        "chain-backfill:mainnet-beta:historic:000",
    ]
    assert [entry["occurredAt"] for entry in payload["entries"]] == [
        1000, 900, 800, 700]
    assert payload["entries"][0]["amountLamports"] == 495_000
    assert payload["entries"][0]["amountKnown"] is True
    assert payload["entries"][-1]["amountLamports"] == 125_000
    assert not any(
        entry["id"] == "chain-backfill:mainnet-beta:duplicate:000"
        for entry in payload["entries"])
    assert len(paged_tip_ids) == 90
    assert len(set(paged_tip_ids)) == 90
    assert paged_tip_ids == sorted(paged_tip_ids, reverse=True)


def test_handler_rejects_mutation_methods_without_touching_schema(monkeypatch):
    called = False

    async def ensure_schema(_env):
        nonlocal called
        called = True

    monkeypatch.setattr(ledger, "ensure_schema", ensure_schema, raising=False)
    monkeypatch.setattr(
        ledger, "method_name", lambda request: request.method, raising=False)
    monkeypatch.setattr(
        ledger,
        "json_response",
        lambda payload, **kwargs: (payload, kwargs),
        raising=False,
    )
    payload, options = _run(ledger.ledger_handler(
        object(), SimpleNamespace(method="POST", url="https://example/")))
    assert payload == {"error": "method_not_allowed"}
    assert options["status"] == 405
    assert options["extra_headers"] == {"allow": "GET"}
    assert called is False


def test_contributor_payout_feed_requires_and_applies_owner_scope(monkeypatch):
    rewards = [
        {
            "reward_id": "reward-abcdefgh",
            "recipient_bi": "alice-bi",
            "recipient_name": "alice",
            "repo_owner": "forkmesh",
            "repo_name": "forkmesh",
            "source_kind": "issue",
            "subject_number": 42,
            "subject_title": "A valid report",
            "amount_lamports": 3_000_000,
            "condition": "accepted_report",
            "status": "ready",
            "created_at": 100,
            "updated_at": 100,
            "destination_address": "",
            "tx_signature": "",
        },
        {
            "reward_id": "reward-elsewhere",
            "recipient_bi": "bob-bi",
            "recipient_name": "bob",
            "repo_owner": "another",
            "repo_name": "project",
            "source_kind": "pull",
            "subject_number": 7,
            "subject_title": "Merged work",
            "amount_lamports": 5_000_000,
            "condition": "merged",
            "status": "ready",
            "created_at": 90,
            "updated_at": 90,
            "destination_address": "",
            "tx_signature": "",
        },
    ]

    async def d1_all(_env, sql, *arguments):
        if "FROM contributor_reward_estimates" in sql:
            assert "lower(repo_owner)=?" in sql
            assert arguments == ("forkmesh", "forkmesh")
            return [rewards[0]]
        if "FROM users" in sql:
            return [{"user_bi": "alice-bi", "data": "alice-record"}]
        raise AssertionError(sql)

    async def session(_env, _request):
        return "owner-bi", {"name": "owner-user"}

    async def org_row(_env, _owner):
        return "org-bi", None

    async def owns_node(_env, actor, owner):
        return actor == "owner-user" and owner == "forkmesh"

    async def decrypt(_env, data):
        assert data == "alice-record"
        return {
            "solana": "1" * 32,
            "discordUsername": "alice.reports",
            "discordDisplayName": "Alice Reports",
        }

    async def ensure_schema(_env):
        return None

    def response(payload, status=200, **_options):
        return {"status": status, "body": payload}

    monkeypatch.setattr(ledger, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(ledger, "_account_session_record", session,
                        raising=False)
    monkeypatch.setattr(ledger, "_org_row", org_row, raising=False)
    monkeypatch.setattr(ledger, "_account_owns_node", owns_node,
                        raising=False)
    monkeypatch.setattr(ledger, "decrypt_row", decrypt, raising=False)
    monkeypatch.setattr(ledger, "ensure_schema", ensure_schema, raising=False)
    monkeypatch.setattr(
        ledger, "method_name", lambda request: request.method, raising=False)
    monkeypatch.setattr(ledger, "json_response", response, raising=False)

    result = _run(ledger.contributor_reward_estimates_handler(
        object(), SimpleNamespace(
            method="GET",
            url=("https://example/api/rewards/estimates?ownerScope=1"
                 "&repository=forkmesh%2Fforkmesh"),
        )))
    assert result["status"] == 200
    assert result["body"]["authorization"] == "owner-session"
    assert result["body"]["repository"] == "forkmesh/forkmesh"
    assert [item["rewardId"] for item in result["body"]["rewards"]] == [
        "reward-abcdefgh"
    ]
    assert result["body"]["rewards"][0]["recipientAddress"] == "1" * 32
    assert result["body"]["rewards"][0]["recipientDiscordUsername"] == (
        "alice.reports")
    assert result["body"]["rewards"][0]["recipientDiscordDisplayName"] == (
        "Alice Reports")

    async def unauthorized(_env, _actor, _owner):
        return False

    monkeypatch.setattr(ledger, "_account_owns_node", unauthorized,
                        raising=False)
    denied = _run(ledger.contributor_reward_estimates_handler(
        object(), SimpleNamespace(
            method="GET",
            url=("https://example/api/rewards/estimates?ownerScope=1"
                 "&repository=forkmesh%2Fforkmesh"),
        )))
    assert denied == {
        "status": 403,
        "body": {"error": "reward_owner_required"},
    }


def test_initial_ledger_cursor_binds_as_a_javascript_number(monkeypatch):
    cutoffs = []

    async def source(_env, cutoff, *_arguments):
        cutoffs.append(cutoff)
        return []

    async def quote():
        return {"priceMicros": 0, "observedAt": 0}

    async def backfill(_env):
        return {}

    async def expected(_env, _now):
        return []

    async def ensure_schema(_env):
        return None

    class Date:
        @staticmethod
        def now():
            return 1_000

    for name in (
        "_tip_entries",
        "_contribution_entries",
        "_reward_entries",
        "_chain_entries",
        "_microjob_entries",
        "_contributor_reward_entries",
        "_world_purchase_entries",
        "_advertising_entries",
        "_operating_expense_entries",
        "_backfill_entries",
    ):
        monkeypatch.setattr(ledger, name, source)
    monkeypatch.setattr(ledger, "_expected_expenses", expected)
    monkeypatch.setattr(ledger, "_sol_usd_quote", quote)
    monkeypatch.setattr(ledger, "_backfill_status", backfill)
    monkeypatch.setattr(ledger, "ensure_schema", ensure_schema, raising=False)
    monkeypatch.setattr(ledger, "Date", Date, raising=False)
    monkeypatch.setattr(
        ledger, "method_name", lambda request: request.method, raising=False)
    monkeypatch.setattr(
        ledger, "json_response", lambda payload, **_kwargs: payload,
        raising=False)

    payload = _run(ledger.ledger_handler(
        SimpleNamespace(SOLANA_NETWORK="mainnet-beta"),
        SimpleNamespace(method="GET", url="https://example/api/ledger"),
    ))

    assert payload["ok"] is True
    assert cutoffs
    assert all(type(cutoff) is float for cutoff in cutoffs)
    assert cutoffs[0] == float(9_007_199_254_740_991)


def _wallet_only_handler_payload(monkeypatch, env, reads):
    """Run ledger_handler with every record source stubbed empty."""

    async def source(_env, _cutoff, *_arguments):
        return []

    async def quote():
        return {"priceMicros": 0, "observedAt": 0}

    async def backfill(_env):
        return {}

    async def expected(_env, _now):
        return []

    async def ensure_schema(_env):
        return None

    async def display_balance(_env, address, force=False, budget=None,
                              reader=None):
        reads.append((address, force, reader))
        if budget is not None:
            budget["left"] = int(budget["left"]) - 1
        return (1_500_000_000, 1_700_000_000_000) if force else (None, 0)

    class Date:
        @staticmethod
        def now():
            return 1_000

    for name in (
        "_tip_entries",
        "_contribution_entries",
        "_reward_entries",
        "_chain_entries",
        "_microjob_entries",
        "_contributor_reward_entries",
        "_world_purchase_entries",
        "_advertising_entries",
        "_operating_expense_entries",
        "_backfill_entries",
    ):
        monkeypatch.setattr(ledger, name, source)
    monkeypatch.setattr(ledger, "_expected_expenses", expected)
    monkeypatch.setattr(ledger, "_sol_usd_quote", quote)
    monkeypatch.setattr(ledger, "_backfill_status", backfill)
    monkeypatch.setattr(ledger, "ensure_schema", ensure_schema, raising=False)
    monkeypatch.setattr(ledger, "Date", Date, raising=False)
    monkeypatch.setattr(
        ledger, "_display_balance_lamports", display_balance, raising=False)
    monkeypatch.setattr(
        ledger, "_reward_balance_lamports", "reward-reader", raising=False)
    monkeypatch.setattr(
        ledger, "method_name", lambda request: request.method, raising=False)
    monkeypatch.setattr(
        ledger, "json_response", lambda payload, **_kwargs: payload,
        raising=False)
    return _run(ledger.ledger_handler(env, SimpleNamespace(
        method="GET", url="https://example/api/ledger?refresh=1")))


def test_public_ledger_names_the_system_wallets_it_moves_money_through(
        monkeypatch):
    pool = "6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj"
    treasury = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM"
    reads = []
    payload = _wallet_only_handler_payload(
        monkeypatch,
        SimpleNamespace(
            SOLANA_NETWORK="devnet",
            COMMUNITY_REWARD_NETWORK="devnet",
            COMMUNITY_REWARD_POOL_ADDRESS=pool,
            TREASURY_SOLANA_ADDRESS=treasury,
        ),
        reads,
    )

    wallets = payload["wallets"]
    assert [wallet["key"] for wallet in wallets] == [
        "community-reward-pool", "treasury"]
    assert [wallet["address"] for wallet in wallets] == [pool, treasury]
    assert all(not wallet["sharedAddress"] for wallet in wallets)
    assert wallets[0]["balanceSol"] == "1.5"
    assert wallets[0]["balanceLamports"] == 1_500_000_000
    assert wallets[0]["balanceKnown"] is True
    assert wallets[0]["balanceCheckedAt"] == 1_700_000_000_000
    assert wallets[0]["explorerUrl"].endswith("?cluster=devnet")
    # A public page never signs, and never claims the Worker holds the key.
    assert wallets[0]["custody"] == "external-owner-device-signer"
    # One read per named address, through the network-pinned community reader.
    assert [address for address, _force, _reader in reads] == [pool, treasury]
    assert all(reader == "reward-reader" for _a, _f, reader in reads)
    assert all(force for _a, force, _r in reads)


def test_one_configured_address_serving_both_roles_says_so(monkeypatch):
    treasury = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM"
    reads = []
    payload = _wallet_only_handler_payload(
        monkeypatch,
        SimpleNamespace(
            SOLANA_NETWORK="mainnet-beta",
            TREASURY_SOLANA_ADDRESS=treasury,
        ),
        reads,
    )

    wallets = payload["wallets"]
    # Both names resolve the way the money paths themselves resolve them, and
    # the rows say they are one address so nobody adds a balance to itself.
    assert [wallet["address"] for wallet in wallets] == [treasury, treasury]
    assert all(wallet["sharedAddress"] for wallet in wallets)


def test_a_host_with_no_configured_system_wallet_reads_no_balance(monkeypatch):
    reads = []
    payload = _wallet_only_handler_payload(
        monkeypatch, SimpleNamespace(SOLANA_NETWORK="mainnet-beta"), reads)

    assert payload["wallets"] == []
    assert reads == []


def test_ledger_page_is_public_discoverable_and_cache_busted():
    page = (PUBLIC / "ledger.html").read_text(encoding="utf-8")
    client = (PUBLIC / "ledger.js").read_text(encoding="utf-8")
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    routes = (SRC / "static_routes.py").read_text(encoding="utf-8")
    wrangler = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
    header = (REPOSITORY / "www/public/site-header.js").read_text(
        encoding="utf-8")
    footer = (REPOSITORY / "www/public/site-footer.js").read_text(
        encoding="utf-8")
    www_worker = (REPOSITORY / "www/worker.js").read_text(encoding="utf-8")
    world_worker = (REPOSITORY / "world/worker.js").read_text(encoding="utf-8")

    for marker in (
        "data-ledger-rows",
        "data-ledger-search",
        "data-ledger-load-more",
        "data-ledger-backfill-note",
        'data-ledger-view="expenses"',
        'data-ledger-view="income"',
        'data-ledger-view="debt"',
        'data-ledger-view="paid"',
        "data-ledger-expense-count",
        "data-ledger-income-count",
        "data-ledger-debt-count",
        "data-ledger-paid-count",
        "data-ledger-expenses-total",
        "data-ledger-income-total",
        "data-ledger-debt-total",
        "Record &amp; linked subjects",
        "Transaction ID",
        'src="/ledger.js?v=${version}"',
        '<div data-forkmesh-header="simple"></div>',
        '<div data-forkmesh-footer="standard"></div>',
    ):
        assert marker in page
    assert 'fetch(`/api/ledger?${parameters}`' in client
    assert 'fetch("/api/rewards/estimates"' in client
    assert "function contributorRewardEntry(reward)" in client
    assert "function isPaidPayment(entry)" in client
    assert "function isAccountingExpense(entry)" in client
    assert "function isAccountingIncome(entry)" in client
    assert "function isAccountingDebt(entry)" in client
    assert "function formatAccountingTotal(total)" in client
    assert "total.unvaluedLamports += exactInteger(entry?.amountLamports)" in client
    assert 'parts.push(`${solFromLamports(total.unvaluedLamports)} SOL`)' in client
    assert 'entry?.verification === "finalized-exact-work-payment"' in client
    assert 'new URLSearchParams({ limit: "100" })' in client
    assert "row.dataset.accountingExpense" in client
    assert "row.dataset.accountingIncome" in client
    assert "row.dataset.accountingDebt" in client
    assert "row.dataset.paidPayment" in client
    assert 'state.view === "expenses"' in client
    assert 'state.view === "income"' in client
    assert 'state.view === "debt"' in client
    assert 'state.view === "paid"' in client
    assert 'setText("[data-ledger-expense-count]", expenses.toLocaleString())' in client
    assert 'setText("[data-ledger-income-count]", income.toLocaleString())' in client
    assert 'setText("[data-ledger-debt-count]", debt.toLocaleString())' in client
    assert 'setText("[data-ledger-paid-count]", paid.toLocaleString())' in client
    assert '`contributor-reward:${rewardId}`' in client
    assert "title: subjectTitle" in client
    assert 'className = href\n          ? "ledger-subject-title"' in client
    assert "link?.title" in client
    assert ".ledger-subject-title" in page
    assert '"ready"' in client
    assert '"historical-chain-transfer"' in client
    assert "signature.textContent = String(entry.transactionId)" in client
    assert "link?.type" in client and "link?.role" in client
    assert "function makeUserAvatar(name)" in client
    assert "function loadAvatarPortrait(name)" in client
    assert "ledger-link-person" in client
    assert ".ledger-avatar" in page
    # Once the canonical paid row arrives — on the first page or an older one —
    # it replaces the estimate projection so the issue reward stays one rich
    # record with its sender and verified receipt.
    assert "state.entries.filter((entry) => !settled.has" in client
    assert '"/api/ledger"' in runtime
    assert '"ledger"' in routes and '"/ledger.html"' in routes
    assert '"!/ledger/"' in wrangler
    assert 'href="/ledger"' in header
    assert '{ href: "/ledger", label: "Financial ledger", plugin: "ledger" }' in footer
    assert '"/ledger"' in www_worker
    assert '"/ledger"' not in world_worker
    assert "return env.APP.fetch(request);" in world_worker


def test_ledger_page_tops_the_records_with_wallet_balances():
    page = (PUBLIC / "ledger.html").read_text(encoding="utf-8")
    client = (PUBLIC / "ledger.js").read_text(encoding="utf-8")

    # The strip sits above the loaded-page summary, so the first thing a
    # reader sees is what the wallets hold, not what this page happens to
    # have loaded.
    assert page.index("data-ledger-wallets") < page.index(
        "data-ledger-expenses-total")
    assert ".ledger-wallet-amount" in page
    # System wallets ride the ledger response the page already fetches.
    assert "state.wallets = Array.isArray(payload.wallets)" in client
    # The reader's own published payout address is the one wallet a cached
    # public page cannot name, so the client resolves it from the session.
    assert '"forkmesh.session"' in client
    assert "/api/accounts/${encodeURIComponent(account)}" in client
    assert "/api/world/wallet?address=" in client
    # Only an explicit Refresh may cost an on-chain read.
    assert 'if (refresh) parameters.set("refresh", "1")' in client
    assert "void load({ refresh: true })" in client


def test_backfill_enrolls_only_public_system_addresses():
    source = (SRC / "ledger.py").read_text(encoding="utf-8")
    cron = (SRC / "entry.py").read_text(encoding="utf-8")
    migration = (
        ROOT / "migrations/0144_ledger_chain_backfill.sql"
    ).read_text(encoding="utf-8")

    for marker in (
        "legacy_custody_reconciliation",
        "world_element_purchases",
        "COMMUNITY_REWARD_POOL_ADDRESS",
        '"getSignaturesForAddress"',
        '"getTransaction"',
        '"memberWalletHistoriesScanned": False',
    ):
        assert marker in source
    assert "wallet_balances" not in source
    assert '"ledger-backfill"' in cron
    assert "ledger_backfill_sources" in migration
    assert "ledger_backfill_transactions" in migration


def test_paid_claim_and_subject_number_share_one_durable_record():
    microjobs = (SRC / "microjobs.py").read_text(encoding="utf-8")
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    migration = (ROOT / "migrations/0143_world_sweep_amount.sql").read_text(
        encoding="utf-8")

    assert '"number": filed_number' in microjobs
    assert '"issueNumber": filed_number' in microjobs
    assert "return number" in entry.split(
        "async def file_issue(self, owner, repo, submission):", 1)[1].split(
            "async def load_pull_record", 1)[0]
    assert (
        "UPDATE microjob_claims SET status='paid', tx_signature=?" in entry
    )
    assert "sweep_lamports=?, sweep_error=''" in entry
    assert "ADD COLUMN sweep_lamports" in migration


_REMINDER_NOW_MS = 1_800_000_000_000


def _reminder_runtime(monkeypatch, *, reward, account, previous=None,
                      waiting=(2, 7_000_000), owns=True):
    """Wire the wallet-reminder handler to in-memory stand-ins.

    Returns the mutable record of everything it tried to do, so a test can
    assert on what was mailed, notified, audited and written.
    """
    seen = {"emails": [], "notifications": [], "audits": [], "writes": [],
            "recorded": []}

    async def d1_first(_env, sql, *arguments):
        if "FROM contributor_reward_estimates WHERE reward_id=?" in sql:
            return dict(reward) if reward else None
        if "FROM reward_wallet_reminders" in sql:
            return dict(previous) if previous else None
        if "COUNT(*) AS n" in sql:
            return {"n": waiting[0], "lamports": waiting[1]}
        raise AssertionError(sql)

    async def d1_run(_env, sql, *arguments):
        seen["writes"].append((sql, arguments))

    async def session(_env, _request, _data=None):
        return "owner-bi", {"name": "owner-user"}

    async def org_row(_env, _owner):
        return "org-bi", None

    async def owns_node(_env, actor, owner):
        return owns and actor == "owner-user" and owner == "forkmesh"

    async def account_row(_env, name):
        return ("recipient-bi", dict(account)) if account else ("", None)

    async def send_email(_env, to_email, subject, text, html=None, **kwargs):
        seen["emails"].append({"to": to_email, "subject": subject,
                               "text": text, "html": html,
                               "kind": kwargs.get("email_kind")})
        return True

    async def record_account_email(_env, name, kind, ok):
        seen["recorded"].append((name, kind, ok))

    async def notify(_env, recipient, kind, title, **kwargs):
        seen["notifications"].append((recipient, kind, title, kwargs))
        return True

    async def audit(_env, actor, action, target_type="", target="",
                    outcome="success", details=None):
        seen["audits"].append((actor, action, target, outcome, details))

    async def ensure_schema(_env):
        return None

    async def body(_request, *_a, **_k):
        return {"rewardId": "reward-abcdefgh"}

    async def blind(_env, value):
        return value + "-bi"

    class Date:
        @staticmethod
        def now():
            return _REMINDER_NOW_MS

    def response(payload, status=200, **options):
        return {"status": status, "body": payload, "options": options}

    for name, value in (
        ("d1_first", d1_first), ("d1_run", d1_run), ("blind_index", blind),
        ("_account_session_record", session), ("_org_row", org_row),
        ("_account_owns_node", owns_node), ("_account_row", account_row),
        ("_send_email", send_email),
        ("_record_account_email", record_account_email),
        ("enqueue_notification", notify), ("_audit_sensitive_action", audit),
        ("ensure_schema", ensure_schema),
        ("bounded_json_request", body), ("Date", Date),
        ("json_response", response),
        ("notification_email_enabled", lambda rec, kind: True),
        ("_html_escape", lambda text: str(text)),
        ("_forkmesh_email_card_html",
         lambda *args, **kwargs: "<html>%s</html>" % args[0]),
        ("FORKMESH_SITE_URL", "https://forkmesh.com/"),
        ("method_name", lambda request: request.method),
    ):
        monkeypatch.setattr(ledger, name, value, raising=False)
    return seen


_PENDING_WALLET_REWARD = {
    "reward_id": "reward-abcdefgh",
    "recipient_bi": "alice-bi",
    "recipient_name": "alice",
    "repo_owner": "forkmesh",
    "repo_name": "forkmesh",
    "status": "pending_wallet",
    "tx_signature": "",
}
_NO_WALLET_ACCOUNT = {
    "name": "alice",
    "email": "alice@example.com",
    "email_verified": True,
    "solana": "",
}


def _post_reminder():
    return _run(ledger.contributor_reward_wallet_reminder_handler(
        object(),
        SimpleNamespace(method="POST",
                        url="https://example/api/rewards/wallet-reminder")))


def test_wallet_reminder_mails_the_recipient_and_claims_the_window(monkeypatch):
    seen = _reminder_runtime(
        monkeypatch, reward=_PENDING_WALLET_REWARD,
        account=_NO_WALLET_ACCOUNT)
    result = _post_reminder()

    assert result["status"] == 200
    assert result["body"]["ok"] is True
    assert result["body"]["emailed"] is True
    assert result["body"]["recipient"] == "alice"
    assert result["body"]["pendingCount"] == 2
    assert [email["to"] for email in seen["emails"]] == ["alice@example.com"]
    assert "payout address" in seen["emails"][0]["text"]
    assert seen["emails"][0]["kind"] == "reward_wallet_reminder"
    assert seen["recorded"] == [("alice", "reward_wallet_reminder", True)]
    # The window is claimed before the send, so a crash mid-mail cannot
    # double-send, and it is keyed on the person rather than on the reward.
    assert len(seen["writes"]) == 1
    sql, arguments = seen["writes"][0]
    assert "INSERT INTO reward_wallet_reminders" in sql
    assert arguments[0] == "alice-bi" and arguments[1] == "alice"
    assert [note[0] for note in seen["notifications"]] == ["alice"]
    assert seen["notifications"][0][1] == "pending_reward"
    assert seen["audits"][0][1] == "reward.wallet_reminder"


def test_wallet_reminder_refuses_a_non_owner(monkeypatch):
    seen = _reminder_runtime(
        monkeypatch, reward=_PENDING_WALLET_REWARD,
        account=_NO_WALLET_ACCOUNT, owns=False)
    result = _post_reminder()

    assert result["status"] == 403
    assert result["body"] == {"error": "reward_owner_required"}
    assert seen["emails"] == [] and seen["writes"] == []


def test_wallet_reminder_refuses_a_recipient_who_already_published_one(
        monkeypatch):
    account = dict(_NO_WALLET_ACCOUNT,
                   solana="6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj")
    seen = _reminder_runtime(
        monkeypatch, reward=_PENDING_WALLET_REWARD, account=account)
    result = _post_reminder()

    assert result["status"] == 409
    assert result["body"]["error"] == "wallet_already_published"
    assert seen["emails"] == [] and seen["writes"] == []


def test_wallet_reminder_refuses_a_reward_that_is_no_longer_waiting(
        monkeypatch):
    reward = dict(_PENDING_WALLET_REWARD, status="ready")
    seen = _reminder_runtime(
        monkeypatch, reward=reward, account=_NO_WALLET_ACCOUNT)
    result = _post_reminder()

    assert result["status"] == 409
    assert result["body"] == {"error": "reward_not_awaiting_wallet"}
    assert seen["emails"] == []


def test_wallet_reminder_rate_limits_per_recipient_not_per_reward(monkeypatch):
    recent = (_REMINDER_NOW_MS - ledger.REWARD_WALLET_REMINDER_INTERVAL_MS
              + 5_000)
    seen = _reminder_runtime(
        monkeypatch, reward=_PENDING_WALLET_REWARD,
        account=_NO_WALLET_ACCOUNT, previous={"sent_at": recent})
    result = _post_reminder()

    assert result["status"] == 429
    assert result["body"]["error"] == "reminder_too_soon"
    assert result["body"]["retryAfterMs"] == 5_000
    assert result["options"]["extra_headers"]["retry-after"] == "5"
    assert seen["emails"] == [] and seen["writes"] == []


def test_wallet_reminder_still_notifies_without_a_verified_email(monkeypatch):
    account = dict(_NO_WALLET_ACCOUNT, email_verified=False)
    seen = _reminder_runtime(
        monkeypatch, reward=_PENDING_WALLET_REWARD, account=account)
    result = _post_reminder()

    assert result["status"] == 200
    assert result["body"]["emailed"] is False
    assert seen["emails"] == []
    assert [note[0] for note in seen["notifications"]] == ["alice"]


def test_wallet_reminder_rejects_reads_and_unknown_reward_ids(monkeypatch):
    _reminder_runtime(monkeypatch, reward=_PENDING_WALLET_REWARD,
                      account=_NO_WALLET_ACCOUNT)
    read = _run(ledger.contributor_reward_wallet_reminder_handler(
        object(),
        SimpleNamespace(method="GET",
                        url="https://example/api/rewards/wallet-reminder")))
    assert read["status"] == 405
    assert read["options"]["extra_headers"] == {"allow": "POST"}

    async def bad_body(_request, *_a, **_k):
        return {"rewardId": "../../etc/passwd"}

    monkeypatch.setattr(ledger, "bounded_json_request", bad_body,
                        raising=False)
    assert _post_reminder()["status"] == 400


def test_contributor_reward_entries_name_the_issue_they_pay_for(monkeypatch):
    rows = [{
        "reward_id": "reward-abcdefgh",
        "recipient_bi": "alice-bi",
        "recipient_name": "alice",
        "repo_owner": "forkmesh",
        "repo_name": "forkmesh",
        "source_kind": "issue",
        "subject_number": 628,
        "subject_title": "Ledger rows should name their subject",
        "subject_summary": (
            "The autocomplete selection looked successful but never inserted "
            "the chosen username."),
        "amount_lamports": 4_000_000,
        "condition": "accepted_report",
        "status": "pending_wallet",
        "occurred_at": 500,
        "created_at": 500,
        "source_address": "",
        "destination_address": "",
        "tx_signature": "",
    }]

    async def d1_all(_env, sql, *arguments):
        if "FROM contributor_reward_estimates AS rewards" in sql:
            assert "rewards.recipient_bi" in sql
            return rows
        if "FROM users" in sql:
            assert arguments == ("alice-bi",)
            return [{"user_bi": "alice-bi", "data": "alice-record"}]
        raise AssertionError(sql)

    async def decrypt(_env, data):
        assert data == "alice-record"
        return {
            "solana": "1" * 32,
            "discordUsername": "alice.reports",
            "discordDisplayName": "Alice Reports",
        }

    monkeypatch.setattr(ledger, "d1_all", d1_all, raising=False)
    monkeypatch.setattr(ledger, "decrypt_row", decrypt, raising=False)
    entries = _run(ledger._contributor_reward_entries(
        object(), 1_000, "~", 40, "mainnet-beta", 0))

    assert entries[0]["status"] == "ready"
    recipient_wallet = next(
        link for link in entries[0]["links"]
        if link.get("role") == "recipient" and link.get("type") == "wallet")
    assert recipient_wallet["key"] == "1" * 32
    recipient = next(
        link for link in entries[0]["links"]
        if link.get("role") == "recipient" and link.get("type") == "user")
    assert recipient["discordUsername"] == "alice.reports"
    assert recipient["discordDisplayName"] == "Alice Reports"
    subject = next(link for link in entries[0]["links"]
                   if link.get("role") == "subject")
    assert subject["label"] == "forkmesh/forkmesh issue #628"
    assert subject["title"] == "Ledger rows should name their subject"
    assert subject["summary"].startswith("The autocomplete selection")
    assert subject["url"] == "/forkmesh/forkmesh/issues/628"

    # A record the relay has no headline for carries no empty title field.
    rows[0]["subject_title"] = ""
    untitled = _run(ledger._contributor_reward_entries(
        object(), 1_000, "~", 40, "mainnet-beta", 0))
    assert all("title" not in link for link in untitled[0]["links"])


def test_a_verified_work_payment_keeps_its_task_link_and_entered_usd(
        monkeypatch):
    async def d1_all(_env, _sql, *_arguments):
        return [{
            "signature": "work-payment-signature",
            "from_bi": "alice-bi",
            "to_bi": "bob-bi",
            "lamports": 50_000_000,
            "network": "mainnet",
            "ts": 1_200,
            "task_description": "  Build   the payment dialog  ",
            "output_url": "https://github.com/forkmesh/forkmesh/pull/777",
            "entered_currency": "USD",
            "entered_usd_micros": 5_000_000,
            "sol_usd_price_micros": 100_000_000,
            "price_observed_at": 1_100,
            "sender_name": "alice",
            "recipient_name": "bob",
        }]

    monkeypatch.setattr(ledger, "d1_all", d1_all, raising=False)
    entries = _run(ledger._tip_entries(
        object(), 9_000, "￿", 40, 250_000_000))

    assert len(entries) == 1
    payment = entries[0]
    assert payment["title"] == "Task payment"
    assert payment["amountSol"] == "0.05"
    assert payment["amountUsd"] == "5.000000"
    assert payment["valuationBasis"] == "recorded-at-payment-sol-usd"
    assert payment["valuationObservedAt"] == 1_100
    assert payment["verification"] == "finalized-exact-work-payment"
    assert "verified on Solana" in payment["note"]
    work = next(link for link in payment["links"]
                if link["type"] == "work-product")
    assert work["title"] == "Build the payment dialog"
    assert work["url"] == "https://github.com/forkmesh/forkmesh/pull/777"


def test_a_paid_reward_is_one_record_that_names_both_sides(monkeypatch):
    """A reward payout listed twice was one transfer, counted twice.

    Paying an owner-authored estimate signs a member-tip transfer and reports
    it to the tip-notice endpoint, so the same signature reaches both tables.
    The estimate owns the transfer's state and names the issue it paid for, so
    it is the record that survives; the notice's own row is suppressed rather
    than published beside it as a "Member-to-member tip".
    """
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript(
        """
        CREATE TABLE users (user_bi TEXT PRIMARY KEY, username TEXT);
        CREATE TABLE tip_notice (
          signature TEXT PRIMARY KEY, from_bi TEXT, to_bi TEXT,
          lamports INTEGER, network TEXT, ts INTEGER, emailed INTEGER
        );
        CREATE TABLE contributor_reward_estimates (
          reward_id TEXT PRIMARY KEY, recipient_bi TEXT,
          recipient_name TEXT, repo_owner TEXT, repo_name TEXT,
          source_kind TEXT, subject_number INTEGER, subject_title TEXT,
          subject_summary TEXT,
          amount_lamports INTEGER, condition TEXT, status TEXT,
          created_at INTEGER, updated_at INTEGER, source_address TEXT,
          destination_address TEXT, tx_signature TEXT, submitted_at INTEGER,
          paid_at INTEGER,
          payment_usd_micros INTEGER, payment_sol_usd_micros INTEGER,
          payment_valued_at INTEGER
        );

        INSERT INTO users VALUES ('owner-bi', 'jett');
        INSERT INTO users VALUES ('reporter-bi', 'md-mehboob');
        INSERT INTO tip_notice VALUES (
          'reward-signature', 'owner-bi', 'reporter-bi', 2000000,
          'mainnet', 1200, 1
        );
        INSERT INTO tip_notice VALUES (
          'plain-tip-signature', 'owner-bi', 'reporter-bi', 500000,
          'mainnet', 1100, 1
        );
        INSERT INTO contributor_reward_estimates VALUES (
          'reward-forkmesh-forkmesh-issue-627', 'reporter-bi', 'md-mehboob',
          'forkmesh', 'forkmesh', 'issue', 627,
          'Clicking an @mention autocomplete result does not insert the username',
          'Selecting a suggestion failed to insert the username.', 2000000,
          'accepted_report', 'paid', 1000, 1200, 'owner-wallet',
          'reporter-wallet', 'reward-signature', 0, 1200, 250000, 125000000,
          1200
        );
        """
    )

    async def d1_all(_env, sql, *arguments):
        return [dict(row) for row in database.execute(sql, arguments).fetchall()]

    monkeypatch.setattr(ledger, "d1_all", d1_all, raising=False)
    try:
        tips = _run(ledger._tip_entries(None, 9_000, "￿", 40, 0))
        rewards = _run(ledger._contributor_reward_entries(
            None, 9_000, "￿", 40, "mainnet-beta", 0))

        # The reward's own notice is gone; an ordinary tip still publishes.
        assert [entry["id"] for entry in tips] == ["tip:plain-tip-signature"]

        reward = rewards[0]
        assert reward["title"] == "Issue report reward"
        assert reward["transactionId"] == "reward-signature"
        assert reward["amountUsd"] == "0.250000"
        assert reward["valuationBasis"] == "recorded-at-payment-sol-usd"
        assert next(link for link in reward["links"]
                    if link.get("role") == "subject")["summary"].startswith(
                        "Selecting a suggestion")
        # Both sides of the payment, on the one row that publishes it.
        assert [(link["role"], link["key"]) for link in reward["links"]
                if link["type"] == "user"] == [
            ("sender", "jett"), ("recipient", "md-mehboob")]

        # A cancelled estimate publishes no record, so it must not silence the
        # notice that is the only remaining account of the transfer.
        database.execute(
            "UPDATE contributor_reward_estimates SET status='cancelled'")
        restored = _run(ledger._tip_entries(None, 9_000, "￿", 40, 0))
        assert [entry["id"] for entry in restored] == [
            "tip:reward-signature", "tip:plain-tip-signature"]
    finally:
        database.close()


# --- landed pulls leave "pays on merge" ---------------------------------------

_MERGE_NOW_MS = 1_800_000_000_000


def _pull_reward_row(number, **overrides):
    row = {
        "reward_id": "reward-forkmesh-forkmesh-pull-%d" % number,
        "recipient_bi": "gaurav-bi",
        "recipient_name": "gaurav2631",
        "repo_owner": "forkmesh",
        "repo_name": "forkmesh",
        "source_kind": "pull",
        "subject_number": number,
        "subject_title": "Pull %d" % number,
        "subject_summary": "",
        "amount_lamports": 20_000_000,
        "condition": "merged",
        "status": "pending_merge",
        "created_at": 10,
        "updated_at": 10,
        "destination_address": "",
        "tx_signature": "",
        "payment_usd_micros": 0,
        "payment_sol_usd_micros": 0,
        "payment_valued_at": 0,
    }
    row.update(overrides)
    return row


def _pull_record_blob(number, status="merged", reward_id=None):
    reward_id = (
        "reward-forkmesh-forkmesh-pull-%d" % number
        if reward_id is None else reward_id)
    header = ["---", "number: %d" % number, "status: %s" % status]
    if reward_id:
        header.append("rewardId: %s" % reward_id)
    return {"ok": True, "content": "\n".join(header + ["---", ""])}


def _merge_heal_runtime(monkeypatch, records):
    """Stub the gateway read and the D1 write the merge heal performs."""
    seen = {"reads": [], "writes": []}

    async def gateway(_env, owner, repo, action):
        seen["reads"].append((owner, repo, action))
        return records.get(action)

    def blob_text(blob):
        return blob.get("content", "") if isinstance(blob, dict) else ""

    async def d1_run(_env, sql, *arguments):
        seen["writes"].append((sql, arguments))

    class Date:
        @staticmethod
        def now():
            return _MERGE_NOW_MS

    monkeypatch.setattr(ledger, "_forkbot_repo_gateway_json", gateway,
                        raising=False)
    monkeypatch.setattr(ledger, "_forkbot_blob_text", blob_text,
                        raising=False)
    monkeypatch.setattr(ledger, "d1_run", d1_run, raising=False)
    monkeypatch.setattr(ledger, "Date", Date, raising=False)
    monkeypatch.setattr(ledger, "_merged_pull_reward_misses", {},
                        raising=False)
    return seen


def _pull_action(number):
    return ("blob?path=.forkmesh%%2Fpulls%%2F%d%%2Fpull.md"
            "&ref=forkmesh%%2Fpull%%2F%d" % (number, number))


def test_a_landed_pull_estimate_is_verified_before_leaving_pending_merge(
        monkeypatch):
    seen = _merge_heal_runtime(monkeypatch, {
        _pull_action(14): _pull_record_blob(14),
        # The number was reused by a pull that merged without this reward.
        _pull_action(40): _pull_record_blob(40, reward_id=""),
        _pull_action(50): _pull_record_blob(50, status="open"),
        _pull_action(60): _pull_record_blob(60),
    })
    rows = [
        _pull_reward_row(14),
        _pull_reward_row(40),
        _pull_reward_row(50),
        _pull_reward_row(60),
        _pull_reward_row(70, status="pending_wallet"),
        _pull_reward_row(80, source_kind="issue", condition="accepted_report"),
    ]

    assert _run(ledger._settle_merged_pull_rewards(object(), rows)) == 2
    # Each check reads the pull's own branch, and only two run per request.
    assert [read[2] for read in seen["reads"]] == [
        _pull_action(14), _pull_action(40)]
    assert [row["status"] for row in rows[:4]] == [
        "pending_wallet", "pending_merge", "pending_merge", "pending_merge"]
    assert rows[0]["updated_at"] == _MERGE_NOW_MS
    assert len(seen["writes"]) == 1
    sql, arguments = seen["writes"][0]
    assert "SET status='pending_wallet'" in sql
    assert "status='pending_merge'" in sql and "tx_signature=''" in sql
    assert arguments == (_MERGE_NOW_MS, "reward-forkmesh-forkmesh-pull-14")

    # The next read skips the pull it just found unmerged and moves on.
    _run(ledger._settle_merged_pull_rewards(object(), rows))
    assert [read[2] for read in seen["reads"][2:]] == [
        _pull_action(50), _pull_action(60)]
    assert rows[3]["status"] == "pending_wallet"


def _pull_action_at(number, ref):
    return ("blob?path=.forkmesh%%2Fpulls%%2F%d%%2Fpull.md&ref=%s"
            % (number, ref.replace("/", "%2F")))


def test_a_merged_pull_without_its_branch_verifies_from_its_metadata_ref(
        monkeypatch):
    seen = _merge_heal_runtime(monkeypatch, {
        _pull_action_at(14, "refs/pr/14/metadata"): _pull_record_blob(14),
        _pull_action_at(36, "forkmesh/pulls"): _pull_record_blob(36),
    })
    rows = [_pull_reward_row(14), _pull_reward_row(36)]

    assert _run(ledger._settle_merged_pull_rewards(object(), rows)) == 2

    assert [read[2] for read in seen["reads"]] == [
        _pull_action(14), _pull_action_at(14, "refs/pr/14/metadata"),
        _pull_action(36), _pull_action_at(36, "refs/pr/36/metadata"),
        _pull_action_at(36, "forkmesh/pulls")]
    assert [row["status"] for row in rows] == [
        "pending_wallet", "pending_wallet"]


def test_the_pulls_own_branch_still_decides_before_the_fallbacks(monkeypatch):
    seen = _merge_heal_runtime(monkeypatch, {
        _pull_action(14): _pull_record_blob(14, status="open"),
        _pull_action_at(14, "refs/pr/14/metadata"): _pull_record_blob(14),
        _pull_action_at(14, "forkmesh/pulls"): _pull_record_blob(14),
    })
    rows = [_pull_reward_row(14)]

    _run(ledger._settle_merged_pull_rewards(object(), rows))

    assert [read[2] for read in seen["reads"]] == [_pull_action(14)]
    assert rows[0]["status"] == "pending_merge"
    assert not seen["writes"]


def test_a_fallback_record_for_another_pull_verifies_nothing(monkeypatch):
    seen = _merge_heal_runtime(monkeypatch, {
        _pull_action_at(14, "refs/pr/14/metadata"): _pull_record_blob(15),
        _pull_action_at(14, "forkmesh/pulls"): _pull_record_blob(
            14, reward_id="reward-someone-else"),
    })
    rows = [_pull_reward_row(14)]

    _run(ledger._settle_merged_pull_rewards(object(), rows))

    assert len(seen["reads"]) == 3
    assert rows[0]["status"] == "pending_merge"
    assert not seen["writes"]


def test_an_unreadable_pull_record_promotes_nothing(monkeypatch):
    seen = _merge_heal_runtime(monkeypatch, {})
    rows = [_pull_reward_row(14)]

    _run(ledger._settle_merged_pull_rewards(object(), rows))

    assert rows[0]["status"] == "pending_merge"
    assert not seen["writes"]


def _estimates_runtime(monkeypatch, rows, captured_sql):
    async def d1_all(_env, sql, *arguments):
        if "FROM contributor_reward_estimates" in sql:
            captured_sql.append(sql)
            return [dict(row) for row in rows]
        if "FROM users" in sql:
            return [{"user_bi": "gaurav-bi", "data": "gaurav-record"}]
        raise AssertionError(sql)

    async def session(_env, _request):
        return "owner-bi", {"name": "owner-user"}

    async def org_row(_env, _owner):
        return "org-bi", None

    async def owns_node(_env, actor, owner):
        return actor == "owner-user" and owner == "forkmesh"

    async def decrypt(_env, _data):
        return {"solana": "1" * 32}

    async def ensure_schema(_env):
        return None

    def response(payload, status=200, **_options):
        return {"status": status, "body": payload}

    for name, value in (
        ("d1_all", d1_all), ("_account_session_record", session),
        ("_org_row", org_row), ("_account_owns_node", owns_node),
        ("decrypt_row", decrypt), ("ensure_schema", ensure_schema),
        ("method_name", lambda request: request.method),
        ("json_response", response),
    ):
        monkeypatch.setattr(ledger, name, value, raising=False)


def _estimates(owner_scoped):
    url = "https://example/api/rewards/estimates"
    if owner_scoped:
        url += "?ownerScope=1&repository=forkmesh%2Fforkmesh"
    return _run(ledger.contributor_reward_estimates_handler(
        object(), SimpleNamespace(method="GET", url=url)))


def test_the_payment_queue_offers_a_verified_landed_pull_as_ready(monkeypatch):
    seen = _merge_heal_runtime(
        monkeypatch, {_pull_action(14): _pull_record_blob(14)})
    _estimates_runtime(monkeypatch, [_pull_reward_row(14)], [])

    # The desktop Ledger reads the public list, so both lists heal.
    for owner_scoped in (False, True):
        monkeypatch.setattr(ledger, "_merged_pull_reward_misses", {},
                            raising=False)
        result = _estimates(owner_scoped)
        reward = result["body"]["rewards"][0]
        assert reward["status"] == "ready"
        assert reward["recipientAddress"] == "1" * 32
    assert len(seen["writes"]) == 2


def test_only_the_owner_list_says_when_a_contributor_was_last_reminded(
        monkeypatch):
    _merge_heal_runtime(monkeypatch, {})
    sent_at = _MERGE_NOW_MS - 60_000

    async def no_wallet(_env, _data):
        return {"solana": ""}

    captured = []
    _estimates_runtime(monkeypatch, [_pull_reward_row(
        14, status="pending_wallet", wallet_reminder_sent_at=sent_at)],
        captured)
    monkeypatch.setattr(ledger, "decrypt_row", no_wallet, raising=False)

    owner = _estimates(True)["body"]["rewards"][0]
    assert owner["status"] == "pending_wallet"
    assert owner["walletReminderSentAt"] == sent_at
    assert owner["walletReminderAvailableAt"] == (
        sent_at + ledger.REWARD_WALLET_REMINDER_INTERVAL_MS)
    assert "reward_wallet_reminders" in captured[0]

    public = _estimates(False)["body"]["rewards"][0]
    assert "walletReminderSentAt" not in public
    assert "walletReminderAvailableAt" not in public
    assert "reward_wallet_reminders" not in captured[1]


def test_a_sent_reminder_says_when_it_and_the_one_before_went_out(monkeypatch):
    previous = _REMINDER_NOW_MS - 5 * 24 * 60 * 60 * 1000
    _reminder_runtime(
        monkeypatch, reward=_PENDING_WALLET_REWARD,
        account=_NO_WALLET_ACCOUNT, previous={"sent_at": previous})

    result = _post_reminder()

    assert result["status"] == 200
    assert result["body"]["sentAt"] == _REMINDER_NOW_MS
    assert result["body"]["previousSentAt"] == previous


def test_ledger_avatars_come_from_the_directory_before_per_account_lookups():
    client = (PUBLIC / "ledger.js").read_text(encoding="utf-8")
    portrait = client.split("function loadAvatarPortrait(name)", 1)[1].split(
        "function loadAccountPortrait(name)", 1)[0]
    assert "loadDirectoryPortraits().then" in portrait
    assert "portraits.has(name) ? portraits.get(name) : loadAccountPortrait(name)" in portrait
    directory = client.split("function loadDirectoryPortraits()", 1)[1].split(
        "function loadAvatarPortrait(name)", 1)[0]
    assert 'fetch("/api/accounts/users"' in directory
    assert "if (directoryPortraits) return directoryPortraits;" in directory


def _expected_expense_rows(*charges):
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.execute(
        "CREATE TABLE operating_expenses (expense_id TEXT PRIMARY KEY, "
        "vendor TEXT, paid_by_name TEXT, amount_minor INTEGER, "
        "currency TEXT, incurred_at INTEGER)")
    database.executemany(
        "INSERT INTO operating_expenses VALUES (?,?,?,?,?,?)", charges)

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in database.execute(sql, args).fetchall()]

    return d1_all


def _utc_ms(year, month, day):
    from datetime import datetime, timezone
    return int(datetime(year, month, day, tzinfo=timezone.utc)
               .timestamp() * 1000)


def test_a_monthly_vendor_with_a_skipped_month_is_expected_until_recorded(
        monkeypatch):
    monkeypatch.setattr(ledger, "d1_all", _expected_expense_rows(
        ("anthropic-6", "Anthropic", "jett", 9675, "USD",
         _utc_ms(2026, 6, 22)),
        ("anthropic-9", "Anthropic", "jett", 2178, "USD",
         _utc_ms(2026, 7, 13)),
        ("anthropic-10", "Anthropic", "jett", 21775, "USD",
         _utc_ms(2026, 7, 26)),
        ("anthropic-12", "Anthropic", "jett", 21775, "USD",
         _utc_ms(2026, 9, 12)),
        ("apple-dev", "Apple", "jett", 10000, "USD", _utc_ms(2026, 8, 6)),
        ("openai-old", "OpenAI", "jett", 1089, "USD", _utc_ms(2025, 7, 13)),
        ("openai-new", "OpenAI", "jett", 9165, "USD", _utc_ms(2026, 7, 5)),
    ), raising=False)

    expected = _run(ledger._expected_expenses(
        None, _utc_ms(2026, 10, 7)))

    assert [item["id"] for item in expected] == [
        "expected-expense:anthropic:2026-08",
        "expected-expense:anthropic:2026-10",
    ]
    august, october = expected
    assert august["status"] == "missing_receipt"
    assert august["amountMinor"] == 2178 + 21775
    assert august["basisExpenseId"] == "anthropic-10"
    assert august["expectedAt"] == _utc_ms(2026, 8, 26) + 43_200_000
    assert october["status"] == "expected"
    assert october["amountMinor"] == 21775
    assert october["expectedAt"] == _utc_ms(2026, 10, 12) + 43_200_000

    entry = ledger._expected_expense_entry(august)
    assert entry["category"] == "expected-expense"
    assert entry["direction"] == "forecast"
    assert entry["amountUsd"] == "239.53"
    assert entry["valuationBasis"] == "monthly-history-estimate"
    assert entry["sourceTable"] == "operating_expenses"
    assert entry["title"] == "Anthropic receipt missing for August 2026"
    assert "record it to replace this estimate" in entry["note"]
    assert any(link["url"] == "/@jett" for link in entry["links"])


def test_a_lapsed_monthly_vendor_stops_being_expected(monkeypatch):
    monkeypatch.setattr(ledger, "d1_all", _expected_expense_rows(
        ("cursor-6", "Cursor", "kaif", 2000, "USD", _utc_ms(2026, 5, 3)),
        ("cursor-7", "Cursor", "kaif", 2000, "USD", _utc_ms(2026, 6, 3)),
    ), raising=False)

    assert _run(ledger._expected_expenses(None, _utc_ms(2026, 8, 20)))
    assert _run(ledger._expected_expenses(
        None, _utc_ms(2026, 9, 20))) == []


_EXPENSE_NOW_MS = 1_792_172_288_341


def _expense_runtime(monkeypatch, *, actor="owner-user", admin=True,
                     payers=("jett",)):
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.execute(
        "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    database.executescript(
        (ROOT / "migrations" / "0166_operating_expenses.sql").read_text(
            encoding="utf-8"))
    database.execute("DELETE FROM operating_expenses")
    seen = {"signed": [], "audits": [], "database": database}

    async def signed(_env, request, digest, proof, prefix):
        seen["signed"].append((digest, proof, prefix))
        return ("actor-bi", {"name": actor}) if actor else ("", None)

    async def is_admin(_env, name):
        return admin and name == actor

    async def account_row(_env, name):
        return ((name + "-bi", {"name": name, "status": "active"})
                if name in payers else ("", None))

    async def audit(_env, who, action, target_type="", target="",
                    outcome="success", details=None):
        seen["audits"].append((who, action, target, outcome, details))

    async def d1_first(_env, sql, *arguments):
        row = database.execute(sql, arguments).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *arguments):
        database.execute(sql, arguments)

    async def ensure_schema(_env):
        return None

    class Date:
        @staticmethod
        def now():
            return _EXPENSE_NOW_MS

    def response(payload, status=200, **options):
        return {"status": status, "body": payload, "options": options}

    for name, value in (
        ("_plugin_scope_signed_session", signed), ("_is_admin", is_admin),
        ("_account_row", account_row), ("_audit_sensitive_action", audit),
        ("d1_first", d1_first), ("d1_run", d1_run),
        ("ensure_schema", ensure_schema), ("Date", Date),
        ("json_response", response),
        ("method_name", lambda request: request.method),
    ):
        monkeypatch.setattr(ledger, name, value, raising=False)
    return seen


_ANTHROPIC_0013 = {
    "vendor": "Anthropic",
    "description": "Anthropic subscription invoice",
    "paidBy": "@Jett",
    "amount": "217.75",
    "currency": "usd",
    "externalReference": "8RYATVG1-0013",
    "incurredOn": "2026-10-16",
}


def _post_expense(body, method="POST"):
    raw = body if isinstance(body, str) else __import__("json").dumps(body)

    async def text():
        return raw

    return _run(ledger.operating_expense_record_handler(
        object(),
        SimpleNamespace(method=method, text=text,
                        url="https://app.forkmesh.com/api/ledger/expenses")))


def _expense_rows(seen):
    return [dict(row) for row in seen["database"].execute(
        "SELECT expense_id,vendor,paid_by_name,amount_minor,currency,"
        "external_reference,incurred_at,recorded_at,reimbursement_status "
        "FROM operating_expenses")]


def test_an_admin_records_a_member_paid_expense_as_an_outstanding_debt(
        monkeypatch):
    import hashlib
    import json

    seen = _expense_runtime(monkeypatch)
    result = _post_expense(_ANTHROPIC_0013)

    assert result["status"] == 201
    assert result["body"]["created"] is True
    assert result["body"]["expense"]["id"] == "anthropic-8ryatvg1-0013"
    assert seen["signed"] == [(
        hashlib.sha256(json.dumps(_ANTHROPIC_0013).encode()).hexdigest(),
        "forkmesh-ledger-expense-request-v1", "/api/ledger/expenses")]
    assert _expense_rows(seen) == [{
        "expense_id": "anthropic-8ryatvg1-0013", "vendor": "Anthropic",
        "paid_by_name": "jett", "amount_minor": 21775, "currency": "USD",
        "external_reference": "8RYATVG1-0013",
        "incurred_at": 1_792_108_800_000, "recorded_at": _EXPENSE_NOW_MS,
        "reimbursement_status": "outstanding"}]
    assert seen["audits"] == [(
        "owner-user", "ledger.expense.record", "anthropic-8ryatvg1-0013",
        "success", {"paidBy": "jett", "amountMinor": 21775,
                    "currency": "USD"})]


def test_recording_the_same_invoice_again_is_idempotent(monkeypatch):
    seen = _expense_runtime(monkeypatch)
    assert _post_expense(_ANTHROPIC_0013)["status"] == 201
    again = _post_expense(_ANTHROPIC_0013)

    assert again["status"] == 200
    assert again["body"]["created"] is False
    assert len(_expense_rows(seen)) == 1
    assert len(seen["audits"]) == 1


def test_a_reused_invoice_reference_with_other_details_is_refused(monkeypatch):
    seen = _expense_runtime(monkeypatch)
    _post_expense(_ANTHROPIC_0013)
    result = _post_expense({**_ANTHROPIC_0013, "amount": "200"})

    assert result["status"] == 409
    assert result["body"]["error"] == "expense_reference_conflict"
    assert result["body"]["expense"]["amountMinor"] == 21775
    assert [row["amount_minor"] for row in _expense_rows(seen)] == [21775]


def test_only_a_signed_administrator_may_record_an_expense(monkeypatch):
    seen = _expense_runtime(monkeypatch, admin=False)
    denied = _post_expense(_ANTHROPIC_0013)
    assert denied["status"] == 403
    assert denied["body"] == {"error": "admin_required"}
    assert seen["audits"][0][3] == "denied"
    assert _expense_rows(seen) == []

    seen = _expense_runtime(monkeypatch, actor="")
    unsigned = _post_expense(_ANTHROPIC_0013)
    assert unsigned["status"] == 401
    assert _expense_rows(seen) == []


def test_expense_fields_are_validated_before_anything_is_written(monkeypatch):
    seen = _expense_runtime(monkeypatch)
    bad = _post_expense({
        **_ANTHROPIC_0013, "amount": "217.755", "incurredOn": "2026-10-17",
        "externalReference": "", "originalAmount": "100"})
    assert bad["status"] == 400
    assert bad["body"]["fields"] == [
        "amount", "externalReference", "incurredOn", "originalCurrency"]

    unknown = _post_expense({**_ANTHROPIC_0013, "paidBy": "nobody"})
    assert unknown["status"] == 404
    assert unknown["body"] == {"error": "payer_not_found"}

    assert _post_expense("not json")["status"] == 400
    assert _post_expense(_ANTHROPIC_0013, method="GET")["status"] == 405
    assert _expense_rows(seen) == []


def test_an_expense_keeps_its_original_card_currency(monkeypatch):
    seen = _expense_runtime(monkeypatch, payers=("mdkaifansari04",))
    result = _post_expense({
        "vendor": "Cursor", "description": "Cursor subscription",
        "paidBy": "mdkaifansari04", "amount": 23.6,
        "externalReference": "E45E60F2-0008", "incurredOn": "2026-10-01",
        "originalAmount": "2343.59", "originalCurrency": "inr"})

    assert result["status"] == 201
    assert result["body"]["expense"]["amountMinor"] == 2360
    assert result["body"]["expense"]["originalAmountMinor"] == 234359
    assert result["body"]["expense"]["originalCurrency"] == "INR"
    assert len(_expense_rows(seen)) == 1
