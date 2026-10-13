#!/usr/bin/env python3
"""Offline migration for historical Worker-held Solana wallet seeds.

The live Worker has no wallet-key generation, signing, or broadcast path. This
tool is deliberately separate from the Worker bundle and operates on an
operator-exported SQLite snapshot. It:

1. decrypts legacy D1 row blobs locally using the historical DATA_KEY;
2. verifies that each seed derives the public address recorded beside it;
3. reconciles every public address against an explicitly selected Solana RPC;
4. exports seeds only inside a passphrase-encrypted, owner-controlled artifact;
5. emits a fail-closed D1 scrub SQL patch only after a separate confirmation
   file and a fresh zero-balance reconciliation. Active account rows lose the
   key fields; retired custody rows are replaced by key-free public
   reconciliation evidence.

It never signs a transaction, submits a transaction, moves funds, prints a
seed, or mutates a remote D1 database.
"""

from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import sys
import time
from typing import Any, Callable
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
    )
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:  # pragma: no cover - exercised by CLI environments only.
    AESGCM = None
    Ed25519PrivateKey = None
    serialization = None


TOOL_VERSION = "2"
AUDIT_FORMAT = "forkmesh-legacy-solana-custody-audit-v1"
ARTIFACT_FORMAT = "forkmesh-legacy-solana-custody-artifact-v1"
PAYLOAD_FORMAT = "forkmesh-legacy-solana-custody-payload-v1"
CONFIRMATION_FORMAT = "forkmesh-legacy-solana-scrub-confirmation-v1"
SCRUB_CONFIRMATION = (
    "I HAVE IMPORTED EVERY LEGACY KEY AND RECONCILED EVERY ADDRESS AT ZERO"
)
ARTIFACT_AAD = ARTIFACT_FORMAT.encode("ascii")
MAX_RPC_RESPONSE_BYTES = 1024 * 1024
SCRYPT_N = 1 << 17
SCRYPT_R = 8
SCRYPT_P = 1
SOLANA_ADDRESS_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
INSECURE_DATA_KEYS = {
    "",
    "forkmesh-dev-data-key",
    "forkmesh-dev-data-key-change-me",
}

# These are historical compatibility locations only. The current Worker must
# never add a wallet key to any of them.
TABLE_SPECS = (
    ("accounts", "name_bi", (("donation_secret", "donation_address"),)),
    ("users", "user_bi", (("donation_secret", "donation_address"),)),
    ("nodes", "node_bi", (("donation_secret", "donation_address"),)),
    (
        "federated_signup",
        "reference",
        (("donation_secret", "donation_address"),),
    ),
    ("issue_bounty", "bounty_bi", (("secret", "address"),)),
    ("bounty_wallet", "wallet_bi", (("secret", "address"),)),
    ("central_fund", "id", (("secret", "address"),)),
)
RETIRED_ROW_TABLES = frozenset({
    "federated_signup",
    "issue_bounty",
    "bounty_wallet",
    "central_fund",
})
WALLET_KEY_NAMES = frozenset({
    "donationsecret",
    "walletsecret",
    "walletprivatekey",
    "solanasecret",
    "solanaprivatekey",
    "seedbase64url",
    "mnemonic",
    "recoveryphrase",
})


class CustodyMigrationError(RuntimeError):
    """Expected, redaction-safe migration failure."""


class LegacyRecord:
    """In-memory key-bearing record whose repr never includes secret material."""

    __slots__ = (
        "table",
        "pk_column",
        "pk_value",
        "key_field",
        "address_field",
        "address",
        "seed",
        "record",
        "stored",
    )

    def __init__(
        self,
        *,
        table: str,
        pk_column: str,
        pk_value: Any,
        key_field: str,
        address_field: str,
        address: str,
        seed: str,
        record: dict[str, Any],
        stored: str,
    ) -> None:
        self.table = table
        self.pk_column = pk_column
        self.pk_value = pk_value
        self.key_field = key_field
        self.address_field = address_field
        self.address = address
        self.seed = seed
        self.record = record
        self.stored = stored

    def __repr__(self) -> str:
        return (
            "LegacyRecord(table=%r, record_id=%r, address=%r, seed=<redacted>)"
            % (self.table, self.record_id, self.address)
        )

    @property
    def record_id(self) -> str:
        value = "%s\x00%s\x00%s" % (
            self.table,
            self.pk_column,
            str(self.pk_value),
        )
        return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _require_crypto() -> None:
    if AESGCM is None or Ed25519PrivateKey is None or serialization is None:
        raise CustodyMigrationError(
            "cryptography_dependency_required"
        )


def _canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("utf-8")


