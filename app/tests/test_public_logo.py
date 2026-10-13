#!/usr/bin/env python3
"""Static HTML checks for the public brand logo."""

from html.parser import HTMLParser
from pathlib import Path
import re

from _dashboard_shell import assembled_dashboard


PROJECT = Path(__file__).resolve().parents[2]
APP_PUBLIC = PROJECT / "app" / "public"
WWW_PUBLIC = PROJECT / "www" / "public"
PUBLIC_DIRS = (("app", APP_PUBLIC), ("www", WWW_PUBLIC))
LOGO_SRCS = {"/assets/logo.png", "/assets/logo.svg"}


class BrandLogoParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self._in_brand = False
        self._brand_depth = 0
        self.brand_logos = []
        self.brand_links = 0

    def handle_starttag(self, tag, attrs):
        attr_map = dict(attrs)
        classes = set(attr_map.get("class", "").split())

        if (
            tag == "a"
            and "brand" in classes
            and attr_map.get("href") in {"/", "/dashboard"}
        ):
            self._in_brand = True
            self._brand_depth = 1
            self.brand_links += 1
            return

        if self._in_brand:
            if tag not in {"br", "hr", "img", "input", "link", "meta"}:
                self._brand_depth += 1
            if tag == "img" and "brand-mark" in classes:
                self.brand_logos.append(attr_map)

    def handle_endtag(self, tag):
        if not self._in_brand:
            return

        if tag not in {"br", "hr", "img", "input", "link", "meta"}:
            self._brand_depth -= 1
        if self._brand_depth <= 0:
            self._in_brand = False


class LogoImageParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.logos = []

    def handle_starttag(self, tag, attrs):
        attr_map = dict(attrs)
        if tag == "img" and attr_map.get("src") in LOGO_SRCS:
            self.logos.append(attr_map)


def test_all_public_html_pages_use_logo_in_brand_link():
    assert (WWW_PUBLIC / "assets" / "logo.png").is_file()
    assert (WWW_PUBLIC / "assets" / "logo.svg").is_file()

    # dashboard/partials/*.html are shell fragments (no <head>/brand link); they
    # are composed into generated dashboard assets before deploy, so they are not
    # standalone pages and are exempt from the per-page brand-link contract.
    html_pages = [
        (owner, public, page)
        for owner, public in PUBLIC_DIRS
        for page in sorted(public.rglob("*.html"))
        if "partials" not in page.relative_to(public).parts
    ]
    assert html_pages

    missing = []
    for owner, public, page in html_pages:
        rel = page.relative_to(public).as_posix()
        # dashboard/shell.html is the authored scaffold; its brand link lives
        # in the header partial and every BUILT page document (parsed directly
        # below) carries it inline.
        if rel == "dashboard/shell.html":
            continue
        html = page.read_text(encoding="utf-8")
        if '<div data-forkmesh-header="simple"></div>' in html:
            header_js = (WWW_PUBLIC / "site-header.js").read_text(encoding="utf-8")
            if (
                'href="/"' in header_js
                and 'src="/assets/logo.svg"' in header_js
                and 'aria-hidden="true"' in header_js
            ):
                continue

        parser = BrandLogoParser()
        parser.feed(html)

        has_logo = any(
            logo.get("src") in LOGO_SRCS
            and logo.get("alt") == ""
            and logo.get("aria-hidden") == "true"
            for logo in parser.brand_logos
        )
        if parser.brand_links != 1 or not has_logo:
            missing.append(owner + "/" + rel)

    assert missing == [], "Missing brand logo in: %s" % ", ".join(missing)


def test_brand_logo_size_comes_from_shared_stylesheet():
    css = (WWW_PUBLIC / "styles.css").read_text(encoding="utf-8")
    match = re.search(r"\.brand-mark\s*\{(?P<body>[^}]*)\}", css)

    assert match is not None
    body = match.group("body")
    assert "width: 2rem;" in body
    assert "height: 2.25rem;" in body
    assert "object-fit: contain;" in body
    assert "flex-shrink: 0;" in body
    assert "display: block;" in body


def test_dashboard_assets_are_root_relative_for_deep_links():
    html = (APP_PUBLIC / "dashboard" / "index.html").read_text(encoding="utf-8")

    # Root-relative so deep-link paths (/owner/repo/...) resolve the bundle.
    # The deploy staging pass resolves this shared placeholder to its content
    # hash without checking generated hashes into the document.
    assert 'src="/dashboard.js?v=${version}"' in html
    assert 'href="styles.css"' not in html
    assert 'src="dashboard.js"' not in html


def test_dashboard_logo_does_not_paint_light_background():
    html = assembled_dashboard()
    parser = LogoImageParser()
    parser.feed(html)

    assert parser.logos
    for logo in parser.logos:
        classes = set(logo.get("class", "").split())
        assert "bg-foreground" not in classes
        assert "p-1" not in classes
        assert "rounded-full" not in classes


def test_dark_pages_do_not_wrap_logo_in_light_circle():
    bad_classes = {"bg-foreground", "bg-white", "bg-white/90", "p-1", "rounded-full"}
    offenders = []

    html_pages = [
        (owner, public, page)
        for owner, public in PUBLIC_DIRS
        for page in sorted(public.rglob("*.html"))
        if "partials" not in page.relative_to(public).parts
    ]
    for owner, public, page in html_pages:
        rel = page.relative_to(public).as_posix()
        if rel == "dashboard/shell.html":
            continue
        html = page.read_text(encoding="utf-8")

        is_dark = (
            'class="dark' in html
            or 'data-dashboard-theme="dark"' in html
            or 'content="dark"' in html
            or "content=\"light dark\"" in html
        )
        if not is_dark:
            continue

        parser = LogoImageParser()
        parser.feed(html)
        for logo in parser.logos:
            classes = set(logo.get("class", "").split())
            overlap = sorted(classes & bad_classes)
            if overlap:
                offenders.append("%s/%s: %s" % (owner, rel, " ".join(overlap)))

    assert offenders == []


if __name__ == "__main__":
    test_all_public_html_pages_use_logo_in_brand_link()
    test_brand_logo_size_comes_from_shared_stylesheet()
    test_dashboard_assets_are_root_relative_for_deep_links()
    test_dashboard_logo_does_not_paint_light_background()
    test_dark_pages_do_not_wrap_logo_in_light_circle()
