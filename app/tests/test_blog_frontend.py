#!/usr/bin/env python3
"""Static contract tests for the public blog pages."""

from html.parser import HTMLParser
from pathlib import Path
import tomllib


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
PUBLIC = REPOSITORY / "www" / "public"
BLOG_PAGE = PUBLIC / "blog.html"
ENTRY_TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
WRANGLER = tomllib.loads(
    (REPOSITORY / "www" / "wrangler.toml").read_text(encoding="utf-8")
)
REDIRECTS = PUBLIC / "_redirects"
FEATURE_IMAGES = PUBLIC / "assets" / "blog" / "features"
FORKMESH_FOREVER_POST = PUBLIC / "blog" / "forkmesh-forever" / "index.html"
FORKMESH_FOREVER_VIDEO_ID = "EsQMZuS6lbE"
RETIRED_FOREVER_MEDIA = (
    PUBLIC / "assets" / "video" / "forkmesh-forever.mp4",
    PUBLIC / "assets" / "video" / "forkmesh-forever.en.vtt",
)
PREVIOUS_UPDATE_SLUG = "1-2-7-and-a-phone-that-listens"
PREVIOUS_UPDATE_POST = PUBLIC / "blog" / PREVIOUS_UPDATE_SLUG / "index.html"
PREVIOUS_UPDATE_IMAGE = FEATURE_IMAGES / f"{PREVIOUS_UPDATE_SLUG}.webp"
LATEST_UPDATE_SLUG = "runs-that-listen-branches-that-let-go"
LATEST_UPDATE_POST = PUBLIC / "blog" / LATEST_UPDATE_SLUG / "index.html"
LATEST_UPDATE_IMAGE = FEATURE_IMAGES / f"{LATEST_UPDATE_SLUG}.webp"
VS_CODE_PLUGIN_POST = PUBLIC / "blog" / "vs-code-extension" / "index.html"
VS_CODE_PLUGIN_IMAGE = FEATURE_IMAGES / "vs-code-extension.webp"


class BlogNavParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self._in_nav = False
        self._nav_depth = 0
        self.links = []

    def handle_starttag(self, tag, attrs):
        attr_map = dict(attrs)
        if tag == "nav" and attr_map.get("aria-label") == "Blog":
            self._in_nav = True
            self._nav_depth = 1
            return

        if self._in_nav:
            if tag not in {"br", "hr", "img", "input", "link", "meta"}:
                self._nav_depth += 1
            if tag == "a":
                self.links.append(attr_map)

    def handle_endtag(self, tag):
        if not self._in_nav:
            return

        if tag not in {"br", "hr", "img", "input", "link", "meta"}:
            self._nav_depth -= 1
        if self._nav_depth <= 0:
            self._in_nav = False


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _feature_post_paths():
    feature_slugs = {path.stem for path in FEATURE_IMAGES.glob("*.webp")}
    return sorted(
        path for path in (PUBLIC / "blog").glob("*/index.html")
        if path.parent.name in feature_slugs
    )


def test_blog_page_uses_editorial_feature_archive_shell():
    html = _read(BLOG_PAGE)

    for marker in (
        '<link rel="canonical" href="https://forkmesh.com/blog">',
        'class="blog1-shell"',
        'class="blog1-hero"',
        'Our latest <span class="blog1-mono">articles</span>',
        'class="blog1-featured-image blog1-has-art"',
        'class="blog1-next"',
    ):
        assert marker in html


def test_blog_page_mounts_the_universal_site_header():
    # The blog's page-specific topbar (brand + hardcoded "Sign Up / Log In")
    # was replaced by the shared session-aware header, which shows the
    # logged-in account chip instead of the auth links.
    html = _read(BLOG_PAGE)

    assert 'href="/site-header.css"' in html
    assert 'src="/site-header.js?v=${version}"' in html
    assert '<div data-forkmesh-header="simple"></div>' in html
    assert 'class="blog1-topbar"' not in html
    assert "Sign Up / Log In" not in html


def test_blog_page_footer_keeps_only_twitter_social_link():
    # The blog mounts the shared site footer like every other public page
    # instead of carrying its own bespoke markup, so the social-link contract
    # is asserted against the shared renderer.
    html = _read(BLOG_PAGE)
    footer_js = (Path(__file__).resolve().parents[2] / "www" / "public" / "site-footer.js").read_text(
        encoding="utf-8"
    )

    assert '<div data-forkmesh-footer="standard"></div>' in html
    assert 'src="/site-footer.js?v=${version}"' in html
    assert 'class="blog1-footer"' not in html

    assert '"https://x.com/forkmesh"' in footer_js
    assert ("https://twitter.com/" + "forkmesh") not in footer_js
    for removed in ("GitHub", "Discord", "LinkedIn", "YouTube"):
        assert 'aria-label="%s"' % removed not in footer_js


