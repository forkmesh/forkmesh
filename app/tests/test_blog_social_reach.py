"""Blog share drafts, aggregate reach, and shared World entry contracts."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT.parent / "www" / "public"
WORLD_PUBLIC = ROOT.parent / "world" / "public" / "world"
ENTRY = (ROOT / "src/entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src/app_runtime.py").read_text(encoding="utf-8")
SCHEMA = (ROOT / "src/schema.py").read_text(encoding="utf-8")
SOCIAL = (PUBLIC / "blog-social.js").read_text(encoding="utf-8")
HEADER = (PUBLIC / "site-header.js").read_text(encoding="utf-8")
WORLD = (WORLD_PUBLIC / "world.js").read_text(encoding="utf-8")
WORLD_CSS = (WORLD_PUBLIC / "world.css").read_text(encoding="utf-8")
WORLD_INDEX = (WORLD_PUBLIC / "index.html").read_text(encoding="utf-8")


def test_every_blog_social_mount_builds_bounded_prefilled_drafts():
    assert "const MASTODON_LIMIT = 500" in SOCIAL
    assert "const TWITTER_LIMIT = 250" in SOCIAL
    # The image description is its own copyable field, not inlined into the
    # Mastodon body where it spent the character budget and still left the
    # attached image undescribed.
    assert "Image alt:" not in SOCIAL
    assert '"Image alt text",' in SOCIAL
    assert "image description field" in SOCIAL
    # Mastodon browses by hashtag, so its draft carries the wider tag set.
    tags = SOCIAL.split("const MASTODON_TAGS =", 1)[1].split(";", 1)[0]
    assert tags.count("#") >= 8
    for tag in ("#ForkMesh", "#OpenSource", "#DevTools", "#Git", "#FOSS"):
        assert tag in tags
    assert 'const TWITTER_TAGS = "#ForkMesh #OpenSource"' in SOCIAL
    assert "mastodon.social/share?" in SOCIAL
    assert "x.com/intent/post?" in SOCIAL
    assert "reddit.com/r/forkmesh/submit?" in SOCIAL
    assert "Open ${draft.label} with this post prefilled" in SOCIAL
    posts = list((PUBLIC / "blog").glob("*/index.html"))
    assert posts
    for post in posts:
        html = post.read_text(encoding="utf-8")
        assert "data-blog-social" in html, post
        assert "/blog-social.js" in html, post


def test_reddit_drafts_keep_the_blog_posts_readable():
    # Reddit text posts support Markdown. The generated draft should retain
    # article headings, paragraphs, lists, quotes, and inline emphasis rather
    # than reducing the complete post to one long line of text.
    for marker in (
        "function markdownInline(node)",
        "function markdownList(list, depth = 0)",
        "function markdownBlocks(node)",
        "function redditMarkdown(article, fallback)",
        'return inline ? [`${"#".repeat(level)} ${inline}`] : [];',
        'return inline ? [`> **Note:** ${inline}`] : [];',
        'return `**${content}**`;',
        'return `*${content}*`;',
        'return `~~${content}~~`;',
        'return href ? `[${content}](${href})` : content;',
        "[Read the full post on ForkMesh](${context.url})",
    ):
        assert marker in SOCIAL
    assert "context.article || context.description" not in SOCIAL


def test_blog_reach_is_aggregate_only_and_referrers_are_host_only():
    migration = (
        ROOT / "migrations/0094_blog_post_metrics.sql"
    ).read_text(encoding="utf-8")
    for table in (
        "blog_post_metrics",
        "blog_post_unique_hll",
        "blog_post_referrers",
    ):
        assert table in migration
        assert table in SCHEMA
    for forbidden in ("ip_address", "user_agent", "session_id", "visitor_id"):
        assert forbidden not in migration
    assert "async def record_blog_visit" in ENTRY
    assert "world_visitor_metrics.hll_register" in ENTRY
    assert "precision=BLOG_POST_UNIQUE_PRECISION" in ENTRY
    assert "async def blog_post_metrics" in ENTRY
    assert '"uniqueViews"' in ENTRY
    assert '"referrers": board' in ENTRY
    assert "/api/blog/metrics/" in ENTRY
    assert "data-blog-social-metrics" not in SOCIAL
    assert "HTTP referrer leaderboard" in SOCIAL
    assert "if (board.length)" in SOCIAL


def test_a_reload_is_not_another_view():
    # The counter used to move on every page load, so refreshing a post
    # inflated its reach; a reader is folded once per window instead.
    migration = (
        ROOT / "migrations/0127_blog_post_view_seen.sql"
    ).read_text(encoding="utf-8")
    assert "blog_post_view_seen" in migration
    assert "blog_post_view_seen" in SCHEMA
    for forbidden in ("ip_address", "user_agent", "session_id", "visitor_id"):
        assert forbidden not in migration
    fold = ENTRY.split("async def _fold_blog_post_view", 1)[1].split(
        "\n\n\nasync def ", 1)[0]
    assert "SELECT counted_at,referrer_host FROM blog_post_view_seen" in fold
    assert "BLOG_POST_VIEW_WINDOW_MS" in fold
    # The window runs from the last read that counted, so a reader who keeps
    # reloading cannot push it ahead of themselves forever.
    assert 'seen.get("counted_at")' in fold
    assert "now if counted else 0" in fold
    assert "INSERT INTO blog_post_view_seen " in fold
    # Every counter the page shows sits behind the dedupe decision.
    counted = fold.split("if counted:", 1)[1]
    assert "INSERT INTO blog_post_metrics" in counted
    assert "INSERT INTO blog_post_unique_hll" in counted
    assert "INSERT INTO blog_post_metrics" not in fold.split("if counted:", 1)[0]
    # document.referrer survives a reload in browsers, so a source only counts
    # again when the same reader arrives from a different one.
    assert 'seen.get("referrer_host")' in fold
    assert "if count_referrer:" in fold
    # Rows exist only for the window; nothing blinded is kept past retention.
    assert "async def prune_blog_post_view_seen" in ENTRY
    assert "await prune_blog_post_view_seen(env)" in ENTRY
    assert "DELETE FROM blog_post_view_seen WHERE last_at < ?" in ENTRY
    assert "BLOG_POST_VIEW_SEEN_TTL_MS" in ENTRY


def test_a_network_sending_readers_fills_its_posted_on_slot():
    # A post is announced on Mastodon long before anyone pastes the permalink
    # back in. The referrer board already proves it, so the slot reports the
    # detected source instead of reading as unannounced.
    assert "def _blog_post_network_for_host" in ENTRY
    hosts = ENTRY.split("BLOG_POST_NETWORK_HOSTS = (", 1)[1].split(")\n", 1)[0]
    for host in ("mastodon.social", "reddit.com", "x.com"):
        assert host in hosts
    metrics = ENTRY.split("async def blog_post_metrics", 1)[1].split(
        "\n\n\ndef ", 1)[0]
    assert "_blog_post_network_for_host(row[\"host\"])" in metrics
    # A detected instance is evidence, not a permalink: it never merges into
    # the stored links an administrator edits and saves.
    assert "if not network or network in links or network in detected" in metrics
    assert '"detected": detected,' in metrics
    # The announce instance is usually a small referrer, so detection scans
    # further down the ranking than the ten rows the board renders.
    assert "BLOG_POST_REFERRER_SCAN_LIMIT = 50" in ENTRY
    assert "slug, BLOG_POST_REFERRER_SCAN_LIMIT)" in metrics
    assert "board = ranked[:BLOG_POST_REFERRER_LIMIT]" in metrics
    assert "for row in ranked:" in metrics
    assert "function detectedFor(state, key)" in SOCIAL
    assert "Seen on ${detected.host} — paste the permalink" in SOCIAL
    assert "blog-social-detected-note" in SOCIAL
    assert 'item.classList.add("is-detected")' in SOCIAL
    styles = (PUBLIC / "blog-social.css").read_text(encoding="utf-8")
    assert ".blog-social-item.is-detected" in styles
    assert ".blog-social-detected-note" in styles


def test_www_served_posts_report_their_own_view_so_reach_is_not_zero():
    # The blog is static on the www Worker, so the navigation record_blog_visit
    # folds in never reaches this Worker: without the beacon every post reads
    # 0 views forever.
    assert "async def blog_post_view_beacon" in ENTRY
    assert "async def _fold_blog_post_view" in ENTRY
    beacon = ENTRY.split("async def blog_post_view_beacon", 1)[1].split(
        "\n\n\ndef ", 1)[0]
    assert "_request_origin_allowed(env, request)" in beacon
    assert "_is_link_preview_agent(request)" in beacon
    assert "_fold_blog_post_view(" in beacon
    routing = APP_RUNTIME.split(
        'blog_metrics_prefix = "/api/blog/metrics/"', 1
    )[1]
    routing = routing.split("\n\n", 1)[0]
    assert 'if method == "POST":' in routing
    assert "blog_post_view_beacon(self.env, request, slug)" in routing
    assert '"allow": "GET, POST"' in routing
    assert '`/api/blog/metrics/${encodeURIComponent(slug)}`' in SOCIAL
    assert 'method: "POST"' in SOCIAL
    assert 'referrer: document.referrer || ""' in SOCIAL
    # Simple request: a page view must not cost a preflight round trip.
    assert '"content-type": "text/plain;charset=UTF-8"' in SOCIAL
    assert "await reportView(slug) || await readMetrics(slug)" in SOCIAL


def test_announcement_permalinks_are_public_to_read_and_admin_to_write():
    migration = (
        ROOT / "migrations/0126_blog_post_links.sql"
    ).read_text(encoding="utf-8")
    assert "blog_post_links" in migration
    assert "blog_post_links" in SCHEMA
    for forbidden in ("ip_address", "user_agent", "session_id", "visitor_id"):
        assert forbidden not in migration
    assert "async def blog_post_links_handler" in ENTRY
    handler = ENTRY.split("async def blog_post_links_handler", 1)[1].split(
        "\n\n\nasync def ", 1)[0]
    assert 'if method in ("GET", "HEAD")' in handler
    assert 'if method != "PUT"' in handler
    assert "_authed_account_name(env, request, data)" in handler
    assert "await _is_admin(env, actor)" in handler
    assert '"error": "not_admin"' in handler
    assert '"error": "invalid_permalink"' in handler
    # Only https permalinks are ever stored or served back.
    assert 'if parsed.scheme != "https" or not parsed.hostname' in ENTRY
    assert "/api/blog/links/" in APP_RUNTIME
    # The page renders the same three slots as editable fields for an admin,
    # and the Worker re-checks is_admin on every write.
    assert "function isAdminSession()" in SOCIAL
    assert 'input.type = "url"' in SOCIAL
    # A blank slot still says so in the field; a slot whose network is already
    # sending readers says that instead (see the detected-source test).
    assert ": EMPTY_LABEL;" in SOCIAL
    assert 'method: "PUT"' in SOCIAL
    assert '`/api/blog/links/${encodeURIComponent(slug)}`' in SOCIAL
    assert "Sign in as an administrator to edit these permalinks." in SOCIAL
    # The world's blog banner reads the same slots, so a permalink saved after
    # the deploy has to win there too rather than reading as unannounced.
    distribution = ENTRY.split(
        "async def _blog_post_distribution", 1)[1].split(
            "\n\n\nasync def ", 1)[0]
    assert "await blog_post_links(env, slug)" in distribution
    assert "apply_blog_distribution_overrides(" in distribution


def test_share_drafts_line_up_with_the_posted_on_slots():
    grid = SOCIAL.split('grid.className = "blog-social-drafts";', 1)[1]
    grid = grid.split("panel.append(summary, grid);", 1)[0]
    assert grid.index('network: "reddit"') < grid.index('network: "mastodon"')
    assert grid.index('network: "mastodon"') < grid.index('network: "twitter"')
    networks = SOCIAL.split("const NETWORKS = [", 1)[1].split("];", 1)[0]
    assert networks.index("reddit") < networks.index("mastodon")
    assert networks.index("mastodon") < networks.index("twitter")
    styles = (PUBLIC / "blog-social.css").read_text(encoding="utf-8")
    assert "--blog-social-columns:" in styles
    assert styles.count("grid-template-columns: var(--blog-social-columns)") == 2


def test_readers_get_a_share_card_and_the_long_drafts_fold_away():
    render = SOCIAL.split("  function render(host) {", 1)[1]
    assert render.index("renderShare(drafts)") < render.index("renderPosted(state)")
    assert 'document.createElement("details")' in render
    assert render.index("host.append(renderShare(drafts), posted);") < render.index(
        "host.append(panel, state.metrics);")
    assert '"Prefilled drafts"' in render
    share = SOCIAL.split("  function renderShare(drafts) {", 1)[1].split(
        "\n  }\n", 1)[0]
    assert '"Share this post"' in share
    assert '"Copy link", drafts.context.url, "blog-social-button is-primary"' in share
    assert "NETWORKS.forEach((network) => {" in share
    assert "link.href = shareHref(network.key, drafts);" in share
    assert 'link.rel = "noopener noreferrer";' in share
    assert "await navigator.clipboard.writeText(text);" in SOCIAL
    assert 'document.execCommand("copy")' in SOCIAL
    assert 'copyButton("Copy", draft.text, "blog-social-button is-small")' in SOCIAL
    assert "state.metrics.append(section);" in SOCIAL
    assert "state.host.prepend(section);" not in SOCIAL
    styles = (PUBLIC / "blog-social.css").read_text(encoding="utf-8")
    for selector in (
        ".blog-social-share {",
        ".blog-social-button {",
        ".blog-social-button.is-primary {",
        "html.light .blog-social {",
        ".blog-social-drafts-panel > summary {",
        ".blog-social-drafts-panel[open] > summary .blog-social-summary-chevron {",
    ):
        assert selector in styles
    assert "font: 12px/1.5 ui-monospace" not in styles


def test_shared_header_has_prominent_world_link():
    assert 'class="fm-header-app"' in HEADER
    # Public marketing pages point at the canonical App, but Docs/Blog served
    # through a node must return to that node's dashboard so its host-only
    # signed-in state remains available.
    assert 'const APP_HREF = MARKETING_HOST' in HEADER
    assert '? "https://app.forkmesh.com/"' in HEADER
    assert ': "/dashboard";' in HEADER
    assert 'href="${APP_HREF}"' in HEADER
    assert 'class="fm-header-world"' in HEADER
    assert 'href="https://world.forkmesh.com/"' in HEADER
    # The link carries a live dot, not a member tally: the count was a number
    # nobody navigated by, and it made the shortcut the widest thing in the bar.
    assert "fm-header-world-count" not in HEADER


def test_world_logo_refreshes_and_loader_sits_on_a_blank_page():
    assert "data-world-logo-refresh" in WORLD
    assert "location.reload()" in WORLD
    assert "data-world-loading-progress" in WORLD
    assert "setLoadingProgress(100" in WORLD
    assert "world-loader-dash" in WORLD_INDEX
    assert ".world-loading-curtain" not in WORLD_CSS
    loading_rule = WORLD_INDEX.split(".world-loading-curtain {", 1)[1].split(
        "}", 1
    )[0]
    assert "width: min(360px" in loading_rule
    backdrop = WORLD_INDEX.split(".world-boot {", 1)[1].split("}", 1)[0]
    assert "inset: 0" in backdrop
    assert "background: #f5f6f8" in backdrop
