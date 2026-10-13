#!/usr/bin/env python3
"""Inbound website-referral board: what counts, what never does, what shows."""

import ast
import asyncio
import json
import re
import shutil
import sqlite3
import subprocess
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qsl, urlencode, urlparse, urlunparse

import pytest
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[1]
WWW_PUBLIC = ROOT.parent / "www" / "public"
ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
MIGRATION = ROOT / "migrations" / "0084_site_referrers.sql"
URL_MIGRATION = ROOT / "migrations" / "0095_site_referrer_urls.sql"
REFERRALS_JS = (
    ROOT / "public" / "dashboard" / "js" / "09-referrals.js"
).read_text(encoding="utf-8")
REFERRALS_HTML = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "referrals.html"
).read_text(encoding="utf-8")
PRIVACY = (WWW_PUBLIC / "privacy.html").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()

BROWSER_UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
              "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126 Safari/537.36")
NAV_HEADERS = {
    "accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "sec-fetch-dest": "document",
    "sec-fetch-mode": "navigate",
    "user-agent": BROWSER_UA,
}


def _top_level_node(name):
    for node in ast.parse(ENTRY, filename=str(ENTRY_PATH)).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name == name:
                return node
    raise AssertionError(f"{name} not found")


def _entry_constant(name):
    for node in ast.parse(ENTRY, filename=str(ENTRY_PATH)).body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == name:
                    value = node.value
                    # frozenset({...}) / re.compile("...") wrap their literal.
                    if isinstance(value, ast.Call) and value.args:
                        literal = ast.literal_eval(value.args[0])
                        return (frozenset(literal)
                                if isinstance(value.func, ast.Name)
                                and value.func.id == "frozenset"
                                else literal)
                    return ast.literal_eval(value)
    raise AssertionError(f"{name} not found")


def _load_function(name, namespace):
    module = ast.fix_missing_locations(
        ast.Module(body=[_top_level_node(name)], type_ignores=[]))
    exec(compile(module, str(ENTRY_PATH), "exec"), namespace)
    return namespace[name]


def _db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(MIGRATION.read_text(encoding="utf-8"))
    db.executescript(URL_MIGRATION.read_text(encoding="utf-8"))
    return db


def _namespace(db):
    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def ensure_schema(_env):
        return None

    cache_puts = []

    async def edge_cache_match(_key):
        return None

    async def edge_cache_put(key, resp):
        cache_puts.append((key, resp))

    def json_response(data, status=200, cache_seconds=None,
                      cache_control=None, extra_headers=None):
        return SimpleNamespace(
            data=data, status=status, cache_seconds=cache_seconds)

    namespace = {
        "d1_run": d1_run,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "ensure_schema": ensure_schema,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "json_response": json_response,
        "urlparse": urlparse,
        "parse_qsl": parse_qsl,
        "urlencode": urlencode,
        "urlunparse": urlunparse,
        "re": re,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "method_name": lambda request: request.method,
        "Date": SimpleNamespace(now=lambda: 1_720_000_000_000),
        "REFERRAL_PREVIEW_AGENTS": _entry_constant("REFERRAL_PREVIEW_AGENTS"),
    }
    for name in ("MAX_SITE_REFERRER_HOST", "MAX_SITE_REFERRER_URL",
                 "SITE_REFERRER_REDACTED_QUERY_KEYS",
                 "SITE_REFERRER_SELF_HOSTS",
                 "SITE_REFERRER_LEADERBOARD_CACHE_KEY",
                 "SITE_REFERRER_LEADERBOARD_TTL",
                 "SITE_REFERRER_LEADERBOARD_LIMIT",
                 "SITE_REFERRER_RETAIN_HOSTS"):
        namespace[name] = _entry_constant(name)
    namespace["_SITE_REFERRER_HOST_RE"] = re.compile(
        _entry_constant("_SITE_REFERRER_HOST_RE"))
    for name in ("_site_referrer_host", "_site_referrer_url",
                 "_is_page_navigation",
                 "_is_link_preview_agent", "record_site_referral",
                 "site_referrer_leaderboard", "prune_site_referrers"):
        _load_function(name, namespace)
    namespace["_cache_puts"] = cache_puts
    return namespace


