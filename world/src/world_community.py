"""Validation and projection helpers for world community services.

The live Worker keeps persistence, authentication, and audit writes in
``entry.py``.  This module owns the data boundary shared by the D1-backed
Fediverse directory and media rooms:

* directory records contain public instance metadata only;
* relationship-shaped data is accepted only with explicit public + consent
  flags;
* every URL is HTTPS, credential-free, fragment-free, and publicly routable;
* shared-media entries are links to supported provider pages, never copied
  media or an autoplay instruction; and
* all lists and text fields are bounded before they reach D1 or a response.

There are deliberately no Worker-runtime imports here, which makes the
privacy and authorization-independent validation rules directly testable with
normal CPython.
"""

import ipaddress
import json
import re
from urllib.parse import parse_qsl, urlencode, urlparse


FEDIVERSE_KINDS = frozenset({"mastodon", "lemmy", "x", "reddit"})
FEDIVERSE_REGISTRATION = frozenset({
    "open", "approval-required", "closed", "unknown",
})
FEDIVERSE_ACTIVITY = frozenset({"low", "moderate", "high", "unknown"})
FEDIVERSE_MODERATION = frozenset({
    "moderated", "community-moderated", "unmoderated", "unknown",
})
FEDIVERSE_CONSENT_SOURCES = frozenset({
    "instance-operator", "public-api", "user-approved", "moderator-reviewed",
})

MEDIA_SESSION_TYPES = frozenset({
    "listening-room",
    "dj-session",
    "video-room",
    "watch-party",
    "repository-launch",
    "organization-presentation",
})
MEDIA_ROLES = frozenset({"owner", "moderator"})
MEDIA_ITEM_STATUSES = frozenset({"queued", "stopped", "removed"})
MEDIA_PLAYBACK_STATES = frozenset({"playing", "paused", "stopped"})
MEDIA_SCHEDULE_STATUSES = frozenset({
    "scheduled", "cancelled", "completed",
})

MAX_DIRECTORY_RECORDS = 200
MAX_MEDIA_SPACES_PER_OWNER = 5
MAX_MEDIA_SPACES_RESPONSE = 50
MAX_MEDIA_ITEMS_PER_SPACE = 100
MAX_MEDIA_ITEMS_RESPONSE = 100
MAX_MEDIA_SCHEDULES_PER_SPACE = 50
MAX_MEDIA_SCHEDULES_RESPONSE = 50
MAX_MEDIA_MODERATORS_PER_SPACE = 20
MEDIA_MAX_POSITION_MS = 7 * 24 * 60 * 60 * 1000

MEDIA_REMOVED_RETENTION_MS = 30 * 24 * 60 * 60 * 1000
MEDIA_SCHEDULE_RETENTION_MS = 30 * 24 * 60 * 60 * 1000
MEDIA_ARCHIVED_RETENTION_MS = 90 * 24 * 60 * 60 * 1000
MEDIA_ACTIVE_RETENTION_MS = 180 * 24 * 60 * 60 * 1000
MEDIA_MAX_SCHEDULE_HORIZON_MS = 365 * 24 * 60 * 60 * 1000
MEDIA_MAX_SCHEDULE_DURATION_MS = 24 * 60 * 60 * 1000

WORLD_COMMUNITY_BODY_MAX_BYTES = 32 * 1024

_ID_RE = re.compile(r"^[a-f0-9]{32}$")
_HOST_LABEL_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")
_LANGUAGE_RE = re.compile(r"^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$")
_PEERTUBE_PATH_RE = re.compile(
    r"^/(?:w/|videos/(?:watch|embed)/)[A-Za-z0-9_-]{4,128}/?$")
_MEDIA_FILE_RE = re.compile(
    r"\.(?:aac|flac|m3u8?|mp3|mp4|ogg|ogv|opus|wav|webm)(?:$|[?#])",
    re.IGNORECASE,
)

_NON_PUBLIC_HOST_SUFFIXES = (
    ".internal",
    ".invalid",
    ".local",
    ".localhost",
    ".test",
    ".example",
    ".onion",
)

