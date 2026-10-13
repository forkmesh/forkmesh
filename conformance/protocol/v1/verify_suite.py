#!/usr/bin/env python3
"""Clean-room verifier for the ForkMesh protocol suite v1.

Written from the normative documents in ``www/docs/protocol`` only. It imports
nothing from the ForkMesh source tree (the suite tests pin a standard-library
import list), so agreement with the reference implementations is independent
evidence rather than a copy of their behavior.

It first checks that documents, schemas, vectors, and the threat register are
versioned together (FM-SUITE-001), then verifies every vector set it claims.
Exit status: 0 consistent and passing, 1 otherwise.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
import unicodedata
from pathlib import Path
from typing import Any, Callable, Iterator

DEFAULT_ROOT = Path(__file__).resolve().parents[3]
MANIFEST_PATH = "conformance/protocol/v1/manifest.json"


class Reject(Exception):
    """A protocol rejection carrying the suite's stable error code."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --------------------------------------------------------------------------
# Canonical JSON (encoding-and-identifiers.md, FM-ENC-001..005)

SAFE_INTEGER = 2**53 - 1
MAX_DEPTH = 32
MAX_DOCUMENT_BYTES = 1_048_576
KEY_RE = re.compile(r"[A-Za-z0-9_.:/-]{1,256}")
_SHORT_ESCAPES = {
    '"': '\\"',
    "\\": "\\\\",
    "\b": "\\b",
    "\f": "\\f",
    "\n": "\\n",
    "\r": "\\r",
    "\t": "\\t",
}


def normalize_string(value: str) -> str:
    normalized = unicodedata.normalize("NFC", value)
    if any(0xD800 <= ord(char) <= 0xDFFF for char in normalized):
        raise Reject("invalid_unicode")
    return normalized


def normalize(value: Any, depth: int = 0) -> Any:
    if depth > MAX_DEPTH:
        raise Reject("json_too_deep")
    if value is None or value is True or value is False:
        return value
    if isinstance(value, int):
        if not -SAFE_INTEGER <= value <= SAFE_INTEGER:
            raise Reject("unsafe_integer")
        return value
    if isinstance(value, float):
        raise Reject("floating_point_forbidden")
    if isinstance(value, str):
        return normalize_string(value)
    if isinstance(value, list):
        return [normalize(item, depth + 1) for item in value]
    if isinstance(value, dict):
        result: dict[str, Any] = {}
        for key, item in value.items():
            if not isinstance(key, str) or not KEY_RE.fullmatch(key):
                raise Reject("invalid_key")
            if key in result:
                raise Reject("duplicate_key")
            result[key] = normalize(item, depth + 1)
        return result
    raise Reject("invalid_json_type")


def encode_string(value: str, ascii_only: bool = False) -> str:
    parts = ['"']
    for char in value:
        point = ord(char)
        if char in _SHORT_ESCAPES:
            parts.append(_SHORT_ESCAPES[char])
        elif point < 0x20:
            parts.append(f"\\u{point:04x}")
        elif ascii_only and point > 0x7E:
            if point > 0xFFFF:
                point -= 0x10000
                parts.append(f"\\u{0xD800 + (point >> 10):04x}\\u{0xDC00 + (point & 0x3FF):04x}")
            else:
                parts.append(f"\\u{point:04x}")
        else:
            parts.append(char)
    parts.append('"')
    return "".join(parts)


