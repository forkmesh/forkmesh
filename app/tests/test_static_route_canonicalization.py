#!/usr/bin/env python3

import re
import sys
import tomllib
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
REPOSITORY = APP.parent
WWW_PUBLIC = REPOSITORY / "www" / "public"
APP_PUBLIC = APP / "public"
WORLD_PUBLIC = REPOSITORY / "world" / "public"
REDIRECTS = WWW_PUBLIC / "_redirects"
APP_WRANGLER = tomllib.loads((APP / "wrangler.toml").read_text(encoding="utf-8"))
SRC = APP / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import static_routes  # noqa: E402


WWW_PAGE_ROUTES = {
    "/favicon.ico": "/favicon/favicon.ico",
    "/apple-touch-icon.png": "/favicon/apple-touch-icon.png",
    "/apple-touch-icon-precomposed.png": "/favicon/apple-touch-icon.png",
}


def _redirect_rules():
    rules = []
    for raw_line in REDIRECTS.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        source, target, status = line.split()[:3]
        rules.append((source, target, status))
    return rules


def test_www_redirects_only_rewrite_www_owned_pages():
    rules = _redirect_rules()
    assert len({source for source, _, _ in rules}) == len(rules)
    assert all(source != "/" for source, _, _ in rules)
    assert (WWW_PUBLIC / "index.html").is_file()
    rewrites = {
        source: target
        for source, target, status in rules
        if status == "200" and ":slug" not in source
    }
    assert rewrites == WWW_PAGE_ROUTES
    for target in rewrites.values():
        assert (WWW_PUBLIC / target.lstrip("/")).is_file()

    for app_route in (
        "/dashboard",
        "/login",
        "/signup",
        "/chat",
        "/network",
        "/status",
        "/world",
    ):
        assert app_route not in rewrites

    for page in (
        "blog.html",
        "security-report.html",
        "desktop.html",
        "about.html",
        "features.html",
        "market.html",
        "press.html",
        "careers.html",
        "changelog.html",
        "privacy.html",
        "terms.html",
        "docs/index.html",
    ):
        assert (WWW_PUBLIC / page).is_file()


def test_blog_and_docs_directory_routes_are_canonical():
    rules = set(_redirect_rules())
    assert ("/blog/:slug", "/blog/:slug/", "308") in rules
    assert ("/docs/:slug", "/docs/:slug/", "308") not in rules
    assert ("/docs", "/docs/", "308") in rules
    feed_paths = {"/rss.xml", "/feed.xml", "/blog/rss.xml", "/blog/feed.xml"}
    assert not any(source in feed_paths for source, _, _ in rules)


def test_source_html_is_physically_partitioned():
    assert (WWW_PUBLIC / "blog.html").is_file()
    assert (WWW_PUBLIC / "docs/index.html").is_file()
    assert not (APP_PUBLIC / "blog").exists()
    assert not (APP_PUBLIC / "docs").exists()

    assert (APP_PUBLIC / "dashboard/index.html").is_file()
    assert (APP_PUBLIC / "login.html").is_file()
    assert not (WWW_PUBLIC / "dashboard/index.html").exists()
    assert not (WWW_PUBLIC / "login.html").exists()

    assert (WORLD_PUBLIC / "world/index.html").is_file()
    assert not (WWW_PUBLIC / "world").exists()
    assert not (APP_PUBLIC / "world").exists()


def test_internal_www_links_do_not_publish_html_implementation_paths():
    pattern = re.compile(r'\b(?:href|action)="(/[^"#?]+\.html)(?:[#?][^"]*)?"')
    violations = []
    for path in sorted(WWW_PUBLIC.rglob("*.html")):
        if "/dashboard/partials/" in path.as_posix():
            continue
        for match in pattern.finditer(path.read_text(encoding="utf-8")):
            violations.append(f"{path.relative_to(WWW_PUBLIC)} -> {match.group(1)}")
    assert violations == []


def test_app_dynamic_routes_run_before_static_assets():
    run_worker_first = APP_WRANGLER["assets"]["run_worker_first"]
    assert "/" in run_worker_first
    assert "/api" in run_worker_first
    assert "/api/*" in run_worker_first
    assert "/developers" in run_worker_first
    assert "/*/*" in run_worker_first
    assert "/@*" in run_worker_first
    assert "/%40*" in run_worker_first

    for exception in (
        "/assets/*",
        "/dashboard-plugins/*",
        "/favicon/*",
    ):
        assert "!" + exception in run_worker_first
    for worker_owned in (
        "/blog/*",
        "/docs/*",
        "/world/*",
        "/dashboard/*",
        "/notes/*",
    ):
        assert worker_owned in run_worker_first
    for asset_page in (
        "/dashboard",
        "/dashboard/repos",
        "/dashboard/network",
        "/dashboard/chat",
        "/dashboard/settings",
        "/dashboard/profile",
        "/dashboard/profile/repositories",
    ):
        assert "!" + asset_page in run_worker_first
        assert "!" + asset_page + "/" in run_worker_first
    # Marketplace-owned dashboard pages are resolved through the generated
    # plugin registry, so they must reach the Worker instead of bypassing it as
    # authored static assets.
    for plugin_page in (
        "/dashboard/tasks",
        "/dashboard/calendar",
        "/dashboard/notes",
    ):
        assert "!" + plugin_page not in run_worker_first


