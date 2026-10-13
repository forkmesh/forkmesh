"""UTM capture, first/last-touch attribution, and acquisition reporting.

The measurement substrate for the acquisition experiments. A visitor's
browser holds only an opaque random session id in the ``fm_attr`` cookie;
every UTM value is parsed, sanitised and allowlisted here, server-side, and
stored against that session. First-touch is written once and never
overwritten; last-touch follows the newest tagged visit.

Loaded lazily by entry.py (``_attribution = _LazyModule("attribution")``) so
its source is not part of the entrypoint's validated startup scope. Shared
Worker primitives (``d1_run``, ``Date``, ``blind_index``, ...) arrive through
``_bind_runtime`` on first use, exactly like api_metrics.py; the test suite
binds an in-memory SQLite stand-in instead.

Two things this module deliberately does not do:

* It never touches the anonymous clone or read path. Capture runs only on a
  served top-level HTML navigation; ``git`` requests carry no browser cookie
  and are never joined to a session (see ``docs/attribution.md``).
* It never renders a number it has not fully collected. Every list read is
  cursor-paginated and consumed to exhaustion, and the collected row count is
  asserted against the server-side total before any aggregate is shown.
"""

import csv
import hashlib
import html
import io
import json
import os
import re
import time
from datetime import datetime, timedelta, timezone
from urllib.parse import parse_qs


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the attribution implementation."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# --- metric dictionary ------------------------------------------------------
# Exactly these event names exist. Adding one means adding it here first, so
# reporting, the dictionary in docs/attribution.md and the browser beacon
# allowlist cannot drift apart.
EVENT_NAMES = (
    "landing_view",
    "clone_url_copied",
    "signup_started",
    "signup_completed",
    "first_mirror_created",
    "contribution_made",
    "managed_waitlist_submitted",
    "discovery_call_accepted",
)
# The only events a browser may report itself. Everything else is emitted by
# the server at the moment the underlying thing actually happened.
BROWSER_EVENT_NAMES = frozenset({"clone_url_copied"})
# Consecutive funnel stages the dashboard reports conversion between.
FUNNEL_STAGES = (
    "sessions",
    "clone_url_copied",
    "signup_started",
    "signup_completed",
    "first_mirror_created",
    "contribution_made",
)
SIDE_FUNNEL_STAGES = ("managed_waitlist_submitted", "discovery_call_accepted")

UTM_KEYS = ("source", "medium", "campaign", "term", "content")
COOKIE_NAME = "fm_attr"
COOKIE_MAX_AGE_SECONDS = 90 * 24 * 60 * 60
RELAY_HEADER = "x-forkmesh-attribution-relay"
RELAY_COUNTRY_HEADER = "x-forkmesh-attribution-country"
PUBLIC_PAGE_FLAG = "PUBLIC_ACQUISITION_PAGE"

MAX_UTM_VALUE = 64
MAX_METADATA_BYTES = 1024
PAGE_SIZE = 100
MAX_PAGE_SIZE = 500
# A session whose only activity is one landing view, with no further request
# after this long, is treated as a bot by the bounce rule.
BOUNCE_WINDOW_MS = 2_000
# How old a session must be before the bounce rule may judge it; a human who
# is still reading the page must not be flagged mid-visit.
BOUNCE_GRACE_MS = 60 * 60 * 1000
# Repeat navigations inside this window update last_seen_at but do not add
# another landing_view row, so a quick click-through does not cost a write
# per page on D1's bounded write budget.
LANDING_VIEW_REPEAT_MS = 60 * 1000
IP_RATE_LIMIT = 30
IP_RATE_WINDOW_MS = 60 * 60 * 1000
IP_RETENTION_MS = 24 * 60 * 60 * 1000
IP_SESSION_LIST_CAP = 40
DEFAULT_RETENTION_DAYS = 400

DEFAULT_ALLOWED_SOURCES = ("reddit", "mastodon", "x", "hn", "direct", "search", "docs", "blog", "discord", "email")
DEFAULT_ALLOWED_MEDIUMS = ("social", "referral", "organic", "comparison", "email")
DEFAULT_ALLOWED_CAMPAIGNS = ("forever_launch", "search_to_clone_478", "category_comparison_509", "reddit_scout", "changelog", "start_instance", "revenue_contest", "product_update", "promotion", "repo_mirrored")
DEFAULT_BOT_UA_PATTERNS = (
    "bot", "crawler", "spider", "crawl", "slurp", "curl/", "wget/",
    "python-requests", "python-urllib", "go-http-client", "java/",
    "libwww", "httpclient", "okhttp", "headlesschrome", "phantomjs",
    "lighthouse", "pagespeed", "facebookexternalhit", "bingpreview",
    "ahrefs", "semrush", "mj12", "dotbot", "petalbot", "bytespider",
    "gptbot", "claudebot", "ccbot", "anthropic-ai", "scrapy", "scanner",
)
# EU-27 plus the EEA states (IS, LI, NO). ePrivacy consent for a first-party
# analytics cookie is unsettled and there is no consent banner yet, so these
# visitors get the page with no cookie, no session and no event (same as
# DNT). Operator-configurable through ATTRIBUTION_SKIP_COUNTRIES.
DEFAULT_SKIP_COUNTRIES = (
    "AT", "BE", "BG", "HR", "CY", "CZ", "DK", "EE", "FI", "FR", "DE", "GR",
    "HU", "IE", "IT", "LV", "LT", "LU", "MT", "NL", "PL", "PT", "RO", "SK",
    "SI", "ES", "SE", "IS", "LI", "NO",
)

SESSION_ID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
_UTM_DISALLOWED_RE = re.compile(r"[^a-z0-9_\-.]")
_CURSOR_RE = re.compile(r"^[0-9]{1,16}\.[0-9a-f-]{36}$")
_DAY_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
_EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")


class ReportIntegrityError(RuntimeError):
    """A report read fewer rows than the server said exist."""


# --- runtime shims ----------------------------------------------------------

def _now():
    date = globals().get("Date")
    if date is not None:
        try:
            return int(date.now())
        except Exception:
            pass
    return int(time.time() * 1000)


def new_id():
    """A random UUIDv4 string; cryptographically random in the Worker."""
    raw = None
    crypto = globals().get("js_crypto")
    uint8 = globals().get("Uint8Array")
    if crypto is not None and uint8 is not None:
        try:
            raw = bytes(crypto.getRandomValues(uint8.new(16)).to_py())
        except Exception:
            raw = None
    if raw is None or len(raw) != 16:
        raw = os.urandom(16)
    buf = bytearray(raw)
    buf[6] = (buf[6] & 0x0F) | 0x40
    buf[8] = (buf[8] & 0x3F) | 0x80
    text = buf.hex()
    return "%s-%s-%s-%s-%s" % (
        text[:8], text[8:12], text[12:16], text[16:20], text[20:])


def _header(request, name):
    try:
        return str(request.headers.get(name) or "")
    except Exception:
        return ""


# --- configuration ----------------------------------------------------------

def _config_list(env, name, default):
    raw = str(getattr(env, name, "") or "").strip()
    if not raw:
        return tuple(default)
    values = []
    for part in raw.replace("\n", ",").split(","):
        part = part.strip()
        if part:
            values.append(part)
    return tuple(values) if values else tuple(default)


def allowlists(env):
    return {
        "source": frozenset(
            sanitize_utm(v) for v in _config_list(
                env, "ATTRIBUTION_ALLOWED_SOURCES", DEFAULT_ALLOWED_SOURCES)
            if sanitize_utm(v)),
        "medium": frozenset(
            sanitize_utm(v) for v in _config_list(
                env, "ATTRIBUTION_ALLOWED_MEDIUMS", DEFAULT_ALLOWED_MEDIUMS)
            if sanitize_utm(v)),
        "campaign": frozenset(
            sanitize_utm(v) for v in _config_list(
                env, "ATTRIBUTION_ALLOWED_CAMPAIGNS", DEFAULT_ALLOWED_CAMPAIGNS)
            if sanitize_utm(v)),
    }


def bot_agent_patterns(env):
    return tuple(
        p.lower() for p in _config_list(
            env, "ATTRIBUTION_BOT_UA_PATTERNS", DEFAULT_BOT_UA_PATTERNS))


def skip_countries(env):
    return frozenset(
        c.strip().upper() for c in _config_list(
            env, "ATTRIBUTION_SKIP_COUNTRIES", DEFAULT_SKIP_COUNTRIES))


def retention_days(env):
    try:
        days = int(str(getattr(env, "ATTRIBUTION_RETENTION_DAYS", "") or "")
                   .strip() or DEFAULT_RETENTION_DAYS)
    except ValueError:
        days = DEFAULT_RETENTION_DAYS
    return max(1, min(3650, days))


def public_page_enabled(env):
    return str(getattr(env, PUBLIC_PAGE_FLAG, "") or "").strip().lower() in (
        "1", "true", "yes", "on")