def encode(value: Any, ascii_only: bool = False) -> str:
    """Serialize without whitespace, keys in code-point order."""

    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return encode_string(value, ascii_only)
    if isinstance(value, list):
        return "[" + ",".join(encode(item, ascii_only) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(
            encode_string(key, ascii_only) + ":" + encode(value[key], ascii_only)
            for key in sorted(value)
        ) + "}"
    raise Reject("invalid_json_type")


def canonical_bytes(value: Any) -> bytes:
    return encode(normalize(value)).encode("utf-8")


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise Reject("duplicate_key")
        result[key] = value
    return result


def _forbid_float(_text: str) -> Any:
    raise Reject("floating_point_forbidden")


def _forbid_constant(_text: str) -> Any:
    raise Reject("invalid_number")


def parse_canonical_document(raw: bytes) -> dict[str, Any]:
    if not raw or len(raw) > MAX_DOCUMENT_BYTES:
        raise Reject("invalid_event_size")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise Reject("invalid_json") from None
    if text.startswith("﻿"):
        raise Reject("invalid_json")
    try:
        value = json.loads(
            text,
            object_pairs_hook=_pairs,
            parse_float=_forbid_float,
            parse_constant=_forbid_constant,
        )
    except RecursionError:
        raise Reject("json_too_deep") from None
    except ValueError:
        raise Reject("invalid_json") from None
    if not isinstance(value, dict):
        raise Reject("event_not_object")
    if canonical_bytes(value) != raw:
        raise Reject("noncanonical_encoding")
    return value


# --------------------------------------------------------------------------
# base64url (FM-ENC-006)

_B64_RE = re.compile(r"[A-Za-z0-9_-]*")


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def b64url_decode(value: Any, size: int | None, code: str) -> bytes:
    if not isinstance(value, str) or not _B64_RE.fullmatch(value) or len(value) % 4 == 1:
        raise Reject(code)
    data = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    if (size is not None and len(data) != size) or b64url(data) != value:
        raise Reject(code)
    return data


# --------------------------------------------------------------------------
# Ed25519, RFC 8032 section 5.1.7 plus FM-SIG-001 strictness

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)
_IDENTITY = (0, 1, 1, 0)


def _point_add(p: tuple[int, ...], q: tuple[int, ...]) -> tuple[int, int, int, int]:
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _scalar_mult(scalar: int, point: tuple[int, ...]) -> tuple[int, ...]:
    result: tuple[int, ...] = _IDENTITY
    while scalar:
        if scalar & 1:
            result = _point_add(result, point)
        point = _point_add(point, point)
        scalar >>= 1
    return result


def _point_equal(p: tuple[int, ...], q: tuple[int, ...]) -> bool:
    return (p[0] * q[2] - q[0] * p[2]) % _P == 0 and (p[1] * q[2] - q[1] * p[2]) % _P == 0


def _recover_x(y: int, sign: int) -> int | None:
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


def _decode_point(raw: bytes) -> tuple[int, int, int, int] | None:
    if len(raw) != 32:
        return None
    number = int.from_bytes(raw, "little")
    y = number & ((1 << 255) - 1)
    x = _recover_x(y, number >> 255)
    if x is None:
        return None
    return (x, y, 1, x * y % _P)


_BASE_Y = 4 * pow(5, _P - 2, _P) % _P
_BASE = _decode_point(_BASE_Y.to_bytes(32, "little"))


def ed25519_verify(public_key: bytes, signature: bytes, message: bytes) -> bool:
    if len(public_key) != 32 or len(signature) != 64:
        return False
    public = _decode_point(public_key)
    nonce = _decode_point(signature[:32])
    scalar = int.from_bytes(signature[32:], "little")
    if public is None or nonce is None or scalar >= _L:
        return False
    if _point_equal(public, _IDENTITY) or not _point_equal(_scalar_mult(_L, public), _IDENTITY):
        return False
    challenge = int.from_bytes(hashlib.sha512(signature[:32] + public_key + message).digest(), "little") % _L
    return _point_equal(_scalar_mult(scalar, _BASE), _point_add(nonce, _scalar_mult(challenge, public)))


# --------------------------------------------------------------------------
# Identifiers (FM-ID-001..006)

FAMILIES = (
    "issue", "pull_request", "comment", "label", "reaction",
    "review", "milestone", "board", "notification", "moderation",
)
REPOSITORY_RE = re.compile(r"urn:forkmesh:repository:sha256:[0-9a-f]{64}")
OBJECT_RE = re.compile(r"urn:forkmesh:collaboration:([a-z_]+):sha256:[0-9a-f]{64}")
DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}")
RELATION_RE = re.compile(r"[a-z][a-z0-9_.-]{0,63}")
POLICY_SCHEMA = "forkmesh-collaboration-policy-v1"
POLICY_KEYS = frozenset({"schema", "repository", "maintainers", "moderators", "notifiers"})


def repository_id(verified_root: Any) -> str:
    if not isinstance(verified_root, str) or not verified_root:
        raise Reject("invalid_repository_identity")
    root = normalize_string(verified_root).encode("utf-8")
    return "urn:forkmesh:repository:sha256:" + _sha256_hex(b"forkmesh-repository-identity-v1\n" + root)


