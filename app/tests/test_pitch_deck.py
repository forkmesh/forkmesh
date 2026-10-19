"""The investor deck a CRM contact opens: routing, the beacon and the page.

``pitch_deck_runtime.py`` is small enough to run its pure halves directly;
the wiring (entry export, dispatcher order, asset config, marketing proxy)
is asserted as source text like the rest of the route contracts.
"""

import asyncio
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import pitch_deck_runtime as deck  # noqa: E402

ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
URLS = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
APP_WRANGLER = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
WWW_WORKER = (REPO / "www" / "worker.js").read_text(encoding="utf-8")
WWW_WRANGLER = (REPO / "www" / "wrangler.toml").read_text(encoding="utf-8")
DECK_HTML = (ROOT / "public" / "pitch-deck" / "deck.html").read_text(encoding="utf-8")
CRM_WORKER = (REPO / "marketplace" / "packages" / "crm" / "content" / "worker"
              / "crm.py").read_text(encoding="utf-8")


def test_the_deck_link_is_routed_only_through_a_live_token():
    # The entrypoint exposes the two handlers lazily, like the hackathon's.
    assert '_pitch_deck = _LazyModule("pitch_deck_runtime")' in ENTRY
    assert 'pitch_deck_page_handler = _pitch_deck.export("page_handler")' in ENTRY
    assert 'pitch_deck_events_handler = _pitch_deck.export("events_handler")' in ENTRY
    assert '"PITCH_DECK_PAGE_RE", "PITCH_DECK_EVENTS_RE",' in ENTRY
    assert re.search(r'PITCH_DECK_PAGE_RE = re\.compile\(r"\^/pitch-deck/\(\[a-z0-9\]\{10,40\}\)/\?\$"\)', URLS)
    # The whole prefix is owned by the Worker: the document is an asset, but
    # no spelling other than a token ever reaches it.
    assert '"/pitch-deck",\n  "/pitch-deck/*",' in APP_WRANGLER
    page_block = APP_RUNTIME.split('if url.path == "/pitch-deck" or url.path.startswith("/pitch-deck/"):', 1)[1]
    assert "return await self._serve_not_found_page(url)" in page_block.split("\n\n", 1)[0]
    # It runs before the owner/repo shortcut would swallow the two-segment path.
    assert APP_RUNTIME.index('url.path.startswith("/pitch-deck/")') < APP_RUNTIME.index(
        "if looks_like_repo_route(url.path):")
    # The beacon is dispatched among the API routes.
    assert "pitch_deck_events = PITCH_DECK_EVENTS_RE.match(url.path)" in APP_RUNTIME
    # The apex proxies the document in place so the link the CRM hands out
    # (https://forkmesh.com/pitch-deck/<token>) is the one that opens.
    assert "const PITCH_DECK_DOCUMENTS = /^\\/pitch-deck\\/[a-z0-9]{10,40}\\/?$/;" in WWW_WORKER
    assert "if (PITCH_DECK_DOCUMENTS.test(path)) {\n      return proxyToApp(request, env);" in WWW_WORKER
    assert '"/pitch-deck/*",' in WWW_WRANGLER
    assert 'DECK_SITE_ORIGIN = "https://forkmesh.com"' in CRM_WORKER


def test_the_document_is_stamped_noindex_and_reports_to_its_own_token():
    assert deck.CONFIG_MARKER in DECK_HTML
    assert '<meta name="robots" content="noindex, nofollow" />' in DECK_HTML
    assert "/api/pitch-deck/${encodeURIComponent(token)}/events" in DECK_HTML
    assert "navigator.sendBeacon(" in DECK_HTML and "keepalive: true" in DECK_HTML
    # Fourteen pages, numbered the way the CRM labels them.
    pages = re.findall(r'<section class="slide[^"]*" id="p(\d+)" data-page="(\d+)"', DECK_HTML)
    assert [int(a) for a, _ in pages] == list(range(1, 15))
    assert all(a == b for a, b in pages)
    titles = re.search(r"DECK_PAGE_TITLES = \((.*?)\n\)", CRM_WORKER, re.S).group(1)
    assert len(re.findall(r'"[^"]+"', titles)) == 14
    # The page's CSP admits nothing external: no fonts, scripts or images
    # from other origins, and beacons only to this one.
    assert "connect-src 'self'" in deck.PAGE_CSP and "default-src 'none'" in deck.PAGE_CSP
    assert "manifest-src 'self'" in deck.PAGE_CSP
    assert "https://fonts." not in DECK_HTML and "cdn." not in DECK_HTML