def report_timezone(env):
    """The one fixed reporting timezone: ``UTC`` or a ``+HH:MM`` offset.

    A fixed offset needs no tz database, which the Worker runtime does not
    ship. Anything unparseable falls back to UTC rather than guessing.
    """
    raw = str(getattr(env, "ATTRIBUTION_TIMEZONE", "") or "UTC").strip()
    if raw.upper() in ("", "UTC", "Z"):
        return timezone.utc, "UTC"
    match = re.fullmatch(r"([+-])(\d{2}):?(\d{2})", raw)
    if not match:
        return timezone.utc, "UTC"
    sign = -1 if match.group(1) == "-" else 1
    hours, minutes = int(match.group(2)), int(match.group(3))
    if hours > 14 or minutes > 59:
        return timezone.utc, "UTC"
    offset = timedelta(hours=hours, minutes=minutes) * sign
    return timezone(offset), "UTC%s%02d:%02d" % (match.group(1), hours, minutes)


# --- sanitisation and allowlisting -----------------------------------------

def sanitize_utm(value):
    """Lowercase, trim, keep only ``[a-z0-9_\\-.]``, cap at 64; else None."""
    if value is None:
        return None
    text = str(value)[:1024].strip().lower()
    text = "".join(ch for ch in text if ch >= " " and ch != "\x7f")
    text = _UTM_DISALLOWED_RE.sub("", text)
    text = text[:MAX_UTM_VALUE]
    return text or None


def parse_utms(query):
    """The five UTM fields from a query string, sanitised; absent ones None."""
    try:
        params = parse_qs(str(query or "")[:4096], keep_blank_values=False)
    except Exception:
        params = {}
    utms = {}
    for key in UTM_KEYS:
        values = params.get("utm_" + key) or []
        utms[key] = sanitize_utm(values[0]) if values else None
    return utms


def bucket_utms(utms, lists):
    """Allowlist source/medium/campaign; off-list values become ``other``.

    Returns ``(bucketed, raw_other)``: the values to store on the session and
    the sanitised originals of anything bucketed, kept in event metadata for
    review so a new channel can be added to the allowlist later.
    """
    bucketed = {}
    raw_other = {}
    for key in UTM_KEYS:
        value = utms.get(key)
        allowed = lists.get(key)
        if value is not None and allowed is not None and value not in allowed:
            bucketed[key] = "other"
            raw_other[key] = value
        else:
            bucketed[key] = value
    return bucketed, raw_other


def has_utms(utms):
    return any(utms.get(key) for key in UTM_KEYS)


def bot_reason_for_agent(user_agent, patterns):
    agent = str(user_agent or "").lower()[:512]
    if not agent:
        return "no_user_agent"
    for pattern in patterns:
        if pattern and pattern in agent:
            return "user_agent"
    return ""


# --- privacy gate -----------------------------------------------------------

def tracking_declined(request, env, country=None):
    """True when no cookie, session or event may be created for this request.

    ``DNT: 1`` and ``Sec-GPC: 1`` are honoured unconditionally. Visitors from
    a configured skip country (EU/EEA by default, until a consent banner
    exists) are treated the same way.
    """
    if _header(request, "dnt").strip() == "1":
        return True
    if _header(request, "sec-gpc").strip() == "1":
        return True
    if country is None:
        country = request_country(request)
    if country and country.upper() in skip_countries(env):
        return True
    return False


def request_country(request):
    relay = _header(request, RELAY_HEADER).strip().lower()
    if relay:
        forwarded = _header(request, RELAY_COUNTRY_HEADER).strip().upper()
        if forwarded:
            return forwarded[:2]
    resolver = globals().get("world_request_country")
    if resolver is not None:
        try:
            return str(resolver(request) or "").strip().upper()[:2]
        except Exception:
            return ""
    return _header(request, "cf-ipcountry").strip().upper()[:2]


def cookie_domain(env, request):
    resolver = globals().get("_portable_session_cookie_domain")
    if resolver is None:
        return ""
    try:
        return str(resolver(env, request) or "")
    except Exception:
        return ""


def cookie_header(session_id, domain, secure=True):
    parts = [
        COOKIE_NAME + "=" + session_id,
        "Path=/",
    ]
    if domain:
        parts.append("Domain=" + domain.lstrip("."))
    parts.append("Max-Age=%d" % COOKIE_MAX_AGE_SECONDS)
    parts.append("HttpOnly")
    if secure:
        parts.append("Secure")
    parts.append("SameSite=Lax")
    return "; ".join(parts)


def clear_cookie_header(domain, secure=True):
    parts = [COOKIE_NAME + "=", "Path=/"]
    if domain:
        parts.append("Domain=" + domain.lstrip("."))
    parts.extend(["Max-Age=0", "HttpOnly"])
    if secure:
        parts.append("Secure")
    parts.append("SameSite=Lax")
    return "; ".join(parts)


def attach_set_cookie(response, value):
    """Append a Set-Cookie to any response shape this Worker produces.

    A Python ``workers.Response`` is a Pyodide FetchResponse wrapper: its
    ``.headers`` is a *copy* of the JS headers as a dict, so mutating it
    changes nothing the client sees and ``_append_set_cookie`` raises. The
    live headers are on the wrapped JS response (``js_object``). Native JS
    responses and boundary-crossed HTTPMessage shapes still go through the
    entrypoint's ``_append_set_cookie``.
    """
    if not value:
        return False
    js_object = getattr(response, "js_object", None)
    if js_object is None:
        js_object = getattr(response, "js_response", None)
    headers = getattr(js_object, "headers", None) if js_object is not None else None
    append = getattr(headers, "append", None) if headers is not None else None
    if callable(append):
        append("Set-Cookie", value)
        return True
    _append_set_cookie(response, value)
    return True


def session_id_from_request(request):
    reader = globals().get("_cookie_value")
    if reader is None:
        return ""
    value = str(reader(request, COOKIE_NAME) or "").strip().lower()
    return value if SESSION_ID_RE.match(value) else ""


def referrer_host(request, self_host=""):
    resolver = globals().get("_site_referrer_host")
    referer = _header(request, "referer")
    if resolver is None or not referer:
        return ""
    try:
        return str(resolver(referer, self_host) or "")[:253]
    except Exception:
        return ""


def _bounded_metadata(metadata):
    if not isinstance(metadata, dict):
        metadata = {}
    text = json.dumps(metadata, separators=(",", ":"), sort_keys=True)
    if len(text.encode("utf-8")) > MAX_METADATA_BYTES:
        text = json.dumps(
            {"truncated": True}, separators=(",", ":"))
    return text


def _clean_path(path):
    path = str(path or "/")[:512]
    if not path.startswith("/"):
        path = "/" + path
    return path


# --- capture ---------------------------------------------------------------

async def _filtered_hit(env, reason, now):
    day = _utc_day(now)
    await d1_run(
        env,
        "INSERT INTO attribution_filtered_hits (day, reason, hits) "
        "VALUES (?,?,1) ON CONFLICT(day, reason) DO UPDATE SET hits=hits+1",
        day, reason)


async def record_page_view(env, observation):
    """Fold one served HTML navigation into attribution state.

    ``observation`` carries everything already read off the request:
    ``session_id`` (validated cookie value or ""), ``path``, ``query``,
    ``referrer_host``, ``user_agent``, ``ip`` (transient, never stored) and
    ``now``. Returns ``(session_id, minted)``; ``("", False)`` means nothing
    was recorded and no cookie should be set.
    """
    now = int(observation.get("now") or _now())
    reason = bot_reason_for_agent(
        observation.get("user_agent"), bot_agent_patterns(env))
    session_id = str(observation.get("session_id") or "")
    if reason and not session_id:
        # A known crawler is counted, never given a session or a cookie: it
        # keeps no cookies, so a row per request would only burn the write
        # budget while telling reporting nothing it does not already exclude.
        await _filtered_hit(env, reason, now)
        return "", False
    utms = parse_utms(observation.get("query"))
    bucketed, raw_other = bucket_utms(utms, allowlists(env))
    tagged = has_utms(utms)
    path = _clean_path(observation.get("path"))
    row = None
    if session_id:
        row = await d1_first(
            env,
            "SELECT id, first_seen_at, last_seen_at, first_utm_source, is_bot, "
            "bot_reason FROM attribution_sessions WHERE id=?",
            session_id)
    metadata = {"path": path}
    if raw_other:
        metadata["utm_other"] = raw_other
    if row is None:
        # A valid id with no row is adopted rather than replaced: the browser
        # already holds that cookie (the www relay minted it, or the row aged
        # out), and minting another would spawn a new session per navigation.
        minted = not session_id
        if minted:
            session_id = new_id()
        # Two requests of one page load can both find no row for an adopted
        # id; the loser of that race refreshes the row instead of failing the
        # capture with a primary-key error.
        await d1_run(
            env,
            "INSERT INTO attribution_sessions (id, first_seen_at, last_seen_at, "
            "first_utm_source, first_utm_medium, first_utm_campaign, "
            "first_utm_term, first_utm_content, "
            "last_utm_source, last_utm_medium, last_utm_campaign, "
            "last_utm_term, last_utm_content, "
            "first_landing_path, first_referrer_host, is_bot, bot_reason) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(id) DO UPDATE SET last_seen_at=excluded.last_seen_at",
            session_id, now, now,
            bucketed["source"], bucketed["medium"], bucketed["campaign"],
            bucketed["term"], bucketed["content"],
            bucketed["source"], bucketed["medium"], bucketed["campaign"],
            bucketed["term"], bucketed["content"],
            path, str(observation.get("referrer_host") or "")[:253],
            1 if reason else 0, reason)
        await _insert_event(env, session_id, "landing_view", metadata, now)
        await _note_session_for_ip(env, observation.get("ip"), session_id, now)
        return session_id, minted
    updates = []
    args = []
    if tagged:
        for key in UTM_KEYS:
            updates.append("last_utm_%s=?" % key)
            args.append(bucketed[key])
        if not row.get("first_utm_source"):
            for key in UTM_KEYS:
                updates.append("first_utm_%s=?" % key)
                args.append(bucketed[key])
    last_seen = int(row.get("last_seen_at") or 0)
    if tagged or now - last_seen >= BOUNCE_WINDOW_MS:
        updates.append("last_seen_at=?")
        args.append(now)
        if int(row.get("is_bot") or 0) and row.get("bot_reason") == "bounce":
            # A further request after the window is exactly what the bounce
            # rule said never happened; lift the flag rather than keep a
            # provably wrong one.
            updates.append("is_bot=0")
            updates.append("bot_reason=''")
    if updates:
        await d1_run(
            env,
            "UPDATE attribution_sessions SET " + ", ".join(updates)
            + " WHERE id=?",
            *(args + [session_id]))
    if tagged or now - last_seen >= LANDING_VIEW_REPEAT_MS:
        await _insert_event(env, session_id, "landing_view", metadata, now)
    return session_id, False