def policy_id(policy: Any) -> str:
    if not isinstance(policy, dict) or set(policy) != POLICY_KEYS or policy.get("schema") != POLICY_SCHEMA:
        raise Reject("invalid_policy")
    if not isinstance(policy["repository"], str) or not REPOSITORY_RE.fullmatch(policy["repository"]):
        raise Reject("invalid_repository")
    for role in ("maintainers", "moderators", "notifiers"):
        members = policy[role]
        if not isinstance(members, list):
            raise Reject("invalid_policy_role")
        for member in members:
            b64url_decode(member, 32, "invalid_policy_key")
    for role in ("maintainers", "moderators", "notifiers"):
        if policy[role] != sorted(set(policy[role])):
            raise Reject("noncanonical_policy")
    return "sha256:" + _sha256_hex(canonical_bytes(policy))


def validate_references(family: str, references: Any) -> list[dict[str, str]]:
    if not isinstance(references, list) or len(references) > 16:
        raise Reject("invalid_references")
    for reference in references:
        if not isinstance(reference, dict) or set(reference) != {"relation", "family", "object"}:
            raise Reject("invalid_reference")
        if not isinstance(reference["relation"], str) or not RELATION_RE.fullmatch(reference["relation"]):
            raise Reject("invalid_reference_relation")
        match = OBJECT_RE.fullmatch(reference["object"]) if isinstance(reference["object"], str) else None
        if reference["family"] not in FAMILIES or not match or match.group(1) != reference["family"]:
            raise Reject("invalid_reference_target")
    keys = [(item["relation"], item["family"], item["object"]) for item in references]
    if keys != sorted(set(keys)):
        raise Reject("noncanonical_references")
    subjects = [item["family"] for item in references if item["relation"] == "subject"]
    if family == "comment" and (len(subjects) != 1 or subjects[0] not in {"issue", "pull_request", "comment"}):
        raise Reject("comment_subject_required")
    if family == "reaction" and (
        len(subjects) != 1 or subjects[0] not in {"issue", "pull_request", "comment", "review"}
    ):
        raise Reject("reaction_subject_required")
    if family == "review" and subjects != ["pull_request"]:
        raise Reject("review_pull_required")
    if family == "moderation" and (len(subjects) != 1 or subjects[0] in {"moderation", "notification"}):
        raise Reject("moderation_subject_required")
    if family in {"issue", "pull_request", "label", "milestone", "board"} and subjects:
        raise Reject("unexpected_subject")
    if family == "notification" and len(subjects) > 1:
        raise Reject("invalid_notification_subject")
    return references


def object_id(repository: Any, family: Any, creator: Any, object_key: Any, references: Any) -> str:
    if not isinstance(repository, str) or not REPOSITORY_RE.fullmatch(repository) or family not in FAMILIES:
        raise Reject("invalid_object_identity_input")
    b64url_decode(creator, 32, "invalid_author")
    b64url_decode(object_key, 16, "invalid_object_key")
    identity = {
        "repository": repository,
        "family": family,
        "creator": creator,
        "objectKey": object_key,
        "references": validate_references(family, references),
    }
    digest = _sha256_hex(b"forkmesh-collaboration-object-v1\n" + canonical_bytes(identity))
    return f"urn:forkmesh:collaboration:{family}:sha256:{digest}"


def refs_canonical(advertisement: bytes) -> str:
    refs: list[tuple[str, str]] = []
    offset = 0
    while offset + 4 <= len(advertisement):
        try:
            length = int(advertisement[offset:offset + 4], 16)
        except ValueError:
            break
        if length == 0:
            offset += 4
            continue
        if length < 4 or offset + length > len(advertisement):
            break
        line = advertisement[offset + 4:offset + length].rstrip(b"\n").split(b"\x00", 1)[0]
        offset += length
        sha, separator, name = line.partition(b" ")
        if not separator:
            continue
        refname = name.decode("utf-8", "ignore")
        if refname == "HEAD" or refname.endswith("^{}"):
            continue
        if refname.startswith(("refs/heads/", "refs/tags/")):
            refs.append((refname, sha.decode("ascii", "ignore")))
    return "\n".join(f"{sha} {name}" for name, sha in sorted(refs, key=lambda item: item[0]))


def migration_digest(domain: str, *parts: str) -> str:
    return _sha256_hex((domain + "\n" + "\n".join(parts)).encode("utf-8"))


# --------------------------------------------------------------------------
# Collaboration envelopes (signatures-and-authentication.md, FM-SIG-003/004)

