#!/usr/bin/env python3
"""Contracts for the blog's RSS 2.0 feed (/blog/rss.xml).

The WWW build derives static feeds from the shipped blog index. App retains
the same renderer for self-hosted deployments, so these cover parsing,
rendering, and routing that keeps rss.xml from becoming a post slug.
"""

from pathlib import Path
from xml.etree import ElementTree
import sys


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
SRC = ROOT / "src"
ENTRY_PATH = SRC / "entry.py"
APP_RUNTIME_PATH = SRC / "app_runtime.py"
# The World routes that read the published feed moved into a sibling
# module so Cloudflare's Python startup validation never parses them.
WORLD_ROUTES_PATH = REPOSITORY / "world" / "src" / "world_routes.py"
BLOG_PATH = REPOSITORY / "www" / "public" / "blog.html"

if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import blog_feed  # noqa: E402


CONTENT_NS = "{http://purl.org/rss/1.0/modules/content/}encoded"
MEDIA_NS = "{http://search.yahoo.com/mrss/}content"
ATOM_LINK = "{http://www.w3.org/2005/Atom}link"

_CARD_HTML = (
    '<a class="blog1-card" href="/blog/desktop-node-mirrors/"'
    ' data-published="2026-07-04" data-section="mesh">'
    '<img class="blog1-post-image blog1-has-art"'
    ' src="/assets/blog/features/desktop-node-mirrors.webp" alt="art">'
    '<div class="blog1-card-copy">'
    '<p class="blog1-card-meta">Distributed hosting &amp; more · Feature 01'
    '</p>\n<h3>Desktop-node &amp; mirrors</h3>\n'
    '<p>Actively   mirrored\nrepositories live on <b>independent</b> nodes.'
    '</p></div></a>'
)


