#!/usr/bin/env python3
"""Social-preview info card (adhoc #46).

A Mastodon/Slack/Twitter unfurl of a repo page used to show only the ForkMesh
logo; now og:image points at a rendered 1200x630 PNG stats card. og_card.py is
a pure, js-free sibling module (like activitypub.py), so these tests import
the real renderer and decode its actual PNG output; the entry.py wiring (route
+ og tags) is pinned against the worker source text.

Run: python3 -m pytest cloudflare_worker/tests/test_repo_og_card.py
"""
import importlib.util
import struct
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"
spec = importlib.util.spec_from_file_location("og_card", SRC / "og_card.py")
og_card = importlib.util.module_from_spec(spec)
spec.loader.exec_module(og_card)

ENTRY_SRC = (
    (SRC / "entry.py").read_text(encoding="utf-8") + "\n\n\n"
    + (SRC / "app_runtime.py").read_text(encoding="utf-8")
)
URLS_SRC = (SRC / "urls.py").read_text(encoding="utf-8")

NOW_S = 1_780_000_000  # fixed "now" so age formatting is deterministic

SAMPLE = {
    "owner": "forkmesh",
    "repo": "forkmesh",
    "description": "Distributed Git hosting on a mesh of desktop nodes.",
    "branch": "main",
    "host": "forkmesh.com",
    "stars": 12,
    "mirrors": 61,
    "issues": "46",
    "pulls": "3",
    "commits": "1284",
    "branches": "17",
    "sizeBytes": 73400320,
    "updatedAt": str((NOW_S - 9 * 3600) * 1000),
    "followers": 8,
}


def decode_png_rgb(png):
    """Minimal decoder for the card's own output (8-bit RGB, filter 0)."""
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    idat = b""
    w = h = 0
    while pos < len(png):
        length, ctype = struct.unpack(">I4s", png[pos:pos + 8])
        pos += 8
        chunk = png[pos:pos + length]
        pos += length + 4
        if ctype == b"IHDR":
            w, h, depth, color, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk)
            assert (depth, color, interlace) == (8, 2, 0)
        elif ctype == b"IDAT":
            idat += chunk
        # verify chunk CRCs while we are here
        crc = struct.unpack(">I", png[pos - 4:pos])[0]
        assert crc == zlib.crc32(ctype + chunk) & 0xFFFFFFFF
    raw = zlib.decompress(idat)
    stride = w * 3
    assert len(raw) == h * (1 + stride)
    rows = []
    for y in range(h):
        assert raw[y * (1 + stride)] == 0  # our encoder never filters
        start = y * (1 + stride) + 1
        rows.append(raw[start:start + stride])
    return w, h, rows


def pixel(rows, x, y):
    return tuple(rows[y][x * 3:x * 3 + 3])


# --- Renderer ----------------------------------------------------------------

def test_card_is_a_valid_og_sized_png():
    png = og_card.render_repo_card(SAMPLE, NOW_S)
    w, h, rows = decode_png_rgb(png)
    assert (w, h) == (og_card.CARD_W, og_card.CARD_H) == (1200, 630)


# The terminal window's geometry (see og_card.render_repo_card).
WIN = og_card.TERM_MARGIN
BAR_BOTTOM = WIN + og_card.TERM_BAR_H
BODY_TOP = BAR_BOTTOM + 30
LEFT = WIN + og_card.TERM_PAD


def _unscanned(y):
    """A body row the scanline pass leaves at full brightness."""
    return y if (y - BAR_BOTTOM) % 2 else y + 1