_FEDIVERSE_ALLOWED_KEYS = frozenset({
    "kind",
    "name",
    "url",
    "icon",
    "description",
    "languages",
    "topics",
    "registration",
    "publicActivity",
    "relationships",
    "communities",
    "moderation",
    "consentedFollowers",
    "consentedProfiles",
    "approvedSubscriptions",
    "publicOnly",
    "consentConfirmed",
    "consentSource",
    "consentCheckedAt",
})

_SENSITIVE_INPUT_KEYS = frozenset({
    "account",
    "accounts",
    "authorization",
    "browsinghistory",
    "cookie",
    "credentials",
    "email",
    "emails",
    "exactactivity",
    "followers",
    "following",
    "formcontents",
    "ip",
    "ipaddress",
    "password",
    "privateaccount",
    "privateaccounts",
    "privatefollowers",
    "rawip",
    "searchterms",
    "secret",
    "subscriptions",
    "token",
    "useragent",
})

_MEDIA_PROVIDER_HOSTS = {
    "somafm": ("somafm.com",),
    "youtube": ("youtube.com", "youtu.be", "youtube-nocookie.com"),
    "vimeo": ("vimeo.com",),
    "soundcloud": ("soundcloud.com",),
    "twitch": ("twitch.tv",),
    "internet-archive": ("archive.org",),
}

_MEDIA_QUERY_KEYS = {
    "somafm": frozenset(),
    "youtube": frozenset({"v", "list", "index", "t", "start"}),
    "vimeo": frozenset({"h"}),
    "soundcloud": frozenset(),
    "twitch": frozenset({"video", "channel", "collection", "filter", "sort", "time"}),
    "internet-archive": frozenset(),
    "peertube": frozenset({"start"}),
}


def valid_resource_id(value):
    """Whether ``value`` is a server-created opaque world resource id."""
    return bool(_ID_RE.fullmatch(str(value or "").strip().lower()))


def _clean_text(value, limit):
    raw = str(value or "")
    clean = "".join(
        ch if ch.isprintable() and ch not in "<>" else " " for ch in raw
    )
    return " ".join(clean.split())[:limit].strip()


def _clean_choice(value, allowed, fallback=""):
    choice = str(value or "").strip().lower()
    return choice if choice in allowed else fallback


def _safe_int(value, fallback=0):
    if isinstance(value, bool):
        return fallback
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def _public_hostname(hostname):
    host = str(hostname or "").strip().lower().rstrip(".")
    if (
        not host
        or len(host) > 253
        or "." not in host
        or host == "localhost"
        or any(host.endswith(suffix) for suffix in _NON_PUBLIC_HOST_SUFFIXES)
    ):
        return ""
    try:
        ipaddress.ip_address(host)
        return ""
    except ValueError:
        pass
    labels = host.split(".")
    if any(not _HOST_LABEL_RE.fullmatch(label) for label in labels):
        return ""
    return host


def _host_matches(host, suffix):
    host = str(host or "").lower()
    suffix = str(suffix or "").lower()
    return host == suffix or host.endswith("." + suffix)


def normalize_https_url(
        value, *, base=False, same_host="", query_keys=None, max_length=1200):
    """Return a canonical public HTTPS URL, or ``""``.

    IP literals, local/internal names, credentials, fragments, non-default
    ports, and unbounded query strings are rejected.  ``query_keys`` is an
    explicit allowlist; passing ``None`` rejects every query parameter.
    """
    raw = str(value or "").strip()
    if not raw or len(raw) > max_length or "\\" in raw:
        return ""
    try:
        parsed = urlparse(raw)
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if (
        parsed.scheme.lower() != "https"
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
        or port not in (None, 443)
    ):
        return ""
    host = _public_hostname(parsed.hostname)
    if not host or (same_host and host != str(same_host).lower()):
        return ""
    path = parsed.path or "/"
    if len(path) > 600 or any(ord(ch) < 32 for ch in path):
        return ""
    if base and path != "/":
        return ""
    if base and (parsed.params or parsed.query):
        return ""
    if parsed.params:
        return ""
    try:
        pairs = parse_qsl(
            parsed.query, keep_blank_values=True, strict_parsing=False,
        )
    except ValueError:
        return ""
    if len(pairs) > 8:
        return ""
    allowed = frozenset(query_keys or ())
    if any(key not in allowed or len(value) > 160 for key, value in pairs):
        return ""
    query = urlencode(pairs) if pairs else ""
    netloc = host
    # Port 443 is canonicalized away.
    return parsed._replace(
        scheme="https",
        netloc=netloc,
        path=path,
        params="",
        query=query,
        fragment="",
    ).geturl()


