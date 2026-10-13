"""Normalizers for the world's Twitter/X and Reddit banner feeds.

Neither network exposes a CORS-open browser API the way mastodon.social
does, so the Worker proxies one bounded, edge-cached read of each public
feed and the banners in /world repaint from the normalized result. The
functions here are pure so the parsing rules stay testable without the
Cloudflare/Pyodide runtime.
"""

import json
import re
from datetime import datetime
from email.utils import parsedate_to_datetime

import blog_feed


TWITTER_HANDLE = "forkmesh"
TWITTER_PROFILE_URL = "https://x.com/forkmesh"
# X's widget-syndication timeline is the only unauthenticated read left; it
# serves HTML whose __NEXT_DATA__ script carries the timeline JSON. It is
# best-effort: when it stops serving, the banner keeps its static sign.
TWITTER_SYNDICATION_URL = (
    "https://syndication.twitter.com/srv/timeline-profile/screen-name/"
    + TWITTER_HANDLE
)
X_API_USER_URL = (
    "https://api.x.com/2/users/by/username/" + TWITTER_HANDLE
)
X_API_POSTS_URL = "https://api.x.com/2/users/{user_id}/tweets"
REDDIT_SUBREDDIT = "forkmesh"
REDDIT_PROFILE_URL = "https://www.reddit.com/r/forkmesh/"
REDDIT_LISTING_URL = (
    "https://www.reddit.com/r/forkmesh/new.json?limit=10&raw_json=1"
)
# Reddit rejects generic user agents; the documented convention is
# platform:app-id:version (by /u/owner).
REDDIT_USER_AGENT = "web:forkmesh-world-banner:v1 (by /u/forkmesh)"
# The blog board rides the blog's own RSS feed: the Worker builds that
# document from our static index (no external fetch, see blog_feed) and this
# module reduces the published items to the banner's post shape, so the board
# shows exactly what a subscriber sees — preview text and artwork included.
BLOG_URL = blog_feed.BLOG_URL
BLOG_FEED_URL = blog_feed.FEED_URL
BLOG_INDEX_ASSET = blog_feed.BLOG_INDEX_ASSET

SOCIAL_POSTS_LIMIT = 6
MAX_POST_TEXT = 400
MAX_POST_AUTHOR = 40

_NEXT_DATA_RE = re.compile(
    r'<script[^>]*id="__NEXT_DATA__"[^>]*>(.*?)</script>',
    re.DOTALL,
)


def _clean_text(value, limit=MAX_POST_TEXT):
    flat = re.sub(r"\s+", " ", str(value or "")).strip()
    return flat[:limit]