def _request(referer, headers=None, method="GET"):
    merged = dict(NAV_HEADERS)
    merged.update(headers or {})
    if referer is not None:
        merged["referer"] = referer
    return SimpleNamespace(
        method=method,
        headers=SimpleNamespace(get=lambda key: merged.get(key)))


def _url(host="forkmesh.com"):
    return urlparse("https://%s/pricing" % host)


def _referrals_js_slice(start, end):
    return REFERRALS_JS.split(start, 1)[1].split(end, 1)[0]


# The row builder and its URL guard, lifted out of the dashboard bundle and
# run for real: the guard is the thing standing between an attacker-controlled
# Referer and an href, so asserting on its source text is not enough.
_ROW_HARNESS = r'''
const source = process.argv[2];
const rows = JSON.parse(process.argv[3]);
const escapeHtml = (value) => String(value ?? "")
  .replace(/&/g, "&amp;")
  .replace(/</g, "&lt;")
  .replace(/>/g, "&gt;")
  .replace(/"/g, "&quot;")
  .replace(/'/g, "&#39;");
const referralPlural = (value, word) => `${Number(value) || 0} ${word}s`;
const build = new Function(
  "escapeHtml",
  "referralPlural",
  source + "\nreturn { referralSiteUrl, referralSiteRowHtml };",
);
const api = build(escapeHtml, referralPlural);
process.stdout.write(JSON.stringify(rows.map((row, index) => ({
  href: api.referralSiteUrl(row),
  html: api.referralSiteRowHtml(row, index),
}))));
'''


def _render_site_rows(rows):
    source = _referrals_js_slice(
        "  function referralSiteUrl", "  function renderReferralSites")
    completed = subprocess.run(
        ["node", "--input-type=module", "-", "function referralSiteUrl"
         + source, json.dumps(rows)],
        input=_ROW_HARNESS, text=True, capture_output=True, check=True)
    return json.loads(completed.stdout)


def _seed(db, host, visits, last_ts=0, last_url=""):
    db.execute(
        "INSERT INTO site_referrers "
        "(host, visits, first_ts, last_ts, last_url) "
        "VALUES (?,?,?,?,?)",
        (host, visits, last_ts, last_ts, last_url))
    db.commit()


def test_migration_adds_one_bounded_latest_url_to_the_aggregate_host_row():
    db = _db()
    columns = {
        row[1] for row in db.execute(
            "PRAGMA table_info(site_referrers)").fetchall()
    }
    assert columns == {"host", "visits", "first_ts", "last_ts", "last_url"}
    assert "CREATE TABLE IF NOT EXISTS site_referrers" in SCHEMA
    assert "last_url TEXT NOT NULL DEFAULT ''" in SCHEMA
    # Privacy contract: aggregate destination-independent reach, nothing that
    # identifies the visitor who followed the link.
    for forbidden in ("ip", "user_agent", "session", "landing_path"):
        assert forbidden not in columns


def test_referrer_hosts_are_normalized_and_junk_is_dropped():
    host = _namespace(_db())["_site_referrer_host"]
    assert host("https://news.ycombinator.com/item?id=1", "forkmesh.com") == (
        "news.ycombinator.com")
    assert host("https://WWW.Reddit.com./r/git", "forkmesh.com") == "reddit.com"
    assert host("http://lobste.rs/s/abc", "forkmesh.com") == "lobste.rs"
    for junk in (
        "", None, "not a url", "/relative/path",
        "javascript:alert(1)", "data:text/html,x", "android-app://com.slack",
        "https://192.168.1.5/x", "https://[::1]/x", "https://localhost/x",
        "https://box.local/x", "https://runner.internal/x",
        "https://" + "a" * 120 + ".com/x",
    ):
        assert host(junk, "forkmesh.com") == "", junk


