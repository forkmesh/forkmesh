#!/usr/bin/env python3
"""Per-page dashboard contract: one built document per page, real-link nav.

The dashboard is true separate pages (dashboard_shell.PAGES): each page URL
serves its own prebuilt document containing the shared chrome plus exactly one
view, the sidebar marks the active page at build time, and legacy
/dashboard?section= URLs 308 to the per-page paths in the Worker.
"""

import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
WWW_PUBLIC = ROOT.parent / "www" / "public"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402

from _dashboard_shell import assembled_dashboard_page  # noqa: E402
from _dashboard_bundle import assembled_dashboard_js  # noqa: E402

WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
REDIRECT_LINES = (WWW_PUBLIC / "_redirects").read_text(encoding="utf-8").splitlines()
RUNTIME_TEXT = (SRC / "app_runtime.py").read_text(encoding="utf-8")


def test_every_page_document_is_built_and_deterministic():
    for page_id, meta in dashboard_shell.PAGES.items():
        built = (PUBLIC / meta["asset"]).read_text(encoding="utf-8")
        assert built == assembled_dashboard_page(page_id), (
            "%s is stale — run tools/build_dashboard_assets.py" % meta["asset"])


def test_new_repository_modal_only_ships_on_the_repositories_page():
    for page_id in dashboard_shell.PAGES:
        html = assembled_dashboard_page(page_id)
        assert ("data-new-repo-modal" in html) is (page_id == "repos"), page_id


def test_settings_does_not_advertise_unbacked_pronouns_control():
    source = (PUBLIC / "dashboard" / "partials" / "views" / "settings.html").read_text(
        encoding="utf-8")
    built = (PUBLIC / "dashboard" / "settings" / "index.html").read_text(
        encoding="utf-8")

    for html in (source, built):
        assert ">Pronouns<" not in html
        assert "Prefer not to say" not in html


def test_client_bundle_sources_use_the_shared_version_placeholder():
    # Source pages show the shared variable while deploy staging resolves its
    # value without checking generated hashes into every document.
    for meta in dashboard_shell.PAGES.values():
        html = (PUBLIC / meta["asset"]).read_text(encoding="utf-8")
        assert 'src="/dashboard.js?v=${version}"' in html, meta["asset"]
        if meta["asset"] == dashboard_shell.PAGES["chat"]["asset"]:
            assert 'src="/dashboard-chat.js?v=${version}"' in html, meta["asset"]
        else:
            assert 'src="/dashboard-chat.js' not in html, meta["asset"]


def test_each_page_document_has_exactly_one_view_and_its_page_marker():
    for page_id, meta in dashboard_shell.PAGES.items():
        html = assembled_dashboard_page(page_id)
        assert html.count("<section data-view=") == 1, page_id
        # The data-view attribute keeps the legacy section name (settings ->
        # "profile", repo -> "explore") so dashboard.js selectors kept working.
        assert ('<section data-view="%s"' % meta["section"]) in html, page_id
        assert ('data-page="%s"' % page_id) in html, page_id
        assert ('data-dashboard-section="%s"' % meta["section"]) in html, page_id
        # The one view is active at parse time — no flash, no client toggle.
        assert 'class="view active' in html, page_id
        # Shared chrome ships on every page.
        for marker in ("data-app-header", "data-sidebar-main-menu",
                       "data-profile-sidebar-template"):
            assert marker in html, (page_id, marker)


def test_sidebar_marks_the_active_page_at_build_time():
    for page_id, meta in dashboard_shell.PAGES.items():
        html = assembled_dashboard_page(page_id)
        sidebar = html[html.index("data-sidebar-main-menu"):html.index("data-sidebar-top-repositories")]
        active = sidebar.count('aria-current="page"')
        if meta["nav"]:
            assert active == 1, page_id
            assert ('data-nav="%s" data-nav-link aria-current="page"' % meta["nav"]) in sidebar, page_id
        else:
            # Settings has no sidebar item of its own.
            assert active == 0, page_id