def test_blog_page_indexes_every_feature_post():
    html = _read(BLOG_PAGE)
    posts = _feature_post_paths()

    assert len(posts) == 96
    assert html.count('class="blog1-card" href="/blog/') == 95
    assert html.count('class="blog1-search-result" href="/blog/') == 95
    assert html.count('data-blog-search-text=') == 95
    assert html.count('href="/blog/forkmesh-coaster/"') == 2
    assert html.count('href="/blog/start-your-own-forkmesh/"') == 2
    assert html.count('href="/blog/runs-that-listen-branches-that-let-go/"') == 2
    assert (
        'data-blog-search-text="runs that listen branches that let go product update '
        'cloud runner follow-ups steer images live stream phone merge clean up '
        'create pr update from base merged pull requests refs pr metadata branches '
        'deleted upstream mesh notifications chat formatting slack markup marketing '
        'crm phones account email tracking analytics consent"'
        in html
    )
    assert html.count('href="/blog/the-work-keeps-its-place/"') == 2
    assert (
        'data-blog-search-text="the work keeps its place product update '
        'hosted repositories browser merge cores GitHub GitLab Codeberg runner '
        'sessions branch diff cleanup transcripts web file editing Android local '
        'repository sync iOS commit graph Office profiles avatars rooms focus '
        'campfire applause"'
        in html
    )
    assert html.count('href="/blog/1-2-7-and-a-phone-that-listens/"') == 2
    assert (
        'data-blog-search-text="1.2.7 and a phone that listens product update '
        'desktop release 1.2.6 windows installer qt6openglwidgets dll clean install'
        in html
    )
    assert html.count('href="/blog/office-identity/"') == 2
    assert (
        'data-blog-search-text="office identity product update '
        'developers stay signed in office reloads team switches desktop agents clean up cloud runs merge'
        in html
    )
    assert html.count('href="/blog/the-week-after-the-move/"') == 2
    assert (
        'data-blog-search-text="the week after the move product update '
        'cloudflare retired d1 deleted workers email dkim dmarc deploy on push'
        in html
    )
    assert html.count('href="/blog/open-collective/"') == 2
    assert (
        'data-blog-search-text="forkmesh is on open collective announcement '
        'opencollective funding money donations expenses transparency'
        in html
    )
    assert html.count('href="/blog/three-small-servers/"') == 2
    assert (
        'data-blog-search-text="three small servers and a long september '
        'product update forkmesh next cloudflare repository'
        in html
    )
    assert html.count('href="/blog/signals-that-lead-somewhere/"') == 2
    assert (
        'data-blog-search-text="signals that lead somewhere product update '
        'durable usage reset pings contextual teammate follow ups agent sessions '
        'responsive diagnostics cloud logs cached synthetic status probes '
        'marketing channels current main v0.7.29 candidate v0.7.95"'
        in html
    )
    assert html.count('href="/blog/proof-before-promises/"') == 2
    assert (
        'data-blog-search-text="proof before promises product update free protocol '
        'paid service business model human gated agent loops health aware routing '
        'custody release evidence current main v0.7.29 candidate v0.7.95 launch '
        'readiness"'
        in html
    )
    assert html.count('href="/blog/the-mirror-becomes-a-front-door/"') == 2
    assert (
        'data-blog-search-text="the mirror becomes a front door product update '
        'direct https independent mirror dashboard portable login mobile coding '
        'agent marketplace plugin lifecycle tasks dependencies spellcheck current '
        'main v0.7.29 candidate v0.7.95'
        in html
    )
    assert html.count('href="/blog/the-mesh-keeps-moving/"') == 2
    assert (
        'data-blog-search-text="the mesh keeps moving product update '
        'shared teammate runs agent sessions priorities task links portable '
        'identity recovery mobile node foreground sealed private chat end to '
        'end encryption status monitoring gaps mirror recovery current main '
        'v0.7.29'
        in html
    )
    assert html.count(
        'href="/blog/the-teammate-who-asks-what-should-leave/"'
    ) == 2
    assert (
        'data-blog-search-text="the teammate who asks what should leave '
        'product update reaper grim reaper feature retirement'
        in html
    )
    assert html.count('href="/blog/a-wider-door-a-clearer-trail/"') == 2
    assert (
        'data-blog-search-text="a wider door a clearer trail product update'
        in html
    )
    assert html.count('href="/blog/a-team-around-the-work/"') == 2
    assert (
        'data-blog-search-text="a team around the work product update'
        in html
    )
    assert 'href="/blog/shorter-loops-stronger-proof/"' in html
    assert (
        'data-blog-search-text="shorter loops stronger proof product update'
        in html
    )
    assert 'href="/blog/one-app-one-mesh/"' in html
    assert 'href="/blog/safer-rooms-steadier-mirrors/"' in html
    assert 'href="/blog/a-week-closer-to-the-mesh/"' in html
    assert 'href="/blog/forkmesh-forever/"' in html
    assert 'href="/blog/building-forkmesh-in-the-open/"' in html
    assert 'href="/blog/desktop-node-mirrors/"' in html
    assert 'href="/blog/status-blog-and-changelog/"' in html
    assert "Every ForkMesh feature now has a short technical post" in html


