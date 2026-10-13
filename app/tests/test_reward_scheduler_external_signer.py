#!/usr/bin/env python3
"""Non-custodial reward scheduler and external signer contracts."""

from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
from types import SimpleNamespace
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import reward_scheduler as rewards  # noqa: E402


SOURCE_WALLET = "11111111111111111111111111111111"
WALLET_A = "So11111111111111111111111111111111111111112"
WALLET_B = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"


def candidate(node, operator, wallet):
    return {
        "nodeId": node,
        "operatorId": operator,
        "walletAddress": wallet,
        "online": True,
        "healthy": True,
        "abuseFlagged": False,
        "attestationVerified": True,
        "attestationId": f"attest-{node}",
        "mirrorsForkMesh": True,
        "integrityVerified": True,
        "uptimeMinutes": 720,
        "healthCheckedAt": "2026-07-23T10:00:00Z",
    }


def test_public_entropy_selection_is_deterministic_across_input_order():
    candidates = [
        candidate("node-a", "alice", WALLET_A),
        candidate("node-b", "bob", WALLET_B),
    ]
    kwargs = {
        "policy": rewards.EligibilityPolicy(),
        "round_id": "2026-07-23T10",
        "public_entropy": "solana-blockhash-after-snapshot",
        "entropy_source": "Solana finalized block 123",
        "source_wallet": SOURCE_WALLET,
        "lamports": 5000,
        "network": "devnet",
        "now": datetime(2026, 7, 23, 10, tzinfo=timezone.utc),
    }
    first = rewards.schedule_reward(candidates, **kwargs)
    second = rewards.schedule_reward(reversed(candidates), **kwargs)

    assert first["intentId"] == second["intentId"]
    assert first["destinationWallet"] == second["destinationWallet"]
    assert first["signing"] == {
        "required": True,
        "performed": False,
        "method": "external",
    }
    assert first["custody"]["forkMeshHoldsUserKeys"] is False
    assert "private" not in json.dumps(first).lower()


def test_eligibility_rejects_duplicate_wallet_operator_and_bad_health():
    candidates = [
        candidate("node-a", "alice", WALLET_A),
        candidate("node-b", "bob", WALLET_A),
        candidate("node-c", "alice", WALLET_B),
        {**candidate("node-d", "dana", WALLET_B), "healthy": False},
    ]
    accepted, rejected = rewards.eligible_candidates(
        candidates, rewards.EligibilityPolicy()
    )
    assert len(accepted) == 1
    assert {item["reason"] for item in rejected} == {
        "duplicate_wallet",
        "duplicate_operator",
        "unhealthy",
    }


def test_pending_reward_expires_without_moving_funds():
    created = datetime(2026, 7, 23, 10, tzinfo=timezone.utc)
    pending = rewards.create_pending_reward(
        recipient_id="alice",
        source_wallet=SOURCE_WALLET,
        lamports=1000,
        source_pool_id="community-devnet",
        now=created,
    )
    assert pending["status"] == "pending_wallet"
    assert pending["custody"]["fundsMoved"] is False
    assert rewards.parse_time(pending["expiresAt"]) == created + timedelta(hours=24)

    expired = rewards.expire_pending_reward(
        pending, now=created + timedelta(hours=24, seconds=1)
    )
    assert expired["status"] == "expired_returned_to_source"
    assert expired["custody"]["fundsRemainInSourceWallet"] is True


def test_claim_signing_window_never_extends_past_pending_expiry():
    created = datetime(2026, 7, 23, 10, tzinfo=timezone.utc)
    pending = rewards.create_pending_reward(
        recipient_id="alice",
        source_wallet=SOURCE_WALLET,
        lamports=1000,
        source_pool_id="community-devnet",
        now=created,
    )
    claim = rewards.claim_pending_reward(
        pending,
        destination_wallet=WALLET_A,
        now=created + timedelta(hours=23, minutes=59),
        signing_window_minutes=15,
    )
    assert claim["expiresAt"] == pending["expiresAt"]


def test_external_signer_receives_only_public_intent_over_stdin():
    intent = rewards.schedule_reward(
        [candidate("node-a", "alice", WALLET_A)],
        policy=rewards.EligibilityPolicy(),
        round_id="round-1",
        public_entropy="entropy-after-cutoff",
        entropy_source="test vector",
        source_wallet=SOURCE_WALLET,
        lamports=500,
        network="devnet",
    )
    captured = {}

    def fake_runner(command, **kwargs):
        captured["command"] = command
        captured["input"] = kwargs["input"]
        return SimpleNamespace(
            stdout=json.dumps(
                {
                    "intentId": intent["intentId"],
                    "signerPublicKey": SOURCE_WALLET,
                    "signedTransaction": "base64-signed-transaction-value",
                }
            )
        )

    signed = rewards.ExternalSigner(
        ["local-wallet", "sign"], runner=fake_runner
    ).sign(intent)
    assert captured["command"] == ["local-wallet", "sign"]
    assert json.loads(captured["input"])["intentId"] == intent["intentId"]
    assert "secret" not in captured["input"].lower()
    assert "seed" not in captured["input"].lower()
    assert signed["custody"]["privateKeyReceivedByForkMesh"] is False


def test_external_signer_rejects_sensitive_key_fields():
    with pytest.raises(rewards.RewardError):
        rewards.ExternalSigner(["signer"], runner=lambda *args, **kwargs: None).sign(
            {
                "intentId": "test",
                "network": "devnet",
                "privateKey": "must-not-be-accepted",
                "signing": {"performed": False},
            }
        )


def test_external_signer_rejects_expired_intent_without_running_command():
    intent = {
        "intentId": "expired",
        "network": "devnet",
        "sourceWallet": SOURCE_WALLET,
        "expiresAt": "2026-07-23T10:00:00Z",
        "signing": {"performed": False},
    }
    calls = []
    signer = rewards.ExternalSigner(
        ["signer"],
        runner=lambda *args, **kwargs: calls.append((args, kwargs)),
        clock=lambda: datetime(2026, 7, 23, 10, 1, tzinfo=timezone.utc),
    )
    with pytest.raises(rewards.RewardError, match="expired"):
        signer.sign(intent)
    assert calls == []