def _source(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def test_index_cards_parse_into_feed_entries():
    entries = blog_feed.parse_blog_index(_CARD_HTML)
    assert len(entries) == 1
    entry = entries[0]
    assert entry["slug"] == "desktop-node-mirrors"
    assert entry["title"] == "Desktop-node & mirrors"
    assert entry["category"] == "Distributed hosting & more · Feature 01"
    # Whitespace is flattened and the card's own markup stays out of the text.
    assert entry["summary"] == (
        "Actively mirrored repositories live on <b>independent</b> nodes.")
    assert entry["url"] == "https://forkmesh.com/blog/desktop-node-mirrors/"
    assert entry["image"] == (
        "https://forkmesh.com/assets/blog/features/desktop-node-mirrors.webp")
    assert entry["imageBytes"] == 0
    # The card's publication day, read as midnight UTC.
    assert entry["publishedMs"] == 1783123200000
    # An undated or malformed card still parses; it just has no date.
    undated = blog_feed.parse_blog_index(
        _CARD_HTML.replace(' data-published="2026-07-04"', ""))
    assert undated[0]["publishedMs"] == 0
    assert blog_feed.parse_blog_index(
        _CARD_HTML.replace("2026-07-04", "yesterday"))[0]["publishedMs"] == 0
    # Missing or unrecognizable markup degrades to an empty list.
    assert blog_feed.parse_blog_index(None) == []
    assert blog_feed.parse_blog_index("<html>no cards</html>") == []
    assert blog_feed.build_feed("<html>no cards</html>") == ""


def test_feed_document_is_well_formed_rss_with_preview_text_and_image():
    entries = blog_feed.parse_blog_index(_CARD_HTML)
    entries[0]["imageBytes"] = 123_456
    document = blog_feed.render_rss(entries, 1_700_000_000_000)
    root = ElementTree.fromstring(document)
    assert root.tag == "rss" and root.get("version") == "2.0"
    channel = root.find("channel")
    assert channel.findtext("title") == "ForkMesh Blog"
    assert channel.findtext("link") == "https://forkmesh.com/blog"
    assert channel.findtext("description")
    assert channel.findtext("language") == "en-us"
    # The self link tells readers (and validators) where the feed lives.
    self_link = channel.find(ATOM_LINK)
    assert self_link.get("href") == "https://forkmesh.com/blog/rss.xml"
    assert self_link.get("rel") == "self"
    assert channel.findtext("lastBuildDate") == (
        "Tue, 14 Nov 2023 22:13:20 GMT")
    # The channel's pubDate is the newest item's: readers (and the world's
    # blog board) take it as when the blog last posted.
    assert channel.findtext("pubDate") == "Sat, 04 Jul 2026 00:00:00 GMT"
    items = channel.findall("item")
    assert len(items) == 1
    item = items[0]
    assert item.findtext("title") == "Desktop-node & mirrors"
    assert item.findtext("link") == (
        "https://forkmesh.com/blog/desktop-node-mirrors/")
    assert item.findtext("guid") == item.findtext("link")
    assert item.findtext("category") == (
        "Distributed hosting & more · Feature 01")
    assert item.findtext("pubDate") == "Sat, 04 Jul 2026 00:00:00 GMT"
    # An undated card ships without a pubDate rather than a fake one.
    undated = ElementTree.fromstring(blog_feed.build_feed(
        _CARD_HTML.replace(' data-published="2026-07-04"', "")))
    assert undated.find("channel/pubDate") is None
    assert undated.find("channel/item/pubDate") is None
    # The preview text every reader shows.
    assert item.findtext("description") == (
        "Actively mirrored repositories live on <b>independent</b> nodes.")
    # The artwork, as both the classic enclosure and the media extension.
    image = (
        "https://forkmesh.com/assets/blog/features/desktop-node-mirrors.webp")
    assert item.find("enclosure").get("url") == image
    assert item.find("enclosure").get("type") == "image/webp"
    assert item.find("enclosure").get("length") == "123456"
    assert item.find(MEDIA_NS).get("url") == image
    # The rendered body pairs the image with the same preview text.
    body = item.findtext(CONTENT_NS)
    assert '<img src="%s"' % image in body
    assert "Actively mirrored repositories" in body
    assert "Read the post" in body


def test_feed_renders_every_shipped_blog_post():
    entries = blog_feed.parse_blog_index(_source(BLOG_PATH))
    # The shipped index carries one card per feature post, all with artwork.
    assert len(entries) > 40
    assert all(entry["title"] and entry["summary"] for entry in entries)
    assert all(entry["url"].startswith("https://forkmesh.com/blog/")
               for entry in entries)
    assert all(entry["image"].startswith("https://forkmesh.com/assets/blog/")
               for entry in entries)
    # Every shipped card is dated, so the feed always has a last-post stamp.
    assert all(entry["publishedMs"] > 0 for entry in entries)
    channel = ElementTree.fromstring(
        blog_feed.render_rss(entries)).find("channel")
    assert len(channel.findall("item")) == len(entries)
    newest = channel.find("item")
    assert newest.findtext("title") == "Start your own ForkMesh"
    assert newest.findtext("link") == (
        "https://forkmesh.com/blog/start-your-own-forkmesh/")
    assert newest.findtext("pubDate") == "Wed, 07 Oct 2026 00:00:00 GMT"


def test_feed_parses_back_into_the_entries_it_rendered():
    entries = blog_feed.parse_blog_index(_source(BLOG_PATH))
    round_trip = blog_feed.parse_rss(blog_feed.render_rss(entries))
    assert len(round_trip) == len(entries)
    for before, after in zip(entries, round_trip):
        assert after["title"] == before["title"]
        assert after["summary"] == before["summary"]
        assert after["category"] == before["category"]
        assert after["image"] == before["image"]
        assert after["url"] == before["url"]
        assert after["publishedMs"] == before["publishedMs"]
    assert blog_feed.parse_rss("") == []
    assert blog_feed.parse_rss("<rss><channel/></rss>") == []


def test_markup_and_cdata_cannot_escape_the_document():
    hostile = _CARD_HTML.replace(
        "Desktop-node &amp; mirrors", "]]&gt; &lt;script&gt;x&lt;/script&gt;")
    document = blog_feed.build_feed(hostile)
    # Still parses, and the injected markup survives only as escaped text.
    item = ElementTree.fromstring(document).find("channel/item")
    assert item.findtext("title") == "]]> <script>x</script>"
    assert "<script>x</script>" not in document
    assert "]]>" not in item.findtext(CONTENT_NS)


def test_worker_owns_the_feed_route_and_serves_it_as_rss():
    entry = _source(ENTRY_PATH) + _source(WORLD_ROUTES_PATH)
    runtime = _source(APP_RUNTIME_PATH)
    assert "async def blog_rss_handler(env, request):" in entry
    assert "blog_feed.FEED_PATH" in runtime
    assert 'return await blog_rss_handler(self.env, request)' in runtime
    idx = entry.index("async def blog_rss_handler")
    handler = entry[idx:idx + 2000]
    assert '"application/rss+xml; charset=utf-8"' in handler
    # Built from our own static asset, edge-cached, never a blank feed.
    assert "edge_cache_match(BLOG_RSS_CACHE_KEY)" in handler
    assert "edge_cache_put(BLOG_RSS_CACHE_KEY" in handler
    assert '"blog_feed_unavailable"' in handler
    assert 'method_name(request) not in ("GET", "HEAD")' in handler
    # The world banner reads the published document, not a second render.
    assert "async def _blog_feed_published_document(env, request):" in entry
    assert "await _blog_feed_published_document(env, request)" in entry


def test_self_host_feed_paths_stay_worker_first_without_www_alias_redirects():
    wrangler = _source(ROOT / "wrangler.toml")
    redirects = _source(REPOSITORY / "www" / "public" / "_redirects")
    assert '"/blog/*",' in wrangler
    for path in ("/rss.xml", "/feed.xml"):
        assert '"%s",' % path in wrangler
    for path in ("/rss.xml", "/feed.xml", "/blog/rss.xml", "/blog/feed.xml"):
        assert not any(
            line.startswith(path + " ") for line in redirects.splitlines()
        )


def test_blog_page_advertises_the_feed():
    blog = _source(BLOG_PATH)
    assert ('<link rel="alternate" type="application/rss+xml"'
            ' title="ForkMesh Blog" href="/blog/rss.xml">') in blog
    # And a visible link for readers who do not autodiscover.
    assert '<a class="blog1-feed-link" href="/blog/rss.xml">' in blog
