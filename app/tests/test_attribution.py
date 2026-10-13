#!/usr/bin/env python3
"""UTM capture, attribution and acquisition reporting (docs/attribution.md).

The real ``attribution.py`` runs against an in-memory SQLite standing in for
D1, with the handful of Worker primitives it borrows from entry.py replaced
by small fakes. The pagination claims are load-bearing enough to run rather
than read: a report must return every row, not the first page, and a count
mismatch must raise instead of rendering a smaller number.
"""

import ast
import asyncio
import hashlib
import importlib.util
import json
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
SRC = ROOT / "src"
sys.path.insert(0, str(SRC))

import attribution  # noqa: E402
from schema import SCHEMA_STATEMENTS  # noqa: E402

ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
RUNTIME_TEXT = (SRC / "app_runtime.py").read_text(encoding="utf-8")
WWW_WORKER = (REPO / "www" / "worker.js").read_text(encoding="utf-8")

# A Wednesday, so the default range is a clean earlier Monday–Sunday week.
NOW = int(datetime(2026, 9, 9, 12, 0, tzinfo=timezone.utc).timestamp() * 1000)
HOUR = 60 * 60 * 1000
DAY = 24 * HOUR


def _entry_source(name):
    tree = ast.parse(ENTRY_TEXT)
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name == name:
                return ast.get_source_segment(ENTRY_TEXT, node)
    raise AssertionError(name)


# The real page-navigation gate, lifted from entry.py, so the tests exercise
# the exact Accept/Sec-Fetch rule production uses.
_navigation_namespace = {}
exec(_entry_source("_is_page_navigation"), _navigation_namespace)
is_page_navigation = _navigation_namespace["_is_page_navigation"]


class FakeD1:
    def __init__(self):
        self.connection = sqlite3.connect(":memory:")
        self.connection.row_factory = sqlite3.Row
        for statement in SCHEMA_STATEMENTS:
            if (
                "attribution_" in statement
                or "CREATE TABLE IF NOT EXISTS users " in statement
                or "mirror_serve_counters" in statement
            ):
                self.connection.execute(statement)

    async def run(self, env, sql, *args):
        self.connection.execute(sql, args)

    async def first(self, env, sql, *args):
        row = self.connection.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.connection.execute(sql, args)]

    def count(self, table):
        return self.connection.execute(
            "SELECT COUNT(*) FROM " + table).fetchone()[0]


class Headers:
    def __init__(self, values=None):
        self._values = {k.lower(): v for k, v in (values or {}).items()}
        self.appended = []

    def get(self, name):
        return self._values.get(name.lower())

    def set(self, name, value):
        self._values[name.lower()] = value

    def append(self, name, value):
        self.appended.append((name, value))
        self._values.setdefault(name.lower(), value)


class Request:
    def __init__(self, url, method="GET", headers=None, body=None, cf=None):
        self.url = url
        self.method = method
        self.headers = Headers(headers)
        self.body = body
        self.cf = cf