def _string_list(value, count, length, pattern=None):
    if value is None:
        return []
    if not isinstance(value, list) or len(value) > count:
        return None
    output = []
    seen = set()
    for item in value:
        if not isinstance(item, str):
            return None
        clean = _clean_text(item, length)
        if not clean or (pattern is not None and not pattern.fullmatch(clean)):
            return None
        marker = clean.lower()
        if marker not in seen:
            seen.add(marker)
            output.append(clean)
    return output


def _public_named_list(value, count=8):
    """Normalize public community/instance names.

    Object entries must explicitly carry ``public: true``. Plain strings rely
    on the record-level public-only attestation and remain supported for public
    NodeInfo-style metadata.
    """
    if value is None:
        return []
    if not isinstance(value, list) or len(value) > count:
        return None
    output = []
    seen = set()
    for item in value:
        if isinstance(item, dict):
            if item.get("public") is not True:
                return None
            raw = item.get("name")
        elif isinstance(item, str):
            raw = item
        else:
            return None
        clean = _clean_text(raw, 60)
        if not clean:
            return None
        marker = clean.lower()
        if marker not in seen:
            seen.add(marker)
            output.append(clean)
    return output


def _valid_consent_timestamp(value, now):
    checked_at = _safe_int(value)
    now = _safe_int(now)
    return (
        checked_at
        if (
            checked_at > 0
            and now > 0
            and checked_at <= now + 5 * 60 * 1000
            and checked_at >= now - 365 * 24 * 60 * 60 * 1000
        )
        else 0
    )


def _consented_public_profiles(
        value, *, now, default_source, default_checked_at, platform=""):
    """Normalize explicit public-profile attestations without secrets.

    The evidence object records only who made the attestation and when. It is
    not proof of OAuth ownership and deliberately contains no access token,
    remote account id, email address, or fetched private relationship data.
    """
    if value is None:
        return []
    if not isinstance(value, list) or len(value) > 12:
        return None
    output = []
    seen = set()
    for item in value:
        if (
            not isinstance(item, dict)
            or item.get("public") is not True
            or item.get("consent") is not True
            or str(item.get("accountVisibility") or "public").lower()
            != "public"
        ):
            return None
        handle = _clean_text(item.get("handle"), 80)
        if not handle or handle.lower() in seen:
            return None
        avatar = ""
        if item.get("avatar"):
            avatar = normalize_https_url(
                item.get("avatar"), query_keys=(), max_length=800)
            if not avatar:
                return None
        profile_url = ""
        if item.get("profileUrl"):
            profile_url = normalize_https_url(
                item.get("profileUrl"), query_keys=(), max_length=800)
            if not profile_url:
                return None
        if platform:
            if not profile_url:
                return None
            profile_host = urlparse(profile_url).hostname or ""
            allowed_hosts = {
                "x": ("x.com", "twitter.com"),
                "reddit": ("reddit.com",),
            }.get(platform, ())
            if not any(_host_matches(profile_host, host)
                       for host in allowed_hosts):
                return None
        evidence_source = _clean_choice(
            item.get("consentSource") or default_source,
            FEDIVERSE_CONSENT_SOURCES,
        )
        evidence_checked_at = _valid_consent_timestamp(
            item.get("consentCheckedAt") or default_checked_at, now)
        if not evidence_source or not evidence_checked_at:
            return None
        seen.add(handle.lower())
        output.append({
            "handle": handle,
            "avatar": avatar,
            "profileUrl": profile_url,
            "public": True,
            "consent": True,
            "consentEvidence": {
                "type": "operator-attestation",
                "source": evidence_source,
                "checkedAt": evidence_checked_at,
                "oauthVerifiedByForkMesh": False,
            },
        })
    return output