def test_card_draws_a_terminal_window():
    png = og_card.render_repo_card(SAMPLE, NOW_S)
    _, _, rows = decode_png_rgb(png)
    # Backdrop outside the window, title bar, then the near-black body.
    assert pixel(rows, 2, 2) not in {og_card._TERM_BODY, og_card._TERM_BAR}
    assert pixel(rows, og_card.CARD_W - WIN - 30, WIN + 8) == og_card._TERM_BAR
    blank = (og_card.CARD_W - WIN - 20, _unscanned(og_card.CARD_H - 120))
    assert pixel(rows, *blank) == og_card._TERM_BODY
    # Faint scanlines: the next body row is the same black, a little dimmer.
    dim = pixel(rows, blank[0], blank[1] + 1)
    assert dim != og_card._TERM_BODY and sum(dim) < sum(og_card._TERM_BODY)
    # Three traffic-light dots at the left of the title bar.
    middle = WIN + og_card.TERM_BAR_H // 2
    for index, color in enumerate(og_card._TERM_DOTS):
        assert pixel(rows, WIN + 28 + index * 24, middle) == color
    # Rounded corners: the window's very corner pixel is backdrop.
    assert pixel(rows, WIN, WIN) not in {og_card._TERM_BAR, og_card._TERM_BORDER}


def _bright_pixels(rows, top, height, color=og_card._TERM_TEXT):
    return sum(1 for y in range(top, top + height)
               for x in range(LEFT, og_card.CARD_W - 200)
               if pixel(rows, x, y) == color)


def test_card_renders_the_command_and_description():
    with_title = og_card.render_repo_card(SAMPLE, NOW_S)
    without = og_card.render_repo_card(dict(SAMPLE, owner="x", repo="y"),
                                       NOW_S)
    assert with_title != without  # the repository name reaches the pixels
    _, _, rows = decode_png_rgb(with_title)
    assert _bright_pixels(rows, BODY_TOP, 40) > 300     # $ ... forkmesh/forkmesh
    assert _bright_pixels(rows, BODY_TOP + 60, 40) > 300  # description


def test_short_and_long_descriptions_lay_out_within_the_window():
    cols = 40
    assert og_card._wrap_cols("My shell config.", cols, 3) == ["My shell config."]
    lines = og_card._wrap_cols("a deliberately long description " * 20, cols, 3)
    assert len(lines) == 3
    assert all(len(line) <= cols for line in lines)
    assert lines[-1].endswith("\u2026")
    short = og_card.render_repo_card(dict(SAMPLE, description="Tiny."), NOW_S)
    long = og_card.render_repo_card(
        dict(SAMPLE, description="word " * 200), NOW_S)
    _, _, short_rows = decode_png_rgb(short)
    _, _, long_rows = decode_png_rgb(long)
    third_line = BODY_TOP + 60 + 2 * 52
    assert _bright_pixels(long_rows, third_line, 36) > 300
    # The short card has no third description line: that band holds the
    # dimmer stats labels and bold values, not description text.
    assert _bright_pixels(short_rows, third_line, 36) < \
        _bright_pixels(long_rows, third_line, 36)
    for png in (short, long):
        assert decode_png_rgb(png)[:2] == (1200, 630)


def test_zero_counts_are_left_off_the_card():
    zeros = dict(SAMPLE, stars=0, pulls="", mirrors=1, followers=0,
                 contributors=0)
    labels = [label for label, _ in og_card.repo_card_stats(zeros, NOW_S)]
    assert labels == ["commits", "issues", "branches", "size", "updated"]
    full = dict(SAMPLE, contributors=273)
    pairs = dict(og_card.repo_card_stats(full, NOW_S))
    assert pairs["contributors"] == "273"
    assert pairs["stars"] == "12" and pairs["mirrors"] == "61"
    assert pairs["issues"] == "46" and pairs["updated"] == "9h ago"
    assert og_card.repo_card_stats({"owner": "o", "repo": "r"}, NOW_S) == []
    assert og_card.render_repo_card(zeros, NOW_S) != og_card.render_repo_card(
        dict(zeros, stars=5), NOW_S)


