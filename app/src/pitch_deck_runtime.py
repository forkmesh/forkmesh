"""The investor pitch deck a CRM contact opens, and its view analytics.

Every ``/pitch-deck/<token>`` link is minted by the CRM package for exactly
one contact (``crm.py`` owns ``crm_deck_links``, ``crm_deck_sessions`` and
``crm_deck_page_views``; it also reads them back for the "who viewed what"
panel). This module answers only the two routes the contact's browser uses:

* ``GET /pitch-deck/<token>`` serves the deck document (``public/pitch-deck/
  deck.html``) with the link's viewer name and token stamped into it. The
  document is an ordinary asset, but the Worker owns its whole path prefix
  (``run_worker_first`` in wrangler.toml), so the only way to read it is
  through a live token.
* ``POST /api/pitch-deck/<token>/events`` is the beacon the deck sends when
  the reader changes page, every so often while a page stays open, and when
  the tab closes. Seconds accumulate per (session, page); nothing about the
  reader is taken beyond what the request already carries (country, agent).

Loaded lazily from entry.py like hackathon_runtime; ``_bind_runtime``
supplies the shared Worker/D1 primitives on first use.
"""

import json
import re
from urllib.parse import urlparse


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this module."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


TOKEN_RE = re.compile(r"^[a-z0-9]{10,40}$")
SESSION_ID_RE = re.compile(r"^[a-z0-9]{8,40}$")
DECK_ASSET = "pitch-deck/deck.html"
CONFIG_MARKER = '<script id="deck-config" type="application/json">{}</script>'
MAX_BODY_BYTES = 8 * 1024
MAX_EVENTS = 60
MAX_PAGES = 40
# One beacon reports at most ten minutes on a page: a tab left open overnight
# is not a reader, and the cap keeps a forged beacon from inflating a total.
MAX_EVENT_SECONDS = 600
MAX_SESSION_SECONDS = 6 * 3600
MAX_AGENT_CHARS = 160
PAGE_CSP = (
    "default-src 'none'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'none'; img-src 'self' data:; font-src 'self'; "
    "style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
    "connect-src 'self'"
)


def _now():
    return int(Date.now())


def deck_config(link, viewer, now):
    """What the document learns about its reader: the token it reports to,
    a first name for the "prepared for" line, and when it was served."""
    first = str(viewer or "").strip().split(" ")[0][:40] if viewer else ""
    return {
        "token": str(link.get("token") or ""),
        "deck": str(link.get("deck") or "seed-2026"),
        "viewer": first,
        "servedAt": int(now),
    }


def parse_events(raw, now):
    """Validate one beacon body into (session id, page events, agent).

    Returns ``(None, "reason")`` for anything malformed. Pages are 1-based
    slide numbers; seconds are whole seconds spent on that slide since the
    previous beacon. Duplicate pages in one beacon are summed.
    """
    try:
        data = json.loads(raw or "{}")
    except (TypeError, ValueError):
        return None, "invalid_json"
    if not isinstance(data, dict):
        return None, "invalid_json"
    session = str(data.get("session") or "").strip().lower()
    if not SESSION_ID_RE.fullmatch(session):
        return None, "invalid_session"
    events = data.get("events")
    if not isinstance(events, list) or len(events) > MAX_EVENTS:
        return None, "invalid_events"
    merged = {}
    for item in events:
        if not isinstance(item, dict):
            return None, "invalid_events"
        try:
            page = int(item.get("page"))
            seconds = int(item.get("seconds") or 0)
        except (TypeError, ValueError):
            return None, "invalid_events"
        if page < 1 or page > MAX_PAGES:
            return None, "invalid_events"
        seconds = max(0, min(MAX_EVENT_SECONDS, seconds))
        merged[page] = min(MAX_EVENT_SECONDS, merged.get(page, 0) + seconds)
    started = data.get("startedAt")
    try:
        started = int(started)
    except (TypeError, ValueError):
        started = now
    if started <= 0 or started > now:
        started = now
    return {
        "session": session,
        "pages": sorted(merged.items()),
        "startedAt": started,
    }, ""


async def _link(env, token):
    if not TOKEN_RE.fullmatch(str(token or "")):
        return None
    try:
        return await d1_first(
            env,
            "SELECT token, org_bi, contact_id, deck, created_by_name, "
            "revoked_at, data FROM crm_deck_links WHERE token=?",
            token,
        )
    except Exception:
        # The CRM package creates these tables when an organization installs
        # it; before that there is nothing to look up.
        return None


def _inactive_page():
    return (
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<meta name=\"robots\" content=\"noindex\"><title>ForkMesh</title>"
        "<style>body{margin:0;min-height:100vh;display:grid;place-items:center;"
        "background:#0b1020;color:#e6e9f2;font:16px/1.5 system-ui,sans-serif}"
        "main{max-width:32rem;padding:2rem;text-align:center}"
        "a{color:#a78bfa}</style></head><body><main>"
        "<p style=\"letter-spacing:.2em;font-size:.75rem;color:#8b93b0\">FORKMESH</p>"
        "<h1 style=\"font-size:1.5rem\">This deck link is no longer active.</h1>"
        "<p>Ask the person who sent it for a fresh link, or write to "
        "<a href=\"mailto:founders@forkmesh.com\">founders@forkmesh.com</a>.</p>"
        "</main></body></html>"
    )


