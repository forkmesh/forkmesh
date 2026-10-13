"""Deterministic, auditable policy for non-custodial mirror rewards.

This module deliberately has no network, database, wallet, or Worker-runtime
dependencies.  The edge layer supplies a frozen set of independently observed
facts and public Solana entropy; this module only filters, de-duplicates, and
selects a candidate.  It never accepts or handles private-key material.
"""

from __future__ import annotations

import hashlib
import json
import re


SOLANA_ADDRESS_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
INTEGRITY_STATES = frozenset({"verified", "signed-manifest-verified"})


def _canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _digest(value):
    return hashlib.sha256(_canonical_json(value).encode()).hexdigest()


def bounded_int(value, default, minimum, maximum):
    """Return an operator setting constrained to a safe public range."""
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        parsed = int(default)
    return max(int(minimum), min(int(maximum), parsed))


def eligibility_snapshot(candidates, *, minimum_uptime_ms,
                         minimum_contribution_units=0):
    """Return deterministic accepted/rejected reward candidates.

    A wallet, operator account, node identity, and device identity may each
    occur at most once.  Those controls materially raise the cost of farming,
    but the audit explicitly avoids claiming that they prove personhood.
    """
    ordered = sorted(
        (dict(candidate or {}) for candidate in candidates or []),
        key=lambda item: (
            str(item.get("operatorId") or ""),
            str(item.get("nodeId") or ""),
            str(item.get("walletAddress") or ""),
        ),
    )
    accepted = []
    rejected = []
    seen_nodes = set()
    seen_operators = set()
    seen_wallets = set()
    seen_devices = set()
    for item in ordered:
        node_id = str(item.get("nodeId") or "").strip().lower()
        operator_id = str(item.get("operatorId") or "").strip().lower()
        device_id = str(item.get("deviceId") or "").strip().lower()
        wallet = str(item.get("walletAddress") or "").strip()
        uptime_ms = max(0, int(item.get("observedUptimeMs") or 0))
        contribution_units = max(
            0, int(item.get("contributionUnits") or 0))
        reason = ""
        if not node_id or not operator_id or not device_id:
            reason = "stable_identity_missing"
        elif not item.get("attestationSourceApproved"):
            reason = "attestation_source_unapproved"
        elif not item.get("identitySignatureVerified"):
            reason = "identity_signature_unverified"
        elif not item.get("healthAttestationSignatureVerified"):
            reason = "health_attestation_signature_unverified"
        elif not item.get("healthAttestationFresh"):
            reason = "health_attestation_stale"
        elif not item.get("online"):
            reason = "offline"
        elif not item.get("healthy"):
            reason = "health_check_failed"
        elif not item.get("mirrorsForkMesh"):
            reason = "forkmesh_mirror_unverified"
        elif str(item.get("integrity") or "") not in INTEGRITY_STATES:
            reason = "integrity_unverified"
        elif item.get("abuseFlagged"):
            reason = "abuse_flagged"
        elif uptime_ms < int(minimum_uptime_ms):
            reason = "minimum_observed_uptime_not_met"
        elif contribution_units < int(minimum_contribution_units):
            reason = "minimum_contribution_not_met"
        elif not SOLANA_ADDRESS_RE.fullmatch(wallet):
            reason = "self_custodial_wallet_missing"
        elif node_id in seen_nodes:
            reason = "duplicate_node_identity"
        elif operator_id in seen_operators:
            reason = "duplicate_operator_identity"
        elif wallet in seen_wallets:
            reason = "duplicate_wallet"
        elif device_id in seen_devices:
            reason = "duplicate_device_identity"
        if reason:
            rejected.append({
                "nodeId": node_id or "unknown",
                "reason": reason,
            })
            continue
        public = {
            "nodeId": node_id,
            "operatorId": operator_id,
            "deviceId": device_id,
            "walletAddress": wallet,
            "observedUptimeMs": uptime_ms,
            "contributionUnits": contribution_units,
            "healthCheckedAt": int(item.get("healthCheckedAt") or 0),
            "attestationId": str(item.get("attestationId") or ""),
        }
        accepted.append(public)
        seen_nodes.add(node_id)
        seen_operators.add(operator_id)
        seen_wallets.add(wallet)
        seen_devices.add(device_id)
    return {
        "eligible": accepted,
        "rejected": rejected,
        "snapshotHash": _digest(accepted),
        "policy": {
            "minimumObservedUptimeMs": int(minimum_uptime_ms),
            "minimumContributionUnits": int(minimum_contribution_units),
            "oneRewardPerNode": True,
            "oneRewardPerOperator": True,
            "oneRewardPerWallet": True,
            "oneRewardPerDevice": True,
            "requiresSignedIdentity": True,
            "requiresApprovedAttestationSource": True,
            "requiresSignedFreshHealthAttestation": True,
            "requiresHealthyHttpsEndpoint": True,
            "requiresForkMeshMirror": True,
            "requiresVerifiedIntegrity": True,
            "excludesAbuseFlags": True,
        },
        "limitations": [
            "Identity, device, wallet, uptime, and rate controls reduce but "
            "cannot eliminate Sybil attacks.",
            "A healthy signed endpoint proves recent availability and identity, "
            "not that its operator is universally trustworthy.",
        ],
    }


def select_candidate(snapshot, *, round_id, public_entropy,
                     entropy_source):
    """Select one entry using a reproducible SHA-256 modulo operation."""
    eligible = list((snapshot or {}).get("eligible") or [])
    if not eligible:
        return None
    if not round_id or not public_entropy or not entropy_source:
        return None
    snapshot_hash = str((snapshot or {}).get("snapshotHash") or "")
    digest = hashlib.sha256(
        (
            "forkmesh-reward-selection-v1\0"
            + str(round_id)
            + "\0"
            + str(public_entropy)
            + "\0"
            + snapshot_hash
        ).encode()
    ).digest()
    index = int.from_bytes(digest, "big") % len(eligible)
    return {
        "candidate": eligible[index],
        "algorithm": "sha256-modulo-v1",
        "selectedIndex": index,
        "eligibleCount": len(eligible),
        "roundId": str(round_id),
        "snapshotHash": snapshot_hash,
        "entropySource": str(entropy_source),
        "publicEntropy": str(public_entropy),
        "entropyHash": hashlib.sha256(
            str(public_entropy).encode()).hexdigest(),
    }


def schedule_offset_ms(*, round_id, public_entropy, jitter_window_ms):
    """Derive a reproducible execution offset from public entropy."""
    window = max(0, int(jitter_window_ms))
    if not window:
        return 0
    digest = hashlib.sha256(
        (
            "forkmesh-reward-schedule-v1\0"
            + str(round_id)
            + "\0"
            + str(public_entropy)
        ).encode()
    ).digest()
    return int.from_bytes(digest[:8], "big") % (window + 1)