def test_deck_config_carries_only_a_first_name_and_the_token():
    link = {"token": "abcdefghij12", "deck": "seed-2026"}
    config = deck.deck_config(link, "Ada Lovelace", 1700000000000)
    assert config == {"token": "abcdefghij12", "deck": "seed-2026",
                      "viewer": "Ada", "servedAt": 1700000000000}
    assert deck.deck_config(link, "", 5)["viewer"] == ""
    assert len(deck.deck_config(link, "x" * 80, 5)["viewer"]) == 40


def test_beacons_are_validated_capped_and_merged_per_page():
    now = 1700000000000
    parsed, error = deck.parse_events(json.dumps({
        "session": "AbC123def456", "startedAt": now - 5000,
        "events": [{"page": 3, "seconds": 12}, {"page": 3, "seconds": 8},
                   {"page": 1, "seconds": 9999}, {"page": 7, "seconds": -4}],
    }), now)
    assert error == ""
    assert parsed["session"] == "abc123def456"
    assert parsed["startedAt"] == now - 5000
    assert parsed["pages"] == [(1, deck.MAX_EVENT_SECONDS), (3, 20), (7, 0)]
    # A start in the future or missing falls back to now.
    assert deck.parse_events(json.dumps({"session": "abcdefgh", "events": []}), now)[0]["startedAt"] == now
    assert deck.parse_events(json.dumps({"session": "abcdefgh", "startedAt": now + 1, "events": []}), now)[0]["startedAt"] == now
    for body, reason in (
        ("not json", "invalid_json"),
        ("[]", "invalid_json"),
        (json.dumps({"session": "short", "events": []}), "invalid_session"),
        (json.dumps({"session": "abcdefgh", "events": "x"}), "invalid_events"),
        (json.dumps({"session": "abcdefgh", "events": [{"page": 0, "seconds": 1}]}), "invalid_events"),
        (json.dumps({"session": "abcdefgh", "events": [{"page": 99, "seconds": 1}]}), "invalid_events"),
        (json.dumps({"session": "abcdefgh", "events": [{"page": "x"}]}), "invalid_events"),
        (json.dumps({"session": "abcdefgh", "events": [{"page": 1}] * 61}), "invalid_events"),
    ):
        assert deck.parse_events(body, now) == (None, reason), body


class _Env:
    """Just enough of the Worker for the two handlers: D1 rows and an asset."""

    def __init__(self, link=None, html=None):
        self.link = link
        self.html = html if html is not None else DECK_HTML
        self.runs = []
        self.ASSETS = self

    async def fetch(self, url):
        self.fetched = url
        html = self.html

        class _Resp:
            async def text(self_inner):
                return html
        return _Resp()


class _Request:
    def __init__(self, method="GET", url="https://app.forkmesh.com/pitch-deck/abcdefghij12",
                 body="", headers=None):
        self.method = method
        self.url = url
        self._body = body
        self.headers = headers or {}

    async def text(self):
        return self._body


class _Response:
    def __init__(self, body, status=200, headers=None):
        self.body, self.status, self.headers = body, status, headers or {}


def _bind(env):
    # The handlers pass their env first, so the stubs answer for whichever
    # environment a test hands them (the module binds these once).
    async def d1_first(current, sql, *args):
        return current.link if current.link and "FROM crm_deck_links" in sql else None

    async def d1_run(current, sql, *args):
        current.runs.append((sql, args))

    async def decrypt_row(_env, value):
        return json.loads(value)

    def json_response(data, status=200, cache_control=None, extra_headers=None):
        return _Response(json.dumps(data), status, dict(extra_headers or {}))

    deck._bind_runtime({
        "d1_first": d1_first, "d1_run": d1_run, "decrypt_row": decrypt_row,
        "json_response": json_response, "Response": _Response,
        "method_name": lambda request: request.method,
        "Date": type("Date", (), {"now": staticmethod(lambda: 1700000000000)}),
    })


def test_the_page_is_served_for_a_live_link_and_gone_for_a_revoked_one():
    asyncio.run(_page_flow())


async def _page_flow():
    live = {"token": "abcdefghij12", "org_bi": "org:acme", "contact_id": "c" * 32,
            "deck": "seed-2026", "revoked_at": 0,
            "data": json.dumps({"viewer": "Ada Lovelace", "email": "ada@example.com"})}
    env = _Env(live)
    _bind(env)
    response = await deck.page_handler(env, _Request(), "abcdefghij12")
    assert response.status == 200
    assert env.fetched == "https://app.forkmesh.com/pitch-deck/deck.html"
    assert response.headers["cache-control"] == "private, no-store"
    assert response.headers["x-robots-tag"] == "noindex, nofollow"
    assert response.headers["content-security-policy"] == deck.PAGE_CSP
    stamped = re.search(r'<script id="deck-config" type="application/json">(.*?)</script>',
                        response.body).group(1)
    assert json.loads(stamped) == {"token": "abcdefghij12", "deck": "seed-2026",
                                   "viewer": "Ada", "servedAt": 1700000000000}
    # Unknown token: None, so the dispatcher serves its 404 page.
    assert await deck.page_handler(_Env(None), _Request(), "abcdefghij12") is None
    assert await deck.page_handler(env, _Request(), "../etc") is None
    revoked = _Env(dict(live, revoked_at=5))
    gone = await deck.page_handler(revoked, _Request(), "abcdefghij12")
    assert gone.status == 410 and "no longer active" in gone.body
    assert (await deck.page_handler(env, _Request("POST"), "abcdefghij12")).status == 405


