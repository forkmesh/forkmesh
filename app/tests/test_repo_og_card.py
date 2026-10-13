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


def test_card_paints_background_panel_and_logo():
    png = og_card.render_repo_card(SAMPLE, NOW_S)
    _, _, rows = decode_png_rgb(png)
    # Page background outside the panel, panel fill + border inside.
    assert pixel(rows, 2, 2) == (1, 4, 9)
    assert pixel(rows, 29, 29) == (48, 54, 61)
    assert pixel(rows, 600, 60) == (13, 17, 23)
    # The logo blit must land in the top-right corner area: something there is
    # neither background nor panel fill.
    logo_region = {
        pixel(rows, x, y)
        for x in range(og_card.CARD_W - 28 - og_card._LOGO_W,
                       og_card.CARD_W - 28)
        for y in range(68, 68 + og_card._LOGO_H, 4)
    }
    assert logo_region - {(1, 4, 9), (13, 17, 23), (48, 54, 61)}
    # And it stays small: the left half of the card at logo height is text on
    # panel fill, not logo pixels (the old full-bleed logo failure mode).
    assert pixel(rows, 320, 300) in {(13, 17, 23), (240, 246, 252)}


def test_card_renders_title_text_pixels():
    with_title = og_card.render_repo_card(SAMPLE, NOW_S)
    without = og_card.render_repo_card(dict(SAMPLE, owner="x", repo="y"),
                                       NOW_S)
    assert with_title != without  # title actually reaches the pixels
    _, _, rows = decode_png_rgb(with_title)
    fg = sum(1 for x in range(72, 1000)
             for y in range(96, 138) if pixel(rows, x, y) == (240, 246, 252))
    assert fg > 500  # a real run of bright title pixels


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
    assert 'og_image += "?v=" + quote(social_version, safe="")' in ENTRY_SRC
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