def test_stat_grid_aligns_columns_and_narrows_to_fit():
    pairs = [("commits", "45.5k"), ("contributors", "273"), ("issues", "39"),
             ("branches", "2"), ("size", "382.9 MB"), ("updated", "1h ago")]
    wide = og_card._stat_grid(pairs, 60)
    assert [len(row) for row in wide] == [3, 3]
    narrow = og_card._stat_grid(pairs, 48)
    assert [len(row) for row in narrow] == [2, 2, 2]
    # Keys in a column are padded to one width, so the values line up.
    assert len({len(row[0][0]) for row in narrow}) == 1


def test_accent_comes_from_the_theme_else_the_logo():
    side = 40
    red = (side, side, b"\xe7\x4c\x3c\xff" * (side * side))
    assert og_card.logo_accent(red) == (0xe7, 0x4c, 0x3c)
    grey = (side, side, b"\x80\x80\x80\xff" * (side * side))
    assert og_card.logo_accent(grey) is None
    assert og_card.parse_color("#10b981") == (16, 185, 129)
    assert og_card.parse_color("nope") is None
    _, _, rows = decode_png_rgb(og_card.render_repo_card(
        dict(SAMPLE, accent="#ff3030"), NOW_S, red))
    themed = sum(1 for y in range(BODY_TOP, BODY_TOP + 40)
                 for x in range(LEFT, LEFT + 30)
                 if pixel(rows, x, y) == (255, 48, 48))
    assert themed > 20  # the prompt's "$" is drawn in the theme accent
    _, _, rows = decode_png_rgb(og_card.render_repo_card(SAMPLE, NOW_S, red))
    from_logo = sum(1 for y in range(BODY_TOP, BODY_TOP + 40)
                    for x in range(LEFT, LEFT + 30)
                    if pixel(rows, x, y) == (0xe7, 0x4c, 0x3c))
    assert from_logo > 20


def test_missing_stats_render_as_zeros_not_errors():
    png = og_card.render_repo_card({"owner": "o", "repo": "r"}, NOW_S)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"


def test_non_ascii_falls_back_instead_of_crashing():
    png = og_card.render_repo_card(
        dict(SAMPLE, description="ümläut — café \U0001f680"),
        NOW_S)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"


def test_long_names_shrink_and_ellipsize_within_canvas():
    png = og_card.render_repo_card(
        dict(SAMPLE, owner="a" * 60, repo="b" * 60,
             description="word " * 120), NOW_S)
    w, h, _ = decode_png_rgb(png)
    assert (w, h) == (1200, 630)


def test_font_table_covers_printable_ascii():
    assert len(og_card._FONT) == (127 - 32) * 5


# --- Formatters ----------------------------------------------------------------

def test_format_count_compacts_and_tolerates_catalog_strings():
    assert og_card.format_count("") == "0"
    assert og_card.format_count(None) == "0"
    assert og_card.format_count("46") == "46"
    assert og_card.format_count(999) == "999"
    assert og_card.format_count(1284) == "1.3k"
    assert og_card.format_count(2000) == "2k"
    assert og_card.format_count(3_400_000) == "3.4M"
    assert og_card.format_count("garbage") == "0"


def test_format_size_humanizes():
    assert og_card.format_size(0) == "0 B"
    assert og_card.format_size(512) == "512 B"
    assert og_card.format_size(73400320) == "70 MB"
    assert og_card.format_size(1536) == "1.5 KB"
    assert og_card.format_size("nope") == "0 B"


def test_format_age_accepts_ms_seconds_and_iso():
    assert og_card.format_age(str((NOW_S - 9 * 3600) * 1000), NOW_S) == "9h ago"
    assert og_card.format_age(str(NOW_S - 3 * 86400), NOW_S) == "3d ago"
    assert og_card.format_age("2026-01-15T12:00:00Z", NOW_S).endswith("ago")
    assert og_card.format_age("", NOW_S) == "-"
    assert og_card.format_age("not-a-date", NOW_S) == "-"
    assert og_card.format_age(str(NOW_S - 30), NOW_S) == "now"


# --- entry.py wiring ------------------------------------------------------------

