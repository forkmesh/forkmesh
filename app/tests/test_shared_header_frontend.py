#!/usr/bin/env python3
"""Static contracts for the shared simple page header."""

import re
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[2]
APP_PUBLIC = PROJECT / "app" / "public"
WWW_PUBLIC = PROJECT / "www" / "public"
SIMPLE_HEADER_PAGES = (
    WWW_PUBLIC / "features.html",
    WWW_PUBLIC / "terms.html",
    WWW_PUBLIC / "careers.html",
    WWW_PUBLIC / "changelog.html",
    WWW_PUBLIC / "privacy.html",
    APP_PUBLIC / "status.html",
    APP_PUBLIC / "ledger.html",
    WWW_PUBLIC / "desktop.html",
)


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _css_declarations(css: str, selector: str) -> dict[str, str]:
    match = re.search(
        rf"(?ms)^[ \t]*{re.escape(selector)}[ \t]*\{{(?P<body>[^}}]*)\}}",
        css,
    )
    assert match, f"missing CSS rule for {selector}"
    return {
        name: value.strip()
        for name, value in re.findall(
            r"(?m)^[ \t]*([\w-]+)\s*:\s*([^;]+);",
            match.group("body"),
        )
    }


def _assert_declarations(
    css: str,
    selector: str,
    expected: dict[str, str],
) -> None:
    declarations = _css_declarations(css, selector)
    for name, value in expected.items():
        assert declarations.get(name) == value, (
            f"{selector} must set {name}: {value}; "
            f"found {declarations.get(name)!r}"
        )


def test_shared_simple_header_mounts_on_requested_pages():
    for page in SIMPLE_HEADER_PAGES:
        html = _read(page)
        body = html[html.index("<body") :]

        assert 'href="/site-header.css"' in html, f"{page.name} missing header CSS"
        assert 'src="/site-header.js?v=${version}"' in html, f"{page.name} missing header JS"
        assert '<div data-forkmesh-header="simple"></div>' in body

        assert 'class="site-header"' not in body
        assert 'class="global-nav"' not in body
        assert 'aria-label="Primary"' not in body
        assert "<header" not in body


def test_shared_simple_header_renderer_contains_about_header_contract():
    js = _read(WWW_PUBLIC / "site-header.js")
    css = _read(WWW_PUBLIC / "site-header.css")

    for marker in (
        'querySelectorAll("[data-forkmesh-header]")',
        'aria-label="ForkMesh home"',
        'src="/assets/logo.svg"',
        'aria-label="Primary"',
        'href="/docs"',
        ">Docs</a>",
        'href="/blog"',
        ">Blog</a>",
        'const APP_HREF = MARKETING_HOST',
        '? "https://app.forkmesh.com/"',
        ': "/dashboard";',
        'href="${APP_HREF}"',
        ">Dashboard</a>",
        'const HOME_HREF = MARKETING_HOST ? "/" : "https://forkmesh.com/";',
        '<a href="${HOME_HREF}">Home</a>',
        'href="/login"',
        ">Login</a>",
    ):
        assert marker in js

    for marker in (
        ".forkmesh-simple-header",
        ".forkmesh-simple-header-nav",
        ".forkmesh-simple-brand",
        ".forkmesh-simple-brand-mark",
    ):
        assert marker in css


UNIVERSAL_HEADER_PAGES = SIMPLE_HEADER_PAGES + (
    WWW_PUBLIC / "about.html",
    WWW_PUBLIC / "press.html",
    APP_PUBLIC / "network.html",
    APP_PUBLIC / "chat.html",
    WWW_PUBLIC / "blog.html",
    APP_PUBLIC / "login.html",
    APP_PUBLIC / "signup.html",
    APP_PUBLIC / "forgot-password.html",
    APP_PUBLIC / "reset-password.html",
    APP_PUBLIC / "mirror-payouts.html",
    WWW_PUBLIC / "market.html",
    WWW_PUBLIC / "security-report.html",
    WWW_PUBLIC / "404.html",
    WWW_PUBLIC / "docs" / "index.html",
)


def test_universal_header_mounts_on_every_page_except_home():
    # One shared, session-aware header across the site. The home page keeps
    # its own hero header; the dashboard SPA keeps its in-app chrome. Docs
    # mounts it too, with its own search toolbar below.
    for page in UNIVERSAL_HEADER_PAGES:
        html = _read(page)
        assert 'href="/site-header.css"' in html, f"{page.name} missing header CSS"
        assert 'src="/site-header.js?v=${version}"' in html, f"{page.name} missing header JS"
        assert '<div data-forkmesh-header="simple"></div>' in html, page.name

    home = _read(WWW_PUBLIC / "index.html")
    assert "data-forkmesh-header" not in home