def test_vs_code_post_is_kept_as_an_optional_marketplace_plugin():
    index = _read(BLOG_PAGE)
    post = _read(VS_CODE_PLUGIN_POST)

    assert index.count('href="/blog/vs-code-extension/"') == 2
    assert "VS Code Marketplace plugin" in index
    assert "Marketplace · Plugin" in index
    assert "VS Code Marketplace plugin · ForkMesh Blog" in post
    assert "The plugin is optional and is not installed by the base desktop app" in post
    assert 'href="/market"' in post
    assert VS_CODE_PLUGIN_IMAGE.is_file()


def test_feature_blog_posts_have_images_and_article_shells():
    sample = PUBLIC / "blog" / "desktop-node-mirrors" / "index.html"
    html = _read(sample)

    for marker in (
        '<link rel="canonical" href="https://forkmesh.com/blog/desktop-node-mirrors/">',
        "Desktop-node mirrors",
        'src="/assets/blog/features/desktop-node-mirrors.webp"',
        "Why it matters",
        "How ForkMesh handles it",
        "Where it fits",
        'href="/blog">← All blog posts</a>',
    ):
        assert marker in html


def test_every_blog_post_carries_fillable_social_permalinks():
    posts = sorted((PUBLIC / "blog").glob("*/index.html"))

    assert len(posts) == 97
    for post in posts:
        html = _read(post)
        for marker in (
            '<link rel="stylesheet" href="/blog-social.css">',
            '<script src="/blog-social.js?v=${version}" defer></script>',
            "<section data-blog-social",
            'data-reddit=""',
            'data-mastodon=""',
            'data-twitter=""',
        ):
            assert marker in html, post


def test_blog_social_renderer_shows_unfilled_networks_as_empty():
    script = _read(PUBLIC / "blog-social.js")
    styles = _read(PUBLIC / "blog-social.css")

    for marker in (
        '{ key: "reddit", label: "Reddit" }',
        '{ key: "mastodon", label: "Mastodon" }',
        '{ key: "twitter", label: "X (Twitter)" }',
        'const EMPTY_LABEL = "Not posted yet";',
        # A blank attribute — and anything that is not an http(s) permalink —
        # renders as the empty slot instead of becoming an anchor.
        'return /^https?:\\/\\//i.test(raw) ? raw : "";',
        'empty.className = "blog-social-empty";',
        'link.rel = "noopener noreferrer me";',
    ):
        assert marker in script

    assert ".blog-social-item.is-empty" in styles
    assert ".blog-social-empty" in styles


def test_feature_blog_images_exist_for_each_generated_post():
    posts = _feature_post_paths()
    images = sorted(FEATURE_IMAGES.glob("*.webp"))

    assert len(images) == 96
    for post in posts:
        slug = post.parent.name
        assert FEATURE_IMAGES.joinpath(slug + ".webp").is_file()