async def _viewer(env, link):
    try:
        sealed = await decrypt_row(env, link.get("data"))
    except Exception:
        return ""
    return str(sealed.get("viewer") or "") if isinstance(sealed, dict) else ""


def open_ping(viewer, country):
    who = str(viewer or "").strip()[:80] or "Your contact"
    where = (" from " + country) if country else ""
    return (
        "%s opened the investor deck" % who,
        "First visit%s on the deck link you sent. Every page they read shows "
        "on their record." % where,
    )


async def _ping_open(env, link, session, viewer, country):
    recipient = str(link.get("created_by_name") or "").strip().lower()
    notify = globals().get("enqueue_notification")
    if not recipient or not callable(notify):
        return False
    title, body = open_ping(viewer, country)
    try:
        return await notify(
            env, recipient, "plugin_activity", title, body=body,
            href="/dashboard/crm", source="plugin",
            dedupe="deck-open:" + str(link.get("token") or "") + ":" + session)
    except Exception:
        return False


async def page_handler(env, request, token):
    """``GET /pitch-deck/<token>`` -- the deck, or None when no link exists."""
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    link = await _link(env, token)
    if not link:
        return None
    headers = {
        "content-type": "text/html; charset=utf-8",
        "cache-control": "private, no-store",
        "x-robots-tag": "noindex, nofollow",
        "referrer-policy": "no-referrer",
        "content-security-policy": PAGE_CSP,
        "x-content-type-options": "nosniff",
        "x-frame-options": "DENY",
    }
    if int(link.get("revoked_at") or 0) > 0:
        return Response(_inactive_page(), status=410, headers=headers)
    viewer = await _viewer(env, link)
    parts = urlparse(str(request.url))
    base = parts.scheme + "://" + parts.netloc + "/"
    try:
        resp = await env.ASSETS.fetch(base + DECK_ASSET)
        body = await resp.text()
    except Exception:
        body = ""
    if CONFIG_MARKER not in body:
        return json_response({"error": "deck_unavailable"}, status=503)
    config = json.dumps(deck_config(link, viewer, _now()))
    # "</" cannot appear inside the JSON block without closing the script.
    config = config.replace("</", "<\\/")
    body = body.replace(
        CONFIG_MARKER,
        '<script id="deck-config" type="application/json">' + config + "</script>",
        1,
    )
    return Response(body, status=200, headers=headers)


async def events_handler(env, request, token):
    """``POST /api/pitch-deck/<token>/events`` -- record one view beacon."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    link = await _link(env, token)
    if not link or int(link.get("revoked_at") or 0) > 0:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    if len(raw) > MAX_BODY_BYTES:
        return json_response({"error": "payload_too_large"}, status=413,
                             cache_control="no-store")
    now = _now()
    parsed, error = parse_events(raw, now)
    if error:
        return json_response({"error": error}, status=400,
                             cache_control="no-store")
    total = sum(seconds for _, seconds in parsed["pages"])
    country = ""
    agent = ""
    try:
        country = str(request.headers.get("cf-ipcountry") or "")[:2].upper()
        agent = str(request.headers.get("user-agent") or "")[:MAX_AGENT_CHARS]
    except Exception:
        pass
    seen = await d1_first(
        env, "SELECT session_id FROM crm_deck_sessions WHERE session_id=?",
        parsed["session"])
    await d1_run(
        env,
        "INSERT INTO crm_deck_sessions(session_id,token,started_at,last_at,"
        "seconds,pages,country,agent) VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(session_id) DO UPDATE SET last_at=excluded.last_at,"
        "seconds=MIN(?,crm_deck_sessions.seconds+excluded.seconds)",
        parsed["session"], token, parsed["startedAt"], now, total, 0, country,
        agent, MAX_SESSION_SECONDS,
    )
    for page, seconds in parsed["pages"]:
        await d1_run(
            env,
            "INSERT INTO crm_deck_page_views(session_id,token,page,seconds,"
            "first_at,last_at) VALUES(?,?,?,?,?,?) "
            "ON CONFLICT(session_id,page) DO UPDATE SET "
            "seconds=MIN(?,crm_deck_page_views.seconds+excluded.seconds),"
            "last_at=excluded.last_at",
            parsed["session"], token, page, seconds, now, now,
            MAX_SESSION_SECONDS,
        )
    if parsed["pages"]:
        await d1_run(
            env,
            "UPDATE crm_deck_sessions SET pages=(SELECT COUNT(*) FROM "
            "crm_deck_page_views WHERE session_id=?) WHERE session_id=?",
            parsed["session"], parsed["session"],
        )
    if not seen:
        await _ping_open(env, link, parsed["session"],
                         await _viewer(env, link), country)
    return json_response({"ok": True}, status=200, cache_control="no-store")
