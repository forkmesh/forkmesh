#!/usr/bin/env python3
"""Static contract tests for the public docs page shell."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "www" / "public"
DOCS_CSS = PUBLIC / "docs-page.css"
DOCS_JS = PUBLIC / "docs-page.js"
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


def test_docs_pages_link_the_platform_tabs():
    for page in DOCS_PAGES:
        html = _read(page)

        platforms = re.search(r'<nav class="dp-platforms"[^>]*>(.*?)</nav>', html, flags=re.DOTALL)
        assert platforms, page
        assert '<a class="active" href="/docs/">Desktop</a>' in platforms.group(1)
        assert '<a class="" href="/docs/browser-extension/">Extension</a>' in platforms.group(1)
        assert '<a class="" href="/docs/mobile/">Mobile</a>' in platforms.group(1)
        assert "Coming soon" not in html
        assert "coming soon" not in html
        assert 'aria-disabled="true"' not in html
        assert (ROOT / "docs" / "browser-extension.md").is_file()
        assert (ROOT / "docs" / "mobile" / "README.md").is_file()


def test_docs_top_nav_stays_put_on_a_phone_and_tabs_only_scroll_sideways():
    css = _read(DOCS_CSS)
    tabs = re.findall(r"\n\.dp-tabs \{(.*?)\}", css, flags=re.DOTALL)
    assert any("overflow-y: hidden" in rule and "touch-action: pan-x" in rule for rule in tabs)
    for page in DOCS_PAGES:
        html = _read(page)
        phone = re.search(r"@media \(max-width: 767px\) \{(.*?)\n      \}", html, flags=re.DOTALL)
        assert phone, page
        assert "position: sticky" in phone.group(1)
        assert ".dp-toolbar" in phone.group(1)


def test_docs_grids_cannot_widen_the_page_on_a_phone():
    pages = {
        PUBLIC / "docs" / "index.html": ".route-row",
        PUBLIC / "mirror.html": ".mirror-grid",
    }
    for page, selector in pages.items():
        html = _read(page)
        assert "grid-template-columns: 1fr;" not in html, page
        assert selector in html, page
    assert "dl { grid-template-columns: minmax(0, 1fr); }" in _read(
        PUBLIC / "docs" / "git-compatibility" / "index.html"
    )
    assert ".doc-body table { display: block; overflow-x: auto; }" in _read(
        PUBLIC / "docs" / "protocol" / "index.html"
    )


def test_docs_landing_shares_the_library_shell():
    import sys

    sys.path.insert(0, str(ROOT / "app" / "tools"))
    import docs_site

    for page in DOCS_PAGES:
        html = _read(page)

        assert 'href="/docs-page.css?v=${version}"' in html
        assert 'src="/docs-page.js?v=${version}"' in html
        assert 'src="/docs-explore.js?v=${version}"' in html
        assert 'class="dp-body dp-home"' in html
        assert '<main class="dp-shell">' in html
        assert '<aside class="dp-sidebar">' in html
        assert '<aside class="dp-rail">' in html
        assert docs_site.SIDEBAR_PLACEHOLDER in html
        assert "grid-template-columns: 272px minmax(0, 1fr) 248px" in _read(DOCS_CSS)


def test_docs_nav_exposes_high_level_section_links():
    import sys

    sys.path.insert(0, str(ROOT / "app" / "tools"))
    import docs_site

    for page in DOCS_PAGES:
        html = _read(page)

        tabs = re.search(
            r'<nav\b[^>]*aria-label="Documentation areas"[^>]*>(.*?)</nav>',
            html,
            flags=re.DOTALL,
        )
        assert tabs, page
        strip = tabs.group(1)
        for href, label in docs_site.DOCS_TABS:
            assert f'href="{href}"' in strip, (page, href)
            assert f">{label}<" in strip, (page, label)
        assert '<a class="active" href="/docs/">Overview</a>' in strip
        for anchor in re.findall(r'href="/docs/#([^"]+)"', strip):
            assert f'id="{anchor}"' in html, (page, anchor)


def test_docs_landing_leads_with_the_six_pillars():
    for page in DOCS_PAGES:
        html = _read(page)

        pillars = re.search(r'<ol class="dh-pillars">(.*?)</ol>\s*</section>', html, flags=re.DOTALL)
        assert pillars, page
        cards = re.findall(r'<li\s+class="dh-pillar"', pillars.group(1))
        assert len(cards) == 6
        for title in (
            "Resilient mirroring and distribution",
            "Censorship and security threat model",
            "Security and cryptography",
            "Self-hosting a small team can run",
            "Tested under real blocking",
            "Fits the workflow you already have",
        ):
            assert title in pillars.group(1), title
        assert pillars.group(1).count('class="dp-art"') == 6
        assert 'id="who-it-is-for"' in html
        assert html.index('id="pillars"') < html.index('id="mental-model"')
        for href in re.findall(r'href="(/docs/[^"#]+)', pillars.group(1)):
            relative = href[len("/docs/"):].strip("/")
            assert (
                (ROOT / "docs" / f"{relative}.md").is_file()
                or (ROOT / "docs" / relative / "README.md").is_file()
            ), href


def test_docs_pages_keep_reference_like_docs_shell():
    for page in DOCS_PAGES:
        html = _read(page)

        for marker in (
            'class="dp-shell',
            "Search docs...",
            "Copy link",
            "On this page",
            "The six pillars",
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
    for page in DOCS_PAGES:
        html = _read(page)

        assert 'href="/site-header.css"' in html
        assert 'src="/site-header.js?v=${version}"' in html
        assert '<div data-forkmesh-header="simple"></div>' in html
        assert 'aria-label="Docs header"' not in html
        assert ">Contact Us<" not in html
        assert 'id="theme-toggle"' not in html
        assert "function applyTheme" not in html
        docs_header = re.search(r"<header\b[^>]*>", html, flags=re.DOTALL)
        assert docs_header, page
        assert 'class="dp-toolbar"' in docs_header.group(0), page
        assert 'id="docs-search"' in html
    toolbar = re.search(r"\n\.dp-toolbar \{(.*?)\}", _read(DOCS_CSS), flags=re.DOTALL)
    assert toolbar
    assert "position: sticky" in toolbar.group(1)
    assert "z-index: 30" in toolbar.group(1)


def test_docs_pages_keep_the_desktop_only_single_column_search_row():
    for page in DOCS_PAGES:
        html = _read(page)

        assert '<div class="dp-toolbar-inner">' in html
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
            "text-[1.78rem]",
            "text-[1.5rem]",
            "text-[1.1rem]",
        ):
            assert new_size in html


def test_docs_pages_include_working_inline_search():
    script = _read(DOCS_JS)
    for marker in ("function renderDocsSearchResults", "docsSearchIndex", "event.metaKey || event.ctrlKey"):
        assert marker in script
    for page in DOCS_PAGES:
        html = _read(page)

        for marker in (
            'id="docs-search"',
            'id="docs-search-panel"',
            'data-search-title="Repository model"',
            'data-search-title="The six pillars"',
            'src="/docs-page.js?v=${version}"',
        ):
            assert marker in html


def test_docs_logo_keeps_same_pixels_across_theme_modes():
    for page in DOCS_PAGES:
        html = _read(page)

        assert "html.dark .brand-mark" not in html
        assert "filter: invert" not in html


def test_docs_pages_wire_helpfulness_feedback():
    script = _read(DOCS_JS)
    for marker in ('fetch("/api/feedback"', 'source: "docs"', 'vote: "like"', 'vote: "dislike"'):
        assert marker in script
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