def test_previous_product_update_keeps_production_metadata_art_and_navigation():
    post = _read(PREVIOUS_UPDATE_POST)
    index = _read(BLOG_PAGE)
    former_latest = _read(PUBLIC / "blog" / "office-identity" / "index.html")
    alt = (
        'alt="A rainy, moonlit clearing in a pine forest: a glowing green '
        'workstation screen on a stone plinth at left, a phone at right sending '
        'out cyan sound rings and chat bubbles that dissolve into the sky, four '
        'lit crates in the middle, and the official ForkMesh logo with its '
        'vertical reflection on the wet ground in front"'
    )

    for marker in (
        '<title>1.2.7, and a phone that listens · ForkMesh Blog</title>',
        '<link rel="canonical" href="https://forkmesh.com/blog/1-2-7-and-a-phone-that-listens/">',
        '<meta property="og:url" content="https://forkmesh.com/blog/1-2-7-and-a-phone-that-listens/">',
        '<meta property="og:image" content="https://forkmesh.com/assets/blog/features/1-2-7-and-a-phone-that-listens.webp">',
        '<meta name="twitter:image" content="https://forkmesh.com/assets/blog/features/1-2-7-and-a-phone-that-listens.webp">',
        '<meta name="twitter:card" content="summary_large_image">',
        '<meta property="article:published_time" content="2026-10-02T00:00:00-04:00">',
        '<meta property="og:image:width" content="1672">',
        '<meta property="og:image:height" content="941">',
        'src="/assets/blog/features/1-2-7-and-a-phone-that-listens.webp"',
        alt,
        '<span class="pill">October 2, 2026</span>',
        '<span>8 min read</span>',
        'Desktop 1.2.7',
        'Start a desktop agent from your phone',
        'Saying it out loud',
        "Direct messages that don't stay",
        'Pings that get through',
        'A busier CRM',
        'Smaller things',
        'Coming next',
        'the only part of it we published was the mirror node',
        'Windows will still say "Unknown publisher"',
        'ad-hoc signed and not notarized',
        "which isn't on the App Store yet",
        'stays approved until you remove it under Settings → Phone',
        'href="/docs/mobile/siri/"',
        'href="/#downloads"',
        '<section data-blog-social',
        'data-reddit=""',
        'data-mastodon=""',
        'data-twitter=""',
        '<a href="/blog">All posts</a>',
        'href="/blog/office-identity/">Office identity →',
    ):
        assert marker in post

    assert post.count(alt) == 1
    assert post.count(alt[5:-1]) == 3
    assert index.count(alt) == 1
    assert index.index(
        'href="/blog/1-2-7-and-a-phone-that-listens/"'
    ) < index.index(
        'href="/blog/office-identity/"'
    )
    assert 'data-published="2026-10-02" data-section="updates"' in index
    assert "Product update · October 2, 2026" in index
    assert (
        'href="/blog/1-2-7-and-a-phone-that-listens/">'
        '← 1.2.7, and a phone that listens'
        in former_latest
    )
    assert 'href="/blog">All posts</a>' in former_latest
    assert (
        'href="/blog/the-week-after-the-move/">'
        'The week after the move →'
        in former_latest
    )

    image = PREVIOUS_UPDATE_IMAGE.read_bytes()
    assert image[:4] == b"RIFF" and image[8:12] == b"WEBP"
    assert image[12:16] == b"VP8 "
    frame = image[20:30]
    assert frame[3:6] == b"\x9d\x01\x2a"
    assert int.from_bytes(frame[6:8], "little") & 0x3FFF == 1672
    assert int.from_bytes(frame[8:10], "little") & 0x3FFF == 941


def test_latest_product_update_has_metadata_art_cta_and_navigation():
    post = _read(LATEST_UPDATE_POST)
    index = _read(BLOG_PAGE)
    alt = (
        'alt="A rain-wet stone clearing at night where a glowing green runner '
        'tower sends a thread of cyan light to a phone resting on a rock, while '
        'faint branch paths fade into the forest behind them; the official '
        'ForkMesh logo and its vertical reflection stand in the foreground"'
    )

    for marker in (
        '<title>Runs that listen, branches that let go · ForkMesh Blog</title>',
        '<link rel="canonical" href="https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/">',
        '<meta property="og:url" content="https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/">',
        '<meta property="og:image" content="https://forkmesh.com/assets/blog/features/runs-that-listen-branches-that-let-go.webp">',
        '<meta name="twitter:image" content="https://forkmesh.com/assets/blog/features/runs-that-listen-branches-that-let-go.webp">',
        '<meta property="article:published_time" content="2026-10-07T19:00:00-04:00">',
        '<meta property="og:image:width" content="1672">',
        '<meta property="og:image:height" content="941">',
        'src="/assets/blog/features/runs-that-listen-branches-that-let-go.webp"',
        alt,
        '<span class="pill">October 7, 2026</span>',
        '<span>6 min read</span>',
        '<h2>Follow-ups reach the run</h2>',
        '<h2>Branches that let go</h2>',
        'Stop still does not reach a runner',
        'so the server and web changes below are live there',
        'The iPhone app is still in App Review',
        'Private repositories still queue the work for the desktop.',
        'href="https://app.forkmesh.com/signup?utm_source=blog&amp;utm_medium=referral&amp;utm_campaign=changelog&amp;utm_content=runs-that-listen-branches-that-let-go">Try ForkMesh in the browser</a>',
        '<section data-blog-social',
        'data-reddit=""',
        'data-mastodon=""',
        'data-twitter=""',
        '<a href="/blog">All posts</a>',
        'href="/blog/the-work-keeps-its-place/">The work keeps its place →</a>',
        'href="/blog/forkmesh-coaster/">← Ride the ForkMesh coaster</a>',
    ):
        assert marker in post

    assert post.count(alt) == 1
    assert post.count(alt[5:-1]) == 3
    assert index.count(alt) == 1
    assert 'data-published="2026-10-07" data-section="updates"' in index
    assert "Product update · October 7, 2026" in index
    assert index.index(
        'href="/blog/runs-that-listen-branches-that-let-go/"'
    ) < index.index('href="/blog/the-work-keeps-its-place/"')
    image = LATEST_UPDATE_IMAGE.read_bytes()
    assert image[:4] == b"RIFF" and image[8:12] == b"WEBP"
    assert image[12:16] == b"VP8 "
    frame = image[20:30]
    assert frame[3:6] == b"\x9d\x01\x2a"
    assert int.from_bytes(frame[6:8], "little") & 0x3FFF == 1672
    assert int.from_bytes(frame[8:10], "little") & 0x3FFF == 941


