"""Signed browser-session grants accepted by independent mirror dashboards.

The controller signs only a narrow identity/session projection.  A mirror can
verify it with the already-published router key; it never receives the D1
session row, password material, the controller bearer token, or a signing key.
"""

from __future__ import annotations

import base64
import json
import re
from typing import Any, Callable


SESSION_TYPE = "forkmesh.portable-session"
SESSION_AUDIENCE = "forkmesh-mirror-dashboard"
SESSION_SCHEMA_VERSION = 1
SESSION_MESSAGE_PREFIX = b"forkmesh-portable-session-v1\n"
SESSION_MAX_LIFETIME_MS = 12 * 60 * 60 * 1000
SESSION_CLOCK_SKEW_MS = 60 * 1000
SESSION_TOKEN_RE = re.compile(
    r"^v1\.([A-Za-z0-9_-]{80,1024})\.([A-Za-z0-9_-]{86})$"
)
_OPAQUE_RE = re.compile(r"^[A-Za-z0-9_-]{16,128}$")
_ACCOUNT_ID_RE = re.compile(r"^[0-9a-f]{32,128}$")
_NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$")


class PortableSessionError(ValueError):
    """A portable session is malformed, stale, or not authentic."""


def _b64url_encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def _b64url_decode(value: str, expected_size: int | None = None) -> bytes:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise PortableSessionError("portable session encoding is invalid")
    try:
        decoded = base64.urlsafe_b64decode(
            value + "=" * ((4 - len(value) % 4) % 4)
        )
    except (ValueError, TypeError) as error:
        raise PortableSessionError(
            "portable session encoding is invalid"
        ) from error
    if expected_size is not None and len(decoded) != expected_size:
        raise PortableSessionError("portable session encoding is invalid")
    # Padding-free base64url leaves unused low bits in some final characters.
    # Python accepts alternate spellings that decode to the same bytes, so a
    # visibly modified signed token could otherwise still verify. Accept only
    # the canonical textual representation.
    if _b64url_encode(decoded) != value:
        raise PortableSessionError("portable session encoding is invalid")
    return decoded


def canonical_json(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=True,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise PortableSessionError("portable session claim is invalid") from error


def build_claim(
    *,
    account_id: str,
    account_name: str,
    session_id: str,
    grant_id: str,
    issued_at: int,
    expires_at: int,
) -> dict[str, Any]:
    """Build the exact privacy-bounded claim signed by the controller."""
    claim = {
        "aud": SESSION_AUDIENCE,
        "exp": expires_at,
        "iat": issued_at,
        "jti": grant_id,
        "name": account_name,
        "sid": session_id,
        "sub": account_id,
        "type": SESSION_TYPE,
        "v": SESSION_SCHEMA_VERSION,
    }
    validate_claim(claim, now_ms=issued_at, allow_current_expiry=True)
    return claim


def validate_claim(
    value: Any,
    *,
    now_ms: int,
    allow_current_expiry: bool = False,
) -> dict[str, Any]:
    required = {
        "aud", "exp", "iat", "jti", "name", "sid", "sub", "type", "v"
    }
    if not isinstance(value, dict) or set(value) != required:
        raise PortableSessionError("portable session claim is invalid")
    if (
        value.get("type") != SESSION_TYPE
        or value.get("aud") != SESSION_AUDIENCE
        or value.get("v") != SESSION_SCHEMA_VERSION
        or not isinstance(value.get("iat"), int)
        or isinstance(value.get("iat"), bool)
        or not isinstance(value.get("exp"), int)
        or isinstance(value.get("exp"), bool)
        or not _ACCOUNT_ID_RE.fullmatch(str(value.get("sub") or ""))
        or not _NAME_RE.fullmatch(str(value.get("name") or ""))
        or not _OPAQUE_RE.fullmatch(str(value.get("sid") or ""))
        or not _OPAQUE_RE.fullmatch(str(value.get("jti") or ""))
    ):
        raise PortableSessionError("portable session claim is invalid")
    issued_at = value["iat"]
    expires_at = value["exp"]
    if (
        issued_at <= 0
        or expires_at <= issued_at
        or expires_at - issued_at > SESSION_MAX_LIFETIME_MS
        or issued_at > int(now_ms) + SESSION_CLOCK_SKEW_MS
        or (
            not allow_current_expiry
            and expires_at <= int(now_ms) - SESSION_CLOCK_SKEW_MS
        )
    ):
        raise PortableSessionError("portable session is expired or not current")
    return dict(value)


def encode_claim(claim: dict[str, Any]) -> str:
    return _b64url_encode(canonical_json(claim))


def signing_message(encoded_claim: str) -> bytes:
    _b64url_decode(encoded_claim)
    return SESSION_MESSAGE_PREFIX + encoded_claim.encode("ascii")


def assemble_token(encoded_claim: str, signature: str) -> str:
    _b64url_decode(encoded_claim)
    _b64url_decode(signature, 64)
    token = "v1." + encoded_claim + "." + signature
    if not SESSION_TOKEN_RE.fullmatch(token):
        raise PortableSessionError("portable session token is invalid")
    return token


def parse_token(
    token: str,
    *,
    now_ms: int,
    verify: Callable[[bytes, bytes], bool],
) -> dict[str, Any]:
    """Validate exact framing, canonical payload, time, and signature."""
    match = SESSION_TOKEN_RE.fullmatch(str(token or ""))
    if not match:
        raise PortableSessionError("portable session token is invalid")
    encoded_claim, encoded_signature = match.groups()
    raw_claim = _b64url_decode(encoded_claim)
    signature = _b64url_decode(encoded_signature, 64)
    try:
        claim = json.loads(raw_claim.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise PortableSessionError("portable session claim is invalid") from error
    claim = validate_claim(claim, now_ms=now_ms)
    if canonical_json(claim) != raw_claim:
        raise PortableSessionError("portable session claim is not canonical")
    try:
        verified = bool(verify(signing_message(encoded_claim), signature))
    except Exception as error:
        raise PortableSessionError(
            "portable session signature is invalid"
        ) from error
    if not verified:
        raise PortableSessionError("portable session signature is invalid")
    return claim


def cookie_value(header: Any, name: str = "forkmesh_portable") -> str:
    """Extract one bounded cookie without accepting duplicate ambiguity."""
    raw = str(header or "")
    if len(raw) > 16 * 1024:
        return ""
    found = []
    for item in raw.split(";"):
        key, separator, value = item.strip().partition("=")
        if separator and key == name:
            found.append(value.strip())
    return found[0] if len(found) == 1 else ""