def _sha256_hex(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _b64encode(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def _b64decode(value: str, *, error: str) -> bytes:
    try:
        return base64.b64decode(value, validate=True)
    except Exception:
        raise CustodyMigrationError(error) from None


def _b64url_decode(value: str) -> bytes:
    if not isinstance(value, str) or not value:
        raise CustodyMigrationError("invalid_legacy_seed")
    try:
        raw = base64.urlsafe_b64decode(
            value.encode("ascii") + b"=" * ((4 - len(value) % 4) % 4)
        )
    except Exception:
        raise CustodyMigrationError("invalid_legacy_seed") from None
    if len(raw) != 32:
        raise CustodyMigrationError("invalid_legacy_seed")
    return raw


def _base58_encode(value: bytes) -> str:
    number = int.from_bytes(value, "big")
    encoded = ""
    while number:
        number, remainder = divmod(number, 58)
        encoded = BASE58_ALPHABET[remainder] + encoded
    leading = len(value) - len(value.lstrip(b"\x00"))
    return "1" * leading + (encoded or "1")


def _derive_solana_address(seed_b64url: str) -> str:
    _require_crypto()
    seed = _b64url_decode(seed_b64url)
    try:
        private_key = Ed25519PrivateKey.from_private_bytes(seed)
        public = private_key.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )
    except Exception:
        raise CustodyMigrationError("invalid_legacy_seed") from None
    return _base58_encode(public)


def _data_cipher(data_key: str) -> AESGCM:
    _require_crypto()
    if not isinstance(data_key, str) or data_key.strip() in INSECURE_DATA_KEYS:
        raise CustodyMigrationError("valid_historical_data_key_required")
    return AESGCM(hashlib.sha256(data_key.strip().encode("utf-8")).digest())


def decrypt_d1_record(stored: str, data_key: str) -> dict[str, Any]:
    """Mirror entry.py's historical SHA-256(DATA_KEY) + AES-GCM row format."""
    blob = _b64decode(stored, error="legacy_row_decryption_failed")
    if len(blob) < 12 + 16:
        raise CustodyMigrationError("legacy_row_decryption_failed")
    try:
        plaintext = _data_cipher(data_key).decrypt(blob[:12], blob[12:], None)
        value = json.loads(plaintext.decode("utf-8"))
    except CustodyMigrationError:
        raise
    except Exception:
        raise CustodyMigrationError("legacy_row_decryption_failed") from None
    if not isinstance(value, dict):
        raise CustodyMigrationError("legacy_row_not_an_object")
    return value


def encrypt_d1_record(value: dict[str, Any], data_key: str) -> str:
    nonce = os.urandom(12)
    ciphertext = _data_cipher(data_key).encrypt(
        nonce,
        json.dumps(value, separators=(",", ":"), ensure_ascii=True).encode(),
        None,
    )
    return _b64encode(nonce + ciphertext)


def _table_exists(connection: sqlite3.Connection, table: str) -> bool:
    row = connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
        (table,),
    ).fetchone()
    return bool(row)


def _quoted_identifier(value: str) -> str:
    if not re.fullmatch(r"[a-z_][a-z0-9_]*", value):
        raise CustodyMigrationError("invalid_schema_identifier")
    return '"' + value + '"'


def _contains_wallet_key(value: Any) -> bool:
    """Match every wallet-key spelling the upgraded Worker refuses to load."""
    if isinstance(value, dict):
        normalized_keys = {
            re.sub(r"[^a-z0-9]", "", str(key).lower())
            for key in value
        }
        for key, item in value.items():
            normalized = re.sub(r"[^a-z0-9]", "", str(key).lower())
            if (
                normalized in WALLET_KEY_NAMES
                or (normalized == "secret" and "address" in normalized_keys)
                or _contains_wallet_key(item)
            ):
                return True
    elif isinstance(value, list):
        return any(_contains_wallet_key(item) for item in value)
    return False


