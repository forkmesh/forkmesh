import re
import shutil
import sys
import tomllib
from pathlib import Path
from urllib.parse import urlsplit
from xml.etree import ElementTree


WWW = Path(__file__).resolve().parents[1]
REPOSITORY = WWW.parent
PUBLIC = WWW / "public"
WRANGLER = tomllib.loads((WWW / "wrangler.toml").read_text(encoding="utf-8"))
APP_TOOLS = REPOSITORY / "app" / "tools"
if str(APP_TOOLS) not in sys.path:
    sys.path.insert(0, str(APP_TOOLS))

import build_site_assets  # noqa: E402


def redirect_rules():
    rules = []
    for raw_line in (PUBLIC / "_redirects").read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line:
            rules.append(tuple(line.split()[:3]))
    return rules


def test_blog_pages_and_built_feeds_stay_out_of_python():
    assets = WRANGLER["assets"]
    worker_first = assets["run_worker_first"]
    assert assets["html_handling"] == "auto-trailing-slash"
    assert "/blog" not in worker_first
    assert "/blog/*" not in worker_first
    for path in ("/rss.xml", "/feed.xml", "/blog/rss.xml", "/blog/feed.xml"):
        assert path in worker_first
    assert (PUBLIC / "blog.html").is_file()
    assert list((PUBLIC / "blog").glob("*/index.html"))


def test_docs_pages_are_static_directory_assets():
    worker_first = WRANGLER["assets"]["run_worker_first"]
    assert WRANGLER["assets"]["not_found_handling"] == "none"
    assert "/docs" not in worker_first
    assert "/docs/*" not in worker_first
    assert not (PUBLIC / "docs.html").exists()
    assert (PUBLIC / "docs/index.html").is_file()
    assert list((PUBLIC / "docs").glob("*/index.html"))


def test_blog_redirects_have_one_canonical_destination():
    rules = redirect_rules()
    assert ("/blog", "/blog.html", "200") not in rules
    assert ("/blog/", "/blog", "308") in rules
    assert ("/blogs", "/blog", "308") in rules
    assert ("/blog/:slug", "/blog/:slug/", "308") in rules
    assert ("/blog/:slug/", "/blog/:slug/index.html", "200") not in rules


def test_feed_aliases_do_not_redirect_or_fall_into_article_redirects():
    rules = redirect_rules()
    assert ("/blog/:slug", "/blog/:slug/", "308") in rules
    feed_paths = {"/rss.xml", "/feed.xml", "/blog/rss.xml", "/blog/feed.xml"}
    assert not any(source in feed_paths for source, _, _ in rules)


def test_www_feed_artifacts_are_deterministic_and_use_exact_image_sizes(tmp_path):
    destination = tmp_path / "public"
    shutil.copytree(PUBLIC, destination)
    build_site_assets.build_www_feeds(destination)
    first = {
        relative: (destination / relative).read_bytes()
        for relative in build_site_assets.WWW_FEED_ASSETS
    }
    build_site_assets.build_www_feeds(destination)
    second = {
        relative: (destination / relative).read_bytes()
        for relative in build_site_assets.WWW_FEED_ASSETS
    }
    assert first == second
    assert len(set(first.values())) == 1

    channel = ElementTree.fromstring(first["rss.xml"]).find("channel")
    items = channel.findall("item")
    entries = build_site_assets.blog_feed.parse_blog_index(
        (PUBLIC / "blog.html").read_text(encoding="utf-8")
    )
    assert len(items) == len(entries) > 40
    assert channel.findtext("lastBuildDate") == channel.findtext("pubDate")
    for item, entry in zip(items, entries):
        enclosure = item.find("enclosure")
        image_path = destination / urlsplit(entry["image"]).path.lstrip("/")
        assert int(enclosure.get("length")) == image_path.stat().st_size > 0


def test_www_feed_build_stays_deterministic_without_published_dates(tmp_path):
    destination = tmp_path / "public"
    image = destination / "assets" / "post.webp"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"image")
    (destination / "blog.html").write_text(
        '<a class="blog1-card" href="/blog/post/">'
        '<img class="blog1-post-image" src="/assets/post.webp">'
        '<p class="blog1-card-meta">Feature</p><h3>Post</h3>'
        '<p>Summary</p></a>',
        encoding="utf-8",
    )
    build_site_assets.build_www_feeds(destination)
    first = (destination / "rss.xml").read_bytes()
    build_site_assets.build_www_feeds(destination)
    assert (destination / "rss.xml").read_bytes() == first
    channel = ElementTree.fromstring(first).find("channel")
    assert channel.findtext("lastBuildDate") == "Thu, 01 Jan 1970 00:00:00 GMT"
    assert channel.find("pubDate") is None
    assert channel.find("item/enclosure").get("length") == "5"


def test_docs_redirects_defer_to_directory_html_handling():
    rules = redirect_rules()
    assert ("/docs", "/docs/index.html", "200") not in rules
    assert ("/docs/", "/docs", "308") not in rules
    assert ("/docs", "/docs/", "308") in rules
    assert ("/docs.html", "/docs/", "308") in rules
    assert ("/docs/:slug", "/docs/:slug/", "308") not in rules
    assert ("/docs/:slug/", "/docs/:slug/index.html", "200") not in rules
    assert WRANGLER["assets"]["html_handling"] == "auto-trailing-slash"


def test_docs_json_assets_are_not_redirected():
    patterns = []
    for source, _, _ in redirect_rules():
        if source.startswith("#") or ":" not in source and "*" not in source:
            continue
        parts = []
        for segment in source.split("/"):
            if segment.startswith(":"):
                parts.append("[^/]+")
            else:
                parts.append(".*".join(re.escape(piece) for piece in segment.split("*")))
        patterns.append((source, re.compile("/".join(parts))))
    assets = sorted((WWW / "docs").rglob("*.json"))
    assert assets
    for asset in assets:
        path = "/docs/" + asset.relative_to(WWW / "docs").as_posix()
        caught = [source for source, pattern in patterns if pattern.fullmatch(path)]
        assert not caught, (path, caught)


def test_marketplace_ide_docs_redirect_to_market():
    rules = redirect_rules()
    for source in ("/docs/ide-extension", "/docs/ide-extension/"):
        assert (source, "/market", "308") in rules
    for source in (
        "/docs/ide-extension-protocol",
        "/docs/ide-extension-protocol/",
    ):
        assert (source, "/market", "308") in rules
    assert not any(
        source in {"/blog/vs-code-extension", "/blog/vs-code-extension/"}
        for source, _target, _status in rules
    )


def test_flat_marketing_pages_defer_to_html_handling():
    rules = redirect_rules()
    worker_first = WRANGLER["assets"]["run_worker_first"]
    for page in (
        "about",
        "careers",
        "changelog",
        "desktop",
        "features",
        "press",
        "privacy",
        "security-report",
        "terms",
    ):
        assert (PUBLIC / f"{page}.html").is_file()
        assert (f"/{page}", f"/{page}.html", "200") not in rules
        # Flat pages pass through the Worker only so acquisition attribution
        # can observe them (docs/attribution.md); the Worker still resolves
        # the clean URL through the assets binding's html_handling.
        assert f"/{page}" in worker_first
    assert [rule for rule in rules if rule[2] == "200"] == [
        ("/favicon.ico", "/favicon/favicon.ico", "200"),
        ("/apple-touch-icon.png", "/favicon/apple-touch-icon.png", "200"),
        ("/apple-touch-icon-precomposed.png", "/favicon/apple-touch-icon.png", "200"),
    ]