async def _note_session_for_ip(env, ip, session_id, now):
    """Count this session against its source address for the hourly rule."""
    ip = str(ip or "").strip()
    if not ip:
        return
    digest = await blind_index(
        env, "attribution-ip-v1:%s:%s" % (_utc_day(now), ip))
    hour_bucket = now // IP_RATE_WINDOW_MS
    row = await d1_first(
        env,
        "SELECT sessions, session_ids FROM attribution_ip_hourly "
        "WHERE ip_hash=? AND hour_bucket=?",
        digest, hour_bucket)
    try:
        ids = list(json.loads(str((row or {}).get("session_ids") or "[]")))
    except (TypeError, ValueError):
        ids = []
    ids = [str(i) for i in ids if SESSION_ID_RE.match(str(i))]
    if session_id not in ids and len(ids) < IP_SESSION_LIST_CAP:
        ids.append(session_id)
    sessions = int((row or {}).get("sessions") or 0) + 1
    await d1_run(
        env,
        "INSERT INTO attribution_ip_hourly (ip_hash, hour_bucket, sessions, "
        "session_ids) VALUES (?,?,?,?) "
        "ON CONFLICT(ip_hash, hour_bucket) DO UPDATE SET "
        "sessions=excluded.sessions, session_ids=excluded.session_ids",
        digest, hour_bucket, sessions,
        json.dumps(ids, separators=(",", ":")))
    if sessions > IP_RATE_LIMIT and ids:
        marks = ",".join("?" for _ in ids)
        await d1_run(
            env,
            "UPDATE attribution_sessions SET is_bot=1, bot_reason='ip_rate' "
            "WHERE is_bot=0 AND id IN (" + marks + ")",
            *ids)


async def _insert_event(env, session_id, event_name, metadata, now):
    await d1_run(
        env,
        "INSERT INTO attribution_events (id, session_id, event_name, "
        "occurred_at, metadata) VALUES (?,?,?,?,?)",
        new_id(), session_id, event_name, now, _bounded_metadata(metadata))


async def emit(env, session_id, event_name, metadata=None, dedupe=False):
    """Record one named event against an existing session. Never raises."""
    if event_name not in EVENT_NAMES:
        raise ValueError("unknown attribution event: %r" % (event_name,))
    session_id = str(session_id or "")
    if not SESSION_ID_RE.match(session_id):
        return False
    try:
        row = await d1_first(
            env,
            "SELECT is_bot, bot_reason FROM attribution_sessions WHERE id=?",
            session_id)
        if row is None:
            return False
        if dedupe:
            existing = await d1_first(
                env,
                "SELECT 1 AS present FROM attribution_events "
                "WHERE session_id=? AND event_name=? LIMIT 1",
                session_id, event_name)
            if existing:
                return False
        now = _now()
        await _insert_event(env, session_id, event_name, metadata, now)
        if int(row.get("is_bot") or 0) and row.get("bot_reason") == "bounce":
            await d1_run(
                env,
                "UPDATE attribution_sessions SET is_bot=0, bot_reason='', "
                "last_seen_at=? WHERE id=?",
                now, session_id)
        else:
            await d1_run(
                env,
                "UPDATE attribution_sessions SET last_seen_at=? WHERE id=?",
                now, session_id)
        return True
    except Exception:
        return False


async def emit_from_request(env, request, event_name, metadata=None,
                            dedupe=False):
    """Emit against the session named by the request's cookie, if any."""
    session_id = session_id_from_request(request)
    if not session_id:
        return False
    return await emit(env, session_id, event_name, metadata, dedupe)


async def emit_for_user(env, user_bi, event_name, metadata=None):
    """Emit against the newest session linked to an account at signup.

    Used for the things a signed-in person does from a desktop node rather
    than a browser (publishing a first mirror, submitting a pull), which
    carry no cookie. No linked session means the signup happened before
    attribution existed or with cookies blocked; nothing is invented.
    """
    user_bi = str(user_bi or "")
    if not user_bi:
        return False
    try:
        row = await d1_first(
            env,
            "SELECT id FROM attribution_sessions WHERE user_id=? "
            "ORDER BY first_seen_at DESC, id DESC LIMIT 1",
            user_bi)
    except Exception:
        return False
    if not row:
        return False
    return await emit(env, row.get("id"), event_name, metadata)


async def link_user(env, request, user_bi, landing_path="/signup"):
    """Attach a completed signup to its browser session.

    Without a session (cookies blocked, DNT, skipped country) the account is
    still recorded, under ``first_utm_source='unknown'``, so denominators
    stay honest. Returns the session id used.
    """
    user_bi = str(user_bi or "")
    if not user_bi:
        return ""
    now = _now()
    session_id = session_id_from_request(request)
    row = None
    if session_id:
        row = await d1_first(
            env, "SELECT id, user_id FROM attribution_sessions WHERE id=?",
            session_id)
    if row is None:
        session_id = new_id()
        await d1_run(
            env,
            "INSERT INTO attribution_sessions (id, first_seen_at, last_seen_at, "
            "first_utm_source, last_utm_source, first_landing_path, user_id) "
            "VALUES (?,?,?,?,?,?,?)",
            session_id, now, now, "unknown", "unknown",
            _clean_path(landing_path), user_bi)
        return session_id
    if not row.get("user_id"):
        await d1_run(
            env,
            "UPDATE attribution_sessions SET user_id=?, last_seen_at=? "
            "WHERE id=?",
            user_bi, now, session_id)
    return session_id


async def delete_session(env, session_id):
    session_id = str(session_id or "")
    if not SESSION_ID_RE.match(session_id):
        return False
    await d1_run(
        env, "DELETE FROM attribution_events WHERE session_id=?", session_id)
    await d1_run(
        env, "DELETE FROM attribution_sessions WHERE id=?", session_id)
    return True


# --- request handlers --------------------------------------------------------

def _observation_from_request(request, path, query, now):
    return {
        "session_id": session_id_from_request(request),
        "path": path,
        "query": query,
        "referrer_host": referrer_host(request),
        "user_agent": _header(request, "user-agent")[:512],
        "ip": _transient_client_address(request),
        "now": now,
    }


async def capture_page_view(env, request, url, response, status):
    """Middleware for HTML navigations served by this Worker.

    Runs after the response exists so it can only ever add a Set-Cookie; a
    failure here must never change what the visitor gets. Only a served,
    top-level ``text/html`` GET qualifies, which is what keeps every git
    smart-HTTP, raw-blob and API request out of attribution by construction.
    """
    try:
        if int(status or 0) != 200 or method_name(request) != "GET":
            return response
        if not _is_page_navigation(request):
            return response
        content_type = str(response.headers.get("content-type") or "").lower()
        if "text/html" not in content_type:
            return response
        if tracking_declined(request, env):
            return response
        await ensure_schema(env)
        now = _now()
        session_id, minted = await record_page_view(
            env, _observation_from_request(
                request, getattr(url, "path", "/"),
                getattr(url, "query", ""), now))
        if minted and session_id:
            secure = str(getattr(url, "scheme", "https")) == "https"
            attach_set_cookie(
                response,
                cookie_header(session_id, cookie_domain(env, request), secure))
    except Exception:
        pass
    return response


