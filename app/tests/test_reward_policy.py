import sys
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "src"
sys.path.insert(0, str(SRC))

import reward_policy as policy  # noqa: E402


WALLET_A = "11111111111111111111111111111111"
WALLET_B = "SysvarRent111111111111111111111111111111111"


def candidate(node, wallet, **overrides):
    value = {
        "nodeId": node,
        "operatorId": node + "-owner",
        "deviceId": node + "-device",
        "walletAddress": wallet,
        "identitySignatureVerified": True,
        "attestationSourceApproved": True,
        "healthAttestationSignatureVerified": True,
        "healthAttestationFresh": True,
        "online": True,
        "healthy": True,
        "mirrorsForkMesh": True,
        "integrity": "signed-manifest-verified",
        "abuseFlagged": False,
        "observedUptimeMs": 7_200_000,
        "contributionUnits": 3,
        "healthCheckedAt": 1_000,
        "attestationId": node + "-attestation",
    }
    value.update(overrides)
    return value


def test_snapshot_enforces_every_eligibility_signal():
    cases = [
        ({"online": False}, "offline"),
        ({"healthy": False}, "health_check_failed"),
        ({"mirrorsForkMesh": False}, "forkmesh_mirror_unverified"),
        ({"integrity": "unknown"}, "integrity_unverified"),
        ({"abuseFlagged": True}, "abuse_flagged"),
        ({"identitySignatureVerified": False},
         "identity_signature_unverified"),
        ({"attestationSourceApproved": False},
         "attestation_source_unapproved"),
        ({"healthAttestationSignatureVerified": False},
         "health_attestation_signature_unverified"),
        ({"healthAttestationFresh": False},
         "health_attestation_stale"),
        ({"observedUptimeMs": 59_999},
         "minimum_observed_uptime_not_met"),
        ({"contributionUnits": 0}, "minimum_contribution_not_met"),
        ({"walletAddress": "not-a-wallet"},
         "self_custodial_wallet_missing"),
    ]
    for index, (change, expected) in enumerate(cases):
        snap = policy.eligibility_snapshot(
            [candidate("n" + str(index), WALLET_A, **change)],
            minimum_uptime_ms=60_000,
            minimum_contribution_units=1,
        )
        assert snap["eligible"] == []
        assert snap["rejected"][0]["reason"] == expected


def test_snapshot_deduplicates_operator_wallet_device_and_node():
    rows = [
        candidate("alpha", WALLET_A),
        candidate("alpha", WALLET_B, operatorId="second"),
        candidate("beta", WALLET_A),
        candidate("gamma", WALLET_B, operatorId="alpha-owner"),
        candidate("delta", WALLET_B, deviceId="alpha-device"),
    ]
    snap = policy.eligibility_snapshot(
        rows, minimum_uptime_ms=1, minimum_contribution_units=0)
    assert [item["nodeId"] for item in snap["eligible"]] == ["alpha"]
    assert {item["reason"] for item in snap["rejected"]} == {
        "duplicate_node_identity",
        "duplicate_wallet",
        "duplicate_operator_identity",
        "duplicate_device_identity",
    }
    assert snap["policy"]["oneRewardPerWallet"] is True
    assert "cannot eliminate Sybil" in snap["limitations"][0]


def test_selection_and_schedule_are_publicly_reproducible():
    snap = policy.eligibility_snapshot(
        [candidate("beta", WALLET_B), candidate("alpha", WALLET_A)],
        minimum_uptime_ms=1,
    )
    first = policy.select_candidate(
        snap,
        round_id="2026-07-23T10",
        public_entropy="solana-finalized-blockhash",
        entropy_source="solana-devnet-finalized-blockhash",
    )
    second = policy.select_candidate(
        snap,
        round_id="2026-07-23T10",
        public_entropy="solana-finalized-blockhash",
        entropy_source="solana-devnet-finalized-blockhash",
    )
    assert first == second
    assert first["eligibleCount"] == 2
    assert first["candidate"] in snap["eligible"]
    assert policy.schedule_offset_ms(
        round_id="round",
        public_entropy="entropy",
        jitter_window_ms=900_000,
    ) == policy.schedule_offset_ms(
        round_id="round",
        public_entropy="entropy",
        jitter_window_ms=900_000,
    )


def test_bounds_operator_configuration():
    assert policy.bounded_int("bad", 60, 10, 100) == 60
    assert policy.bounded_int(-1, 60, 10, 100) == 10
    assert policy.bounded_int(999, 60, 10, 100) == 100