def test_latest_full_referrer_url_is_safe_bounded_and_redacts_secrets():
    safe = _namespace(_db())["_site_referrer_url"]
    assert safe(
        "https://news.ycombinator.com/item?id=42&utm_source=front",
        "news.ycombinator.com",
    ) == "https://news.ycombinator.com/item?id=42&utm_source=front"
    assert safe(
        "https://www.reddit.com/r/git/comments/abc?token=secret&q=forkmesh#reply",
        "reddit.com",
    ) == (
        "https://reddit.com/r/git/comments/abc?"
        "token=%5Bredacted%5D&q=forkmesh"
    )
    assert safe("https://user:pass@reddit.com/r/git", "reddit.com") == ""
    assert safe("https://evil.example/x", "reddit.com") == ""


def test_own_deployment_never_counts_as_a_referring_website():
    host = _namespace(_db())["_site_referrer_host"]
    for own in ("forkmesh.com", "www.forkmesh.com"):
        assert host("https://forkmesh.com/pricing", own) == ""
        assert host("https://forkmesh.com/pricing", own) == ""
    # Sibling subdomains and the parent zone of a preview deployment.
    assert host("https://b.forkmesh.com/x", "forkmesh.com") == ""
    assert host("https://forkmesh.com/x", "preview.forkmesh.com") == ""
    assert host("https://forkmesh.internal/x", "forkmesh.com") == ""
    # A genuinely different site with a similar tail still counts.
    assert host("https://mesh.com/x", "forkmesh.com") == "mesh.com"


def test_only_served_page_navigations_are_counted():
    db = _db()
    ns = _namespace(db)
    run = lambda request, status=200, url=None: asyncio.run(  # noqa: E731
        ns["record_site_referral"](object(), request, url or _url(), status))

    run(_request("https://news.ycombinator.com/item?id=1"))
    assert db.execute(
        "SELECT visits FROM site_referrers WHERE host='news.ycombinator.com'"
    ).fetchone()["visits"] == 1

    ignored = [
        # Subresources and API calls: a page load is one visit, not thirty.
        (_request("https://news.ycombinator.com/x",
                  {"sec-fetch-dest": "image", "accept": "image/*"}), 200),
        (_request("https://news.ycombinator.com/x",
                  {"accept": "application/json", "sec-fetch-dest": "empty",
                   "sec-fetch-mode": "cors"}), 200),
        # Not a served page.
        (_request("https://news.ycombinator.com/x"), 404),
        (_request("https://news.ycombinator.com/x"), 302),
        # Not a browser visit.
        (_request("https://news.ycombinator.com/x", method="POST"), 200),
        (_request("https://news.ycombinator.com/x",
                  {"user-agent": "Mastodon/4.2.1"}), 200),
        # Internal navigation, and no referrer at all.
        (_request("https://forkmesh.com/pricing"), 200),
        (_request(None), 200),
    ]
    for request, status in ignored:
        run(request, status)
    assert db.execute(
        "SELECT SUM(visits) AS n FROM site_referrers").fetchone()["n"] == 1


def test_repeat_visits_bump_the_same_row_and_keep_first_seen():
    db = _db()
    ns = _namespace(db)
    for _ in range(3):
        asyncio.run(ns["record_site_referral"](
            object(), _request("https://lobste.rs/s/abc"), _url(), 200))
    row = db.execute("SELECT * FROM site_referrers").fetchone()
    assert (row["host"], row["visits"]) == ("lobste.rs", 3)
    assert row["last_url"] == "https://lobste.rs/s/abc"
    assert row["first_ts"] == row["last_ts"] == 1_720_000_000_000
    assert db.execute("SELECT COUNT(*) FROM site_referrers").fetchone()[0] == 1


def test_counter_failures_never_break_the_page_load():
    ns = _namespace(_db())

    async def broken_d1_run(_env, sql, *args):
        raise RuntimeError("d1 down")

    ns["d1_run"] = broken_d1_run
    asyncio.run(ns["record_site_referral"](
        object(), _request("https://lobste.rs/s/abc"), _url(), 200))
    asyncio.run(ns["record_site_referral"](object(), object(), None, 200))


