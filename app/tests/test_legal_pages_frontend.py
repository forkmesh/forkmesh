#!/usr/bin/env python3
"""Static contracts for terms/privacy pages."""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
LEGAL_PAGES = (
    PUBLIC / "terms.html",
    PUBLIC / "privacy.html",
)


def _read(page: Path) -> str:
    return page.read_text(encoding="utf-8")


def test_legal_pages_use_404_style_simple_nav_only_docs_blog_login():
    for page in LEGAL_PAGES:
        html = _read(page)
        body = html[html.index("<body") :]

        assert 'href="/site-header.css"' in html
        assert 'src="/site-header.js?v=${version}"' in html
        assert '<div data-forkmesh-header="simple"></div>' in body
        assert 'class="site-header"' not in html
        assert 'class="global-nav"' not in html
        assert 'aria-label="Primary"' not in body
        assert 'id="theme-toggle"' not in html
        assert 'class="mesh-bg"' in html
        assert 'class="mesh-grid"' in html
        assert 'class="legal-card' in html


def test_legal_pages_include_standard_footer_mount():
    for page in LEGAL_PAGES:
        html = _read(page)
        body = html[html.index("<body"):]

        for marker in (
            'href="/site-footer.css"',
            'src="/site-footer.js?v=${version}"',
            '<div data-forkmesh-footer="standard"></div>',
        ):
            assert marker in html

        assert "site-footer-links-grid" not in body
        assert '<footer id="signup"' not in body


def test_legal_pages_preserve_core_content_and_cross_links():
    terms = _read(PUBLIC / "terms.html")
    privacy = _read(PUBLIC / "privacy.html")

    assert "ForkMesh Terms" in terms
    assert "No warranty" in terms
    assert 'href="/privacy"' in terms
    assert "ForkMesh Privacy" in privacy
    assert "Install diagnostics" in privacy
    assert 'href="/terms"' in privacy


if __name__ == "__main__":
    test_legal_pages_use_404_style_simple_nav_only_docs_blog_login()
    test_legal_pages_include_standard_footer_mount()
    test_legal_pages_preserve_core_content_and_cross_links()