def test_forkmesh_coaster_post_has_metadata_art_and_navigation():
    post = _read(PUBLIC / "blog" / "forkmesh-coaster" / "index.html")
    index = _read(BLOG_PAGE)
    alt = (
        'alt="A rider\'s avatar in the front car of a ForkMesh World roller '
        'coaster climbing the lift hill into a golden sunset, its rails '
        'glowing cyan and light streaking past, with two vertical loops, '
        'drone rings and the rest of the track running off across sand and '
        'grass behind, and the ForkMesh logo in the corner"'
    )

    for marker in (
        '<title>Ride the ForkMesh coaster · ForkMesh Blog</title>',
        '<link rel="canonical" href="https://forkmesh.com/blog/forkmesh-coaster/">',
        '<meta property="og:image" content="https://forkmesh.com/assets/blog/features/forkmesh-coaster.webp">',
        '<meta name="twitter:image" content="https://forkmesh.com/assets/blog/features/forkmesh-coaster.webp">',
        '<meta property="article:published_time" content="2026-10-07T20:00:00-04:00">',
        '<meta property="og:image:width" content="1672">',
        '<meta property="og:image:height" content="941">',
        'src="/assets/blog/features/forkmesh-coaster.webp"',
        alt,
        '<span class="pill">October 7, 2026</span>',
        '<span>3 min read</span>',
        '<h2>How to get on</h2>',
        'walk to the station on the west edge of town',
        'href="https://world.forkmesh.com/"',
        '<section data-blog-social',
        'data-reddit=""',
        'data-mastodon=""',
        'data-twitter=""',
        '<a href="/blog">All posts</a>',
        'href="/blog/runs-that-listen-branches-that-let-go/">'
        'Runs that listen, branches that let go →</a>',
    ):
        assert marker in post

    assert post.count(alt) == 1
    assert index.count(alt) == 1
    assert "Announcement · October 7, 2026" in index
    assert index.index('href="/blog/forkmesh-coaster/"') < index.index(
        'href="/blog/runs-that-listen-branches-that-let-go/"'
    )
    image = (FEATURE_IMAGES / "forkmesh-coaster.webp").read_bytes()
    assert image[:4] == b"RIFF" and image[8:12] == b"WEBP"
    assert image[12:16] == b"VP8 "
    frame = image[20:30]
    assert frame[3:6] == b"\x9d\x01\x2a"
    assert int.from_bytes(frame[6:8], "little") & 0x3FFF == 1672
    assert int.from_bytes(frame[8:10], "little") & 0x3FFF == 941