def test_universal_header_is_session_aware():
    # A logged-in visitor sees their account chip (Dashboard / Profile / Log
    # out) instead of the old hardcoded Sign Up / Log In links — the reported
    # bug was /chat showing "Sign Up / Log In" to a logged-in user.
    js = _read(WWW_PUBLIC / "site-header.js")

    assert 'localStorage.getItem("forkmesh.session"' in js
    assert "function buildAccountArea(" in js
    assert 'localStorage.removeItem("forkmesh.session")' in js
    account = js.split("function buildAccountArea(", 1)[1].split("\n  function ", 1)[0]
    assert ">Dashboard<" not in account
    assert 'dash.textContent = "Dashboard"' in js
    assert 'profile.textContent = "Public profile"' in js
    assert "edit.href = MARKETING_HOST" in js
    assert '? "https://app.forkmesh.com/dashboard/settings"' in js
    assert 'edit.textContent = "Settings"' in js
    assert 'out.textContent = "Log out"' in js
    # The avatar itself is a native Settings link; the adjacent button keeps
    # the Dashboard / profile / logout menu available as a separate action.
    assert "settings.href = MARKETING_HOST" in js
    assert 'settings.append(makeAccountAvatar(session))' in js
    assert 'settings.setAttribute("aria-label", "Open account settings")' in js
    assert 'chip.setAttribute("aria-label", `Signed in as ${name}; open account menu`)' in js
    # The membership read after mount re-renders the account area, replacing
    # the chip; the dropdown toggle must be wired on every build, not once at
    # mount, or Profile / Settings never open (issue #657, /ledger).
    build = js[js.index("function buildAccountArea("):js.index("function markCurrentPage(")]
    assert "wireDropdown(wrap);" in build
    assert '.querySelectorAll(".fm-header-menu").forEach(wireDropdown)' not in js
    # Login state stays universal: a login/logout in any other open section
    # re-renders this header's account area via the storage event.
    assert 'window.addEventListener("storage"' in js
    assert 'window.addEventListener("forkmesh:session-restored"' in js
    assert "window.ForkMeshAPI?.restoreSession" in js
    assert 'statusDot.className = "fm-header-session-dot"' in js
    assert 'href="/signup">Sign Up</a>' in js


def test_universal_header_fetches_membership_projection_for_badges():
    js = _read(WWW_PUBLIC / "site-header.js")
    css = _read(WWW_PUBLIC / "site-header.css")
    home = _read(WWW_PUBLIC / "home-header-auth.js")

    for marker in (
        'fetch("/api/accounts/sessions"',
        'cache: "no-store"',
        "function membershipBadgeLabel(membership)",
        'return "STUDIO"',
        'return "BUILDER"',
        'return "PRO"',
        'return "SUPPORTER"',
        'badge.className = "fm-header-plan-badge"',
        "membership.studio === true",
        "membership.tier === \"studio\"",
        "membership.builder === true",
        "membership.tier === \"builder\"",
        "membership.pro === true",
        "membership.tier === \"pro\"",
    ):
        assert marker in js

    # The most specific inherited Patreon tier takes precedence.
    studio_idx = js.index('return "STUDIO"')
    builder_idx = js.index('return "BUILDER"')
    pro_idx = js.index('return "PRO"')
    supporter_idx = js.index('return "SUPPORTER"')
    assert studio_idx < builder_idx < pro_idx < supporter_idx

    # Badge text is never read from mutable localStorage plan fields.
    assert "localStorage.getItem(\"forkmesh.plan\")" not in js
    assert "session.tier" not in js
    assert "session.membership" not in js

    assert ".fm-header-plan-badge" in css
    assert 'fetch("/api/accounts/sessions"' in home
    assert 'badge.className = "fm-header-plan-badge home-auth-plan-badge"' in home