def test_route_regex_matches_card_path():
    import re
    match = re.search(r"REPO_CARD_RE = re\.compile\(r\"(.+?)\"\)", URLS_SRC)
    assert match, "REPO_CARD_RE missing from urls.py"
    card_re = re.compile(match.group(1))
    assert card_re.match("/api/repo/forkmesh/forkmesh/card.png")
    assert not card_re.match("/api/repo/forkmesh/forkmesh/media/logo.png")


def test_entry_dispatches_card_route_to_handler():
    assert "REPO_CARD_RE.match(url.path)" in ENTRY_SRC
    assert "async def repo_card_handler" in ENTRY_SRC


def test_repo_page_og_tags_point_at_card():
    # The old behavior (og:image = full-bleed logo) must be gone...
    assert 'og_image = origin + "/assets/logo.png"' not in ENTRY_SRC
    # ...replaced by the rendered card, with large-image hints so Mastodon
    # renders it full width instead of as a thumbnail.
    # The card revision rides on the version, so a redesign refreshes caches.
    assert '"%s.%s" % (social_version, og_card.CARD_REVISION)' in ENTRY_SRC
    assert "social_version = _repository_social_version(repo_record)" in ENTRY_SRC
    assert "_build_rev(self.env)" not in ENTRY_SRC[
        ENTRY_SRC.index("async def _serve_repo_page"):
        ENTRY_SRC.index("async def _serve_repo_profile_page")
    ]
    assert 'summary_large_image' in ENTRY_SRC
    assert 'og:image:width' in ENTRY_SRC and 'og:image:height' in ENTRY_SRC
    assert 'og:description' in ENTRY_SRC


def test_card_handler_is_public_get_and_edge_cached():
    handler = ENTRY_SRC.split("async def repo_card_handler", 1)[1]
    handler = handler.split("async def ", 1)[0]
    assert "_repo_is_private" in handler
    assert "edge_cache_match_media" in handler and "edge_cache_put" in handler
    assert "render_repo_card" in handler


# --- Referral share-link card (adhoc #313) -------------------------------------

REFERRAL_SAMPLE = {
    "name": "jett",
    "host": "forkmesh.com",
    "clicks": 1284,
    "signups": 37,
    "lastTs": str((NOW_S - 2 * 3600) * 1000),
}


def test_referral_card_is_a_valid_og_sized_png():
    png = og_card.render_referral_card(REFERRAL_SAMPLE, NOW_S)
    w, h, _ = decode_png_rgb(png)
    assert (w, h) == (og_card.CARD_W, og_card.CARD_H)


def test_referral_card_pixels_change_with_the_live_counters():
    base = og_card.render_referral_card(REFERRAL_SAMPLE, NOW_S)
    more_clicks = og_card.render_referral_card(
        dict(REFERRAL_SAMPLE, clicks=1285), NOW_S)
    more_signups = og_card.render_referral_card(
        dict(REFERRAL_SAMPLE, signups=38), NOW_S)
    assert base != more_clicks != more_signups
    assert base != more_signups


def test_format_exact_keeps_every_referral_digit():
    assert og_card.format_exact(0) == "0"
    assert og_card.format_exact("37") == "37"
    assert og_card.format_exact(1284) == "1,284"
    assert og_card.format_exact(1285) == "1,285"
    assert og_card.format_exact(None) == "0"
    assert og_card.format_exact("garbage") == "0"


def test_referral_card_survives_missing_counters():
    png = og_card.render_referral_card({"name": "jett"}, NOW_S)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"