async def relay_capture_handler(env, request):
    """``POST /api/attribution/capture`` — the www Worker's page views.

    The marketing origin serves its HTML from static assets and forwards one
    observation per navigation here, with the visitor's own headers, inside
    ``waitUntil`` so the page never waits on this write. It is trusted exactly
    as much as a page view: the only thing a direct caller can do is create a
    session for its own address with sanitised, allowlisted values, which is
    what visiting the site with a tagged link does anyway.
    """
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    if not _is_page_navigation(request):
        return json_response({"ok": True, "recorded": False},
                             cache_control="no-store")
    if tracking_declined(request, env):
        return json_response({"ok": True, "recorded": False},
                             cache_control="no-store")
    provided = str(data.get("sessionId") or "").strip().lower()
    if not SESSION_ID_RE.match(provided):
        return json_response({"error": "invalid_session"}, status=400)
    await ensure_schema(env)
    now = _now()
    observation = {
        "session_id": provided,
        "path": data.get("path"),
        "query": str(data.get("search") or "").lstrip("?"),
        "referrer_host": referrer_host(request, str(data.get("host") or "")),
        "user_agent": _header(request, "user-agent")[:512],
        "ip": _transient_client_address(request),
        "now": now,
    }
    # The relay already set the cookie for a freshly minted id, so a missing
    # row is simply adopted under that id by record_page_view.
    session_id, _ = await record_page_view(env, observation)
    return json_response(
        {"ok": True, "recorded": bool(session_id), "sessionId": session_id},
        cache_control="no-store")


async def browser_event_handler(env, request):
    """``POST /api/attribution/events`` — the copy-clone-URL beacon."""
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not _request_origin_allowed(env, request):
        return json_response({"error": "origin_not_allowed"}, status=403)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    event_name = str(data.get("event") or "").strip()
    if event_name not in BROWSER_EVENT_NAMES:
        return json_response({"error": "unknown_event"}, status=400)
    session_id = session_id_from_request(request)
    if not session_id or tracking_declined(request, env):
        return json_response({"ok": True, "recorded": False}, status=202,
                             cache_control="no-store")
    metadata = {}
    repo = data.get("repo")
    if isinstance(repo, str):
        repo = re.sub(r"[^A-Za-z0-9._/-]", "", repo)[:200]
        if repo:
            metadata["repo"] = repo
    await ensure_schema(env)
    recorded = await emit(env, session_id, event_name, metadata)
    return json_response({"ok": True, "recorded": recorded},
                         cache_control="no-store")


async def delete_session_handler(env, request, url):
    """``DELETE /api/attribution/session`` — self-serve erasure."""
    if method_name(request) not in ("DELETE", "POST"):
        return json_response({"error": "method_not_allowed"}, status=405)
    if not _request_origin_allowed(env, request):
        return json_response({"error": "origin_not_allowed"}, status=403)
    session_id = session_id_from_request(request)
    deleted = False
    if session_id:
        await ensure_schema(env)
        deleted = await delete_session(env, session_id)
    response = json_response({"ok": True, "deleted": deleted},
                             cache_control="no-store")
    secure = str(getattr(url, "scheme", "https")) == "https"
    attach_set_cookie(
        response, clear_cookie_header(cookie_domain(env, request), secure))
    return response


# --- pagination ------------------------------------------------------------

def _encode_cursor(row):
    return "%d.%s" % (int(row["_cursor_ts"]), str(row["_cursor_id"]))


def _decode_cursor(cursor):
    cursor = str(cursor or "").strip()
    if not cursor:
        return None
    if not _CURSOR_RE.match(cursor):
        raise ValueError("invalid cursor")
    ts, _, ident = cursor.partition(".")
    return int(ts), ident


def _page_limit(limit):
    try:
        limit = int(limit or PAGE_SIZE)
    except (TypeError, ValueError):
        limit = PAGE_SIZE
    return max(1, min(MAX_PAGE_SIZE, limit))


def _session_filters(from_ms, to_ms, include_bots):
    where = ["first_seen_at>=?", "first_seen_at<?"]
    args = [int(from_ms), int(to_ms)]
    if not include_bots:
        where.append("is_bot=0")
    return where, args


async def list_sessions(env, from_ms, to_ms, include_bots=False, cursor=None,
                        limit=PAGE_SIZE):
    """One page of sessions first seen in ``[from_ms, to_ms)``.

    Returns ``{items, next_cursor, total_count}``. The keyset cursor is
    ``(first_seen_at, id)``; ``total_count`` is the count under the same
    filters so a consumer can prove it collected every row.
    """
    where, args = _session_filters(from_ms, to_ms, include_bots)
    limit = _page_limit(limit)
    total = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM attribution_sessions WHERE "
        + " AND ".join(where), *args)
    page_where = list(where)
    page_args = list(args)
    decoded = _decode_cursor(cursor)
    if decoded:
        page_where.append("(first_seen_at>? OR (first_seen_at=? AND id>?))")
        page_args.extend([decoded[0], decoded[0], decoded[1]])
    rows = await d1_all(
        env,
        "SELECT id, first_seen_at, last_seen_at, first_utm_source, "
        "first_utm_medium, first_utm_campaign, first_utm_term, "
        "first_utm_content, last_utm_source, last_utm_medium, "
        "last_utm_campaign, last_utm_term, last_utm_content, "
        "first_landing_path, first_referrer_host, is_bot, bot_reason, "
        "user_id FROM attribution_sessions WHERE " + " AND ".join(page_where)
        + " ORDER BY first_seen_at, id LIMIT ?",
        *(page_args + [limit + 1]))
    items = [dict(r) for r in rows[:limit]]
    next_cursor = None
    if len(rows) > limit and items:
        last = items[-1]
        next_cursor = _encode_cursor(
            {"_cursor_ts": last["first_seen_at"], "_cursor_id": last["id"]})
    return {
        "items": items,
        "next_cursor": next_cursor,
        "total_count": int((total or {}).get("n") or 0),
    }


async def list_events(env, from_ms, to_ms, cursor=None, limit=PAGE_SIZE,
                      event_name=None):
    """One page of events that occurred in ``[from_ms, to_ms)``."""
    where = ["occurred_at>=?", "occurred_at<?"]
    args = [int(from_ms), int(to_ms)]
    if event_name:
        where.append("event_name=?")
        args.append(str(event_name))
    limit = _page_limit(limit)
    total = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM attribution_events WHERE "
        + " AND ".join(where), *args)
    page_where = list(where)
    page_args = list(args)
    decoded = _decode_cursor(cursor)
    if decoded:
        page_where.append("(occurred_at>? OR (occurred_at=? AND id>?))")
        page_args.extend([decoded[0], decoded[0], decoded[1]])
    rows = await d1_all(
        env,
        "SELECT id, session_id, event_name, occurred_at, metadata "
        "FROM attribution_events WHERE " + " AND ".join(page_where)
        + " ORDER BY occurred_at, id LIMIT ?",
        *(page_args + [limit + 1]))
    items = [dict(r) for r in rows[:limit]]
    next_cursor = None
    if len(rows) > limit and items:
        last = items[-1]
        next_cursor = _encode_cursor(
            {"_cursor_ts": last["occurred_at"], "_cursor_id": last["id"]})
    return {
        "items": items,
        "next_cursor": next_cursor,
        "total_count": int((total or {}).get("n") or 0),
    }


async def collect_all(page_reader, limit=PAGE_SIZE):
    """Consume a paginated reader to exhaustion and prove nothing was missed.

    ``page_reader(cursor, limit)`` must return ``{items, next_cursor,
    total_count}``. Raises ReportIntegrityError when the rows collected do
    not equal the server-side total: a partial report is surfaced as an
    error, never rendered as a smaller number.
    """
    collected = []
    cursor = None
    total = None
    pages = 0
    while True:
        page = await page_reader(cursor, limit)
        pages += 1
        if total is None:
            total = int(page.get("total_count") or 0)
        collected.extend(page.get("items") or [])
        cursor = page.get("next_cursor")
        if cursor is None:
            break
        if pages > 100_000:
            raise ReportIntegrityError("pagination did not terminate")
    if len(collected) != total:
        raise ReportIntegrityError(
            "collected %d rows but the server reported %d"
            % (len(collected), total))
    return collected


async def list_clone_snapshots(env, from_day, to_day, cursor=None,
                               limit=PAGE_SIZE):
    """One page of daily clone snapshots for ``[from_day, to_day]``.

    Keyset on ``(day, owner, repo)`` encoded as one opaque string.
    """
    where = ["day>=?", "day<=?"]
    args = [str(from_day), str(to_day)]
    limit = _page_limit(limit)
    total = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM attribution_clone_snapshots WHERE "
        + " AND ".join(where), *args)
    page_where = list(where)
    page_args = list(args)
    cursor = str(cursor or "")
    if cursor:
        parts = cursor.split("\x1f")
        if len(parts) != 3:
            raise ValueError("invalid cursor")
        page_where.append(
            "(day>? OR (day=? AND owner>?) OR (day=? AND owner=? AND repo>?))")
        page_args.extend([parts[0], parts[0], parts[1],
                          parts[0], parts[1], parts[2]])
    rows = await d1_all(
        env,
        "SELECT day, owner, repo, clones_total FROM attribution_clone_snapshots"
        " WHERE " + " AND ".join(page_where)
        + " ORDER BY day, owner, repo LIMIT ?",
        *(page_args + [limit + 1]))
    items = [dict(r) for r in rows[:limit]]
    next_cursor = None
    if len(rows) > limit and items:
        last = items[-1]
        next_cursor = "\x1f".join(
            [str(last["day"]), str(last["owner"]), str(last["repo"])])
    return {
        "items": items,
        "next_cursor": next_cursor,
        "total_count": int((total or {}).get("n") or 0),
    }


# --- date ranges ---------------------------------------------------------------