def test_universal_header_organizes_all_pages():
    js = _read(WWW_PUBLIC / "site-header.js")

    # Every site link lives in the hamburger menu, grouped and fully
    # expanded — no nested "More" submenu to open.
    for href in ("/docs", "/chat", "/network", "/blog", "/status",
                 "/ledger", "/features", "/desktop", "/about",
                 "/changelog", "/careers", "/press", "/mirror-payouts",
                 "/security-report", "/privacy", "/terms"):
        assert f'href="{href}"' in js
    assert 'const MARKET_HREF = MARKETING_HOST' in js
    assert ': "https://forkmesh.com/market";' in js
    assert 'href="${MARKET_HREF}">Marketplace ' in js
    # Pricing is a section of the marketing home page, not a page of its own,
    # so the menu entry has to resolve per origin rather than hardcode a path.
    assert 'const PRICING_HREF = MARKETING_HOST' in js
    assert '? "/#pricing"' in js
    assert ': "https://forkmesh.com/#pricing";' in js
    assert 'href="${PRICING_HREF}">Pricing</a>' in js
    assert "fm-nav-group-title" in js
    for group in ("Dashboard", "Product", "Resources", "Community", "Company",
                  "Legal &amp; security", "Account"):
        assert f">{group}</span>" in js
    assert "More <" not in js
    # Current page highlight + hamburger menu.
    assert 'aria-current' in js
    assert "fm-header-burger" in js
    assert "fm-header-mobile" in js
    # Dashboard-chrome parity: version pill, page context, payout shortcut.
    assert "/api/version" in js
    assert "fm-header-context" in js
    assert 'src="/assets/sol.png"' in js


def test_universal_header_keeps_narrow_mobile_pages_inside_the_viewport():
    css = _read(WWW_PUBLIC / "site-header.css")

    assert "@media (max-width: 639.9px)" in css
    assert ".fm-header-account { display: none; }" in css
    assert "@media (max-width: 479.9px)" in css
    assert ".fm-header-app,\n  .fm-header-world { display: none; }" in css
    # All hidden shortcuts remain reachable through the grouped menu.
    js = _read(WWW_PUBLIC / "site-header.js")
    assert '<a href="${APP_HREF}">Dashboard</a>' in js
    assert '<a href="https://world.forkmesh.com/" data-plugin-entry="world">World</a>' in js
    assert 'class="fm-header-mobile-account"' in js


def test_home_header_restores_and_displays_the_active_login():
    home = _read(WWW_PUBLIC / "home-header-auth.js")
    index = _read(WWW_PUBLIC / "index.html")

    assert "window.ForkMeshAPI?.restoreSession" in home
    assert 'status.className = "home-auth-session-dot"' in home
    assert '`Signed in as ${session.nodeName}; open account settings`' in home
    assert ".home-auth-session-dot" in index


def test_universal_header_uses_private_fixed_palette():
    css = _read(WWW_PUBLIC / "site-header.css")

    _assert_declarations(
        css,
        ".forkmesh-simple-header",
        {
            "--fm-header-bg": "#090909",
            "--fm-header-surface": "#141416",
            "--fm-header-fg": "#f5f5f5",
            "--fm-header-muted": "#a3a3a3",
            "--fm-header-border": "#313134",
            "--fm-header-accent": "#2ea043",
            "background": "var(--fm-header-bg)",
            "color": "var(--fm-header-fg)",
            "border-bottom": "1px solid var(--fm-header-border)",
        },
    )
    _assert_declarations(
        css,
        ".fm-header-mobile",
        {
            "background": "var(--fm-header-surface)",
            "border": "1px solid var(--fm-header-border)",
        },
    )
    _assert_declarations(
        css,
        ".fm-header-dropdown",
        {
            "background": "var(--fm-header-surface)",
            "border": "1px solid var(--fm-header-border)",
        },
    )
    _assert_declarations(
        css,
        ".forkmesh-simple-brand",
        {"color": "var(--fm-header-fg)"},
    )
    _assert_declarations(
        css,
        ".fm-nav-group-title",
        {"color": "var(--fm-header-muted)"},
    )
    _assert_declarations(
        css,
        ".fm-header-account-chip:hover",
        {"border-color": "var(--fm-header-accent)"},
    )


def test_universal_header_does_not_reference_host_page_palette():
    css = _read(WWW_PUBLIC / "site-header.css")
    host_tokens = (
        "--background",
        "--bg",
        "--foreground",
        "--fg",
        "--card",
        "--surface2",
        "--muted",
        "--muted-foreground",
        "--border",
        "--accent",
    )
    inherited_references = tuple(
        token
        for token in host_tokens
        if re.search(
            rf"var\(\s*{re.escape(token)}(?:\s*,|\s*\))",
            css,
        )
    )

    assert not inherited_references, (
        f"site-header.css still inherits host page tokens: {inherited_references}"
    )


