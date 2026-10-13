#!/usr/bin/env python3
"""Portable social signals v1: signed stars, follows, endorsements, revocation.

Reference Python implementation of www/docs/social-signals.md. It uses only the
standard library so a third party can copy this one file. The JavaScript
implementation (app/public/social-signals.js) is written separately against the
same document; both must agree on every vector in
conformance/social-signals/v1/vectors.json, and ``gate`` also round-trips a
bundle through both of them.

Commands:
  keygen --out SEED                write a fresh 32-byte Ed25519 seed (hex)
  sign --seed-file SEED RECORD     sign an unsigned signal record
  revoke --seed-file SEED ID       sign a revocation for one of your signals
  merge BUNDLE... [--audience]     import, merge, verify, and export a bundle
  verify BUNDLE...                 print the folded state; exit 1 on rejections
  recommend BUNDLE... --viewer K   explainable recommendations for a viewer
  vectors [--write|--check]        regenerate or compare the committed vectors
  gate                             vectors check + both implementations + interop
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import itertools
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import time
import unicodedata
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
VECTORS_PATH = ROOT / "conformance" / "social-signals" / "v1" / "vectors.json"
JS_RUNNER = ROOT / "app" / "tests" / "run_social_signal_vectors_js.js"

SIGNAL_SCHEMA = "forkmesh-social-signal-v1"
REVOCATION_SCHEMA = "forkmesh-social-revocation-v1"
BUNDLE_SCHEMA = "forkmesh-social-signal-bundle-v1"
STATE_SCHEMA = "forkmesh-social-signal-state-v1"
RECOMMENDATION_SCHEMA = "forkmesh-social-recommendations-v1"
VECTORS_SCHEMA = "forkmesh-social-signal-vectors-v1"

TIERS = {
    "star": "interest",
    "watch": "interest",
    "bookmark": "interest",
    "follow": "interest",
    "recommendation": "interest",
    "usage-endorsement": "attestation",
    "security-review": "attestation",
    "mirror-commitment": "attestation",
}
# Kinds that can support a recommendation, and how much one signer can add.
# A signer's contribution to a subject is the maximum of these, never the sum.
WEIGHTS = {
    "star": 1,
    "watch": 1,
    "recommendation": 2,
    "mirror-commitment": 2,
    "usage-endorsement": 3,
    "security-review": 3,
}
VISIBILITIES = ("public", "followers", "private")
AUDIENCES = {
    "public": ("public",),
    "followers": ("public", "followers"),
    "self": VISIBILITIES,
}
REASONS = ("withdrawn", "superseded", "compromised", "mistaken")
SIGNAL_KEYS = frozenset({
    "schema", "kind", "tier", "signer", "subject", "issuedAt", "expiresAt",
    "visibility", "scope", "evidence", "note", "signalId", "signature",
})
REVOCATION_KEYS = frozenset({
    "schema", "signer", "revokes", "revokedAt", "reason", "revocationId", "signature",
})
BUNDLE_KEYS = frozenset({"schema", "exportedAt", "signals", "revocations"})
CONTROL_KEYS = frozenset({"viewer", "enabled", "asOf", "limit", "mutedSigners", "mutedSubjects"})
EVIDENCE_KEYS = frozenset({"label", "uri", "sha256"})
SCOPE_KEYS = {
    "usage-endorsement": frozenset({"environment", "version", "since"}),
    "security-review": frozenset({"revision", "method", "findings"}),
    "mirror-commitment": frozenset({"endpoint", "protocols"}),
}
ENVIRONMENTS = ("production", "staging", "development")
REVIEW_METHODS = ("manual-audit", "automated-scan", "threat-model")
FINDINGS = ("none", "resolved", "open")
MIRROR_PROTOCOLS = ("forkmesh-relay", "git-https", "git-ssh")
MAX_CLOCK_SKEW_MS = 300_000
MAX_NOTE_BYTES = 1024
MAX_EVIDENCE = 16
MAX_BUNDLE_RECORDS = 10_000
MAX_CONTROL_MUTES = 1000
MAX_URI_BYTES = 2048

SAFE_INTEGER = 2**53 - 1
MAX_DEPTH = 32
KEY_RE = re.compile(r"[A-Za-z0-9_.:/-]{1,256}")
SHA_RE = re.compile(r"sha256:[0-9a-f]{64}")
REPOSITORY_RE = re.compile(r"urn:forkmesh:repository:sha256:[0-9a-f]{64}")
IDENTITY_PREFIX = "urn:forkmesh:identity:ed25519:"
REVISION_RE = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?")
B64_RE = re.compile(r"[A-Za-z0-9_-]*")
URI_RE = re.compile(r"https://[!-~]+")


class SignalError(Exception):
    """A rejection carrying the stable error code from the specification."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


# --------------------------------------------------------------------------
# Canonical JSON (same rules as the protocol suite's FM-ENC-001..005)

_SHORT_ESCAPES = {
    '"': '\\"', "\\": "\\\\", "\b": "\\b", "\f": "\\f",
    "\n": "\\n", "\r": "\\r", "\t": "\\t",
}


def normalize(value: Any, depth: int = 0) -> Any:
    if depth > MAX_DEPTH:
        raise SignalError("json_too_deep")
    if value is None or value is True or value is False:
        return value
    if isinstance(value, int):
        if not -SAFE_INTEGER <= value <= SAFE_INTEGER:
            raise SignalError("invalid_number")
        return value
    if isinstance(value, float):
        raise SignalError("invalid_number")
    if isinstance(value, str):
        normalized = unicodedata.normalize("NFC", value)
        if any(0xD800 <= ord(char) <= 0xDFFF for char in normalized):
            raise SignalError("invalid_unicode")
        return normalized
    if isinstance(value, list):
        return [normalize(item, depth + 1) for item in value]
    if isinstance(value, dict):
        result: dict[str, Any] = {}
        for key, item in value.items():
            if not isinstance(key, str) or not KEY_RE.fullmatch(key):
                raise SignalError("invalid_key")
            result[key] = normalize(item, depth + 1)
        return result
    raise SignalError("invalid_json_type")


def _encode_string(value: str) -> str:
    parts = ['"']
    for char in value:
        if char in _SHORT_ESCAPES:
            parts.append(_SHORT_ESCAPES[char])
        elif ord(char) < 0x20:
            parts.append(f"\\u{ord(char):04x}")
        else:
            parts.append(char)
    parts.append('"')
    return "".join(parts)