def test_referral_card_route_and_handler_are_wired():
    import re
    match = re.search(r"REFERRAL_CARD_RE = re\.compile\(r\"(.+?)\"\)", URLS_SRC)
    assert match, "REFERRAL_CARD_RE missing from urls.py"
    card_re = re.compile(match.group(1))
    assert card_re.match("/api/referrals/jett/card.png")
    assert not card_re.match("/api/referrals/leaderboard")
    assert "REFERRAL_CARD_RE.match(url.path)" in ENTRY_SRC
    handler = ENTRY_SRC.split("async def referral_card_handler", 1)[1]
    handler = handler.split("async def ", 1)[0]
    # Unknown/inactive referrers render nothing; live counters, edge-cached.
    assert "_referral_account" in handler and "not_found" in handler
    assert "_referral_counts" in handler
    assert "render_referral_card" in handler
    assert "edge_cache_match_media" in handler and "edge_cache_put" in handler


def test_binary_response_bodies_are_copied_into_js_owned_buffers():
    # A bare _to_js(bytes) is a VIEW into Python's WASM memory. A binary
    # Response body is streamed to the client (and read by edge-cache put)
    # AFTER the handler returns and the GIL is released; reading that view off
    # the GIL is a runtime crash ("Attempted to use PyProxy when Python GIL not
    # held") that poisons the isolate for every later invocation, including the
    # once-a-minute cron tick. Every binary body must round-trip through
    # Uint8Array.new to land in a JS-owned buffer first (adhoc #203).
    assert "JsResponse.new(_to_js(bytes(" not in ENTRY_SRC
    for site in ("bytes(data)", "bytes(raw)", "bytes(png)"):
        assert "Uint8Array.new(_to_js(%s))" % site in ENTRY_SRC
def test_card_titles_the_public_address():
    short = og_card.render_repo_card(dict(SAMPLE, path="/forkmesh"), NOW_S)
    full = og_card.render_repo_card(SAMPLE, NOW_S)
    explicit = og_card.render_repo_card(
        dict(SAMPLE, path="/forkmesh/forkmesh"), NOW_S)
    assert explicit == full and short != full


# --- Live counts: the sources the repository page uses ----------------------

def test_open_issues_are_the_ledgers_numbered_folders():
    tree = {"ok": True, "entries": [
        {"type": "tree", "name": "1"}, {"type": "tree", "name": "38"},
        {"type": "tree", "name": "120"}, {"type": "blob", "name": "7"},
        {"type": "tree", "name": "README"}, {"type": "tree", "name": "0"}]}
    assert og_card.open_issue_count(tree) == 3
    assert og_card.open_issue_count({"entries": []}) == 0
    assert og_card.open_issue_count(None) is None


def test_branches_are_the_current_branch_list():
    listing = {"ok": True, "defaultBranch": "main", "branches": [
        {"name": "main", "commit": "a" * 40},
        {"name": "feature/x", "commit": "b" * 40}]}
    assert og_card.branch_count(listing) == 2
    assert og_card.branch_count({"ok": True}) is None


def test_contributors_come_from_the_roster_else_the_authors():
    import json as _json
    roster = _json.dumps({"count": 261, "contributors": [
        {"slug": "a", "name": "A", "commits": 3}]})
    assert og_card.roster_count(roster) == 261
    undeclared = _json.dumps({"contributors": [
        {"name": "A"}, {"login": "b"}, {"name": ""}, "junk"]})
    assert og_card.roster_count(undeclared) == 2
    assert og_card.roster_count("not json") is None
    assert og_card.roster_count(_json.dumps({"contributors": []})) is None
    assert og_card.has_roster({"entries": [
        {"type": "tree", "name": "contributors"}]})
    assert not og_card.has_roster({"entries": [
        {"type": "blob", "name": "contributors"}]})
    stats = {"contributorCount": 5, "contributors": [
        {"name": "Author A", "email": "a1@example.com"},
        {"name": "Author B", "email": "b1@example.com"},
        {"name": "dependabot[bot]", "email": "x@example.com"},
        {"name": "Acme Code Actions", "email": "a@example.com"},
    ]}
    # 5 authors, of whom the two listed bots are not people.
    assert og_card.stats_contributor_count(stats, "Acme Code") == 3
    assert og_card.stats_contributor_count({"contributors": []}) is None