def _approved_subscriptions(value):
    if value is None:
        return []
    if not isinstance(value, list) or len(value) > 12:
        return None
    output = []
    seen = set()
    for item in value:
        if (
            not isinstance(item, dict)
            or item.get("public") is not True
            or item.get("consent") is not True
        ):
            return None
        name = _clean_text(item.get("name") or item.get("community"), 80)
        if not name or name.lower() in seen:
            return None
        seen.add(name.lower())
        output.append({
            "name": name,
            "public": True,
            "consent": True,
        })
    return output


def _contains_sensitive_key(value):
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = re.sub(r"[^a-z]", "", str(key or "").lower())
            # These two allowlisted names identify already-consented public
            # projections, not an unfiltered follower/subscription graph.
            if normalized not in (
                    "consentedfollowers", "consentedprofiles",
                    "approvedsubscriptions"):
                if normalized in _SENSITIVE_INPUT_KEYS:
                    return True
            if _contains_sensitive_key(child):
                return True
    elif isinstance(value, list):
        return any(_contains_sensitive_key(item) for item in value)
    return False


def has_public_consent_attestation(value):
    return bool(
        isinstance(value, dict)
        and value.get("publicOnly") is True
        and value.get("consentConfirmed") is True
    )


def normalize_fediverse_record(value, now):
    """Return ``(record, error)`` for an admin directory mutation."""
    if not isinstance(value, dict):
        return None, "invalid_record"
    if _contains_sensitive_key(value):
        return None, "sensitive_fields_not_allowed"
    if not has_public_consent_attestation(value):
        return None, "public_consent_attestation_required"
    if any(key not in _FEDIVERSE_ALLOWED_KEYS for key in value):
        return None, "unsupported_field"

    kind = _clean_choice(value.get("kind"), FEDIVERSE_KINDS)
    name = _clean_text(value.get("name"), 80)
    url = normalize_https_url(value.get("url"), base=True, max_length=500)
    if not kind:
        return None, "invalid_kind"
    if not name:
        return None, "invalid_name"
    if not url:
        return None, "invalid_public_https_url"
    host = urlparse(url).hostname or ""

    icon = ""
    if value.get("icon"):
        icon = normalize_https_url(
            value.get("icon"),
            same_host=host,
            query_keys=(),
            max_length=800,
        )
        if not icon:
            return None, "invalid_icon_url"
    description = _clean_text(value.get("description"), 280)
    consent_source = _clean_choice(
        value.get("consentSource"), FEDIVERSE_CONSENT_SOURCES)
    checked_at = _valid_consent_timestamp(value.get("consentCheckedAt"), now)
    if not consent_source:
        return None, "invalid_consent_source"
    if not checked_at:
        return None, "invalid_consent_timestamp"
    languages = _string_list(
        value.get("languages"), 8, 12, pattern=_LANGUAGE_RE)
    topics = _string_list(value.get("topics"), 12, 40)
    relationships = _public_named_list(value.get("relationships"), 8)
    communities = _public_named_list(value.get("communities"), 12)
    followers = _consented_public_profiles(
        value.get("consentedFollowers"),
        now=now,
        default_source=consent_source,
        default_checked_at=checked_at,
    )
    profiles = _consented_public_profiles(
        value.get("consentedProfiles"),
        now=now,
        default_source=consent_source,
        default_checked_at=checked_at,
        platform=kind if kind in ("x", "reddit") else "",
    )
    subscriptions = _approved_subscriptions(
        value.get("approvedSubscriptions"))
    if languages is None:
        return None, "invalid_languages"
    if topics is None:
        return None, "invalid_topics"
    if relationships is None:
        return None, "invalid_public_relationships"
    if communities is None:
        return None, "invalid_public_communities"
    if followers is None:
        return None, "invalid_consented_followers"
    if profiles is None:
        return None, "invalid_consented_profiles"
    if subscriptions is None:
        return None, "invalid_approved_subscriptions"
    if kind == "mastodon" and (communities or subscriptions or profiles):
        return None, "lemmy_fields_not_allowed"
    if kind == "lemmy" and (followers or profiles):
        return None, "mastodon_fields_not_allowed"
    if kind in ("x", "reddit") and (
            followers or communities or subscriptions):
        return None, "fediverse_fields_not_allowed"
    if kind not in ("x", "reddit") and profiles:
        return None, "social_profile_fields_not_allowed"
    if kind in ("x", "reddit") and not profiles:
        return None, "consented_profiles_required"
    if kind == "x" and not any(
            _host_matches(host, allowed) for allowed in ("x.com", "twitter.com")):
        return None, "invalid_platform_url"
    if kind == "reddit" and not _host_matches(host, "reddit.com"):
        return None, "invalid_platform_url"

    registration = _clean_choice(
        value.get("registration"), FEDIVERSE_REGISTRATION, "unknown")
    activity = _clean_choice(
        value.get("publicActivity"), FEDIVERSE_ACTIVITY, "unknown")
    moderation = _clean_choice(
        value.get("moderation"), FEDIVERSE_MODERATION, "unknown")
    return {
        "kind": kind,
        "name": name,
        "url": url,
        "host": host,
        "icon": icon,
        "description": description,
        "languages": languages,
        "topics": topics,
        "registration": registration,
        "publicActivity": activity,
        "relationships": relationships,
        "communities": communities if kind == "lemmy" else [],
        "moderation": moderation if kind == "lemmy" else "",
        "consentedFollowers": followers if kind == "mastodon" else [],
        "consentedProfiles": profiles if kind in ("x", "reddit") else [],
        "approvedSubscriptions": subscriptions if kind == "lemmy" else [],
        "publicOnly": True,
        "consentConfirmed": True,
        "consentSource": consent_source,
        "consentCheckedAt": checked_at,
    }, ""