def test_universal_header_theme_control_is_an_icon_pair():
    # Drawn, not typed: the ☀/☾ glyphs render as a spindly asterisk at 16px
    # and change weight between platforms. The button shows the theme a click
    # moves you to, so exactly one of the two is visible at a time.
    js = _read(WWW_PUBLIC / "site-header.js")
    css = _read(WWW_PUBLIC / "site-header.css")

    assert '"☀"' not in js
    assert '"☾"' not in js
    assert 'class="fm-theme-sun"' in js
    assert 'class="fm-theme-moon"' in js
    # toggleAttribute, not `.hidden`: the icons are <svg> elements, and the
    # `hidden` property belongs to HTMLElement, so assigning it on an SVG
    # never touched the attribute and the swap silently did nothing.
    assert 'sun?.toggleAttribute("hidden", light);' in js
    assert 'moon?.toggleAttribute("hidden", !light);' in js
    assert ".fm-header-theme svg[hidden]" in css


def test_universal_header_signup_is_a_textured_rounded_rectangle():
    css = _read(WWW_PUBLIC / "site-header.css")
    textured = {
        "border": "1px solid var(--fm-btn-primary-border)",
        "border-radius": "0.375rem",
        "background": "var(--fm-btn-primary-fill)",
        "box-shadow": "var(--fm-btn-primary-shadow)",
        "color": "var(--fm-btn-primary-fg)",
    }

    _assert_declarations(css, ".fm-header-signup", textured)
    _assert_declarations(
        css, ".fm-header-mobile-account .fm-header-signup", textured,
    )
    _assert_declarations(
        css,
        ".forkmesh-simple-header",
        {
            "--fm-btn-primary-fg": "#111113",
            "--fm-btn-primary-fill": (
                "linear-gradient(180deg, #ffffff 0%, #e8e8ea 100%)"
            ),
        },
    )
    _assert_declarations(
        css,
        "html.light .forkmesh-simple-header",
        {
            "--fm-btn-primary-fg": "#ffffff",
            "--fm-btn-primary-fill": (
                "linear-gradient(180deg, #2c2c2c 0%, #222222 100%)"
            ),
            "--fm-btn-primary-shadow": (
                "inset 0 1px 0 rgba(255, 255, 255, 0.17), "
                "0 1px 2px rgba(0, 0, 0, 0.16)"
            ),
        },
    )


def test_header_world_is_a_globe_and_the_menu_marks_the_page_in_blue():
    js = _read(WWW_PUBLIC / "site-header.js")
    css = _read(WWW_PUBLIC / "site-header.css")

    world = js.split('class="fm-header-world"', 1)[1].split("</a>", 1)[0]
    assert 'class="fm-header-world-icon"' in world
    assert '<circle cx="12" cy="12" r="9"></circle>' in world
    assert "◉" not in js
    assert "#5eea8b" not in css

    _assert_declarations(
        css, ".forkmesh-simple-header", {"--fm-header-active": "#58a6ff"},
    )
    _assert_declarations(
        css,
        "html.light .forkmesh-simple-header",
        {"--fm-header-active": "#0969da"},
    )
    current = _css_declarations(
        css, '.forkmesh-simple-header-nav a[aria-current="page"] .fm-nav-icon',
    )
    assert current["color"] == "var(--fm-header-active)"
    assert "fm-header-accent" not in _css_declarations(
        css, '.forkmesh-simple-header-nav a[aria-current="page"]',
    )["background"]
    assert ".fm-nav-marketplace .fm-nav-icon" not in css
    assert ".forkmesh-simple-header-nav .fm-nav-marketplace {" not in css

    icons = js.split("const NAV_ICONS = {", 1)[1].split("};", 1)[0]
    nav = js.split("const NAV_HTML = `", 1)[1].split("`;", 1)[0]
    for href in re.findall(r'<a (?:class="[^"]*" )?href="([^"]+)"', nav):
        template = re.fullmatch(r"\$\{(\w+)\}", href)
        key = f"[{template.group(1)}]" if template else f'"{href}"'
        assert f"    {key}: '" in icons, href


def test_blog_posts_mount_universal_header():
    # Blog posts used to carry their own mini header; they now mount the same
    # universal header as the rest of the site.
    posts = sorted((WWW_PUBLIC / "blog").glob("*/index.html"))
    assert posts
    for page in posts:
        html = _read(page)
        assert 'href="/site-header.css"' in html, f"{page} missing header CSS"
        assert 'src="/site-header.js?v=${version}"' in html, f"{page} missing header JS"
        assert '<div data-forkmesh-header="simple"></div>' in html, page
        assert '<header class="top">' not in html, page
        assert 'class="global-nav"' not in html, page