def _utc_day(now_ms):
    return datetime.fromtimestamp(
        int(now_ms) / 1000, tz=timezone.utc).strftime("%Y-%m-%d")


def _local_day_start_ms(day, tz):
    return int(datetime(day.year, day.month, day.day, tzinfo=tz)
               .timestamp() * 1000)


def default_range(now_ms, tz):
    """The most recent complete Monday–Sunday week, as local-day bounds."""
    today = datetime.fromtimestamp(int(now_ms) / 1000, tz=tz).date()
    days_since_sunday = (today.weekday() + 1) % 7
    end = today - timedelta(days=days_since_sunday)
    if end == today:
        end = today - timedelta(days=7)
    start = end - timedelta(days=6)
    return start, end


def parse_range(params, now_ms, tz):
    """``from``/``to`` query days (inclusive) or the default week.

    Returns ``(from_ms, to_ms, from_day, to_day)`` with ``to_ms`` exclusive
    and clamped to the report's own start, so rows created while the report
    is being paginated can never make its integrity check fail.
    """
    start, end = default_range(now_ms, tz)
    raw_from = str((params.get("from") or [""])[0] or "").strip()
    raw_to = str((params.get("to") or [""])[0] or "").strip()
    if _DAY_RE.match(raw_from) and _DAY_RE.match(raw_to):
        try:
            candidate_start = datetime.strptime(raw_from, "%Y-%m-%d").date()
            candidate_end = datetime.strptime(raw_to, "%Y-%m-%d").date()
            if candidate_start <= candidate_end and (
                    candidate_end - candidate_start).days <= 400:
                start, end = candidate_start, candidate_end
        except ValueError:
            pass
    from_ms = _local_day_start_ms(start, tz)
    to_ms = _local_day_start_ms(end + timedelta(days=1), tz)
    to_ms = min(to_ms, int(now_ms))
    return from_ms, to_ms, start.isoformat(), end.isoformat()


# --- reporting -------------------------------------------------------------

def _channel_key(session):
    return (
        str(session.get("first_utm_source") or "(none)"),
        str(session.get("first_utm_campaign") or "(none)"),
    )


async def channel_report(env, from_ms, to_ms, include_bots=False, now_ms=None):
    """Sessions-by-channel funnel for sessions first seen in the range.

    First-touch is the reporting default. Every list this touches is read
    through collect_all, so a partial page can never become a smaller
    number on the dashboard.
    """
    now_ms = int(now_ms or _now())
    report_started = now_ms

    async def sessions_page(cursor, limit):
        return await list_sessions(
            env, from_ms, to_ms, include_bots, cursor, limit)

    async def bot_sessions_page(cursor, limit):
        return await list_sessions(env, from_ms, to_ms, True, cursor, limit)

    async def events_page(cursor, limit):
        # Events cannot precede their session, and everything that happened
        # after this report started is excluded so paging stays stable.
        return await list_events(env, from_ms, report_started, cursor, limit)

    sessions = await collect_all(sessions_page)
    session_ids = {s["id"] for s in sessions}
    excluded_bots = 0
    if not include_bots:
        all_sessions = await collect_all(bot_sessions_page)
        excluded_bots = len(all_sessions) - len(sessions)
    events = await collect_all(events_page)

    per_session_events = {}
    for event in events:
        sid = event.get("session_id")
        if sid in session_ids:
            per_session_events.setdefault(sid, set()).add(
                event.get("event_name"))

    rows = {}
    for session in sessions:
        key = _channel_key(session)
        row = rows.setdefault(key, {
            "utm_source": key[0], "utm_campaign": key[1], "sessions": 0,
            **{name: 0 for name in EVENT_NAMES if name != "landing_view"},
        })
        row["sessions"] += 1
        for name in per_session_events.get(session["id"], ()):
            if name in row:
                row[name] += 1
    ordered = sorted(
        rows.values(),
        key=lambda r: (-r["sessions"], r["utm_source"], r["utm_campaign"]))
    totals = {"sessions": len(sessions),
              **{name: 0 for name in EVENT_NAMES if name != "landing_view"}}
    for row in ordered:
        for name in totals:
            if name != "sessions":
                totals[name] += row[name]
    for row in ordered + [totals]:
        row["conversion"] = _conversions(row)

    clone_stats = await clone_count_report(
        env, _utc_day(from_ms), _utc_day(max(from_ms, to_ms - 1)))
    filtered = await filtered_hit_counts(
        env, _utc_day(from_ms), _utc_day(max(from_ms, to_ms - 1)))
    return {
        "rows": ordered,
        "row_count": len(ordered),
        "totals": totals,
        "sessions_collected": len(sessions),
        "events_collected": len(events),
        "excluded_bot_sessions": excluded_bots,
        "filtered_hits": filtered,
        "clones": clone_stats,
        "include_bots": bool(include_bots),
    }


def _conversions(row):
    """Rate between each consecutive funnel stage, None when undefined."""
    out = {}
    stages = list(FUNNEL_STAGES)
    for previous, current in zip(stages, stages[1:]):
        denominator = int(row.get(previous) or 0)
        numerator = int(row.get(current) or 0)
        out[current] = (numerator / denominator) if denominator else None
    waitlist = int(row.get("managed_waitlist_submitted") or 0)
    out["discovery_call_accepted"] = (
        int(row.get("discovery_call_accepted") or 0) / waitlist
        if waitlist else None)
    return out


async def clone_count_report(env, from_day, to_day):
    """Unattributed clone totals for the day range, from daily snapshots.

    LIMITATION — read before changing: an anonymous ``git clone`` sends no
    browser cookie, so a clone can never be joined to a session or a
    channel. ``clone_url_copied`` is a browser intent signal; the clone
    count below is a server-side total with no session and no user. They
    are two different metrics. Never sum them, and never present the copy
    count as a clone count. The ratio is shown only as a labelled estimate.
    """
    async def page(cursor, limit):
        return await list_clone_snapshots(env, from_day, to_day, cursor, limit)

    snapshots = await collect_all(page)
    # Also need the last snapshot before the range for the first day's delta.
    previous = {}
    prior_rows = await d1_all(
        env,
        "SELECT s.owner, s.repo, s.clones_total FROM attribution_clone_snapshots"
        " AS s JOIN (SELECT owner, repo, MAX(day) AS day FROM "
        "attribution_clone_snapshots WHERE day<? GROUP BY owner, repo) AS latest"
        " ON latest.owner=s.owner AND latest.repo=s.repo AND latest.day=s.day",
        str(from_day))
    for row in prior_rows or []:
        previous[(row["owner"], row["repo"])] = int(row["clones_total"] or 0)
    per_day = {}
    per_repo = {}
    for snapshot in snapshots:
        key = (snapshot["owner"], snapshot["repo"])
        total = int(snapshot["clones_total"] or 0)
        delta = max(0, total - previous.get(key, 0))
        previous[key] = total
        if delta:
            per_day[snapshot["day"]] = per_day.get(snapshot["day"], 0) + delta
            per_repo[key] = per_repo.get(key, 0) + delta
    return {
        "total": sum(per_day.values()),
        "days": sorted(per_day.items()),
        "repositories": sorted(
            ({"owner": k[0], "repo": k[1], "clones": v}
             for k, v in per_repo.items()),
            key=lambda r: (-r["clones"], r["owner"], r["repo"])),
        "snapshot_rows": len(snapshots),
        "from_day": from_day,
        "to_day": to_day,
    }


async def filtered_hit_counts(env, from_day, to_day):
    rows = await d1_all(
        env,
        "SELECT reason, SUM(hits) AS hits FROM attribution_filtered_hits "
        "WHERE day>=? AND day<=? GROUP BY reason ORDER BY reason",
        str(from_day), str(to_day))
    return {str(r["reason"]): int(r["hits"] or 0) for r in rows or []}


def report_csv(report):
    out = io.StringIO()
    writer = csv.writer(out)
    columns = ["utm_source", "utm_campaign", "sessions"] + [
        name for name in EVENT_NAMES if name != "landing_view"]
    writer.writerow(columns)
    for row in report["rows"]:
        writer.writerow([row.get(c, "") for c in columns])
    totals = report["totals"]
    writer.writerow(["TOTAL", ""] + [totals.get(c, "") for c in columns[2:]])
    writer.writerow([])
    writer.writerow(["row_count", report["row_count"]])
    writer.writerow(["sessions_collected", report["sessions_collected"]])
    writer.writerow(["excluded_bot_sessions", report["excluded_bot_sessions"]])
    writer.writerow(["unattributed_clones_total", report["clones"]["total"]])
    return out.getvalue()


# --- maintenance (cron) --------------------------------------------------------