def _json_object(value):
    if isinstance(value, dict):
        return value
    try:
        parsed = json.loads(str(value or ""))
    except (TypeError, ValueError):
        return {}
    return parsed if isinstance(parsed, dict) else {}


def fediverse_public_record(row, now=0):
    """Project one D1 row to its complete public-only directory shape."""
    row = row if isinstance(row, dict) else {}
    data = _json_object(row.get("data"))
    if (
        data.get("publicOnly") is not True
        or data.get("consentConfirmed") is not True
    ):
        return None
    data = dict(data)
    # Early migration builds duplicated the already-indexed host inside the
    # public JSON blob. It is derived again from `url`, never trusted.
    data.pop("host", None)
    validation_now = max(
        1,
        _safe_int(now),
        _safe_int(data.get("consentCheckedAt")),
        _safe_int(row.get("updated_at")),
    )
    normalized, error = normalize_fediverse_record(data, validation_now)
    if error:
        return None
    data = dict(normalized)
    data.pop("host", None)
    data["id"] = str(row.get("instance_id") or "")[:32]
    data["updatedAt"] = _safe_int(row.get("updated_at"))
    return data


def fediverse_directory_payload(rows, now):
    payload = {
        "schemaVersion": 3,
        "updatedAt": None,
        "mastodon": [],
        "lemmy": [],
        "x": [],
        "reddit": [],
        "privacy": {
            "publicInstanceMetadataOnly": True,
            "publicDirectoryMetadataOnly": True,
            "privateFollowersExcluded": True,
            "privateAccountsExcluded": True,
            "connectionsRequireUserConsent": True,
            "operatorAttestationsAreNotOAuthVerification": True,
        },
    }
    updated = 0
    for row in list(rows or [])[:MAX_DIRECTORY_RECORDS]:
        record = fediverse_public_record(row, now)
        if record is None:
            continue
        updated = max(updated, _safe_int(record.get("updatedAt")))
        payload[record["kind"]].append(record)
    for kind in FEDIVERSE_KINDS:
        payload[kind].sort(
            key=lambda item: (
                str(item.get("name") or "").lower(),
                str(item.get("url") or ""),
            )
        )
    payload["updatedAt"] = updated or _safe_int(now)
    return payload


