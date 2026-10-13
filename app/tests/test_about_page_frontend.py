#!/usr/bin/env python3
"""Static contracts for the about page chrome."""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
ABOUT_HTML = PUBLIC / "about.html"


def _about_html() -> str:
    return ABOUT_HTML.read_text(encoding="utf-8")


def test_about_page_uses_terms_inspired_header_treatment():
    html = _about_html()

    assert 'href="/site-header.css"' in html
    assert 'src="/site-header.js?v=${version}"' in html
    assert '<div data-forkmesh-header="simple"></div>' in html
    assert 'class="about-shell' in html
    assert 'class="mesh-bg"' not in html
    assert 'class="mesh-grid"' not in html
    assert 'class="about-intro' in html
    assert 'class="about-hero ' not in html
    assert 'class="about-hero-card' not in html
    assert "button-primary" not in html
    assert "button-ghost" not in html
    assert "Sign Up / Log In" not in html
    assert "See the changelog" not in html
    assert 'class="site-header"' not in html
    assert 'class="global-nav"' not in html
    assert 'aria-label="Primary"' not in html[html.index("<body") :]
    assert 'id="theme-toggle"' not in html


def test_about_page_uses_standard_footer_mount():
    html = _about_html()
    body = html[html.index("<body"):]

    for marker in (
        'href="/site-footer.css"',
        'src="/site-footer.js?v=${version}"',
        '<div data-forkmesh-footer="standard"></div>',
    ):
        assert marker in html

    assert "site-footer-links-grid" not in body
    assert '<footer id="signup"' not in body


if __name__ == "__main__":
    test_about_page_uses_terms_inspired_header_treatment()
    test_about_page_uses_standard_footer_mount()
