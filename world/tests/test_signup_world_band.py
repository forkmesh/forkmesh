#!/usr/bin/env python3
"""Standalone ForkMesh World bands, and where they are not wanted.

[data-forkmesh-world] lets a page that renders no site footer mount the World
band on its own. The signup page used to; it no longer does. A "step into the
developer city" pitch below a half-filled signup form competes with the one
action the page exists for, and referral links land there mid-signup.
"""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "app" / "public"
WWW_PUBLIC = PUBLIC.parents[1] / "www" / "public"
SIGNUP_HTML = (PUBLIC / "signup.html").read_text(encoding="utf-8")
FOOTER_JS = (WWW_PUBLIC / "site-footer.js").read_text(encoding="utf-8")
FOOTER_CSS = (WWW_PUBLIC / "site-footer.css").read_text(encoding="utf-8")


def test_signup_page_carries_no_world_band_or_footer():
    assert "data-forkmesh-world" not in SIGNUP_HTML
    assert "data-forkmesh-footer" not in SIGNUP_HTML
    # The footer bundle was loaded for the band alone, so it goes with it.
    assert "/site-footer.css" not in SIGNUP_HTML
    assert "/site-footer.js" not in SIGNUP_HTML


def test_footer_script_mounts_standalone_world_bands():
    assert "[data-forkmesh-world]" in FOOTER_JS
    assert "mountWorldBands" in FOOTER_JS
    assert "site-footer-world-standalone" in FOOTER_JS
    assert (
        'const WORLD_EMBED_URL = "https://world.forkmesh.com/world/"'
        in FOOTER_JS
    )
    assert "WORLD_EMBED_ORIGINS" in FOOTER_JS
    assert "WORLD_EMBED_ORIGINS.has(window.location.origin)" in FOOTER_JS
    assert 'href="/world/"' in FOOTER_JS
    assert 'src="${WORLD_EMBED_URL}"' in FOOTER_JS


def test_standalone_band_carries_the_footer_palette():
    # Outside .forkmesh-footer the --fm-footer-* variables are undefined, so
    # the standalone band declares them (dark) plus a light-theme override.
    assert ".site-footer-world-standalone {" in FOOTER_CSS
    assert "html.light .site-footer-world-standalone {" in FOOTER_CSS
    for var in ("--fm-footer-panel", "--fm-footer-fg", "--fm-footer-muted",
                "--fm-footer-border"):
        assert FOOTER_CSS.count(var + ":") >= 3