def test_start_your_own_forkmesh_post_has_metadata_art_campaign_and_navigation():
    post = _read(PUBLIC / "blog" / "start-your-own-forkmesh" / "index.html")
    index = _read(BLOG_PAGE)
    coaster = _read(PUBLIC / "blog" / "forkmesh-coaster" / "index.html")
    alt = (
        'alt="The Start your own instance page: a line saying it sets up Git '
        'hosting, repositories, chat and agents under your own name on your own '
        'server in about fifteen minutes, then step 1, Name your project, with an '
        'Acme Code preview marked Powered by ForkMesh, and step 2, Your server, '
        'asking for an IP address, SSH user and port"'
    )
    tagged = (
        'href="https://app.forkmesh.com/start?utm_source=blog&amp;utm_medium=referral'
        '&amp;utm_campaign=start_instance&amp;utm_content=start-your-own-forkmesh"'
    )

    for marker in (
        '<title>Start your own ForkMesh · ForkMesh Blog</title>',
        '<link rel="canonical" href="https://forkmesh.com/blog/start-your-own-forkmesh/">',
        '<meta property="og:image" content="https://forkmesh.com/assets/blog/features/start-your-own-forkmesh.webp">',
        '<meta name="twitter:image" content="https://forkmesh.com/assets/blog/features/start-your-own-forkmesh.webp">',
        '<meta property="article:published_time" content="2026-10-07T21:00:00-04:00">',
        '<meta property="og:image:width" content="1672">',
        '<meta property="og:image:height" content="941">',
        'src="/assets/blog/features/start-your-own-forkmesh.webp"',
        alt,
        '<span class="pill">October 7, 2026</span>',
        '<span>4 min read</span>',
        '<h2>What you need</h2>',
        '<h2>Your keys stay yours</h2>',
        'href="https://forkmesh.com/install-instance.sh"',
        '<section data-blog-social',
        'data-reddit=""',
        'data-mastodon=""',
        'data-twitter=""',
        '<a href="/blog">All posts</a>',
        'href="/blog/forkmesh-coaster/">Ride the ForkMesh coaster →</a>',
    ):
        assert marker in post

    assert post.count(tagged) == 2
    assert post.count(alt) == 1
    assert index.count(alt) == 1
    assert index.index('href="/blog/start-your-own-forkmesh/"') < index.index(
        'href="/blog/forkmesh-coaster/"'
    )
    assert 'href="/blog/start-your-own-forkmesh/">← Start your own ForkMesh</a>' in coaster
    image = (FEATURE_IMAGES / "start-your-own-forkmesh.webp").read_bytes()
    assert image[:4] == b"RIFF" and image[8:12] == b"WEBP"
    assert image[12:16] == b"VP8 "
    frame = image[20:30]
    assert frame[3:6] == b"\x9d\x01\x2a"
    assert int.from_bytes(frame[6:8], "little") & 0x3FFF == 1672
    assert int.from_bytes(frame[8:10], "little") & 0x3FFF == 941


def test_the_work_keeps_its_place_keeps_its_metadata_art_and_navigation():
    post = _read(PUBLIC / "blog" / "the-work-keeps-its-place" / "index.html")
    index = _read(BLOG_PAGE)
    alt = (
        'alt="A moonlit wet-stone clearing connects a glowing repository prism '
        'to a browser workstation, a phone, and a glass office pavilion, with '
        'the official ForkMesh logo and its vertical reflection in the foreground"'
    )

    for marker in (
        '<title>The work keeps its place · ForkMesh Blog</title>',
        '<link rel="canonical" href="https://forkmesh.com/blog/the-work-keeps-its-place/">',
        '<meta property="og:image" content="https://forkmesh.com/assets/blog/features/the-work-keeps-its-place.webp">',
        '<meta name="twitter:image" content="https://forkmesh.com/assets/blog/features/the-work-keeps-its-place.webp">',
        '<meta property="article:published_time" content="2026-10-04T19:00:00-04:00">',
        '<meta property="og:image:width" content="1672">',
        '<meta property="og:image:height" content="941">',
        'src="/assets/blog/features/the-work-keeps-its-place.webp"',
        alt,
        '<span class="pill">October 4, 2026</span>',
        '<span>5 min read</span>',
        'Browser mirroring from GitHub, GitLab, or Codeberg works on the hosted ForkMesh service only.',
        'href="https://app.forkmesh.com/signup?utm_source=blog&amp;utm_medium=referral&amp;utm_campaign=changelog&amp;utm_content=the-work-keeps-its-place">Try ForkMesh in the browser</a>',
        '<section data-blog-social',
        'data-reddit=""',
        'data-mastodon=""',
        'data-twitter=""',
        '<a href="/blog">All posts</a>',
        'href="/blog/brief-reports-clear-signals/">Brief reports, clear signals →</a>',
        'href="/blog/runs-that-listen-branches-that-let-go/">'
        '← Runs that listen, branches that let go</a>',
    ):
        assert marker in post

    assert post.count(alt) == 1
    assert index.index('href="/blog/the-work-keeps-its-place/"') < index.index(
        'href="/blog/brief-reports-clear-signals/"'
    )
    image = (FEATURE_IMAGES / "the-work-keeps-its-place.webp").read_bytes()
    assert image[:4] == b"RIFF" and image[8:12] == b"WEBP"
    assert image[12:16] == b"VP8 "
    frame = image[20:30]
    assert frame[3:6] == b"\x9d\x01\x2a"
    assert int.from_bytes(frame[6:8], "little") & 0x3FFF == 1672
    assert int.from_bytes(frame[8:10], "little") & 0x3FFF == 941


