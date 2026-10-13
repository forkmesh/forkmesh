"""Bounded UTC community-event policy for ForkMesh World.

Only event announcement metadata is public.  Authentication, authorization,
audit logging, and D1 orchestration live in ``world_events_api.py``.
"""

from datetime import datetime, timezone
import json
import re


WORLD_EVENT_BODY_MAX_BYTES = 16 * 1024
MAX_EVENT_RECORDS = 500
MAX_EVENTS_RESPONSE = 64
MAX_EVENT_DURATION_MS = 7 * 24 * 60 * 60 * 1000
MAX_EVENT_HORIZON_MS = 2 * 365 * 24 * 60 * 60 * 1000
EVENT_RETENTION_MS = 30 * 24 * 60 * 60 * 1000

EVENT_TYPES = frozenset({
    "community",
    "hackathon",
    "infrastructure",
    "release",
    "security",
    "workshop",
})

_RESOURCE_RE = re.compile(r"^[a-f0-9-]{16,64}$")
_UTC_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(?::\d{2}(?:\.\d{1,3})?)?Z$")
_CONTROL_RE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")


def valid_event_id(value):
    return bool(_RESOURCE_RE.fullmatch(str(value or "").strip().lower()))


def _text(value, limit):
    if not isinstance(value, str):
        return ""
    return " ".join(_CONTROL_RE.sub("", value).split())[:limit].strip()


def parse_utc_ms(value):
    """Parse one explicit UTC ISO-8601 instant, rejecting local offsets."""
    raw = str(value or "").strip()
    if not _UTC_RE.fullmatch(raw):
        return 0
    try:
        parsed = datetime.fromisoformat(raw[:-1] + "+00:00")
    except (TypeError, ValueError):
        return 0
    return int(parsed.timestamp() * 1000)


def utc_iso(value):
    try:
        instant = datetime.fromtimestamp(
            int(value) / 1000, tz=timezone.utc)
    except (TypeError, ValueError, OverflowError):
        return ""
    return instant.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def normalize_event(data, now_ms, existing=None):
    """Return a complete normalized record or a stable validation error."""
    if not isinstance(data, dict):
        return None, "invalid_json"
    allowed = {
        "sessionToken", "type", "title", "description", "destination",
        "startsAt", "endsAt",
    }
    if any(key not in allowed for key in data):
        return None, "unsupported_field"
    base = existing if isinstance(existing, dict) else {}
    merged = {**base, **data}
    event_type = _text(merged.get("type"), 32).lower()
    if event_type not in EVENT_TYPES:
        return None, "invalid_event_type"
    title = _text(merged.get("title"), 160)
    if not title:
        return None, "title_required"
    description = _text(merged.get("description"), 500)
    destination = _text(merged.get("destination"), 100)
    if not destination:
        return None, "destination_required"
    starts_at = parse_utc_ms(merged.get("startsAt"))
    ends_at = parse_utc_ms(merged.get("endsAt"))
    if not starts_at or not ends_at:
        return None, "utc_times_required"
    if ends_at <= starts_at:
        return None, "invalid_event_window"
    if ends_at - starts_at > MAX_EVENT_DURATION_MS:
        return None, "event_too_long"
    now_ms = int(now_ms)
    if ends_at <= now_ms:
        return None, "event_already_expired"
    if starts_at > now_ms + MAX_EVENT_HORIZON_MS:
        return None, "event_too_far_ahead"
    return {
        "type": event_type,
        "title": title,
        "description": description,
        "destination": destination,
        "startsAt": utc_iso(starts_at),
        "endsAt": utc_iso(ends_at),
        "startsAtMs": starts_at,
        "endsAtMs": ends_at,
    }, ""


def public_event(row):
    """Project a scheduled D1 row without its author blind index."""
    if not isinstance(row, dict):
        return None
    try:
        data = json.loads(str(row.get("data") or "{}"))
    except (TypeError, ValueError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    starts_at = int(row.get("starts_at") or 0)
    ends_at = int(row.get("ends_at") or 0)
    event_id = str(row.get("event_id") or "").strip().lower()
    if (
        not valid_event_id(event_id)
        or str(row.get("status") or "") != "scheduled"
        or not starts_at
        or not ends_at
        or ends_at <= starts_at
        or ends_at - starts_at > MAX_EVENT_DURATION_MS
    ):
        return None
    event_type = _text(data.get("type"), 32).lower()
    title = _text(data.get("title"), 160)
    destination = _text(data.get("destination"), 100)
    if event_type not in EVENT_TYPES or not title or not destination:
        return None
    return {
        "id": event_id,
        "type": event_type,
        "title": title,
        "description": _text(data.get("description"), 500),
        "destination": destination,
        "startsAt": utc_iso(starts_at),
        "endsAt": utc_iso(ends_at),
        "updatedAt": int(row.get("updated_at") or 0),
    }


def event_list_payload(rows, now_ms):
    events = []
    for row in list(rows or [])[:MAX_EVENTS_RESPONSE]:
        event = public_event(row)
        if event and parse_utc_ms(event["endsAt"]) > int(now_ms):
            events.append(event)
    return {
        "ok": True,
        "generatedAt": utc_iso(now_ms),
        "events": events,
        "timeStandard": "UTC",
    }
