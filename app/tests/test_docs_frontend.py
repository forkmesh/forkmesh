#!/usr/bin/env python3
"""Static contract tests for the public docs page shell."""

import re
from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
DOCS_PAGES = (PUBLIC / "docs" / "index.html",)
CONTRIBUTION_GUIDE = PUBLIC / "docs" / "contributions" / "index.html"


def _read(page: Path) -> str:
    return page.read_text(encoding="utf-8")


def test_contribution_counting_guide_documents_native_public_ledger():
    assert CONTRIBUTION_GUIDE.is_file()
    html = _read(CONTRIBUTION_GUIDE)
    docs_index = _read(PUBLIC / "docs" / "index.html")

    for marker in (
        "How ForkMesh counts contributions",
        "Commits",
        "Issues",
        "Pull requests",
        "Reviews",
        "Repositories",
        "public repositories only",
        "configured Git identity",
        "Mirrors",
        "Languages",
        "Partial history",
        "does not depend on GitHub",
    ):
        assert marker in html

    assert '<link rel="stylesheet" href="/site-header.css"' in html
    assert '<script src="/site-header.js?v=${version}"' in html
    assert '<div data-forkmesh-header="simple"></div>' in html
    assert 'href="/docs/contributions"' in docs_index


def test_docs_pages_use_local_tailwind_and_page_local_styles():
    for page in DOCS_PAGES:
        html = _read(page)

        assert 'href="/tailwind.css"' in html
        assert "https://cdn.tailwindcss.com" not in html
        assert 'href="styles.css"' not in html
        assert 'href="../styles.css"' not in html
        assert "<style>" in html


def test_docs_pages_expose_platform_tabs_with_coming_soon_states():
    for page in DOCS_PAGES:
        html = _read(page)

        assert ">Desktop<" in html
        assert ">Extension<" in html
        assert ">Mobile<" in html
        assert html.count("Coming soon") >= 2
        assert 'aria-disabled="true"' in html


def test_docs_nav_spans_the_viewport_with_edge_pinned_sidebars():
    for page in DOCS_PAGES:
        html = _read(page)

        # The shell is no longer a centered max-width container: the section
        # nav hugs the left edge and the outline hugs the right edge.
        assert "max-w-[1540px]" not in html
        assert 'class="docs-shell grid w-full ' in html
        assert 'class="docs-sidebar-left hidden lg:block"' in html
        assert 'class="docs-sidebar-right hidden lg:block"' in html
        assert "justify-self: start" in html
        assert "justify-self: end" in html
        assert "grid-template-columns: 270px minmax(0, 1fr) 270px" in html


def test_docs_nav_exposes_high_level_section_links():
    for page in DOCS_PAGES:
        html = _read(page)

        tabs = re.search(
            r'<nav\b[^>]*aria-label="Product documentation"[^>]*>(.*?)</nav>',
            html,
            flags=re.DOTALL,
        )
        assert tabs, page
        strip = tabs.group(1)
        for href, label in (
            ("#welcome", "Desktop"),
            ("#install", "Install"),
            ("#quickstart", "Quickstart"),
            ("#repository", "Concepts"),
            ("#api-websockets", "APIs"),
            ("#self-hosting", "Self-hosting"),
            ("/docs/protocol", "Protocol spec"),
            ("/docs/onboarding", "Team onboarding"),
            ("#library", "Library"),
        ):
            assert f'href="{href}"' in strip, (page, href)
            assert f">{label}<" in strip, (page, label)
        # Every anchor the strip points at exists on the page.
        for anchor in re.findall(r'href="#([^"]+)"', strip):
            assert f'id="{anchor}"' in html, (page, anchor)


def test_docs_pages_keep_reference_like_docs_shell():
    for page in DOCS_PAGES:
        html = _read(page)

        for marker in (
            'class="docs-shell',
            "Search...",
            "Copy page",
            "On this page",
            "The ForkMesh Mental Model",
            "Was this page helpful?",
            "Install ForkMesh Desktop",
        ):
            assert marker in html