class Response:
    def __init__(self, body="", status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = Headers(headers)


def navigation(url, cookie="", extra=None, ip="203.0.113.7", agent=None):
    headers = {
        "accept": "text/html,application/xhtml+xml",
        "sec-fetch-dest": "document",
        "sec-fetch-mode": "navigate",
        "user-agent": agent or (
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/120 Safari/537.36"),
        "cf-connecting-ip": ip,
    }
    if cookie:
        headers["cookie"] = "fm_attr=" + cookie
    headers.update(extra or {})
    return Request(url, "GET", headers)


def html_response():
    return Response("<!doctype html>", 200, {
        "content-type": "text/html; charset=utf-8"})


def set_cookies(response):
    return [v for k, v in response.headers.appended if k.lower() == "set-cookie"]


@pytest.fixture
def harness(monkeypatch):
    db = FakeD1()
    clock = {"now": NOW}
    env = SimpleNamespace()
    admin = {"ok": True}

    async def blind_index(env, value):
        return hashlib.sha256(str(value).encode("utf-8")).hexdigest()

    async def ensure_schema(env):
        return None

    async def bounded_json_request(request, *args):
        return json.loads(request.body or "null")

    async def check_admin(env, request):
        return admin["ok"]

    def cookie_value(request, name):
        header = request.headers.get("cookie") or ""
        for part in header.split(";"):
            key, sep, value = part.strip().partition("=")
            if sep and key == name:
                return value
        return ""

    def referrer_host(referer, self_host=""):
        host = str(urlparse(referer).hostname or "").lower()
        if host.startswith("www."):
            host = host[4:]
        if not host or host.endswith("forkmesh.com"):
            return ""
        return host

    def cookie_domain(env, request):
        host = str(urlparse(request.url).hostname or "")
        return ".forkmesh.com" if host.endswith("forkmesh.com") else ""

    def append_set_cookie(response, value):
        response.headers.append("Set-Cookie", value)
        return response

    def json_response(data, status=200, cache_control=None, **kwargs):
        return Response(json.dumps(data), status, {
            "content-type": "application/json; charset=utf-8"})

    fakes = {
        "d1_run": db.run, "d1_first": db.first, "d1_all": db.all,
        "Date": SimpleNamespace(now=lambda: clock["now"]),
        "blind_index": blind_index, "ensure_schema": ensure_schema,
        "_cookie_value": cookie_value,
        "method_name": lambda request: request.method,
        "_is_page_navigation": is_page_navigation,
        "_site_referrer_host": referrer_host,
        "_transient_client_address": (
            lambda request: request.headers.get("cf-connecting-ip") or ""),
        "world_request_country": (
            lambda request: (request.cf or {}).get("country", "")),
        "_portable_session_cookie_domain": cookie_domain,
        "_append_set_cookie": append_set_cookie,
        "_request_origin_allowed": lambda env, request: True,
        "bounded_json_request": bounded_json_request,
        "json_response": json_response,
        "Response": Response,
        "_check_admin_page_auth": check_admin,
        "_admin_shell": _admin_shell(clock),
        "urlparse": urlparse, "quote": quote,
    }
    for name, value in fakes.items():
        monkeypatch.setattr(attribution, name, value, raising=False)
    return SimpleNamespace(db=db, clock=clock, env=env, admin=admin)


def _admin_shell(clock):
    """The real admin shell, with no database behind its sidebar badges."""
    spec = importlib.util.spec_from_file_location(
        "admin_shell_under_test", ROOT / "src" / "admin_shell.py")
    shell = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shell)

    async def no_rows(env, sql, *args):
        return None

    shell._bind_runtime({"d1_first": no_rows,
                         "Date": SimpleNamespace(now=lambda: clock["now"])})
    return shell


def run(coro):
    return asyncio.run(coro)


async def capture(harness, request, response=None):
    response = response or html_response()
    url = urlparse(request.url)
    return await attribution.capture_page_view(
        harness.env, request, url, response, response.status)


# --- pagination correctness (section 9) ---------------------------------------

def test_report_returns_all_250_rows_not_the_page_size(harness):
    start = NOW - 5 * DAY
    for i in range(250):
        harness.db.connection.execute(
            "INSERT INTO attribution_sessions (id, first_seen_at, last_seen_at,"
            " first_utm_source, first_utm_campaign, first_landing_path) "
            "VALUES (?,?,?,?,?,?)",
            ("%08x-0000-4000-8000-%012x" % (i, i), start + i, start + i,
             "reddit" if i % 2 else "hn", "forever_launch", "/"))
    first_page = run(attribution.list_sessions(
        harness.env, start, NOW, False, None, attribution.PAGE_SIZE))
    assert len(first_page["items"]) == 100
    assert first_page["next_cursor"] is not None
    assert first_page["total_count"] == 250

    report = run(attribution.channel_report(harness.env, start, NOW, False, NOW))
    assert report["sessions_collected"] == 250
    assert report["totals"]["sessions"] == 250
    assert sum(row["sessions"] for row in report["rows"]) == 250
    assert report["row_count"] == 2


def test_events_and_clone_snapshots_page_past_100_rows(harness):
    start = NOW - 5 * DAY
    for i in range(130):
        session_id = "%08x-0000-4000-8000-%012x" % (i, i)
        harness.db.connection.execute(
            "INSERT INTO attribution_sessions (id, first_seen_at, last_seen_at,"
            " first_utm_source) VALUES (?,?,?,?)",
            (session_id, start + i, start + i, "hn"))
        harness.db.connection.execute(
            "INSERT INTO attribution_events (id, session_id, event_name,"
            " occurred_at) VALUES (?,?,?,?)",
            ("%08x-0000-4000-9000-%012x" % (i, i), session_id,
             "contribution_made", start + i + 1))
    first_events = run(attribution.list_events(
        harness.env, start, NOW, None, attribution.PAGE_SIZE))
    assert len(first_events["items"]) == 100
    assert first_events["next_cursor"] is not None
    assert first_events["total_count"] == 130

    report = run(attribution.channel_report(harness.env, start, NOW, False, NOW))
    assert report["events_collected"] == 130
    assert report["totals"]["contribution_made"] == 130

    day1 = int(datetime(2026, 9, 1, 0, 5, tzinfo=timezone.utc).timestamp() * 1000)

    def counters(clones, at):
        for i in range(150):
            harness.db.connection.execute(
                "INSERT INTO mirror_serve_counters (node_name, owner, repo,"
                " clones, website, updated_at) VALUES (?,?,?,?,0,?) "
                "ON CONFLICT(node_name, owner, repo) DO UPDATE SET "
                "clones=excluded.clones",
                ("mirror35", "owner%03d" % i, "repo", clones, at))

    counters(1, day1)
    assert run(attribution.snapshot_clone_counts(
        harness.env, day1))["repositories"] == 150
    counters(3, day1 + DAY)
    assert run(attribution.snapshot_clone_counts(
        harness.env, day1 + DAY))["repositories"] == 150

    first_snapshots = run(attribution.list_clone_snapshots(
        harness.env, "2026-09-02", "2026-09-02", None, attribution.PAGE_SIZE))
    assert len(first_snapshots["items"]) == 100
    assert first_snapshots["next_cursor"] is not None
    assert first_snapshots["total_count"] == 150
    clones = run(attribution.clone_count_report(
        harness.env, "2026-09-02", "2026-09-02"))
    assert clones["snapshot_rows"] == 150
    assert clones["total"] == 300
    assert len(clones["repositories"]) == 150


def test_count_mismatch_raises_instead_of_rendering(harness):
    async def short_reader(cursor, limit):
        # Claims 250 rows exist but only ever hands out 200.
        if cursor is None:
            return {"items": [{"n": i} for i in range(100)],
                    "next_cursor": "1.x", "total_count": 250}
        return {"items": [{"n": i} for i in range(100)],
                "next_cursor": None, "total_count": 250}

    with pytest.raises(attribution.ReportIntegrityError):
        run(attribution.collect_all(short_reader))

    # A row disappearing between pages is caught the same way.
    start = NOW - DAY
    for i in range(150):
        harness.db.connection.execute(
            "INSERT INTO attribution_sessions (id, first_seen_at, last_seen_at)"
            " VALUES (?,?,?)",
            ("%08x-0000-4000-8000-%012x" % (i, i), start + i, start + i))
    calls = {"n": 0}

    async def racing_reader(cursor, limit):
        page = await attribution.list_sessions(
            harness.env, start, NOW, False, cursor, limit)
        calls["n"] += 1
        if calls["n"] == 1:
            harness.db.connection.execute(
                "DELETE FROM attribution_sessions WHERE id=?",
                ("%08x-0000-4000-8000-%012x" % (149, 149),))
        return page

    with pytest.raises(attribution.ReportIntegrityError):
        run(attribution.collect_all(racing_reader))


def test_admin_report_surfaces_integrity_failure_as_an_error(harness, monkeypatch):
    async def broken(*args, **kwargs):
        raise attribution.ReportIntegrityError("collected 3 rows but 4 exist")

    monkeypatch.setattr(attribution, "channel_report", broken)
    request = Request("https://app.forkmesh.com/admin/acquisition")
    response = run(attribution.admin_dashboard_handler(harness.env, request))
    assert response.status == 500
    assert "Report not shown" in response.body
    assert "collected 3 rows but 4 exist" in response.body
    api = run(attribution.admin_report_handler(
        harness.env, Request("https://app.forkmesh.com/api/admin/acquisition/report")))
    assert api.status == 500
    assert json.loads(api.body)["error"] == "report_integrity"


def test_list_endpoints_return_items_cursor_and_total(harness):
    page = run(attribution.list_events(harness.env, 0, NOW))
    assert set(page) == {"items", "next_cursor", "total_count"}
    page = run(attribution.list_clone_snapshots(
        harness.env, "2026-01-01", "2026-12-31"))
    assert set(page) == {"items", "next_cursor", "total_count"}


# --- privacy gate (section 6) ---------------------------------------------------

@pytest.mark.parametrize("header", [{"dnt": "1"}, {"sec-gpc": "1"}])
def test_dnt_and_gpc_produce_no_cookie_and_no_rows(harness, header):
    request = navigation(
        "https://app.forkmesh.com/features?utm_source=reddit", extra=header)
    response = run(capture(harness, request))
    assert set_cookies(response) == []
    assert harness.db.count("attribution_sessions") == 0
    assert harness.db.count("attribution_events") == 0

    relay = Request(
        "https://app.forkmesh.com/api/attribution/capture", "POST",
        {**request.headers._values, "x-forkmesh-attribution-relay": "www"},
        json.dumps({"sessionId": attribution.new_id(), "minted": True,
                    "path": "/", "search": "?utm_source=reddit"}))
    result = run(attribution.relay_capture_handler(harness.env, relay))
    assert json.loads(result.body)["recorded"] is False
    assert harness.db.count("attribution_sessions") == 0


def test_skip_country_gets_the_page_with_nothing_recorded(harness):
    request = navigation("https://app.forkmesh.com/features?utm_source=hn")
    request.cf = {"country": "DE"}
    response = run(capture(harness, request))
    assert set_cookies(response) == []
    assert harness.db.count("attribution_sessions") == 0
    # The relay carries the marketing origin's edge country in a header.
    relay = Request(
        "https://app.forkmesh.com/api/attribution/capture", "POST",
        {**request.headers._values, "x-forkmesh-attribution-relay": "www",
         "x-forkmesh-attribution-country": "FR"},
        json.dumps({"sessionId": attribution.new_id(), "minted": True,
                    "path": "/", "search": ""}))
    result = run(attribution.relay_capture_handler(harness.env, relay))
    assert json.loads(result.body)["recorded"] is False
    assert harness.db.count("attribution_sessions") == 0


def test_no_ip_or_full_referrer_is_stored(harness):
    request = navigation(
        "https://app.forkmesh.com/?utm_source=hn",
        extra={"referer": "https://news.ycombinator.com/item?id=123&secret=1"})
    run(capture(harness, request))
    row = harness.db.first(None, "SELECT * FROM attribution_sessions")
    row = run(row)
    assert row["first_referrer_host"] == "news.ycombinator.com"
    assert "203.0.113.7" not in json.dumps(row)
    events = run(harness.db.all(None, "SELECT metadata FROM attribution_events"))
    assert "203.0.113.7" not in json.dumps(events)
    assert "secret" not in json.dumps(events)
    ip_rows = run(harness.db.all(None, "SELECT * FROM attribution_ip_hourly"))
    assert len(ip_rows) == 1
    assert "203.0.113.7" not in json.dumps(ip_rows)


# --- sanitisation and allowlist (section 2) --------------------------------------

def test_malicious_utm_source_is_sanitised_bucketed_and_escaped(harness):
    hostile = "<script>alert(1)</script>\x00\x01\x1f" + "A" * 500
    request = navigation(
        "https://app.forkmesh.com/features?utm_source=" + quote(hostile)
        + "&utm_campaign=forever_launch&utm_medium=%22%3E%3Cimg%20src%3Dx%3E")
    response = run(capture(harness, request))
    cookies = set_cookies(response)
    assert len(cookies) == 1
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert row["first_utm_source"] == "other"
    assert row["last_utm_source"] == "other"
    assert row["first_utm_campaign"] == "forever_launch"
    assert row["first_utm_medium"] == "other"
    event = run(harness.db.first(
        None, "SELECT metadata FROM attribution_events"))
    raw = json.loads(event["metadata"])["utm_other"]["source"]
    assert "<" not in raw and ">" not in raw
    assert all(ch >= " " for ch in raw)
    assert len(raw) <= 64
    assert raw.startswith("scriptalert1script")
    assert json.loads(event["metadata"])["utm_other"]["medium"] == "imgsrcx"

    # Even a poisoned channel label renders escaped.
    report = run(attribution.channel_report(
        harness.env, NOW - DAY, NOW + HOUR, False, NOW + HOUR))
    assert report["row_count"] == 1
    report["rows"][0]["utm_source"] = "<script>alert('x')</script>"
    page = attribution.render_dashboard_html(
        report, "2026-09-01", "2026-09-07", "UTC", False)
    assert "<script>alert" not in page
    assert "&lt;script&gt;alert" in page
    public = attribution.render_transparency_html(
        report, "2026-09-01", "2026-09-07", "UTC")
    assert "<script>alert" not in public


def test_sanitise_rules():
    assert attribution.sanitize_utm("  Reddit ") == "reddit"
    assert attribution.sanitize_utm("Forever_Launch-2026.v1") == "forever_launch-2026.v1"
    assert attribution.sanitize_utm("a" * 100) == "a" * 64
    assert attribution.sanitize_utm("\x00\x07") is None
    assert attribution.sanitize_utm("") is None
    assert attribution.sanitize_utm(None) is None
    assert attribution.parse_utms("utm_source=HN&utm_term=x%20y&other=1") == {
        "source": "hn", "medium": None, "campaign": None, "term": "xy",
        "content": None}


def test_allowlist_is_configuration(harness):
    harness.env.ATTRIBUTION_ALLOWED_SOURCES = "reddit, Lobsters"
    lists = attribution.allowlists(harness.env)
    assert lists["source"] == {"reddit", "lobsters"}
    assert lists["campaign"] == set(attribution.DEFAULT_ALLOWED_CAMPAIGNS)
    bucketed, raw = attribution.bucket_utms(
        {"source": "lobsters", "medium": "banner", "campaign": "forever_launch",
         "term": "anything-goes", "content": None}, lists)
    assert bucketed == {"source": "lobsters", "medium": "other",
                        "campaign": "forever_launch", "term": "anything-goes",
                        "content": None}
    assert raw == {"medium": "banner"}


# --- attribution semantics (sections 1, 2, 4) -------------------------------------

def test_first_touch_is_never_overwritten_and_last_touch_follows(harness):
    first = navigation("https://app.forkmesh.com/?utm_source=reddit"
                       "&utm_campaign=forever_launch")
    response = run(capture(harness, first))
    cookie = set_cookies(response)[0]
    session_id = cookie.split(";")[0].split("=", 1)[1]
    assert attribution.SESSION_ID_RE.match(session_id)
    assert "HttpOnly" in cookie and "Secure" in cookie
    assert "SameSite=Lax" in cookie and "Path=/" in cookie
    assert "Max-Age=7776000" in cookie and "Domain=forkmesh.com" in cookie

    harness.clock["now"] += 2 * HOUR
    second = navigation(
        "https://app.forkmesh.com/pricing?utm_source=hn&utm_campaign=search_to_clone_478",
        cookie=session_id)
    response = run(capture(harness, second))
    assert set_cookies(response) == []  # known session: no new cookie
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert row["first_utm_source"] == "reddit"
    assert row["first_utm_campaign"] == "forever_launch"
    assert row["last_utm_source"] == "hn"
    assert row["last_utm_campaign"] == "search_to_clone_478"
    assert row["first_landing_path"] == "/"
    assert row["last_seen_at"] == harness.clock["now"]
    assert harness.db.count("attribution_sessions") == 1
    assert harness.db.count("attribution_events") == 2

    # An untagged visit later populates nothing but keeps the session warm.
    harness.clock["now"] += HOUR
    run(capture(harness, navigation("https://app.forkmesh.com/docs",
                                    cookie=session_id)))
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert row["last_utm_source"] == "hn"
    assert row["last_seen_at"] == harness.clock["now"]


def test_cookie_lands_on_a_python_workers_response(harness):
    # A Python workers.Response wraps a JS response; its .headers is a dict
    # COPY, so a cookie appended there never reaches the browser. The live
    # headers hang off js_object (observed on wrangler 4.120 / Pyodide).
    class JsHeaders:
        def __init__(self):
            self.appended = []

        def append(self, name, value):
            self.appended.append((name, value))

    class WrappedResponse:
        def __init__(self):
            self.status = 200
            self.js_object = SimpleNamespace(headers=JsHeaders())

        @property
        def headers(self):
            return {"content-type": "text/html; charset=utf-8"}

    wrapped = WrappedResponse()
    request = navigation("https://app.forkmesh.com/signup?utm_source=hn")
    result = run(attribution.capture_page_view(
        harness.env, request, urlparse(request.url), wrapped, 200))
    assert result is wrapped
    appended = wrapped.js_object.headers.appended
    assert len(appended) == 1 and appended[0][0] == "Set-Cookie"
    assert appended[0][1].startswith("fm_attr=")
    assert harness.db.count("attribution_sessions") == 1
    # Deletion clears through the same path.
    session_id = appended[0][1].split(";")[0].split("=", 1)[1]
    cleared = WrappedResponse()
    monkeypatched = {"response": cleared}

    def json_response(data, status=200, **kwargs):
        return monkeypatched["response"]

    original = attribution.json_response
    attribution.json_response = json_response
    try:
        deletion = Request("https://app.forkmesh.com/api/attribution/session",
                           "DELETE", {"cookie": "fm_attr=" + session_id})
        run(attribution.delete_session_handler(
            harness.env, deletion, urlparse(deletion.url)))
    finally:
        attribution.json_response = original
    assert cleared.js_object.headers.appended[0][1].startswith("fm_attr=;")
    assert harness.db.count("attribution_sessions") == 0


def test_valid_cookie_without_a_row_is_adopted_not_replaced(harness):
    stale = attribution.new_id()
    response = run(capture(harness, navigation(
        "https://app.forkmesh.com/features", cookie=stale)))
    assert set_cookies(response) == []
    row = run(harness.db.first(None, "SELECT id FROM attribution_sessions"))
    assert row["id"] == stale


def test_concurrent_adoption_of_one_id_does_not_fail_the_capture(
        harness, monkeypatch):
    # Two requests of one page load both read "no row" before either inserts.
    stale = attribution.new_id()
    real_first = harness.db.first

    async def first_missing_session(env, sql, *args):
        if "FROM attribution_sessions WHERE id=?" in sql:
            return None
        return await real_first(env, sql, *args)

    monkeypatch.setattr(attribution, "d1_first", first_missing_session)
    for _ in range(2):
        relay = Request(
            "https://app.forkmesh.com/api/attribution/capture", "POST",
            {"accept": "text/html", "sec-fetch-dest": "document",
             "sec-fetch-mode": "navigate",
             "user-agent": "Mozilla/5.0 Firefox/130",
             "cf-connecting-ip": "198.51.100.4",
             "x-forkmesh-attribution-relay": "www"},
            json.dumps({"sessionId": stale, "minted": True,
                        "path": "/features", "host": "forkmesh.com"}))
        result = run(attribution.relay_capture_handler(harness.env, relay))
        assert json.loads(result.body)["sessionId"] == stale
    assert harness.db.count("attribution_sessions") == 1


def test_relay_capture_adopts_the_id_www_minted(harness):
    minted = attribution.new_id()
    relay = Request(
        "https://app.forkmesh.com/api/attribution/capture", "POST",
        {"accept": "text/html", "sec-fetch-dest": "document",
         "sec-fetch-mode": "navigate", "user-agent": "Mozilla/5.0 Firefox/130",
         "cf-connecting-ip": "198.51.100.4",
         "x-forkmesh-attribution-relay": "www",
         "x-forkmesh-attribution-country": "US",
         "referer": "https://old.reddit.com/r/git/"},
        json.dumps({"sessionId": minted, "minted": True, "path": "/features",
                    "search": "?utm_source=reddit&utm_medium=social",
                    "host": "forkmesh.com"}))
    result = run(attribution.relay_capture_handler(harness.env, relay))
    body = json.loads(result.body)
    assert body["recorded"] is True and body["sessionId"] == minted
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert row["id"] == minted
    assert row["first_utm_source"] == "reddit"
    assert row["first_utm_medium"] == "social"
    assert row["first_landing_path"] == "/features"
    assert row["first_referrer_host"] == "old.reddit.com"
    # A rejected id never creates anything.
    bad = Request(
        "https://app.forkmesh.com/api/attribution/capture", "POST",
        relay.headers._values,
        json.dumps({"sessionId": "not-a-uuid", "minted": True, "path": "/"}))
    assert run(attribution.relay_capture_handler(harness.env, bad)).status == 400
    assert harness.db.count("attribution_sessions") == 1


def test_signup_links_the_session_or_records_unknown(harness):
    response = run(capture(harness, navigation(
        "https://forkmesh.com/?utm_source=mastodon")))
    session_id = set_cookies(response)[0].split(";")[0].split("=", 1)[1]
    signup = Request("https://app.forkmesh.com/api/accounts/signup", "POST",
                     {"cookie": "fm_attr=" + session_id})
    harness.clock["now"] += 1000
    linked = run(attribution.link_user(harness.env, signup, "user-bi-1"))
    assert linked == session_id
    assert run(attribution.emit(harness.env, linked, "signup_completed"))
    harness.clock["now"] += 1000
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert row["user_id"] == "user-bi-1"
    assert row["first_utm_source"] == "mastodon"

    blocked = Request("https://app.forkmesh.com/api/accounts/signup", "POST")
    unknown = run(attribution.link_user(harness.env, blocked, "user-bi-2"))
    row = run(harness.db.first(
        None, "SELECT * FROM attribution_sessions WHERE id=?", unknown))
    assert row["first_utm_source"] == "unknown"
    assert row["user_id"] == "user-bi-2"
    assert row["is_bot"] == 0

    # Node-originated events join through the session linked at signup.
    assert run(attribution.emit_for_user(
        harness.env, "user-bi-1", "first_mirror_created", {"repo": "a/b"}))
    assert not run(attribution.emit_for_user(
        harness.env, "nobody", "contribution_made"))
    names = [r["event_name"] for r in run(harness.db.all(
        None, "SELECT event_name FROM attribution_events ORDER BY occurred_at, id"))]
    assert names == ["landing_view", "signup_completed", "first_mirror_created"]



def test_events_api_filters_by_event_name_to_find_one_account(harness):
    response = run(capture(harness, navigation(
        "https://forkmesh.com/?utm_source=mastodon")))
    session_id = set_cookies(response)[0].split(";")[0].split("=", 1)[1]
    signup = Request("https://app.forkmesh.com/api/accounts/signup", "POST",
                     {"cookie": "fm_attr=" + session_id})
    harness.clock["now"] += 1000
    run(attribution.link_user(harness.env, signup, "user-bi-1"))
    assert run(attribution.emit(harness.env, session_id, "signup_completed"))
    assert run(attribution.emit_for_user(
        harness.env, "user-bi-1", "first_mirror_created", {"repo": "a/b"}))
    harness.clock["now"] += 1000
    base = ("https://app.forkmesh.com/api/admin/acquisition/events"
            "?from=2026-09-09&to=2026-09-09")
    filtered = run(attribution.admin_list_handler(
        harness.env, Request(base + "&event=first_mirror_created"), "events"))
    page = json.loads(filtered.body)
    assert page["total_count"] == 1
    assert [(item["event_name"], item["session_id"])
            for item in page["items"]] == [("first_mirror_created", session_id)]
    assert json.loads(page["items"][0]["metadata"]) == {"repo": "a/b"}
    everything = run(attribution.admin_list_handler(
        harness.env, Request(base), "events"))
    assert json.loads(everything.body)["total_count"] == 3
    unknown = run(attribution.admin_list_handler(
        harness.env, Request(base + "&event=made_up"), "events"))
    assert unknown.status == 400
    assert json.loads(unknown.body) == {"error": "unknown_event"}

def test_event_dictionary_is_closed(harness):
    assert attribution.EVENT_NAMES == (
        "landing_view", "clone_url_copied", "signup_started",
        "signup_completed", "first_mirror_created", "contribution_made",
        "managed_waitlist_submitted", "discovery_call_accepted")
    with pytest.raises(ValueError):
        run(attribution.emit(harness.env, attribution.new_id(), "made_up"))
    beacon = Request(
        "https://app.forkmesh.com/api/attribution/events", "POST",
        {"origin": "https://app.forkmesh.com"},
        json.dumps({"event": "signup_completed"}))
    assert run(attribution.browser_event_handler(
        harness.env, beacon)).status == 400
    # Only clone_url_copied may come from a browser, and only with a session.
    no_session = Request(
        "https://app.forkmesh.com/api/attribution/events", "POST", {},
        json.dumps({"event": "clone_url_copied", "repo": "alice/repo"}))
    accepted = run(attribution.browser_event_handler(harness.env, no_session))
    assert accepted.status == 202
    assert harness.db.count("attribution_events") == 0


def test_clone_url_copied_beacon_joins_the_session(harness):
    response = run(capture(harness, navigation(
        "https://app.forkmesh.com/alice/repo?utm_source=x")))
    session_id = set_cookies(response)[0].split(";")[0].split("=", 1)[1]
    beacon = Request(
        "https://app.forkmesh.com/api/attribution/events", "POST",
        {"cookie": "fm_attr=" + session_id},
        json.dumps({"event": "clone_url_copied",
                    "repo": "alice/repo<script>"}))
    result = run(attribution.browser_event_handler(harness.env, beacon))
    assert json.loads(result.body)["recorded"] is True
    event = run(harness.db.first(
        None, "SELECT metadata FROM attribution_events WHERE event_name=?",
        "clone_url_copied"))
    assert json.loads(event["metadata"]) == {"repo": "alice/reposcript"}


def test_self_serve_deletion_clears_row_and_cookie(harness):
    response = run(capture(harness, navigation(
        "https://app.forkmesh.com/?utm_source=x")))
    session_id = set_cookies(response)[0].split(";")[0].split("=", 1)[1]
    request = Request("https://app.forkmesh.com/api/attribution/session",
                      "DELETE", {"cookie": "fm_attr=" + session_id})
    result = run(attribution.delete_session_handler(
        harness.env, request, urlparse(request.url)))
    assert json.loads(result.body)["deleted"] is True
    cleared = set_cookies(result)[0]
    assert cleared.startswith("fm_attr=;") and "Max-Age=0" in cleared
    assert harness.db.count("attribution_sessions") == 0
    assert harness.db.count("attribution_events") == 0


# --- bot filtering (section 3) ---------------------------------------------------

def test_known_crawler_gets_no_session_but_is_counted(harness):
    request = navigation("https://app.forkmesh.com/?utm_source=reddit",
                         agent="Mozilla/5.0 (compatible; Googlebot/2.1)")
    response = run(capture(harness, request))
    assert set_cookies(response) == []
    assert harness.db.count("attribution_sessions") == 0
    hits = run(harness.db.all(None, "SELECT * FROM attribution_filtered_hits"))
    assert hits == [{"day": "2026-09-09", "reason": "user_agent", "hits": 1}]
    harness.env.ATTRIBUTION_BOT_UA_PATTERNS = "definitely-not-a-bot"
    assert attribution.bot_reason_for_agent(
        "Googlebot", attribution.bot_agent_patterns(harness.env)) == ""


def test_bounce_rule_flags_then_lifts_and_is_excluded_by_default(harness):
    response = run(capture(harness, navigation(
        "https://app.forkmesh.com/?utm_source=hn")))
    session_id = set_cookies(response)[0].split(";")[0].split("=", 1)[1]
    # Too young to judge: nothing happens.
    run(attribution.maintenance(harness.env, NOW + 10 * 60 * 1000))
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert row["is_bot"] == 0
    # An hour later with no further request after two seconds: bot.
    run(attribution.maintenance(harness.env, NOW + 2 * HOUR))
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert (row["is_bot"], row["bot_reason"]) == (1, "bounce")
    report = run(attribution.channel_report(
        harness.env, NOW - DAY, NOW + 3 * HOUR, False, NOW + 3 * HOUR))
    assert report["totals"]["sessions"] == 0
    assert report["excluded_bot_sessions"] == 1
    shown = run(attribution.channel_report(
        harness.env, NOW - DAY, NOW + 3 * HOUR, True, NOW + 3 * HOUR))
    assert shown["totals"]["sessions"] == 1
    # The flag is provably wrong once the session is seen again.
    harness.clock["now"] = NOW + 3 * HOUR
    run(capture(harness, navigation("https://app.forkmesh.com/features",
                                    cookie=session_id)))
    row = run(harness.db.first(None, "SELECT * FROM attribution_sessions"))
    assert (row["is_bot"], row["bot_reason"]) == (0, "")
    # A session that navigated on within the visit is never a bounce.
    response = run(capture(harness, navigation("https://app.forkmesh.com/")))
    other = set_cookies(response)[0].split(";")[0].split("=", 1)[1]
    harness.clock["now"] += 5000
    run(capture(harness, navigation("https://app.forkmesh.com/pricing",
                                    cookie=other)))
    run(attribution.maintenance(harness.env, harness.clock["now"] + 2 * HOUR))
    row = run(harness.db.first(
        None, "SELECT is_bot FROM attribution_sessions WHERE id=?", other))
    assert row["is_bot"] == 0


def test_more_than_thirty_sessions_from_one_address_in_an_hour_are_bots(harness):
    for i in range(31):
        run(capture(harness, navigation(
            "https://app.forkmesh.com/?n=%d" % i, ip="192.0.2.9")))
        harness.clock["now"] += 1000
    rows = run(harness.db.all(
        None, "SELECT is_bot, bot_reason FROM attribution_sessions"))
    assert len(rows) == 31
    assert all(r["is_bot"] == 1 and r["bot_reason"] == "ip_rate" for r in rows)
    # Another address in the same hour is unaffected.
    run(capture(harness, navigation("https://app.forkmesh.com/", ip="192.0.2.10")))
    clean = run(harness.db.first(
        None, "SELECT is_bot FROM attribution_sessions WHERE is_bot=0"))
    assert clean is not None
    # Digest rows live 24 hours, then the sweep removes them.
    assert harness.db.count("attribution_ip_hourly") == 2
    run(attribution.maintenance(harness.env, harness.clock["now"] + 25 * HOUR))
    assert harness.db.count("attribution_ip_hourly") == 0


# --- the clone gap (section 5) -----------------------------------------------------

def test_anonymous_clone_and_read_paths_are_unchanged(harness):
    # A git smart-HTTP request: no browser Accept, no Sec-Fetch headers.
    for path, method in (
            ("/alice/repo/info/refs?service=git-upload-pack", "GET"),
            ("/alice/repo.git/git-upload-pack", "POST"),
            ("/api/repo/alice/repo/raw/main/README.md", "GET")):
        request = Request("https://app.forkmesh.com" + path, method, {
            "accept": "*/*", "user-agent": "git/2.45.0",
            "cf-connecting-ip": "203.0.113.9"})
        response = Response("pack", 200, {
            "content-type": "application/x-git-upload-pack-result"})
        same = run(attribution.capture_page_view(
            harness.env, request, urlparse(request.url), response, 200))
        assert same is response
        assert set_cookies(response) == []
    assert harness.db.count("attribution_sessions") == 0
    assert harness.db.count("attribution_filtered_hits") == 0

    # Attribution is bolted on after the response exists and only ever
    # appends a cookie: no redirect, no interstitial, no auth check.
    source = attribution.__file__
    text = Path(source).read_text(encoding="utf-8")
    assert "status=30" not in text.replace(
        "\"Admin login required.\", status=302", "")
    assert "_git_host" not in text and "_https_mirror_proxy" not in text
    for function_name in ("_https_mirror_route_advance",):
        assert "attribution" not in _entry_source(function_name)
    runtime_git = RUNTIME_TEXT.split("git_info = GIT_INFO_RE.match", 1)[1]
    runtime_git = runtime_git.split("if url.path == \"/\"", 1)[0]
    assert "attribution" not in runtime_git
    hook = RUNTIME_TEXT.index("_attribution.capture_page_view(")
    assert RUNTIME_TEXT.index("_apply_browser_security_headers(\n") < hook
    # The marketing Worker proxies git protocol paths before capture runs.
    # Its capture ships as its own change, so assert once it is present.
    if "captureAttribution(" in WWW_WORKER:
        assert WWW_WORKER.index("isGitProtocol(path) ||") < WWW_WORKER.index(
            "captureAttribution(request, env, ctx, await asset(")


def test_clone_counts_are_snapshot_deltas_never_summed_with_copies(harness):
    def counter(node, owner, repo, clones, at):
        harness.db.connection.execute(
            "INSERT INTO mirror_serve_counters (node_name, owner, repo, clones,"
            " website, updated_at) VALUES (?,?,?,?,0,?) "
            "ON CONFLICT(node_name, owner, repo) DO UPDATE SET clones=excluded.clones",
            (node, owner, repo, clones, at))

    day1 = int(datetime(2026, 9, 1, 0, 5, tzinfo=timezone.utc).timestamp() * 1000)
    counter("mirror35", "alice", "repo", 10, day1)
    counter("mirror36", "alice", "repo", 5, day1)
    run(attribution.snapshot_clone_counts(harness.env, day1))
    counter("mirror35", "alice", "repo", 14, day1 + DAY)
    counter("mirror35", "bob", "tool", 3, day1 + DAY)
    run(attribution.snapshot_clone_counts(harness.env, day1 + DAY))
    counter("mirror36", "alice", "repo", 9, day1 + 2 * DAY)
    run(attribution.snapshot_clone_counts(harness.env, day1 + 2 * DAY))

    clones = run(attribution.clone_count_report(
        harness.env, "2026-09-02", "2026-09-03"))
    assert clones["days"] == [("2026-09-02", 7), ("2026-09-03", 4)]
    assert clones["total"] == 11
    assert clones["repositories"][0] == {"owner": "alice", "repo": "repo",
                                         "clones": 8}
    # The report keeps copies and clones apart, in JSON and in CSV.
    start = day1 + DAY
    report = run(attribution.channel_report(
        harness.env, start, start + 2 * DAY, False, start + 2 * DAY))
    assert report["clones"]["total"] == 11
    assert report["totals"]["clone_url_copied"] == 0
    exported = attribution.report_csv(report)
    assert "unattributed_clones_total,11" in exported
    assert "clone_url_copied" in exported.splitlines()[0]
    # The limitation is written where the next person will read it.
    limitation = " ".join(attribution.clone_count_report.__doc__.split())
    assert "Never sum them" in limitation
    assert "never present the copy count as a clone count" in limitation


# --- dashboard and public page (sections 7, 8) ------------------------------------

def test_default_range_is_the_last_complete_monday_to_sunday_week():
    tz, label = attribution.report_timezone(SimpleNamespace())
    assert label == "UTC"
    start, end = attribution.default_range(NOW, tz)  # NOW is a Wednesday
    assert (start.isoformat(), end.isoformat()) == ("2026-08-31", "2026-09-06")
    sunday = int(datetime(2026, 9, 6, 9, 0, tzinfo=timezone.utc).timestamp() * 1000)
    start, end = attribution.default_range(sunday, tz)
    assert (start.isoformat(), end.isoformat()) == ("2026-08-24", "2026-08-30")
    tz, label = attribution.report_timezone(
        SimpleNamespace(ATTRIBUTION_TIMEZONE="+05:30"))
    assert label == "UTC+05:30"
    from_ms, to_ms, from_day, to_day = attribution.parse_range(
        {"from": ["2026-09-01"], "to": ["2026-09-30"]}, NOW, timezone.utc)
    assert (from_day, to_day) == ("2026-09-01", "2026-09-30")
    assert to_ms == NOW  # clamped to the report's own start


def test_dashboard_requires_admin_and_shows_totals_and_csv(harness):
    harness.admin["ok"] = False
    denied = run(attribution.admin_dashboard_handler(
        harness.env, Request("https://app.forkmesh.com/admin/acquisition")))
    assert denied.status == 302
    assert denied.headers.get("location").startswith("/login?next=")
    harness.admin["ok"] = True
    run(capture(harness, navigation(
        "https://app.forkmesh.com/?utm_source=reddit&utm_campaign=forever_launch")))
    harness.clock["now"] += 1000  # a report never includes its own instant
    request = Request(
        "https://app.forkmesh.com/admin/acquisition?from=2026-09-09&to=2026-09-09")
    page = run(attribution.admin_dashboard_handler(harness.env, request))
    assert page.status == 200
    assert "Showing 1 of 1 channel rows" in page.body
    assert "reddit" in page.body and "forever_launch" in page.body
    assert "Bots excluded" in page.body
    assert "estimate" in page.body
    assert "noindex" in page.body
    csv_request = Request(
        "https://app.forkmesh.com/admin/acquisition?from=2026-09-09&to=2026-09-09&format=csv")
    export = run(attribution.admin_dashboard_handler(harness.env, csv_request))
    assert export.headers.get("content-type").startswith("text/csv")
    assert export.body.splitlines()[0].startswith("utm_source,utm_campaign,sessions")
    assert "reddit,forever_launch,1" in export.body


def test_public_page_is_off_by_default_and_coarse_when_on(harness):
    request = Request("https://app.forkmesh.com/transparency/acquisition")
    assert run(attribution.transparency_page_handler(harness.env, request)) is None
    harness.env.PUBLIC_ACQUISITION_PAGE = "true"
    for i in range(23):
        harness.db.connection.execute(
            "INSERT INTO attribution_sessions (id, first_seen_at, last_seen_at,"
            " first_utm_source) VALUES (?,?,?,?)",
            ("%08x-0000-4000-8000-%012x" % (i, i), NOW - 5 * DAY,
             NOW - 5 * DAY, "reddit"))
    page = run(attribution.transparency_page_handler(harness.env, request))
    assert page.status == 200
    assert "Sessions by channel (rounded)" in page.body
    assert ">20<" in page.body and ">23<" not in page.body
    for forbidden in ("signup", "conversion", "revenue", "leaderboard"):
        assert forbidden not in page.body.lower().replace(
            "creating an account", "")
    assert "Clones cannot be attributed" in page.body
    assert attribution.round_coarse(4) == "under 10"
    assert attribution.round_coarse(1450) == "1500"


def test_discovery_call_is_marked_from_the_waitlist_session(harness):
    response = run(capture(harness, navigation(
        "https://app.forkmesh.com/pricing?utm_source=x")))
    session_id = set_cookies(response)[0].split(";")[0].split("=", 1)[1]
    waitlist = Request("https://app.forkmesh.com/api/waitlist", "POST",
                       {"cookie": "fm_attr=" + session_id})
    email_bi = hashlib.sha256(b"cto@example.com").hexdigest()
    assert run(attribution.emit_from_request(
        harness.env, waitlist, "managed_waitlist_submitted",
        {"source": "pricing", "email_bi": email_bi}))
    admin = Request(
        "https://app.forkmesh.com/api/admin/acquisition/discovery-call", "POST",
        {}, json.dumps({"email": "CTO@example.com"}))
    result = run(attribution.admin_discovery_call_handler(harness.env, admin))
    assert json.loads(result.body)["recorded"] is True
    names = sorted(r["event_name"] for r in run(harness.db.all(
        None, "SELECT event_name FROM attribution_events")))
    assert names == ["discovery_call_accepted", "landing_view",
                     "managed_waitlist_submitted"]
    missing = Request(
        "https://app.forkmesh.com/api/admin/acquisition/discovery-call", "POST",
        {}, json.dumps({"email": "nobody@example.com"}))
    assert run(attribution.admin_discovery_call_handler(
        harness.env, missing)).status == 404


# --- wiring ----------------------------------------------------------------------

def test_worker_wiring_emits_the_named_events_and_schedules_cron():
    for name in ("signup_started", "signup_completed",
                 "managed_waitlist_submitted", "contribution_made"):
        assert '"%s"' % name in ENTRY_TEXT
    catalog = (SRC / "catalog_routes.py").read_text(encoding="utf-8")
    assert '"first_mirror_created"' in catalog
    # The copy-clone-URL beacon ships as its own change; once the fragment
    # carries it, the composed bundle must too.
    fragment = (ROOT / "public" / "dashboard" / "js"
                / "08-repo-detail-network.js").read_text(encoding="utf-8")
    if "clone_url_copied" in fragment:
        dashboard = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
        assert 'event: "clone_url_copied"' in dashboard
        assert "/api/attribution/events" in dashboard
    assert '"attribution-maintenance"' in ENTRY_TEXT
    assert '"attribution-clone-snapshot"' in ENTRY_TEXT
    assert "_attribution = _LazyModule(\"attribution\")" in ENTRY_TEXT
    for route in ("/api/attribution/capture", "/api/attribution/events",
                  "/api/attribution/session", "/admin/acquisition",
                  "/transparency/acquisition",
                  "/api/admin/acquisition/discovery-call"):
        assert '"%s"' % route in RUNTIME_TEXT
    migration = (ROOT / "migrations" / "0174_attribution.sql").read_text(
        encoding="utf-8")
    assert "attribution_sessions" in migration and "attribution_events" in migration
    assert (ROOT / "migrations" / "down" / "0174_attribution.sql").is_file()
    assert (REPO / "docs" / "attribution.md").is_file()