def test_a_beacon_upserts_the_session_and_its_pages():
    asyncio.run(_beacon_flow())


async def _beacon_flow():
    live = {"token": "abcdefghij12", "org_bi": "org:acme", "contact_id": "c" * 32,
            "deck": "seed-2026", "revoked_at": 0, "data": "{}"}
    env = _Env(live)
    _bind(env)
    body = json.dumps({"session": "visit0001", "startedAt": 1699999990000,
                       "events": [{"page": 1, "seconds": 5}, {"page": 2, "seconds": 30}]})
    request = _Request("POST", "https://forkmesh.com/api/pitch-deck/abcdefghij12/events",
                       body, {"cf-ipcountry": "de", "user-agent": "UA " * 100})
    response = await deck.events_handler(env, request, "abcdefghij12")
    assert response.status == 200 and json.loads(response.body) == {"ok": True}
    sql = [run[0] for run in env.runs]
    assert sql[0].startswith("INSERT INTO crm_deck_sessions(")
    assert "ON CONFLICT(session_id) DO UPDATE" in sql[0]
    session_args = env.runs[0][1]
    assert session_args[:5] == ("visit0001", "abcdefghij12", 1699999990000, 1700000000000, 35)
    assert session_args[6] == "DE" and len(session_args[7]) == deck.MAX_AGENT_CHARS
    assert [run[1][2:4] for run in env.runs[1:3]] == [(1, 5), (2, 30)]
    assert sql[3].startswith("UPDATE crm_deck_sessions SET pages=")
    # Revoked or unknown links take nothing; bad bodies are rejected.
    revoked = _Env(dict(live, revoked_at=5))
    assert (await deck.events_handler(revoked, request, "abcdefghij12")).status == 404
    bad = _Request("POST", body="{", headers={})
    assert (await deck.events_handler(env, bad, "abcdefghij12")).status == 400
    huge = _Request("POST", body="x" * (deck.MAX_BODY_BYTES + 1))
    assert (await deck.events_handler(env, huge, "abcdefghij12")).status == 413
    assert (await deck.events_handler(env, _Request("GET"), "abcdefghij12")).status == 405


def test_the_first_beacon_of_a_visit_pings_whoever_sent_the_link():
    asyncio.run(_open_ping_flow())


async def _open_ping_flow():
    live = {"token": "abcdefghij12", "org_bi": "org:acme", "contact_id": "c" * 32,
            "deck": "seed-2026", "revoked_at": 0, "created_by_name": "Jett",
            "data": json.dumps({"viewer": "Ada Lovelace"})}
    env = _Env(live)
    _bind(env)
    pings = []

    async def enqueue_notification(current, recipient, kind, title, **kwargs):
        pings.append((recipient, kind, title, kwargs))
        return True

    deck._bind_runtime({"enqueue_notification": enqueue_notification})
    seen = set()

    async def d1_first(current, sql, *args):
        if "FROM crm_deck_links" in sql:
            return current.link
        return {"session_id": args[0]} if args[0] in seen else None

    original = deck.d1_first
    deck.d1_first = d1_first
    try:
        await _ping_once_per_visit(env, pings, seen)
    finally:
        deck.d1_first = original
        del deck.enqueue_notification


async def _ping_once_per_visit(env, pings, seen):
    body = json.dumps({"session": "visit0002", "events": [{"page": 1, "seconds": 0}]})
    request = _Request("POST", "https://forkmesh.com/api/pitch-deck/abcdefghij12/events",
                       body, {"cf-ipcountry": "fr"})
    assert (await deck.events_handler(env, request, "abcdefghij12")).status == 200
    assert len(pings) == 1
    recipient, kind, title, extra = pings[0]
    assert (recipient, kind) == ("jett", "plugin_activity")
    assert title == "Ada Lovelace opened the investor deck"
    assert "from FR" in extra["body"]
    assert extra["dedupe"] == "deck-open:abcdefghij12:visit0002"
    assert extra["source"] == "plugin" and extra["href"] == "/dashboard/crm"
    seen.add("visit0002")
    assert (await deck.events_handler(env, request, "abcdefghij12")).status == 200
    assert len(pings) == 1
    assert deck.open_ping("", "")[0] == "Your contact opened the investor deck"