def test_counts_token_changes_with_every_count():
    base = {"issues": 39, "branches": 2, "contributors": 261}
    token = og_card.counts_token(base)
    assert token == "i39.b2.c261"
    for key in base:
        assert og_card.counts_token(dict(base, **{key: base[key] + 1})) != token
    assert og_card.counts_token({}) == "i-.b-.c-"


def test_card_revision_is_bumped_for_the_terminal_card():
    assert og_card.CARD_REVISION == "3"


def _load_entry_function(name, namespace):
    import ast
    tree = ast.parse(ENTRY_SRC)
    node = next(n for n in tree.body
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                and n.name == name)
    scope = dict(namespace)
    exec(compile(ast.Module(body=[node], type_ignores=[]), "entry.py",
                 "exec"), scope)
    return scope[name]


def _live_reads(roster=True):
    import json as _json
    answers = {
        ("branches", ()): {"ok": True, "branches": [
            {"name": "main"}, {"name": "feature"}]},
        ("tree", (("path", ".forkmesh/issues/open"),)): {
            "ok": True, "entries": [{"type": "tree", "name": str(n)}
                                    for n in range(1, 40)]},
        ("tree", (("path", ".forkmesh"),)): {"ok": True, "entries": (
            [{"type": "tree", "name": "contributors"}] if roster else [])
            + [{"type": "tree", "name": "issues"}]},
        ("blobs", (("path", og_card.ROSTER_PATH),)): {"ok": True, "blobs": {
            og_card.ROSTER_PATH: {"encoding": "utf8", "truncated": False,
                                  "content": _json.dumps({
                                      "count": 261,
                                      "contributors": [{"name": "A"}]})}}},
        ("stats", ()): {"contributorCount": 4, "contributors": [
            {"name": "A"}, {"name": "B"}, {"name": "github-actions"}]},
    }
    calls = []

    async def read(_env, owner, repo, operation, query=None,
                   context_cache=None):
        key = (operation, tuple(sorted((query or {}).items())))
        calls.append(key)
        return answers.get(key)
    return read, calls


def _counts_function(read):
    import asyncio
    import time
    return _load_entry_function("_repo_card_counts", {
        "_repo_card_read": read, "og_card": og_card, "asyncio": asyncio,
        "time": time, "instance_name": lambda _env: "ForkMesh",
        "_repo_card_counts_memo": {}, "REPO_CARD_COUNTS_TTL_MS": 300000,
        "REPO_CARD_COUNTS_MAX": 256,
    })


def test_card_counts_read_the_ledger_branches_and_roster():
    import asyncio
    read, calls = _live_reads()
    counts_of = _counts_function(read)
    counts = asyncio.run(counts_of(None, "acme", "tool"))
    assert counts == {"issues": 39, "branches": 2, "contributors": 261}
    assert ("stats", ()) not in calls
    before = len(calls)
    assert asyncio.run(counts_of(None, "acme", "tool")) == counts
    assert len(calls) == before
    assert asyncio.run(counts_of(None, "ACME", "Tool")) == counts
    assert len(calls) == before
    asyncio.run(counts_of(None, "acme", "other"))
    assert len(calls) > before


def test_card_counts_fall_back_to_the_bot_filtered_authors():
    import asyncio
    read, calls = _live_reads(roster=False)
    counts = asyncio.run(_counts_function(read)(None, "o", "r"))
    assert counts["contributors"] == 3
    assert ("stats", ()) in calls


def test_card_handler_keys_the_cache_on_the_live_counts():
    handler = ENTRY_SRC.split("async def repo_card_handler", 1)[1]
    handler = handler.split("\nasync def ", 1)[0]
    assert "await _repo_card_counts(env, owner, repo)" in handler
    assert handler.index("_repo_card_counts(") < handler.index(
        "edge_cache_match_media")
    assert '"n=" + og_card.counts_token(counts)' in handler
    assert '"accent": await _repo_card_accent(env),' in handler