def test_leaderboard_ranks_by_visits_and_reports_totals():
    db = _db()
    ns = _namespace(db)
    _seed(
        db, "news.ycombinator.com", 42, 500,
        "https://news.ycombinator.com/item?id=42")
    _seed(db, "lobste.rs", 9, 400, "https://lobste.rs/s/abc")
    _seed(
        db, "mastodon.social", 9, 900,
        "https://mastodon.social/@forkmesh/123")
    _seed(db, "quiet.example", 0, 100)
    resp = asyncio.run(ns["site_referrer_leaderboard"](object()))
    assert resp.data["ok"] is True
    assert [(r["host"], r["visits"]) for r in resp.data["board"]] == [
        ("news.ycombinator.com", 42),
        ("mastodon.social", 9),
        ("lobste.rs", 9),
    ]
    assert [r["url"] for r in resp.data["board"]] == [
        "https://news.ycombinator.com/item?id=42",
        "https://mastodon.social/@forkmesh/123",
        "https://lobste.rs/s/abc",
    ]
    assert (resp.data["sites"], resp.data["visits"]) == (3, 60)
    assert resp.cache_seconds == ns["SITE_REFERRER_LEADERBOARD_TTL"]
    assert len(ns["_cache_puts"]) == 1


def test_prune_keeps_the_busiest_hosts_so_referrer_spam_stays_bounded():
    db = _db()
    ns = _namespace(db)
    ns["SITE_REFERRER_RETAIN_HOSTS"] = 3
    _seed(db, "news.ycombinator.com", 42)
    for i in range(20):
        _seed(db, "spam%02d.example" % i, 1, i)
    asyncio.run(ns["prune_site_referrers"](object()))
    hosts = [r["host"] for r in db.execute(
        "SELECT host FROM site_referrers ORDER BY visits DESC").fetchall()]
    assert len(hosts) == 3
    assert hosts[0] == "news.ycombinator.com"


def test_board_runs_twenty_five_deep_and_carries_each_latest_url():
    db = _db()
    ns = _namespace(db)
    for index in range(30):
        _seed(db, "site%02d.example" % index, 100 - index, index,
              "https://site%02d.example/post" % index)
    resp = asyncio.run(ns["site_referrer_leaderboard"](object()))
    assert ns["SITE_REFERRER_LEADERBOARD_LIMIT"] == 25
    board = resp.data["board"]
    assert len(board) == 25
    assert board[0]["host"] == "site00.example"
    assert board[24]["url"] == "https://site24.example/post"
    # Depth changes what is listed, never what is counted.
    assert (resp.data["sites"], resp.data["visits"]) == (30, 2565)


def test_the_hub_card_keeps_the_common_ten_row_depth():
    overview = ENTRY.split("async def leaderboards_overview", 1)[1].split(
        "async def prune_site_referrers", 1)[0]
    # /leaderboards renders one card per board in a grid of equals, so the
    # deeper referring-site board is trimmed for that surface only.
    assert "site_rows = sites.get(\"board\")" in overview
    assert "site_rows[:LEADERBOARD_LIMIT]" in overview
    assert '"visits", site_rows, "community", "sites")' in overview


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="Node.js is unavailable")
def test_a_row_links_only_a_safe_url_on_its_own_hostname():
    rendered = _render_site_rows([
        {"host": "lobste.rs", "url": "https://lobste.rs/s/abc", "visits": 4},
        # A stored URL that does not belong to the row's own hostname, and the
        # schemes a spammed Referer would use to get a link out of the board.
        {"host": "lobste.rs", "url": "https://evil.example/x", "visits": 3},
        {"host": "lobste.rs", "url": "javascript:alert(1)", "visits": 2},
        {"host": "lobste.rs", "url": "data:text/html,<script>x</script>",
         "visits": 2},
        {"host": "lobste.rs", "url": "https://user:pw@lobste.rs/s/abc",
         "visits": 1},
        # www is folded away in the stored hostname, so it still matches.
        {"host": "lobste.rs", "url": "https://www.lobste.rs/s/xyz",
         "visits": 1},
        # An older row counted before the URL column existed.
        {"host": "lobste.rs", "url": "", "visits": 1},
        # Quotes in the path must not escape the href attribute.
        {"host": "lobste.rs", "url": 'https://lobste.rs/s/a"b', "visits": 1},
    ])
    assert [row["href"] for row in rendered] == [
        "https://lobste.rs/s/abc", "", "", "", "",
        "https://www.lobste.rs/s/xyz", "",
        "https://lobste.rs/s/a%22b",
    ]
    linked, blocked, missing = rendered[0], rendered[1], rendered[6]
    assert 'href="https://lobste.rs/s/abc"' in linked["html"]
    assert 'rel="nofollow ugc noopener noreferrer"' in linked["html"]
    assert 'target="_blank"' in linked["html"]
    for row in (blocked, missing):
        assert "<a " not in row["html"]
        assert "Full page URL captures on the next referral" in row["html"]
    # Every row still reads as a hostname and a visit count.
    assert "lobste.rs" in linked["html"] and "4 visits" in linked["html"]
    for row in rendered:
        assert "javascript:" not in row["html"]
        assert "data:text/html" not in row["html"]