def normalize_media_space(value):
    if not isinstance(value, dict):
        return None, "invalid_space"
    name = _clean_text(value.get("name"), 80)
    description = _clean_text(value.get("description"), 300)
    session_type = _clean_choice(
        value.get("sessionType"), MEDIA_SESSION_TYPES, "listening-room")
    if not name:
        return None, "invalid_name"
    return {
        "name": name,
        "description": description,
        "sessionType": session_type,
    }, ""


def detect_media_provider(value):
    """Return the supported provider id inferred from an HTTPS page URL."""
    raw = str(value or "").strip()
    try:
        host = _public_hostname(urlparse(raw).hostname)
    except (TypeError, ValueError):
        return ""
    if not host:
        return ""
    for provider, suffixes in _MEDIA_PROVIDER_HOSTS.items():
        if any(_host_matches(host, suffix) for suffix in suffixes):
            return provider
    try:
        path = urlparse(raw).path or "/"
    except (TypeError, ValueError):
        path = "/"
    return "peertube" if _PEERTUBE_PATH_RE.fullmatch(path) else ""


def normalize_media_provider_url(value, provider=""):
    raw = str(value or "").strip()
    if not raw or len(raw) > 1200 or _MEDIA_FILE_RE.search(raw):
        return "", ""
    provider = str(provider or "").strip().lower().replace("_", "-")
    inferred = detect_media_provider(raw)
    if not provider:
        provider = inferred
    if provider not in set(_MEDIA_PROVIDER_HOSTS) | {"peertube"}:
        return "", ""
    if inferred != provider:
        return "", ""
    url = normalize_https_url(
        raw,
        query_keys=_MEDIA_QUERY_KEYS.get(provider, frozenset()),
        max_length=1200,
    )
    if not url:
        return "", ""
    if provider == "peertube" and not _PEERTUBE_PATH_RE.fullmatch(
            urlparse(url).path or "/"):
        return "", ""
    return provider, url


def normalize_media_item(value):
    if not isinstance(value, dict):
        return None, "invalid_item"
    if value.get("termsConfirmed") is not True:
        return None, "provider_terms_confirmation_required"
    if value.get("noRebroadcast") is not True:
        return None, "no_rebroadcast_confirmation_required"
    if value.get("autoplay") not in (None, False):
        return None, "autoplay_not_allowed"
    title = _clean_text(value.get("title"), 100)
    provider, url = normalize_media_provider_url(
        value.get("url"), value.get("provider"))
    if not title:
        return None, "invalid_title"
    if not provider or not url:
        return None, "invalid_provider_url"
    return {
        "title": title,
        "provider": provider,
        "url": url,
        "autoplay": False,
        "externalPlaybackOnly": True,
        "noRebroadcast": True,
        # ForkMesh has not received a provider-authorized metadata event.
        # A user-supplied playlist label must never be presented as the
        # provider's current track.
        "providerMetadata": {
            "status": "unavailable",
            "reason": "not_received",
        },
    }, ""


