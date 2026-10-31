#!/usr/bin/env python3
"""Contracts for the Twitter, Reddit, and blog banners beside the Mastodon
kiosk."""

import json
from pathlib import Path
import sys
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SRC = ROOT / "src"
SCENE_PATH = ROOT.parent / "world" / "public" / "world" / "world-scene.js"
GENERATED_PACKAGES_PATH = (
    ROOT.parent / "world" / "public" / "world" / "world-marketplace-packages.js"
)
WORLD_PATH = ROOT.parent / "world" / "public" / "world" / "world.js"
ENTRY_PATH = SRC / "entry.py"
MARKETPLACE = ROOT.parent / "marketplace"
CATALOG_PATH = MARKETPLACE / "catalog.json"
X_PACKAGE = MARKETPLACE / "packages" / "x-banner-board"
REDDIT_PACKAGE = MARKETPLACE / "packages" / "reddit-banner-board"
X_MODULE_PATH = X_PACKAGE / "content" / "world" / "index.js"
REDDIT_MODULE_PATH = REDDIT_PACKAGE / "content" / "world" / "index.js"
# The World route bodies moved into sibling modules so Cloudflare's Python
# startup validation never parses them; the wiring below still spans the
# whole route family, so both sources are read together.
WORLD_ROUTES_PATH = ROOT.parent / "world" / "src" / "world_routes.py"
APP_RUNTIME_PATH = SRC / "app_runtime.py"

if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import blog_feed  # noqa: E402
import world_social_feeds as feeds  # noqa: E402


