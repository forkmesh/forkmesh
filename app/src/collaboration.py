"""Conflict-safe signed collaboration objects.

This module is the dependency-free reference implementation of the
``forkmesh-collaboration-event-v1`` protocol.  It deliberately contains no
database or Worker bindings: an implementation gives it a set of canonical,
signed envelopes and receives the same materialized state regardless of
delivery order.  ``SignedEventLog`` retains the canonical raw envelopes so a
different implementation can verify and fold them again.

The security-sensitive rules live here instead of in a renderer:

* event and object identities are content-derived;
* Ed25519 signatures are verified before an event enters a fold;
* causality is an explicit parent DAG and wall-clock time is never precedence;
* concurrent scalar writes remain visible conflicts;
* collections and tombstones are observed-remove sets;
* repository policy gates privileged object families and moderation lineage;
* duplicate delivery is idempotent and author sequence equivocation fails
  closed.

The browser/Node implementation in ``app/public/collaboration.js`` consumes the
same envelopes and conformance corpus.
"""

from __future__ import annotations

import base64
import hashlib
import json
import re
import unicodedata
from collections import Counter, OrderedDict, defaultdict, deque
from dataclasses import dataclass, field
from threading import Lock
from typing import Any, Iterable, Mapping, Sequence


EVENT_SCHEMA = "forkmesh-collaboration-event-v1"
STATE_SCHEMA = "forkmesh-collaboration-state-v1"
POLICY_SCHEMA = "forkmesh-collaboration-policy-v1"
SIGNING_DOMAIN = b"forkmesh-collaboration-event-v1\n"
OBJECT_DOMAIN = b"forkmesh-collaboration-object-v1\n"

FAMILIES = (
    "issue",
    "pull_request",
    "comment",
    "label",
    "reaction",
    "review",
    "milestone",
    "board",
    "notification",
    "moderation",
)

_SAFE_INTEGER = 2**53 - 1
_MAX_EVENT_BYTES = 1_048_576
_MAX_JSON_DEPTH = 32
_MAX_PARENTS = 64
_MAX_REFERENCES = 16
# Keys are ASCII so Python's code-point sort and JavaScript's UTF-16 sort are
# byte-identical.  Object/event URNs are also used as keys in materialized
# state, hence the URI punctuation admitted here.
_KEY_RE = re.compile(r"^[A-Za-z0-9_.:/-]{1,256}$")
_SHA256_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_REPOSITORY_RE = re.compile(r"^urn:forkmesh:repository:sha256:[0-9a-f]{64}$")
_OBJECT_RE = re.compile(
    r"^urn:forkmesh:collaboration:([a-z_]+):sha256:([0-9a-f]{64})$"
)
_COLOR_RE = re.compile(r"^#[0-9a-f]{6}$")
_REASON_RE = re.compile(r"^[a-z][a-z0-9_.-]{0,63}$")

_TOP_LEVEL_KEYS = frozenset(
    {
        "schema",
        "repository",
        "policy",
        "family",
        "object",
        "action",
        "author",
        "sequence",
        "clock",
        "issuedAt",
        "parents",
        "references",
        "payload",
        "signature",
        "eventId",
    }
)
_UNSIGNED_KEYS = _TOP_LEVEL_KEYS - {"signature", "eventId"}
_SIGNED_NO_ID_KEYS = _TOP_LEVEL_KEYS - {"eventId"}

_SCALAR_FIELDS = {
    "issue": frozenset({"title", "body", "status", "priority", "dueAt"}),
    "pull_request": frozenset(
        {"title", "body", "status", "base", "head", "mergeCommit"}
    ),
    "comment": frozenset({"body"}),
    "label": frozenset({"name", "color", "description"}),
    "reaction": frozenset({"emoji"}),
    "review": frozenset({"state", "body", "revision"}),
    "milestone": frozenset({"title", "description", "status", "dueAt"}),
    "board": frozenset({"name", "description", "status"}),
    "notification": frozenset({"kind", "status", "recipient", "message"}),
    "moderation": frozenset({"decision", "reasonCode", "note"}),
}

_REQUIRED_CREATE_FIELDS = {
    "issue": frozenset({"title", "body"}),
    "pull_request": frozenset({"title", "base", "head"}),
    "comment": frozenset({"body"}),
    "label": frozenset({"name", "color"}),
    "reaction": frozenset({"emoji"}),
    "review": frozenset({"state", "revision"}),
    "milestone": frozenset({"title"}),
    "board": frozenset({"name"}),
    "notification": frozenset({"kind", "recipient"}),
    "moderation": frozenset({"decision", "reasonCode"}),
}

_SET_FIELDS = {
    "issue": frozenset({"labels", "assignees"}),
    "pull_request": frozenset({"labels", "reviewers"}),
    "comment": frozenset(),
    "label": frozenset({"subjects"}),
    "reaction": frozenset(),
    "review": frozenset(),
    "milestone": frozenset({"subjects"}),
    "board": frozenset({"items"}),
    "notification": frozenset(),
    "moderation": frozenset(),
}

_ACTIONS = {
    family: frozenset(
        {"create", "update", "add", "remove", "tombstone", "restore"}
    )
    for family in FAMILIES
    if family not in {"reaction", "moderation"}
}
_ACTIONS["reaction"] = frozenset({"create", "tombstone", "restore"})
_ACTIONS["moderation"] = frozenset({"decide", "appeal", "resolve"})

_LEGACY_SCHEMAS = {
    "issue": "forkmesh-issue-v1",
    "pull_request": "forkmesh-pull-v1",
    "comment": "forkmesh-comment-v1",
    "label": "forkmesh-label-v1",
    "reaction": "forkmesh-reaction-v1",
    "review": "forkmesh-review-v1",
    "milestone": "forkmesh-milestone-v1",
    "board": "forkmesh-project-v1",
    "notification": "forkmesh-notification-v1",
    "moderation": "forkmesh-moderation-v1",
}


class CollaborationError(ValueError):
    """A stable protocol rejection reason plus a human-readable detail."""

    def __init__(self, code: str, detail: str = "") -> None:
        self.code = code
        self.detail = detail
        super().__init__(code if not detail else f"{code}: {detail}")


def _reject(code: str, detail: str = "") -> None:
    raise CollaborationError(code, detail)


def _normalize_string(value: str) -> str:
    normalized = unicodedata.normalize("NFC", value)
    if any(0xD800 <= ord(char) <= 0xDFFF for char in normalized):
        _reject("invalid_unicode", "lone surrogate")
    return normalized


