"""Pure validation helpers for ForkMesh security and privacy controls.

The Worker imports this module, but it intentionally has no Workers/JavaScript
dependencies so the security boundary can be unit-tested with normal CPython.
Only generalized, allowlisted audit metadata may reach administrative APIs.
Raw network identifiers and owner-sealed payload plaintext never belong here.
"""

import base64
import hashlib
import json
import re


ROLE_VALUES = frozenset({
    "supporting_member",
    "repository_contributor",
    "repository_administrator",
    "mirror_operator",
    "organization_member",
    "organization_administrator",
    "moderator",
    "security_reviewer",
    "platform_administrator",
    "automated_agent",
})

PUBLIC_ROLE_VALUES = frozenset({
    "supporting_member",
    "repository_contributor",
    "repository_administrator",
    "mirror_operator",
    "organization_member",
    "organization_administrator",
    "automated_agent",
})

ROLE_SCOPE_TYPES = {
    "supporting_member": frozenset({"platform"}),
    "repository_contributor": frozenset({"repository"}),
    "repository_administrator": frozenset({"repository"}),
    "mirror_operator": frozenset({"platform", "repository"}),
    "organization_member": frozenset({"organization"}),
    "organization_administrator": frozenset({"organization"}),
    "moderator": frozenset({"platform", "organization", "repository"}),
    "security_reviewer": frozenset({"platform", "repository"}),
    "platform_administrator": frozenset({"platform"}),
    "automated_agent": frozenset({"platform", "repository"}),
}

MAX_AUDIT_DETAIL = 500
MAX_OWNER_ENVELOPE_BODY = 1024 * 1024
# An owner envelope carries one wrap entry per enrolled owner device (desktop
# vault plus companion devices such as the mobile node). The bound exists only
# to keep the framing small; enrollment itself is limited by the owner-keys
# registry.
MAX_OWNER_ENVELOPE_RECIPIENTS = 6

_INCIDENT_RE = re.compile(r"^[a-f0-9]{32}$")
_KEY_ID_RE = re.compile(r"^[A-Za-z0-9_-]{16,128}$")
_B64URL_RE = re.compile(r"^[A-Za-z0-9_-]+$")

_AUDIT_FORBIDDEN_KEYS = (
    "secret", "token", "password", "authorization", "cookie", "private",
    "seed", "body", "content", "email", "ip", "wallet", "signature",
)


def normalize_role(value):
    role = str(value or "").strip().lower().replace("-", "_")
    return role if role in ROLE_VALUES else ""


def normalize_scope_type(value):
    scope = str(value or "platform").strip().lower()
    return scope if scope in ("platform", "organization", "repository") else ""


def role_scope_allowed(role, scope_type):
    role = normalize_role(role)
    scope_type = normalize_scope_type(scope_type)
    return bool(role and scope_type in ROLE_SCOPE_TYPES.get(role, ()))


def public_roles(values):
    roles = []
    for value in values or []:
        role = normalize_role(value)
        if role in PUBLIC_ROLE_VALUES and role not in roles:
            roles.append(role)
    return roles


def normalize_incident_id(value):
    incident = str(value or "").strip().lower()
    return incident if _INCIDENT_RE.fullmatch(incident) else ""


def generalized_evidence_summary(value):
    """Return bounded, single-line audit text with controls removed."""
    raw = str(value or "")[:MAX_AUDIT_DETAIL * 2]
    clean = "".join(
        ch for ch in raw if ch in ("\n", "\t") or ord(ch) >= 32
    )
    return " ".join(clean.split())[:MAX_AUDIT_DETAIL]


def sanitize_audit_details(details):
    """Allow only short scalar metadata and drop secret-bearing key names."""
    if not isinstance(details, dict):
        return {}
    out = {}
    for key, value in list(details.items())[:20]:
        clean_key = re.sub(r"[^A-Za-z0-9_.-]", "", str(key or ""))[:48]
        lowered = clean_key.lower()
        if not clean_key or any(word in lowered for word in _AUDIT_FORBIDDEN_KEYS):
            continue
        if isinstance(value, bool):
            out[clean_key] = value
        elif isinstance(value, int):
            out[clean_key] = value
        elif isinstance(value, float):
            if value == value and value not in (float("inf"), float("-inf")):
                out[clean_key] = value
        elif isinstance(value, str):
            out[clean_key] = generalized_evidence_summary(value)[:160]
    return out


def validate_owner_public_bundle(value):
    """Validate a MirrorCrypto public-only recipient bundle.

    Only X25519 and ML-KEM public keys are accepted. The returned key id is
    derived from those exact bytes, so a caller cannot alias one public bundle
    under another recipient id.
    """
    if not isinstance(value, dict) or value.get("v") != 1:
        return None
    x25519 = str(value.get("x25519") or "").strip()
    mlkem768 = str(value.get("mlkem768") or "").strip()
    if not _valid_b64url(x25519, 32, 32):
        return None
    if not _valid_b64url(mlkem768, 1184, 1184):
        return None
    raw_x = _decode_b64url(x25519)
    raw_m = _decode_b64url(mlkem768)
    kid = base64.urlsafe_b64encode(
        hashlib.sha256(raw_x + raw_m).digest()
    ).decode().rstrip("=")
    return {
        "v": 1,
        "x25519": x25519,
        "mlkem768": mlkem768,
        "kid": kid,
    }