def _source(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def test_x_and_reddit_packages_ship_off_but_install_on_the_social_row():
    scene = world_scene_source()
    generated = _source(GENERATED_PACKAGES_PATH)
    x_module = _source(X_MODULE_PATH)
    reddit_module = _source(REDDIT_MODULE_PATH)
    # Boards ship switched off: they stay listed in the World Marketplace,
    # but their code waits behind a lazy import until a visitor installs one.
    for package_id in ("x-banner-board", "reddit-banner-board"):
        entry = generated[generated.index(f'id: "{package_id}",'):]
        entry = entry[: entry.index("\n  },")]
        assert "installedByDefault: false," in entry
        assert "module: null," in entry
        assert (
            f'load: () => import("../../../marketplace/packages/{package_id}'
            '/content/world/index.js"),'
        ) in entry
        assert f"import * as package_{package_id.replace('-', '_')}" not in generated
    assert "BUNDLED_WORLD_PACKAGES.forEach" in scene
    assert "initialRemovedWorldPackages = []" in scene
    assert "initialInstalledWorldPackages = []" in scene
    assert "removedWorldPackages.has" in scene
    assert "optedInWorldPackages.has" in scene
    assert "void installWorldPackage(packageId);" in scene
    # Both portable modules retain the existing placement and ordered slots.
    assert "position: Object.freeze([37.2, 0, -9.2])" in x_module
    assert "position: Object.freeze([27.6, 0, -26.6])" in reddit_module
    assert 'circle: "social"' in x_module
    assert 'circle: "social"' in reddit_module


def test_banner_packages_use_the_common_creator_manifest():
    catalog = json.loads(_source(CATALOG_PATH))
    entries = {entry["id"]: entry for entry in catalog["packages"]}
    assert entries["x-banner-board"]["manifest"] == (
        "packages/x-banner-board/manifest.json"
    )
    assert entries["reddit-banner-board"]["manifest"] == (
        "packages/reddit-banner-board/manifest.json"
    )
    for package, package_id in (
        (X_PACKAGE, "x-banner-board"),
        (REDDIT_PACKAGE, "reddit-banner-board"),
    ):
        manifest = json.loads(_source(package / "manifest.json"))
        assert manifest["id"] == package_id
        assert manifest["type"] == "virtual.object"
        assert manifest["entrypoints"] == {
            "world": "content/world/index.js",
            "settings": "builtin:package",
        }
        assert manifest["creator"]["verified"] is True
        assert manifest["license"]["creatorRetainsOwnership"] is True
        assert manifest["install"] == {"bundled": True, "default": False}
        assert manifest["lifecycle"] == {"migrations": []}
        assert {item["id"] for item in manifest["permissions"]} == {
            "world.socialfeed.read",
            "external.link.open",
        }
        assert (package / "LICENSE").is_file()
        assert (package / "preview" / "README.md").is_file()
        assert (package / "preview" / "market.png").is_file()
        assert len(manifest["screenshots"]) == 1
        assert manifest["screenshots"][0]["path"] == "preview/market.png"
        assert manifest["screenshots"][0]["alt"]


def test_banners_carry_the_official_handles():
    x_module = _source(X_MODULE_PATH)
    reddit_module = _source(REDDIT_MODULE_PATH)
    assert '"@forkmesh"' in x_module
    assert '"r/forkmesh"' in reddit_module
    assert '"https://x.com/forkmesh"' in x_module
    assert '"https://www.reddit.com/r/forkmesh/"' in reddit_module


def test_banner_click_opens_the_profile_in_a_new_tab():
    scene = world_scene_source()
    # The whole board is one click target that rides the same guarded
    # open-link callback the Mastodon kiosk buttons use (noopener new tab).
    assert 'child.userData.interactive = "social-banner-open";' in scene
    assert 'userData?.interactive === "social-banner-open"' in scene
    assert scene.count("onMastodonOpenLink(href)") >= 2


def test_banner_package_removal_cleans_every_world_contribution():
    scene = world_scene_source()
    lifecycle = scene.split("function removeWorldPackage(id)", 1)[1]
    lifecycle = lifecycle.split("function installWorldPackage(id)", 1)[0]
    for contract in (
        "installedWorldPackages.delete(packageId);",
        "records.splice(index, 1);",
        "removeBillboardFromIsland(record.billboard);",
        "removeInteractiveObject(interactive, record.group);",
        "worldElements.delete(packageId);",
        "record.instance.dispose?.();",
    ):
        assert contract in lifecycle, contract
    assert "if (!shared.length) feedRecords.delete(record.feed);" in scene
    assert "spinning.delete(record.spinner);" in scene
    assert "record.object.removeFromParent();\n    relayout();" in scene
    assert "installedWorldPackageIds: () =>" in scene
    assert "worldPackageCatalog: () =>" in scene
    for module_path in (X_MODULE_PATH, REDDIT_MODULE_PATH):
        module = _source(module_path)
        assert "host.disposeObject3D?.(group);" in module
        assert "fetch(" not in module


def test_banner_package_removal_is_persisted_and_reversible_from_marketplace():
    world = _source(WORLD_PATH)
    assert 'REMOVED_WORLD_PACKAGES_KEY = "forkmesh.world.removedPackages.v1"' in world
    assert "storedRemovedWorldPackages()" in world
    assert "initialRemovedWorldPackages: this.removedWorldPackages" in world
    assert 'data-world-package-remove="${escapeHTML(entry.id)}"' in world
    assert 'data-world-package-install="${escapeHTML(entry.id)}"' in world
    assert "this.world.removeWorldPackage?.(packageId)" in world
    assert "this.world.installWorldPackage?.(packageId)" in world
    assert "writeJSON(\n      localStorage,\n      REMOVED_WORLD_PACKAGES_KEY" in world


def test_banner_faces_repaint_from_the_proxy_snapshot():
    scene = world_scene_source()
    assert "function socialBannerTexture(" in scene
    assert "function updateSocialBanners(payload)" in scene
    # Exposed on the scene API so world.js can push snapshots.
    assert "updateSocialBanners,\n" in scene
    # Repaints dispose the previous texture like the Mastodon kiosk does.
    assert scene.count("face.material.map?.dispose?.();") >= 2
    # A feed that is not ready keeps the static sign, never a blank board.
    assert 'feed.state === "ready"' in scene


def test_system_status_board_is_not_built_into_the_world():
    scene = world_scene_source()
    world = _source(WORLD_PATH)
    for removed in (
        'title: "SYSTEM STATUS"',
        "systemStatusBannerTexture",
        "statusBannerRecord",
        "updateSystemStatusBoard",
        'feed: "status"',
    ):
        assert removed not in scene
    for removed in (
        'this.fetchJSON("/api/status/world"',
        "refreshSystemStatusBoard",
        "syncSystemStatusBoardTimer",
        "statusBoardRequestedAt",
        'message.type === "status-update"',
        "data-world-admin-status-light",
    ):
        assert removed not in world


def test_world_notification_tick_uses_a_digest_before_the_full_list():
    world = _source(WORLD_PATH)
    assert "this.notificationToken" in world
    assert "`/api/poll?node=${encodeURIComponent(account)}`" in world
    assert "nextToken === this.notificationToken" in world
    assert "refreshPersonalNotifications(false, { digestOnly: true })" in world


def test_world_fetches_the_proxy_without_credentials_on_a_timer():
    world = _source(WORLD_PATH)
    assert 'const SOCIAL_POSTS_URL = "/api/world/social-posts";' in world
    assert "const SOCIAL_REFRESH_MS = 10 * 60 * 1000;" in world
    assert "startSocialBannersRefresh()" in world
    assert "this.startSocialBannersRefresh();" in world
    assert "window.clearInterval(this.socialFeedsTimer);" in world
    # The proxy read is public and never carries ForkMesh session material.
    idx = world.index("  loadSocialBanners() {")
    assert 'credentials: "omit"' in world[idx:idx + 1200]
    assert "this.world?.updateSocialBanners?.(" in world


def test_worker_routes_the_social_posts_proxy_behind_the_edge_cache():
    entry = (
        _source(ENTRY_PATH)
        + _source(WORLD_ROUTES_PATH)
        + _source(APP_RUNTIME_PATH)
    )
    assert '"/api/world/social-posts", "/api/world/social-posts/"' in entry
    assert "world_social_posts_handler" in entry
    assert (
        'WORLD_SOCIAL_POSTS_CACHE_KEY = (\n'
        '    "https://forkmesh.internal/api/world/social-posts")' in entry
    )
    # Ten-minute TTL matching the Mastodon kiosk cadence keeps the two
    # upstream fetches to one per colo per window (free-plan quota).
    assert "WORLD_SOCIAL_POSTS_TTL = 600" in entry
    assert "edge_cache_match(WORLD_SOCIAL_POSTS_CACHE_KEY)" in entry
    assert "edge_cache_put(WORLD_SOCIAL_POSTS_CACHE_KEY, resp)" in entry
    assert "world_social_feeds.REDDIT_USER_AGENT" in entry


def test_reddit_listing_normalization_bounds_and_links():
    listing = {"data": {"children": [
        {"data": {
            "id": "abc",
            "title": "  A   spaced\ntitle  ",
            "author": "someone",
            "score": "7",
            "num_comments": 3,
            "created_utc": 1752000000,
            "permalink": "/r/forkmesh/comments/abc/a_spaced_title/",
        }},
        {"data": {"title": ""}},
        {"broken": True},
    ]}}
    posts = feeds.normalize_reddit_listing(listing)
    assert len(posts) == 1
    post = posts[0]
    assert post["text"] == "A spaced title"
    assert post["score"] == 7
    assert post["comments"] == 3
    assert post["createdAt"] == 1752000000000
    assert post["url"].startswith("https://www.reddit.com/r/forkmesh/")
    # Malformed payloads normalize to an empty list, never raise.
    assert feeds.normalize_reddit_listing(None) == []
    assert feeds.normalize_reddit_listing({"data": {"children": "x"}}) == []


def test_reddit_listing_is_bounded_to_the_banner_post_limit():
    children = [
        {"data": {"id": str(i), "title": "post %d" % i,
                  "permalink": "/r/forkmesh/%d/" % i}}
        for i in range(20)
    ]
    posts = feeds.normalize_reddit_listing({"data": {"children": children}})
    assert len(posts) == feeds.SOCIAL_POSTS_LIMIT


def test_twitter_timeline_extraction_and_normalization():
    html = (
        '<html><script id="__NEXT_DATA__" type="application/json">'
        '{"props":{"pageProps":{"timeline":{"entries":['
        '{"content":{"tweet":{"id_str":"9","full_text":"hi  there",'
        '"favorite_count":3,"retweet_count":1,'
        '"created_at":1752000000000,"permalink":"/forkmesh/status/9",'
        '"user":{"screen_name":"forkmesh"}}}},'
        '{"content":{"tweet":{"id_str":"10","full_text":""}}}'
        ']}}}}</script></html>'
    )
    next_data = feeds.extract_next_data(html)
    assert isinstance(next_data, dict)
    posts = feeds.normalize_twitter_timeline(next_data)
    assert len(posts) == 1
    post = posts[0]
    assert post["text"] == "hi there"
    assert post["author"] == "@forkmesh"
    assert post["likes"] == 3 and post["retweets"] == 1
    assert post["url"] == "https://x.com/forkmesh/status/9"
    # Classic string dates still resolve to epoch milliseconds.
    assert feeds._twitter_created_ms("Mon, 01 Apr 2024 12:00:00 +0000") > 0
    # Missing or mangled markup degrades to None/[] instead of raising.
    assert feeds.extract_next_data("<html>no data</html>") is None
    assert feeds.extract_next_data(
        '<script id="__NEXT_DATA__">not json</script>') is None
    assert feeds.normalize_twitter_timeline(None) == []
    assert feeds.normalize_twitter_timeline({"props": {}}) == []


def test_official_x_api_timeline_normalization():
    posts = feeds.normalize_x_api_timeline({"data": [{
        "id": "123",
        "text": "Fresh public update",
        "created_at": "2026-07-27T16:30:00.000Z",
        "public_metrics": {"like_count": 4, "retweet_count": 2},
    }]})
    assert posts == [{
        "id": "123",
        "text": "Fresh public update",
        "author": "@forkmesh",
        "likes": 4,
        "retweets": 2,
        "createdAt": 1785169800000,
        "url": "https://x.com/forkmesh/status/123",
    }]
    assert feeds.normalize_x_api_timeline({"data": "bad"}) == []


def test_payload_states_gate_each_feed_independently():
    payload = feeds.social_posts_payload(
        123, [{"id": "t"}], [{"id": "r"}], [{"id": "b"}], False, True, True,
        twitter_reason="No public posts")
    assert payload["ok"] is True and payload["now"] == 123
    assert payload["twitter"]["state"] == "unavailable"
    assert payload["twitter"]["posts"] == []
    assert payload["reddit"]["state"] == "ready"
    assert payload["reddit"]["posts"] == [{"id": "r"}]
    assert payload["blog"]["state"] == "ready"
    assert payload["blog"]["posts"] == [{"id": "b"}]
    assert payload["twitter"]["url"] == "https://x.com/forkmesh"
    assert payload["twitter"]["reason"] == "No public posts"
    assert "X_API_BEARER_TOKEN" in payload["twitter"]["humanTodo"]
    assert payload["reddit"]["url"] == "https://www.reddit.com/r/forkmesh/"
    assert payload["blog"]["url"] == "https://forkmesh.com/blog"
    gated = feeds.social_posts_payload(1, [], [], [{"id": "b"}], True, True,
                                       False)
    assert gated["blog"]["state"] == "unavailable"
    assert gated["blog"]["posts"] == []


def test_twitter_fallback_and_unavailable_reason_are_wired_end_to_end():
    entry = _source(ENTRY_PATH) + _source(WORLD_ROUTES_PATH)
    world = _source(WORLD_PATH)
    scene = world_scene_source()
    assert "world_social_feeds.X_API_USER_URL" in entry
    assert "world_social_feeds.X_API_POSTS_URL" in entry
    assert 'getattr(env, "X_API_BEARER_TOKEN", "")' in entry
    assert "normalize_x_api_timeline" in entry
    assert "reason: String(feed?.reason" in world
    assert "humanTodo: String(feed?.humanTodo" in world
    assert "PUBLIC FEED UNAVAILABLE" in scene


_BLOG_CARD_HTML = (
    '<a class="blog1-card" href="/blog/desktop-node-mirrors/"'
    ' data-published="2026-07-04" data-section="mesh">'
    '<img class="blog1-post-image" src="/x.webp" alt="">'
    '<div class="blog1-card-copy">'
    '<p class="blog1-card-meta">Distributed hosting &amp; more · Feature 01'
    '</p>\n<h3>Desktop-node mirrors</h3>\n'
    '<p>Actively   mirrored\nrepositories live on independent nodes.</p>'
    '</div></a>'
)


def test_blog_feed_normalization_carries_preview_text_and_artwork():
    posts = feeds.normalize_blog_feed(
        blog_feed.build_feed(_BLOG_CARD_HTML, 1_700_000_000_000))
    assert len(posts) == 1
    post = posts[0]
    assert post["id"] == "desktop-node-mirrors"
    assert post["text"] == "Desktop-node mirrors"
    assert post["meta"] == "Distributed hosting & more · Feature 01"
    # The board prints the feed item's description under the headline, and
    # paints its enclosure image as the card's artwork.
    assert post["detail"] == (
        "Actively mirrored repositories live on independent nodes.")
    assert post["image"] == "https://forkmesh.com/x.webp"
    # The item's pubDate rides through as createdAt: the board's plate
    # reads how long ago the newest post went up.
    assert post["createdAt"] == 1783123200000
    undated = feeds.normalize_blog_feed(blog_feed.build_feed(
        _BLOG_CARD_HTML.replace(' data-published="2026-07-04"', "")))
    assert undated[0]["createdAt"] == 0
    assert post["url"] == "https://forkmesh.com/blog/desktop-node-mirrors/"
    # Malformed documents degrade to an empty list, never raise.
    assert feeds.normalize_blog_feed(None) == []
    assert feeds.normalize_blog_feed("<rss><channel/></rss>") == []


def test_blog_distribution_preserves_explicit_posted_and_empty_slots():
    distribution = feeds.normalize_blog_distribution(
        """
        <section data-blog-social
          data-reddit=""
          data-mastodon="https://mastodon.social/@forkmesh/123"
          data-twitter="javascript:alert(1)"></section>
        """
    )
    assert distribution["known"] is True
    states = {item["id"]: item for item in distribution["networks"]}
    assert states["mastodon"]["posted"] is True
    assert states["mastodon"]["url"].startswith("https://mastodon.social/")
    assert states["reddit"] == {
        "id": "reddit", "label": "Reddit", "posted": False, "url": ""}
    assert states["twitter"]["posted"] is False
    assert feeds.normalize_blog_distribution("<p>no slots</p>") == {
        "known": False, "networks": []}


def test_saved_permalinks_win_over_the_checked_in_attributes():
    # Announcing a post saves the permalink instead of redeploying the page,
    # so the banner has to read the saved value the same way the post does.
    markup = feeds.normalize_blog_distribution(
        """
        <section data-blog-social
          data-reddit=""
          data-mastodon="https://mastodon.social/@forkmesh/123"
          data-twitter=""></section>
        """
    )
    merged = feeds.apply_blog_distribution_overrides(
        markup,
        {
            "reddit": "https://reddit.com/r/forkmesh/comments/abc/",
            "mastodon": "https://mastodon.social/@forkmesh/999",
            "twitter": "javascript:alert(1)",
        },
    )
    states = {item["id"]: item for item in merged["networks"]}
    assert merged["known"] is True
    assert states["reddit"]["posted"] is True
    assert states["reddit"]["url"].startswith("https://reddit.com/")
    # The saved permalink replaces the attribute rather than sitting beside it.
    assert states["mastodon"]["url"] == "https://mastodon.social/@forkmesh/999"
    # Anything that is not an http(s) permalink is still not a link.
    assert states["twitter"] == {
        "id": "twitter", "label": "X", "posted": False, "url": ""}

    # A saved permalink makes the slots known even when the section could not
    # be read, and no saved links leaves the markup's own verdict untouched.
    recovered = feeds.apply_blog_distribution_overrides(
        {"known": False, "networks": []},
        {"reddit": "https://reddit.com/r/forkmesh/comments/abc/"},
    )
    assert recovered["known"] is True
    assert {item["id"] for item in recovered["networks"]} == {
        "mastodon", "twitter", "reddit"}
    assert feeds.apply_blog_distribution_overrides(
        {"known": False, "networks": []}, {}) == {
            "known": False, "networks": []}
    assert feeds.apply_blog_distribution_overrides(markup, {}) == markup


def test_blog_feed_is_bounded_to_the_banner_post_limit():
    document = blog_feed.build_feed(_BLOG_CARD_HTML * 20)
    posts = feeds.normalize_blog_feed(document)
    assert len(posts) == feeds.SOCIAL_POSTS_LIMIT


def test_blog_feed_parses_the_shipped_blog_page():
    blog_html = _source(ROOT.parent / "www" / "public" / "blog.html")
    posts = feeds.normalize_blog_feed(blog_feed.build_feed(blog_html))
    assert len(posts) == feeds.SOCIAL_POSTS_LIMIT
    assert all(post["text"] and post["meta"] for post in posts)
    assert all(post["url"].startswith("https://forkmesh.com/blog/")
               for post in posts)
    # Every shipped card has artwork, so every board card gets an image.
    assert all(post["image"].startswith("https://forkmesh.com/assets/")
               for post in posts)


def test_scene_places_the_blog_banner_on_the_social_row():
    scene = world_scene_source()
    assert "BLOG_BANNER_OPTIONS" in scene
    assert "position: [19.8, 0, -32.8]" in scene
    assert "world.add(blogBanner);" in scene
    assert '"https://forkmesh.com/blog"' in scene
    assert "visiblePosts: 4" in scene
    assert "tall: true" in scene
    assert "const HEIGHT = options.tall ? 2560 : SOCIAL_BANNER_HEIGHT;" in scene


def test_banners_carry_sync_and_staleness_plates():
    scene = world_scene_source()
    package_modules = _source(X_MODULE_PATH) + _source(REDDIT_MODULE_PATH)
    # Each banner stand carries the kiosk's two plates: the MM:SS countdown
    # to the next feed sync and the color-coded staleness readout.
    assert "banner-countdown" in scene
    assert "banner-lastpost" in scene
    assert "function updateSocialBannerTimers(payload)" in scene
    assert "updateSocialBannerTimers,\n" in scene
    # Every board, blog included, reads its own newest post's age.
    assert 'label: "SYNCED"' not in scene
    assert (
        scene.count('label: "LAST POST"')
        + package_modules.count('label: "LAST POST"')
        == 3
    )
    # The blog runs the same color rule on a blog's slower cadence.
    assert "const BLOG_POST_FRESH_MS = 7 * 24 * 60 * 60 * 1000;" in scene
    assert "const BLOG_POST_STALE_MS = 21 * 24 * 60 * 60 * 1000;" in scene
    assert "freshMs: BLOG_POST_FRESH_MS," in scene


def test_world_drives_the_banner_clocks_on_a_one_second_tick():
    world = _source(WORLD_PATH)
    assert "syncSocialBannerTimers()" in world
    assert "this.world?.updateSocialBannerTimers?.(" in world
    assert "socialNewestPostAgo(feed)" in world
    assert "blog: timers(this.socialNewestPostAgo(snapshot?.blog))," in world
    # The tick, not a ten-minute interval, drives the reload so the countdown
    # and the fetch can never drift apart.
    idx = world.index("startSocialBannersRefresh() {")
    assert "1000" in world[idx:idx + 300]


def test_worker_folds_the_blog_feed_into_the_social_snapshot():
    entry = _source(ENTRY_PATH) + _source(WORLD_ROUTES_PATH)
    assert "blog_feed.BLOG_INDEX_ASSET" in entry
    # The board reads the same RSS document the public feed serves.
    assert "world_social_feeds.normalize_blog_feed(blog_rss)" in entry
    # The blog read stays inside the Worker's own static assets.
    idx = entry.index("blog_feed.BLOG_INDEX_ASSET")
    assert "env.ASSETS.fetch" in entry[idx - 400:idx]
    assert "_blog_post_reach_summary" in entry
    assert '"uniqueViews": world_visitor_metrics.hll_estimate' in entry
    assert '"referrerSites": int(referrers.get("sites") or 0)' in entry
    assert '"referrerVisits": int(referrers.get("visits") or 0)' in entry


def test_blog_board_draws_feed_artwork_and_preview_text():
    scene = world_scene_source()
    world = _source(WORLD_PATH)
    # The blog board is the one that carries per-post art.
    assert "postArt: true" in scene
    assert scene.count("postArt: true") == 1
    assert "function drawSocialBannerPostCard(" in scene
    # Artwork is cover-cropped into the card tile and only drawn once the
    # image decodes CORS-clean; otherwise the placeholder plate stays.
    assert 'image.crossOrigin = "anonymous";' in scene
    assert "media?.naturalWidth > 0 && media?.naturalHeight > 0" in scene
    # world.js forwards the item's preview text and image to the board, with
    # off-site image URLs dropped so the canvas is never tainted.
    assert "socialPostImage(value)" in world
    assert 'url.pathname.startsWith("/assets/")' in world
    assert "detail: String(post?.detail || \"\")" in world
    assert "age: this.socialPostAge(post?.createdAt)" in world
    assert "distribution: this.socialPostDistribution(post?.distribution)" in world
    assert "reach: this.socialPostReach(post?.reach)" in world
    assert "socialPostReach(value)" in world
    assert 'return `${days}D AGO`;' in world
    assert '`${label} ${states.get(id) ? "POSTED" : "NOT YET"}`' in world
    assert 'post.distribution || "SOCIAL STATUS UNKNOWN"' in scene
    assert 'post.reach || "VIEWS 0 · ~0 UNIQUE' in scene