def _normalize_json(value: Any, _depth: int = 0) -> Any:
    """Return the JSON value admitted by the cross-language canonical subset."""

    if _depth > _MAX_JSON_DEPTH:
        _reject("json_too_deep")
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        if abs(value) > _SAFE_INTEGER:
            _reject("unsafe_integer")
        return value
    if isinstance(value, float):
        _reject("floating_point_forbidden")
    if isinstance(value, str):
        return _normalize_string(value)
    if isinstance(value, (list, tuple)):
        return [_normalize_json(item, _depth + 1) for item in value]
    if isinstance(value, Mapping):
        normalized: dict[str, Any] = {}
        for key, item in value.items():
            if not isinstance(key, str) or not _KEY_RE.fullmatch(key):
                _reject("invalid_key", repr(key))
            canonical_key = _normalize_string(key)
            if canonical_key in normalized:
                _reject("duplicate_key", canonical_key)
            normalized[canonical_key] = _normalize_json(item, _depth + 1)
        return normalized
    _reject("invalid_json_type", type(value).__name__)


def canonical_json(value: Any) -> bytes:
    """Encode the frozen JSON subset identically in Python and JavaScript."""

    normalized = _normalize_json(value)
    return json.dumps(
        normalized,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _pairs_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            _reject("duplicate_key", key)
        result[key] = value
    return result


def parse_canonical_event(raw: bytes | str) -> dict[str, Any]:
    """Parse one envelope and reject alternate or ambiguous wire encodings."""

    if isinstance(raw, str):
        raw = raw.encode("utf-8")
    if not isinstance(raw, bytes) or not raw or len(raw) > _MAX_EVENT_BYTES:
        _reject("invalid_event_size")
    try:
        value = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_pairs_object,
            parse_float=lambda _value: _reject("floating_point_forbidden"),
            parse_constant=lambda _value: _reject("invalid_number"),
        )
    except CollaborationError:
        raise
    except RecursionError:
        _reject("json_too_deep")
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        _reject("invalid_json", str(exc))
    if not isinstance(value, dict):
        _reject("event_not_object")
    if canonical_json(value) != raw:
        _reject("noncanonical_encoding")
    return value


def _b64url_decode(value: Any, expected_bytes: int, code: str) -> bytes:
    if not isinstance(value, str) or "=" in value:
        _reject(code)
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except (ValueError, UnicodeEncodeError) as exc:
        _reject(code, str(exc))
    if len(decoded) != expected_bytes:
        _reject(code)
    if base64.urlsafe_b64encode(decoded).decode().rstrip("=") != value:
        _reject(code)
    return decoded


def _b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