def test_app_page_shells_use_free_static_asset_delivery():
    run_worker_first = APP_WRANGLER["assets"]["run_worker_first"]
    assert APP_WRANGLER["assets"]["html_handling"] == "drop-trailing-slash"
    assert "/*.html" in run_worker_first
    for route in (
        "/chat",
        "/forgot-password",
        "/ledger",
        "/login",
        "/mirror-payouts",
        "/reset-password",
    ):
        assert route not in run_worker_first
        assert "!" + route + "/" in run_worker_first
        assert (APP_PUBLIC / (route[1:] + ".html")).is_file()
    # /signup is intentionally the one Worker-owned account page: loading it
    # mints the HttpOnly, one-time claim required by account creation.
    assert "/signup" in run_worker_first
    assert "/signup/" in run_worker_first
    assert "!/signup/" not in run_worker_first
    assert (APP_PUBLIC / "signup.html").is_file()
    # Composed pages (dashboard_shell.PAGES) serve an index.html under a
    # directory named for the clean URL rather than a hand-written <name>.html.
    for route, asset in (
        ("/dashboard", "dashboard/index.html"),
        ("/referral", "referral/index.html"),
    ):
        assert route not in run_worker_first
        assert "!" + route in run_worker_first
        assert (APP_PUBLIC / asset).is_file()


def test_public_data_pages_are_www_documents_on_published_origins():
    run_worker_first = APP_WRANGLER["assets"]["run_worker_first"]
    rules = {source for source, _, _ in _redirect_rules()}
    worker = (REPOSITORY / "www" / "worker.js").read_text(encoding="utf-8")
    app_paths = worker[worker.index("const APP_PATHS"):worker.index("]);")]
    published = (
        "https://forkmesh.com",
        "https://www.forkmesh.com",
        "https://app.forkmesh.com",
        "https://world.forkmesh.com",
    )
    for route in ("/leaderboards", "/network", "/status"):
        assert route in run_worker_first
        assert "!" + route + "/" not in run_worker_first
        assert (APP_PUBLIC / (route[1:] + ".html")).is_file()
        assert route not in rules
        assert '"%s"' % route not in app_paths
        assert static_routes.external_site_route(route) == "www"
        assert static_routes.external_site_route(route + "/") == "www"
        assert static_routes.external_site_delivery(
            route, "https://app.forkmesh.com", "https://forkmesh.com",
            published,
        ) == ("redirect", "https://forkmesh.com")
        assert static_routes.external_site_delivery(
            route, "https://pp.next.forkmesh.com", "https://forkmesh.com",
            published,
        ) == ("redirect", "https://forkmesh.com")
        assert static_routes.external_site_delivery(
            route, "http://127.0.0.1:8787", "", published,
        ) == ("local", "")
    assert static_routes.external_site_route("/status/extra") is None
    assert "/ledger" not in run_worker_first
    assert '"/ledger"' in app_paths


def test_repo_shortcut_detection_does_not_hijack_assets():
    assert static_routes.looks_like_repo_route("/kaif/forkmesh")
    assert static_routes.looks_like_repo_route("/kaif/forkmesh/issues")
    assert static_routes.looks_like_repo_route("/kaif/forkmesh/tree/src/main.py")
    for path in (
        "/assets/logo.png",
        "/favicon/site.webmanifest",
        "/docs/guide",
        "/blog/introducing-forkmesh/index.html",
        "/world/world.js",
        "/dashboard/partials/header.html",
    ):
        assert not static_routes.looks_like_repo_route(path)


def test_node_host_docs_and_blog_keep_their_session_origin():
    published = (
        "https://forkmesh.com",
        "https://www.forkmesh.com",
        "https://app.forkmesh.com",
        "https://world.forkmesh.com",
    )
    for path in (
        "/docs",
        "/docs/protocol/",
        "/blog",
        "/blog/parallel-agents/",
    ):
        assert static_routes.external_site_delivery(
            path,
            "https://pp.next.forkmesh.com",
            "https://forkmesh.com",
            published,
        ) == ("proxy", "https://forkmesh.com")

    # The official product hosts keep the pre-existing canonical split, and
    # unrelated marketing paths never turn an arbitrary node into a proxy.
    assert static_routes.external_site_delivery(
        "/docs", "https://app.forkmesh.com", "https://forkmesh.com",
        published,
    ) == ("redirect", "https://forkmesh.com")
    # /pricing is a retired page: it stays www-owned so the redirect lands on
    # the origin that owns the home page, whose asset router then sends it on
    # to /#pricing.
    assert static_routes.external_site_delivery(
        "/pricing", "https://pp.next.forkmesh.com", "https://forkmesh.com",
        published,
    ) == ("redirect", "https://forkmesh.com")
    assert static_routes.external_site_delivery(
        "/docs/%2e%2e/pricing", "https://pp.next.forkmesh.com",
        "https://forkmesh.com", published,
    ) == ("redirect", "https://forkmesh.com")


def test_site_delivery_normalizes_origins_without_accepting_paths_or_credentials():
    assert static_routes.normalized_site_origin(
        "HTTPS://PP.NEXT.FORKMESH.COM:443/"
    ) == "https://pp.next.forkmesh.com"
    assert static_routes.normalized_site_origin(
        "http://[::1]:8080"
    ) == "http://[::1]:8080"
    for value in (
        "https://pp.next.forkmesh.com/docs",
        "https://user@pp.next.forkmesh.com",
        "https://pp.next.forkmesh.com?query=1",
        "ftp://pp.next.forkmesh.com",
        "https://pp.next.forkmesh.com:bad",
    ):
        assert static_routes.normalized_site_origin(value) == ""