def _epoch_ms(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return 0
    if number <= 0:
        return 0
    # Reddit reports seconds; anything already in milliseconds passes through.
    return int(number if number > 10**12 else number * 1000)


def normalize_reddit_listing(payload):
    """Bound a public /r/<sub>/new.json listing to the banner's post shape."""
    children = []
    if isinstance(payload, dict):
        data = payload.get("data")
        if isinstance(data, dict) and isinstance(data.get("children"), list):
            children = data["children"]
    posts = []
    for child in children:
        record = child.get("data") if isinstance(child, dict) else None
        if not isinstance(record, dict):
            continue
        title = _clean_text(record.get("title"))
        if not title:
            continue
        permalink = str(record.get("permalink") or "")
        posts.append({
            "id": _clean_text(record.get("id"), 40),
            "text": title,
            "author": _clean_text(record.get("author"), MAX_POST_AUTHOR),
            "score": int(record.get("score") or 0),
            "comments": int(record.get("num_comments") or 0),
            "createdAt": _epoch_ms(record.get("created_utc")),
            "url": (
                "https://www.reddit.com" + permalink
                if permalink.startswith("/")
                else REDDIT_PROFILE_URL
            ),
        })
        if len(posts) >= SOCIAL_POSTS_LIMIT:
            break
    return posts


def extract_next_data(html):
    """Pull the parsed __NEXT_DATA__ JSON object out of a syndication page."""
    match = _NEXT_DATA_RE.search(str(html or ""))
    if not match:
        return None
    try:
        data = json.loads(match.group(1))
    except (ValueError, TypeError):
        return None
    return data if isinstance(data, dict) else None


def _twitter_created_ms(value):
    # created_at arrives either as epoch milliseconds or as Twitter's classic
    # "Mon Apr 01 12:00:00 +0000 2024" string depending on payload vintage.
    ms = _epoch_ms(value)
    if ms:
        return ms
    try:
        return int(parsedate_to_datetime(str(value or "")).timestamp() * 1000)
    except (ValueError, TypeError):
        try:
            return int(datetime.fromisoformat(
                str(value or "").replace("Z", "+00:00")).timestamp() * 1000)
        except (ValueError, TypeError):
            return 0


def normalize_twitter_timeline(next_data):
    """Bound the syndication timeline entries to the banner's post shape."""
    entries = []
    if isinstance(next_data, dict):
        try:
            candidate = (
                next_data["props"]["pageProps"]["timeline"]["entries"])
        except (KeyError, TypeError):
            candidate = None
        if isinstance(candidate, list):
            entries = candidate
    posts = []
    for entry in entries:
        content = entry.get("content") if isinstance(entry, dict) else None
        tweet = content.get("tweet") if isinstance(content, dict) else None
        if not isinstance(tweet, dict):
            continue
        # A retweet carries the original underneath; show what the account
        # amplified, credited to its author.
        retweeted = tweet.get("retweeted_status")
        record = retweeted if isinstance(retweeted, dict) else tweet
        text = _clean_text(record.get("full_text") or record.get("text"))
        if not text:
            continue
        tweet_id = _clean_text(record.get("id_str") or record.get("id"), 40)
        user = record.get("user") if isinstance(record.get("user"), dict) else {}
        screen_name = _clean_text(
            user.get("screen_name") or TWITTER_HANDLE, MAX_POST_AUTHOR)
        permalink = str(tweet.get("permalink") or "")
        posts.append({
            "id": tweet_id,
            "text": text,
            "author": "@" + screen_name,
            "likes": int(record.get("favorite_count") or 0),
            "retweets": int(record.get("retweet_count") or 0),
            "createdAt": _twitter_created_ms(record.get("created_at")),
            "url": (
                "https://x.com" + permalink
                if permalink.startswith("/")
                else "https://x.com/%s/status/%s" % (screen_name, tweet_id)
                if tweet_id
                else TWITTER_PROFILE_URL
            ),
        })
        if len(posts) >= SOCIAL_POSTS_LIMIT:
            break
    return posts


def normalize_x_api_timeline(payload, username=TWITTER_HANDLE):
    """Normalize the official X API v2 user-post timeline response."""
    records = payload.get("data") if isinstance(payload, dict) else None
    posts = []
    for record in records if isinstance(records, list) else []:
        if not isinstance(record, dict):
            continue
        text = _clean_text(record.get("text"))
        tweet_id = _clean_text(record.get("id"), 40)
        if not text or not tweet_id:
            continue
        metrics = (
            record.get("public_metrics")
            if isinstance(record.get("public_metrics"), dict)
            else {}
        )
        posts.append({
            "id": tweet_id,
            "text": text,
            "author": "@" + _clean_text(username, MAX_POST_AUTHOR),
            "likes": int(metrics.get("like_count") or 0),
            "retweets": int(metrics.get("retweet_count") or 0),
            "createdAt": _twitter_created_ms(record.get("created_at")),
            "url": "https://x.com/%s/status/%s" % (username, tweet_id),
        })
        if len(posts) >= SOCIAL_POSTS_LIMIT:
            break
    return posts


def normalize_blog_feed(xml):
    """Bound the blog's published RSS items to the banner's post shape.

    createdAt is the item's pubDate, so the board's plate reads how long ago
    the newest post went up (0 for an undated item, which the plate treats as
    unknown). `detail` is the item's description — the preview text the board
    prints under the headline — and `image` is the item enclosure the board
    paints as the card's artwork.
    """
    posts = []
    for entry in blog_feed.parse_rss(xml):
        title = _clean_text(entry.get("title"), 120)
        if not title:
            continue
        posts.append({
            "id": _clean_text(entry.get("slug"), 80),
            "text": title,
            "detail": _clean_text(entry.get("summary")),
            "meta": _clean_text(entry.get("category"), 80),
            "image": _clean_text(entry.get("image"), 300),
            "createdAt": _epoch_ms(entry.get("publishedMs")),
            "url": _clean_text(entry.get("url"), 300) or BLOG_URL,
        })
        if len(posts) >= SOCIAL_POSTS_LIMIT:
            break
    return posts


def normalize_blog_distribution(html):
    """Read one post's explicit social permalink slots.

    A blank slot means "not posted yet"; a missing/unreadable section means
    unknown. Only http(s) values are returned, matching blog-social.js.
    """
    match = re.search(
        r"<section\b[^>]*\bdata-blog-social\b(?P<attrs>[^>]*)>",
        str(html or ""),
        re.IGNORECASE,
    )
    if not match:
        return {"known": False, "networks": []}
    attrs = match.group("attrs")
    networks = []
    for key, label in (
        ("mastodon", "Mastodon"),
        ("twitter", "X"),
        ("reddit", "Reddit"),
    ):
        value = re.search(
            r"\bdata-%s\s*=\s*([\"'])(.*?)\1" % key,
            attrs,
            re.IGNORECASE | re.DOTALL,
        )
        raw = _clean_text(value.group(2), 300) if value else ""
        url = raw if re.match(r"^https?://", raw, re.IGNORECASE) else ""
        networks.append({
            "id": key,
            "label": label,
            "posted": bool(url),
            "url": url,
        })
    return {"known": True, "networks": networks}


# Same three slots the post page shows, in that page's order, so a post whose
# section could not be read still reports every network once an administrator
# has filled one in.
BLOG_DISTRIBUTION_NETWORKS = (
    ("mastodon", "Mastodon"),
    ("twitter", "X"),
    ("reddit", "Reddit"),
)


def apply_blog_distribution_overrides(distribution, stored):
    """Overlay saved permalinks onto the slots read from a post's markup.

    A permalink an administrator saved wins over the checked-in attribute, and
    only https(s) values are ever surfaced. A saved link also makes the slots
    known: the post is demonstrably announced even when its section could not
    be read.
    """
    base = distribution if isinstance(distribution, dict) else {}
    networks = [
        dict(entry) for entry in base.get("networks") or []
        if isinstance(entry, dict)
    ]
    overrides = {}
    for key, _label in BLOG_DISTRIBUTION_NETWORKS:
        raw = _clean_text((stored or {}).get(key, ""), 300)
        if re.match(r"^https?://", raw, re.IGNORECASE):
            overrides[key] = raw
    if not overrides:
        return {
            "known": bool(base.get("known")),
            "networks": networks,
        }
    if not networks:
        networks = [
            {"id": key, "label": label, "posted": False, "url": ""}
            for key, label in BLOG_DISTRIBUTION_NETWORKS
        ]
    for entry in networks:
        url = overrides.get(str(entry.get("id") or ""))
        if url:
            entry["url"] = url
            entry["posted"] = True
    return {"known": True, "networks": networks}


def social_posts_payload(now, twitter_posts, reddit_posts, blog_posts,
                         twitter_ok, reddit_ok, blog_ok,
                         twitter_reason="", twitter_source="public-syndication"):
    """One public payload for all three banners; states let a banner keep
    its static sign when its feed is unreachable while the others stay
    live."""
    return {
        "ok": True,
        "now": int(now),
        "twitter": {
            "handle": "@" + TWITTER_HANDLE,
            "url": TWITTER_PROFILE_URL,
            "state": "ready" if twitter_ok else "unavailable",
            "posts": twitter_posts if twitter_ok else [],
            "source": twitter_source,
            "reason": _clean_text(twitter_reason, 180),
            "humanTodo": (
                "" if twitter_ok else
                "Add X_API_BEARER_TOKEN to the Worker deployment, or publish "
                "at least one public embeddable post from @forkmesh."
            ),
        },
        "reddit": {
            "handle": "r/" + REDDIT_SUBREDDIT,
            "url": REDDIT_PROFILE_URL,
            "state": "ready" if reddit_ok else "unavailable",
            "posts": reddit_posts if reddit_ok else [],
        },
        "blog": {
            "handle": "forkmesh.com/blog",
            "url": BLOG_URL,
            "feedUrl": BLOG_FEED_URL,
            "state": "ready" if blog_ok else "unavailable",
            "posts": blog_posts if blog_ok else [],
        },
    }
