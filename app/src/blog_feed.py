"""RSS 2.0 feed for the ForkMesh blog.

The blog is a generated static page (``public/blog.html``): one card per
feature post carrying its artwork, section/feature meta line, title, and
blurb. The WWW build derives its static feeds from that shipped index, while
self-hosted App deployments can use the same pure renderer at request time.

The world's blog banner rides the same document: the Worker builds the feed
once per read and :mod:`world_social_feeds` parses it back into banner posts,
so the board shows subscribers' preview text and artwork rather than a second
scrape of the HTML.

Everything here is pure string work, so the parsing and escaping rules stay
testable without the Cloudflare/Pyodide runtime.
"""

import re
from datetime import datetime, timezone
from email.utils import formatdate, parsedate_to_datetime
from html import unescape as _unescape


SITE_URL = "https://forkmesh.com"
BLOG_URL = SITE_URL + "/blog"
BLOG_INDEX_ASSET = "blog.html"
FEED_PATH = "/blog/rss.xml"
FEED_URL = SITE_URL + FEED_PATH
FEED_TITLE = "ForkMesh Blog"
FEED_DESCRIPTION = (
    "Every ForkMesh feature explained as a short technical post: mirrors, "
    "signed collaboration, AI agents, CI, encrypted chat, rewards, and apps."
)
FEED_LANGUAGE = "en-us"
FEED_IMAGE = SITE_URL + "/assets/blog/features/desktop-node-mirrors.webp"
# A generous ceiling on a hand-generated index: the whole catalog fits well
# under it, and a mangled page can never produce an unbounded document.
FEED_LIMIT = 200
MAX_TITLE = 200
MAX_SUMMARY = 600
MAX_CATEGORY = 120

# One feature card on the static blog index. The cards are uniform generated
# markup: anchor, artwork, meta line, title, then the blurb. The image is
# optional so a card that ships without art still becomes a feed item, and
# data-published (the post's publication day) is optional so an undated card
# still parses — it simply ships without a pubDate.
_CARD_RE = re.compile(
    r'<a class="blog1-card" href="(?P<href>/blog/[^"]+)"'
    r'(?:\s+data-published="(?P<published>[^"]*)")?[^>]*>'
    r'\s*(?:<img[^>]*class="blog1-post-image[^"]*"[^>]*'
    r'src="(?P<image>[^"]*)"[^>]*>)?'
    r'.*?<p class="blog1-card-meta">(?P<meta>.*?)</p>'
    r'\s*<h3>(?P<title>.*?)</h3>'
    r'\s*<p>(?P<summary>.*?)</p>',
    re.DOTALL,
)

_ITEM_RE = re.compile(r"<item>(?P<item>.*?)</item>", re.DOTALL)
_ENCLOSURE_RE = re.compile(r'<enclosure[^>]*url="(?P<url>[^"]*)"')

_IMAGE_TYPES = (
    (".webp", "image/webp"),
    (".png", "image/png"),
    (".jpg", "image/jpeg"),
    (".jpeg", "image/jpeg"),
    (".avif", "image/avif"),
    (".gif", "image/gif"),
    (".svg", "image/svg+xml"),
)


def _text(value, limit):
    flat = re.sub(r"\s+", " ", _unescape(str(value or ""))).strip()
    return flat[:limit]


def _absolute(url):
    """Site-absolute URL for a card link or image, or "" for anything else."""
    raw = str(url or "").strip()
    if raw.startswith("https://") or raw.startswith("http://"):
        return raw
    if raw.startswith("/"):
        return SITE_URL + raw
    return ""


def published_ms(value):
    """Epoch milliseconds for a card's ``data-published`` day, or 0.

    Cards stamp the publication day (``YYYY-MM-DD``); the feed needs an
    instant, so the day is read as midnight UTC. Anything else — a missing
    attribute, a malformed day — is "unknown" rather than an error, and the
    item then ships without a pubDate.
    """
    match = re.match(r"^(\d{4})-(\d{2})-(\d{2})$", str(value or "").strip())
    if not match:
        return 0
    try:
        stamp = datetime(
            int(match.group(1)), int(match.group(2)), int(match.group(3)),
            tzinfo=timezone.utc)
    except ValueError:
        return 0
    return int(stamp.timestamp() * 1000)


def image_mime_type(url):
    lowered = str(url or "").lower().split("?")[0]
    for suffix, mime in _IMAGE_TYPES:
        if lowered.endswith(suffix):
            return mime
    return "image/webp"


