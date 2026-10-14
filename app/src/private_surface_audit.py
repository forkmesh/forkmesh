"""Keyless inspection of ForkMesh private storage surfaces.

The auditor intentionally accepts no DATA_KEY or recipient private key.  It
models the view available to a database, backup, or replica operator and emits
only finding codes and row ordinals; values from inspected storage never enter
the report or an exception message.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import json
import re
import sqlite3
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


AUDIT_SCHEMA = "forkmesh-private-surface-audit-v1"
MAX_DEFAULT_ROWS = 100_000
MAX_REPLICA_BYTES = 256 * 1024 * 1024
_HEX_256 = re.compile(r"^[0-9a-f]{64}$")
_B64URL = re.compile(r"^[A-Za-z0-9_-]+$")

# Tables whose payload applies to one repository. Missing optional tables are
# reported in coverage, not treated as a successful zero-row inspection.
REPOSITORY_PAYLOAD_TABLES = (
    "repositories",
    "repo_device_mirrors",
    "repo_shares",
    "issue_inbox",
    "pull_inbox",
    "pull_submission_receipts",
    "discussion_inbox",
    "repo_agents",
    "agent_prompts",
    "repo_security_scans",
    "repo_security_scan_reviews",
    "repo_security_scan_leases",
    "repo_media",
    "about_inbox",
    "repo_terms_flags",
)

# These payloads are always private even though their lookup key is an account
# or thread rather than a repository key.
ACCOUNT_PAYLOAD_TABLES = ("notifications", "thread_subscriptions")

# Only these non-payload fields may remain in the operator-visible envelope for
# private rows. The audit fails closed if a future migration adds another field
# until this contract and the public documentation are reviewed together.
DECLARED_COLUMNS = {
    "repositories": {"key_bi", "owner_bi", "is_private", "data"},
    "repo_source_authorities": {
        "repo_bi", "node_id", "machine_name", "created_at", "updated_at",
        "is_private", "data",
    },
    "repo_device_mirrors": {"repo_bi", "node_id", "updated_at", "data"},
    "repo_shares": {"repo_bi", "grantee_bi", "ts", "data"},
    "issue_inbox": {
        "id", "repo_bi", "submitter_bi", "claimed_by_bi",
        "claim_expires_at", "mirrored_by_bi", "mirrored_at", "data",
    },
    "pull_inbox": {
        "id", "repo_bi", "submitter_bi", "submission_bi", "claimed_by_bi",
        "claim_expires_at", "mirrored_by_bi", "mirrored_at", "data",
    },
    "pull_submission_receipts": {"repo_bi", "submission_bi", "data"},
    "discussion_inbox": {
        "id", "repo_bi", "submitter_bi", "claimed_by_bi",
        "claim_expires_at", "mirrored_by_bi", "mirrored_at", "data",
    },
    "repo_agents": {"repo_bi", "agent_id", "updated_at", "data"},
    "agent_prompts": {"id", "repo_bi", "agent_id", "queued_at", "data"},
    "notifications": {
        "dedupe_bi", "recipient_bi", "ts", "read_at", "data", "content_bi",
    },
    "thread_subscriptions": {
        "thread_bi", "subscriber_bi", "ts", "data",
    },
    "repo_security_scans": {
        "repo_bi", "scan_id", "scanned_at", "received_at", "data",
    },
    "repo_security_scan_reviews": {
        "repo_bi", "scan_id", "finding_id", "reviewer_bi", "reviewed_at",
        "data",
    },
    "repo_security_scan_leases": {
        "repo_bi", "lease_token_bi", "status", "acquired_at", "expires_at",
        "next_attempt_at", "attempt_count", "completed_scan_id", "updated_at",
        "data",
    },
    "repo_media": {"repo_bi", "kind", "updated_at", "data"},
    "about_inbox": {"repo_bi", "queued_at", "data"},
    "repo_terms_flags": {
        "repo_bi", "active", "category", "updated_by_bi", "updated_at",
        "data",
    },
    "repo_state_history": {"key_bi", "state_hash", "ts"},
}


@dataclass(frozen=True)
class Finding:
    code: str
    surface: str
    row: int = 0


def _table_names(connection: sqlite3.Connection) -> set[str]:
    return {
        str(row[0]) for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )
    }


def _columns(connection: sqlite3.Connection, table: str) -> list[str]:
    # table comes only from constants above or sqlite_master.
    return [str(row[1]) for row in connection.execute(
        'PRAGMA table_info("%s")' % table.replace('"', '""'))]


def _canonical_base64_ciphertext(value: object) -> bool:
    """Recognize one D1 AES-GCM row without attempting decryption."""
    if not isinstance(value, str) or not value or len(value) > 16 * 1024 * 1024:
        return False
    if value.startswith("owner-sealed-v1:"):
        return _owner_sealed_wrapper(value)
    try:
        raw = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError):
        return False
    # 96-bit nonce plus at least one byte and the 128-bit GCM tag.
    return len(raw) >= 29 and base64.b64encode(raw).decode("ascii") == value


def _decode_b64url(value: object, size: int | None = None) -> bytes | None:
    if not isinstance(value, str) or not value or "=" in value:
        return None
    if not _B64URL.fullmatch(value):
        return None
    try:
        raw = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except (binascii.Error, ValueError):
        return None
    if size is not None and len(raw) != size:
        return None
    if base64.urlsafe_b64encode(raw).decode().rstrip("=") != value:
        return None
    return raw


def _valid_recipient(value: object) -> bool:
    if not isinstance(value, dict) or set(value) != {
        "kid", "x25519", "mlkem768", "nonce", "tag", "key"
    }:
        return False
    return bool(
        re.fullmatch(r"[A-Za-z0-9_-]{16,128}", str(value.get("kid") or ""))
        and _decode_b64url(value.get("x25519"), 32) is not None
        and _decode_b64url(value.get("mlkem768"), 1088) is not None
        and _decode_b64url(value.get("nonce"), 12) is not None
        and _decode_b64url(value.get("tag"), 16) is not None
        and _decode_b64url(value.get("key"), 32) is not None
    )


def _valid_hybrid_envelope(value: object, kind: str) -> bool:
    if not isinstance(value, dict) or set(value) != {
        "kind", "v", "alg", "nonce", "tag", "body", "recipients"
    }:
        return False
    recipients = value.get("recipients")
    if (
        value.get("kind") != kind
        or value.get("v") != 1
        or value.get("alg") != "x25519+mlkem768/aes256gcm"
        or not isinstance(recipients, list)
        or not recipients
        or len(recipients) > 64
        or _decode_b64url(value.get("nonce"), 12) is None
        or _decode_b64url(value.get("tag"), 16) is None
        or _decode_b64url(value.get("body")) is None
        or not all(_valid_recipient(item) for item in recipients)
    ):
        return False
    kids = [str(item["kid"]) for item in recipients]
    return len(kids) == len(set(kids))


def _owner_sealed_wrapper(value: str) -> bool:
    try:
        encoded = value.split(":", 1)[1]
        raw = _decode_b64url(encoded)
        wrapper = json.loads((raw or b"").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return False
    if not isinstance(wrapper, dict) or not set(wrapper).issubset({"envelope", "routing"}):
        return False
    routing = wrapper.get("routing", {})
    if not isinstance(routing, dict) or not set(routing).issubset({"agentId", "queuedAt"}):
        return False
    return _valid_hybrid_envelope(wrapper.get("envelope"), "forkmesh.owner-sealed")


def _blind_index(value: object, *, empty_ok: bool = False) -> bool:
    if empty_ok and value in (None, ""):
        return True
    return isinstance(value, str) and bool(_HEX_256.fullmatch(value))


def _query_rows(
    connection: sqlite3.Connection,
    table: str,
    columns: list[str],
    private_keys: tuple[str, ...],
    max_rows: int,
) -> tuple[list[sqlite3.Row], bool]:
    quoted = ",".join('"%s"' % item.replace('"', '""') for item in columns)
    if table == "repositories":
        where, args = " WHERE is_private=1", ()
    elif table == "repo_source_authorities":
        marks = ",".join("?" for _ in private_keys) or "NULL"
        where, args = " WHERE repo_bi IN (%s)" % marks, private_keys
    elif table in REPOSITORY_PAYLOAD_TABLES:
        key = "key_bi" if table == "repositories" else "repo_bi"
        marks = ",".join("?" for _ in private_keys) or "NULL"
        where, args = " WHERE %s IN (%s)" % (key, marks), private_keys
    elif table == "repo_state_history":
        marks = ",".join("?" for _ in private_keys) or "NULL"
        where, args = " WHERE key_bi IN (%s)" % marks, private_keys
    else:
        where, args = "", ()
    rows = list(connection.execute(
        "SELECT %s FROM \"%s\"%s LIMIT ?" % (
            quoted, table.replace('"', '""'), where),
        (*args, max_rows + 1),
    ))
    return rows[:max_rows], len(rows) > max_rows


def audit_database(
    connection: sqlite3.Connection, *, max_rows: int = MAX_DEFAULT_ROWS
) -> dict:
    """Inspect protected database rows and return a value-free report."""
    connection.row_factory = sqlite3.Row
    findings: list[Finding] = []
    tables = _table_names(connection)
    if "repositories" not in tables:
        findings.append(Finding("required_table_missing", "repositories"))
        private_keys: tuple[str, ...] = ()
    else:
        repo_columns = set(_columns(connection, "repositories"))
        if "is_private" not in repo_columns:
            findings.append(Finding("privacy_discriminator_missing", "repositories"))
            private_keys = ()
        else:
            private_keys = tuple(str(row[0]) for row in connection.execute(
                "SELECT key_bi FROM repositories WHERE is_private=1"
            ))

    inspected: dict[str, int] = {}
    expected = set(DECLARED_COLUMNS)
    for table in sorted(expected):
        if table not in tables:
            inspected[table] = 0
            findings.append(Finding("surface_missing", table))
            continue
        columns = _columns(connection, table)
        unexpected = set(columns) - DECLARED_COLUMNS[table]
        if unexpected:
            findings.append(Finding("undeclared_metadata_column", table))
        selected = [name for name in columns if name in DECLARED_COLUMNS[table]]
        rows, truncated = _query_rows(
            connection, table, selected, private_keys, max_rows)
        inspected[table] = len(rows)
        if truncated:
            findings.append(Finding("coverage_incomplete", table))
        for ordinal, row in enumerate(rows, 1):
            record = dict(row)
            if table == "repo_state_history":
                findings.append(Finding("private_derived_index", table, ordinal))
                continue
            data = record.get("data")
            if table == "repo_source_authorities":
                if record.get("node_id") or record.get("machine_name"):
                    findings.append(Finding("private_authority_plaintext", table, ordinal))
                # A freshly scrubbed legacy row can reconstruct its authority
                # from repositories.data on the next owner publication.
                if data and not _canonical_base64_ciphertext(data):
                    findings.append(Finding("malformed_ciphertext", table, ordinal))
                continue
            if "data" in record and not _canonical_base64_ciphertext(data):
                findings.append(Finding("malformed_ciphertext", table, ordinal))
            if table == "repo_device_mirrors" and not _blind_index(record.get("node_id")):
                findings.append(Finding("private_device_plaintext", table, ordinal))
            for name, value in record.items():
                if name.endswith("_bi") and not _blind_index(value, empty_ok=True):
                    findings.append(Finding("invalid_blind_index", table, ordinal))

    ordered = sorted(findings, key=lambda item: (item.surface, item.row, item.code))
    return {
        "schema": AUDIT_SCHEMA,
        "ok": not ordered,
        "privateRepositories": len(private_keys),
        "inspectedRows": inspected,
        "findings": [asdict(item) for item in ordered],
    }


def audit_replica_bytes(value: bytes) -> list[str]:
    """Validate one opaque private replica without disclosing parsed values."""
    try:
        root = json.loads(value.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return ["invalid_replica_json"]
    keys = {
        "schemaVersion", "kind", "opaqueId", "ownerKeyId", "keyEpoch",
        "createdAt", "updatedAt", "ciphertextSha256", "envelope",
    }
    if not isinstance(root, dict) or set(root) != keys:
        return ["invalid_replica_metadata"]
    envelope = root.get("envelope")
    canonical_envelope = json.dumps(
        envelope, separators=(",", ":"), ensure_ascii=False).encode()
    recipients = envelope.get("recipients", []) if isinstance(envelope, dict) else []
    kids = [str(item.get("kid") or "") for item in recipients if isinstance(item, dict)]
    valid = (
        root.get("schemaVersion") == 2
        and root.get("kind") == "forkmesh.private-replica"
        and bool(_HEX_256.fullmatch(str(root.get("opaqueId") or "")))
        and isinstance(root.get("keyEpoch"), int)
        and not isinstance(root.get("keyEpoch"), bool)
        and root.get("keyEpoch", 0) > 0
        and isinstance(root.get("createdAt"), int)
        and isinstance(root.get("updatedAt"), int)
        and 0 < root.get("createdAt", 0) <= root.get("updatedAt", 0)
        and root.get("ownerKeyId") in kids
        and _valid_hybrid_envelope(envelope, "forkmesh.mirror")
        and root.get("ciphertextSha256") == hashlib.sha256(canonical_envelope).hexdigest()
    )
    return [] if valid else ["invalid_replica_metadata"]


def audit_replica_paths(paths: Iterable[Path]) -> dict:
    findings = []
    inspected = 0
    epochs: dict[str, int] = {}
    for path in sorted(paths, key=lambda item: str(item)):
        inspected += 1
        try:
            size = path.stat().st_size
            if size <= 0 or size > MAX_REPLICA_BYTES:
                findings.append({
                    "code": "invalid_replica_size",
                    "surface": "replica",
                    "row": inspected,
                })
                continue
            value = path.read_bytes()
        except OSError:
            findings.append({
                "code": "replica_unreadable",
                "surface": "replica",
                "row": inspected,
            })
            continue
        errors = audit_replica_bytes(value)
        for code in errors:
            findings.append({"code": code, "surface": "replica", "row": inspected})
        if errors:
            continue
        root = json.loads(value.decode("utf-8"))
        opaque_id = root["opaqueId"]
        epoch = int(root["keyEpoch"])
        prior = epochs.get(opaque_id)
        if prior is not None and epoch <= prior:
            findings.append({
                "code": "replica_epoch_not_monotonic",
                "surface": "replica",
                "row": inspected,
            })
        epochs[opaque_id] = epoch
    return {"inspected": inspected, "findings": findings}
