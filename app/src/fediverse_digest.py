"""Pure helpers for privacy-safe, once-daily Fediverse update digests.

Repository events are deliberately reduced before they enter the queue.  A
digest carries only public titles, public URLs, an event category, and a UTC
timestamp; source code, issue drafts, private paths, form values, followers,
and request metadata have no field in this schema.

The Worker integration persists one bounded queue per repository or
organization and uses :func:`digest_due` before publishing.  This module has
no network, D1, Worker-runtime, or ActivityPub dependencies, which keeps the
cadence and deduplication policy directly testable.
"""

import hashlib
from datetime import datetime, timezone
from urllib.parse import quote, urlparse


DAY_MS = 24 * 60 * 60 * 1000
MAX_PENDING_EVENTS = 80
MAX_DIGEST_EVENTS = 12
MAX_TITLE = 180
MAX_URL = 500
ALLOWED_EVENT_TYPES = frozenset({
    "issue", "pull_request", "discussion", "release", "security",
    "mirror", "event", "documentation", "commit",
})


def _clean_text(value, limit):
    value = " ".join(str(value or "").split())
    return value[:limit]


def _public_url(value):
    value = _clean_text(value, MAX_URL)
    try:
        parsed = urlparse(value)
    except Exception:
        return ""
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        return ""
    if parsed.username or parsed.password:
        return ""
    # Query strings can contain private search/form values. Event links use a
    # stable public path and optional fragment only.
    return parsed._replace(query="", params="").geturl()


def normalize_controls(value):
    """Return the complete per-repository/organization digest policy."""
    value = value if isinstance(value, dict) else {}
    return {
        "enabled": bool(value.get("enabled", True)),
        "preview": bool(value.get("preview", False)),
        "cadence": "daily",
    }


def repository_event(
        origin, owner, repository, kind, action, reference, title, body,
        timestamp):
    """Reduce an internal repository event to a safe digest-queue event.

    Only a short public summary and stable path survive.  Query strings,
    source excerpts, attachment data, and the full event body are excluded.
    The caller remains responsible for rejecting private repositories before
    this helper is invoked.
    """
    owner = _clean_text(owner, 100).lower()
    repository = _clean_text(repository, 100)
    kind = _clean_text(kind, 32).lower()
    action = _clean_text(action, 32).replace("_", " ").lower()
    reference = _clean_text(reference, 100)
    aliases = {
        "pull": ("pull_request", "pulls"),
        "pull_request": ("pull_request", "pulls"),
        "issue": ("issue", "issues"),
        "discussion": ("discussion", "discussions"),
        "release": ("release", "releases"),
        "commit": ("commit", "commits"),
        "security": ("security", "security"),
        "mirror": ("mirror", "mirrors"),
        "documentation": ("documentation", "docs"),
        "event": ("event", "events"),
    }
    if (
        kind not in aliases
        or not owner
        or "/" in owner
        or not repository
        or "/" in repository
    ):
        return None
    event_type, route = aliases[kind]
    parsed = urlparse(str(origin or ""))
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        return None
    root = parsed._replace(path="", params="", query="", fragment="").geturl()
    path = "/%s/%s/%s" % (
        quote(owner, safe="-._~"),
        quote(repository, safe="-._~"),
        quote(route, safe="-._~"),
    )
    if reference:
        path += "/" + quote(reference, safe="-._~")
    # Only an explicit public title may describe the event. Comment/review
    # bodies can contain drafts, mentions, or security detail, so ``body`` is
    # accepted for call-site compatibility but intentionally never retained.
    del body
    summary = _clean_text(title, 120)
    label = event_type.replace("_", " ")
    event_title = (label.title() + ((" " + action) if action else "")).strip()
    if summary:
        event_title += ": " + summary
    elif reference:
        event_title += " " + reference
    return normalize_event({
        "type": event_type,
        "title": event_title,
        "url": root + path,
        "timestamp": timestamp,
        "scope": owner + "/" + repository.lower(),
    })


def normalize_event(value):
    """Reduce an arbitrary event to the only fields allowed in the queue."""
    if not isinstance(value, dict):
        return None
    event_type = _clean_text(value.get("type"), 32).lower()
    if event_type not in ALLOWED_EVENT_TYPES:
        return None
    title = _clean_text(value.get("title"), MAX_TITLE)
    url = _public_url(value.get("url"))
    if not title or not url:
        return None
    try:
        timestamp = int(value.get("timestamp") or 0)
    except (TypeError, ValueError):
        return None
    if timestamp <= 0:
        return None
    scope = _clean_text(value.get("scope"), 160).lower()
    if not scope or "/" not in scope:
        return None
    event = {
        "type": event_type,
        "title": title,
        "url": url,
        "timestamp": timestamp,
        "scope": scope,
    }
    event["id"] = event_id(event)
    return event


def event_id(event):
    canonical = "\n".join((
        str(event.get("scope") or ""),
        str(event.get("type") or ""),
        str(event.get("title") or ""),
        str(event.get("url") or ""),
    ))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:32]


def add_event(events, value):
    """Append one meaningful event, deduplicate, and keep a bounded queue."""
    event = normalize_event(value)
    current = [item for item in (events or []) if isinstance(item, dict)]
    if event is None:
        return current[-MAX_PENDING_EVENTS:]
    output = [item for item in current if item.get("id") != event["id"]]
    output.append(event)
    output.sort(key=lambda item: int(item.get("timestamp") or 0))
    return output[-MAX_PENDING_EVENTS:]


def digest_due(events, last_published_at, now):
    """Whether a non-empty queue may produce its next daily digest."""
    if not events:
        return False
    try:
        last_published_at = int(last_published_at or 0)
        now = int(now)
    except (TypeError, ValueError):
        return False
    if now <= 0:
        return False
    return last_published_at <= 0 or now - last_published_at >= DAY_MS


def _utc_label(timestamp):
    return datetime.fromtimestamp(
        int(timestamp) / 1000, tz=timezone.utc,
    ).strftime("%Y-%m-%d %H:%M UTC")


def build_digest(events, scope):
    """Build one automated, public, deduplicated digest or return ``None``."""
    normalized = []
    seen = set()
    for raw in events or []:
        event = normalize_event(raw)
        if not event or event["scope"] != scope or event["id"] in seen:
            continue
        seen.add(event["id"])
        normalized.append(event)
    normalized.sort(key=lambda item: int(item["timestamp"]))
    normalized = normalized[-MAX_DIGEST_EVENTS:]
    if not normalized:
        return None
    lines = ["ForkMesh automated daily update for " + scope + ":"]
    for event in normalized:
        lines.append(
            "• [%s] %s — %s (%s)" % (
                event["type"].replace("_", " "),
                event["title"],
                event["url"],
                _utc_label(event["timestamp"]),
            )
        )
    lines.append("Automated post · repository/organization controls apply.")
    return {
        "scope": scope,
        "eventIds": [event["id"] for event in normalized],
        "text": "\n".join(lines),
        "eventCount": len(normalized),
        "automated": True,
    }


def remove_published(events, event_ids):
    """Drop only events named by a successfully created digest."""
    published = {
        str(value) for value in (event_ids or []) if isinstance(value, str)
    }
    return [
        item for item in (events or [])
        if isinstance(item, dict) and str(item.get("id") or "") not in published
    ][-MAX_PENDING_EVENTS:]