# Dependency-free, strict Ed25519 verification.  The Worker uses WebCrypto,
# while CPython tools use this implementation; conformance tests compare them.
_FIELD_Q = 2**255 - 19
_GROUP_L = 2**252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _FIELD_Q - 2, _FIELD_Q)) % _FIELD_Q
_I = pow(2, (_FIELD_Q - 1) // 4, _FIELD_Q)
_IDENTITY = (0, 1)


def _xrecover(y: int) -> int:
    xx = (y * y - 1) * pow(_D * y * y + 1, _FIELD_Q - 2, _FIELD_Q)
    x = pow(xx, (_FIELD_Q + 3) // 8, _FIELD_Q)
    if (x * x - xx) % _FIELD_Q:
        x = (x * _I) % _FIELD_Q
    if x & 1:
        x = _FIELD_Q - x
    return x


_BASE_Y = (4 * pow(5, _FIELD_Q - 2, _FIELD_Q)) % _FIELD_Q
_BASE = (_xrecover(_BASE_Y), _BASE_Y)


def _edwards_add(left: tuple[int, int], right: tuple[int, int]) -> tuple[int, int]:
    return _extended_to_affine(
        _extended_add(_affine_to_extended(left), _affine_to_extended(right))
    )


def _affine_to_extended(point: tuple[int, int]) -> tuple[int, int, int, int]:
    x, y = point
    return x, y, 1, (x * y) % _FIELD_Q


def _extended_to_affine(point: tuple[int, int, int, int]) -> tuple[int, int]:
    x, y, z, _t = point
    inverse = pow(z, _FIELD_Q - 2, _FIELD_Q)
    return (x * inverse) % _FIELD_Q, (y * inverse) % _FIELD_Q


def _extended_add(
    left: tuple[int, int, int, int], right: tuple[int, int, int, int]
) -> tuple[int, int, int, int]:
    x1, y1, z1, t1 = left
    x2, y2, z2, t2 = right
    a = ((y1 - x1) * (y2 - x2)) % _FIELD_Q
    b = ((y1 + x1) * (y2 + x2)) % _FIELD_Q
    c = (2 * _D * t1 * t2) % _FIELD_Q
    d = (2 * z1 * z2) % _FIELD_Q
    e = (b - a) % _FIELD_Q
    f = (d - c) % _FIELD_Q
    g = (d + c) % _FIELD_Q
    h = (b + a) % _FIELD_Q
    return (
        (e * f) % _FIELD_Q,
        (g * h) % _FIELD_Q,
        (f * g) % _FIELD_Q,
        (e * h) % _FIELD_Q,
    )


def _extended_double(
    point: tuple[int, int, int, int]
) -> tuple[int, int, int, int]:
    x, y, z, _t = point
    a = (x * x) % _FIELD_Q
    b = (y * y) % _FIELD_Q
    c = (2 * z * z) % _FIELD_Q
    d = (-a) % _FIELD_Q
    e = ((x + y) * (x + y) - a - b) % _FIELD_Q
    g = (d + b) % _FIELD_Q
    f = (g - c) % _FIELD_Q
    h = (d - b) % _FIELD_Q
    return (
        (e * f) % _FIELD_Q,
        (g * h) % _FIELD_Q,
        (f * g) % _FIELD_Q,
        (e * h) % _FIELD_Q,
    )


def _scalar_mult(point: tuple[int, int], scalar: int) -> tuple[int, int]:
    result = _affine_to_extended(_IDENTITY)
    addend = _affine_to_extended(point)
    while scalar:
        if scalar & 1:
            result = _extended_add(result, addend)
        addend = _extended_double(addend)
        scalar >>= 1
    return _extended_to_affine(result)


def _encode_point(point: tuple[int, int]) -> bytes:
    x, y = point
    encoded = y | ((x & 1) << 255)
    return encoded.to_bytes(32, "little")


def _decode_point(raw: bytes) -> tuple[int, int] | None:
    if len(raw) != 32:
        return None
    encoded = int.from_bytes(raw, "little")
    sign = encoded >> 255
    y = encoded & ((1 << 255) - 1)
    if y >= _FIELD_Q:
        return None
    x = _xrecover(y)
    # x = 0 has no negative; with the sign bit set, _FIELD_Q - 0 would slip
    # an unreduced identity past the identity and subgroup checks.
    if x == 0 and sign:
        return None
    if (x & 1) != sign:
        x = _FIELD_Q - x
    point = (x, y)
    if (-x * x + y * y - 1 - _D * x * x * y * y) % _FIELD_Q:
        return None
    if _encode_point(point) != raw:
        return None
    return point


def _ed25519_verify_uncached(
    public_key: bytes, signature: bytes, message: bytes
) -> bool:
    if len(public_key) != 32 or len(signature) != 64:
        return False
    public = _decode_point(public_key)
    nonce = _decode_point(signature[:32])
    scalar = int.from_bytes(signature[32:], "little")
    if public is None or nonce is None or scalar >= _GROUP_L:
        return False
    # Reject identity, small-order, and non-prime-subgroup public keys.
    if public == _IDENTITY or _scalar_mult(public, _GROUP_L) != _IDENTITY:
        return False
    challenge = int.from_bytes(
        hashlib.sha512(signature[:32] + public_key + message).digest(), "little"
    ) % _GROUP_L
    return _scalar_mult(_BASE, scalar) == _edwards_add(
        nonce, _scalar_mult(public, challenge)
    )


# Cache only a fixed-size digest key, never the potentially 1 MiB event bytes.
# This keeps replay verification inexpensive without turning the verifier into
# an attacker-controlled multi-gigabyte message cache.
_VERIFY_CACHE: OrderedDict[bytes, bool] = OrderedDict()
_VERIFY_CACHE_MAX = 8192
_VERIFY_CACHE_LOCK = Lock()


def ed25519_verify(public_key: bytes, signature: bytes, message: bytes) -> bool:
    """Strict RFC 8032 verification with canonical encodings and subgroup key."""

    cache_key = hashlib.sha256(public_key + signature + message).digest()
    with _VERIFY_CACHE_LOCK:
        if cache_key in _VERIFY_CACHE:
            result = _VERIFY_CACHE.pop(cache_key)
            _VERIFY_CACHE[cache_key] = result
            return result
    result = _ed25519_verify_uncached(public_key, signature, message)
    with _VERIFY_CACHE_LOCK:
        _VERIFY_CACHE[cache_key] = result
        if len(_VERIFY_CACHE) > _VERIFY_CACHE_MAX:
            _VERIFY_CACHE.popitem(last=False)
    return result


def policy_document(
    repository: str,
    *,
    maintainers: Iterable[str] = (),
    moderators: Iterable[str] = (),
    notifiers: Iterable[str] = (),
) -> dict[str, Any]:
    """Build the content-addressed repository authority policy for a fold."""

    if not isinstance(repository, str) or not _REPOSITORY_RE.fullmatch(repository):
        _reject("invalid_repository")

    def role_members(values: Iterable[str]) -> list[str]:
        if isinstance(values, (str, bytes)):
            _reject("invalid_policy_role")
        try:
            members = list(values)
        except TypeError as exc:
            _reject("invalid_policy_role", str(exc))
        for key in members:
            _b64url_decode(key, 32, "invalid_policy_key")
        return sorted(set(members))

    document = {
        "schema": POLICY_SCHEMA,
        "repository": repository,
        "maintainers": role_members(maintainers),
        "moderators": role_members(moderators),
        "notifiers": role_members(notifiers),
    }
    return document


def policy_id(document: Mapping[str, Any]) -> str:
    if set(document) != {
        "schema",
        "repository",
        "maintainers",
        "moderators",
        "notifiers",
    } or document.get("schema") != POLICY_SCHEMA:
        _reject("invalid_policy")
    normalized = policy_document(
        document.get("repository", ""),
        maintainers=document.get("maintainers", ()),
        moderators=document.get("moderators", ()),
        notifiers=document.get("notifiers", ()),
    )
    if normalized != document:
        _reject("noncanonical_policy")
    return "sha256:" + hashlib.sha256(canonical_json(normalized)).hexdigest()


def repository_id(canonical_git_identity: str) -> str:
    """Derive a stable repository identity from a separately verified root."""

    if not isinstance(canonical_git_identity, str) or not canonical_git_identity:
        _reject("invalid_repository_identity")
    digest = hashlib.sha256(
        b"forkmesh-repository-identity-v1\n"
        + _normalize_string(canonical_git_identity).encode("utf-8")
    ).hexdigest()
    return "urn:forkmesh:repository:sha256:" + digest


def _reference_sort_key(reference: Mapping[str, Any]) -> tuple[str, str, str]:
    return (
        str(reference.get("relation", "")),
        str(reference.get("family", "")),
        str(reference.get("object", "")),
    )


def _validate_references(family: str, references: Any) -> list[dict[str, str]]:
    if not isinstance(references, list) or len(references) > _MAX_REFERENCES:
        _reject("invalid_references")
    result: list[dict[str, str]] = []
    for reference in references:
        if not isinstance(reference, dict) or set(reference) != {
            "relation",
            "family",
            "object",
        }:
            _reject("invalid_reference")
        relation = reference.get("relation")
        target_family = reference.get("family")
        target = reference.get("object")
        if not isinstance(relation, str) or not _REASON_RE.fullmatch(relation):
            _reject("invalid_reference_relation")
        if target_family not in FAMILIES or not _valid_object_id(target, target_family):
            _reject("invalid_reference_target")
        result.append(dict(reference))
    if result != sorted(result, key=_reference_sort_key) or len(
        {tuple(_reference_sort_key(item)) for item in result}
    ) != len(result):
        _reject("noncanonical_references")

    subjects = [item for item in result if item["relation"] == "subject"]
    if family == "comment" and (
        len(subjects) != 1
        or subjects[0]["family"] not in {"issue", "pull_request", "comment"}
    ):
        _reject("comment_subject_required")
    if family == "reaction" and (
        len(subjects) != 1
        or subjects[0]["family"]
        not in {"issue", "pull_request", "comment", "review"}
    ):
        _reject("reaction_subject_required")
    if family == "review" and (
        len(subjects) != 1 or subjects[0]["family"] != "pull_request"
    ):
        _reject("review_pull_required")
    if family == "moderation" and (
        len(subjects) != 1
        or subjects[0]["family"] in {"moderation", "notification"}
    ):
        _reject("moderation_subject_required")
    if family in {"issue", "pull_request", "label", "milestone", "board"} and subjects:
        _reject("unexpected_subject")
    if family == "notification" and len(subjects) > 1:
        _reject("invalid_notification_subject")
    return result


def _valid_object_id(value: Any, family: str) -> bool:
    if not isinstance(value, str):
        return False
    match = _OBJECT_RE.fullmatch(value)
    return bool(match and match.group(1) == family)


def object_id(
    repository: str,
    family: str,
    creator: str,
    object_key: str,
    references: Sequence[Mapping[str, Any]] = (),
) -> str:
    """Derive the object identity bound by its first signed event."""

    if (
        not isinstance(repository, str)
        or not _REPOSITORY_RE.fullmatch(repository)
        or family not in FAMILIES
    ):
        _reject("invalid_object_identity_input")
    _b64url_decode(creator, 32, "invalid_author")
    key_bytes = _b64url_decode(object_key, 16, "invalid_object_key")
    normalized_refs = _validate_references(family, list(references))
    identity = {
        "repository": repository,
        "family": family,
        "creator": creator,
        "objectKey": _b64url(key_bytes),
        "references": normalized_refs,
    }
    digest = hashlib.sha256(OBJECT_DOMAIN + canonical_json(identity)).hexdigest()
    return f"urn:forkmesh:collaboration:{family}:sha256:{digest}"


def _validate_text(value: Any, *, maximum: int, empty: bool = True) -> None:
    if not isinstance(value, str) or len(value.encode("utf-8")) > maximum:
        _reject("invalid_field_value")
    # Non-empty is deliberately byte-semantic. Language-specific Unicode
    # whitespace tables must not make independent verifiers disagree.
    if not empty and not value:
        _reject("invalid_field_value")


def _validate_field(family: str, name: str, value: Any) -> None:
    if name not in _SCALAR_FIELDS[family]:
        _reject("unknown_field", name)
    if name in {"title", "name", "kind"}:
        _validate_text(value, maximum=512, empty=False)
    elif name in {"body", "description", "message", "note"}:
        _validate_text(value, maximum=900_000)
    elif name in {"base", "head", "mergeCommit", "revision"}:
        _validate_text(value, maximum=512, empty=False)
    elif name == "color":
        if not isinstance(value, str) or not _COLOR_RE.fullmatch(value):
            _reject("invalid_color")
    elif name == "emoji":
        _validate_text(value, maximum=64, empty=False)
    elif name == "recipient":
        _b64url_decode(value, 32, "invalid_recipient")
    elif name == "priority":
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 9999:
            _reject("invalid_priority")
    elif name == "dueAt":
        if value is not None and (
            isinstance(value, bool)
            or not isinstance(value, int)
            or not 0 <= value <= _SAFE_INTEGER
        ):
            _reject("invalid_due_at")
    elif name == "reasonCode":
        if not isinstance(value, str) or not _REASON_RE.fullmatch(value):
            _reject("invalid_reason_code")
    elif name == "decision":
        if value not in {"allow", "hide", "quarantine"}:
            _reject("invalid_moderation_decision")
    elif name == "status":
        allowed = {
            "issue": {"open", "closed"},
            "pull_request": {"open", "closed", "merged"},
            "milestone": {"open", "closed"},
            "board": {"open", "closed", "archived"},
            "notification": {"unread", "read"},
        }.get(family)
        if allowed is None or value not in allowed:
            _reject("invalid_status")
    elif name == "state":
        if family != "review" or value not in {
            "commented",
            "approved",
            "changes_requested",
            "dismissed",
        }:
            _reject("invalid_review_state")


def _validate_fields(family: str, fields: Any, *, create: bool) -> dict[str, Any]:
    if not isinstance(fields, dict) or not fields:
        _reject("invalid_fields")
    if not set(fields) <= _SCALAR_FIELDS[family]:
        _reject("unknown_field")
    if create and not _REQUIRED_CREATE_FIELDS[family] <= set(fields):
        _reject("missing_create_field")
    immutable = {
        "reaction": {"emoji"},
        "notification": {"kind", "recipient"},
        "review": {"revision"},
    }.get(family, set())
    if not create and set(fields) & immutable:
        _reject("immutable_field")
    for name, value in fields.items():
        _validate_field(family, name, value)
    return fields


def _validate_set_value(family: str, field_name: str, value: Any) -> None:
    if field_name in {"assignees", "reviewers"}:
        _b64url_decode(value, 32, "invalid_set_identity")
        return
    if field_name == "labels":
        if not _valid_object_id(value, "label"):
            _reject("invalid_label_reference")
        return
    if field_name in {"subjects", "items"}:
        if not isinstance(value, dict) or set(value) != {"family", "object"}:
            _reject("invalid_set_reference")
        if value.get("family") not in FAMILIES or not _valid_object_id(
            value.get("object"), value.get("family")
        ):
            _reject("invalid_set_reference")
        return
    _reject("invalid_set_field")


def _validate_legacy(value: Any) -> None:
    if not isinstance(value, dict) or set(value) != {
        "schema",
        "digest",
        "signatureStatus",
    }:
        _reject("invalid_legacy_provenance")
    _validate_text(value.get("schema"), maximum=128, empty=False)
    if not _SHA256_RE.fullmatch(str(value.get("digest", ""))):
        _reject("invalid_legacy_digest")
    if value.get("signatureStatus") not in {"valid", "invalid", "unavailable"}:
        _reject("invalid_legacy_signature_status")


def _validate_payload(family: str, action: str, payload: Any) -> None:
    if not isinstance(payload, dict):
        _reject("invalid_payload")
    if action in {"create", "decide"}:
        if not {"objectKey", "fields"} <= set(payload) or not set(payload) <= {
            "objectKey",
            "fields",
            "legacy",
        }:
            _reject("invalid_create_payload")
        _b64url_decode(payload["objectKey"], 16, "invalid_object_key")
        _validate_fields(family, payload["fields"], create=True)
        if "legacy" in payload:
            _validate_legacy(payload["legacy"])
    elif action == "update":
        if set(payload) != {"fields"}:
            _reject("invalid_update_payload")
        _validate_fields(family, payload["fields"], create=False)
    elif action == "add":
        if set(payload) != {"field", "values"}:
            _reject("invalid_add_payload")
        field_name = payload.get("field")
        values = payload.get("values")
        if field_name not in _SET_FIELDS[family] or not isinstance(values, list) or not values:
            _reject("invalid_add_payload")
        encoded = [canonical_json(value) for value in values]
        if encoded != sorted(set(encoded)):
            _reject("noncanonical_set_values")
        for value in values:
            _validate_set_value(family, field_name, value)
    elif action == "remove":
        if set(payload) != {"field", "values", "observed"}:
            _reject("invalid_remove_payload")
        field_name = payload.get("field")
        values = payload.get("values")
        observed = payload.get("observed")
        if field_name not in _SET_FIELDS[family] or not isinstance(values, list) or not values:
            _reject("invalid_remove_payload")
        encoded = [canonical_json(value) for value in values]
        if encoded != sorted(set(encoded)):
            _reject("noncanonical_set_values")
        for value in values:
            _validate_set_value(family, field_name, value)
        _validate_event_id_list(observed, allow_empty=False)
    elif action == "tombstone":
        if set(payload) != {"reasonCode"} or not _REASON_RE.fullmatch(
            str(payload.get("reasonCode", ""))
        ):
            _reject("invalid_tombstone_payload")
    elif action == "restore":
        if set(payload) != {"reasonCode", "observed"} or not _REASON_RE.fullmatch(
            str(payload.get("reasonCode", ""))
        ):
            _reject("invalid_restore_payload")
        _validate_event_id_list(payload.get("observed"), allow_empty=False)
    elif action == "appeal":
        if not {"predecessor", "reasonCode"} <= set(payload) or not set(payload) <= {
            "predecessor",
            "reasonCode",
            "body",
        }:
            _reject("invalid_appeal_payload")
        if not _SHA256_RE.fullmatch(str(payload.get("predecessor", ""))):
            _reject("invalid_appeal_predecessor")
        if not _REASON_RE.fullmatch(str(payload.get("reasonCode", ""))):
            _reject("invalid_reason_code")
        if "body" in payload:
            _validate_text(payload["body"], maximum=100_000)
    elif action == "resolve":
        if set(payload) != {"predecessor", "appeal", "fields"}:
            _reject("invalid_resolution_payload")
        for name in ("predecessor", "appeal"):
            if not _SHA256_RE.fullmatch(str(payload.get(name, ""))):
                _reject("invalid_resolution_lineage")
        _validate_fields(family, payload["fields"], create=False)
        if "decision" not in payload["fields"] or "reasonCode" not in payload["fields"]:
            _reject("incomplete_resolution")
    else:
        _reject("unknown_action")


def _validate_event_id_list(value: Any, *, allow_empty: bool) -> list[str]:
    if not isinstance(value, list) or (not allow_empty and not value):
        _reject("invalid_event_id_list")
    if any(not isinstance(item, str) or not _SHA256_RE.fullmatch(item) for item in value):
        _reject("invalid_event_id")
    if value != sorted(set(value)):
        _reject("noncanonical_event_id_list")
    return value


def signing_bytes(event: Mapping[str, Any]) -> bytes:
    if set(event) == _TOP_LEVEL_KEYS:
        unsigned = {key: event[key] for key in _UNSIGNED_KEYS}
    elif set(event) == _SIGNED_NO_ID_KEYS:
        unsigned = {key: event[key] for key in _UNSIGNED_KEYS}
    elif set(event) == _UNSIGNED_KEYS:
        unsigned = dict(event)
    else:
        _reject("invalid_envelope_keys")
    return SIGNING_DOMAIN + canonical_json(unsigned)


def event_id(event: Mapping[str, Any]) -> str:
    signature = _b64url_decode(event.get("signature"), 64, "invalid_signature_encoding")
    digest = hashlib.sha256(signing_bytes(event) + b"\x00" + signature).hexdigest()
    return "sha256:" + digest


def finish_event(unsigned_event: Mapping[str, Any], signature: str) -> dict[str, Any]:
    """Attach a detached Ed25519 signature and derive the immutable event id."""

    if set(unsigned_event) != _UNSIGNED_KEYS:
        _reject("invalid_envelope_keys")
    event = dict(_normalize_json(unsigned_event))
    event["signature"] = signature
    event["eventId"] = event_id(event)
    validate_event_shape(event)
    return event


def validate_event_shape(event: Mapping[str, Any]) -> None:
    if set(event) != _TOP_LEVEL_KEYS:
        _reject("invalid_envelope_keys")
    if event.get("schema") != EVENT_SCHEMA:
        _reject("unsupported_schema")
    repository = event.get("repository")
    if not isinstance(repository, str) or not _REPOSITORY_RE.fullmatch(repository):
        _reject("invalid_repository")
    if not _SHA256_RE.fullmatch(str(event.get("policy", ""))):
        _reject("invalid_policy_id")
    family = event.get("family")
    action = event.get("action")
    if family not in FAMILIES or action not in _ACTIONS[family]:
        _reject("unknown_family_action")
    if not _valid_object_id(event.get("object"), family):
        _reject("invalid_object_id")
    _b64url_decode(event.get("author"), 32, "invalid_author")
    _b64url_decode(event.get("signature"), 64, "invalid_signature_encoding")
    for name in ("sequence", "clock", "issuedAt"):
        value = event.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= _SAFE_INTEGER:
            _reject("invalid_" + name)
    parents = _validate_event_id_list(event.get("parents"), allow_empty=True)
    if len(parents) > _MAX_PARENTS:
        _reject("too_many_parents")
    references = _validate_references(family, event.get("references"))
    _validate_payload(family, action, event.get("payload"))
    if event.get("eventId") != event_id(event):
        _reject("event_id_mismatch")
    if action in {"create", "decide"}:
        expected = object_id(
            repository,
            family,
            event["author"],
            event["payload"]["objectKey"],
            references,
        )
        if event["object"] != expected:
            _reject("object_id_mismatch")


def verify_event(
    event: Mapping[str, Any],
    policy: Mapping[str, Any],
    *,
    repository: str | None = None,
) -> None:
    validate_event_shape(event)
    expected_policy = policy_id(policy)
    expected_repository = repository or str(policy["repository"])
    if event["repository"] != expected_repository:
        _reject("wrong_repository")
    if event["policy"] != expected_policy:
        _reject("unknown_policy")
    public_key = _b64url_decode(event["author"], 32, "invalid_author")
    signature = _b64url_decode(event["signature"], 64, "invalid_signature_encoding")
    if not ed25519_verify(public_key, signature, signing_bytes(event)):
        _reject("invalid_signature")


def unsigned_event(
    *,
    repository: str,
    policy: str,
    family: str,
    object: str,
    action: str,
    author: str,
    sequence: int,
    clock: int,
    issued_at: int,
    parents: Sequence[str],
    references: Sequence[Mapping[str, Any]],
    payload: Mapping[str, Any],
) -> dict[str, Any]:
    """Construct the exact unsigned map a client signs (without choosing time)."""

    event = {
        "schema": EVENT_SCHEMA,
        "repository": repository,
        "policy": policy,
        "family": family,
        "object": object,
        "action": action,
        "author": author,
        "sequence": sequence,
        "clock": clock,
        "issuedAt": issued_at,
        "parents": list(parents),
        "references": [dict(reference) for reference in references],
        "payload": dict(payload),
    }
    return _normalize_json(event)


@dataclass
class FoldResult:
    state: dict[str, Any]
    audit: dict[str, Any]
    telemetry: dict[str, Any]
    raw_events: tuple[bytes, ...]

    @property
    def state_digest(self) -> str:
        return "sha256:" + hashlib.sha256(canonical_json(self.state)).hexdigest()


@dataclass
class SignedEventLog:
    """An append-only set of exact envelopes with deterministic export/fold."""

    _raw: dict[str, bytes] = field(default_factory=dict)
    deliveries: int = 0

    def append(self, event: bytes | str | Mapping[str, Any]) -> str:
        raw = canonical_json(event) if isinstance(event, Mapping) else (
            event.encode("utf-8") if isinstance(event, str) else event
        )
        if not isinstance(raw, bytes):
            _reject("invalid_raw_event")
        digest = "sha256:" + hashlib.sha256(raw).hexdigest()
        self.deliveries += 1
        self._raw.setdefault(digest, raw)
        return digest

    def extend(self, events: Iterable[bytes | str | Mapping[str, Any]]) -> None:
        for event in events:
            self.append(event)

    @property
    def raw_events(self) -> tuple[bytes, ...]:
        return tuple(self._raw[digest] for digest in sorted(self._raw))

    def export_ndjson(self) -> bytes:
        return b"".join(raw + b"\n" for raw in self.raw_events)

    @classmethod
    def from_ndjson(cls, data: bytes | str) -> "SignedEventLog":
        if isinstance(data, str):
            data = data.encode("utf-8")
        log = cls()
        # LF is the portable record separator. Preserve a stray CR in the raw
        # record so CRLF or other alternate encodings are audited, not quietly
        # rewritten into a canonical signed envelope.
        for line in data.split(b"\n"):
            if line:
                log.append(line)
        return log

    def fold(self, policy: Mapping[str, Any]) -> FoldResult:
        return fold_events(self.raw_events, policy, deliveries=self.deliveries)


def _raw_digest(raw: bytes) -> str:
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def _event_order(event: Mapping[str, Any]) -> tuple[int, str]:
    return int(event["clock"]), str(event["eventId"])


def _ancestor_map(events: Mapping[str, Mapping[str, Any]]) -> dict[str, frozenset[str]]:
    indegree = {event_id_value: 0 for event_id_value in events}
    children: dict[str, list[str]] = defaultdict(list)
    for event_id_value, event in events.items():
        for parent in event["parents"]:
            if parent in events:
                indegree[event_id_value] += 1
                children[parent].append(event_id_value)

    ready = deque(sorted(key for key, degree in indegree.items() if degree == 0))
    ancestors: dict[str, frozenset[str]] = {}
    while ready:
        event_id_value = ready.popleft()
        lineage: set[str] = set()
        for parent in events[event_id_value]["parents"]:
            if parent in events:
                lineage.add(parent)
                lineage.update(ancestors[parent])
        ancestors[event_id_value] = frozenset(lineage)
        for child in sorted(children[event_id_value]):
            indegree[child] -= 1
            if indegree[child] == 0:
                ready.append(child)

    if len(ancestors) != len(events):
        _reject("causal_cycle")
    return ancestors


def _authorization(
    event: Mapping[str, Any],
    creator: str,
    policy: Mapping[str, Any],
    subject_creator: str = "",
) -> bool:
    family = event["family"]
    action = event["action"]
    author = event["author"]
    maintainers = set(policy["maintainers"])
    moderators = set(policy["moderators"])
    notifiers = set(policy["notifiers"])
    if family == "moderation":
        if action in {"decide", "resolve"}:
            return author in moderators
        return action == "appeal" and bool(subject_creator) and author == subject_creator
    if action == "create":
        if family in {"label", "milestone", "board"}:
            return author in maintainers
        if family == "notification":
            recipient = event["payload"]["fields"]["recipient"]
            return author == recipient or author in notifiers
        return True
    if family in {"issue", "pull_request"}:
        return author == creator or author in maintainers
    if family in {"comment", "reaction"}:
        return author == creator
    if family == "review":
        return author == creator or (
            action == "tombstone" and author in maintainers
        )
    if family in {"label", "milestone", "board"}:
        return author in maintainers
    if family == "notification":
        return author == event.get("_recipient", "")
    return False


def _semantic_event_error(
    event: Mapping[str, Any],
    accepted: Mapping[str, Mapping[str, Any]],
    ancestors: Mapping[str, frozenset[str]],
) -> str | None:
    event_id_value = event["eventId"]
    lineage = ancestors[event_id_value]
    action = event["action"]
    payload = event["payload"]
    if action == "remove":
        expected_values = {canonical_json(value) for value in payload["values"]}
        covered_values: set[bytes] = set()
        for observed in payload["observed"]:
            source = accepted.get(observed)
            source_values = (
                set()
                if source is None or source.get("action") != "add"
                else {
                    canonical_json(value) for value in source["payload"]["values"]
                }
            )
            if (
                observed not in lineage
                or source is None
                or source["object"] != event["object"]
                or source["action"] != "add"
                or source["payload"]["field"] != payload["field"]
                or not expected_values & source_values
            ):
                return "invalid_observed_add"
            covered_values.update(expected_values & source_values)
        if not expected_values <= covered_values:
            return "invalid_observed_add"
    elif action == "restore":
        for observed in payload["observed"]:
            source = accepted.get(observed)
            if (
                observed not in lineage
                or source is None
                or source["object"] != event["object"]
                or source["action"] != "tombstone"
            ):
                return "invalid_observed_tombstone"
    elif action == "appeal":
        predecessor = payload["predecessor"]
        source = accepted.get(predecessor)
        if (
            predecessor not in lineage
            or source is None
            or source["object"] != event["object"]
            or source["action"] not in {"decide", "resolve"}
        ):
            return "invalid_moderation_lineage"
    elif action == "resolve":
        predecessor = accepted.get(payload["predecessor"])
        appeal = accepted.get(payload["appeal"])
        if (
            payload["predecessor"] not in lineage
            or payload["appeal"] not in lineage
            or predecessor is None
            or appeal is None
            or predecessor["object"] != event["object"]
            or appeal["object"] != event["object"]
            or predecessor["action"] not in {"decide", "resolve"}
            or appeal["action"] != "appeal"
            or appeal["payload"]["predecessor"] != payload["predecessor"]
        ):
            return "invalid_moderation_lineage"
    return None


def _maximal_events(
    candidates: Sequence[Mapping[str, Any]],
    ancestors: Mapping[str, frozenset[str]],
) -> list[Mapping[str, Any]]:
    ids = {candidate["eventId"] for candidate in candidates}
    return sorted(
        [
            candidate
            for candidate in candidates
            if not any(
                candidate["eventId"] in ancestors[other]
                for other in ids
                if other != candidate["eventId"]
            )
        ],
        key=_event_order,
    )


def _fold_object(
    family: str,
    object_events: Sequence[Mapping[str, Any]],
    ancestors: Mapping[str, frozenset[str]],
) -> dict[str, Any]:
    ordered = sorted(object_events, key=_event_order)
    creates = [event for event in ordered if event["action"] in {"create", "decide"}]
    first = creates[0]
    writes: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for event in ordered:
        if event["action"] in {"create", "decide", "update", "resolve"}:
            for name, value in event["payload"]["fields"].items():
                writes[name].append({"event": event, "value": value})

    values: dict[str, Any] = {}
    conflicts: dict[str, Any] = {}
    for name in sorted(writes):
        maximal_ids = {
            event["eventId"]
            for event in _maximal_events(
                [write["event"] for write in writes[name]], ancestors
            )
        }
        maximal = [write for write in writes[name] if write["event"]["eventId"] in maximal_ids]
        by_value: dict[bytes, list[dict[str, Any]]] = defaultdict(list)
        for write in maximal:
            by_value[canonical_json(write["value"])].append(write)
        if len(by_value) == 1:
            values[name] = maximal[0]["value"]
        else:
            conflicts[name] = sorted(
                [
                    {"eventId": write["event"]["eventId"], "value": write["value"]}
                    for write in maximal
                ],
                key=lambda item: item["eventId"],
            )

    sets: dict[str, list[Any]] = {}
    for field_name in sorted(_SET_FIELDS[family]):
        additions: dict[tuple[str, bytes], Any] = {}
        removed: set[tuple[str, bytes]] = set()
        for event in ordered:
            if event["action"] == "add" and event["payload"]["field"] == field_name:
                for value in event["payload"]["values"]:
                    additions[(event["eventId"], canonical_json(value))] = value
            elif event["action"] == "remove" and event["payload"]["field"] == field_name:
                wanted = {canonical_json(value) for value in event["payload"]["values"]}
                for observed in event["payload"]["observed"]:
                    for encoded in wanted:
                        if (observed, encoded) in additions:
                            removed.add((observed, encoded))
        visible: dict[bytes, Any] = {}
        for tag, value in additions.items():
            if tag not in removed:
                visible[canonical_json(value)] = value
        sets[field_name] = [visible[key] for key in sorted(visible)]

    tombstones = {
        event["eventId"]
        for event in ordered
        if event["action"] == "tombstone"
    }
    for event in ordered:
        if event["action"] == "restore":
            tombstones.difference_update(event["payload"]["observed"])

    state: dict[str, Any] = {
        "family": family,
        "object": first["object"],
        "creator": first["author"],
        "references": first["references"],
        "values": values,
        "sets": sets,
        "conflicts": conflicts,
        "tombstoned": bool(tombstones),
        "tombstones": sorted(tombstones),
        "visibility": "visible",
        "moderation": [],
        "heads": [
            event["eventId"] for event in _maximal_events(ordered, ancestors)
        ],
        "history": [event["eventId"] for event in ordered],
    }
    if len(creates) > 1:
        state["conflicts"]["object"] = [event["eventId"] for event in creates]
    if family == "moderation":
        appeals = [event for event in ordered if event["action"] == "appeal"]
        resolved = {
            event["payload"]["appeal"]
            for event in ordered
            if event["action"] == "resolve"
        }
        state["pendingAppeals"] = sorted(
            event["eventId"] for event in appeals if event["eventId"] not in resolved
        )
    return state


def fold_events(
    raw_events: Iterable[bytes | str | Mapping[str, Any]],
    policy: Mapping[str, Any],
    *,
    deliveries: int | None = None,
) -> FoldResult:
    """Verify and deterministically fold a collaboration event multiset."""

    policy_digest = policy_id(policy)
    repository = str(policy["repository"])
    unique_raw: dict[str, bytes] = {}
    total = 0
    for supplied in raw_events:
        total += 1
        raw = canonical_json(supplied) if isinstance(supplied, Mapping) else (
            supplied.encode("utf-8") if isinstance(supplied, str) else supplied
        )
        if not isinstance(raw, bytes):
            raw = b""
        unique_raw.setdefault(_raw_digest(raw), raw)
    received = max(total, deliveries or 0)

    candidates: dict[str, dict[str, Any]] = {}
    rejected: dict[str, str] = {}
    signature_valid = 0
    for digest in sorted(unique_raw):
        raw = unique_raw[digest]
        try:
            event = parse_canonical_event(raw)
            validate_event_shape(event)
            if event["repository"] != repository:
                _reject("wrong_repository")
            if event["policy"] != policy_digest:
                _reject("unknown_policy")
            public = _b64url_decode(event["author"], 32, "invalid_author")
            signature = _b64url_decode(
                event["signature"], 64, "invalid_signature_encoding"
            )
            if not ed25519_verify(public, signature, signing_bytes(event)):
                _reject("invalid_signature")
            signature_valid += 1
        except CollaborationError as exc:
            rejected[digest] = exc.code
            continue
        existing = candidates.get(event["eventId"])
        if existing is not None and existing != event:
            rejected[event["eventId"]] = "event_id_collision"
            candidates.pop(event["eventId"], None)
        else:
            candidates[event["eventId"]] = event

    # A key may never sign two meanings in one repository sequence slot.  Both
    # sides of the equivocation are excluded, making arrival order irrelevant.
    sequence_slots: dict[tuple[str, int], list[str]] = defaultdict(list)
    for current in candidates.values():
        sequence_slots[(current["author"], current["sequence"])].append(
            current["eventId"]
        )
    equivocations = 0
    for ids in sequence_slots.values():
        if len(ids) > 1:
            equivocations += 1
            for current_id in ids:
                rejected[current_id] = "author_sequence_equivocation"
                candidates.pop(current_id, None)

    try:
        ancestors = _ancestor_map(candidates)
    except CollaborationError as exc:
        # A real hash cycle is computationally infeasible, but malformed test
        # or alternate-hash inputs still fail closed as a whole DAG.
        for current_id in candidates:
            rejected[current_id] = exc.code
        candidates = {}
        ancestors = {}

    structural: dict[str, dict[str, Any]] = {}
    pending: dict[str, str] = {}
    for current in sorted(candidates.values(), key=_event_order):
        current_id = current["eventId"]
        missing = [parent for parent in current["parents"] if parent not in candidates]
        if missing:
            pending[current_id] = "missing_parent"
            continue
        expected_clock = (
            0
            if not current["parents"]
            else 1 + max(candidates[parent]["clock"] for parent in current["parents"])
        )
        if current["clock"] != expected_clock:
            rejected[current_id] = "invalid_clock"
            continue
        same_author_ancestors = [
            candidates[ancestor]["sequence"]
            for ancestor in ancestors[current_id]
            if candidates[ancestor]["author"] == current["author"]
        ]
        if same_author_ancestors and current["sequence"] <= max(same_author_ancestors):
            rejected[current_id] = "sequence_rollback"
            continue
        structural[current_id] = current

    creates_by_object: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for current in structural.values():
        if current["action"] in {"create", "decide"}:
            creates_by_object[current["object"]].append(current)

    accepted: dict[str, dict[str, Any]] = {}
    for current in sorted(structural.values(), key=_event_order):
        current_id = current["eventId"]
        if current_id in rejected or current_id in pending:
            continue
        bad_parent = next(
            (parent for parent in current["parents"] if parent not in accepted), None
        )
        if bad_parent is not None:
            if bad_parent in pending or bad_parent not in candidates:
                pending[current_id] = "pending_parent"
            else:
                rejected[current_id] = "invalid_parent"
            continue
        creates = creates_by_object.get(current["object"], [])
        if not creates:
            pending[current_id] = "missing_create"
            continue
        create_ids = {event["eventId"] for event in creates}
        if current["action"] not in {"create", "decide"} and not (
            create_ids & set(ancestors[current_id])
        ):
            rejected[current_id] = "not_descendant_of_create"
            continue
        creator = sorted(creates, key=_event_order)[0]["author"]
        canonical_refs = sorted(creates, key=_event_order)[0]["references"]
        if current["references"] != canonical_refs:
            rejected[current_id] = "object_reference_mismatch"
            continue
        subject_creator = ""
        if current["family"] == "moderation" and current["action"] == "appeal":
            subject = next(
                ref for ref in current["references"] if ref["relation"] == "subject"
            )
            subject_creates = creates_by_object.get(subject["object"], [])
            if subject_creates:
                subject_creator = sorted(subject_creates, key=_event_order)[0]["author"]
            if not subject_creator:
                pending[current_id] = "missing_subject_create"
                continue
        auth_event = dict(current)
        if current["family"] == "notification":
            auth_event["_recipient"] = creates[0]["payload"]["fields"]["recipient"]
        if not _authorization(auth_event, creator, policy, subject_creator):
            rejected[current_id] = "unauthorized"
            continue
        semantic_error = _semantic_event_error(current, accepted, ancestors)
        if semantic_error:
            rejected[current_id] = semantic_error
            continue
        accepted[current_id] = current

    grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for current in accepted.values():
        grouped[(current["family"], current["object"])].append(current)

    objects: dict[str, dict[str, Any]] = {family: {} for family in FAMILIES}
    for (family, current_object), events in sorted(grouped.items()):
        objects[family][current_object] = _fold_object(family, events, ancestors)

    # Apply signed moderation after every object has its own deterministic state.
    moderation_by_subject: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for moderation in objects["moderation"].values():
        subject = next(
            ref for ref in moderation["references"] if ref["relation"] == "subject"
        )
        moderation_by_subject[subject["object"]].append(moderation)
    for family in FAMILIES:
        if family == "moderation":
            continue
        for current_object, state in objects[family].items():
            decisions = moderation_by_subject.get(current_object, [])
            if not decisions:
                continue
            state["moderation"] = sorted(item["object"] for item in decisions)
            resolved: set[str] = set()
            conflict = False
            for decision in decisions:
                if "decision" in decision["conflicts"]:
                    conflict = True
                elif "decision" in decision["values"]:
                    resolved.add(decision["values"]["decision"])
            if conflict or len(resolved) > 1:
                state["visibility"] = "quarantined"
                state["moderationConflict"] = sorted(
                    item["object"] for item in decisions
                )
            elif resolved == {"hide"}:
                state["visibility"] = "hidden"
            elif resolved == {"quarantine"}:
                state["visibility"] = "quarantined"

    state = {
        "schema": STATE_SCHEMA,
        "repository": repository,
        "policy": policy_digest,
        "objects": objects,
        "pending": sorted(pending),
    }
    rejection_counts = Counter(rejected.values())
    conflict_count = sum(
        len(value["conflicts"])
        + (1 if "moderationConflict" in value else 0)
        for family_objects in objects.values()
        for value in family_objects.values()
    )
    telemetry = {
        "received": received,
        "uniqueRaw": len(unique_raw),
        "signatureValid": signature_valid,
        "accepted": len(accepted),
        "pending": len(pending),
        "rejected": len(rejected),
        "duplicateDeliveries": max(0, received - len(unique_raw)),
        "equivocations": equivocations,
        "conflicts": conflict_count,
        "rejectionReasons": {
            reason: rejection_counts[reason] for reason in sorted(rejection_counts)
        },
        "acceptedByFamily": {
            family: sum(1 for event in accepted.values() if event["family"] == family)
            for family in FAMILIES
        },
    }
    audit = {
        "accepted": sorted(accepted),
        "pending": [
            {"eventId": current_id, "reason": pending[current_id]}
            for current_id in sorted(pending)
        ],
        "rejected": [
            {"id": current_id, "reason": rejected[current_id]}
            for current_id in sorted(rejected)
        ],
    }
    return FoldResult(
        state=state,
        audit=audit,
        telemetry=telemetry,
        raw_events=tuple(unique_raw[digest] for digest in sorted(unique_raw)),
    )


def migration_draft(
    legacy_record: Mapping[str, Any],
    *,
    repository: str,
    policy: str,
    family: str,
    importer: str,
    sequence: int,
    clock: int,
    issued_at: int,
    parents: Sequence[str] = (),
    references: Sequence[Mapping[str, Any]] = (),
    signature_status: str = "unavailable",
) -> dict[str, Any]:
    """Create a signable v1 import event without impersonating a legacy author.

    The exact legacy bytes remain in the migration bundle.  The new event binds
    their digest and verification status, and is signed by the importer named in
    ``author``.  No old unsigned top-level projection is silently trusted.
    """

    if family not in FAMILIES or legacy_record.get("schema") != _LEGACY_SCHEMAS[family]:
        _reject("unsupported_legacy_schema")
    raw = canonical_json(legacy_record)
    fields = {
        name: legacy_record[name]
        for name in sorted(_SCALAR_FIELDS[family])
        if name in legacy_record
    }
    missing = _REQUIRED_CREATE_FIELDS[family] - set(fields)
    if missing:
        _reject("missing_legacy_field", ",".join(sorted(missing)))
    object_key = _b64url(
        hashlib.sha256(
            b"forkmesh-collaboration-legacy-object-v1\n"
            + family.encode("ascii")
            + b"\n"
            + raw
        ).digest()[:16]
    )
    normalized_refs = _validate_references(family, list(references))
    current_object = object_id(
        repository, family, importer, object_key, normalized_refs
    )
    action = "decide" if family == "moderation" else "create"
    payload = {
        "objectKey": object_key,
        "fields": fields,
        "legacy": {
            "schema": legacy_record["schema"],
            "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
            "signatureStatus": signature_status,
        },
    }
    return unsigned_event(
        repository=repository,
        policy=policy,
        family=family,
        object=current_object,
        action=action,
        author=importer,
        sequence=sequence,
        clock=clock,
        issued_at=issued_at,
        parents=parents,
        references=normalized_refs,
        payload=payload,
    )


__all__ = [
    "CollaborationError",
    "EVENT_SCHEMA",
    "FAMILIES",
    "FoldResult",
    "POLICY_SCHEMA",
    "STATE_SCHEMA",
    "SignedEventLog",
    "canonical_json",
    "ed25519_verify",
    "event_id",
    "finish_event",
    "fold_events",
    "migration_draft",
    "object_id",
    "parse_canonical_event",
    "policy_document",
    "policy_id",
    "repository_id",
    "signing_bytes",
    "unsigned_event",
    "validate_event_shape",
    "verify_event",
]