def test_the_week_after_the_move_keeps_its_metadata_art_and_navigation():
    post = _read(PUBLIC / "blog" / "the-week-after-the-move" / "index.html")
    index = _read(BLOG_PAGE)
    former_latest = _read(
        PUBLIC / "blog" / "open-collective" / "index.html"
    )
    alt = (
        'alt="Three glowing green server monoliths stand on wet stone in a '
        'moonlit forest beside a lit glass office pavilion, a small robot at a '
        'workbench and an overgrown, unlit cloud-shaped frame; the official '
        'ForkMesh logo and its vertical reflection stand in the foreground"'
    )

    for marker in (
        '<title>The week after the move · ForkMesh Blog</title>',
        '<link rel="canonical" href="https://forkmesh.com/blog/the-week-after-the-move/">',
        '<meta property="og:url" content="https://forkmesh.com/blog/the-week-after-the-move/">',
        '<meta property="og:image" content="https://forkmesh.com/assets/blog/features/the-week-after-the-move.webp">',
        '<meta name="twitter:image" content="https://forkmesh.com/assets/blog/features/the-week-after-the-move.webp">',
        '<meta name="twitter:card" content="summary_large_image">',
        '<meta property="article:published_time" content="2026-09-30T00:00:00-04:00">',
        '<meta property="og:image:width" content="1672">',
        '<meta property="og:image:height" content="941">',
        'src="/assets/blog/features/the-week-after-the-move.webp"',
        alt,
        '<span class="pill">September 30, 2026</span>',
        '<span>14 min read</span>',
        'Cloudflare, switched off',
        'Cloud agents finish their own work',
        'Your phone, typing into your terminal',
        'An office you can walk around in',
        'The World got fast',
        'The phone apps: native only',
        'Desktop 1.2.5',
        'Updates that never delete your checkout',
        'Coming next',
        "It's merged, and it's off.",
        'this only works for public repositories',
        'a stolen, unlocked phone that\'s still paired',
        'ad-hoc signed and not notarized',
        'If you\'re installing on Windows for the first time, wait for 1.2.6.',
        'href="https://app.forkmesh.com/office"',
        '<section data-blog-social',
        'data-reddit=""',
        'data-mastodon=""',
        'data-twitter=""',
        '<a href="/blog">All posts</a>',
        'href="/blog/office-identity/">← Office identity',
        'href="/blog/open-collective/">ForkMesh is on Open Collective →',
    ):
        assert marker in post

    assert post.count(alt) == 1
    assert post.count(alt[5:-1]) == 3
    assert index.count(alt) == 1
    assert index.index(
        'href="/blog/office-identity/"'
    ) < index.index(
        'href="/blog/the-week-after-the-move/"'
    )
    assert index.index(
        'href="/blog/the-week-after-the-move/"'
    ) < index.index(
        'href="/blog/open-collective/"'
    )
    assert (
        'data-published="2026-09-30" data-section="updates"' in index
    )
    assert "Product update · September 30, 2026" in index
    assert (
        'href="/blog/the-week-after-the-move/">'
        '← The week after the move'
        in former_latest
    )
    assert 'href="/blog">All posts</a>' in former_latest
    assert (
        'href="/blog/three-small-servers/">'
        'Three small servers and a long September →'
        in former_latest
    )
    assert (
        'href="/blog/open-collective/">'
        '← ForkMesh is on Open Collective'
        in _read(PUBLIC / "blog" / "three-small-servers" / "index.html")
    )

    image = FEATURE_IMAGES.joinpath("the-week-after-the-move.webp").read_bytes()
    assert image[:4] == b"RIFF" and image[8:12] == b"WEBP"
    assert image[12:16] == b"VP8 "
    frame = image[20:30]
    assert frame[3:6] == b"\x9d\x01\x2a"
    assert int.from_bytes(frame[6:8], "little") & 0x3FFF == 1672
    assert int.from_bytes(frame[8:10], "little") & 0x3FFF == 941


def test_open_collective_announcement_keeps_its_metadata():
    post = _read(PUBLIC / "blog" / "open-collective" / "index.html")

    for marker in (
        '<link rel="canonical" href="https://forkmesh.com/blog/open-collective/">',
        '<meta property="article:published_time" content="2026-09-28T00:00:00-04:00">',
        'src="/assets/blog/features/open-collective.webp"',
        '<span class="pill">September 28, 2026</span>',
        '<span>2 min read</span>',
        'href="https://opencollective.com/forkmesh"',
    ):
        assert marker in post