async def maintenance(env, now_ms=None):
    """Hourly sweep: bounce rule, IP-digest retention, session retention."""
    now = int(now_ms or _now())
    await ensure_schema(env)
    await d1_run(
        env,
        "UPDATE attribution_sessions SET is_bot=1, bot_reason='bounce' "
        "WHERE is_bot=0 AND user_id IS NULL AND first_seen_at<? "
        "AND last_seen_at-first_seen_at<? "
        "AND NOT EXISTS (SELECT 1 FROM attribution_events AS e "
        "WHERE e.session_id=attribution_sessions.id "
        "AND e.event_name<>'landing_view')",
        now - BOUNCE_GRACE_MS, BOUNCE_WINDOW_MS)
    await d1_run(
        env,
        "DELETE FROM attribution_ip_hourly WHERE hour_bucket<?",
        (now - IP_RETENTION_MS) // IP_RATE_WINDOW_MS)
    cutoff = now - retention_days(env) * 24 * 60 * 60 * 1000
    await d1_run(
        env,
        "DELETE FROM attribution_events WHERE session_id IN "
        "(SELECT id FROM attribution_sessions WHERE first_seen_at<?)",
        cutoff)
    await d1_run(
        env, "DELETE FROM attribution_sessions WHERE first_seen_at<?", cutoff)
    await d1_run(
        env, "DELETE FROM attribution_filtered_hits WHERE day<?",
        _utc_day(cutoff))
    return {"ok": True}


async def snapshot_clone_counts(env, now_ms=None):
    """Daily: copy cumulative per-repository clone totals into a dated row.

    Reads mirror_serve_counters, which the clone-serving path already
    maintains; nothing here runs on, gates or observes a clone request.
    """
    now = int(now_ms or _now())
    day = _utc_day(now)
    await ensure_schema(env)

    async def page(cursor, limit):
        where = []
        args = []
        cursor = str(cursor or "")
        if cursor:
            owner, _, repo = cursor.partition("\x1f")
            where.append("(owner>? OR (owner=? AND repo>?))")
            args.extend([owner, owner, repo])
        total = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM (SELECT owner, repo FROM "
            "mirror_serve_counters GROUP BY owner, repo)")
        rows = await d1_all(
            env,
            "SELECT owner, repo, SUM(clones) AS clones FROM mirror_serve_counters"
            + (" WHERE " + " AND ".join(where) if where else "")
            + " GROUP BY owner, repo ORDER BY owner, repo LIMIT ?",
            *(args + [limit + 1]))
        items = [dict(r) for r in rows[:limit]]
        next_cursor = None
        if len(rows) > limit and items:
            next_cursor = str(items[-1]["owner"]) + "\x1f" + str(items[-1]["repo"])
        return {"items": items, "next_cursor": next_cursor,
                "total_count": int((total or {}).get("n") or 0)}

    counters = await collect_all(page)
    for row in counters:
        await d1_run(
            env,
            "INSERT INTO attribution_clone_snapshots (day, owner, repo, "
            "clones_total, captured_at) VALUES (?,?,?,?,?) "
            "ON CONFLICT(day, owner, repo) DO UPDATE SET "
            "clones_total=excluded.clones_total, captured_at=excluded.captured_at",
            day, str(row["owner"] or ""), str(row["repo"] or ""),
            int(row["clones"] or 0), now)
    return {"ok": True, "day": day, "repositories": len(counters)}


# --- admin API and dashboard ---------------------------------------------------

async def _admin_gate(env, request):
    return await _check_admin_page_auth(env, request)


def _admin_denied(request):
    if method_name(request) == "GET":
        try:
            url = urlparse(request.url)
            next_path = url.path + (("?" + url.query) if url.query else "")
        except Exception:
            next_path = "/admin/acquisition"
        return Response(
            "Admin login required.", status=302,
            headers={
                "location": "/login?next=" + _quote(next_path),
                "cache-control": "no-store, max-age=0, must-revalidate",
                "content-type": "text/plain; charset=utf-8",
            })
    return json_response({"error": "admin_required"}, status=403,
                         cache_control="no-store")


def _quote(value):
    quoter = globals().get("quote")
    if quoter is None:
        from urllib.parse import quote as quoter
    return quoter(str(value))


def _params(request):
    try:
        return parse_qs(urlparse(request.url).query)
    except Exception:
        return {}


async def admin_report_handler(env, request):
    """``GET /api/admin/acquisition/report`` — JSON of the dashboard data."""
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not await _admin_gate(env, request):
        return _admin_denied(request)
    await ensure_schema(env)
    params = _params(request)
    tz, tz_label = report_timezone(env)
    now = _now()
    from_ms, to_ms, from_day, to_day = parse_range(params, now, tz)
    include_bots = str((params.get("bots") or [""])[0]) == "1"
    try:
        report = await channel_report(env, from_ms, to_ms, include_bots, now)
    except ReportIntegrityError as error:
        return json_response(
            {"error": "report_integrity", "detail": str(error)}, status=500,
            cache_control="no-store")
    report.update({"from": from_day, "to": to_day, "timezone": tz_label})
    return json_response(report, cache_control="no-store")


async def admin_list_handler(env, request, kind):
    """``GET /api/admin/acquisition/{sessions,events}`` — one cursor page."""
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not await _admin_gate(env, request):
        return _admin_denied(request)
    await ensure_schema(env)
    params = _params(request)
    tz, _ = report_timezone(env)
    now = _now()
    from_ms, to_ms, _, _ = parse_range(params, now, tz)
    cursor = (params.get("cursor") or [None])[0]
    limit = (params.get("limit") or [PAGE_SIZE])[0]
    event_name = str((params.get("event") or [""])[0] or "").strip() or None
    if event_name and event_name not in EVENT_NAMES:
        return json_response({"error": "unknown_event"}, status=400)
    try:
        if kind == "sessions":
            page = await list_sessions(
                env, from_ms, to_ms,
                str((params.get("bots") or [""])[0]) == "1", cursor, limit)
        else:
            page = await list_events(
                env, from_ms, to_ms, cursor, limit, event_name)
    except ValueError:
        return json_response({"error": "invalid_cursor"}, status=400)
    return json_response(page, cache_control="no-store")


async def admin_discovery_call_handler(env, request):
    """``POST /api/admin/acquisition/discovery-call`` — mark one accepted.

    Finds the newest managed_waitlist_submitted event for the email (via
    its blind index in event metadata) and emits discovery_call_accepted on
    that session. The email itself is never stored by this module.
    """
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not await _admin_gate(env, request):
        return _admin_denied(request)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    email = str((data or {}).get("email") or "").strip().lower()[:254]
    if not _EMAIL_RE.match(email):
        return json_response({"error": "valid_email_required"}, status=400)
    await ensure_schema(env)
    email_bi = await blind_index(env, email)
    row = await d1_first(
        env,
        "SELECT session_id FROM attribution_events "
        "WHERE event_name='managed_waitlist_submitted' AND metadata LIKE ? "
        "ORDER BY occurred_at DESC, id DESC LIMIT 1",
        '%"email_bi":"' + email_bi + '"%')
    if not row:
        return json_response({"error": "waitlist_session_not_found"},
                             status=404, cache_control="no-store")
    recorded = await emit(
        env, row.get("session_id"), "discovery_call_accepted",
        {"email_bi": email_bi})
    return json_response({"ok": True, "recorded": recorded},
                         cache_control="no-store")


def _esc(value):
    return html.escape(str(value if value is not None else ""), quote=True)


def _pct(value):
    return "–" if value is None else "%.1f%%" % (value * 100)


_DASHBOARD_STYLE = """
 :root{color-scheme:light dark;--bg:#ffffff;--fg:#1f2328;--muted:#59636e;
   --border:#d1d9e0;--card:#f6f8fa;--link:#0969da;--warn:#9a6700}
 @media (prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#c9d1d9;
   --muted:#8b949e;--border:#30363d;--card:#161b22;--link:#58a6ff;--warn:#d29922}}
 body{font:14px/1.5 system-ui,sans-serif;margin:0;background:var(--bg);color:var(--fg)}
 main{max-width:1100px;margin:0 auto;padding:24px}
 h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px}
 .meta{color:var(--muted);font-size:13px}
 a{color:var(--link)}
 form.range{display:flex;gap:10px;align-items:end;flex-wrap:wrap;margin:16px 0;
   padding:12px;border:1px solid var(--border);border-radius:8px;background:var(--card)}
 label{display:flex;flex-direction:column;font-size:12px;color:var(--muted)}
 input,button{font:13px system-ui;padding:6px 8px;border:1px solid var(--border);
   border-radius:6px;background:var(--bg);color:var(--fg)}
 button{cursor:pointer}
 table{border-collapse:collapse;width:100%;font-size:13px}
 th,td{border:1px solid var(--border);padding:6px 8px;text-align:right;white-space:nowrap}
 th:first-child,td:first-child,th:nth-child(2),td:nth-child(2){text-align:left}
 tfoot td{font-weight:600;background:var(--card)}
 .cards{display:flex;gap:12px;flex-wrap:wrap;margin:12px 0}
 .card{border:1px solid var(--border);border-radius:8px;padding:10px 14px;
   background:var(--card);min-width:140px}
 .card .n{font-size:22px;font-weight:600}.card .l{font-size:12px;color:var(--muted)}
 .note{border-left:3px solid var(--warn);padding:8px 12px;background:var(--card);
   margin:12px 0;font-size:13px}
 .wrap{overflow-x:auto}
"""


_ACQUISITION_STYLE = """
.acq td,.acq th{text-align:right;white-space:nowrap}
.acq td:first-child,.acq th:first-child,.acq td:nth-child(2),
.acq th:nth-child(2){text-align:left}
.acq tfoot td{font-weight:600;background:var(--canvas);height:44px;
  border-top:1px solid var(--border)}
"""

# The discovery-call endpoint takes JSON, so its form still needs this one
# helper; the /admin/acquisition route runs under the global script policy.
_DISCOVERY_SCRIPT = (
    "<script>window.__fmDiscovery=function(e){e.preventDefault();"
    "var f=e.target,s=document.getElementById('discovery-status');"
    "fetch(f.action,{method:'POST',credentials:'same-origin',"
    "headers:{'content-type':'application/json'},"
    "body:JSON.stringify({email:f.email.value})}).then(function(r){"
    "return r.json().then(function(j){s.textContent=r.ok?'Recorded.':"
    "('Not recorded: '+(j.error||r.status));});}).catch(function(){"
    "s.textContent='Request failed.';});return false;};</script>")


def render_dashboard_html(report, from_day, to_day, tz_label, include_bots,
                          error=None, admin="", console="", theme="system",
                          badges=None):
    """Server-rendered /admin/acquisition. Every value is HTML-escaped."""
    shell = _admin_shell
    event_columns = [name for name in EVENT_NAMES if name != "landing_view"]
    query_base = "from=%s&amp;to=%s" % (_quote(from_day), _quote(to_day))
    filters = "".join([
        "<form class=\"actions range\" method=\"get\" "
        "action=\"/admin/acquisition\">",
        "<label class=\"sr\" for=\"acq-from\">From</label>",
        "<input id=\"acq-from\" type=\"date\" name=\"from\" value=\"",
        _esc(from_day), "\">",
        "<label class=\"sr\" for=\"acq-to\">To</label>",
        "<input id=\"acq-to\" type=\"date\" name=\"to\" value=\"",
        _esc(to_day), "\">",
        "<label class=\"sr\" for=\"acq-bots\">Bots</label>",
        "<select id=\"acq-bots\" name=\"bots\">",
        "<option value=\"0\"", "" if include_bots else " selected",
        ">Exclude bots</option>",
        "<option value=\"1\"", " selected" if include_bots else "",
        ">Show bots</option></select>",
        "<button class=\"btn\" type=\"submit\">Apply</button>",
        "<a class=\"btn btn-primary\" href=\"/admin/acquisition?", query_base,
        "&amp;bots=", "1" if include_bots else "0", "&amp;format=csv\">",
        shell.icon("download"), "Download CSV</a>",
        "</form>",
    ])
    parts = [shell.page_head(
        "Acquisition",
        "First-touch attribution by session. Range %s to %s (%s, Monday–Sunday "
        "weeks). Bots %s." % (from_day, to_day, tz_label,
                              "included" if include_bots else "excluded"),
        crumb="Growth", actions=filters)]
    page_kwargs = dict(
        active="acquisition", console=console, theme=theme, badges=badges,
        account=admin,
        next_path="/admin/acquisition?from=%s&to=%s&bots=%s" % (
            _quote(from_day), _quote(to_day), "1" if include_bots else "0"),
        head_extra="<style>" + _ACQUISITION_STYLE + "</style>")
    if error:
        parts.append(
            "<div class=\"banner bad\" role=\"alert\">%s<span><strong>Report "
            "not shown.</strong> %s The pagination integrity check failed, so "
            "no number from this range is displayed. Reload to retry.</span>"
            "</div>" % (shell.icon("alert"), _esc(error)))
        return shell.render_page("Acquisition", "".join(parts), **page_kwargs)
    totals = report["totals"]
    clones = report["clones"]
    copied = int(totals.get("clone_url_copied") or 0)
    ratio = ("%.2f" % (clones["total"] / copied)) if copied else "–"
    range_label = "%s – %s" % (from_day, to_day)
    parts.append(
        "<div class=\"grid cols-3\">"
        + _card(totals["sessions"], "Sessions", range_label)
        + _card(totals.get("clone_url_copied"), "Clone URL copied",
                "An intent signal, not a clone count")
        + _card(totals.get("signup_completed"), "Signups",
                "Attributed to a session")
        + _card(totals.get("first_mirror_created"), "First mirrors",
                range_label)
        + _card(totals.get("managed_waitlist_submitted"), "Waitlist",
                range_label)
        + _card(report["excluded_bot_sessions"] if not include_bots else 0,
                "Bot sessions excluded",
                "Bots shown in this range" if include_bots else
                "User agent and behaviour")
        + "</div>")

    channel = ["<table class=\"acq\"><thead><tr><th>utm_source</th>"
               "<th>utm_campaign</th><th>sessions</th>"]
    for name in event_columns:
        channel.append("<th>" + _esc(name) + "</th>")
    channel.append("</tr></thead><tbody>")
    for row in report["rows"]:
        channel.append(
            "<tr><td class=\"mono\">" + _esc(row["utm_source"])
            + "</td><td class=\"mono\">" + _esc(row["utm_campaign"])
            + "</td><td>" + str(int(row["sessions"])) + "</td>")
        for name in event_columns:
            channel.append("<td>" + str(int(row.get(name) or 0)) + "</td>")
        channel.append("</tr>")
    channel.append("</tbody><tfoot><tr><td>TOTAL</td><td></td><td>"
                   + str(int(totals["sessions"])) + "</td>")
    for name in event_columns:
        channel.append("<td>" + str(int(totals.get(name) or 0)) + "</td>")
    channel.append("</tr></tfoot></table>")
    parts.append(shell.card(
        title="Channels",
        description=(
            "Showing %d of %d channel rows, built from %d sessions and %d "
            "events (all pages consumed)." % (
                report["row_count"], report["row_count"],
                report["sessions_collected"], report["events_collected"])),
        table="".join(channel)))

    stage_pairs = list(zip(FUNNEL_STAGES, FUNNEL_STAGES[1:])) + [
        ("managed_waitlist_submitted", "discovery_call_accepted")]
    conversion = ["<table class=\"acq\"><thead><tr><th>utm_source</th>"
                  "<th>utm_campaign</th>"]
    for previous, current in stage_pairs:
        conversion.append(
            "<th>" + _esc(previous) + " → " + _esc(current) + "</th>")
    conversion.append("</tr></thead><tbody>")
    conversion_rows = report["rows"] + [dict(totals, utm_source="TOTAL",
                                             utm_campaign="")]
    for position, row in enumerate(conversion_rows):
        total_row = position == len(conversion_rows) - 1
        conversion.append(
            "<tr%s><td class=\"mono\">%s</td><td class=\"mono\">%s</td>" % (
                " class=\"total\"" if total_row else "",
                _esc(row["utm_source"]), _esc(row["utm_campaign"])))
        for _, current in stage_pairs:
            conversion.append(
                "<td>" + _esc(_pct(row["conversion"].get(current))) + "</td>")
        conversion.append("</tr>")
    conversion.append("</tbody></table>")
    parts.append("<div style=\"height:16px\"></div>")
    parts.append(shell.card(
        title="Conversion between consecutive stages",
        description="Each stage as a share of the one before it.",
        table="".join(conversion)))

    top = []
    for row in clones["repositories"][:20]:
        top.append("<tr><td class=\"mono\">" + _esc(row["owner"])
                   + "</td><td class=\"mono\">" + _esc(row["repo"])
                   + "</td><td>" + str(int(row["clones"])) + "</td></tr>")
    clones_card = (
        "<section class=\"card\"><div class=\"card-h\"><div><h2>Clones "
        "(unattributed)</h2><p>Showing %d of %d repositories with clones; "
        "%d snapshot rows consumed.</p></div></div>"
        "<div class=\"card-b\" style=\"padding-bottom:0\"><div class=\"banner "
        "info\">%s<span>Two different metrics. <code>clone_url_copied</code> "
        "is a browser intent signal joined to a session. The clone count is a "
        "server-side per-repository daily total with no session and no user: "
        "git sends no browser cookie, so a clone can never be attributed to a "
        "channel. They are never summed. The ratio is an <strong>estimate"
        "</strong> of how many clones follow one copy, nothing more.</span>"
        "</div></div>"
        "<div class=\"minis three\" style=\"border-radius:0\">"
        "<div class=\"mini\"><b>%s</b><span>clones %s → %s (UTC days)</span>"
        "</div><div class=\"mini\"><b>%s</b><span>clone URL copied</span></div>"
        "<div class=\"mini\"><b>%s</b><span>clones per copy (estimate)</span>"
        "</div></div><div class=\"card-t\" style=\"margin-top:0\">%s</div>"
        "</section>" % (
            min(20, len(clones["repositories"])), len(clones["repositories"]),
            clones["snapshot_rows"], shell.icon("alert"),
            _esc(clones["total"]), _esc(clones["from_day"]),
            _esc(clones["to_day"]), _esc(copied), _esc(ratio),
            shell.table(
                "<th>owner</th><th>repo</th><th class=\"r\">clones</th>",
                top, "No repository was cloned in this range.")
            .replace("<table>", "<table class=\"acq\">")))
    filtering = shell.card(
        "<dl class=\"kv\"><dt>Bot sessions</dt><dd>%s: %s</dd>"
        "<dt>Crawlers refused</dt><dd>Requests refused before a session "
        "existed: %s</dd></dl>" % (
            "shown" if include_bots else "excluded",
            _esc(report["excluded_bot_sessions"]),
            _esc(", ".join("%s=%d" % (k, v)
                           for k, v in sorted(report["filtered_hits"].items()))
                 or "none")),
        title="Filtering",
        description="What was left out of the figures above, and why.")
    discovery = shell.card(
        "<form class=\"form-row\" method=\"post\" "
        "action=\"/api/admin/acquisition/discovery-call\" "
        "onsubmit=\"return window.__fmDiscovery(event)\">"
        "<label class=\"sr\" for=\"acq-email\">Waitlist email</label>"
        "<input id=\"acq-email\" class=\"grow\" type=\"email\" name=\"email\" "
        "placeholder=\"Waitlist email\" required>"
        "<button class=\"btn btn-primary\" type=\"submit\">Mark discovery call "
        "accepted</button></form>"
        "<p class=\"note\" id=\"discovery-status\" role=\"status\"></p>",
        title="Discovery call",
        description=("Record that a waitlist signup accepted a discovery "
                     "call. The email is matched by blind index and never "
                     "stored."))
    parts.append(
        "<div class=\"grid split\" style=\"margin-top:16px\">" + clones_card
        + "<div class=\"stack\">" + filtering + discovery + "</div></div>")
    return shell.render_page(
        "Acquisition", "".join(parts), body_extra=_DISCOVERY_SCRIPT,
        **page_kwargs)


def _card(value, label, window=""):
    return _admin_shell.kpi(label, value, window)


async def _admin_page_chrome(env, request):
    """The shell context for an administrator viewing an admin page."""
    shell = _admin_shell
    name = globals().get("_account_cookie_name")
    try:
        admin = str((await name(env, request)) if name else "")
    except Exception:
        admin = ""
    return dict(admin=admin, console=shell.console_base(env),
                theme=shell.theme_from_request(request),
                badges=await shell.nav_badges(env))


async def admin_dashboard_handler(env, request):
    """``GET /admin/acquisition`` (``?format=csv`` for the export)."""
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    if not await _admin_gate(env, request):
        return _admin_denied(request)
    await ensure_schema(env)
    params = _params(request)
    tz, tz_label = report_timezone(env)
    now = _now()
    from_ms, to_ms, from_day, to_day = parse_range(params, now, tz)
    include_bots = str((params.get("bots") or [""])[0]) == "1"
    wants_csv = str((params.get("format") or [""])[0]) == "csv"
    headers = {
        "cache-control": "no-store, max-age=0, must-revalidate",
        "x-robots-tag": "noindex",
    }
    try:
        report = await channel_report(env, from_ms, to_ms, include_bots, now)
    except ReportIntegrityError as error:
        body = render_dashboard_html(
            None, from_day, to_day, tz_label, include_bots, error=str(error),
            **(await _admin_page_chrome(env, request)))
        headers["content-type"] = "text/html; charset=utf-8"
        return Response(body, status=500, headers=headers)
    if wants_csv:
        headers["content-type"] = "text/csv; charset=utf-8"
        headers["content-disposition"] = (
            "attachment; filename=\"acquisition-%s-%s%s.csv\""
            % (from_day, to_day, "-with-bots" if include_bots else ""))
        return Response(report_csv(report), status=200, headers=headers)
    headers["content-type"] = "text/html; charset=utf-8"
    return Response(
        render_dashboard_html(report, from_day, to_day, tz_label, include_bots,
                              **(await _admin_page_chrome(env, request))),
        status=200, headers=headers)


# --- public transparency page ------------------------------------------------

def round_coarse(value):
    value = int(value or 0)
    if value < 10:
        return "under 10"
    if value < 1000:
        return str(int(value / 10.0 + 0.5) * 10)
    return str(int(value / 100.0 + 0.5) * 100)


def render_transparency_html(report, from_day, to_day, tz_label):
    """Methodology, filtering rules, limitations and coarse aggregates only.

    Deliberately not a leaderboard: no ranking language, no signup,
    conversion or revenue figures, and every count rounded.
    """
    parts = [
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">",
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">",
        "<title>How ForkMesh measures acquisition</title>",
        "<style>", _DASHBOARD_STYLE, "</style></head><body><main>",
        "<h1>How ForkMesh measures acquisition</h1>",
        "<p class=\"meta\">Period ", _esc(from_day), " to ", _esc(to_day),
        " (", _esc(tz_label), "). Figures are rounded. This page is a "
        "methodology note with coarse totals, not a ranking.</p>",
        "<h2>What is measured</h2>",
        "<p>When a browser loads a ForkMesh page, the server records one "
        "anonymous session identified by a random id in a 90-day cookie. Any "
        "<code>utm_source</code>, <code>utm_medium</code>, "
        "<code>utm_campaign</code>, <code>utm_term</code> and "
        "<code>utm_content</code> values on the link are cleaned server-side "
        "(lowercase, limited character set, at most 64 characters) and "
        "checked against a short allowlist; anything else is stored as "
        "<code>other</code>. The first tagged visit is kept as first-touch "
        "and never overwritten; the most recent tagged visit is last-touch. "
        "Reporting uses first-touch.</p>",
        "<h2>Sessions and the attribution window</h2>",
        "<p>A session is one browser that keeps the cookie; the cookie lasts "
        "90 days from when it was set. Later actions — copying a clone URL, "
        "creating an account, publishing a first mirror, submitting a pull, "
        "joining the managed waitlist — are attributed to that session's "
        "first-touch channel for as long as the session exists. Accounts "
        "created without a session (cookies blocked, Do Not Track, Global "
        "Privacy Control, or a region where no cookie is set) are counted "
        "under <code>unknown</code> rather than dropped.</p>",
        "<h2>Bot filtering and spam control</h2>",
        "<ul><li>Requests whose User-Agent matches a maintained crawler list "
        "create no session; they are counted per day and excluded.</li>",
        "<li>A session with a single landing view, no other event and no "
        "further request after two seconds is flagged as a bot and excluded. "
        "The flag is lifted if the session is seen again.</li>",
        "<li>More than 30 sessions from one network address within an hour "
        "flag all of them. The address is kept only as a digest with a daily "
        "rotating salt, for 24 hours.</li>",
        "<li>Flagged sessions are kept, not deleted, so the filtering can be "
        "audited.</li></ul>",
        "<h2>Known limitations</h2>",
        "<ul><li><strong>Clones cannot be attributed.</strong> <code>git</code> "
        "sends no browser cookie, and ForkMesh will not append tokens to clone "
        "URLs (they would be written into your <code>.git/config</code>). "
        "Copying a clone URL on a page is an attributed intent signal; actual "
        "clones are a separate unattributed per-repository daily count. The "
        "two are never added together.</li>",
        "<li>Nothing is recorded for visitors sending <code>DNT: 1</code> or "
        "<code>Sec-GPC: 1</code>, or from regions where no cookie is set.</li>",
        "<li>Blog posts and documentation pages are served as static files "
        "and are not captured.</li>",
        "<li>Privacy browsers that discard cookies appear as new sessions on "
        "each visit.</li></ul>",
        "<h2>Sessions by channel (rounded)</h2>",
        "<div class=\"wrap\"><table><thead><tr><th>utm_source</th>",
        "<th>utm_campaign</th><th>sessions</th></tr></thead><tbody>",
    ]
    for row in sorted(report["rows"],
                      key=lambda r: (r["utm_source"], r["utm_campaign"])):
        parts.append("<tr><td>" + _esc(row["utm_source"]) + "</td><td>"
                     + _esc(row["utm_campaign"]) + "</td><td>"
                     + _esc(round_coarse(row["sessions"])) + "</td></tr>")
    parts.extend([
        "</tbody><tfoot><tr><td>All channels</td><td></td><td>",
        _esc(round_coarse(report["totals"]["sessions"])),
        "</td></tr></tfoot></table></div>",
        "<h2>Clones (rounded, unattributed)</h2>",
        "<p>Total repository clones served in the period, counted per "
        "repository per day with no session and no user: <strong>",
        _esc(round_coarse(report["clones"]["total"])),
        "</strong>. They cannot be broken down by channel for the reason "
        "above.</p>",
        "<p class=\"meta\">Bot sessions excluded from the sessions table: ",
        _esc(round_coarse(report["excluded_bot_sessions"])),
        ". Crawler requests refused: ",
        _esc(round_coarse(sum(report["filtered_hits"].values()))),
        ".</p>",
        "<p><a href=\"/privacy\">Privacy</a></p>",
        "</main></body></html>",
    ])
    return "".join(parts)


async def transparency_page_handler(env, request):
    """``GET /transparency/acquisition`` — 404 unless the flag is on."""
    if not public_page_enabled(env):
        return None
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    await ensure_schema(env)
    tz, tz_label = report_timezone(env)
    now = _now()
    from_ms, to_ms, from_day, to_day = parse_range({}, now, tz)
    try:
        report = await channel_report(env, from_ms, to_ms, False, now)
    except ReportIntegrityError:
        return Response(
            "<!doctype html><title>Temporarily unavailable</title>"
            "<p>The acquisition figures could not be verified right now.</p>",
            status=503,
            headers={"content-type": "text/html; charset=utf-8",
                     "cache-control": "no-store"})
    return Response(
        render_transparency_html(report, from_day, to_day, tz_label),
        status=200,
        headers={"content-type": "text/html; charset=utf-8",
                 "cache-control": "public, max-age=3600"})