def test_docs_pages_use_home_link_instead_of_github():
    for page in DOCS_PAGES:
        html = _read(page)

        # Home navigation now comes from the universal header's brand link
        # (rendered by site-header.js); the point of this test is that no
        # GitHub links crept back in.
        assert ">GitHub<" not in html
        assert "ForkMesh repository" not in html
        assert 'href="/mainnode/forkmesh"' not in html


def test_docs_pages_mount_the_universal_site_header():
    # The shared header is the sole visible theme controller.
    # The docs toolbar keeps only its search and product tabs beneath it.
    for page in DOCS_PAGES:
        html = _read(page)

        assert 'href="/site-header.css"' in html
        assert 'src="/site-header.js?v=${version}"' in html
        assert '<div data-forkmesh-header="simple"></div>' in html
        # The duplicated chrome is gone; docs-specific tools stay.
        assert 'aria-label="Docs header"' not in html
        assert ">Contact Us<" not in html
        assert 'id="theme-toggle"' not in html
        assert "function applyTheme" not in html
        docs_header = re.search(r"<header\b[^>]*>", html, flags=re.DOTALL)
        assert docs_header, page
        class_attribute = re.search(
            r'class="([^"]*)"',
            docs_header.group(0),
        )
        assert class_attribute, page
        header_classes = set(class_attribute.group(1).split())
        assert {"sticky", "top-0", "z-30"} <= header_classes, page
        assert "z-50" not in header_classes, page
        assert 'id="docs-search"' in html


def test_docs_pages_keep_the_desktop_only_single_column_search_row():
    for page in DOCS_PAGES:
        html = _read(page)

        assert (
            'class="hidden h-20 w-full '
            'grid-cols-1 items-center px-5 sm:px-8 md:grid lg:px-8"'
        ) in html
        assert ".icon-button" not in html
        assert "html.dark .moon" not in html
        assert "html:not(.dark) .sun" not in html
        assert "Map the docs theme onto the universal header" not in html
        assert "--foreground: var(--docs-fg)" not in html


def test_docs_pages_use_reduced_type_scale():
    for page in DOCS_PAGES:
        html = _read(page)

        for old_size in (
            "text-[2.65rem]",
            "text-[2.35rem]",
            "text-[2rem]",
            "text-[1.7rem]",
            "text-[1.55rem]",
            "text-[1.35rem]",
            "text-[1.22rem]",
            "text-[1.15rem]",
            "text-[1.08rem]",
        ):
            assert old_size not in html

        for new_size in (
            "text-[2.25rem]",
            "text-[1.38rem]",
            "text-[1.78rem]",
            "text-[1.5rem]",
            "text-[1.1rem]",
            "text-[1.02rem]",
        ):
            assert new_size in html


def test_docs_pages_include_working_inline_search():
    for page in DOCS_PAGES:
        html = _read(page)

        for marker in (
            'id="docs-search"',
            'id="docs-search-panel"',
            'data-search-title="Repository model"',
            "function renderDocsSearchResults",
            "docsSearchIndex",
            "event.metaKey || event.ctrlKey",
        ):
            assert marker in html


def test_docs_logo_keeps_same_pixels_across_theme_modes():
    for page in DOCS_PAGES:
        html = _read(page)

        assert "html.dark .brand-mark" not in html
        assert "filter: invert" not in html


def test_docs_pages_wire_helpfulness_feedback():
    for page in DOCS_PAGES:
        html = _read(page)

        for marker in (
            "data-feedback-like",
            "data-feedback-dislike",
            "data-feedback-status",
            "data-feedback-modal",
            "data-feedback-form",
            "data-feedback-message",
            "What was confusing, missing, or could be improved?",
            'fetch("/api/feedback"',
            'source: "docs"',
            'vote: "like"',
            'vote: "dislike"',
        ):
            assert marker in html


def test_docs_pages_publish_api_and_operational_reference():
    for page in DOCS_PAGES:
        html = _read(page)

        for marker in (
            'id="api-websockets"',
            "APIs and WebSockets",
            "/api/repo/&lt;owner&gt;/&lt;repo&gt;/host",
            "/api/poll",
            'id="bounty-custody"',
            "Bounty custody and payouts",
            'id="installers-updates"',
            "Installers and updates",
            'id="developer-integrations"',
            "Developer integrations",
            'id="self-hosting"',
            "Self-hosting and development",
        ):
            assert marker in html