def normalize_media_schedule(value, now):
    if not isinstance(value, dict):
        return None, "invalid_schedule"
    now = _safe_int(now)
    starts_at = _safe_int(value.get("startsAt"))
    ends_at = _safe_int(value.get("endsAt"))
    title = _clean_text(value.get("title"), 100)
    session_type = _clean_choice(
        value.get("sessionType"), MEDIA_SESSION_TYPES, "listening-room")
    item_id = str(value.get("itemId") or "").strip().lower()
    if item_id and not valid_resource_id(item_id):
        return None, "invalid_item_id"
    if not title:
        return None, "invalid_title"
    if (
        now <= 0
        or starts_at < now - 5 * 60 * 1000
        or starts_at > now + MEDIA_MAX_SCHEDULE_HORIZON_MS
        or ends_at <= starts_at
        or ends_at - starts_at > MEDIA_MAX_SCHEDULE_DURATION_MS
    ):
        return None, "invalid_schedule_window"
    return {
        "title": title,
        "sessionType": session_type,
        "itemId": item_id,
        "startsAt": starts_at,
        "endsAt": ends_at,
    }, ""


def normalize_media_playback(value):
    """Validate an optimistic shared-playback clock mutation."""

    if not isinstance(value, dict):
        return None, "invalid_playback"
    if any(
        key not in {"state", "itemId", "positionMs", "expectedRevision"}
        for key in value
    ):
        return None, "unsupported_field"
    state = _clean_choice(value.get("state"), MEDIA_PLAYBACK_STATES)
    item_id = str(value.get("itemId") or "").strip().lower()
    position_ms = _safe_int(value.get("positionMs"), -1)
    expected_revision = _safe_int(value.get("expectedRevision"), -1)
    if not state:
        return None, "invalid_playback_state"
    if state in ("playing", "paused") and not valid_resource_id(item_id):
        return None, "invalid_item_id"
    if state == "stopped":
        item_id = ""
        position_ms = 0
    if position_ms < 0 or position_ms > MEDIA_MAX_POSITION_MS:
        return None, "invalid_playback_position"
    if expected_revision < 0:
        return None, "invalid_playback_revision"
    return {
        "state": state,
        "itemId": item_id,
        "positionMs": position_ms,
        "expectedRevision": expected_revision,
    }, ""


def media_playback_public(row, now):
    """Project the persisted base position onto the shared UTC clock."""

    row = row if isinstance(row, dict) else {}
    state = str(row.get("state") or "idle").strip().lower()
    if state not in {"idle", *MEDIA_PLAYBACK_STATES}:
        state = "idle"
    item_id = (
        str(row.get("item_id") or "")[:32]
        if valid_resource_id(row.get("item_id"))
        else ""
    )
    position_ms = max(
        0, min(_safe_int(row.get("position_ms")), MEDIA_MAX_POSITION_MS))
    started_at = max(0, _safe_int(row.get("started_at")))
    now = max(0, _safe_int(now))
    if state == "playing" and started_at > 0 and now > started_at:
        position_ms = min(
            MEDIA_MAX_POSITION_MS, position_ms + now - started_at)
    if state in ("playing", "paused") and not item_id:
        state = "stopped"
        position_ms = 0
    return {
        "state": state,
        "itemId": item_id,
        "positionMs": position_ms,
        "changedAt": max(0, _safe_int(row.get("changed_at"))),
        "startedAt": started_at if state == "playing" else 0,
        "revision": max(0, _safe_int(row.get("revision"))),
        "serverTime": now,
        "coordinationOnly": True,
        "requiresLocalPlaybackConsent": True,
    }


def media_space_public(row, viewer_role="", item_count=0, schedule_count=0):
    row = row if isinstance(row, dict) else {}
    role = _clean_choice(viewer_role, MEDIA_ROLES, "")
    return {
        "id": str(row.get("space_id") or "")[:32],
        "name": _clean_text(row.get("name"), 80),
        "description": _clean_text(row.get("description"), 300),
        "sessionType": _clean_choice(
            row.get("session_type"), MEDIA_SESSION_TYPES, "listening-room"),
        "owner": _clean_text(row.get("owner_label"), 80),
        "viewerRole": role,
        "canModerate": role in MEDIA_ROLES,
        "status": (
            "active" if str(row.get("status") or "") == "active"
            else "archived"
        ),
        "playbackState": (
            "stopped"
            if str(row.get("playback_state") or "") == "stopped" else "idle"
        ),
        "itemCount": max(0, min(_safe_int(item_count), MAX_MEDIA_ITEMS_PER_SPACE)),
        "scheduleCount": max(
            0, min(_safe_int(schedule_count), MAX_MEDIA_SCHEDULES_PER_SPACE)),
        "createdAt": _safe_int(row.get("created_at")),
        "updatedAt": _safe_int(row.get("updated_at")),
    }