def validate_owner_envelope(value, max_body=MAX_OWNER_ENVELOPE_BODY):
    """Validate an opaque client-sealed envelope without decrypting it.

    This is the owner-device form of Qt MirrorCrypto's working hybrid
    envelope.  Each recipient entry carries the ephemeral X25519 public key,
    ML-KEM-768 ciphertext, and AES-GCM-wrapped content key; nonce/tag/body are
    the payload ciphertext.  One entry exists per enrolled owner device (the
    sealing device writes its own entry first), bounded by
    MAX_OWNER_ENVELOPE_RECIPIENTS with duplicate recipient ids refused.  The
    server checks only framing and bounds.  Recipient private keys and
    plaintext remain entirely client-side.
    """
    if not isinstance(value, dict):
        return None
    if value.get("kind") != "forkmesh.owner-sealed" or value.get("v") != 1:
        return None
    if value.get("alg") != "x25519+mlkem768/aes256gcm":
        return None
    nonce = str(value.get("nonce") or "").strip()
    tag = str(value.get("tag") or "").strip()
    body = str(value.get("body") or "").strip()
    recipients = value.get("recipients")
    if (not isinstance(recipients, list) or not recipients
            or len(recipients) > MAX_OWNER_ENVELOPE_RECIPIENTS):
        return None
    validated_recipients = []
    seen_kids = set()
    for recipient in recipients:
        if not isinstance(recipient, dict):
            return None
        kid = str(recipient.get("kid") or "").strip()
        ephemeral = str(recipient.get("x25519") or "").strip()
        kem_ciphertext = str(recipient.get("mlkem768") or "").strip()
        wrap_nonce = str(recipient.get("nonce") or "").strip()
        wrap_tag = str(recipient.get("tag") or "").strip()
        wrapped_key = str(recipient.get("key") or "").strip()
        if (not _KEY_ID_RE.fullmatch(kid)
                or kid in seen_kids
                or not _valid_b64url(ephemeral, 32, 32)
                or not _valid_b64url(kem_ciphertext, 1088, 1088)
                or not _valid_b64url(wrap_nonce, 12, 12)
                or not _valid_b64url(wrap_tag, 16, 16)
                or not _valid_b64url(wrapped_key, 32, 32)):
            return None
        seen_kids.add(kid)
        validated_recipients.append({
            "kid": kid,
            "x25519": ephemeral,
            "mlkem768": kem_ciphertext,
            "nonce": wrap_nonce,
            "tag": wrap_tag,
            "key": wrapped_key,
        })
    if not _valid_b64url(nonce, 12, 12):
        return None
    if not _valid_b64url(tag, 16, 16):
        return None
    if not body or len(body) > int(max_body * 4 / 3) + 8:
        return None
    if not _B64URL_RE.fullmatch(body):
        return None
    try:
        raw_size = len(_decode_b64url(body))
    except Exception:
        return None
    if raw_size > max_body:
        return None
    return {
        "kind": "forkmesh.owner-sealed",
        "v": 1,
        "alg": "x25519+mlkem768/aes256gcm",
        "nonce": nonce,
        "tag": tag,
        "body": body,
        "recipients": validated_recipients,
    }


def owner_envelope_key_id(envelope):
    """Return the first validated recipient id without exposing ciphertext.

    The sealing device writes its own wrap entry first, so this is the id of
    the device that produced the envelope.  Authorization checks against a
    required key should use owner_envelope_key_ids() membership instead.
    """
    valid = validate_owner_envelope(envelope)
    if valid is None:
        return ""
    return valid["recipients"][0]["kid"]


def owner_envelope_key_ids(envelope):
    """Return every validated recipient id, or [] for an invalid envelope."""
    valid = validate_owner_envelope(envelope)
    if valid is None:
        return []
    return [recipient["kid"] for recipient in valid["recipients"]]


def encode_owner_envelope(envelope, routing=None):
    envelope = validate_owner_envelope(envelope)
    if envelope is None:
        return ""
    wrapper = {"envelope": envelope}
    if isinstance(routing, dict):
        safe_routing = {}
        for key in ("agentId", "queuedAt"):
            if key in routing:
                safe_routing[key] = _safe_int(routing.get(key))
        if safe_routing:
            wrapper["routing"] = safe_routing
    raw = json.dumps(wrapper, sort_keys=True, separators=(",", ":")).encode()
    return "owner-sealed-v1:" + base64.urlsafe_b64encode(raw).decode().rstrip("=")


def decode_owner_envelope(value):
    value = str(value or "")
    prefix = "owner-sealed-v1:"
    if not value.startswith(prefix):
        return None
    try:
        raw = _decode_b64url(value[len(prefix):])
        wrapper = json.loads(raw.decode("utf-8"))
    except Exception:
        return None
    if not isinstance(wrapper, dict):
        return None
    envelope = validate_owner_envelope(wrapper.get("envelope"))
    if envelope is None:
        return None
    result = {"envelope": envelope}
    routing = wrapper.get("routing")
    if isinstance(routing, dict):
        result["routing"] = {
            key: _safe_int(routing.get(key))
            for key in ("agentId", "queuedAt") if key in routing
        }
    return result


def _decode_b64url(value):
    value = str(value or "")
    return base64.urlsafe_b64decode(value + "=" * ((4 - len(value) % 4) % 4))


def _valid_b64url(value, minimum, maximum):
    if not value or not _B64URL_RE.fullmatch(value):
        return False
    try:
        size = len(_decode_b64url(value))
    except Exception:
        return False
    return minimum <= size <= maximum


def _safe_int(value):
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0