def test_surfaces_expose_the_board_and_the_privacy_contract():
    assert '"/api/referrals/sites"' in REFERRALS_JS
    assert "data-referrals-sites" in REFERRALS_JS
    assert "data-referrals-sites" in REFERRALS_HTML
    assert "Top referring websites" in REFERRALS_HTML
    # The hostname stays text. The one link a row hands out is its own latest
    # referring page, built from the re-checked URL and worth no ranking.
    row_html = _referrals_js_slice(
        "function referralSiteRowHtml", "function renderReferralSites")
    assert "referralSiteUrl(row)" in row_html
    assert 'rel="nofollow ugc noopener noreferrer"' in row_html
    assert "escapeHtml(href)" in row_html
    # Depth is stated on the panel and matches what the Worker returns.
    assert "top 25 by visits" in REFERRALS_HTML
    assert "const REFERRAL_SITE_BOARD_LIMIT = 25;" in REFERRALS_JS
    assert "top ${REFERRAL_SITE_BOARD_LIMIT} by visits" in REFERRALS_JS
    assert "Referring websites" in PRIVACY
    assert '"/api/referrals/sites"' in APP_RUNTIME
    assert "await record_site_referral(self.env, request, url, status)" in APP_RUNTIME


def test_world_has_a_separate_privacy_safe_http_referrer_board():
    loader = WORLD.split(
        "async loadReferralLeaderboard()", 1
    )[1].split("async copyReferralLink()", 1)[0]
    assert '"/api/leaderboards"' in loader
    assert "snapshot?.sites?.board" in loader
    assert "updateSiteReferrerLeaderboard" in WORLD
    assert 'addLeaderboardCard("http-referrers"' in SCENE
    assert "leaderboardCardState.siteRows" in SCENE
    assert 'boardId === "http-referrers"' in SCENE
    assert "HTTP REFERER LEADERBOARD" in SCENE
    assert "GROUPED BY HOSTNAME · LATEST SAFE FULL URL" in SCENE
    assert "FULL PAGE URL · SENSITIVE QUERY VALUES REDACTED" in SCENE
    assert '"leaderboard-card"' in SCENE
    assert "safeSiteReferrerURL" in SCENE
    assert "updateSiteReferrerLeaderboard" in SCENE
    assert "const LEADERBOARD_ISLAND_CENTER_X = -130;" in SCENE
    assert 'leaderboardDistrict.name = "forkmesh-leaderboard-district"' in SCENE


if __name__ == "__main__":
    test_migration_adds_one_bounded_latest_url_to_the_aggregate_host_row()
    test_referrer_hosts_are_normalized_and_junk_is_dropped()
    test_latest_full_referrer_url_is_safe_bounded_and_redacts_secrets()
    test_own_deployment_never_counts_as_a_referring_website()
    test_only_served_page_navigations_are_counted()
    test_repeat_visits_bump_the_same_row_and_keep_first_seen()
    test_counter_failures_never_break_the_page_load()
    test_leaderboard_ranks_by_visits_and_reports_totals()
    test_prune_keeps_the_busiest_hosts_so_referrer_spam_stays_bounded()
    test_board_runs_twenty_five_deep_and_carries_each_latest_url()
    test_the_hub_card_keeps_the_common_ten_row_depth()
    if shutil.which("node"):
        test_a_row_links_only_a_safe_url_on_its_own_hostname()
    test_surfaces_expose_the_board_and_the_privacy_contract()
    test_world_has_a_separate_privacy_safe_http_referrer_board()