def media_item_public(row):
    row = row if isinstance(row, dict) else {}
    data = _json_object(row.get("data"))
    provider, url = normalize_media_provider_url(
        data.get("url"), data.get("provider"))
    if not provider or not url:
        return None
    status = _clean_choice(
        row.get("status"), MEDIA_ITEM_STATUSES, "removed")
    if status == "removed":
        return None
    return {
        "id": str(row.get("item_id") or "")[:32],
        "title": _clean_text(data.get("title"), 100),
        "provider": provider,
        "url": url,
        "autoplay": False,
        "externalPlaybackOnly": True,
        "status": status,
        "addedAt": _safe_int(row.get("created_at")),
        "providerMetadata": {
            "status": "unavailable",
            "reason": "not_received",
        },
    }


def media_schedule_public(row):
    row = row if isinstance(row, dict) else {}
    data = _json_object(row.get("data"))
    status = _clean_choice(
        row.get("status"), MEDIA_SCHEDULE_STATUSES, "cancelled")
    if status == "cancelled":
        return None
    return {
        "id": str(row.get("schedule_id") or "")[:32],
        "title": _clean_text(data.get("title"), 100),
        "sessionType": _clean_choice(
            data.get("sessionType"), MEDIA_SESSION_TYPES, "listening-room"),
        "itemId": (
            str(data.get("itemId") or "")[:32]
            if valid_resource_id(data.get("itemId")) else ""
        ),
        "startsAt": _safe_int(row.get("starts_at")),
        "endsAt": _safe_int(row.get("ends_at")),
        "status": status,
    }


def media_role_public(row):
    row = row if isinstance(row, dict) else {}
    role = _clean_choice(row.get("role"), MEDIA_ROLES, "")
    if role != "moderator":
        return None
    return {
        "id": str(row.get("role_id") or "")[:32],
        "account": _clean_text(row.get("account_label"), 80),
        "role": "moderator",
        "grantedAt": _safe_int(row.get("created_at")),
    }


def media_detail_payload(space, viewer_role, item_rows, schedule_rows,
                         role_rows=None, playback_row=None, now=0):
    items = []
    for row in list(item_rows or [])[:MAX_MEDIA_ITEMS_RESPONSE]:
        item = media_item_public(row)
        if item is not None:
            items.append(item)
    schedules = []
    for row in list(schedule_rows or [])[:MAX_MEDIA_SCHEDULES_RESPONSE]:
        schedule = media_schedule_public(row)
        if schedule is not None:
            schedules.append(schedule)
    playback = media_playback_public(playback_row, now)
    space_payload = media_space_public(
        space, viewer_role, len(items), len(schedules))
    space_payload["playbackState"] = playback["state"]
    payload = {
        "ok": True,
        "space": space_payload,
        "items": items,
        "schedules": schedules,
        "playback": playback,
        "mediaPolicy": {
            "httpsProviderPagesOnly": True,
            "autoplay": False,
            "serverStoresMedia": False,
            "rebroadcastAuthorized": False,
            "serverAuthoritativeCoordination": True,
            "localPlaybackConsentRequired": True,
            "providerTrackMetadataRequiresPermittedReceipt": True,
            "userPlaylistTitlesAreNotProviderTrackMetadata": True,
        },
    }
    if viewer_role == "owner":
        roles = []
        for row in list(role_rows or [])[:MAX_MEDIA_MODERATORS_PER_SPACE]:
            role = media_role_public(row)
            if role is not None:
                roles.append(role)
        payload["roles"] = roles
    return payload
