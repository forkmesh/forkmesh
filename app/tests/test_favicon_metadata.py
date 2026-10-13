#!/usr/bin/env python3
"""Static metadata checks for public pages and favicon assets."""

from html.parser import HTMLParser
import json
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[2]
PUBLIC_DIRS = (
    ("app", PROJECT / "app" / "public"),
    ("www", PROJECT / "www" / "public"),
)
WWW_PUBLIC = PROJECT / "www" / "public"
FAVICON_DIR = WWW_PUBLIC / "favicon"
SELF_CONTAINED_DARK_PAGES = {
    "index.html",
}

REQUIRED_HEAD_LINKS = [
    {
        "rel": "icon",
        "href": "/favicon/favicon.ico",
        "sizes": "any",
    },
    {
        "rel": "icon",
        "href": "/favicon/favicon.svg",
        "type": "image/svg+xml",
    },
    {
        "rel": "icon",
        "type": "image/png",
        "sizes": "32x32",
        "href": "/favicon/favicon-32x32.png",
    },
    {
        "rel": "icon",
        "type": "image/png",
        "sizes": "16x16",
        "href": "/favicon/favicon-16x16.png",
    },
    {
        "rel": "apple-touch-icon",
        "sizes": "180x180",
        "href": "/favicon/apple-touch-icon.png",
    },
    {
        "rel": "manifest",
        "href": "/favicon/site.webmanifest",
    },
]

REQUIRED_HEAD_META = {
    "application-name": "ForkMesh",
    "apple-mobile-web-app-title": "ForkMesh",
    "theme-color": "#090909",
    # Site-wide light/dark support (2026-07-11): every page advertises both
    # schemes; site-header.js / static-page.js stamp html.light/html.dark from
    # the visitor's saved choice or OS preference. The self-contained landing
    # pages remain dark-branded and are allowed below.
    "color-scheme": "light dark",
}


class HeadMetadataParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_head = False
        self.links = []
        self.meta = {}
        self.descriptions = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "head":
            self.in_head = True
            return
        if not self.in_head:
            return
        if tag == "link":
            self.links.append(attrs)
        if tag == "meta":
            name = attrs.get("name")
            content = attrs.get("content")
            if name and content is not None:
                self.meta[name] = content
                if name == "description":
                    self.descriptions.append(content)

    def handle_endtag(self, tag):
        if tag == "head":
            self.in_head = False


def _normal_rel(value):
    return " ".join(str(value or "").split())


def _has_link(parser, expected):
    for link in parser.links:
        if _normal_rel(link.get("rel")) != expected["rel"]:
            continue
        if all(link.get(key) == value for key, value in expected.items() if key != "rel"):
            return True
    return False


def _is_generated_feature_blog_page(rel_path, html):
    return (
        rel_path.startswith("blog/")
        and rel_path.endswith("/index.html")
        and "generated from the live ForkMesh feature catalog" in html
    )


def test_public_pages_use_shared_favicon_metadata():
    missing = []
    pages = (
        (owner, public, page)
        for owner, public in PUBLIC_DIRS
        for page in sorted(public.rglob("*.html"))
    )
    for owner, public, page in pages:
        rel_path = page.relative_to(public).as_posix()
        # dashboard/partials/*.html are shell fragments composed into generated
        # dashboard assets before deploy; they have no <head>, so the shared
        # favicon-metadata contract doesn't apply to them.
        if "partials" in page.relative_to(public).parts:
            continue
        html = page.read_text(encoding="utf-8")
        parser = HeadMetadataParser()
        parser.feed(html)

        if _is_generated_feature_blog_page(rel_path, html):
            if not parser.descriptions:
                missing.append(f"{owner}/{rel_path}: meta description")
            if not _has_link(
                    parser, {"rel": "icon", "href": "/favicon/favicon.ico",
                             "sizes": "any"}):
                missing.append(f"{owner}/{rel_path}: link favicon.ico")
            if not _has_link(
                    parser, {"rel": "icon", "href": "/favicon/favicon.svg",
                             "type": "image/svg+xml"}):
                missing.append(f"{owner}/{rel_path}: link favicon.svg")
            continue

        for expected in REQUIRED_HEAD_LINKS:
            if not _has_link(parser, expected):
                missing.append(f"{owner}/{rel_path}: link {expected}")
        for name, content in REQUIRED_HEAD_META.items():
            # The self-contained landing page keeps its dark hero design and
            # is the one page allowed to stay dark-only.
            if (
                name == "color-scheme"
                and owner == "www"
                and rel_path in SELF_CONTAINED_DARK_PAGES
                and parser.meta.get(name) == "dark"
            ):
                continue
            if parser.meta.get(name) != content:
                missing.append(f"{owner}/{rel_path}: meta {name}={content}")
        if not parser.descriptions:
            missing.append(f"{owner}/{rel_path}: meta description")

    assert missing == [], "\n".join(missing)


def test_favicon_manifest_points_at_existing_brand_assets():
    manifest = json.loads((FAVICON_DIR / "site.webmanifest").read_text(encoding="utf-8"))

    assert manifest["name"] == "ForkMesh"
    assert manifest["short_name"] == "ForkMesh"
    assert manifest["theme_color"] == "#090909"
    assert manifest["background_color"] == "#090909"
    assert manifest["display"] == "standalone"
    assert manifest["icons"] == [
        {
            "src": "/favicon/android-chrome-192x192.png",
            "sizes": "192x192",
            "type": "image/png",
        },
        {
            "src": "/favicon/android-chrome-512x512.png",
            "sizes": "512x512",
            "type": "image/png",
        },
    ]

    for icon in manifest["icons"]:
        assert (WWW_PUBLIC / icon["src"].lstrip("/")).is_file()


def test_svg_favicon_is_the_logo():
    assert (FAVICON_DIR / "favicon.svg").read_bytes() == (PROJECT / "logo.svg").read_bytes()


if __name__ == "__main__":
    test_public_pages_use_shared_favicon_metadata()
    test_favicon_manifest_points_at_existing_brand_assets()
    test_svg_favicon_is_the_logo()
