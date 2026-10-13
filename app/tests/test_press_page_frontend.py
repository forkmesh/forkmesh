#!/usr/bin/env python3
"""Static contracts for the press page."""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
PRESS_HTML = PUBLIC / "press.html"
FOOTER_JS = PUBLIC / "site-footer.js"


def _press_html() -> str:
    return PRESS_HTML.read_text(encoding="utf-8")


def test_press_page_exists_with_press_kit_content():
    html = _press_html()

    assert "<title>ForkMesh Press Kit</title>" in html
    assert 'href="https://forkmesh.com/press"' in html
    assert "ForkMesh Press Kit" in html
    assert "Logo" in html
    assert "Style guide" in html
    assert "BetaNYC" in html
    assert "coming soon" in html
    assert 'src="/assets/logo.svg"' in html
    assert 'href="/assets/logo.svg"' in html
    assert 'download="forkmesh-logo.svg"' in html
    assert 'href="/assets/logo.png"' in html
    assert 'download="forkmesh-logo.png"' in html
    assert "#090909" in html
    assert "#4ade80" in html
    assert "ForkMesh Lato" in html
    assert "ForkMesh Mono" in html


def test_press_page_uses_shared_simple_header_and_standard_footer():
    html = _press_html()
    body = html[html.index("<body") :]

    for marker in (
        'href="/styles.css"',
        'href="/site-header.css"',
        'src="/site-header.js?v=${version}"',
        'href="/site-footer.css"',
        'src="/site-footer.js?v=${version}"',
        '<div data-forkmesh-header="simple"></div>',
        '<div data-forkmesh-footer="standard"></div>',
    ):
        assert marker in html

    assert 'class="site-header"' not in body
    assert 'class="global-nav"' not in body
    assert "site-footer-links-grid" not in body
    assert '<footer id="signup"' not in body


def test_press_page_is_discoverable_from_shared_footer():
    footer = FOOTER_JS.read_text(encoding="utf-8")

    assert '{ href: "/press", label: "Press" }' in footer


def test_press_page_uses_solid_dark_background_without_gradients():
    html = _press_html()

    assert "gradient(" not in html
    assert "mask-image:" not in html
    assert "background: var(--background);" in html


def test_press_page_headings_swatches_and_status_follow_theme_tokens():
    html = _press_html()

    assert "color: var(--foreground);" in html
    assert "border: 1px solid var(--border);" in html
    assert "color: var(--warning);" in html
    assert "color: #fff;" not in html
    assert "color: #f5d78e;" not in html


if __name__ == "__main__":
    test_press_page_exists_with_press_kit_content()
    test_press_page_uses_shared_simple_header_and_standard_footer()
    test_press_page_is_discoverable_from_shared_footer()
    test_press_page_uses_solid_dark_background_without_gradients()
    test_press_page_headings_swatches_and_status_follow_theme_tokens()