def escape_xml(value):
    return (
        str(value or "")
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def parse_blog_index(html):
    """Bound the blog index's feature cards to the feed's entry shape."""
    entries = []
    for match in _CARD_RE.finditer(str(html or "")):
        title = _text(match.group("title"), MAX_TITLE)
        if not title:
            continue
        href = match.group("href")
        entries.append({
            "slug": _text(href.strip("/").split("/")[-1], 80),
            "title": title,
            "summary": _text(match.group("summary"), MAX_SUMMARY),
            "category": _text(match.group("meta"), MAX_CATEGORY),
            "image": _absolute(match.group("image")),
            "imageBytes": 0,
            "url": _absolute(href),
            "publishedMs": published_ms(match.group("published")),
        })
        if len(entries) >= FEED_LIMIT:
            break
    return entries


def _item_xml(entry):
    url = escape_xml(entry.get("url") or BLOG_URL)
    title = escape_xml(entry.get("title"))
    summary = escape_xml(entry.get("summary"))
    category = entry.get("category") or ""
    image = str(entry.get("image") or "")
    parts = [
        "    <item>",
        "      <title>%s</title>" % title,
        "      <link>%s</link>" % url,
        '      <guid isPermaLink="true">%s</guid>' % url,
    ]
    if category:
        parts.append("      <category>%s</category>" % escape_xml(category))
    stamp = int(entry.get("publishedMs") or 0)
    if stamp > 0:
        parts.append(
            "      <pubDate>%s</pubDate>"
            % formatdate(stamp / 1000.0, usegmt=True))
    parts.append("      <description>%s</description>" % summary)
    if image:
        safe = escape_xml(image)
        mime = image_mime_type(image)
        try:
            image_bytes = max(0, int(entry.get("imageBytes") or 0))
        except (TypeError, ValueError):
            image_bytes = 0
        parts.append(
            '      <enclosure url="%s" length="%d" type="%s"/>'
            % (safe, image_bytes, mime))
        parts.append(
            '      <media:content url="%s" medium="image" type="%s"/>'
            % (safe, mime))
        parts.append('      <media:thumbnail url="%s"/>' % safe)
    # The rendered body readers show: artwork first, then the same preview
    # text, so an item reads like the card it came from. "]]>" cannot appear
    # in our own generated copy, but a stray one would end the section early.
    body = []
    if image:
        body.append(
            '<p><img src="%s" alt="%s"></p>'
            % (escape_xml(image), escape_xml(entry.get("title"))))
    if entry.get("summary"):
        body.append("<p>%s</p>" % escape_xml(entry.get("summary")))
    body.append('<p><a href="%s">Read the post</a></p>' % url)
    parts.append(
        "      <content:encoded><![CDATA[%s]]></content:encoded>"
        % "".join(body).replace("]]>", "]]&gt;"))
    parts.append("    </item>")
    return "\n".join(parts)


def render_rss(entries, built_ms=0):
    """Render the RSS 2.0 document for parsed index entries.

    Each dated entry ships its own pubDate, the channel's pubDate is the
    newest of them (readers and the world's blog board take that as "last
    posted"), and lastBuildDate stamps the read.
    """
    try:
        stamp = float(built_ms) / 1000.0
    except (TypeError, ValueError):
        stamp = 0.0
    built = formatdate(stamp if stamp > 0 else None, usegmt=True)
    dated = [entry for entry in (entries or []) if isinstance(entry, dict)]
    items = [_item_xml(entry) for entry in dated]
    newest = max([int(entry.get("publishedMs") or 0) for entry in dated] or [0])
    channel_published = (
        "    <pubDate>%s</pubDate>" % formatdate(newest / 1000.0, usegmt=True)
        if newest > 0 else "")
    return "\n".join([
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom"'
        ' xmlns:content="http://purl.org/rss/1.0/modules/content/"'
        ' xmlns:media="http://search.yahoo.com/mrss/">',
        "  <channel>",
        "    <title>%s</title>" % escape_xml(FEED_TITLE),
        "    <link>%s</link>" % escape_xml(BLOG_URL),
        "    <description>%s</description>" % escape_xml(FEED_DESCRIPTION),
        "    <language>%s</language>" % FEED_LANGUAGE,
        channel_published,
        "    <lastBuildDate>%s</lastBuildDate>" % built,
        "    <generator>ForkMesh</generator>",
        '    <atom:link href="%s" rel="self" type="application/rss+xml"/>'
        % escape_xml(FEED_URL),
        "    <image>",
        "      <url>%s</url>" % escape_xml(FEED_IMAGE),
        "      <title>%s</title>" % escape_xml(FEED_TITLE),
        "      <link>%s</link>" % escape_xml(BLOG_URL),
        "    </image>",
        "\n".join(items) if items else "",
        "  </channel>",
        "</rss>",
        "",
    ])


def build_feed(html, built_ms=0):
    """Index HTML in, RSS document out. Empty when nothing parsed."""
    entries = parse_blog_index(html)
    if not entries:
        return ""
    return render_rss(entries, built_ms)


def _tag_text(item, tag):
    match = re.search(
        r"<%s(?:\s[^>]*)?>(.*?)</%s>" % (tag, tag), item, re.DOTALL)
    if not match:
        return ""
    return _text(match.group(1), MAX_SUMMARY)


def _rfc822_ms(value):
    """Epoch milliseconds for a feed pubDate, or 0 when it is unreadable."""
    raw = str(value or "").strip()
    if not raw:
        return 0
    try:
        return int(parsedate_to_datetime(raw).timestamp() * 1000)
    except (TypeError, ValueError, OverflowError):
        return 0


def parse_rss(xml):
    """Read our own feed document back into the entry shape.

    The world banner consumes the published feed rather than re-scraping the
    index, so this is the reverse of :func:`render_rss` for the fields the
    board draws.
    """
    entries = []
    for match in _ITEM_RE.finditer(str(xml or "")):
        item = match.group("item")
        title = _tag_text(item, "title")
        if not title:
            continue
        url = _tag_text(item, "link")
        enclosure = _ENCLOSURE_RE.search(item)
        entries.append({
            "slug": _text(url.strip("/").split("/")[-1], 80),
            "title": title[:MAX_TITLE],
            "summary": _tag_text(item, "description"),
            "category": _tag_text(item, "category")[:MAX_CATEGORY],
            "image": _unescape(enclosure.group("url")) if enclosure else "",
            "url": url,
            "publishedMs": _rfc822_ms(_tag_text(item, "pubDate")),
        })
        if len(entries) >= FEED_LIMIT:
            break
    return entries