def _encode(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return _encode_string(value)
    if isinstance(value, list):
        return "[" + ",".join(_encode(item) for item in value) + "]"
    return "{" + ",".join(
        _encode_string(key) + ":" + _encode(value[key]) for key in sorted(value)
    ) + "}"


def canonical_bytes(value: Any) -> bytes:
    return _encode(normalize(value)).encode("utf-8")


def _digest(domain: str, value: Any) -> str:
    return "sha256:" + hashlib.sha256(domain.encode("utf-8") + b"\n" + canonical_bytes(value)).hexdigest()


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def b64url_decode(value: Any, size: int, code: str) -> bytes:
    if not isinstance(value, str) or not B64_RE.fullmatch(value) or len(value) % 4 == 1:
        raise SignalError(code)
    data = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    if len(data) != size or b64url(data) != value:
        raise SignalError(code)
    return data


# --------------------------------------------------------------------------
# Ed25519 (RFC 8032) with the protocol suite's FM-SIG-001 strictness

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)
_IDENTITY = (0, 1, 1, 0)


def _add(p: tuple[int, ...], q: tuple[int, ...]) -> tuple[int, ...]:
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(scalar: int, point: tuple[int, ...]) -> tuple[int, ...]:
    result = _IDENTITY
    while scalar:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def _equal(p: tuple[int, ...], q: tuple[int, ...]) -> bool:
    return (p[0] * q[2] - q[0] * p[2]) % _P == 0 and (p[1] * q[2] - q[1] * p[2]) % _P == 0


def _decode_point(raw: bytes) -> tuple[int, ...] | None:
    number = int.from_bytes(raw, "little")
    y, sign = number & ((1 << 255) - 1), number >> 255
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        if sign:
            return None
        return (0, y, 1, 0)
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    if (x & 1) != sign:
        x = _P - x
    return (x, y, 1, x * y % _P)


def _encode_point(point: tuple[int, ...]) -> bytes:
    inverse = pow(point[2], _P - 2, _P)
    x, y = point[0] * inverse % _P, point[1] * inverse % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


_BASE = _decode_point((4 * pow(5, _P - 2, _P) % _P).to_bytes(32, "little"))
assert _BASE is not None


def _secret(seed: bytes) -> tuple[int, bytes, bytes]:
    if len(seed) != 32:
        raise SignalError("invalid_seed")
    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return scalar, digest[32:], _encode_point(_mul(scalar, _BASE))


def public_key(seed: bytes) -> str:
    return b64url(_secret(seed)[2])


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    scalar, prefix, public = _secret(seed)
    nonce = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little") % _L
    encoded_r = _encode_point(_mul(nonce, _BASE))
    challenge = int.from_bytes(hashlib.sha512(encoded_r + public + message).digest(), "little") % _L
    return encoded_r + ((nonce + challenge * scalar) % _L).to_bytes(32, "little")


def ed25519_verify(public: bytes, signature: bytes, message: bytes) -> bool:
    point = _decode_point(public)
    nonce = _decode_point(signature[:32])
    scalar = int.from_bytes(signature[32:], "little")
    if point is None or nonce is None or scalar >= _L:
        return False
    if _equal(point, _IDENTITY) or not _equal(_mul(_L, point), _IDENTITY):
        return False
    challenge = int.from_bytes(hashlib.sha512(signature[:32] + public + message).digest(), "little") % _L
    return _equal(_mul(scalar, _BASE), _add(nonce, _mul(challenge, point)))


# --------------------------------------------------------------------------
# Records