def scan_legacy_records(
    database: Path,
    data_key: str,
) -> tuple[list[LegacyRecord], dict[str, int]]:
    """Decrypt every relevant row and return only verified key-bearing records.

    If any relevant encrypted row cannot be decrypted, the entire scan fails.
    Skipping an unreadable row could strand an undiscovered wallet key.
    """
    _require_crypto()
    if not database.is_file():
        raise CustodyMigrationError("sqlite_snapshot_required")
    try:
        connection = sqlite3.connect(
            "file:%s?mode=ro" % database.resolve().as_posix(),
            uri=True,
        )
        connection.row_factory = sqlite3.Row
    except Exception:
        raise CustodyMigrationError("sqlite_snapshot_open_failed") from None

    records: list[LegacyRecord] = []
    scanned: dict[str, int] = {}
    failures = 0
    try:
        for table, pk_column, field_pairs in TABLE_SPECS:
            if not _table_exists(connection, table):
                continue
            columns = {
                str(row["name"])
                for row in connection.execute(
                    "PRAGMA table_info(%s)" % _quoted_identifier(table)
                )
            }
            if pk_column not in columns or "data" not in columns:
                raise CustodyMigrationError("legacy_table_schema_mismatch")
            rows = connection.execute(
                "SELECT %s AS pk_value, data FROM %s"
                % (_quoted_identifier(pk_column), _quoted_identifier(table))
            )
            scanned[table] = 0
            for row in rows:
                scanned[table] += 1
                try:
                    stored = str(row["data"] or "")
                    record = decrypt_d1_record(stored, data_key)
                    for key_field, address_field in field_pairs:
                        seed = record.get(key_field)
                        if seed in (None, ""):
                            continue
                        if not isinstance(seed, str):
                            raise CustodyMigrationError(
                                "invalid_legacy_seed"
                            )
                        address = str(record.get(address_field) or "").strip()
                        if not SOLANA_ADDRESS_RE.fullmatch(address):
                            raise CustodyMigrationError(
                                "invalid_legacy_public_address"
                            )
                        derived = _derive_solana_address(seed)
                        if not hmac.compare_digest(derived, address):
                            raise CustodyMigrationError(
                                "legacy_seed_address_mismatch"
                            )
                        records.append(
                            LegacyRecord(
                                table=table,
                                pk_column=pk_column,
                                pk_value=row["pk_value"],
                                key_field=key_field,
                                address_field=address_field,
                                address=address,
                                seed=seed,
                                record=record,
                                stored=stored,
                            )
                        )
                    # Never declare a row clean merely because it used a
                    # key-field alias this tool cannot safely export. Unknown
                    # shapes need an explicitly reviewed tool update.
                    unrecognized = dict(record)
                    for key_field, _address_field in field_pairs:
                        unrecognized.pop(key_field, None)
                    if _contains_wallet_key(unrecognized):
                        raise CustodyMigrationError(
                            "unsupported_legacy_wallet_key_shape"
                        )
                except CustodyMigrationError:
                    failures += 1
    finally:
        connection.close()
    if failures:
        raise CustodyMigrationError(
            "legacy_scan_failed_for_%d_relevant_row(s)" % failures
        )
    _validate_unique_addresses(records)
    return records, scanned


def _validate_unique_addresses(records: list[LegacyRecord]) -> None:
    seeds: dict[str, str] = {}
    for record in records:
        previous = seeds.get(record.address)
        if previous is not None and not hmac.compare_digest(
            previous, record.seed
        ):
            raise CustodyMigrationError("conflicting_seed_for_public_address")
        seeds[record.address] = record.seed


def _rpc_origin(rpc_url: str) -> str:
    parsed = urlsplit(rpc_url)
    if parsed.scheme != "https" or not parsed.hostname:
        raise CustodyMigrationError("https_solana_rpc_required")
    port = (":" + str(parsed.port)) if parsed.port else ""
    return parsed.scheme + "://" + parsed.hostname + port


def read_public_balance(
    rpc_url: str,
    address: str,
    *,
    timeout_seconds: float = 10.0,
) -> tuple[int, int]:
    """Return finalized public balance and context slot; never submit a tx."""
    _rpc_origin(rpc_url)
    body = _canonical_json(
        {
            "jsonrpc": "2.0",
            "id": "forkmesh-legacy-custody-audit",
            "method": "getBalance",
            "params": [address, {"commitment": "finalized"}],
        }
    )
    request = Request(
        rpc_url,
        data=body,
        method="POST",
        headers={
            "content-type": "application/json",
            "accept": "application/json",
        },
    )
    try:
        with urlopen(request, timeout=timeout_seconds) as response:
            raw = response.read(MAX_RPC_RESPONSE_BYTES + 1)
    except Exception:
        raise CustodyMigrationError("solana_balance_reconciliation_failed") from None
    if len(raw) > MAX_RPC_RESPONSE_BYTES:
        raise CustodyMigrationError("solana_rpc_response_too_large")
    try:
        payload = json.loads(raw.decode("utf-8"))
        result = payload["result"]
        balance = int(result["value"])
        slot = int(result["context"]["slot"])
    except Exception:
        raise CustodyMigrationError("invalid_solana_balance_response") from None
    if balance < 0 or slot < 0:
        raise CustodyMigrationError("invalid_solana_balance_response")
    return balance, slot


def build_public_audit(
    records: list[LegacyRecord],
    scanned: dict[str, int],
    *,
    network: str,
    rpc_url: str,
    balance_reader: Callable[[str, str], tuple[int, int]] = read_public_balance,
    now: int | None = None,
) -> dict[str, Any]:
    _rpc_origin(rpc_url)
    now = int(now if now is not None else time.time() * 1000)
    balances: dict[str, tuple[int, int]] = {}
    for address in sorted({record.address for record in records}):
        balances[address] = balance_reader(rpc_url, address)
    public_records = []
    for record in sorted(records, key=lambda item: item.record_id):
        balance, slot = balances[record.address]
        public_records.append(
            {
                "recordId": record.record_id,
                "sourceTable": record.table,
                "publicAddress": record.address,
                "balanceLamports": balance,
                "rpcSlot": slot,
                "reconciledAt": now,
            }
        )
    return {
        "format": AUDIT_FORMAT,
        "toolVersion": TOOL_VERSION,
        "createdAt": now,
        "network": network,
        "rpcOrigin": _rpc_origin(rpc_url),
        "keyBearingRecordCount": len(records),
        "uniqueAddressCount": len(balances),
        "scannedRowsByTable": dict(sorted(scanned.items())),
        "allBalancesReconciled": True,
        "records": public_records,
        "notice": (
            "Public audit only. It contains no wallet seed and proves no funds "
            "were moved."
        ),
    }


