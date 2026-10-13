#!/usr/bin/env python3

import re
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
WWW_PUBLIC = REPOSITORY / "www" / "public"
PAGE = (WWW_PUBLIC / "mobile.html").read_text(encoding="utf-8")
STYLES = (WWW_PUBLIC / "mobile.css").read_text(encoding="utf-8")
APP_STORE = "https://apps.apple.com/app/forkmesh/id6803060735"


def test_the_page_wears_the_homepage_chrome_and_metadata():
    assert '<link rel="canonical" href="https://forkmesh.com/mobile" />' in PAGE
    assert '<meta property="og:url" content="https://forkmesh.com/mobile" />' in PAGE
    assert '<meta name="apple-itunes-app" content="app-id=6803060735" />' in PAGE
    for asset in ("/tailwind.css", "/home-nav.css", "/mobile.css", "/site-footer.css"):
        assert f'<link rel="stylesheet" href="{asset}" />' in PAGE, asset
    for script in ("/home-nav.js", "/home-theme.js", "/home-header-auth.js", "/site-footer.js"):
        assert f'<script src="{script}?v=${{version}}" defer></script>' in PAGE, script
    assert '<header class="home-nav-wrap">' in PAGE
    assert '<svg class="hero-schematic"' in PAGE
    assert '<g id="hero-schematic-art">' in PAGE
    assert '<div data-forkmesh-footer="landing" data-forkmesh-footer-word="hidden"></div>' in PAGE


def test_the_hero_says_now_in_your_pocket():
    hero = PAGE[PAGE.index('<h1 id="hero-title"'):PAGE.index("</h1>")]
    assert "<span>Your Code. Your World.</span>" in hero
    assert "<span>Now in Your Pocket.</span>" in hero


def test_the_page_navigation_marks_itself_and_leaves_no_homepage_anchors():
    nav = PAGE[PAGE.index('<header class="home-nav-wrap">'):PAGE.index("</header>")]
    assert '<a class="home-menu-item" href="/mobile" aria-current="page">' in nav
    assert '<a href="/mobile" aria-current="page">Mobile app</a>' in nav
    assert 'href="#' not in nav
    assert "data-forkmesh-membership" not in nav


def test_download_buttons_point_at_the_store_listing_and_the_signed_apk():
    assert PAGE.count(f'href="{APP_STORE}"') == 2
    assert PAGE.count('href="/#downloads"') == 2
    assert f'"downloadUrl": "{APP_STORE}"' in PAGE


def test_every_screenshot_is_published_and_every_published_one_is_used():
    used = set(re.findall(r'src="/assets/mobile/([a-z0-9-]+\.webp)"', PAGE))
    published = {path.name for path in (WWW_PUBLIC / "assets" / "mobile").glob("*.webp")}
    assert used == published
    assert (WWW_PUBLIC / "assets" / "mobile" / "og.png").is_file()
    for tag in re.findall(r"<img [^>]*>", PAGE):
        assert "alt=" in tag, tag
        assert 'width="' in tag and 'height="' in tag, tag


def test_sections_carry_no_green_kicker():
    assert "kicker" not in PAGE
    assert "kicker" not in STYLES
    assert "--cap-accent" not in STYLES


def test_buttons_use_the_homepage_texture():
    for token in (
        "--btn-primary-fill: linear-gradient(180deg, #ffffff 0%, #e8e8ea 100%);",
        "--btn-primary-fill: linear-gradient(180deg, #2c2c2c 0%, #222222 100%);",
        "--btn-secondary-fill: linear-gradient(180deg, #ffffff 0%, #fcfcfc 100%);",
        "--btn-secondary-border: #e1e2e3;",
    ):
        assert token in STYLES, token


def test_desktop_control_copy_keeps_the_cryptographic_boundary_precise():
    assert "Live desktop-control\n              payloads stay encrypted end to end" in PAGE
    assert "desktop merges and approvals use its authenticated, encrypted control channel" in PAGE
    assert "merges and approvals are signed on the phone" not in PAGE
    assert "with no host in the middle" not in PAGE


def test_face_id_copy_names_only_the_actions_that_ask_for_it():
    assert "Face ID for sensitive actions" in PAGE
    assert "Face ID before anything destructive" not in PAGE
    assert "Deleting, revoking and resetting all ask for Face ID" not in PAGE


def test_availability_lines_promise_only_what_ships():
    meta = re.search(r'<meta name="description" content="([^"]*)"', PAGE).group(1)
    systems = re.search(r'"operatingSystem": "([^"]*)"', PAGE).group(1)
    hero_note = re.search(r'<p class="mob-note[^"]*">(.*?)</p>', PAGE).group(1)
    call = re.search(r"<p>(Free on the App Store[^<]*)</p>", PAGE).group(1)
    assert systems == "iOS, iPadOS, Android"
    assert hero_note == "Free<i>&middot;</i>iPhone and iPad<i>&middot;</i>Android as a signed APK"
    assert call == "Free on the App Store for iPhone and iPad, and as a signed APK for Android."
    for line in (meta, systems, hero_note, call):
        assert "Watch" not in line and "watchOS" not in line, line