EVENT_SCHEMA = "forkmesh-collaboration-event-v1"
EVENT_DOMAIN = b"forkmesh-collaboration-event-v1\n"
TOP_LEVEL_KEYS = frozenset({
    "schema", "repository", "policy", "family", "object", "action", "author", "sequence",
    "clock", "issuedAt", "parents", "references", "payload", "signature", "eventId",
})
UNSIGNED_KEYS = TOP_LEVEL_KEYS - {"signature", "eventId"}
ACTIONS = {family: {"create", "update", "add", "remove", "tombstone", "restore"} for family in FAMILIES}
ACTIONS["reaction"] = {"create", "tombstone", "restore"}
ACTIONS["moderation"] = {"decide", "appeal", "resolve"}


def verify_event(raw: bytes, policy: dict[str, Any]) -> dict[str, Any]:
    """Envelope, identity, binding, and signature checks; payload semantics are fold scope."""

    event = parse_canonical_document(raw)
    if set(event) != TOP_LEVEL_KEYS:
        raise Reject("invalid_envelope_keys")
    if event["schema"] != EVENT_SCHEMA:
        raise Reject("unsupported_schema")
    if not isinstance(event["repository"], str) or not REPOSITORY_RE.fullmatch(event["repository"]):
        raise Reject("invalid_repository")
    if not isinstance(event["policy"], str) or not DIGEST_RE.fullmatch(event["policy"]):
        raise Reject("invalid_policy_id")
    family, action = event["family"], event["action"]
    if family not in FAMILIES or action not in ACTIONS[family]:
        raise Reject("unknown_family_action")
    match = OBJECT_RE.fullmatch(event["object"]) if isinstance(event["object"], str) else None
    if not match or match.group(1) != family:
        raise Reject("invalid_object_id")
    author = b64url_decode(event["author"], 32, "invalid_author")
    signature = b64url_decode(event["signature"], 64, "invalid_signature_encoding")
    for name in ("sequence", "clock", "issuedAt"):
        value = event[name]
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= SAFE_INTEGER:
            raise Reject("invalid_" + name)
    parents = event["parents"]
    if not isinstance(parents, list) or not all(isinstance(item, str) and DIGEST_RE.fullmatch(item) for item in parents):
        raise Reject("invalid_event_id")
    if parents != sorted(set(parents)):
        raise Reject("noncanonical_event_id_list")
    if len(parents) > 64:
        raise Reject("too_many_parents")
    references = validate_references(family, event["references"])
    if not isinstance(event["payload"], dict):
        raise Reject("invalid_payload")
    signing = EVENT_DOMAIN + canonical_bytes({key: event[key] for key in UNSIGNED_KEYS})
    if event["eventId"] != "sha256:" + _sha256_hex(signing + b"\x00" + signature):
        raise Reject("event_id_mismatch")
    if action in {"create", "decide"}:
        expected = object_id(event["repository"], family, event["author"], event["payload"].get("objectKey"), references)
        if event["object"] != expected:
            raise Reject("object_id_mismatch")
    if event["repository"] != policy["repository"]:
        raise Reject("wrong_repository")
    if event["policy"] != policy_id(policy):
        raise Reject("unknown_policy")
    if not ed25519_verify(author, signature, signing):
        raise Reject("invalid_signature")
    return event


# --------------------------------------------------------------------------
# Authentication (FM-AUTH-001..004)

FRESHNESS_WINDOW_MS = 300_000
_NAME_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?")
_SEGMENT_RE = re.compile(r"[A-Za-z0-9._-]{1,100}")


def request_is_fresh(ts: str, now_ms: int) -> bool:
    if not re.fullmatch(r"[0-9]{1,16}", ts):
        return False
    return abs(now_ms - int(ts)) <= FRESHNESS_WINDOW_MS


def git_push_message(owner: str, repo: str, method: str, ts: str, body_sha256: str) -> str | None:
    owner, repo = owner.strip().lower(), repo.strip()
    method, ts, body_sha256 = method.strip().upper(), ts.strip(), body_sha256.strip().lower()
    if (
        not _NAME_RE.fullmatch(owner)
        or not _SEGMENT_RE.fullmatch(repo)
        or repo in {".", ".."}
        or method not in {"GET", "POST"}
        or not re.fullmatch(r"[0-9]{1,16}", ts)
        or not re.fullmatch(r"[0-9a-f]{64}", body_sha256)
    ):
        return None
    return "\n".join(("forkmesh-git-push-v1", owner, repo, method, ts, body_sha256))