def _write_private_file(path: Path, content: bytes) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, stat.S_IRUSR | stat.S_IWUSR)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
    except Exception:
        try:
            path.unlink()
        except OSError:
            pass
        raise


def _write_public_file(path: Path, value: dict[str, Any]) -> None:
    _write_private_file(path, _canonical_json(value) + b"\n")


def _artifact_key(passphrase: str, salt: bytes, *, n: int = SCRYPT_N) -> bytes:
    if not isinstance(passphrase, str) or len(passphrase) < 16:
        raise CustodyMigrationError("artifact_passphrase_too_short")
    try:
        return hashlib.scrypt(
            passphrase.encode("utf-8"),
            salt=salt,
            n=n,
            r=SCRYPT_R,
            p=SCRYPT_P,
            dklen=32,
            maxmem=512 * 1024 * 1024,
        )
    except Exception:
        raise CustodyMigrationError("artifact_key_derivation_failed") from None


def create_artifact(
    records: list[LegacyRecord],
    audit: dict[str, Any],
    passphrase: str,
    *,
    now: int | None = None,
    scrypt_n: int = SCRYPT_N,
) -> dict[str, Any]:
    _require_crypto()
    if not records:
        raise CustodyMigrationError("no_legacy_wallet_keys_found")
    now = int(now if now is not None else time.time() * 1000)
    audit_bytes = _canonical_json(audit)
    payload = {
        "format": PAYLOAD_FORMAT,
        "toolVersion": TOOL_VERSION,
        "createdAt": now,
        "network": audit["network"],
        "auditSha256": _sha256_hex(audit_bytes),
        "records": [
            {
                "table": record.table,
                "pkColumn": record.pk_column,
                "pkValue": record.pk_value,
                "keyField": record.key_field,
                "addressField": record.address_field,
                "publicAddress": record.address,
                "seedBase64Url": record.seed,
            }
            for record in sorted(records, key=lambda item: item.record_id)
        ],
    }
    salt = os.urandom(16)
    nonce = os.urandom(12)
    key = _artifact_key(passphrase, salt, n=scrypt_n)
    ciphertext = AESGCM(key).encrypt(
        nonce,
        _canonical_json(payload),
        ARTIFACT_AAD,
    )
    return {
        "format": ARTIFACT_FORMAT,
        "toolVersion": TOOL_VERSION,
        "createdAt": now,
        "network": audit["network"],
        "auditSha256": payload["auditSha256"],
        "recordCount": len(records),
        "uniqueAddressCount": audit["uniqueAddressCount"],
        "kdf": {
            "name": "scrypt",
            "n": scrypt_n,
            "r": SCRYPT_R,
            "p": SCRYPT_P,
            "saltBase64": _b64encode(salt),
        },
        "cipher": {
            "name": "aes-256-gcm",
            "nonceBase64": _b64encode(nonce),
            "ciphertextBase64": _b64encode(ciphertext),
            "aad": ARTIFACT_FORMAT,
        },
        "notice": (
            "Encrypted owner-controlled migration artifact. ForkMesh cannot "
            "recover the passphrase."
        ),
    }


def open_artifact(
    artifact: dict[str, Any],
    passphrase: str,
) -> dict[str, Any]:
    _require_crypto()
    if artifact.get("format") != ARTIFACT_FORMAT:
        raise CustodyMigrationError("unsupported_artifact_format")
    try:
        kdf = artifact["kdf"]
        cipher = artifact["cipher"]
        if kdf["name"] != "scrypt" or cipher["name"] != "aes-256-gcm":
            raise KeyError
        if cipher.get("aad") != ARTIFACT_FORMAT:
            raise KeyError
        salt = _b64decode(kdf["saltBase64"], error="invalid_artifact")
        nonce = _b64decode(cipher["nonceBase64"], error="invalid_artifact")
        ciphertext = _b64decode(
            cipher["ciphertextBase64"],
            error="invalid_artifact",
        )
        n = int(kdf["n"])
        if (
            n < (1 << 15)
            or n > (1 << 20)
            or int(kdf["r"]) != SCRYPT_R
            or int(kdf["p"]) != SCRYPT_P
            or len(salt) != 16
            or len(nonce) != 12
        ):
            raise KeyError
        key = _artifact_key(passphrase, salt, n=n)
        plaintext = AESGCM(key).decrypt(
            nonce,
            ciphertext,
            ARTIFACT_AAD,
        )
        payload = json.loads(plaintext.decode("utf-8"))
    except CustodyMigrationError:
        raise
    except Exception:
        raise CustodyMigrationError("artifact_decryption_failed") from None
    if not isinstance(payload, dict) or payload.get("format") != PAYLOAD_FORMAT:
        raise CustodyMigrationError("invalid_artifact_payload")
    records = payload.get("records")
    try:
        record_count_matches = (
            isinstance(records, list)
            and len(records) == int(artifact.get("recordCount", -1))
        )
    except Exception:
        record_count_matches = False
    if (
        not record_count_matches
        or payload.get("network") != artifact.get("network")
        or payload.get("auditSha256") != artifact.get("auditSha256")
    ):
        raise CustodyMigrationError("artifact_metadata_mismatch")
    return payload


