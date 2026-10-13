"""Pure, runtime-independent helpers for the Patreon integration."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import hmac
import re


MAX_BODY_BYTES = 256 * 1024
SUBSCRIPTION_GRACE_MS = 3 * 24 * 60 * 60 * 1000
PRO_TIER_CENTS = 1000
BUILDER_TIER_CENTS = 5000
STUDIO_TIER_CENTS = 10000
PRO_ROLE = "pro_member"
BUILDER_ROLE = "builder_member"
STUDIO_ROLE = "studio_member"
SUPPORTER_ROLE = "supporting_member"
SCOPE_TYPE_PLATFORM = "platform"
PATREON_EVENT_TYPES = frozenset({
    "members:pledge:create",
    "members:pledge:update",
    "members:pledge:delete",
})

_IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9_-]{1,128}$")
_SIGNATURE_RE = re.compile(r"^[0-9a-f]{32}$")


def _identifier(value):
    value = str(value or "").strip()
    return value if _IDENTIFIER_RE.fullmatch(value) else ""


def _mapping(value):
    return value if isinstance(value, dict) else {}


def _resource_id(value, resource_type):
    resource = _mapping(value)
    if str(resource.get("type") or "") != resource_type:
        return ""
    return _identifier(resource.get("id"))


def _relationship_id(resource, relationship, resource_type):
    relationships = _mapping(_mapping(resource).get("relationships"))
    data = _mapping(_mapping(relationships.get(relationship)).get("data"))
    return _resource_id(data, resource_type)


def _amount_cents(value):
    if isinstance(value, bool):
        return None
    try:
        amount = int(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return amount if 0 <= amount <= 100_000_000 else None


def _limited_text(value, limit=256):
    value = str(value or "").strip()
    return value if len(value) <= limit else ""


def verify_patreon_signature(secret, body, presented):
    """Verify Patreon's legacy MD5 HMAC without timing-leaky comparison."""
    secret = str(secret or "")
    body = str(body or "")
    presented = str(presented or "").strip().lower()
    if not secret or not _SIGNATURE_RE.fullmatch(presented):
        return False
    expected = hmac.new(
        secret.encode("utf-8"), body.encode("utf-8"), hashlib.md5
    ).hexdigest()
    return hmac.compare_digest(expected, presented)


def _normalize_expires_at_iso(value):
    """Return an ISO-8601 timestamp as epoch milliseconds, or zero."""
    value = _limited_text(value, 64)
    if not value:
        return 0
    try:
        parsed = datetime.fromisoformat(
            value[:-1] + "+00:00" if value.endswith("Z") else value
        )
    except (TypeError, ValueError, OverflowError):
        return 0
    if parsed.tzinfo is None:
        return 0
    try:
        return max(
            0,
            int(parsed.astimezone(timezone.utc).timestamp() * 1000),
        )
    except (ValueError, OverflowError, OSError):
        return 0


def _tier_from_amount_cents(amount_cents):
    amount = _amount_cents(amount_cents)
    if not amount:
        return "free"
    if amount >= STUDIO_TIER_CENTS:
        return "studio"
    if amount >= BUILDER_TIER_CENTS:
        return "builder"
    if amount >= PRO_TIER_CENTS:
        return "pro"
    return "supporter"


def _roles_for_tier(tier):
    """Return inherited platform roles for a Patreon campaign tier."""
    hierarchy = {
        "supporter": (SUPPORTER_ROLE,),
        "pro": (PRO_ROLE, SUPPORTER_ROLE),
        "builder": (BUILDER_ROLE, PRO_ROLE, SUPPORTER_ROLE),
        "studio": (
            STUDIO_ROLE, BUILDER_ROLE, PRO_ROLE, SUPPORTER_ROLE,
        ),
    }
    return hierarchy.get(str(tier or "").lower(), ())


def parse_oauth_identity(payload, campaign_id):
    """Extract the authenticated user and its requested campaign membership."""
    campaign_id = _identifier(campaign_id)
    root = _mapping(payload)
    user = _mapping(root.get("data"))
    user_id = _resource_id(user, "user")
    if not campaign_id or not user_id:
        return None

    # JSON:API to-many relationship data is a list, not an object.
    raw_memberships = _mapping(user.get("relationships")).get("memberships")
    relationship_data = _mapping(raw_memberships).get("data")
    if not isinstance(relationship_data, list):
        return None
    related_ids = {
        member_id
        for member_id in (
            _resource_id(item, "member") for item in relationship_data
        )
        if member_id
    }
    if not related_ids:
        return None

    included = root.get("included")
    if not isinstance(included, list):
        return None
    for candidate in included:
        member = _mapping(candidate)
        member_id = _resource_id(member, "member")
        if member_id not in related_ids:
            continue
        if _relationship_id(member, "campaign", "campaign") != campaign_id:
            continue
        attrs = _mapping(member.get("attributes"))
        amount = _amount_cents(attrs.get("currently_entitled_amount_cents"))
        status = _limited_text(attrs.get("patron_status"), 64)
        next_charge = _limited_text(attrs.get("next_charge_date"), 64)
        if amount is None or not status:
            return None
        return {
            "patreon_user_id": user_id,
            "amount_cents": amount,
            "patron_status": status,
            "next_charge_at": next_charge,
        }
    return None


def parse_member_snapshot(payload, expected_member_id, campaign_id):
    """Extract the authoritative membership projection fetched by fixed ID."""
    expected_member_id = _identifier(expected_member_id)
    campaign_id = _identifier(campaign_id)
    member = _mapping(_mapping(payload).get("data"))
    if (
        not expected_member_id
        or not campaign_id
        or _resource_id(member, "member") != expected_member_id
        or _relationship_id(member, "campaign", "campaign") != campaign_id
    ):
        return None
    user_id = _relationship_id(member, "user", "user")
    attrs = _mapping(member.get("attributes"))
    amount = _amount_cents(attrs.get("currently_entitled_amount_cents"))
    status = _limited_text(attrs.get("patron_status"), 64)
    next_charge = _limited_text(attrs.get("next_charge_date"), 64)
    email = _limited_text(attrs.get("email"), 320).lower()
    if amount is None or not status:
        return None
    return {
        "patreon_user_id": user_id,
        "email": email,
        "amount_cents": amount,
        "patron_status": status,
        "next_charge_at": next_charge,
    }


def _extract_patreon_event(payload, event_type):
    """Parse only the signed notification fields needed to reconcile by ID."""
    event_type = str(event_type or "").strip().lower()
    if event_type not in PATREON_EVENT_TYPES:
        return None
    member = _mapping(_mapping(payload).get("data"))
    if str(member.get("type") or "") != "member":
        return None
    return {
        "event": event_type,
        "pledge_id": _identifier(member.get("id")),
    }