def test_forkmesh_forever_post_embeds_the_youtube_video():
    html = _read(FORKMESH_FOREVER_POST)
    watch_url = f"https://www.youtube.com/watch?v={FORKMESH_FOREVER_VIDEO_ID}"
    embed_url = (
        "https://www.youtube-nocookie.com/embed/"
        f"{FORKMESH_FOREVER_VIDEO_ID}"
    )

    for marker in (
        '<link rel="canonical" href="https://forkmesh.com/blog/forkmesh-forever/">',
        '<iframe class="hero-video"',
        f'src="{embed_url}?rel=0&amp;modestbranding=1&amp;playsinline=1"',
        'allowfullscreen',
        'referrerpolicy="strict-origin-when-cross-origin"',
        f'<meta property="og:video" content="{watch_url}">',
        '<meta property="og:video:type" content="text/html">',
        '"@type": "VideoObject"',
        f'"contentUrl": "{watch_url}"',
        f'"embedUrl": "{embed_url}"',
        # The embed can be blocked, so the page still links out in plain HTML.
        f'<a href="{watch_url}"',
    ):
        assert marker in html

    # The self-hosted copies are retired; nothing may point back at them.
    assert "/assets/video/forkmesh-forever.mp4" not in html
    assert "/assets/video/forkmesh-forever.en.vtt" not in html
    assert "<video" not in html
    for retired in RETIRED_FOREVER_MEDIA:
        assert not retired.exists()


def test_blog_page_search_opens_dialog():
    html = _read(BLOG_PAGE)

    for marker in (
        'class="blog1-search-trigger"',
        'aria-haspopup="dialog"',
        'aria-controls="blog-search-modal"',
        'id="blog-search-modal"',
        'role="dialog"',
        'aria-modal="true"',
        'id="blog-search-input"',
        'data-blog-search-empty',
        "openBlogSearch",
        "filterBlogSearchResults",
    ):
        assert marker in html


def test_blog_page_uses_landing_green_accent_for_primary_art():
    html = _read(BLOG_PAGE)

    assert "--blog1-green:#3fb950;" in html
    assert "--blog1-cyan:#76d8f6;" in html
    assert "--blog1-blue:" not in html


def test_blog_interaction_states_use_neutral_dark_not_green():
    html = _read(BLOG_PAGE)

    for marker in (
        "--blog1-hover-line:rgba(246,246,242,.24);",
        "--blog1-hover-fill:rgba(255,255,255,.045);",
        "--blog1-hover-ring:rgba(255,255,255,.08);",
        "--blog1-popup-sheen:rgba(255,255,255,.11);",
    ):
        assert marker in html


def test_blog_search_result_copy_is_compact():
    html = _read(BLOG_PAGE)

    for marker in (
        ".blog1-search-result-meta",
        "font-size:11px;",
        ".blog1-search-result-title",
        "font-size:18px;",
        ".blog1-search-result-copy",
        "font-size:13px;",
    ):
        assert marker in html


def test_blog_paths_are_owned_by_redirects_not_python_worker():
    assert 'if url.path == "/blog":' not in ENTRY_TEXT
    assert 'if url.path == "/blog.html":' not in ENTRY_TEXT
    assert REDIRECTS.exists()
    redirects = _read(REDIRECTS)
    assert "/blog /blog.html 200" not in redirects
    assert "/blog/ /blog 308" in redirects
    assert "/blogs /blog 308" in redirects
    assert "/blog/:slug /blog/:slug/ 308" in redirects
    assert "/blog/:slug/ /blog/:slug/index.html 200" not in redirects
    assert "/blog.html /blog 308" not in redirects

    assert "name" not in WRANGLER
    assert WRANGLER["assets"]["html_handling"] == "auto-trailing-slash"
    assert "/blog" not in WRANGLER["assets"]["run_worker_first"]
    assert "/blog/*" not in WRANGLER["assets"]["run_worker_first"]
    assert "durable_objects" not in WRANGLER


if __name__ == "__main__":
    test_blog_page_uses_editorial_feature_archive_shell()
    test_blog_page_mounts_the_universal_site_header()
    test_blog_page_footer_keeps_only_twitter_social_link()
    test_blog_page_indexes_every_feature_post()
    test_feature_blog_posts_have_images_and_article_shells()
    test_every_blog_post_carries_fillable_social_permalinks()
    test_blog_social_renderer_shows_unfilled_networks_as_empty()
    test_feature_blog_images_exist_for_each_generated_post()
    test_latest_product_update_has_production_metadata_art_and_navigation()
    test_open_collective_announcement_keeps_its_metadata()
    test_forkmesh_forever_post_embeds_a_deployable_captioned_video()
    test_blog_page_search_opens_dialog()
    test_blog_page_uses_landing_green_accent_for_primary_art()
    test_blog_interaction_states_use_neutral_dark_not_green()
    test_blog_search_result_copy_is_compact()
    test_blog_paths_are_owned_by_redirects_not_python_worker()
