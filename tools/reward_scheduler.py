#!/usr/bin/env python3
"""Non-custodial mirror reward scheduling and external-signer protocol.

This module never generates, accepts, or stores a private key.  It produces an
auditable transfer intent from a frozen eligibility snapshot, then optionally
sends that public intent to a local signer executable over stdin.  The signer is
responsible for hardware-wallet, OS-keychain, or desktop-wallet authorization.

Pending rewards are metadata reservations only: funds remain in the source
wallet until an external signer authorizes a transfer.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Callable, Iterable


BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
BASE58_INDEX = {character: index for index, character in enumerate(BASE58_ALPHABET)}
NETWORKS = frozenset({"devnet", "testnet", "mainnet-beta"})
SENSITIVE_KEY_RE = re.compile(r"(?:private|secret|seed|mnemonic|keypair)", re.I)


class RewardError(RuntimeError):
    """A public-safe scheduling/signing failure."""


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def isoformat(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def parse_time(value: str) -> datetime:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def valid_solana_address(value: str) -> bool:
    if not isinstance(value, str) or not 32 <= len(value) <= 44:
        return False
    number = 0
    try:
        for character in value:
            number = number * 58 + BASE58_INDEX[character]
    except KeyError:
        return False
    decoded = b"" if number == 0 else number.to_bytes((number.bit_length() + 7) // 8, "big")
    leading_zeroes = len(value) - len(value.lstrip("1"))
    return len(b"\0" * leading_zeroes + decoded) == 32


def _canonical_json(payload: Any) -> str:
    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _digest(payload: Any) -> str:
    return hashlib.sha256(_canonical_json(payload).encode("utf-8")).hexdigest()


def _contains_sensitive_key(payload: Any) -> bool:
    if isinstance(payload, dict):
        return any(
            SENSITIVE_KEY_RE.search(str(key)) or _contains_sensitive_key(value)
            for key, value in payload.items()
        )
    if isinstance(payload, list):
        return any(_contains_sensitive_key(value) for value in payload)
    return False


@dataclass(frozen=True)
class EligibilityPolicy:
    minimum_uptime_minutes: int = 60
    require_attestation: bool = True
    require_forkmesh_mirror: bool = True
    require_integrity: bool = True


def eligible_candidates(
    candidates: Iterable[dict[str, Any]],
    policy: EligibilityPolicy,
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """Filter and de-duplicate a frozen candidate snapshot.

    One operator identity and one wallet can receive at most one entry in a
    round.  This is a mitigation, not proof of personhood; the returned audit
    makes that limitation visible to operators and reviewers.
    """

    accepted: list[dict[str, Any]] = []
    rejected: list[dict[str, str]] = []
    used_wallets: set[str] = set()
    used_operators: set[str] = set()
    ordered = sorted(
        (dict(item) for item in candidates),
        key=lambda item: (
            str(item.get("operatorId") or ""),
            str(item.get("walletAddress") or ""),
            str(item.get("nodeId") or ""),
        ),
    )
    for item in ordered:
        node_id = str(item.get("nodeId") or "")
        operator_id = str(item.get("operatorId") or "")
        wallet = str(item.get("walletAddress") or "")
        reason = ""
        if not node_id or not operator_id:
            reason = "missing_stable_identity"
        elif not item.get("online"):
            reason = "offline"
        elif not item.get("healthy"):
            reason = "unhealthy"
        elif item.get("abuseFlagged"):
            reason = "abuse_flagged"
        elif policy.require_attestation and not item.get("attestationVerified"):
            reason = "attestation_unverified"
        elif policy.require_forkmesh_mirror and not item.get("mirrorsForkMesh"):
            reason = "forkmesh_mirror_unverified"
        elif policy.require_integrity and not item.get("integrityVerified"):
            reason = "integrity_unverified"
        elif int(item.get("uptimeMinutes") or 0) < policy.minimum_uptime_minutes:
            reason = "minimum_uptime_not_met"
        elif not valid_solana_address(wallet):
            reason = "invalid_or_missing_wallet"
        elif wallet in used_wallets:
            reason = "duplicate_wallet"
        elif operator_id in used_operators:
            reason = "duplicate_operator"
        if reason:
            rejected.append({"nodeId": node_id or "unknown", "reason": reason})
            continue
        normalized = {
            "nodeId": node_id,
            "operatorId": operator_id,
            "walletAddress": wallet,
            "uptimeMinutes": int(item.get("uptimeMinutes") or 0),
            "healthCheckedAt": str(item.get("healthCheckedAt") or ""),
            "attestationId": str(item.get("attestationId") or ""),
        }
        accepted.append(normalized)
        used_wallets.add(wallet)
        used_operators.add(operator_id)
    return accepted, rejected


def jittered_run_offset_seconds(
    *, round_id: str, public_entropy: str, window_seconds: int = 3600
) -> int:
    if window_seconds <= 0:
        raise RewardError("reward window must be positive")
    digest = hashlib.sha256(
        f"forkmesh-reward-time-v1\0{round_id}\0{public_entropy}".encode("utf-8")
    ).digest()
    return int.from_bytes(digest[:8], "big") % window_seconds


def schedule_reward(
    candidates: Iterable[dict[str, Any]],
    *,
    policy: EligibilityPolicy,
    round_id: str,
    public_entropy: str,
    entropy_source: str,
    source_wallet: str,
    lamports: int,
    network: str = "mainnet-beta",
    now: datetime | None = None,
    expires_in_minutes: int = 15,
) -> dict[str, Any]:
    """Choose one eligible wallet and return an unsigned transfer intent."""

    if not round_id or not public_entropy or not entropy_source:
        raise RewardError("round id, public entropy, and entropy source are required")
    if network not in NETWORKS:
        raise RewardError(f"unsupported Solana network: {network}")
    if not valid_solana_address(source_wallet):
        raise RewardError("source wallet is not a valid Solana public address")
    if lamports <= 0:
        raise RewardError("reward amount must be a positive lamport count")
    accepted, rejected = eligible_candidates(candidates, policy)
    if not accepted:
        raise RewardError("no eligible mirror node is available")

    snapshot_digest = _digest(accepted)
    selection_digest = hashlib.sha256(
        (
            "forkmesh-reward-selection-v1\0"
            + round_id
            + "\0"
            + public_entropy
            + "\0"
            + snapshot_digest
        ).encode("utf-8")
    ).digest()
    selected_index = int.from_bytes(selection_digest, "big") % len(accepted)
    selected = accepted[selected_index]
    created_at = now or utc_now()
    expires_at = created_at + timedelta(minutes=expires_in_minutes)
    intent_core = {
        "network": network,
        "sourceWallet": source_wallet,
        "destinationWallet": selected["walletAddress"],
        "lamports": lamports,
        "roundId": round_id,
        "snapshotDigest": snapshot_digest,
        "entropyDigest": hashlib.sha256(public_entropy.encode("utf-8")).hexdigest(),
    }
    intent_id = _digest(intent_core)
    return {
        "schemaVersion": 1,
        "type": "forkmesh.solana.reward-transfer-intent",
        "intentId": intent_id,
        "createdAt": isoformat(created_at),
        "expiresAt": isoformat(expires_at),
        "network": network,
        "sourceWallet": source_wallet,
        "destinationWallet": selected["walletAddress"],
        "lamports": lamports,
        "selection": {
            "roundId": round_id,
            "algorithm": "sha256-modulo-v1",
            "entropySource": entropy_source,
            "entropyDigest": intent_core["entropyDigest"],
            "snapshotDigest": snapshot_digest,
            "eligibleCount": len(accepted),
            "selectedIndex": selected_index,
            "selectedNodeId": selected["nodeId"],
            "rejected": rejected,
            "limitations": [
                "Wallet and operator de-duplication reduce but do not eliminate Sybil risk.",
                "Entropy must become unpredictable only after the eligibility snapshot closes.",
            ],
        },
        "custody": {
            "forkMeshHoldsUserKeys": False,
            "fundsRemainInSourceWalletUntilSigned": True,
        },
        "signing": {"required": True, "performed": False, "method": "external"},
    }


def create_pending_reward(
    *,
    recipient_id: str,
    source_wallet: str,
    lamports: int,
    source_pool_id: str,
    now: datetime | None = None,
    expires_in_hours: int = 24,
) -> dict[str, Any]:
    if not recipient_id or not source_pool_id:
        raise RewardError("recipient and source pool identifiers are required")
    if not valid_solana_address(source_wallet):
        raise RewardError("source wallet is not a valid Solana public address")
    if lamports <= 0:
        raise RewardError("pending amount must be positive")
    if expires_in_hours <= 0 or expires_in_hours > 24:
        raise RewardError("pending rewards may remain claimable for at most 24 hours")
    created_at = now or utc_now()
    core = {
        "recipientId": recipient_id,
        "sourceWallet": source_wallet,
        "sourcePoolId": source_pool_id,
        "lamports": lamports,
        "createdAt": isoformat(created_at),
    }
    return {
        "schemaVersion": 1,
        "type": "forkmesh.solana.pending-reward",
        "allocationId": _digest(core),
        "recipientId": recipient_id,
        "sourcePoolId": source_pool_id,
        "sourceWallet": source_wallet,
        "lamports": lamports,
        "createdAt": isoformat(created_at),
        "expiresAt": isoformat(created_at + timedelta(hours=expires_in_hours)),
        "status": "pending_wallet",
        "custody": {
            "fundsMoved": False,
            "fundsRemainInSourceWallet": True,
            "forkMeshCreatedWallet": False,
        },
    }


def expire_pending_reward(
    allocation: dict[str, Any], *, now: datetime | None = None
) -> dict[str, Any]:
    if allocation.get("type") != "forkmesh.solana.pending-reward":
        raise RewardError("unsupported pending allocation document")
    current = now or utc_now()
    if current < parse_time(str(allocation["expiresAt"])):
        raise RewardError("pending allocation has not expired")
    expired = dict(allocation)
    expired["status"] = "expired_returned_to_source"
    expired["expiredAt"] = isoformat(current)
    expired["custody"] = {
        "fundsMoved": False,
        "fundsRemainInSourceWallet": True,
        "forkMeshCreatedWallet": False,
    }
    return expired


def claim_pending_reward(
    allocation: dict[str, Any],
    *,
    destination_wallet: str,
    network: str = "mainnet-beta",
    now: datetime | None = None,
    signing_window_minutes: int = 15,
) -> dict[str, Any]:
    current = now or utc_now()
    if allocation.get("status") != "pending_wallet":
        raise RewardError("pending allocation is not claimable")
    if current >= parse_time(str(allocation["expiresAt"])):
        raise RewardError("pending allocation has expired")
    if network not in NETWORKS:
        raise RewardError(f"unsupported Solana network: {network}")
    if not valid_solana_address(destination_wallet):
        raise RewardError("destination wallet is not a valid Solana public address")
    core = {
        "allocationId": allocation["allocationId"],
        "network": network,
        "sourceWallet": allocation["sourceWallet"],
        "destinationWallet": destination_wallet,
        "lamports": int(allocation["lamports"]),
    }
    claim_expiry = min(
        parse_time(str(allocation["expiresAt"])),
        current + timedelta(minutes=signing_window_minutes),
    )
    return {
        "schemaVersion": 1,
        "type": "forkmesh.solana.pending-claim-intent",
        "intentId": _digest(core),
        "allocationId": allocation["allocationId"],
        "createdAt": isoformat(current),
        "expiresAt": isoformat(claim_expiry),
        "network": network,
        "sourceWallet": allocation["sourceWallet"],
        "destinationWallet": destination_wallet,
        "lamports": int(allocation["lamports"]),
        "custody": {
            "forkMeshHoldsUserKeys": False,
            "fundsRemainInSourceWalletUntilSigned": True,
        },
        "signing": {"required": True, "performed": False, "method": "external"},
    }


class ExternalSigner:
    """JSON-over-stdin adapter for a local wallet/hardware signer."""

    def __init__(
        self,
        command: list[str],
        *,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
        clock: Callable[[], datetime] = utc_now,
    ) -> None:
        if not command or any(not item for item in command):
            raise RewardError("external signer command is empty")
        self.command = list(command)
        self._runner = runner
        self._clock = clock

    def sign(self, intent: dict[str, Any]) -> dict[str, Any]:
        if _contains_sensitive_key(intent):
            raise RewardError("signing intent contains a prohibited sensitive-key field")
        if intent.get("signing", {}).get("performed"):
            raise RewardError("intent is already marked signed")
        if intent.get("expiresAt") and self._clock() >= parse_time(
            str(intent["expiresAt"])
        ):
            raise RewardError("signing intent has expired")
        try:
            result = self._runner(
                self.command,
                input=_canonical_json(intent) + "\n",
                text=True,
                capture_output=True,
                check=True,
            )
        except (OSError, subprocess.CalledProcessError) as exc:
            raise RewardError("external signer failed; inspect its local logs") from exc
        try:
            response = json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise RewardError("external signer returned invalid JSON") from exc
        if _contains_sensitive_key(response):
            raise RewardError("external signer returned a prohibited sensitive-key field")
        if response.get("intentId") != intent.get("intentId"):
            raise RewardError("external signer response does not match the intent id")
        signer_public_key = str(response.get("signerPublicKey") or "")
        if signer_public_key != intent.get("sourceWallet"):
            raise RewardError("external signer does not control the source wallet")
        signed_transaction = response.get("signedTransaction")
        if not isinstance(signed_transaction, str) or len(signed_transaction) < 16:
            raise RewardError("external signer did not return a signed transaction")
        return {
            "schemaVersion": 1,
            "type": "forkmesh.solana.externally-signed-transaction",
            "intentId": intent["intentId"],
            "network": intent["network"],
            "signerPublicKey": signer_public_key,
            "signedTransaction": signed_transaction,
            "signedAt": str(response.get("signedAt") or isoformat(utc_now())),
            "custody": {
                "privateKeyReceivedByForkMesh": False,
                "signingPerformedExternally": True,
            },
        }


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    schedule = commands.add_parser("schedule")
    schedule.add_argument("--candidates", type=Path, required=True)
    schedule.add_argument("--output", type=Path, required=True)
    schedule.add_argument("--round-id", required=True)
    schedule.add_argument("--public-entropy", required=True)
    schedule.add_argument("--entropy-source", required=True)
    schedule.add_argument("--source-wallet", required=True)
    schedule.add_argument("--lamports", required=True, type=int)
    schedule.add_argument(
        "--network", choices=sorted(NETWORKS), default="mainnet-beta")
    schedule.add_argument("--minimum-uptime-minutes", type=int, default=60)

    pending = commands.add_parser("pending")
    pending.add_argument("--recipient-id", required=True)
    pending.add_argument("--source-pool-id", required=True)
    pending.add_argument("--source-wallet", required=True)
    pending.add_argument("--lamports", required=True, type=int)
    pending.add_argument("--output", type=Path, required=True)

    expire = commands.add_parser("expire")
    expire.add_argument("--allocation", type=Path, required=True)
    expire.add_argument("--output", type=Path, required=True)

    claim = commands.add_parser("claim")
    claim.add_argument("--allocation", type=Path, required=True)
    claim.add_argument("--destination-wallet", required=True)
    claim.add_argument(
        "--network", choices=sorted(NETWORKS), default="mainnet-beta")
    claim.add_argument("--output", type=Path, required=True)

    sign = commands.add_parser("sign")
    sign.add_argument("--intent", type=Path, required=True)
    sign.add_argument("--output", type=Path, required=True)
    sign.add_argument(
        "signer_command",
        nargs=argparse.REMAINDER,
        help="local signer executable and arguments, following --",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "schedule":
            candidates = _load_json(args.candidates)
            if not isinstance(candidates, list):
                raise RewardError("candidate document must be a JSON array")
            payload = schedule_reward(
                candidates,
                policy=EligibilityPolicy(
                    minimum_uptime_minutes=args.minimum_uptime_minutes
                ),
                round_id=args.round_id,
                public_entropy=args.public_entropy,
                entropy_source=args.entropy_source,
                source_wallet=args.source_wallet,
                lamports=args.lamports,
                network=args.network,
            )
        elif args.command == "pending":
            payload = create_pending_reward(
                recipient_id=args.recipient_id,
                source_wallet=args.source_wallet,
                lamports=args.lamports,
                source_pool_id=args.source_pool_id,
            )
        elif args.command == "expire":
            payload = expire_pending_reward(_load_json(args.allocation))
        elif args.command == "claim":
            payload = claim_pending_reward(
                _load_json(args.allocation),
                destination_wallet=args.destination_wallet,
                network=args.network,
            )
        else:
            command = list(args.signer_command)
            if command and command[0] == "--":
                command = command[1:]
            payload = ExternalSigner(command).sign(_load_json(args.intent))
        _write_json(args.output, payload)
        print(f"Wrote {payload['type']} to {args.output}")
        return 0
    except (RewardError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"reward scheduler failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