def artifact_sha256(artifact: dict[str, Any]) -> str:
    return _sha256_hex(_canonical_json(artifact))


def _read_json_file(path: Path, *, error: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        raise CustodyMigrationError(error) from None
    if not isinstance(value, dict):
        raise CustodyMigrationError(error)
    return value


def _artifact_record_map(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    values = payload.get("records")
    if not isinstance(values, list):
        raise CustodyMigrationError("invalid_artifact_payload")
    result: dict[str, dict[str, Any]] = {}
    for value in values:
        if not isinstance(value, dict):
            raise CustodyMigrationError("invalid_artifact_payload")
        locator = "%s\x00%s\x00%s" % (
            value.get("table", ""),
            value.get("pkColumn", ""),
            str(value.get("pkValue", "")),
        )
        record_id = hashlib.sha256(locator.encode("utf-8")).hexdigest()
        if record_id in result:
            raise CustodyMigrationError("duplicate_artifact_record")
        result[record_id] = value
    return result


def verify_artifact_matches_records(
    payload: dict[str, Any],
    records: list[LegacyRecord],
) -> None:
    expected = _artifact_record_map(payload)
    actual = {record.record_id: record for record in records}
    if set(expected) != set(actual):
        raise CustodyMigrationError("artifact_snapshot_record_mismatch")
    for record_id, record in actual.items():
        value = expected[record_id]
        checks = (
            str(value.get("table", "")) == record.table,
            str(value.get("pkColumn", "")) == record.pk_column,
            value.get("pkValue") == record.pk_value,
            str(value.get("keyField", "")) == record.key_field,
            str(value.get("addressField", "")) == record.address_field,
            str(value.get("publicAddress", "")) == record.address,
        )
        seed = value.get("seedBase64Url")
        if (
            not all(checks)
            or not isinstance(seed, str)
            or not hmac.compare_digest(seed, record.seed)
        ):
            raise CustodyMigrationError("artifact_snapshot_record_mismatch")


def _sql_literal(value: Any) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    return "'" + str(value).replace("'", "''") + "'"


def build_scrub_sql(
    records: list[LegacyRecord],
    data_key: str,
    *,
    artifact_digest: str,
    audit_digest: str,
    confirmed_at: int,
    prepared_at: int | None = None,
    reconciliation_records: list[dict[str, Any]] | None = None,
    network: str = "",
) -> str:
    """Create optimistic, fail-closed SQL containing no plaintext wallet key."""
    if not re.fullmatch(r"[0-9a-f]{64}", artifact_digest):
        raise CustodyMigrationError("invalid_artifact_digest")
    if not re.fullmatch(r"[0-9a-f]{64}", audit_digest):
        raise CustodyMigrationError("invalid_audit_digest")
    prepared_at = int(
        prepared_at if prepared_at is not None else time.time() * 1000
    )
    if records and reconciliation_records is None:
        raise CustodyMigrationError("reconciliation_records_required")
    lines = [
        "-- ForkMesh legacy Solana custody scrub; contains no wallet keys.",
        "-- Apply only to the quiesced D1 database exported for this audit.",
        "-- D1 file execution supplies the transaction; do not add BEGIN/COMMIT.",
        "CREATE TABLE IF NOT EXISTS legacy_custody_migration_audit ("
        "migration_id TEXT PRIMARY KEY, artifact_sha256 TEXT NOT NULL, "
        "audit_sha256 TEXT NOT NULL, record_count INTEGER NOT NULL, "
        "address_count INTEGER NOT NULL, confirmed_at INTEGER NOT NULL, "
        "prepared_at INTEGER NOT NULL, applied_at INTEGER NOT NULL, "
        "status TEXT NOT NULL CHECK(status='scrubbed'), "
        "tool_version TEXT NOT NULL);",
        "CREATE TABLE IF NOT EXISTS legacy_custody_reconciliation ("
        "record_id TEXT PRIMARY KEY, source_table TEXT NOT NULL, "
        "public_address TEXT NOT NULL, network TEXT NOT NULL, "
        "balance_lamports INTEGER NOT NULL CHECK(balance_lamports=0), "
        "rpc_slot INTEGER NOT NULL, reconciled_at INTEGER NOT NULL, "
        "artifact_sha256 TEXT NOT NULL, audit_sha256 TEXT NOT NULL);",
        "CREATE TABLE IF NOT EXISTS schema_meta ("
        "k TEXT PRIMARY KEY, v TEXT NOT NULL);",
    ]
    for record in sorted(records, key=lambda item: item.record_id):
        table = _quoted_identifier(record.table)
        pk_column = _quoted_identifier(record.pk_column)
        if record.table in RETIRED_ROW_TABLES:
            lines.extend([
                "DELETE FROM %s WHERE %s=%s AND data=%s;"
                % (
                    table,
                    pk_column,
                    _sql_literal(record.pk_value),
                    _sql_literal(record.stored),
                ),
                # A missing row is idempotent only when the exact public
                # reconciliation evidence from a prior application exists.
                "SELECT CASE WHEN changes()=1 OR ("
                "NOT EXISTS(SELECT 1 FROM %s WHERE %s=%s) AND "
                "EXISTS(SELECT 1 FROM legacy_custody_reconciliation "
                "WHERE record_id=%s AND artifact_sha256=%s "
                "AND audit_sha256=%s AND balance_lamports=0)"
                ") THEN 1 ELSE abs(-9223372036854775808) END;"
                % (
                    table,
                    pk_column,
                    _sql_literal(record.pk_value),
                    _sql_literal(record.record_id),
                    _sql_literal(artifact_digest),
                    _sql_literal(audit_digest),
                ),
            ])
        else:
            cleaned = dict(record.record)
            cleaned.pop(record.key_field, None)
            # Historical raw signed bytes are unnecessary after custody transfer
            # and could otherwise remain a rebroadcast temptation.
            cleaned.pop("payout_tx", None)
            cleaned["legacy_custody_migrated_at"] = prepared_at
            cleaned["legacy_custody_artifact_sha256"] = artifact_digest
            cleaned["legacy_custody_status"] = (
                "exported_and_zero_balance_confirmed")
            replacement = encrypt_d1_record(cleaned, data_key)
            lines.extend([
                "UPDATE %s SET data=%s WHERE %s=%s AND data=%s;"
                % (
                    table,
                    _sql_literal(replacement),
                    pk_column,
                    _sql_literal(record.pk_value),
                    _sql_literal(record.stored),
                ),
                # The same reviewed SQL is idempotent, but a divergent row is
                # never accepted. abs(INT64_MIN) aborts the D1 transaction.
                "SELECT CASE WHEN changes()=1 OR EXISTS("
                "SELECT 1 FROM %s WHERE %s=%s AND data=%s"
                ") THEN 1 "
                "ELSE abs(-9223372036854775808) END;"
                % (
                    table,
                    pk_column,
                    _sql_literal(record.pk_value),
                    _sql_literal(replacement),
                ),
            ])
    reconciliation = {
        str(item.get("recordId") or ""): item
        for item in (reconciliation_records or [])
        if isinstance(item, dict)
    }
    if reconciliation_records is not None:
        if (
            not network
            or set(reconciliation) != {record.record_id for record in records}
        ):
            raise CustodyMigrationError("invalid_reconciliation_records")
        for record in sorted(records, key=lambda item: item.record_id):
            item = reconciliation[record.record_id]
            try:
                balance = int(item.get("balanceLamports"))
                rpc_slot = int(item.get("rpcSlot"))
                reconciled_at = int(item.get("reconciledAt"))
            except Exception:
                raise CustodyMigrationError(
                    "invalid_reconciliation_records") from None
            if (
                item.get("sourceTable") != record.table
                or item.get("publicAddress") != record.address
                or balance != 0
                or rpc_slot < 0
                or reconciled_at <= 0
            ):
                raise CustodyMigrationError("invalid_reconciliation_records")
            lines.extend([
                "INSERT INTO legacy_custody_reconciliation "
                "(record_id,source_table,public_address,network,"
                "balance_lamports,rpc_slot,reconciled_at,artifact_sha256,"
                "audit_sha256) VALUES (%s,%s,%s,%s,0,%d,%d,%s,%s) "
                "ON CONFLICT(record_id) DO NOTHING;"
                % (
                    _sql_literal(record.record_id),
                    _sql_literal(record.table),
                    _sql_literal(record.address),
                    _sql_literal(network),
                    rpc_slot,
                    reconciled_at,
                    _sql_literal(artifact_digest),
                    _sql_literal(audit_digest),
                ),
                "SELECT CASE WHEN EXISTS("
                "SELECT 1 FROM legacy_custody_reconciliation "
                "WHERE record_id=%s AND source_table=%s "
                "AND public_address=%s AND network=%s "
                "AND balance_lamports=0 AND rpc_slot=%d "
                "AND reconciled_at=%d AND artifact_sha256=%s "
                "AND audit_sha256=%s) THEN 1 "
                "ELSE abs(-9223372036854775808) END;"
                % (
                    _sql_literal(record.record_id),
                    _sql_literal(record.table),
                    _sql_literal(record.address),
                    _sql_literal(network),
                    rpc_slot,
                    reconciled_at,
                    _sql_literal(artifact_digest),
                    _sql_literal(audit_digest),
                ),
            ])
    migration_id = artifact_digest[:32]
    lines.extend(
        [
            "INSERT INTO legacy_custody_migration_audit "
            "(migration_id, artifact_sha256, audit_sha256, record_count, "
            "address_count, confirmed_at, prepared_at, applied_at, status, "
            "tool_version) VALUES (%s,%s,%s,%d,%d,%d,%d,"
            "CAST(strftime('%%s','now') AS INTEGER)*1000,'scrubbed',%s) "
            "ON CONFLICT(migration_id) DO NOTHING;"
            % (
                _sql_literal(migration_id),
                _sql_literal(artifact_digest),
                _sql_literal(audit_digest),
                len(records),
                len({record.address for record in records}),
                int(confirmed_at),
                prepared_at,
                _sql_literal(TOOL_VERSION),
            ),
            "SELECT CASE WHEN EXISTS("
            "SELECT 1 FROM legacy_custody_migration_audit "
            "WHERE migration_id=%s AND artifact_sha256=%s "
            "AND audit_sha256=%s AND record_count=%d "
            "AND address_count=%d AND confirmed_at=%d "
            "AND prepared_at=%d AND status='scrubbed' "
            "AND tool_version=%s) THEN 1 "
            "ELSE abs(-9223372036854775808) END;"
            % (
                _sql_literal(migration_id),
                _sql_literal(artifact_digest),
                _sql_literal(audit_digest),
                len(records),
                len({record.address for record in records}),
                int(confirmed_at),
                prepared_at,
                _sql_literal(TOOL_VERSION),
            ),
            "INSERT INTO schema_meta (k,v) VALUES "
            "('legacy_wallet_custody_v2',%s) "
            "ON CONFLICT(k) DO UPDATE SET v=excluded.v;"
            % _sql_literal("clean-v2:" + artifact_digest),
            "",
        ]
    )
    sql = "\n".join(lines)
    for record in records:
        if record.seed and record.seed in sql:
            raise CustodyMigrationError("secret_detected_in_scrub_sql")
    return sql


def _resolve_secret(env_name: str, prompt: str) -> str:
    value = os.environ.get(env_name, "")
    if value:
        return value
    if not sys.stdin.isatty():
        raise CustodyMigrationError("%s_required" % env_name.lower())
    return getpass.getpass(prompt)


def _rpc_url(env_name: str) -> str:
    value = os.environ.get(env_name, "").strip()
    if not value:
        raise CustodyMigrationError("%s_required" % env_name.lower())
    _rpc_origin(value)
    return value


def _command_inventory(args: argparse.Namespace) -> None:
    data_key = _resolve_secret(
        args.data_key_env,
        "Historical ForkMesh DATA_KEY: ",
    )
    records, scanned = scan_legacy_records(args.database, data_key)
    audit = build_public_audit(
        records,
        scanned,
        network=args.network,
        rpc_url=_rpc_url(args.rpc_url_env),
    )
    _write_public_file(args.output, audit)
    print(
        "Wrote redacted audit for %d key-bearing record(s) and %d address(es): %s"
        % (
            audit["keyBearingRecordCount"],
            audit["uniqueAddressCount"],
            args.output,
        )
    )


def _command_export(args: argparse.Namespace) -> None:
    data_key = _resolve_secret(
        args.data_key_env,
        "Historical ForkMesh DATA_KEY: ",
    )
    passphrase = _resolve_secret(
        args.artifact_passphrase_env,
        "New artifact passphrase (16+ characters): ",
    )
    if hmac.compare_digest(data_key, passphrase):
        raise CustodyMigrationError(
            "artifact_passphrase_must_differ_from_data_key"
        )
    records, scanned = scan_legacy_records(args.database, data_key)
    audit = build_public_audit(
        records,
        scanned,
        network=args.network,
        rpc_url=_rpc_url(args.rpc_url_env),
    )
    artifact = create_artifact(records, audit, passphrase)
    _write_public_file(args.audit_output, audit)
    _write_private_file(
        args.artifact,
        _canonical_json(artifact) + b"\n",
    )
    print(
        "Wrote encrypted artifact %s (sha256 %s) and redacted audit %s."
        % (args.artifact, artifact_sha256(artifact), args.audit_output)
    )
    print("No funds were moved and no D1 row was changed.")


def _command_prepare_scrub(args: argparse.Namespace) -> None:
    data_key = _resolve_secret(
        args.data_key_env,
        "Historical ForkMesh DATA_KEY: ",
    )
    passphrase = _resolve_secret(
        args.artifact_passphrase_env,
        "Artifact passphrase: ",
    )
    artifact = _read_json_file(args.artifact, error="invalid_artifact")
    artifact_digest = artifact_sha256(artifact)
    payload = open_artifact(artifact, passphrase)
    if args.network != artifact.get("network"):
        raise CustodyMigrationError("artifact_network_mismatch")
    confirmation = _read_json_file(
        args.confirmation,
        error="invalid_scrub_confirmation",
    )
    if (
        confirmation.get("format") != CONFIRMATION_FORMAT
        or confirmation.get("confirmation") != SCRUB_CONFIRMATION
        or confirmation.get("artifactSha256") != artifact_digest
        or confirmation.get("auditSha256") != artifact.get("auditSha256")
    ):
        raise CustodyMigrationError("invalid_scrub_confirmation")
    try:
        confirmed_at = int(confirmation["confirmedAt"])
    except Exception:
        raise CustodyMigrationError("invalid_scrub_confirmation") from None
    if confirmed_at <= 0:
        raise CustodyMigrationError("invalid_scrub_confirmation")

    records, scanned = scan_legacy_records(args.database, data_key)
    verify_artifact_matches_records(payload, records)
    audit = build_public_audit(
        records,
        scanned,
        network=args.network,
        rpc_url=_rpc_url(args.rpc_url_env),
    )
    nonzero = [
        item
        for item in audit["records"]
        if int(item["balanceLamports"]) != 0
    ]
    if nonzero:
        raise CustodyMigrationError(
            "refusing_to_scrub_%d_nonzero_address_record(s)" % len(nonzero)
        )
    if payload.get("auditSha256") != artifact.get("auditSha256"):
        raise CustodyMigrationError("artifact_audit_digest_mismatch")
    sql = build_scrub_sql(
        records,
        data_key,
        artifact_digest=artifact_digest,
        audit_digest=str(artifact["auditSha256"]),
        confirmed_at=confirmed_at,
        reconciliation_records=audit["records"],
        network=args.network,
    )
    _write_private_file(args.output_sql, sql.encode("utf-8"))
    print(
        "Prepared zero-balance, optimistic scrub SQL for %d record(s): %s"
        % (len(records), args.output_sql)
    )
    print("D1 was not contacted or changed; review and apply the SQL explicitly.")


def _add_common_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--database",
        type=Path,
        required=True,
        help="Read-only SQLite snapshot created from an explicit D1 export.",
    )
    parser.add_argument(
        "--network",
        required=True,
        choices=("mainnet-beta", "devnet", "testnet"),
        help="Network whose balances must be reconciled.",
    )
    parser.add_argument(
        "--data-key-env",
        default="FORKMESH_LEGACY_DATA_KEY",
        help="Environment variable containing historical DATA_KEY (never a key).",
    )
    parser.add_argument(
        "--rpc-url-env",
        default="FORKMESH_SOLANA_RPC_URL",
        help="Environment variable containing the HTTPS RPC URL.",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Audit and migrate historical ForkMesh Solana custody rows offline."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    inventory = subparsers.add_parser(
        "inventory",
        help="Write a redacted, balance-reconciled inventory.",
    )
    _add_common_arguments(inventory)
    inventory.add_argument("--output", type=Path, required=True)
    inventory.set_defaults(handler=_command_inventory)

    export = subparsers.add_parser(
        "export",
        help="Write a redacted audit and encrypted owner-controlled artifact.",
    )
    _add_common_arguments(export)
    export.add_argument("--artifact", type=Path, required=True)
    export.add_argument("--audit-output", type=Path, required=True)
    export.add_argument(
        "--artifact-passphrase-env",
        default="FORKMESH_CUSTODY_ARTIFACT_PASSPHRASE",
        help="Environment variable containing the new artifact passphrase.",
    )
    export.set_defaults(handler=_command_export)

    scrub = subparsers.add_parser(
        "prepare-scrub",
        help=(
            "After explicit confirmation and fresh zero-balance checks, write "
            "an optimistic D1 scrub SQL patch without applying it."
        ),
    )
    _add_common_arguments(scrub)
    scrub.add_argument("--artifact", type=Path, required=True)
    scrub.add_argument("--confirmation", type=Path, required=True)
    scrub.add_argument("--output-sql", type=Path, required=True)
    scrub.add_argument(
        "--artifact-passphrase-env",
        default="FORKMESH_CUSTODY_ARTIFACT_PASSPHRASE",
        help="Environment variable containing the artifact passphrase.",
    )
    scrub.set_defaults(handler=_command_prepare_scrub)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        args.handler(args)
    except CustodyMigrationError as error:
        # Every raised message is a fixed redaction-safe code. Never print an
        # exception repr or traceback: either could include a secret-bearing row.
        print("error: %s" % error, file=sys.stderr)
        return 2
    except FileExistsError:
        print("error: output_file_already_exists", file=sys.stderr)
        return 2
    except Exception:
        print("error: unexpected_migration_failure", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