def test_sidebar_nav_is_real_links_not_section_buttons():
    html = assembled_dashboard_page("home")
    for path in ("/dashboard", "/dashboard/profile", "/dashboard/repos",
                 "/dashboard/network", "/dashboard/chat", "/ledger"):
        assert ('href="%s"' % path) in html, path
    assert 'data-nav="ledger" data-nav-link href="/ledger"' in html
    # The old client-side section router markup is gone everywhere.
    for page_id in dashboard_shell.PAGES:
        assert "data-section=" not in assembled_dashboard_page(page_id), page_id


def test_routing_config_covers_every_page():
    run_worker_first = WRANGLER["assets"]["run_worker_first"]
    assert "/dashboard" not in run_worker_first
    assert "!/dashboard" in run_worker_first
    assert "/dashboard/*" in run_worker_first
    for route, asset in static_routes.DASHBOARD_PAGE_ASSETS.items():
        assert (PUBLIC / asset).is_file(), asset
        assert ("!" + route) in run_worker_first, route
        # Product pages on the marketing origin hand off in the asset router;
        # the app origin still serves its prebuilt document directly.
        if route.startswith("/dashboard/"):
            expected = "/dashboard/* https://app.forkmesh.com/dashboard/:splat 308"
        else:
            expected = route + " https://app.forkmesh.com" + route + " 308"
        assert expected in REDIRECT_LINES, route
    assert WRANGLER["assets"]["html_handling"] == "drop-trailing-slash"
    # Direct .html hits 404 — clean URLs are canonical.
    for asset in static_routes.DASHBOARD_PAGE_ASSETS.values():
        assert ("/" + asset) in static_routes.BLOCKED_STATIC_HTML_PATHS, asset
    assert ("/" + static_routes.DASHBOARD_REPO_ASSET) in static_routes.BLOCKED_STATIC_HTML_PATHS


def test_page_routes_match_the_pages_table():
    routes = {meta["route"]: meta["asset"]
              for meta in dashboard_shell.PAGES.values() if meta["route"]}
    assert routes == static_routes.DASHBOARD_PAGE_ASSETS


def test_worker_serves_repo_document_and_redirects_legacy_urls():
    # The Worker implementation lives in app_runtime; entry only exports the
    # lazy route helpers after the runtime split.
    assert 'DASHBOARD_REPO_ASSET' in RUNTIME_TEXT
    assert "dashboard_section_redirect(url.path, url.query)" in RUNTIME_TEXT
    assert "_serve_dashboard_shell" not in RUNTIME_TEXT
    # Legacy section names all map to per-page paths (a 308 is permanent, so
    # the mapping must stay in sync with PAGES routes).
    legacy = set(static_routes.DASHBOARD_SECTION_PATHS)
    assert legacy == {"home", "repos", "network", "chat", "profile",
                      "profile-overview", "profile-repositories"}
    for target in static_routes.DASHBOARD_SECTION_PATHS.values():
        assert target in static_routes.DASHBOARD_PAGE_ASSETS, target


def test_bundle_has_no_legacy_section_router():
    js = assembled_dashboard_js()
    for gone in ("function setSection(", "function showSection(",
                 "SECTION_ROUTES", "function sectionUrl(",
                 "`/dashboard?section="):
        assert gone not in js, gone
    # Per-page boot + within-page routers that remain.
    for kept in ("PAGE_INITS", "function initSharedChrome(",
                 "function initPageHistory(", "settingsSectionFromPath",
                 "function openRepoPage("):
        assert kept in js, kept


def test_dashboard_html_duplicate_is_gone():
    assert not (PUBLIC / "dashboard.html").exists()
    assert "/dashboard.html" not in WRANGLER["assets"]["run_worker_first"]


def test_no_built_asset_still_links_legacy_section_urls():
    for path in sorted(PUBLIC.rglob("*.html")) + sorted(PUBLIC.glob("*.js")):
        text = path.read_text(encoding="utf-8")
        if path.name.endswith(".js") and "SECTION_PATHS" in text:
            continue  # the boot shim maps legacy URLs; it does not emit them
        assert 'href="/dashboard?section=' not in text, path