def catalog_v2_record_hash(record: dict[str, Any]) -> str:
    bound = {key: value for key, value in record.items() if key != "signature"}
    return _sha256_hex(encode(bound, ascii_only=True).encode("ascii"))


_SESSION_TOKEN_RE = re.compile(r"v1\.([A-Za-z0-9_-]{80,1024})\.([A-Za-z0-9_-]{86})")
_SESSION_KEYS = frozenset({"aud", "exp", "iat", "jti", "name", "sid", "sub", "type", "v"})
_OPAQUE_RE = re.compile(r"[A-Za-z0-9_-]{16,128}")
_ACCOUNT_RE = re.compile(r"[0-9a-f]{32,128}")
SESSION_MAX_LIFETIME_MS = 12 * 3_600_000
SESSION_SKEW_MS = 60_000


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def parse_session(token: str, now_ms: int, router_key: bytes) -> dict[str, Any]:
    match = _SESSION_TOKEN_RE.fullmatch(token)
    if not match:
        raise Reject("token_invalid")
    encoded_claim, encoded_signature = match.groups()
    raw_claim = b64url_decode(encoded_claim, None, "encoding_invalid")
    signature = b64url_decode(encoded_signature, 64, "encoding_invalid")
    try:
        claim = json.loads(raw_claim.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise Reject("claim_invalid") from None
    if (
        not isinstance(claim, dict)
        or set(claim) != _SESSION_KEYS
        or claim["type"] != "forkmesh.portable-session"
        or claim["aud"] != "forkmesh-mirror-dashboard"
        or not _is_int(claim["v"]) or claim["v"] != 1
        or not _is_int(claim["iat"]) or not _is_int(claim["exp"])
        or not isinstance(claim["sub"], str) or not _ACCOUNT_RE.fullmatch(claim["sub"])
        or not isinstance(claim["name"], str) or not _NAME_RE.fullmatch(claim["name"])
        or not isinstance(claim["sid"], str) or not _OPAQUE_RE.fullmatch(claim["sid"])
        or not isinstance(claim["jti"], str) or not _OPAQUE_RE.fullmatch(claim["jti"])
    ):
        raise Reject("claim_invalid")
    if (
        claim["iat"] <= 0
        or claim["exp"] <= claim["iat"]
        or claim["exp"] - claim["iat"] > SESSION_MAX_LIFETIME_MS
        or claim["iat"] > now_ms + SESSION_SKEW_MS
        or claim["exp"] <= now_ms - SESSION_SKEW_MS
    ):
        raise Reject("not_current")
    if encode(claim, ascii_only=True).encode("ascii") != raw_claim:
        raise Reject("claim_noncanonical")
    message = b"forkmesh-portable-session-v1\n" + encoded_claim.encode("ascii")
    if not ed25519_verify(router_key, signature, message):
        raise Reject("signature_invalid")
    return claim


# --------------------------------------------------------------------------
# Vector runners

Result = tuple[str, bool, str]


def _outcome(call: Callable[[], Any]) -> tuple[bool, Any]:
    try:
        return True, call()
    except Reject as exc:
        return False, exc.code


def _judge(case: dict[str, Any], valid: bool, value: Any, compare: Callable[[Any], bool] | None = None) -> Result:
    expect = case["expect"]
    if valid != expect["valid"]:
        return case["id"], False, f"expected valid={expect['valid']}, got {value!r}"
    if not valid and value != expect["error"]:
        return case["id"], False, f"expected error {expect['error']}, got {value}"
    if valid and compare is not None and not compare(value):
        return case["id"], False, f"unexpected value {value!r}"
    return case["id"], True, ""


def run_canonical_json(data: dict[str, Any]) -> Iterator[Result]:
    for case in data["parse"]:
        raw = bytes.fromhex(case["rawHex"])
        yield _judge(case, *_outcome(lambda raw=raw: parse_canonical_document(raw)))
    for case in data["encode"]:
        valid, value = _outcome(lambda case=case: canonical_bytes(case["value"]).decode("utf-8"))
        yield _judge(case, valid, value, lambda text, case=case: text == case["expect"]["canonicalText"])


def run_identifiers(data: dict[str, Any]) -> Iterator[Result]:
    for case in data["repositories"]:
        valid, value = _outcome(lambda case=case: repository_id(case["verifiedRoot"]))
        yield _judge(case, valid, value, lambda v, case=case: v == case["expect"]["repositoryId"])
    for case in data["policies"]:
        valid, value = _outcome(lambda case=case: policy_id(case["policy"]))
        yield _judge(case, valid, value, lambda v, case=case: v == case["expect"]["policyId"])
    for case in data["objects"]:
        valid, value = _outcome(lambda case=case: object_id(
            case["repository"], case["family"], case["creator"], case["objectKey"], case["references"]
        ))
        yield _judge(case, valid, value, lambda v, case=case: v == case["expect"]["objectId"])
    for case in data["refsState"]:
        canonical = refs_canonical(bytes.fromhex(case["advertisementHex"]))
        state_hash = _sha256_hex(canonical.encode("utf-8"))
        ok = canonical == case["expect"]["canonicalText"] and state_hash == case["expect"]["stateHash"]
        yield case["id"], ok, "" if ok else f"got {canonical!r} {state_hash}"
    for case in data["migration"]:
        if case["operation"] == "migrationId":
            value = "mig_" + migration_digest("forkmesh-migration-job-v1", case["repositoryId"])[:24]
        elif case["operation"] == "objectKey":
            value = migration_digest("forkmesh-migration-object-v1", case["objectType"], case["sourceId"])
        else:
            value = "sha256:" + _sha256_hex(encode(case["document"]).encode("utf-8"))
        ok = value == case["expect"]["value"]
        yield case["id"], ok, "" if ok else f"got {value}"


def run_signatures(data: dict[str, Any]) -> Iterator[Result]:
    for case in data["ed25519"]:
        valid = ed25519_verify(
            bytes.fromhex(case["publicKeyHex"]), bytes.fromhex(case["signatureHex"]), bytes.fromhex(case["messageHex"])
        )
        yield case["id"], valid == case["expect"]["valid"], f"got valid={valid}"
    record_hash = catalog_v2_record_hash(data["catalogV2Record"])
    for case in data["canonicalStrings"]:
        message = case["messageText"]
        if "scheme" in case:
            rebuilt = "\n".join([case["scheme"]] + [value for _name, value in case["fields"]])
            if rebuilt != message:
                yield case["id"], False, "messageText does not match scheme and fields"
                continue
            if case["scheme"] == "forkmesh-catalog-v2" and dict(case["fields"]).get("recordSha256") != record_hash:
                yield case["id"], False, f"catalog v2 record hash is {record_hash}"
                continue
        try:
            valid = ed25519_verify(
                b64url_decode(case["publicKey"], 32, "key"),
                b64url_decode(case["signature"], 64, "signature"),
                message.encode("utf-8"),
            )
        except Reject:
            valid = False
        yield case["id"], valid == case["expect"]["valid"], f"got valid={valid}"
    for case in data["gitPushMessages"]:
        fields = case["input"]
        message = git_push_message(fields["owner"], fields["repo"], fields["method"], fields["ts"], fields["bodySha256"])
        ok = (message is not None) == case["expect"]["valid"] and (
            message is None or message == case["expect"]["messageText"]
        )
        yield case["id"], ok, f"got {message!r}"
    for case in data["freshness"]:
        fresh = request_is_fresh(case["ts"], case["nowMs"])
        yield case["id"], fresh == case["expect"]["fresh"], f"got fresh={fresh}"


def run_events(data: dict[str, Any]) -> Iterator[Result]:
    policy = data["policies"]["primary"]
    for case in data["cases"]:
        raw = bytes.fromhex(case["rawHex"])
        valid, value = _outcome(lambda raw=raw: verify_event(raw, policy))
        yield _judge(case, valid, value, lambda event, case=case: event["eventId"] == case["expect"]["eventId"])


def run_sessions(data: dict[str, Any]) -> Iterator[Result]:
    router = b64url_decode(data["routerPublicKey"], 32, "router")
    for case in data["cases"]:
        valid, value = _outcome(lambda case=case: parse_session(case["token"], case["nowMs"], router))
        yield _judge(case, valid, value, lambda claim, case=case: claim == case["expect"]["claim"])


RUNNERS: dict[str, Callable[[dict[str, Any]], Iterator[Result]]] = {
    "canonical-json": run_canonical_json,
    "identifiers": run_identifiers,
    "signatures": run_signatures,
    "events": run_events,
    "sessions": run_sessions,
}


def run_vectors(root: Path) -> dict[str, dict[str, Any]]:
    manifest = json.loads((root / MANIFEST_PATH).read_text(encoding="utf-8"))
    report: dict[str, dict[str, Any]] = {}
    for entry in manifest["vectors"]:
        name = Path(entry["path"]).stem
        runner = RUNNERS.get(name)
        if runner is None:
            report[name] = {"claimed": False}
            continue
        data = json.loads((root / entry["path"]).read_text(encoding="utf-8"))
        failures = []
        total = 0
        for case_id, ok, detail in runner(data):
            total += 1
            if not ok:
                failures.append({"id": case_id, "detail": detail})
        report[name] = {"claimed": True, "cases": total, "failures": failures}
    return report


# --------------------------------------------------------------------------
# Suite consistency (FM-SUITE-001) and the stable release gate

_DEFINITION_RE = re.compile(r"\*\*(FM-[A-Z]+-\d{3})\*\*\s+(MUST NOT|MUST|SHOULD NOT|SHOULD|MAY)\b")
_REFERENCE_RE = re.compile(r"\bFM-[A-Z]+-\d{3}\b")
_THREAT_ROW_RE = re.compile(r"^\|\s*(TM-\d{3})\s*\|(.*)\|\s*$", re.MULTILINE)
SEVERITIES = ("critical", "high", "medium", "low")
STATUSES = ("mitigated", "accepted", "open")
SUITE_STATUSES = ("draft", "candidate", "stable")


def _case_requirements(value: Any) -> Iterator[tuple[str, list[str]]]:
    if isinstance(value, dict):
        if "id" in value and isinstance(value.get("requirements"), list):
            yield value["id"], value["requirements"]
        for item in value.values():
            yield from _case_requirements(item)
    elif isinstance(value, list):
        for item in value:
            yield from _case_requirements(item)


def check_consistency(root: Path) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    manifest_file = root / MANIFEST_PATH
    try:
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return [f"manifest unreadable: {exc}"], {}
    version = manifest.get("version")
    status = manifest.get("status")
    if status not in SUITE_STATUSES:
        errors.append(f"manifest status {status!r} is not one of {SUITE_STATUSES}")

    def bound_file(entry: dict[str, Any], kind: str) -> bytes | None:
        path = root / str(entry.get("path", ""))
        try:
            data = path.read_bytes()
        except OSError:
            errors.append(f"{kind} missing: {entry.get('path')}")
            return None
        if _sha256_hex(data) != entry.get("sha256"):
            errors.append(f"{kind} digest mismatch: {entry.get('path')}")
        return data

    requirements = manifest.get("requirements", {})
    defined: dict[str, str] = {}
    referenced: set[str] = set()
    for entry in manifest.get("documents", []):
        data = bound_file(entry, "document")
        if data is None:
            continue
        text = data.decode("utf-8")
        if f"Suite version: `{version}`" not in text:
            errors.append(f"document does not declare suite version {version}: {entry['path']}")
        for requirement, level in _DEFINITION_RE.findall(text):
            if requirement in defined:
                errors.append(f"requirement defined twice: {requirement}")
            defined[requirement] = level
        referenced.update(_REFERENCE_RE.findall(text))
    for requirement in sorted(referenced - set(defined)):
        errors.append(f"documents reference undefined requirement {requirement}")
    for requirement in sorted(set(defined) ^ set(requirements)):
        where = "documents" if requirement in defined else "manifest"
        errors.append(f"requirement {requirement} appears only in the {where}")

    for entry in manifest.get("schemas", []):
        data = bound_file(entry, "schema")
        if data is not None:
            try:
                json.loads(data)
            except ValueError:
                errors.append(f"schema is not JSON: {entry['path']}")

    cited: dict[str, set[str]] = {}
    vector_names = set()
    for entry in manifest.get("vectors", []):
        data = bound_file(entry, "vector file")
        if data is None:
            continue
        name = Path(entry["path"]).stem
        vector_names.add(name)
        vectors = json.loads(data)
        if vectors.get("suiteVersion") != version or vectors.get("vectorSet") != name:
            errors.append(f"vector file version or name mismatch: {entry['path']}")
        seen: set[str] = set()
        for case_id, case_requirements in _case_requirements(vectors):
            if case_id in seen:
                errors.append(f"duplicate case id {case_id} in {name}")
            seen.add(case_id)
            for requirement in case_requirements:
                if requirement not in requirements:
                    errors.append(f"{name}:{case_id} cites unknown requirement {requirement}")
                cited.setdefault(f"vectors/{name}.json", set()).add(requirement)

    for requirement, spec in requirements.items():
        if requirement in defined and spec.get("level") != defined[requirement]:
            errors.append(f"{requirement} level differs between manifest and documents")
        evidence = spec.get("evidence") or []
        if not evidence:
            errors.append(f"{requirement} has no evidence")
        for item in evidence:
            if item.startswith("vectors/"):
                if requirement not in cited.get(item, set()):
                    errors.append(f"{requirement} evidence {item} has no case citing it")
            elif item.startswith("test:"):
                path, _, function = item[len("test:"):].partition("::")
                try:
                    source = (root / path).read_text(encoding="utf-8")
                except OSError:
                    errors.append(f"{requirement} evidence file missing: {path}")
                    continue
                if not re.search(rf"^(?:async )?def {re.escape(function)}\(", source, re.MULTILINE):
                    errors.append(f"{requirement} evidence test missing: {item}")
            else:
                errors.append(f"{requirement} has unrecognized evidence {item}")

    for implementation in manifest.get("implementations", []):
        for vector_set in implementation.get("vectorSets", []):
            if vector_set.split(":", 1)[0] not in vector_names:
                errors.append(f"implementation {implementation.get('id')} claims unknown vector set {vector_set}")

    blockers: list[str] = []
    register_entry = manifest.get("threatRegister") or {}
    register_data = bound_file(register_entry, "threat register") if register_entry else None
    if register_data is None:
        errors.append("threat register is not bound to the manifest")
    else:
        register = json.loads(register_data)
        if register.get("suiteVersion") != version:
            errors.append("threat register version mismatch")
        document_rows: dict[str, list[str]] = {}
        try:
            document_text = (root / register_entry["document"]).read_text(encoding="utf-8")
        except (KeyError, OSError):
            document_text = ""
            errors.append("threat model document is missing")
        for threat_id, rest in _THREAT_ROW_RE.findall(document_text):
            document_rows[threat_id] = [cell.strip().lower() for cell in rest.split("|")]
        ids = set()
        for threat in register.get("threats", []):
            threat_id = threat.get("id")
            if threat_id in ids:
                errors.append(f"duplicate threat {threat_id}")
            ids.add(threat_id)
            severity, threat_status = threat.get("severity"), threat.get("status")
            if severity not in SEVERITIES or threat_status not in STATUSES:
                errors.append(f"{threat_id} has invalid severity or status")
            for requirement in threat.get("requirements", []):
                if requirement not in requirements:
                    errors.append(f"{threat_id} cites unknown requirement {requirement}")
            if threat_status == "mitigated" and not threat.get("requirements"):
                errors.append(f"{threat_id} is mitigated without a normative control")
            acceptance = threat.get("acceptance") or {}
            if threat_status == "accepted" and not all(acceptance.get(key) for key in ("reviewer", "date", "rationale")):
                errors.append(f"{threat_id} is accepted without a reviewer, date, and rationale")
            row = document_rows.get(threat_id)
            if row is None or len(row) < 5 or row[3] != severity or row[4] != threat_status:
                errors.append(f"{threat_id} severity/status differs between register and threat model")
            if severity in {"critical", "high"} and threat_status == "open":
                blockers.append(threat_id)
        for threat_id in sorted(set(document_rows) - ids):
            errors.append(f"threat model lists {threat_id} absent from the register")
    if status == "stable" and blockers:
        errors.append("stable release blocked by open high-severity threats: " + ", ".join(blockers))
    return errors, {"version": version, "status": status, "stableReleaseBlockers": blockers}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="repository root to verify")
    args = parser.parse_args()
    errors, info = check_consistency(args.root)
    report = run_vectors(args.root) if not any("manifest unreadable" in e for e in errors) else {}
    failed = sum(len(entry.get("failures", [])) for entry in report.values())
    output = {**info, "consistencyErrors": errors, "vectorSets": report}
    sys.stdout.write(json.dumps(output, indent=2, sort_keys=True) + "\n")
    return 1 if errors or failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