def _safe_nonnegative(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= SAFE_INTEGER


def _text(value: Any, minimum: int, maximum: int) -> bool:
    return isinstance(value, str) and minimum <= len(value.encode("utf-8")) <= maximum


def _https_uri(value: Any) -> bool:
    return isinstance(value, str) and len(value.encode("utf-8")) <= MAX_URI_BYTES and bool(URI_RE.fullmatch(value))


def _identity_key(subject: Any) -> str | None:
    if not isinstance(subject, str) or not subject.startswith(IDENTITY_PREFIX):
        return None
    key = subject[len(IDENTITY_PREFIX):]
    try:
        b64url_decode(key, 32, "invalid_subject")
    except SignalError:
        return None
    return key


def _unsigned(record: dict[str, Any], *drop: str) -> dict[str, Any]:
    return {key: value for key, value in record.items() if key not in drop}


def signal_id(record: dict[str, Any]) -> str:
    return _digest(SIGNAL_SCHEMA, _unsigned(record, "signalId", "signature"))


def revocation_id(record: dict[str, Any]) -> str:
    return _digest(REVOCATION_SCHEMA, _unsigned(record, "revocationId", "signature"))


def _validate_scope(kind: str, record: dict[str, Any]) -> None:
    scope, evidence = record["scope"], record["evidence"]
    if TIERS[kind] == "interest":
        if scope != {} or evidence != []:
            raise SignalError("interest_claims_scope")
        return
    if not isinstance(scope, dict) or set(scope) != SCOPE_KEYS[kind]:
        raise SignalError("invalid_scope")
    if kind == "usage-endorsement":
        ok = (scope["environment"] in ENVIRONMENTS and _text(scope["version"], 1, 64)
              and _safe_nonnegative(scope["since"]) and scope["since"] <= record["issuedAt"])
    elif kind == "security-review":
        ok = (isinstance(scope["revision"], str) and bool(REVISION_RE.fullmatch(scope["revision"]))
              and scope["method"] in REVIEW_METHODS and scope["findings"] in FINDINGS)
    else:
        protocols = scope["protocols"]
        ok = (_https_uri(scope["endpoint"]) and isinstance(protocols, list) and bool(protocols)
              and all(item in MIRROR_PROTOCOLS for item in protocols)
              and protocols == sorted(set(protocols)))
    if not ok:
        raise SignalError("invalid_scope")
    if not isinstance(evidence, list):
        raise SignalError("invalid_evidence")
    if not evidence:
        raise SignalError("evidence_required")
    if len(evidence) > MAX_EVIDENCE:
        raise SignalError("invalid_evidence")
    for item in evidence:
        if (not isinstance(item, dict) or set(item) != EVIDENCE_KEYS or not _text(item["label"], 1, 120)
                or not _https_uri(item["uri"])
                or not (item["sha256"] is None or (isinstance(item["sha256"], str) and SHA_RE.fullmatch(item["sha256"])))):
            raise SignalError("invalid_evidence")


def validate_signal(raw: Any, as_of: int) -> dict[str, Any]:
    """Return the normalized record, or raise the first failing rule's code."""

    record = normalize(raw)
    if not isinstance(record, dict):
        raise SignalError("invalid_record")
    if set(record) != SIGNAL_KEYS:
        raise SignalError("invalid_record_keys")
    if record["schema"] != SIGNAL_SCHEMA:
        raise SignalError("unsupported_schema")
    kind = record["kind"]
    if not isinstance(kind, str) or kind not in TIERS:
        raise SignalError("unknown_kind")
    if record["tier"] != TIERS[kind]:
        raise SignalError("tier_mismatch")
    signer = b64url_decode(record["signer"], 32, "invalid_signer")
    if kind == "follow":
        followed = _identity_key(record["subject"])
        if followed is None:
            raise SignalError("invalid_subject")
        if followed == record["signer"]:
            raise SignalError("self_follow")
    elif not isinstance(record["subject"], str) or not REPOSITORY_RE.fullmatch(record["subject"]):
        raise SignalError("invalid_subject")
    if not _safe_nonnegative(record["issuedAt"]):
        raise SignalError("invalid_issued_at")
    expires = record["expiresAt"]
    if expires is not None and not (_safe_nonnegative(expires) and expires > record["issuedAt"]):
        raise SignalError("invalid_expires_at")
    if kind == "mirror-commitment" and expires is None:
        raise SignalError("commitment_requires_expiry")
    if record["visibility"] not in VISIBILITIES:
        raise SignalError("invalid_visibility")
    if not _text(record["note"], 0, MAX_NOTE_BYTES):
        raise SignalError("invalid_note")
    _validate_scope(kind, record)
    signature = b64url_decode(record["signature"], 64, "invalid_signature_encoding")
    if record["signalId"] != signal_id(record):
        raise SignalError("signal_id_mismatch")
    message = SIGNAL_SCHEMA.encode() + b"\n" + canonical_bytes(_unsigned(record, "signalId", "signature"))
    if not ed25519_verify(signer, signature, message):
        raise SignalError("invalid_signature")
    if record["issuedAt"] > as_of + MAX_CLOCK_SKEW_MS:
        raise SignalError("issued_in_future")
    return record


def validate_revocation(raw: Any, as_of: int) -> dict[str, Any]:
    record = normalize(raw)
    if not isinstance(record, dict):
        raise SignalError("invalid_record")
    if set(record) != REVOCATION_KEYS:
        raise SignalError("invalid_record_keys")
    if record["schema"] != REVOCATION_SCHEMA:
        raise SignalError("unsupported_schema")
    signer = b64url_decode(record["signer"], 32, "invalid_signer")
    if not isinstance(record["revokes"], str) or not SHA_RE.fullmatch(record["revokes"]):
        raise SignalError("invalid_revokes")
    if not _safe_nonnegative(record["revokedAt"]):
        raise SignalError("invalid_revoked_at")
    if record["reason"] not in REASONS:
        raise SignalError("invalid_reason")
    signature = b64url_decode(record["signature"], 64, "invalid_signature_encoding")
    if record["revocationId"] != revocation_id(record):
        raise SignalError("revocation_id_mismatch")
    message = REVOCATION_SCHEMA.encode() + b"\n" + canonical_bytes(_unsigned(record, "revocationId", "signature"))
    if not ed25519_verify(signer, signature, message):
        raise SignalError("invalid_signature")
    if record["revokedAt"] > as_of + MAX_CLOCK_SKEW_MS:
        raise SignalError("issued_in_future")
    return record


def sign_signal(seed: bytes, unsigned: dict[str, Any]) -> dict[str, Any]:
    record = normalize({**_unsigned(unsigned, "signalId", "signature"), "signer": public_key(seed)})
    record.setdefault("schema", SIGNAL_SCHEMA)
    record.setdefault("tier", TIERS.get(record.get("kind"), "interest"))
    record.setdefault("expiresAt", None)
    record.setdefault("visibility", "public")
    record.setdefault("scope", {})
    record.setdefault("evidence", [])
    record.setdefault("note", "")
    message = SIGNAL_SCHEMA.encode() + b"\n" + canonical_bytes(record)
    record["signalId"] = signal_id(record)
    record["signature"] = b64url(ed25519_sign(seed, message))
    return record


def sign_revocation(seed: bytes, revokes: str, revoked_at: int, reason: str = "withdrawn") -> dict[str, Any]:
    record = {
        "schema": REVOCATION_SCHEMA,
        "signer": public_key(seed),
        "revokes": revokes,
        "revokedAt": revoked_at,
        "reason": reason,
    }
    message = REVOCATION_SCHEMA.encode() + b"\n" + canonical_bytes(record)
    record["revocationId"] = revocation_id(record)
    record["signature"] = b64url(ed25519_sign(seed, message))
    return record


def describe(record: dict[str, Any]) -> str:
    """One line a client shows next to the signal. Attestations always name
    their category, scope, and evidence count so they never read as a star."""

    kind, scope = record["kind"], record["scope"]
    evidence = f"({len(record['evidence'])} evidence)"
    if kind == "usage-endorsement":
        return f"Usage endorsement: {scope['environment']} use of {scope['version']} {evidence}"
    if kind == "security-review":
        return (f"Security review: {scope['method']} of {scope['revision'][:12]}, "
                f"findings {scope['findings']} {evidence}")
    if kind == "mirror-commitment":
        return f"Mirror commitment: {scope['endpoint']} until {record['expiresAt']} {evidence}"
    return {"star": "Star", "watch": "Watching", "bookmark": "Bookmark",
            "follow": "Follow", "recommendation": "Recommendation"}[kind]


# --------------------------------------------------------------------------
# Signal store: import, merge, fold, export


class SignalStore:
    """A set of verified records. Merging is a set union keyed by content
    address, so any import order yields the same state digest."""

    def __init__(self) -> None:
        self.signals: dict[str, dict[str, Any]] = {}
        self.revocations: dict[str, dict[str, Any]] = {}

    def import_bundle(self, raw: Any, as_of: int) -> dict[str, Any]:
        if not isinstance(raw, dict) or set(raw) != BUNDLE_KEYS:
            raise SignalError("invalid_bundle")
        if raw["schema"] != BUNDLE_SCHEMA:
            raise SignalError("unsupported_bundle_schema")
        if (not _safe_nonnegative(raw["exportedAt"]) or not isinstance(raw["signals"], list)
                or not isinstance(raw["revocations"], list)):
            raise SignalError("invalid_bundle")
        if len(raw["signals"]) + len(raw["revocations"]) > MAX_BUNDLE_RECORDS:
            raise SignalError("bundle_too_large")
        report: dict[str, Any] = {"accepted": 0, "duplicate": 0, "rejected": []}
        for kind, records, validate, id_key, target in (
            ("signal", raw["signals"], validate_signal, "signalId", self.signals),
            ("revocation", raw["revocations"], validate_revocation, "revocationId", self.revocations),
        ):
            for item in records:
                try:
                    record = validate(item, as_of)
                except SignalError as error:
                    claimed = item.get(id_key) if isinstance(item, dict) else None
                    report["rejected"].append({
                        "type": kind,
                        "id": claimed if isinstance(claimed, str) else None,
                        "error": error.code,
                    })
                    continue
                if record[id_key] in target:
                    report["duplicate"] += 1
                else:
                    target[record[id_key]] = record
                    report["accepted"] += 1
        return report

    def digest(self) -> str:
        return _digest(STATE_SCHEMA, {
            "revocations": sorted(self.revocations),
            "signals": sorted(self.signals),
        })

    def fold(self, as_of: int) -> dict[str, Any]:
        revoked_by: dict[str, str] = {}
        revocations = []
        for rid in sorted(self.revocations):
            revocation = self.revocations[rid]
            target = self.signals.get(revocation["revokes"])
            if target is None:
                status = "pending"
            elif target["signer"] != revocation["signer"]:
                status = "signer_mismatch"
            else:
                status = "applied"
                revoked_by.setdefault(revocation["revokes"], rid)
            revocations.append({"revocationId": rid, "revokes": revocation["revokes"], "status": status})
        latest: dict[tuple[str, str, str], dict[str, Any]] = {}
        for record in self.signals.values():
            key = (record["signer"], record["kind"], record["subject"])
            current = latest.get(key)
            if current is None or (record["issuedAt"], record["signalId"]) > (current["issuedAt"], current["signalId"]):
                latest[key] = record
        signals = []
        for sid in sorted(self.signals):
            record = self.signals[sid]
            newest = latest[(record["signer"], record["kind"], record["subject"])]["signalId"]
            if sid in revoked_by:
                status = "revoked"
            elif newest != sid:
                status = "superseded"
            elif record["expiresAt"] is not None and record["expiresAt"] <= as_of:
                status = "expired"
            else:
                status = "active"
            signals.append({
                "signalId": sid,
                "kind": record["kind"],
                "tier": record["tier"],
                "signer": record["signer"],
                "subject": record["subject"],
                "issuedAt": record["issuedAt"],
                "expiresAt": record["expiresAt"],
                "visibility": record["visibility"],
                "status": status,
                "revokedBy": revoked_by.get(sid),
                "supersededBy": None if newest == sid else newest,
            })
        return {
            "schema": STATE_SCHEMA,
            "asOf": as_of,
            "digest": self.digest(),
            "signals": signals,
            "revocations": revocations,
        }

    def export_bundle(self, audience: str, exported_at: int, signer: str | None = None) -> dict[str, Any]:
        if audience not in AUDIENCES:
            raise SignalError("invalid_audience")
        allowed = AUDIENCES[audience]
        signals = [
            self.signals[sid] for sid in sorted(self.signals)
            if self.signals[sid]["visibility"] in allowed
            and (signer is None or self.signals[sid]["signer"] == signer)
        ]
        included = {record["signalId"] for record in signals}
        revocations = [
            self.revocations[rid] for rid in sorted(self.revocations)
            if (signer is None or self.revocations[rid]["signer"] == signer)
            and (self.revocations[rid]["revokes"] in included
                 or (audience == "self" and self.revocations[rid]["revokes"] not in self.signals))
        ]
        return {
            "schema": BUNDLE_SCHEMA,
            "exportedAt": exported_at,
            "signals": signals,
            "revocations": revocations,
        }

    def recommend(self, controls: Any) -> dict[str, Any]:
        if not isinstance(controls, dict) or set(controls) != CONTROL_KEYS:
            raise SignalError("invalid_controls")
        viewer = controls["viewer"]
        b64url_decode(viewer, 32, "invalid_controls")
        muted_signers, muted_subjects = controls["mutedSigners"], controls["mutedSubjects"]
        if (not isinstance(controls["enabled"], bool) or not _safe_nonnegative(controls["asOf"])
                or not isinstance(controls["limit"], int) or isinstance(controls["limit"], bool)
                or not 1 <= controls["limit"] <= 100
                or not isinstance(muted_signers, list) or not isinstance(muted_subjects, list)
                or len(muted_signers) > MAX_CONTROL_MUTES or len(muted_subjects) > MAX_CONTROL_MUTES
                or not all(isinstance(item, str) for item in muted_signers + muted_subjects)):
            raise SignalError("invalid_controls")
        excluded = {key: 0 for key in (
            "inactive", "mutedSigner", "untrusted", "mutedSubject", "private", "alreadyKnown")}
        result = {
            "schema": RECOMMENDATION_SCHEMA,
            "viewer": viewer,
            "enabled": controls["enabled"],
            "considered": 0,
            "excluded": excluded,
            "recommendations": [],
        }
        if not controls["enabled"]:
            return result
        state = self.fold(controls["asOf"])
        active = [item for item in state["signals"] if item["status"] == "active"]
        trusted = {
            item["subject"][len(IDENTITY_PREFIX):] for item in active
            if item["kind"] == "follow" and item["signer"] == viewer
        }
        known = {item["subject"] for item in active if item["signer"] == viewer}
        muted_signer_set, muted_subject_set = set(muted_signers), set(muted_subjects)
        supporters: dict[str, dict[str, list[dict[str, Any]]]] = {}
        for item in state["signals"]:
            if item["kind"] not in WEIGHTS or item["signer"] == viewer:
                continue
            result["considered"] += 1
            if item["status"] != "active":
                reason = "inactive"
            elif item["signer"] in muted_signer_set:
                reason = "mutedSigner"
            elif item["signer"] not in trusted:
                reason = "untrusted"
            elif item["subject"] in muted_subject_set:
                reason = "mutedSubject"
            elif item["visibility"] == "private":
                reason = "private"
            elif item["subject"] in known:
                reason = "alreadyKnown"
            else:
                record = self.signals[item["signalId"]]
                supporters.setdefault(item["subject"], {}).setdefault(item["signer"], []).append({
                    "signalId": item["signalId"],
                    "signer": item["signer"],
                    "kind": item["kind"],
                    "tier": item["tier"],
                    "weight": WEIGHTS[item["kind"]],
                    "summary": describe(record),
                    "scope": record["scope"],
                    "evidence": record["evidence"],
                })
                continue
            excluded[reason] += 1
        ranked = []
        for subject, by_signer in supporters.items():
            ranked.append({
                "subject": subject,
                "score": sum(max(reason["weight"] for reason in reasons) for reasons in by_signer.values()),
                "supporters": len(by_signer),
                "reasons": [reason for signer in sorted(by_signer) for reason in by_signer[signer]],
            })
        ranked.sort(key=lambda entry: (-entry["score"], entry["subject"]))
        result["recommendations"] = ranked[:controls["limit"]]
        return result


# --------------------------------------------------------------------------
# Conformance vectors


def _seed(name: str) -> bytes:
    return hashlib.sha256(b"forkmesh-social-signal-test-key\n" + name.encode()).digest()


def _repository(name: str) -> str:
    return "urn:forkmesh:repository:sha256:" + hashlib.sha256(name.encode()).hexdigest()


def _resign_raw(seed: bytes, record: dict[str, Any], id_key: str, domain: str) -> dict[str, Any]:
    """Re-sign a (possibly invalid) record without validation, for negative vectors."""

    unsigned = _unsigned(record, id_key, "signature")
    message = domain.encode() + b"\n" + canonical_bytes(unsigned)
    return {**unsigned, id_key: _digest(domain, unsigned), "signature": b64url(ed25519_sign(seed, message))}


def _record_case(case_id: str, record: Any, as_of: int, validate=validate_signal) -> dict[str, Any]:
    try:
        normalized = validate(record, as_of)
    except SignalError as error:
        expect: dict[str, Any] = {"valid": False, "error": error.code}
    else:
        if validate is validate_signal:
            expect = {"valid": True, "signalId": normalized["signalId"], "summary": describe(normalized)}
        else:
            expect = {"valid": True, "revocationId": normalized["revocationId"]}
    return {"id": case_id, "asOf": as_of, "input": record, "expect": expect}


def build_vectors() -> dict[str, Any]:
    names = ("alice", "bob", "carol", "dave", "mallory")
    seeds = {name: _seed(name) for name in names}
    keys = {name: public_key(seed) for name, seed in seeds.items()}
    t0 = 1_767_225_600_000  # 2026-01-01T00:00:00Z
    day = 86_400_000
    as_of = t0 + 30 * day
    repo_a, repo_b, repo_c, repo_d = (_repository(name) for name in ("atlas", "beacon", "cinder", "delta"))
    commit = hashlib.sha1(b"beacon-release").hexdigest()

    def sign(name: str, **fields: Any) -> dict[str, Any]:
        return sign_signal(seeds[name], fields)

    def follow(name: str, target: str, at: int, visibility: str = "public") -> dict[str, Any]:
        return sign(name, kind="follow", subject=IDENTITY_PREFIX + keys[target], issuedAt=at, visibility=visibility)

    star = sign("bob", kind="star", subject=repo_a, issuedAt=t0 + day, note="Great docs")
    usage = sign("bob", kind="usage-endorsement", subject=repo_b, issuedAt=t0 + 2 * day,
                 scope={"environment": "production", "version": "2.4.1", "since": t0},
                 evidence=[{"label": "Deployment manifest", "uri": "https://example.org/bob/deploy.yaml",
                            "sha256": "sha256:" + hashlib.sha256(b"deploy").hexdigest()}])
    review = sign("carol", kind="security-review", subject=repo_b, issuedAt=t0 + 3 * day,
                  scope={"revision": commit, "method": "manual-audit", "findings": "resolved"},
                  evidence=[{"label": "Audit report", "uri": "https://example.org/carol/beacon-audit.pdf",
                             "sha256": None},
                            {"label": "Fix pull request", "uri": "https://app.forkmesh.com/beacon/beacon/pulls/7",
                             "sha256": None}])
    mirror = sign("dave", kind="mirror-commitment", subject=repo_c, issuedAt=t0 + day, expiresAt=t0 + 365 * day,
                  scope={"endpoint": "https://mirror.example.net/cinder.git", "protocols": ["git-https", "git-ssh"]},
                  evidence=[{"label": "Mirror health", "uri": "https://mirror.example.net/health", "sha256": None}])
    lapsed_mirror = sign("dave", kind="mirror-commitment", subject=repo_d, issuedAt=t0 + day, expiresAt=t0 + 10 * day,
                         scope={"endpoint": "https://mirror.example.net/delta.git", "protocols": ["forkmesh-relay"]},
                         evidence=[{"label": "Mirror health", "uri": "https://mirror.example.net/health", "sha256": None}])
    private_bookmark = sign("alice", kind="bookmark", subject=repo_d, issuedAt=t0 + day, visibility="private")
    followers_watch = sign("carol", kind="watch", subject=repo_c, issuedAt=t0 + 4 * day, visibility="followers")
    carol_recommend = sign("carol", kind="recommendation", subject=repo_a, issuedAt=t0 + 5 * day,
                           note="Pairs well with beacon")
    old_star = sign("carol", kind="star", subject=repo_c, issuedAt=t0 + day)
    new_star = sign("carol", kind="star", subject=repo_c, issuedAt=t0 + 6 * day)
    revoked_star = sign("bob", kind="star", subject=repo_d, issuedAt=t0 + day)
    revoke_star = sign_revocation(seeds["bob"], revoked_star["signalId"], t0 + 7 * day, "mistaken")
    forged_revoke = sign_revocation(seeds["mallory"], star["signalId"], t0 + 7 * day, "compromised")
    pending_revoke = sign_revocation(seeds["bob"], "sha256:" + "ab" * 32, t0 + 7 * day)
    mallory_star = sign("mallory", kind="star", subject=repo_d, issuedAt=t0 + day)
    alice_follows = [follow("alice", "bob", t0), follow("alice", "carol", t0), follow("alice", "dave", t0)]
    alice_star = sign("alice", kind="star", subject=repo_a, issuedAt=t0 + 2 * day)
    bob_follows_mallory = follow("bob", "mallory", t0, visibility="followers")

    records = []

    def add(case_id: str, record: Any, at: int = as_of) -> None:
        records.append(_record_case(case_id, record, at))

    for case_id, record in (
        ("interest-star", star), ("attestation-usage-endorsement", usage),
        ("attestation-security-review", review), ("attestation-mirror-commitment", mirror),
        ("private-bookmark", private_bookmark), ("follow", alice_follows[0]),
        ("recommendation-with-note", carol_recommend),
    ):
        add(case_id, record)
    composed = sign("bob", kind="star", subject=repo_b, issuedAt=t0, note="Caf\u00e9 \u2713")
    add("decomposed-note-normalizes-to-signed-form", {**composed, "note": "Cafe\u0301 \u2713"})

    def tampered(case_id: str, base: dict[str, Any], seed_name: str | None, **changes: Any) -> None:
        record = {**base, **changes}
        if seed_name is not None:
            record = _resign_raw(seeds[seed_name], record, "signalId", SIGNAL_SCHEMA)
        add(case_id, record)

    tampered("reject-star-claims-attestation-tier", star, "bob", tier="attestation")
    tampered("reject-endorsement-claims-interest-tier", usage, "bob", tier="interest")
    tampered("reject-star-with-scope", star, "bob", scope={"environment": "production"})
    tampered("reject-star-with-evidence", star, "bob", evidence=usage["evidence"])
    tampered("reject-endorsement-without-evidence", usage, "bob", evidence=[])
    tampered("reject-endorsement-unknown-environment", usage, "bob",
             scope={**usage["scope"], "environment": "somewhere"})
    tampered("reject-endorsement-since-after-issue", usage, "bob",
             scope={**usage["scope"], "since": usage["issuedAt"] + 1})
    tampered("reject-review-short-revision", review, "carol", scope={**review["scope"], "revision": commit[:12]})
    tampered("reject-evidence-plain-http", review, "carol",
             evidence=[{"label": "Audit", "uri": "http://example.org/audit", "sha256": None}])
    tampered("reject-evidence-extra-key", review, "carol",
             evidence=[{**review["evidence"][0], "trusted": True}])
    tampered("reject-mirror-without-expiry", mirror, "dave", expiresAt=None)
    tampered("reject-mirror-unsorted-protocols", mirror, "dave",
             scope={**mirror["scope"], "protocols": ["git-ssh", "git-https"]})
    tampered("reject-expiry-before-issue", star, "bob", expiresAt=star["issuedAt"])
    tampered("reject-unknown-kind", star, "bob", kind="like")
    tampered("reject-follow-repository-subject", alice_follows[0], "alice", subject=repo_a)
    tampered("reject-star-identity-subject", star, "bob", subject=IDENTITY_PREFIX + keys["alice"])
    tampered("reject-self-follow", alice_follows[0], "alice", subject=IDENTITY_PREFIX + keys["alice"])
    tampered("reject-unknown-visibility", star, "bob", visibility="unlisted")
    tampered("reject-oversized-note", star, "bob", note="x" * (MAX_NOTE_BYTES + 1))
    tampered("reject-unsupported-schema", star, "bob", schema="forkmesh-social-signal-v2")
    tampered("reject-note-edited-after-signing", star, None, note="Terrible docs")
    substituted = {**star, "signer": keys["mallory"]}
    add("reject-signer-substituted", {**substituted, "signalId": signal_id(substituted)})
    tampered("reject-signature-by-other-key", {**star, "signer": keys["bob"]}, "mallory", signer=keys["bob"])
    add("reject-small-order-signer", _resign_raw(
        seeds["mallory"], {**star, "signer": b64url((1).to_bytes(32, "little"))}, "signalId", SIGNAL_SCHEMA))
    malleable = int.from_bytes(b64url_decode(star["signature"], 64, "x")[32:], "little") + _L
    add("reject-signature-s-plus-l", {
        **star, "signature": b64url(b64url_decode(star["signature"], 64, "x")[:32] + malleable.to_bytes(32, "little"))})
    add("reject-short-signature", {**star, "signature": star["signature"][:80]})
    add("reject-missing-field", _unsigned(star, "note"))
    add("reject-extra-field", {**star, "score": 5})
    add("reject-fractional-timestamp", {**star, "issuedAt": 1.5})
    add("reject-unsafe-integer", {**star, "issuedAt": 2**53})
    add("reject-issued-in-future", star, star["issuedAt"] - MAX_CLOCK_SKEW_MS - 1)
    add("accept-within-clock-skew", star, star["issuedAt"] - MAX_CLOCK_SKEW_MS)

    revocation_cases = []
    for case_id, record, at in (
        ("revocation-valid", revoke_star, as_of),
        ("revocation-pending-target", pending_revoke, as_of),
        ("reject-revocation-tampered-target",
         {**revoke_star, "revokes": star["signalId"]}, as_of),
        ("reject-revocation-other-key", _resign_raw(
            seeds["mallory"], {**revoke_star}, "revocationId", REVOCATION_SCHEMA), as_of),
        ("reject-revocation-unknown-reason", _resign_raw(
            seeds["bob"], {**revoke_star, "reason": "because"}, "revocationId", REVOCATION_SCHEMA), as_of),
        ("reject-revocation-invalid-target", _resign_raw(
            seeds["bob"], {**revoke_star, "revokes": "sha1:abc"}, "revocationId", REVOCATION_SCHEMA), as_of),
        ("reject-revocation-in-future", revoke_star, revoke_star["revokedAt"] - MAX_CLOCK_SKEW_MS - 1),
    ):
        revocation_cases.append(_record_case(case_id, record, at, validate_revocation))

    def bundle(signals: list[Any], revocations: list[Any], exported_at: int = as_of) -> dict[str, Any]:
        return {"schema": BUNDLE_SCHEMA, "exportedAt": exported_at, "signals": signals, "revocations": revocations}

    bundles = [
        bundle(alice_follows + [alice_star, private_bookmark], []),
        bundle([star, usage, revoked_star, bob_follows_mallory, star], [revoke_star, pending_revoke]),
        bundle([review, followers_watch, carol_recommend, old_star, new_star], [forged_revoke]),
        bundle([mirror, lapsed_mirror, mallory_star, usage,
                {**star, "note": "Terrible docs"}], [{**revoke_star, "reason": "because"}]),
    ]
    viewer = keys["alice"]

    def controls(**changes: Any) -> dict[str, Any]:
        base = {"viewer": viewer, "enabled": True, "asOf": as_of, "limit": 10,
                "mutedSigners": [], "mutedSubjects": []}
        return {**base, **changes}

    control_cases = [
        ("default", controls()),
        ("disabled", controls(enabled=False)),
        ("mute-carol", controls(mutedSigners=[keys["carol"]])),
        ("mute-beacon", controls(mutedSubjects=[repo_b])),
        ("limit-one", controls(limit=1)),
        ("before-mirror-lapse", controls(asOf=t0 + 5 * day)),
        ("stranger-viewer", controls(viewer=keys["mallory"])),
    ]

    scenario_store = SignalStore()
    imports = [scenario_store.import_bundle(item, as_of) for item in bundles]
    scenario = {
        "id": "four-services-merge",
        "asOf": as_of,
        "bundles": bundles,
        "orders": [list(order) for order in itertools.permutations(range(len(bundles)))],
        "expect": {
            "imports": imports,
            "state": scenario_store.fold(as_of),
            "exports": {
                "public": scenario_store.export_bundle("public", as_of),
                "followers": scenario_store.export_bundle("followers", as_of),
                "self": scenario_store.export_bundle("self", as_of),
                "self-alice": scenario_store.export_bundle("self", as_of, viewer),
            },
            "recommendations": [
                {"id": case_id, "controls": value, "output": scenario_store.recommend(value)}
                for case_id, value in control_cases
            ],
        },
    }

    invalid_controls = []
    for case_id, value in (
        ("controls-missing-key", _unsigned(controls(), "mutedSubjects")),
        ("controls-limit-zero", controls(limit=0)),
        ("controls-enabled-string", controls(enabled="no")),
        ("controls-bad-viewer", controls(viewer="alice")),
    ):
        try:
            scenario_store.recommend(value)
        except SignalError as error:
            invalid_controls.append({"id": case_id, "controls": value, "error": error.code})
        else:
            raise AssertionError(case_id)

    invalid_bundles = []
    for case_id, value in (
        ("bundle-not-object", []),
        ("bundle-wrong-schema", {**bundle([], []), "schema": "forkmesh-stars-v1"}),
        ("bundle-missing-revocations", _unsigned(bundle([], []), "revocations")),
        ("bundle-too-large", bundle([{}] * (MAX_BUNDLE_RECORDS + 1), [])),
    ):
        try:
            SignalStore().import_bundle(value, as_of)
        except SignalError as error:
            invalid_bundles.append({"id": case_id, "error": error.code,
                                    "input": value if case_id != "bundle-too-large" else None,
                                    "emptySignals": MAX_BUNDLE_RECORDS + 1 if case_id == "bundle-too-large" else 0})
        else:
            raise AssertionError(case_id)

    return {
        "schema": VECTORS_SCHEMA,
        "version": "1.0.0",
        "specification": "www/docs/social-signals.md",
        "note": "Test keys only. Seeds are SHA-256 of 'forkmesh-social-signal-test-key\\n' + name.",
        "keys": {name: {"seed": seeds[name].hex(), "publicKey": keys[name]} for name in names},
        "records": records,
        "revocations": revocation_cases,
        "scenarios": [scenario],
        "invalidControls": invalid_controls,
        "invalidBundles": invalid_bundles,
    }


def _read_vectors_text() -> str:
    # A Windows checkout may convert line endings; the content is what counts.
    return VECTORS_PATH.read_text(encoding="utf-8").replace("\r\n", "\n") if VECTORS_PATH.exists() else ""


def _render_vectors(vectors: dict[str, Any]) -> str:
    return json.dumps(vectors, indent=1, ensure_ascii=False, sort_keys=True) + "\n"


def _bundle_input(case: dict[str, Any]) -> Any:
    if case["emptySignals"]:
        return {"schema": BUNDLE_SCHEMA, "exportedAt": 0, "signals": [{}] * case["emptySignals"], "revocations": []}
    return case["input"]


def run_vectors(vectors: dict[str, Any]) -> list[dict[str, Any]]:
    """Check this implementation against the committed vectors."""

    results = []

    def record(group: str, case_id: str, ok: bool, detail: Any = None) -> None:
        results.append({"group": group, "id": case_id, "ok": bool(ok), "detail": None if ok else detail})

    def outcome(call) -> dict[str, Any]:
        try:
            return {"valid": True, "value": call()}
        except SignalError as error:
            return {"valid": False, "error": error.code}

    for case in vectors["records"]:
        actual = outcome(lambda: validate_signal(case["input"], case["asOf"]))
        expect = case["expect"]
        if actual["valid"]:
            got = {"valid": True, "signalId": actual["value"]["signalId"], "summary": describe(actual["value"])}
        else:
            got = actual
        record("records", case["id"], got == expect, got)
    for case in vectors["revocations"]:
        actual = outcome(lambda: validate_revocation(case["input"], case["asOf"]))
        got = ({"valid": True, "revocationId": actual["value"]["revocationId"]} if actual["valid"] else actual)
        record("revocations", case["id"], got == case["expect"], got)
    for scenario in vectors["scenarios"]:
        expect = scenario["expect"]
        for order in scenario["orders"]:
            store = SignalStore()
            reports = {index: store.import_bundle(scenario["bundles"][index], scenario["asOf"]) for index in order}
            label = f"{scenario['id']}:order-{''.join(map(str, order))}"
            record("merge", label, store.fold(scenario["asOf"]) == expect["state"], store.digest())
            if order == sorted(order):
                record("import", scenario["id"], [reports[i] for i in order] == expect["imports"],
                       [reports[i] for i in order])
                for audience, bundle in expect["exports"].items():
                    name, _, signer = audience.partition("-")
                    exported = store.export_bundle(name, scenario["asOf"], expect_signer(vectors, signer))
                    record("export", f"{scenario['id']}:{audience}", exported == bundle, exported)
                    round_trip = SignalStore()
                    round_trip.import_bundle(json.loads(json.dumps(exported)), scenario["asOf"])
                    record("export", f"{scenario['id']}:{audience}:reimport",
                           round_trip.export_bundle(name, scenario["asOf"]) == bundle, None)
                for case in expect["recommendations"]:
                    got = store.recommend(case["controls"])
                    record("recommend", f"{scenario['id']}:{case['id']}", got == case["output"], got)
                for case in vectors["invalidControls"]:
                    got = outcome(lambda: store.recommend(case["controls"]))
                    record("controls", case["id"], got == {"valid": False, "error": case["error"]}, got)
    for case in vectors["invalidBundles"]:
        got = outcome(lambda: SignalStore().import_bundle(_bundle_input(case), 0))
        record("bundles", case["id"], got == {"valid": False, "error": case["error"]}, got)
    return results


def expect_signer(vectors: dict[str, Any], name: str) -> str | None:
    return vectors["keys"][name]["publicKey"] if name else None


# --------------------------------------------------------------------------
# JavaScript implementation and cross-implementation round trip


def _node() -> str:
    node = shutil.which("node")
    if not node:
        raise SystemExit("social-signals: Node.js is required to run the JavaScript implementation")
    return node


def run_javascript(command: str, payload: Any = None) -> Any:
    completed = subprocess.run(
        [_node(), "--no-warnings", str(JS_RUNNER), command],
        input=json.dumps(payload) if payload is not None else "",
        capture_output=True, text=True, check=False, cwd=ROOT, timeout=300,
    )
    if completed.returncode not in (0, 1) or not completed.stdout.strip():
        raise SystemExit(f"social-signals: JavaScript {command} failed\n{completed.stderr}")
    return json.loads(completed.stdout)


def interop_round_trip(vectors: dict[str, Any]) -> list[dict[str, Any]]:
    """Python signs and exports; JavaScript imports, merges, revokes, and
    exports; Python imports that bundle and must see the JavaScript revocation."""

    results = []
    scenario = vectors["scenarios"][0]
    as_of = scenario["asOf"]
    alice = bytes.fromhex(vectors["keys"]["alice"]["seed"])
    python_store = SignalStore()
    for item in scenario["bundles"][:2]:
        python_store.import_bundle(item, as_of)
    fresh = sign_signal(alice, {
        "kind": "watch", "subject": _repository("interop"), "issuedAt": as_of - 1000, "note": "signed in Python"})
    python_store.import_bundle({"schema": BUNDLE_SCHEMA, "exportedAt": as_of, "signals": [fresh], "revocations": []}, as_of)
    python_bundle = python_store.export_bundle("self", as_of)
    js = run_javascript("interop", {
        "asOf": as_of,
        "bundles": [python_bundle] + scenario["bundles"][2:],
        "seed": vectors["keys"]["alice"]["seed"],
        "revoke": fresh["signalId"],
        "sign": {"kind": "star", "subject": _repository("interop"), "issuedAt": as_of - 500,
                 "note": "signed in JavaScript"},
    })
    results.append({"group": "interop", "id": "javascript-import-reports",
                    "ok": all(not report["rejected"] for report in js["reports"][:1]), "detail": js["reports"]})
    merged = SignalStore()
    report = merged.import_bundle(js["bundle"], as_of)
    results.append({"group": "interop", "id": "python-verifies-javascript-bundle",
                    "ok": not report["rejected"], "detail": report})
    state = merged.fold(as_of)
    statuses = {item["signalId"]: item["status"] for item in state["signals"]}
    results.append({"group": "interop", "id": "javascript-revocation-applies-in-python",
                    "ok": statuses.get(fresh["signalId"]) == "revoked", "detail": statuses.get(fresh["signalId"])})
    js_star = [item for item in state["signals"]
               if item["kind"] == "star" and item["subject"] == _repository("interop")]
    results.append({"group": "interop", "id": "javascript-signature-verifies-in-python",
                    "ok": len(js_star) == 1 and js_star[0]["status"] == "active", "detail": js_star})
    results.append({"group": "interop", "id": "states-agree",
                    "ok": state["digest"] == js["state"]["digest"] and state == js["state"],
                    "detail": [state["digest"], js["state"]["digest"]]})
    return results


# --------------------------------------------------------------------------
# CLI


def _now_ms() -> int:
    return int(time.time() * 1000)


def _load_json(path: str) -> Any:
    if path == "-":
        return json.load(sys.stdin)
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _read_seed(path: str) -> bytes:
    text = Path(path).read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", text):
        raise SystemExit("social-signals: seed file must hold 64 lowercase hex characters")
    return bytes.fromhex(text)


def _emit(value: Any) -> None:
    sys.stdout.write(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def _load_store(paths: list[str], as_of: int) -> tuple[SignalStore, list[dict[str, Any]]]:
    store, reports = SignalStore(), []
    for path in paths:
        try:
            report = store.import_bundle(_load_json(path), as_of)
        except SignalError as error:
            report = {"accepted": 0, "duplicate": 0, "rejected": [{"type": "bundle", "id": None, "error": error.code}]}
        reports.append({"bundle": path, **report})
    return store, reports


def _summarize(results: list[dict[str, Any]], implementation: str) -> dict[str, Any]:
    failed = [item for item in results if not item["ok"]]
    groups: dict[str, int] = {}
    for item in results:
        groups[item["group"]] = groups.get(item["group"], 0) + 1
    return {"implementation": implementation, "cases": len(results), "groups": groups,
            "failed": failed[:20], "failures": len(failed)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    keygen = sub.add_parser("keygen")
    keygen.add_argument("--out", required=True)
    sign = sub.add_parser("sign")
    sign.add_argument("--seed-file", required=True)
    sign.add_argument("record", help="unsigned record JSON file, or - for stdin")
    revoke = sub.add_parser("revoke")
    revoke.add_argument("--seed-file", required=True)
    revoke.add_argument("signal_id")
    revoke.add_argument("--reason", choices=REASONS, default="withdrawn")
    revoke.add_argument("--revoked-at", type=int)
    for name in ("merge", "verify", "recommend"):
        command = sub.add_parser(name)
        command.add_argument("bundles", nargs="+")
        command.add_argument("--as-of", type=int)
    merge = sub.choices["merge"]
    merge.add_argument("--audience", choices=tuple(AUDIENCES), default="self")
    merge.add_argument("--signer", help="export only this signer's records")
    merge.add_argument("--output")
    recommend = sub.choices["recommend"]
    recommend.add_argument("--viewer", required=True)
    recommend.add_argument("--disable", action="store_true", help="recommendations off: explain nothing, rank nothing")
    recommend.add_argument("--limit", type=int, default=20)
    recommend.add_argument("--mute-signer", action="append", default=[])
    recommend.add_argument("--mute-subject", action="append", default=[])
    vectors = sub.add_parser("vectors")
    mode = vectors.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    sub.add_parser("gate")
    args = parser.parse_args(argv)

    try:
        if args.command == "keygen":
            path = Path(args.out)
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "w", encoding="ascii") as handle:
                handle.write(secrets.token_bytes(32).hex() + "\n")
            print(public_key(_read_seed(args.out)))
            return 0
        if args.command == "sign":
            _emit(sign_signal(_read_seed(args.seed_file), _load_json(args.record)))
            return 0
        if args.command == "revoke":
            _emit(sign_revocation(_read_seed(args.seed_file), args.signal_id,
                                  args.revoked_at if args.revoked_at is not None else _now_ms(), args.reason))
            return 0
        if args.command in ("merge", "verify", "recommend"):
            as_of = args.as_of if args.as_of is not None else _now_ms()
            store, reports = _load_store(args.bundles, as_of)
            rejected = any(report["rejected"] for report in reports)
            if args.command == "merge":
                bundle = store.export_bundle(args.audience, as_of, args.signer)
                if args.output:
                    Path(args.output).write_text(json.dumps(bundle, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
                else:
                    _emit(bundle)
                sys.stderr.write(json.dumps({"imports": reports, "digest": store.digest()}) + "\n")
                return 0
            if args.command == "verify":
                _emit({"imports": reports, "state": store.fold(as_of)})
                return 1 if rejected else 0
            _emit(store.recommend({
                "viewer": args.viewer, "enabled": not args.disable, "asOf": as_of, "limit": args.limit,
                "mutedSigners": args.mute_signer, "mutedSubjects": args.mute_subject,
            }))
            return 0
        if args.command == "vectors":
            rendered = _render_vectors(build_vectors())
            if args.write:
                VECTORS_PATH.parent.mkdir(parents=True, exist_ok=True)
                VECTORS_PATH.write_text(rendered, encoding="utf-8")
                return 0
            if args.check:
                current = _read_vectors_text()
                if current != rendered:
                    print("social-signals: committed vectors are stale; run tools/social_signals.py vectors --write")
                    return 1
                return 0
            summary = _summarize(run_vectors(json.loads(_read_vectors_text())), "python")
            _emit(summary)
            return 1 if summary["failures"] else 0
        # gate
        committed = json.loads(_read_vectors_text())
        report = {
            "vectorsCurrent": _read_vectors_text() == _render_vectors(build_vectors()),
            "python": _summarize(run_vectors(committed), "python"),
            "javascript": _summarize(run_javascript("vectors")["results"], "javascript"),
            "interop": _summarize(interop_round_trip(committed), "python+javascript"),
        }
        _emit(report)
        ok = report["vectorsCurrent"] and not any(report[name]["failures"] for name in ("python", "javascript", "interop"))
        if report["javascript"]["cases"] != report["python"]["cases"]:
            ok = False
            sys.stderr.write("social-signals: implementations ran different case counts\n")
        return 0 if ok else 1
    except SignalError as error:
        sys.stderr.write(f"social-signals: {error.code}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
