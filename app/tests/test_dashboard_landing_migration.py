#!/usr/bin/env python3
"""Static contracts for the landing/dashboard split."""

from pathlib import Path
import re
import tomllib

from _dashboard_shell import assembled_dashboard, assembled_dashboard_page
from _dashboard_bundle import assembled_dashboard_js
import dashboard_shell
from dashboard_shell import PAGES
import static_routes


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
WWW_PUBLIC = ROOT.parent / "www" / "public"
WORLD_PUBLIC = ROOT.parent / "world" / "public"
VIEWS = PUBLIC / "dashboard" / "partials" / "views"
ENTRY_TEXT = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
URLS_TEXT = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
STATIC_ROUTES_TEXT = (ROOT / "src" / "static_routes.py").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
REDIRECTS = (WWW_PUBLIC / "_redirects").read_text(encoding="utf-8")
REPO_HOST_ROUTE_RE = (
    'r"^/api/repo/([^/]+)/([^/]+)/(host|tree|blobs|blob|raw|history|commit|compare|merge-preview|branches|search|stats|sizes)$"'
)
REPO_DIRECT_BROWSE_GATE = "and action in REPO_HOST_READ_ACTIONS"


def _repo_host_read_actions() -> set[str]:
    match = re.search(
        r"REPO_HOST_READ_ACTIONS = frozenset\(\{(.*?)\}\)", ENTRY_TEXT, re.S)
    assert match
    return set(re.findall(r'"([^"]+)"', match.group(1)))


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_dashboard_shell_is_split_into_composable_partials():
    source_shell = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")

    # The authored shell stays split - it references partials rather than
    # inlining the chrome.
    assert "<!--#include" in source_shell
    for name in ("header", "sidebar", "main", "modals"):
        assert (PUBLIC / "dashboard" / "partials" / (name + ".html")).is_file()
        assert ('<!--#include partial="%s"-->' % name) in source_shell
    assert '<!--#include partial="network-rail"-->' not in source_shell

    # One prebuilt static document per PAGES entry, each carrying its page id
    # and exactly one view, with no runtime include pass. The old single-shell
    # public/dashboard.html duplicate is gone.
    for page_id, meta in PAGES.items():
        built = (PUBLIC / meta["asset"]).read_text(encoding="utf-8")
        assert built == assembled_dashboard_page(page_id)
        assert "<!--#include" not in built
        assert ('<body class="bg-background text-foreground" data-page="%s">' % page_id) in built
        assert built.count("<section data-view=") == 1
    assert not (PUBLIC / "dashboard.html").exists()
    assert "self.env.ASSETS.fetch(" in ENTRY_TEXT
    assert "assemble_shell(" not in ENTRY_TEXT
    assert "tools/build_dashboard_assets.py" in ENTRY_TEXT


def test_www_root_keeps_regular_site_and_links_to_the_world():
    index = _read(WWW_PUBLIC / "index.html")
    world = _read(WORLD_PUBLIC / "world" / "index.html")

    assert "ForkMesh lets developers collaborate on, mirror, and preserve" in index
    assert "without relying on any single hosting provider" in index
    assert "Code hosting that lives on the network." not in index
    # The World drops visitors straight into the interactive city: the old
    # "A living city for code." marketing hero is gone, and the shell stays
    # blank while it boots — only a watchdog-revealed load error remains.
    assert "A living city for code." not in world
    assert "ENTERING THE WORLD" not in world
    assert "data-world-load-error" in world
    assert 'data-world-mode="public"' in world
    # The marketing worker answers / with the regular site. The combined app
    # serves its dashboard at /, while self-hosted single-worker installs can
    # opt into the staged marketing homepage.
    assert "forkmesh.session" not in index
    assert 'location.replace("/dashboard")' not in index
    assert 'location.replace("/dashboard.html")' not in index
    assert "/" in WRANGLER["assets"]["run_worker_first"]
    assert 'if url.path == "/" and method_name(request) in ("GET", "HEAD"):' in ENTRY_TEXT
    assert 'return await self._serve_dashboard_asset(\n                url, "dashboard/index.html")' in ENTRY_TEXT
    assert 'base + "index.html"' in ENTRY_TEXT
    assert 'href="https://world.forkmesh.com/' in index


def test_dashboard_exposes_live_hydration_targets():
    dashboard = assembled_dashboard()

    assert 'src="/dashboard.js?v=${version}"' in dashboard
    assert dashboard.count('src="/dashboard-chat.js?v=${version}"') == 1
    for marker in (
        "data-dashboard-profile-name",
        "data-sidebar-repo-list",
        "data-sidebar-repo-count",
        "data-logout-button",
        "data-repo-count",
        "data-network-node-list",
        "data-network-summary",
    ):
        assert marker in dashboard

    # The persistent network rail (and its mini chat widget) is gone: network
    # stats live in the Network section and chat is now its own dashboard
    # section/route, not a sidebar rail.
    assert "data-network-rail-summary" not in dashboard
    assert 'id="sideChatMessages"' not in dashboard

    for placeholder in (
        'data-repo="you/meshcore"',
        'data-repo="relay-eu-1/relay-monitor"',
        "1,204",
    ):
        assert placeholder not in dashboard


def test_dashboard_get_paid_button_uses_small_sol_logo():
    # The button lives in the shared header partial, so any composed page has it.
    dashboard = assembled_dashboard_page("home")

    assert 'href="/mirror-payouts"' in dashboard
    assert 'src="/assets/sol.png"' in dashboard
    assert 'alt="" aria-hidden="true"' in dashboard
    assert 'class="h-4 w-4 shrink-0 rounded-full object-contain"' in dashboard


def test_dashboard_home_hides_unready_sponsorship_target_list():
    dashboard = assembled_dashboard()

    assert "Sponsorship target list" not in dashboard
    for sponsor in ("DigitalOcean", "Tailscale", "Sentry", "Supabase"):
        assert sponsor not in dashboard


def test_dashboard_nav_links_to_chat_page():
    dashboard = _read(PUBLIC / "dashboard" / "index.html")

    drawer_nav = dashboard.split('data-sidebar-main-menu', 1)[1].split(
        "</nav>", 1)[0]
    assert drawer_nav.index('href="https://forkmesh.com/"') < drawer_nav.index("Repositories")
    assert drawer_nav.index("Repositories") < drawer_nav.index('href="/dashboard/chat"')
    assert drawer_nav.index('href="/dashboard/chat"') < drawer_nav.index('href="/dashboard/network"')
    assert drawer_nav.index('href="/dashboard/network"') < drawer_nav.index('href="/docs"')
    assert "Chat" in drawer_nav
    assert 'data-nav="chat" data-nav-link href="/dashboard/chat"' in drawer_nav
    assert 'data-lucide="messages-square"' in drawer_nav
    # Sidebar entries are real page links now - no client-router buttons.
    assert "<button" not in drawer_nav
    assert "data-section=" not in drawer_nav
    assert 'href="/chat"' not in drawer_nav


def test_dashboard_uses_github_system_font_without_affecting_code_or_site_fonts():
    dashboard = _read(PUBLIC / "dashboard" / "index.html")
    dashboard_js = _read(PUBLIC / "dashboard.js")
    site_css = _read(WWW_PUBLIC / "styles.css")

    assert 'font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Noto Sans", Helvetica, Arial, sans-serif' in dashboard
    assert 'src: url("/assets/fonts/HelveticaNeueRoman.otf") format("opentype")' not in dashboard
    assert 'src: url("/assets/fonts/HelveticaNeueBold.otf") format("opentype")' not in dashboard
    assert 'sans: ["-apple-system", "BlinkMacSystemFont", "Segoe UI", "Noto Sans", "Helvetica", "Arial", "sans-serif"]' in dashboard
    assert 'font-family: "ForkMesh Dashboard Mono"' in dashboard
    assert 'mono: ["ForkMesh Dashboard Mono", "ui-monospace", "SFMono-Regular", "monospace"]' in dashboard
    assert "fonts.googleapis.com" not in dashboard
    assert "Outfit" not in dashboard
    assert "Geist Mono" not in dashboard
    assert (
        '<pre class="max-h-[32rem] overflow-auto p-4 font-mono text-xs leading-5 text-muted-foreground"><code>' in dashboard_js
        or 'font-mono text-foreground">${escapeHtml(line) || " "}</span>' in dashboard_js
        or 'font-mono text-zinc-200">${highlightCodeLine(line, path)}</span>' in dashboard_js
    )
    assert '--sans: "ForkMesh Lato"' in site_css
    assert "ForkMesh Helvetica" not in site_css


def test_dashboard_hydrator_uses_existing_worker_apis():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for endpoint in (
        "/api/repositories",
        "/api/network/overview",
    ):
        assert endpoint in dashboard_js

    assert "forkmesh.session" in dashboard_js
    assert "renderRepositories" in dashboard_js
    assert "renderNetwork" in dashboard_js
    assert "requestedRepoKey()" in dashboard_js
    # Signed-out visitors browse repositories as guests instead of being
    # bounced back to the landing page (adhoc #123).
    assert 'location.replace("/");\n        return;' not in dashboard_js
    assert "data-guest-auth-link" in dashboard_js
    # No fabricated "guest" profile (adhoc #185): guests keep the baked chrome
    # defaults, and the account pages bounce signed-out visitors to login.
    assert 'renderProfile(session || { nodeName: "guest" })' not in dashboard_js
    assert '{ nodeName: "guest" }' not in dashboard_js
    assert 'location.replace("/login?next="' in dashboard_js
    assert "function clearForkMeshBrowserState()" in dashboard_js
    assert 'key?.startsWith("forkmesh.") && !preserved.has(key)' in dashboard_js
    assert "forkmesh_session=; Path=/; Max-Age=0" in dashboard_js


def test_dashboard_repository_list_has_loading_state_before_empty_filter():
    dashboard = assembled_dashboard_page("repos")
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "Loading repositories from an online node..." in dashboard
    assert "repositoriesLoading: true" in dashboard_js
    assert "if (state.repositoriesLoading)" in dashboard_js
    assert "Loading repositories from an online node..." in dashboard_js
    assert "No repositories match this filter." in dashboard_js
    assert dashboard_js.index("if (state.repositoriesLoading)") < \
        dashboard_js.index("No repositories match this filter.")


def test_dashboard_profile_page_removes_secondary_profile_picture_card():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")
    login_js = _read(PUBLIC / "login.js")

    assert "A generated placeholder is shown for now" not in dashboard
    assert ">IMG<" not in dashboard
    assert "data-profile-page-avatar-image" in dashboard
    assert "<h2 class=\"text-sm font-semibold text-foreground\">Profile picture</h2>" not in dashboard
    assert "data-profile-picture-avatar" not in dashboard
    assert "data-profile-picture-name" not in dashboard
    assert "data-profile-picture-status" not in dashboard
    assert '$("[data-profile-picture-avatar]")' not in dashboard_js
    assert '$("[data-profile-picture-name]")' not in dashboard_js
    assert '$("[data-profile-picture-status]")' not in dashboard_js
    assert "avatarPng: body.avatarPng || \"\"" in dashboard_js
    assert "avatarUpdatedAt: Number(body.avatarUpdatedAt) || 0" in dashboard_js
    assert "function applyAvatar" in dashboard_js
    assert "refreshPublicProfile" in dashboard_js
    assert "avatarPng: body.avatarPng || \"\"" in login_js


def test_dashboard_loads_profile_once_without_periodic_polling():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    # Boot reuses the canonical payload stored by login and chains the
    # notification load off it. Only an old/cross-origin session takes one
    # fallback read, and no periodic re-fetch follows it.
    assert "canonicalProfileReady = hydrateCanonicalProfile(session)" in dashboard_js
    assert (
        "if (!force && Number(session?.accountSnapshotVersion) >= 1) return session;"
        in dashboard_js
    )
    assert "if (bounceToVerification(nextSession)) return;" in dashboard_js
    assert "return loadNotifications();" in dashboard_js
    assert ".catch(() => {});" in dashboard_js
    assert "function startProfileSync()" not in dashboard_js
    assert "async function pollStatus(" not in dashboard_js
    # Session validity is event-driven too: the 20s poll this used to assert
    # was the relay's most-requested endpoint, and the account event socket's
    # ticket handshake now carries the revocation signal instead. See
    # test_poll_digest for the full contract.
    assert not re.search(r"setInterval\(\s*validate\b", dashboard_js)
    assert "startAccountEventChannel(" in dashboard_js
    assert "if (response.status === 401) logout();" in dashboard_js


def test_dashboard_defaults_to_home_and_keeps_repositories_available():
    home = _read(PUBLIC / "dashboard" / "index.html")
    repos = assembled_dashboard_page("repos")

    # /dashboard is the home document: the home view is baked active and the
    # home sidebar link carries aria-current at build time (no JS pass).
    assert 'data-page="home"' in home
    assert 'data-nav="home" data-nav-link aria-current="page"' in home
    assert 'data-nav="repos" data-nav-link aria-current="page"' not in home
    visible = _strip_html_comments(home)
    assert 'data-view="home" class="view active' in visible
    assert 'data-view="repos"' not in visible
    assert 'data-sidebar-main-menu' in home
    # Repositories stay one real link away: /dashboard/repos is its own page
    # document with its own baked-active nav state.
    assert 'href="/dashboard/repos"' in home
    assert 'data-page="repos"' in repos
    assert 'data-nav="repos" data-nav-link aria-current="page"' in repos
    assert 'data-view="repos" class="view active' in _strip_html_comments(repos)


def test_dashboard_profile_views_are_own_pages_and_client_section_router_is_gone():
    home = _read(PUBLIC / "dashboard" / "index.html")
    profile = assembled_dashboard_page("profile")
    profile_repositories = assembled_dashboard_page("profile-repositories")
    dashboard_js = _read(PUBLIC / "dashboard.js")
    visible = _strip_html_comments(home)

    assert 'data-view="home" class="view active' in visible
    assert 'data-view="profile-overview"' in profile
    assert 'data-view="profile-repositories"' in profile_repositories
    assert 'data-profile-tabs' in profile
    assert 'data-profile-repo-search' in profile_repositories
    assert 'data-home-feed' in home
    assert 'data-home-top-repositories' in home
    assert 'data-home-right-rail' in home
    assert 'data-home-left-rail' in home
    # The rail's own account chip is gone: the header's avatar links to account
    # settings and its adjacent menu offers the other signed-in actions.
    assert 'data-home-user-avatar' not in home
    assert 'data-home-user-name' not in home
    assert 'data-composer-slot' in home
    assert 'data-home-agents' in home
    assert 'data-home-action-panel' not in home
    assert 'data-home-agent-input' not in home
    assert 'data-home-agent-sample' not in home
    assert 'data-home-feed-filter' not in home
    assert 'data-home-blog-card' in home
    assert 'data-home-contributions' not in visible
    # The client-side section router is deleted: boot dispatches per page via
    # PAGE_INITS keyed off <body data-page>.
    for removed in ("SECTION_ROUTES", "showSection(", "requestedSection(", "sectionUrl("):
        assert removed not in dashboard_js
    assert "const PAGE_INITS = {" in dashboard_js
    assert "(PAGE_INITS[currentPage()] || initHomePage)();" in dashboard_js
    # Legacy /dashboard?section=X URLs 308 in the Worker, with a client shim
    # for cached home documents.
    assert "target = dashboard_section_redirect(url.path, url.query)" in ENTRY_TEXT
    assert "function legacyRedirectTarget()" in dashboard_js
    assert "location.replace(legacyTarget);" in dashboard_js


def test_dashboard_home_left_rail_uses_theme_aware_panel_background():
    home = _read(VIEWS / "home.html")

    # bg-card (not a hardcoded dark hex) so the rail switches with the
    # dashboard's light/dark theme instead of always rendering dark.
    assert 'data-home-left-rail class="min-w-0 border-b border-border bg-card' in home
    assert "bg-[#0d1117]" not in home
    assert 'lg:border-b-0 lg:border-r' in home
    assert 'lg:min-h-full' in home
    assert 'data-home-content-column class="min-w-0 px-4 py-6 sm:px-6 lg:px-8"' in home


def test_dashboard_profile_about_is_editable_for_logged_in_user():
    # The about modal lives in the shared modals partial; the panel is in the
    # profile-overview view, which is the profile page's only view.
    dashboard = assembled_dashboard_page("profile")
    dashboard_js = _read(PUBLIC / "dashboard.js")
    profile = _read(VIEWS / "profile-overview.html")

    for marker in (
        "data-profile-about-panel",
        "data-profile-about-owner",
        "data-profile-about-body",
        "data-profile-about-edit",
        "data-profile-about-modal",
        "data-profile-about-textarea",
        "data-profile-about-save",
        "data-profile-about-hint",
    ):
        assert marker in dashboard
    assert "README.md" not in profile
    assert "About yourself" in profile
    assert 'aria-label="Edit about yourself"' in profile

    for marker in (
        "function renderProfileAbout(session)",
        "function setProfileAboutModalOpen(open)",
        "async function saveProfileAbout()",
        "profileAbout: body.profileAbout ?? body.profileReadme ?? base.profileAbout ?? base.profileReadme ??",
        "postProfile({ profileAbout })",
        "renderProfileAbout(nextSession)",
        'const sidebarName = $("[data-sidebar-user-name]")',
    ):
        assert marker in dashboard_js


def test_dashboard_profile_overview_uses_real_profile_data_not_placeholders():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    # The whole composed profile page: the overview view plus the shared
    # profile-sidebar template (which moved into the main.html wrapper).
    profile = assembled_dashboard_page("profile")

    assert "data-profile-overview-layout" in profile
    assert "data-profile-overview-card" not in profile
    assert "lg:grid-cols-[296px_minmax(0,1fr)]" in profile
    assert "h-72 w-72" in profile
    assert "data-profile-bio" in profile
    assert "data-profile-followers" in profile
    assert "data-profile-following" in profile
    assert "data-profile-mirrors" in profile
    assert "data-profile-location" in profile
    assert "data-profile-timezone" in profile
    assert "data-profile-achievements" not in profile
    assert "data-profile-highlights" not in profile
    assert "Sponsors dashboard" not in profile
    assert "Cracked dev @ 20" not in profile
    assert "Breaking the INTERNET" not in profile
    assert ">30<" not in profile
    assert ">19<" not in profile
    assert "data-profile-about-panel" in profile
    assert "data-profile-contribution-card" in profile
    assert "data-profile-activity-list" in profile
    assert "profileFollowers" in dashboard_js
    assert "profileFollowing" in dashboard_js
    assert "profileMirrorCount" in dashboard_js
    assert "session?.profileLocation" in dashboard_js
    assert "session?.profileTimezone" in dashboard_js
    assert 'rounded-lg border border-border bg-card p-4' not in profile


def test_dashboard_profile_contribution_card_has_panorama_contract():
    profile = _read(VIEWS / "profile-overview.html")
    state_js = _read(PUBLIC / "dashboard" / "js" / "01-state.js")

    for marker in (
        "data-profile-contribution-card",
        "data-profile-contribution-summary",
        "data-profile-contribution-skyline",
        "data-profile-contribution-tooltip",
        "data-profile-contribution-radar",
        "data-profile-contribution-languages",
        "data-profile-contribution-metrics",
        "data-profile-contribution-coverage",
        "data-profile-contribution-periods",
        "data-profile-contribution-loading",
        "data-profile-contribution-empty",
        "data-profile-contribution-error",
        "data-profile-contribution-retry",
        "data-profile-activity-items",
        "data-profile-activity-empty",
        "Contribution settings",
        "Learn how we count contributions",
    ):
        if marker == "Contribution settings":
            assert marker not in profile
        else:
            assert marker in profile

    assert "data-profile-contribution-calendar" not in profile
    assert "data-contribution-months" not in profile
    assert "data-contribution-cells" not in profile
    assert "data-contribution-legend" not in profile
    assert "min-w-[880px]" not in profile
    assert "#1c1c1f" not in profile
    assert "#14532d" not in profile
    assert 'href="/docs/contributions"' in profile
    assert profile.count("<svg") == 3
    assert profile.count("<title") == 3
    assert profile.count("<desc") == 3
    assert 'data-profile-activity-list class="grid gap-6"' in profile
    for field in (
        "range: null",
        "data: null",
        "loading: false",
        'error: ""',
        'requestKey: ""',
        'selectedDay: ""',
        "cache: {}",
    ):
        assert field in state_js
    for stale in ("year:", "liveHistory", "loadedYears"):
        assert stale not in state_js


def test_dashboard_profile_contribution_theme_is_semantic_and_responsive():
    shell = _read(PUBLIC / "dashboard" / "shell.html")

    variables = (
        "--contribution-commits",
        "--contribution-issues",
        "--contribution-pulls",
        "--contribution-reviews",
        "--contribution-repositories",
        "--contribution-empty",
        "--contribution-unverified",
        "--contribution-grid-edge",
        "--contribution-cube-top",
        "--contribution-cube-left",
        "--contribution-cube-right",
        "--contribution-tooltip-bg",
        "--contribution-tooltip-fg",
        "--contribution-tooltip-border",
    )
    dark = shell[shell.index(":root,"):shell.index('[data-dashboard-theme="light"]')]
    light = shell[shell.index('[data-dashboard-theme="light"]'):shell.index("html {")]
    for variable in variables:
        assert variable in dark
        assert variable in light

    assert "[data-profile-contribution-panorama]" in shell
    assert "grid-template-columns: minmax(0, 1fr)" in shell
    assert "[data-profile-contribution-skyline-viewport]" in shell
    assert "overflow-x: auto" in shell
    assert "@media (prefers-reduced-motion: reduce)" in shell
    assert "[hidden] {" in shell
    assert "display: none !important" in shell
    assert "initContributionActivity" not in shell
    assert "contributionColors" not in shell


def test_dashboard_profile_contribution_renderer_uses_one_native_request():
    account_js = _read(PUBLIC / "dashboard" / "js" / "04-account.js")
    helpers_js = _read(PUBLIC / "dashboard" / "js" / "02-helpers.js")
    explorer_js = _read(PUBLIC / "dashboard" / "js" / "05-repo-list-explorer.js")
    network_js = _read(PUBLIC / "dashboard" / "js" / "08-repo-detail-network.js")

    for helper in (
        "profileContributionRange",
        "normalizeProfileContributionDays",
        "profileContributionStackLevel",
        "profileContributionCubeFaces",
        "profileContributionRadarPoints",
        "profileContributionLanguageSegments",
        "profileContributionTooltipText",
        "profileContributionCoverageMessage",
        "profileContributionCategoryKnown",
        "renderProfileContributionStatus",
        "renderProfileActivity",
    ):
        assert f"function {helper}" in account_js

    assert '"/api/accounts/" + encodeURIComponent(name) + "/contributions?from="' in account_js
    assert '"&to=" + encodeURIComponent(range.to)' in account_js
    assert "state.profileContributions.requestKey !== requestKey" in account_js
    assert "sessionStorage.setItem(profileContributionCacheKey(requestKey)" in account_js
    assert 'data-profile-contribution-period="${escapeHtml(period.value)}"' in account_js
    assert 'aria-current="${active ? "true" : "false"}"' in account_js
    assert "scrollLeft = viewport.scrollWidth - viewport.clientWidth" in account_js
    assert "cacheBust: false" in account_js
    assert "error.status = response.status" in helpers_js
    assert "temporaryFailure" in account_js
    assert "if (!temporaryFailure) removeProfileContributionCache(requestKey)" in account_js
    assert "currentYear - 5" in account_js
    assert "md:hidden" in account_js
    assert "Loading verified contribution activity" in account_js
    assert "Contribution activity is unavailable" in account_js

    for stale in (
        "loadProfileContributionHistories",
        "PROFILE_HISTORY_REPO_LIMIT",
        "PROFILE_HISTORY_CONCURRENCY",
        "addCatalogActivityWeeks",
        "commitMatchesProfile",
        "profileContributionAliases",
    ):
        assert stale not in account_js
    assert 'repoLiveUrl(repo, "history")' not in account_js
    assert "renderProfileContributionGraph();" not in explorer_js
    assert "data-profile-contribution-period" in network_js
    assert "data-profile-contribution-retry" in network_js
    assert "data-profile-contribution-year" not in network_js


def test_dashboard_profile_tabs_are_unified_with_app_header():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    overview = assembled_dashboard_page("profile")
    repositories = assembled_dashboard_page("profile-repositories")

    for profile in (overview, repositories):
        header = profile[
            profile.index("data-app-header") - 80:
            profile.index("data-app-header") + 260
        ]
        assert "border-b border-border" not in header
        assert "data-profile-header-band" in profile
        assert "data-profile-tabs-inner" in profile
        assert 'data-profile-header-band class="border-b border-border bg-background px-4"' in profile
        assert 'data-profile-tabs-inner class="w-full"' in profile
        assert 'data-profile-tabs class="flex min-w-0 gap-2 overflow-x-auto text-sm"' in profile
        assert profile.index("data-profile-header-band") < profile.index("data-profile-tabs")
        assert 'data-profile-tabs-inner class="mx-auto max-w-[1280px]"' not in profile
        assert "mb-8 flex min-w-0 gap-2 overflow-x-auto border-b border-border text-sm" not in profile
        assert "mb-5 flex min-w-0 gap-4 overflow-x-auto border-b border-border text-sm" not in profile

    # The legacy section name is baked per page at build time; there is no JS
    # pass mutating the root's section any more.
    assert 'data-dashboard-root data-dashboard-section="home"' in assembled_dashboard_page("home")
    assert 'data-dashboard-root data-dashboard-section="profile-overview"' in overview
    assert '[data-dashboard-section="home"] [data-app-header]' in overview
    assert "dashboardRoot.dataset.dashboardSection" not in dashboard_js
    assert overview.index("data-profile-header-band") < overview.index("data-profile-overview-layout")
    assert repositories.index("data-profile-header-band") < repositories.index("data-profile-repo-search")
    # The context slot ships empty and is filled per page by the JS; the home
    # page keeps it empty rather than restating "Dashboard" next to the logo.
    assert 'data-dashboard-header-context class="min-w-0 truncate"></span>' in overview
    assert 'headerContext.textContent = "Dashboard";' not in dashboard_js
    assert 'const headerContext = $("[data-dashboard-header-context]")' in dashboard_js
    assert 'const renderedName = ($("[data-profile-page-node-name]")?.textContent || "").trim();' in dashboard_js
    assert 'function renderHeaderContext(section = currentSection())' in dashboard_js
    assert 'renderHeaderContext();' in dashboard_js


def test_dashboard_profile_tabs_link_to_network_without_placeholder_tabs():
    overview = _read(VIEWS / "profile-overview.html")
    repositories = _read(VIEWS / "profile-repositories.html")
    network = _read(VIEWS / "network.html")

    for profile in (overview, repositories, network):
        tabs = profile[
            profile.index("data-profile-tabs")
            : profile.index("</nav>", profile.index("data-profile-tabs"))
        ]
        # Tabs are real page links now, not client-router buttons.
        assert 'href="/dashboard/profile"' in tabs
        assert 'href="/dashboard/profile/repositories"' in tabs
        assert 'href="/dashboard/network"' in tabs
        assert "data-section=" not in tabs
        assert "<button" not in tabs
        assert tabs.index("Overview") < tabs.index("Repositories")
        assert tabs.index("Repositories") < tabs.index("Network")
        assert "Projects" not in tabs
        assert "Packages" not in tabs
        assert "Stars" not in tabs
        assert ">40<" not in tabs

    # The active tab underline is baked into each page's own view.
    assert 'href="/dashboard/profile" class="inline-flex items-center gap-2 border-b-2 border-[#f78166]' in overview
    assert 'href="/dashboard/profile/repositories" class="inline-flex items-center gap-2 border-b-2 border-[#f78166]' in repositories
    assert 'href="/dashboard/network" class="inline-flex items-center gap-2 border-b-2 border-[#f78166]' in network
    assert 'data-network-page-inner class="w-full px-4 py-6 sm:px-6 lg:px-8"' in network
    assert "mx-auto max-w-[1432px]" in network
    for icon in ("shield", "zap", "lock", "refresh-cw"):
        assert (
            f'data-lucide="{icon}" class="w-3.5 h-3.5 text-muted-foreground shrink-0"'
            in network
        )
        assert (
            f'data-lucide="{icon}" class="w-3.5 h-3.5 text-primary shrink-0"'
            not in network
        )


def test_dashboard_profile_repositories_reuses_overview_sidebar_component():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    overview = _read(VIEWS / "profile-overview.html")
    repositories = _read(VIEWS / "profile-repositories.html")
    network = _read(VIEWS / "network.html")

    # The sidebar template ships in the shared main.html wrapper so every page
    # document can render it into its slot.
    assert "data-profile-sidebar-template" in _read(
        PUBLIC / "dashboard" / "partials" / "main.html")
    assert 'data-profile-sidebar-slot data-profile-sidebar-context="overview"' in overview
    assert 'data-profile-sidebar-slot data-profile-sidebar-context="repositories"' in repositories
    assert 'data-profile-sidebar-slot data-profile-sidebar-context="network"' in network
    assert 'data-network-layout class="grid gap-8 lg:grid-cols-[296px_minmax(0,1fr)]"' in network
    assert network.index('data-profile-sidebar-slot data-profile-sidebar-context="network"') < network.index("data-network-summary")
    assert "data-profile-sidebar-card" not in repositories
    assert "data-profile-sidebar-card" not in network
    assert 'rounded-lg border border-border bg-card p-4' not in repositories
    assert "function profileSidebarMarkup(session)" in dashboard_js
    assert "function renderProfileSidebars(session)" in dashboard_js
    assert '$$("[data-profile-sidebar-slot]").forEach' in dashboard_js
    assert "renderProfileSidebars(session);" in dashboard_js
    assert "$$('[data-profile-page-node-name]')" in dashboard_js
    assert "$$('[data-profile-page-email]')" in dashboard_js


def test_dashboard_profile_repository_count_uses_loaded_repository_groups():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    overview = _read(VIEWS / "profile-overview.html")
    repositories = _read(VIEWS / "profile-repositories.html")

    assert 'data-profile-repo-count' in overview
    assert 'data-profile-repo-count' in repositories
    assert ">226<" not in overview
    assert ">226<" not in repositories
    assert "function renderProfileRepositoryCount" in dashboard_js
    assert '$$("[data-profile-repo-count]").forEach' in dashboard_js
    assert "renderProfileRepositoryCount();" in dashboard_js
    # Counts follow the same source as the list, which is scoped to the viewed
    # account on every profile page (see the owner-scoping test below).
    assert "profileRepositoryGroups().length" in dashboard_js


def test_dashboard_profile_repositories_refresh_after_identity_hydration():
    dashboard_js = assembled_dashboard_js()
    hydration = dashboard_js.split(
        "async function hydrateCanonicalProfile(session, { force = false } = {})", 1
    )[1]
    hydration = hydration.split("function repoBelongsToProfile", 1)[0]
    render = dashboard_js.split("function renderProfile(session)", 1)[1]
    render = render.split("function dashboardMockRepositoriesEnabled", 1)[0]

    # The canonical account response supplies linked-node aliases that may not
    # exist in the cached session. If the repository catalog won the boot race,
    # renderProfile must recompute both consumers after writeSession installs
    # those aliases (#586).
    assert hydration.index("writeSession(nextSession);") < hydration.index(
        "renderProfile(nextSession);")
    assert "renderProfileRepositories();" in render
    assert "renderProfileRepositoryCount();" in render


def test_dashboard_profile_repositories_are_scoped_to_the_viewed_account():
    # The authenticated catalog returns every public repo plus the private repos
    # shared WITH the viewer, so the profile must filter by owner itself. It
    # previously only did so in public-profile mode, which listed the whole mesh
    # under "Your repositories" and counted repos the account does not own
    # (#538). The filter must therefore be unconditional.
    dashboard_js = _read(PUBLIC / "dashboard.js")
    body = dashboard_js.split("function profileRepositoryGroups()", 1)[1]
    body = body.split("function renderProfileRepositories", 1)[0]

    # No public-profile-only gate around the filter.
    assert "if (state.publicProfile)" not in body
    # The subject-derived aliases are always applied to the loaded groups.
    assert "profileRepositoryAliases()" in body
    assert "repoBelongsToProfile(sourceOfTruth(group), aliases)" in body
    assert "(group.members || []).some((member) => repoBelongsToProfile(member, aliases))" in body
    # An account with no resolvable identity owns nothing - never the catalog.
    assert "if (!aliases.size) return [];" in body


def test_dashboard_home_uses_github_dark_typography_and_blue_links():
    dashboard = _read(PUBLIC / "dashboard" / "index.html")
    dashboard_js = _read(PUBLIC / "dashboard.js")
    home = _read(VIEWS / "home.html")

    assert '--dashboard-background-rgb: 1 4 9' in dashboard
    assert '--dashboard-card-rgb: 13 17 23' in dashboard
    assert '"-apple-system", "BlinkMacSystemFont", "Segoe UI", "Noto Sans"' in dashboard
    assert '--dashboard-link: #58a6ff' in dashboard
    assert 'text-accent hover:underline' in home
    assert 'text-primary hover:underline' not in home
    assert 'text-accent hover:underline' in dashboard_js
    assert 'text-primary hover:underline">${escapeHtml(repo.name || "repository")}' not in dashboard_js


def test_dashboard_home_widgets_are_wired_to_real_data_and_actions():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    home = _read(VIEWS / "home.html")

    assert "Update your 2FA" not in home
    assert "to see dashboard activity within ForkMesh" not in home
    # The hand-written "field notes" AD card is gone from the right rail; the
    # live blog card below it is the rail's only editorial block.
    assert 'data-home-ad-card' not in home
    assert "ForkMesh field notes" not in home
    assert 'href="/blog/parallel-agents/"' not in home
    assert 'href="/blog"' in home
    assert 'data-home-feed-card' not in home
    assert "Dashboard feed adopts GitHub-style repository discovery" not in home

    for marker in (
        "function renderHomeFeed()",
        "function renderHomeBlogPosts()",
        "async function loadHomeAgentSessions()",
        'orgAgentRequest("GET", "/api/account/agent-sessions")',
        "return actor && !self.has(actor);",
        "const searchable = all.length >= HOME_REPO_SEARCH_MIN_REPOSITORIES;",
        "renderHomeFeed();",
        "repositoryMatchesQuery(sourceOfTruth(group), query)",
    ):
        assert marker in dashboard_js

    assert "3 hours ago" not in dashboard_js
    assert "mesh-maintainer" not in dashboard_js


def test_home_right_rail_lists_latest_blog_posts_with_artwork():
    """adhoc #381: the right-rail card shows the three newest blog posts,
    with their images, read from the blog's own edge-cached RSS feed."""
    home = _read(VIEWS / "home.html")
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "Latest from the blog" in home
    assert 'data-home-blog-list' in home
    # The hand-maintained release list the card used to bake in is gone.
    assert "The Living Code City" not in home
    assert "The Living Code City" not in dashboard_js
    assert "Loading release notes" not in home

    for marker in (
        "const HOME_BLOG_POST_LIMIT = 3;",
        'const HOME_BLOG_FEED_URL = "/rss.xml";',
        "function parseHomeBlogFeed(xml)",
        'doc.querySelectorAll("item")',
        'item.querySelector("enclosure")?.getAttribute("url")',
        ".slice(0, HOME_BLOG_POST_LIMIT)",
        "async function loadHomeBlogPosts()",
        "void loadHomeBlogPosts();",
    ):
        assert marker in dashboard_js
    # Post artwork is painted, and titles/URLs stay escaped like every other
    # feed-sourced string in the dashboard.
    assert 'loading="lazy" class="block aspect-[16/9] w-full object-cover"' in dashboard_js
    assert "${escapeHtml(post.image)}" in dashboard_js
    assert "${escapeHtml(post.title)}" in dashboard_js


def test_home_blog_artwork_keeps_the_feed_host_the_app_ships_no_blog_assets():
    """The dashboard runs on the App origin, whose asset bundle deliberately
    carries no /assets/blog/* (build_site_assets.shared_assets skips it), so a
    path-only image src 404s and every card paints a broken image. Post links
    stay path-relative (the Worker 308s /blog/*), artwork keeps the feed's own
    absolute URL, and only ForkMesh's own hosts are ever fetched."""
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        'const HOME_BLOG_IMAGE_HOSTS = new Set(["forkmesh.com", "www.forkmesh.com"]);',
        "function homeBlogImageUrl(value)",
        "if (parsed.origin === window.location.origin) {",
        'if (parsed.protocol !== "https:") return "";',
        "if (!HOME_BLOG_IMAGE_HOSTS.has(parsed.hostname.toLowerCase())) return \"\";",
        "return parsed.href;",
        'image: homeBlogImageUrl(item.querySelector("enclosure")?.getAttribute("url")),',
    ):
        assert marker in dashboard_js
    # The link stays on this origin; only the artwork keeps its host.
    assert 'href: homeBlogUrl(item.querySelector("link")?.textContent),' in dashboard_js
    assert 'image: homeBlogUrl(' not in dashboard_js


def test_home_show_more_repositories_link_is_hidden_for_small_accounts():
    home = _read(VIEWS / "home.html")
    dashboard_js = _read(PUBLIC / "dashboard.js")

    # Ships hidden, so an account that never crosses the threshold is never
    # offered a "more" that expands to the rows already on screen.
    assert (
        'data-home-repos-show-more type="button" class="mt-4 hidden '
        'text-xs font-medium text-muted-foreground hover:text-accent">Show more</button>'
    ) in home
    for marker in (
        "const HOME_TOP_REPOSITORY_LIMIT = 8;",
        "const HOME_SHOW_MORE_MIN_REPOSITORIES = 25;",
        'const showMore = $("[data-home-repos-show-more]");',
        '"hidden", matches.length <= HOME_SHOW_MORE_MIN_REPOSITORIES);',
        "state.homeRepositoriesExpanded",
        "matches.slice(0, HOME_TOP_REPOSITORY_LIMIT)",
        '$("[data-home-repos-show-more]")?.addEventListener("click", () => {',
    ):
        assert marker in dashboard_js


def test_dashboard_pages_have_no_footer_or_version_chip():
    header = _read(PUBLIC / "dashboard" / "partials" / "header.html")
    shell = _read(PUBLIC / "dashboard" / "shell.html")
    page = _read(PUBLIC / "dashboard" / "index.html")
    dashboard_js = _read(PUBLIC / "dashboard.js")

    # The version chip and the footer that carried it are gone: dashboard pages
    # end at the shell, with nothing below it.
    assert not (PUBLIC / "dashboard" / "partials" / "footer.html").exists()
    assert "data-app-version" not in header
    assert 'partial="footer"' not in shell
    assert "data-dashboard-footer" not in page
    assert "data-app-version" not in page
    assert "renderAppVersion" not in dashboard_js
    assert "forkmesh.appVersion" not in dashboard_js


def test_dashboard_top_repositories_include_linked_organization_aliases():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    for marker in (
        "homeOrganizationRepositories",
        "async function loadHomeOrganizationRepositories()",
        'await fetchJson("/api/orgs")',
        'source: "organization-alias"',
        "organizationOwned: true",
        ">organization</span>",
        "void ensureHomeOrganizationRepositories();",
    ):
        assert marker in dashboard_js


def test_dashboard_logo_refreshes_the_dashboard_document():
    header = _read(PUBLIC / "dashboard" / "partials" / "header.html")
    assert "data-dashboard-logo" in header
    assert 'href="/dashboard"' in header
    assert 'aria-label="Refresh ForkMesh Dashboard"' in header


def test_dashboard_has_mobile_responsive_navigation_drawers():
    dashboard = _read(PUBLIC / "dashboard" / "index.html")
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        "data-dashboard-shell",
        "data-mobile-menu-toggle",
        "data-mobile-sidebar-backdrop",
        "data-dashboard-sidebar",
        "dashboard-sidebar-open",
    ):
        assert marker in dashboard
    for removed_marker in (
        "data-mobile-drawer-close",
        "data-mobile-network-toggle",
        "data-mobile-network-backdrop",
        "data-mobile-network-close",
        "data-network-rail-toggle",
        "data-dashboard-network-rail",
        "network-drawer-open",
        "networkRail",
    ):
        assert removed_marker not in dashboard

    for marker in (
        "function setMobileSidebarOpen(open)",
        "function closeMobileDrawers()",
        "data-mobile-menu-toggle",
        "closeMobileDrawers();",
    ):
        assert marker in dashboard_js
    for removed_marker in (
        "function setMobileNetworkOpen(open)",
        "data-mobile-network-toggle",
        "data-mobile-network-close",
        "data-network-rail",
        "network-drawer-open",
    ):
        assert removed_marker not in dashboard_js
    assert "matchMedia(\"(min-width: 1024px)\")" not in dashboard_js


def test_dashboard_header_removes_dead_create_and_network_rail_controls():
    dashboard = _read(PUBLIC / "dashboard" / "index.html")
    header = dashboard[
        dashboard.index("data-app-header") - 80:
        dashboard.index("</header>", dashboard.index("data-app-header"))
    ]

    assert "data-mobile-network-toggle" not in header
    assert "data-network-rail-toggle" not in header
    assert 'aria-label="Open network drawer"' not in header
    assert 'aria-label="Open network panel"' not in header
    assert 'aria-label="Create new"' not in header


def test_dashboard_uses_github_like_global_shell_and_hamburger_drawer():
    dashboard = _read(PUBLIC / "dashboard" / "index.html")
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "--dashboard-background-rgb: 1 4 9" in dashboard
    assert "--dashboard-header-rgb: 1 4 9" in dashboard
    assert "--dashboard-sidebar-rgb: 12 17 23" in dashboard
    assert "--dashboard-border-rgb: 61 68 77" in dashboard
    assert "data-global-search" in dashboard
    assert "data-sidebar-top-repositories" in dashboard
    assert "data-dashboard-sidebar" in dashboard
    assert "data-mobile-menu-toggle" in dashboard
    hamburger = dashboard[
        dashboard.index("data-mobile-menu-toggle") - 240:
        dashboard.index("data-mobile-menu-toggle") + 480
    ]
    sidebar = dashboard[
        dashboard.index("data-dashboard-sidebar") - 240:
        dashboard.index("data-dashboard-sidebar") + 480
    ]
    # The sidebar tracks the theme (light/dark), not a hardcoded dark color,
    # so it doesn't stay black when the rest of the dashboard switches to
    # light mode.
    assert "#0C1117" not in dashboard
    assert "bg-[rgb(var(--dashboard-sidebar-rgb))]" in dashboard
    assert "lg:hidden" not in hamburger
    assert "lg:flex" not in sidebar
    assert "function setMobileSidebarOpen(open)" in dashboard_js
    assert "matchMedia(\"(min-width: 1024px)\")" not in dashboard_js


def test_dashboard_global_header_search_has_keyboard_backed_repo_results():
    dashboard = assembled_dashboard()
    dashboard_js = assembled_dashboard_js()

    assert "data-global-search-shell" in dashboard
    assert "data-global-search-panel" in dashboard
    assert "data-global-search-results" in dashboard
    assert 'aria-controls="globalSearchResults"' in dashboard
    assert 'aria-keyshortcuts="/"' in dashboard
    assert "function focusGlobalSearch()" in dashboard_js
    assert "function renderGlobalSearchResults()" in dashboard_js
    assert "function selectGlobalSearchResult(" in dashboard_js
    assert 'event.key === "/"' in dashboard_js
    assert "focusGlobalSearch()" in dashboard_js
    assert "typingTarget" in dashboard_js
    assert '$("[data-global-search]")?.addEventListener("keydown"' in dashboard_js
    assert "renderRepoDetail(repo);" in dashboard_js


def test_dashboard_global_header_search_also_filters_the_visible_page():
    dashboard = assembled_dashboard()
    dashboard_js = assembled_dashboard_js()

    assert "data-global-search-page-filter" in dashboard
    for marker in (
        "function applyGlobalSearchPageFilter()",
        "function renderGlobalSearchPageFilter()",
        "function pageFilterQueries(ownQuery)",
        "function repositoryMatchesPageFilter(repo, ownQuery)",
        "function renderRepoPageFilter()",
        "function networkRowsMatchingPageFilter(rows)",
        "applyGlobalSearchPageFilter();",
    ):
        assert marker in dashboard_js
    # Filtering reuses already-loaded rows and must not turn a keystroke into a
    # repository/network fetch.
    page_filter = dashboard_js[
        dashboard_js.index("function renderGlobalSearchPageFilter()")
        : dashboard_js.index("function globalSearchPageFilterCount()")
    ]
    assert "fetch(" not in page_filter


def test_dashboard_has_scoped_light_dark_appearance_controls():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")
    landing = _read(WWW_PUBLIC / "index.html")
    login = _read(PUBLIC / "login.html")

    for marker in (
        'data-dashboard-theme',
        'meta name="color-scheme" content="light dark"',
        "data-profile-appearance-panel",
        "data-appearance-theme",
        "data-appearance-theme-status",
        "forkmesh.dashboard.theme",
    ):
        assert marker in dashboard

    # The appearance panel lives on the settings page's view (and nowhere else
    # is a popover shortcut needed).
    settings_view = _read(VIEWS / "settings.html")
    assert "data-profile-appearance-panel" in settings_view
    assert "data-appearance-theme" in settings_view
    assert "data-appearance-settings-button" not in dashboard

    for marker in (
        "function readDashboardTheme()",
        "function applyDashboardTheme(theme)",
        "forkmesh.dashboard.theme",
        "forkmesh.theme",
        "[data-appearance-theme]",
    ):
        assert marker in dashboard_js
    assert 'localStorage.setItem("forkmesh.theme", nextTheme)' in dashboard_js
    assert 'localStorage.getItem("forkmesh.theme")' in dashboard_js
    assert "forkmesh_theme=" in dashboard_js
    assert "Domain=.forkmesh.com" in dashboard_js
    assert 'new URLSearchParams(location.search).get("theme")' in dashboard

    static_js = _read(WWW_PUBLIC / "static-page.js")
    assert 'localStorage.getItem("forkmesh.dashboard.theme")' in static_js
    assert 'localStorage.setItem("forkmesh.dashboard.theme", chosen)' in static_js
    site_header_js = _read(WWW_PUBLIC / "site-header.js")
    for theme_key in ("forkmesh.dashboard.theme", "forkmesh.theme"):
        assert theme_key in site_header_js
    for docs_page in (WWW_PUBLIC / "docs" / "index.html",):
        docs = _read(docs_page)
        assert 'src="/site-header.js?v=${version}"' in docs
        assert 'id="theme-toggle"' not in docs
        assert "function applyTheme" not in docs
        assert "localStorage" not in docs
        assert 'localStorage.getItem("forkmesh.dashboard.theme")' not in docs
        assert 'localStorage.setItem("forkmesh.dashboard.theme", chosen)' not in docs

    assert "function setAppearanceModalOpen(open)" not in dashboard_js
    assert "data-appearance-settings-button" not in dashboard_js
    assert "data-appearance-modal" not in dashboard

    assert "data-profile-appearance-panel" not in landing
    assert "data-appearance-theme" not in landing
    assert "data-home-theme" in landing
    assert 'src="/home-theme.js?v=${version}"' in landing
    assert "forkmesh.dashboard.theme" in landing

    assert "data-profile-appearance-panel" not in login
    assert "data-appearance-theme" not in login
    assert "forkmesh.dashboard.theme" not in login


def test_dashboard_light_theme_overrides_every_dark_theme_color_variable():
    # Regression guard: the header background once stayed black in light mode
    # because --dashboard-header-rgb was only ever defined in the dark theme
    # block. Every color variable set for dark must have a light override too
    # (font-size is the one deliberate exception: it's theme-independent and
    # lives on the shared :root/dark selector).
    dashboard = _read(PUBLIC / "dashboard" / "index.html")

    dark_block = dashboard[
        dashboard.index('[data-dashboard-theme="dark"] {'):
        dashboard.index('[data-dashboard-theme="light"] {')
    ]
    light_block = dashboard[
        dashboard.index('[data-dashboard-theme="light"] {'):
        dashboard.index("* {", dashboard.index('[data-dashboard-theme="light"] {'))
    ]
    dark_vars = set(re.findall(r"--([a-zA-Z0-9-]+):", dark_block)) - {"font-size"}
    light_vars = set(re.findall(r"--([a-zA-Z0-9-]+):", light_block))
    missing = dark_vars - light_vars
    assert not missing, f"light theme is missing overrides for: {sorted(missing)}"


def test_dashboard_mobile_drawer_shadow_only_renders_while_open():
    # Regression guard: the mobile nav drawer's box-shadow used to be set
    # unconditionally on [data-dashboard-sidebar], which sits off-canvas
    # (translateX(-100%)) by default. A box-shadow isn't clipped by its own
    # element being off-screen, so a dark blur bled ~100px into the visible
    # page at all times — most visible in light mode as a smudge in the
    # bottom-left corner. The shadow must only apply once the drawer is
    # actually open.
    dashboard = _read(PUBLIC / "dashboard" / "index.html")

    closed_rule = dashboard[
        dashboard.index("[data-dashboard-sidebar] {"):
        dashboard.index("}", dashboard.index("[data-dashboard-sidebar] {"))
    ]
    open_rule = dashboard[
        dashboard.index("body.dashboard-sidebar-open [data-dashboard-sidebar] {"):
        dashboard.index(
            "}", dashboard.index("body.dashboard-sidebar-open [data-dashboard-sidebar] {")
        )
    ]
    assert "box-shadow: none" in closed_rule
    assert "box-shadow: 0 8px 24px" in open_rule


def _strip_html_comments(html: str) -> str:
    while "<!--" in html:
        start = html.index("<!--")
        end = html.index("-->", start) + 3
        html = html[:start] + html[end:]
    return html


def test_dashboard_repository_detail_keeps_code_comments_issues_shell():
    # The repos list and the worker-served repo detail are separate page
    # documents now; assert across every composed page.
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        'data-view="repos"',
        'data-view="explore"',
        'data-repo-detail',
        'id="dashboard-repo-feature-template"',
        'data-state="open"',
        'data-state="closed"',
        'data-state="all"',
    ):
        assert marker in dashboard
    for marker in (
        "function repositoryCard",
        "function renderRepoDetail",
        "loadRepositoryTree(repo",
        "loadRepositoryBlob(repo",
        # Issues load from the repo's git tree so the Open-by-default state
        # filter (issue #270) has real published issues to work with.
        "loadRepoIssues(repo)",
        # Pull requests and discussions load lazily on first tab view rather than
        # eagerly on every refresh (adhoc #105: eager per-record blob fan-out
        # tripped the host rate limit and made reloads stop working).
        "loadRepoCollection(state.selectedRepo, tab,",
        "loadRepoCommits(repo)",
        "loadRepoMirrors(repo)",
        "Copy clone",
        "Open clean URL",
        "data-dashboard-repo-tab=\"${tab}\"",
        '"code", "issues", "pulls", "discussions", "insights", "projects", "actions", "security", ...(canSeeAgentsTab',
        # Releases load lazily on first tab view from .forkmesh/releases/<channel>/release.json.
        "loadRepoReleases(state.selectedRepo)",
        "loadRepoInsights(state.selectedRepo)",
    ):
        assert marker in dashboard_js
    # Contextual sections remain real routes/panels, but their duplicate
    # tab-strip entries were replaced by links beside the relevant content.
    assert 'data-dashboard-repo-tab="mirrors"' in dashboard_js
    assert '"discussions", "mirrors", ...(canSeeAgentsTab' not in dashboard_js
    assert "data-dashboard-history-button" in dashboard_js
    assert "data-dashboard-releases-button" in dashboard_js
    assert "data-repo-about-sizemap-open" in dashboard_js


def test_dashboard_release_probes_use_the_committed_forkmesh_directory():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert dashboard_js.count('fetchRepoOptionalTree(repo, ".forkmesh/releases")') == 2
    assert '{ path: "releases" }' not in dashboard_js


def test_dashboard_repository_cards_are_clickable_metric_summaries():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    card = dashboard_js[
        dashboard_js.index("function repositoryCard")
        : dashboard_js.index("function updateRepositoryPagination")
    ]

    for marker in (
        'data-dashboard-open-repo="${escapeHtml(key)}"',
        'role="link"',
        "repoActivitySparkline(activityWeeks,",
        "groupRepoMetric(group, [\"issueCount\"",
        "groupRepoMetric(group, [\"commitCount\"",
        "groupRepoMetric(group, [\"pullCount\"",
        "groupRepoMetric(group, [\"discussionCount\"",
    ):
        assert marker in card
    for marker in (
        "backfillVisibleRepoActivity(visible)",
        "fetchJson(repoLiveUrl(origin, \"history\"))",
        "activityWeeksFromCommits(data.commits)",
        "repoActivityFetches",
    ):
        assert marker not in dashboard_js
    assert "groupActivityWeeks(group)" in card
    assert "repoActivitySparkline(activityWeeks, { totalHint: commitTotal })" in card
    assert "data-dashboard-copy" not in card
    assert "Copy clone" not in card


def test_repository_cards_and_profile_rows_match_github_repository_lists():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "function profileRepositoryRow(group)" in dashboard_js
    assert "data-profile-repository-row" in dashboard_js
    assert "data-repo-star-button" in dashboard_js
    assert "data-repo-language-dot" in dashboard_js
    assert "data-repo-activity-sparkline" in dashboard_js
    assert "Find a repository..." in dashboard
    assert "Type" in dashboard
    assert "Language" in dashboard
    assert "Sort" in dashboard


def test_dashboard_can_seed_mock_repositories_for_ui_testing_by_query_param():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "function dashboardMockRepositoriesEnabled()" in dashboard_js
    assert "function dashboardMockRepositories()" in dashboard_js
    assert 'new URLSearchParams(location.search).get("mockRepos")' in dashboard_js
    assert 'owner: "demo-alice"' in dashboard_js
    assert 'name: "mesh-workbench"' in dashboard_js
    assert 'name: "mobile-mirror-client"' in dashboard_js
    assert 'name: "security-review-lab"' in dashboard_js
    assert "if (dashboardMockRepositoriesEnabled())" in dashboard_js
    assert "renderRepositories(dashboardMockRepositories(), state.session);" in dashboard_js
    assert "mockRepos" not in assembled_dashboard()


def test_dashboard_repository_detail_uses_github_like_inner_layout():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    for marker in (
        'data-repo-layout="github-like"',
        "data-repo-about-actions",
        "Go to file",
        "data-repo-commit-summary",
        "data-repo-about",
        "data-repo-live-summary",
        "data-repo-readme",
        "Pull requests",
    ):
        assert marker in render
    assert 'data-repo-github-header class="border-b border-border"' in render
    assert 'data-repo-github-header class="rounded-t-lg border border-border' not in render
    assert 'data-repo-github-header class="border-b border-border">\n          <div class="grid' not in render
    assert 'data-lucide="${icon}"' in dashboard_js
    assert 'function pullStateIconHtml(stateLabel, extra = "")' in dashboard_js


def test_repository_code_page_matches_github_code_layout():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    assert "data-repo-github-header" in render
    assert "data-repo-action-watch" in render
    assert "data-repo-action-fork" in render
    assert "data-repo-action-star" in render
    assert "data-repo-code-sidebar" in render
    assert "data-repo-file-table" in render
    assert "data-repo-about-rail" in render
    assert "Watch" in render
    assert "Fork" in render
    assert "Star" in render
    assert "Add file" in render
    # The Name/Last-commit-message/Last-commit-date column header row was
    # dropped (adhoc #87) so the file table reads as a compact GitHub-style
    # commit line; the summary banner still carries commit + date.
    assert "Last commit date" not in render
    assert "data-repo-commit-date" in render
    assert ">Code<" in render


def test_repository_header_is_compact_and_actions_live_in_about():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    header = _read(PUBLIC / "dashboard" / "partials" / "header.html")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    header_context = dashboard_js[
        dashboard_js.index("function renderHeaderContext")
        : dashboard_js.index("// ---- Public-profile mode")
    ]

    assert "data-repo-header-meta" in header
    assert "data-repo-header-visibility" in header
    assert "data-repo-availability-status" in header
    assert "repoDisplayKey(repo)" in header_context
    assert 'renderHeaderContext("explore");' in render

    tab_band = render[
        render.index("data-repo-github-header")
        : render.index("data-repo-content-grid")
    ]
    assert 'class="border-b border-border"' in tab_band
    assert "rounded-t-lg" not in tab_band
    assert "Repository facts" not in tab_band
    assert "repo.description" not in tab_band
    assert 'role="tab" data-dashboard-repo-tab="mirrors"' not in tab_band

    assert render.index("data-repo-about-description") < render.index("data-repo-about-actions")
    about_actions = render[
        render.index("data-repo-about-actions")
        : render.index("data-repo-about-langs")
    ]
    for marker in (
        "data-repo-action-watch",
        "data-repo-action-fork",
        "data-repo-action-star",
        'data-dashboard-repo-tab="mirrors"',
        'data-repo-about-actions aria-label="Repository details"',
        'data-repo-about-website',
        'data-repo-about-release',
        'data-repo-watch-count class="font-mono"',
        'data-repo-fork-count class="font-mono"',
        'data-repo-star-count class="font-mono"',
        'data-dashboard-repo-count="mirrors" data-dashboard-repo-tab-count="mirrors"',
    ):
        assert marker in about_actions


def test_repository_code_toolbar_carries_fork_and_about_carries_code_map():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    tab_band = render[
        render.index("<div data-repo-github-header")
        : render.index("<div data-repo-content-grid")
    ]
    # The tab bar holds only the tabs; Fork lives by the code.
    assert "data-repo-fork" not in tab_band
    assert "forkSplitButton" not in tab_band

    toolbar = render[
        render.index("<div data-repo-root-toolbar")
        : render.index("<div data-repo-pathbar")
    ]
    focus_actions = render[
        render.index("<div data-repo-focus-actions")
        : render.index("<div data-repo-code-workspace")
    ]
    # Fork | count sits directly left of "Add file", in both toolbars.
    for bar, compact in ((toolbar, "false"), (focus_actions, "true")):
        fork = bar.index("${forkSplitButton(%s)}" % compact)
        assert fork < bar.index("Add file")
        assert "data-repo-codemap-open" not in bar[fork:bar.index("Add file")]
    assert "md:grid-cols-[auto_minmax(0,1fr)_auto_auto_auto]" in toolbar
    # The main toolbar lost Code map to the About box; the file view keeps it,
    # since About is hidden there.
    assert "data-repo-codemap-open" not in toolbar
    assert "data-repo-codemap-open" in focus_actions

    fork_split = render[
        render.index("const forkSplitButton = (compact) =>")
        : render.index("const settingsPanel")
    ]
    assert "websiteForkAvailable" in fork_split
    assert "data-repo-fork-split" in fork_split
    assert "data-repo-fork-create data-repo-fork-go" in fork_split
    assert "data-repo-fork-split-count" in fork_split
    assert "<span data-repo-fork-count>" in fork_split
    # The count keeps its width so the number arriving does not shift the row.
    assert 'style="min-width:2.25rem"' in fork_split
    assert "const websiteForkAvailable = !repo.isPrivate;" in render
    assert 'event.target.closest("[data-repo-fork-split-count]")' in dashboard_js

    about = render[
        render.index("<aside data-repo-about")
        : render.index("<aside data-repo-dir-about")
    ]
    code_map = about.index("data-repo-about-codemap")
    assert about.index("data-repo-about-actions") < code_map
    assert code_map < about.index("data-repo-about-langs")
    assert 'data-repo-codemap-open title="See the whole codebase' in about
    assert "w-full" in about[code_map:about.index("data-repo-about-langs")]
    assert "[data-repo-about-codemap]" in dashboard_js[
        dashboard_js.index("function repoCodeMapNotice")
        : dashboard_js.index("async function openRepoCodeMap")
    ]


def test_repository_logo_moved_from_about_to_settings():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    about = render[
        render.index("<aside data-repo-about")
        : render.index("<aside data-repo-dir-about")
    ]
    assert "data-repo-logo-workflow" not in dashboard_js
    assert "Repository logo" not in about
    assert "data-repo-about-logo" not in dashboard_js
    assert "data-repo-logo-suggestion-form" not in dashboard_js
    # Banner stays in the About form.
    assert "data-repo-about-banner" in about

    settings = render[
        render.index("const settingsPanel = canSeeSettingsTab ?")
        : render.index("detail.innerHTML = ")
    ]
    assert "const canSeeSettingsTab = canEditAbout;" in render
    assert "const canEditAbout = sessionAdministersRepo(repo);" in render
    logo = settings[
        settings.index("data-repo-settings-logo class")
        : settings.index("data-repo-pull-rules-card")
    ]
    for marker in (
        "Repository logo",
        "data-repo-settings-logo-current",
        'nativeRepositoryLogoMarkup(repo, "h-16 w-16")',
        "data-repo-settings-logo-form",
        'data-repo-settings-logo-file type="file" accept="image/png"',
        "data-repo-settings-logo-remove",
        "data-repo-logo-suggestions",
        "data-repo-logo-suggestion-status",
    ):
        assert marker in logo, marker
    assert '${repo.isPrivate ? "" : `<div data-repo-settings-logo' in settings

    loader = dashboard_js[
        dashboard_js.index("async function loadRepoSettingsLogo")
        : dashboard_js.index("async function reviewRepoLogoSuggestion")
    ]
    # The upload is the About endpoint's logoPng, behind its owner/admin check.
    assert "saveRepoSettingsFromWeb(repo, { logoPng })" in loader
    assert "loadRepoLogoSuggestions(repo)" in loader
    assert 'logo?.source !== "owner"' in loader
    assert "void loadRepoSettingsLogo(state.selectedRepo);" in dashboard_js


def test_repository_context_links_and_fediverse_avatars_replace_redundant_ui():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    shell = _read(PUBLIC / "dashboard" / "shell.html")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    follower_loader = dashboard_js[
        dashboard_js.index("function repoFediverseFollowerAvatar")
        : dashboard_js.index("async function loadRepoAboutInfo")
    ]

    tab_band = render[
        render.index("data-repo-github-header")
        : render.index("data-repo-content-grid")
    ]
    for tab in ("commits", "sizemap", "releases"):
        assert f'data-dashboard-repo-tab="{tab}"' not in tab_band

    assert "data-dashboard-history-button" in render
    assert "data-dashboard-releases-button" in render
    assert "data-repo-about-sizemap-open" in render
    assert 'activateRepoTab("releases");' in dashboard_js
    assert 'open.onclick = () => activateRepoTab("sizemap");' in dashboard_js
    assert "data-repo-product-promise" not in render

    assert "data-repo-social-badge" not in render
    assert "followersList" in follower_loader
    assert "follower?.avatarUrl" in follower_loader
    assert 'referrerpolicy="no-referrer"' in follower_loader
    assert 'followerAvatars.classList.toggle("flex", shown.length > 0);' in follower_loader

    assert "--dashboard-header-rgb: 246 248 250;" in dashboard_shell.apply_theme(shell, lambda rel: _read(PUBLIC / rel))
    assert "[data-repo-github-header]" in shell
    assert "background: rgb(var(--dashboard-header-rgb));" in shell


def test_dashboard_about_links_readme_activity_and_owner_edit():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    click_handler = dashboard_js[
        dashboard_js.index('const aboutEditButton = event.target.closest("[data-repo-about-edit]")')
        : dashboard_js.index('const commitButton = event.target.closest("[data-dashboard-commit-hash]")')
    ]
    submit_handler = dashboard_js[
        dashboard_js.index('const aboutForm = event.target.closest("[data-repo-about-form]")')
        : dashboard_js.index('const issueForm = event.target.closest("[data-repo-issue-form]")')
    ]

    for marker in (
        "const canEditAbout = sessionAdministersRepo(repo);",
        "data-repo-about-edit",
        "data-repo-about-form",
        "data-repo-about-description",
        "data-repo-readme-link",
        "data-repo-activity-link",
        "data-repo-insights",
        'href="${escapeHtml(readmeHref)}"',
        'href="${escapeHtml(`${repoLinkUrl(repo)}/insights`)}"',
    ):
        assert marker in render
    for marker in (
        "setRepoAboutEditing(true);",
        "loadRepositoryBlob(state.selectedRepo, readmeLink.dataset.repoReadmePath || \"README.md\")",
        "activateRepoTab(\"insights\");",
    ):
        assert marker in click_handler
    for marker in (
        "saveRepoAboutFromWeb(state.selectedRepo, description, media)",
        "applyRepoAboutDescription(state.selectedRepo, body.description ?? description)",
        "Only the repository owner or an organization owner or admin can edit About.",
    ):
        assert marker in submit_handler


def test_dashboard_about_rail_only_shows_on_code_tab():
    # The About rail only makes sense beside the file tree/README (owner
    # decision 2026-07-12, discussion #2): every other tab — commits,
    # releases, issues, projects, pulls, discussions, insights, mirrors,
    # agents — should go full-width instead of leaving an orphaned rail.
    dashboard_js = _read(PUBLIC / "dashboard.js")
    tab_state = dashboard_js[
        dashboard_js.index("function setRepoTab")
        : dashboard_js.index("function repoPathParts")
    ]

    assert 'const showAbout = tab === "code";' in tab_state
    assert 'contentGrid?.classList.toggle("lg:grid-cols-[minmax(0,1fr)_18rem]", showAbout);' in tab_state
    assert 'contentGrid?.classList.toggle("lg:grid-cols-1", !showAbout);' in tab_state
    assert 'about?.classList.toggle("hidden", !showAbout);' in tab_state


def test_repository_settings_move_out_of_about_and_add_owner_delete():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail(repo)")
        : dashboard_js.index("\n  function renderRepoCodeButton")
    ]
    about_form = render[
        render.index('<form data-repo-about-form')
        : render.index("</form>", render.index('<form data-repo-about-form'))
    ]
    settings_panel = render[
        render.index('data-dashboard-repo-tab-panel="settings"')
        : render.index('detail.innerHTML = `')
    ]

    assert 'settings: { label: "Settings", icon: "settings"' in render
    assert '...(canSeeAgentsTab ? ["agents"] : []), ...(canSeeSettingsTab ? ["settings"] : [])' in render
    assert 'data-lucide="pencil"' in render
    assert 'data-lucide="settings" class="h-3.5 w-3.5"></i></button>' not in render
    assert "data-repo-ap-federate" not in about_form
    assert "data-repo-alert-status-emails" not in about_form
    assert "data-repo-settings-form" in settings_panel
    assert "Daily digest preview" in settings_panel
    assert "data-repo-delete" in settings_panel
    assert "Delete repository" in settings_panel

    assert "function saveRepoSettingsFromWeb(" in dashboard_js
    assert "function deleteRepoFromWeb(" in dashboard_js
    assert 'method: "DELETE"' in dashboard_js
    assert "Type ${expected} to confirm repository deletion." in dashboard_js
    assert 'location.assign("/dashboard/repos")' in dashboard_js


def test_dashboard_repository_nav_is_full_width_while_content_stays_bounded():
    # The tab band spans the viewport, while the repository content keeps the
    # existing readable max width on wide screens.
    for path in (PUBLIC / "dashboard" / "repo.html", VIEWS / "repo.html"):
        dashboard = _read(path)
        explore = dashboard[
            dashboard.index('data-view="explore"')
            : dashboard.index('data-repo-detail')
        ]

        assert 'data-explore-inner class="w-full"' in explore

    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    assert 'class="flex w-full min-w-0 gap-1 overflow-x-auto px-3 sm:px-6"' in render
    assert 'data-repo-content-grid class="mx-auto grid w-full max-w-7xl' in render


def test_dashboard_repository_tabs_keep_border_without_selected_background():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    tab_state = dashboard_js[
        dashboard_js.index("function setRepoTab")
        : dashboard_js.index("function repoPathParts")
    ]

    shell = _read(PUBLIC / "dashboard" / "shell.html")

    # The selected tab is marked the way GitHub marks it: a coral underline
    # drawn from aria-selected, with no filled background. The underline is not
    # green: green is reserved for primary action buttons.
    assert 'aria-selected="${tab === "code" ? "true" : "false"}"' in render
    assert "bg-secondary text-foreground" not in render.split("role=\"tab\"", 1)[1].split("</button>", 1)[0]
    assert '[data-dashboard-repo-tab][role="tab"][aria-selected="true"]::after' in shell
    assert "--dashboard-tab-underline: #f78166;" in shell
    assert 'button.classList.toggle("bg-secondary", active);' not in tab_state
    assert 'button.setAttribute("aria-selected", active ? "true" : "false");' in tab_state


def test_dashboard_repository_tree_uses_monochrome_folder_and_file_icons():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    icon_helper = dashboard_js[
        dashboard_js.index("function fileIconHtml")
        : dashboard_js.index("function formatSize")
    ]
    tree_renderer = dashboard_js[
        dashboard_js.index("function renderRepoTreeRows()")
        : dashboard_js.index("async function loadRepositoryBlob")
    ]

    assert 'fileIconHtml(entry, "h-4 w-4 shrink-0")' in tree_renderer
    assert 'data-lucide="${isTree ? "folder" : "file"}"' in icon_helper
    assert "text-foreground" in icon_helper
    assert "/assets/file-icons/" not in icon_helper


def test_dashboard_repository_metadata_constrains_long_values_without_fake_language_mix():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    assert 'grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3' in render
    assert "flex justify-between gap-3" not in render
    assert "data-repo-live-summary" in render
    # The Data size chip/row and the Repository metadata block were removed in
    # the About-rail cleanup; languages now render from the REAL live file
    # index (loadRepoAboutFilesAndLanguages), never a hardcoded mock mix.
    assert "formatSize(repo.sizeBytes)" not in render
    assert "TypeScript" not in render
    assert "CSS" not in render
    assert "JavaScript" not in render
    assert "grid-cols-[72fr_18fr_10fr]" not in render


def test_dashboard_repository_tabs_read_public_mirror_data_not_owner_inbox():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    for marker in (
        'data-dashboard-repo-tab-panel="commits"',
        "data-dashboard-history-button",
        "data-repo-commits",
        "async function loadRepoCommits(repo)",
        "fetchRepoJson(repoLiveUrl(repo, \"history\"))",
        "function parseFrontMatter(markdown)",
        "async function loadRepoRecordsFromMirror(repo, config)",
        # Issues read the public git tree via loadRepoIssues; pulls and
        # discussions read it lazily through loadRepoCollection on tab view.
        "loadRepoIssues(repo)",
        "loadRepoCollection(state.selectedRepo, tab,",
        'dir: ".forkmesh/issues", file: (number) => `issue-${Number(number)}.json`',
        'dir: ".forkmesh/pulls", file: "pull.md"',
        'dir: ".forkmesh/discussions", file: "discussion.md"',
        "fetchRepoJson(repoLiveUrl(repo, \"tree\", { path: config.dir, ...refParams }))",
        "Object.assign(blobs, await fetchRepoBlobs(",
        "paths.slice(offset, offset + 50), refParams",
        "fetchMissingRepoBlobs(",
        "so new ones are created from the ${desktopBrand()}desktop app.",
    ):
        assert marker in dashboard_js

    assert "fetchJson(`${repoApiBase(repo)}/${kind}`)" not in dashboard_js
    assert "fetchJson(`${repoApiBase(repo)}/commits`)" not in dashboard_js
    assert "New issue" not in render
    assert "New pull request" not in render
    assert "New discussion" not in render


def test_dashboard_repository_commit_rows_use_github_like_hash_copy_controls():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    commits = dashboard_js[
        dashboard_js.index("async function loadRepoCommits")
        : dashboard_js.index("function renderRepoCommitFiles")
    ]
    copy_handler_start = dashboard_js.index('const copyButton = event.target.closest("[data-dashboard-copy]")')
    click_handler = dashboard_js[
        copy_handler_start
        : dashboard_js.index('$("#repoSearch")', copy_handler_start)
    ]

    for marker in (
        "const hash = String(commit.hash || \"\");",
        "const shortHash = hash.slice(0, 7) || \"unknown\";",
        "data-dashboard-commit-row",
        "data-dashboard-commit-hash=\"${escapeHtml(hash)}\"",
        "data-dashboard-copy=\"${escapeHtml(hash)}\"",
        "data-dashboard-copy-kind=\"commit-hash\"",
        "aria-label=\"Copy commit hash ${escapeHtml(shortHash)}\"",
        "data-dashboard-commit-short-hash",
        "font-mono text-xs font-semibold text-foreground",
    ):
        assert marker in commits

    assert 'data-lucide="git-commit-horizontal" class="h-3.5 w-3.5 text-muted-foreground"' in dashboard_js
    assert 'text-primary">${escapeHtml(String(commit.hash || "").slice(0, 7))}</span>' not in commits
    assert 'data-lucide="git-commit-horizontal" class="h-3.5 w-3.5 text-primary"></i>Commits' not in dashboard_js
    assert "const copied = await copyTextToClipboard(text);" in click_handler
    assert "if (copied) {" in click_handler
    assert "copyButton.classList.add(\"copied\");" in click_handler


def test_dashboard_repository_commits_never_leave_an_offline_mirror_spinning():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    commits = dashboard_js[
        dashboard_js.index("const REPO_COMMITS_FETCH_TIMEOUT_MS")
        : dashboard_js.index("function renderRepoCommitFiles")
    ]
    fetcher = dashboard_js[
        dashboard_js.index("async function fetchRepoJson")
        : dashboard_js.index("const PULL_METADATA_BRANCH")
    ]

    assert "REPO_COMMITS_FETCH_TIMEOUT_MS = 30 * 1000" in commits
    assert "container.innerHTML = repoIsLive(repo)" in commits
    assert "REPO_COMMITS_UNAVAILABLE_HTML" in commits
    assert "Commit history is unavailable until a reachable mirror host serves this repository." in commits
    assert "const controller = new AbortController();" in commits
    assert "() => controller.abort()" in commits
    assert "{ signal: controller.signal }" in commits
    assert "window.clearTimeout(timer);" in commits
    assert "async function fetchRepoJson(path, options = {})" in fetcher
    assert "signal: options.signal" in fetcher


def test_dashboard_latest_commit_history_button_opens_commits_tab():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    click_handler_start = dashboard_js.index('const historyButton = event.target.closest("[data-dashboard-history-button]")')
    click_handler = dashboard_js[
        click_handler_start
        : dashboard_js.index('const repoTabButton = event.target.closest("[data-dashboard-repo-tab]")', click_handler_start)
    ]

    for marker in (
        'a href="${escapeHtml(`${repoLinkUrl(repo)}/commits`)}" data-dashboard-history-button',
        'aria-label="Open commit history"',
        'data-lucide="history"',
        "<span>History</span>",
        'data-dashboard-repo-count="commits" data-dashboard-repo-tab-count="commits"',
    ):
        assert marker in render

    assert "event.preventDefault();" in click_handler
    assert 'activateRepoTab("commits");' in click_handler
    assert "return;" in click_handler
    assert 'commits: { label: "Commits"' not in render


def test_dashboard_code_tree_rows_use_live_commit_messages():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    tailwind_css = _read(PUBLIC / "dashboard" / "tailwind.css")
    mirror_gateway = (
        ROOT.parent / "tools" / "mirror_gateway.py"
    ).read_text(encoding="utf-8")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    tree_loader = dashboard_js[
        dashboard_js.index("async function loadRepositoryTree")
        : dashboard_js.index("async function loadRepositoryBlob")
    ]

    for marker in (
        "function updateRepoCommitSummary(commit, repo)",
        "updateRepoCommitSummary(data.latestCommit, repo);",
        'const message = entry.message || entry.commitMessage || entry.subject || "mirrored repository object";',
        "${escapeHtml(message)}",
        'data-repo-tree-commit-message class="hidden w-full min-w-0 truncate text-left',
    ):
        assert marker in dashboard_js
    assert dashboard_js.count(
        "sm:grid-cols-[1.5rem_minmax(9rem,0.8fr)_minmax(0,1fr)_8rem]"
    ) >= 2
    assert (
        "grid-template-columns:1.5rem minmax(9rem,.8fr) "
        "minmax(0,1fr) 8rem"
    ) in tailwind_css

    for marker in (
        "data-repo-commit-avatar",
        "data-repo-commit-author",
        "data-repo-commit-message",
        "data-repo-commit-hash",
        "data-repo-commit-date",
    ):
        assert marker in render

    for marker in (
        "def _commit_summary(self, commit: str, path: str = \"\")",
        '"log",',
            '"--format=%H%x1f%an%x1f%cI%x1f%s"',
        'args += ["--", path]',
        '"message": fields[3]',
        '"commitMessage": fields[3]',
        'entry.update(self._commit_summary(commit, full_path))',
        '"latestCommit": self._commit_summary(commit)',
        '"tree": repository.tree',
    ):
        assert marker in mirror_gateway

    # The commit summary now paints a loading skeleton on first render and is
    # filled by the live-commit updater; the placeholder text survives only as
    # that updater's fallback, never as static render output.
    assert "published latest mirror metadata" not in render
    assert 'subject || "published latest mirror metadata"' in dashboard_js
    assert "animate-pulse rounded bg-muted-foreground/20" in render
    assert 'entry.message || entry.commitMessage || "mirrored repository object"' not in tree_loader


def test_dashboard_repository_issue_and_pull_tabs_match_github_lists():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    for marker in (
        "function renderRepoCollectionPanel(kind, repo, openCount, closedCount)",
        'data-repo-collection-toolbar="pulls"',
        'data-repo-filter-query="pulls"',
        'data-repo-filter-query="issues"',
        'placeholder="Search pull requests by title, body, author, or #number"',
        'data-dashboard-pull-filter="${name}"',
        'data-repo-collection-layout="pulls"',
        'data-repo-collection-layout="issues"',
        "New issue",
        "New pull request",
        "Authored by me",
        '["Author", "author"]',
        '["Labels", "label"]',
        '["Milestones", "milestone"]',
        '["Assignees", "assignee"]',
        'select("Author", "author"',
    ):
        assert marker in dashboard_js

    # The decorative issues left rail was removed with the About rail (adhoc #25).
    assert "data-repo-collection-sidebar" not in dashboard_js
    assert 'renderRepoCollectionPanel("issues", repo, issuesCount, repoCount(repo, ["closedIssues", "closedIssueCount"]))' in render
    assert 'renderRepoCollectionPanel("pulls", repo, pullsCount, repoCount(repo, ["closedPulls", "closedPullCount"]))' in render


def test_web_pull_submission_signs_a_portable_mirror_comparison():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    submit = dashboard_js[
        dashboard_js.index("async function submitWebPullOpen")
        : dashboard_js.index("async function handleDiscussionReplySubmit")
    ]

    for marker in (
        'repoLiveUrl(repo, "compare", {',
        "const patch = String(comparison.patch || \"\");",
        "const commits = String(comparison.commits || \"\");",
        "const content = [title, base, signedHead, patch, commits].join(NUL);",
        "description: cleanBody",
        "creationBaseOid",
        "creationHeadOid",
    ):
        if marker.startswith("creation"):
            assert marker not in submit
        else:
            assert marker in submit
    assert "const patch = \"\";" not in submit
    assert "body: cleanBody" not in submit
    assert 'throw new Error("no_changes")' in submit
    assert "const targetRepo = pullRequestTargetRepo(repo);" in submit
    assert 'fetchRepoMutation(targetRepo, "pulls", payload)' in submit
    assert "head: signedHead" in submit
    assert {"compare", "merge-preview", "branches"} <= _repo_host_read_actions()
    # merge-preview resolves base/head exactly the way compare does, so the two
    # share one query builder rather than growing a second copy that could drift.
    assert 'elif operation in ("compare", "merge-preview"):' in ENTRY_TEXT


def test_dashboard_repository_issue_and_pull_tabs_paginate_records_at_the_bottom():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    record_renderer = dashboard_js[
        dashboard_js.index("function renderRepoRecordList")
        : dashboard_js.index("async function loadRepoCollection")
    ]
    collection_loader = dashboard_js[
        dashboard_js.index("async function loadRepoCollection")
        : dashboard_js.index("async function loadRepoCommits")
    ]
    collection_renderer = dashboard_js[
        dashboard_js.index("function renderRepoCollection(kind)")
        : dashboard_js.index("async function loadRepoCollection")
    ]
    click_handler = dashboard_js[
        dashboard_js.index('const repoCollectionPageButton = event.target.closest("[data-repo-collection-page]")')
        : dashboard_js.index('const copyButton = event.target.closest("[data-dashboard-copy]")')
    ]

    for marker in (
        "REPO_COLLECTION_PAGE_SIZE",
        "repoCollectionPages",
        "function renderRepoCollectionPagination(kind, page, totalPages, totalItems)",
        'data-repo-collection-pagination="${kind}"',
        'data-repo-collection-page="${kind}"',
        'data-repo-collection-page-target="${page - 1}"',
        'data-repo-collection-page-target="${page + 1}"',
        "items.slice(start, end)",
        "Page ${page} of ${totalPages}",
    ):
        assert marker in dashboard_js

    assert "renderRepoRecordList(visible, config, kind)" in collection_renderer
    assert "renderRepoCollection(kind);" in collection_loader
    assert "const page = state.repoCollectionPages[kind] || 1;" in record_renderer
    assert "loadRepoCollection(state.selectedRepo, kind, `[data-repo-${kind}]`);" in click_handler
    # Issue #420: paginating the Issues list must keep the open/closed/all filter
    # (renderRepoIssues) instead of re-rendering the raw, unfiltered record list.
    assert 'if (kind === "issues") renderRepoIssues();' in click_handler
    assert 'else if (kind === "pulls") renderRepoCollection("pulls");' in click_handler
    # Issue #420: 25 records per page (not 5) for issues and pulls.
    assert "const REPO_COLLECTION_PAGE_SIZE = 25;" in dashboard_js


def test_dashboard_repository_record_chips_and_sidebar_links_use_neutral_github_like_colors():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    pagination = dashboard_js[
        dashboard_js.index("function renderRepoCollectionPagination")
        : dashboard_js.index("function renderRepoRecordList")
    ]
    records = dashboard_js[
        dashboard_js.index("function renderRepoRecordList")
        : dashboard_js.index("async function loadRepoCollection")
    ]
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    assert "rounded-md border border-border bg-secondary/60 px-2 py-0.5" in records
    assert "data-repo-record-state" in records
    assert "data-repo-record-state" in dashboard_js[
        dashboard_js.index("function renderRepoRecordDetail")
        : dashboard_js.index("async function loadRepoRecordDetail")
    ]
    assert "rounded-full border border-border px-2 py-0.5 text-[10px] font-mono text-muted-foreground" not in records
    # The current page is GitHub's blue pagination pill, never the green
    # primary action colour.
    assert 'number === page ? "bg-[#1f6feb] font-semibold text-white"' in pagination
    assert 'number === page ? "bg-primary text-primary-foreground border-primary"' not in pagination
    assert 'data-dashboard-open-repo="${escapeHtml(key)}" data-clone-url="${escapeHtml(cloneUrl(origin))}" role="link"' in dashboard_js
    assert "browse-repo-button inline-flex items-center gap-1 text-xs font-medium text-foreground transition-colors" not in dashboard_js
    # The About rail's Clone-availability row was removed with the metadata
    # cleanup; the accent-primary variant must stay gone regardless.
    assert '${live ? "text-primary" : "text-muted-foreground"}">${live ? "available" : "offline"}</dd>' not in render


def test_dashboard_repository_records_open_live_markdown_detail_views():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
            "async function loadRepoRecordDetail(repo, kind, number, preferMirror = false)",
        "function renderRepoRecordDetail(repo, kind, number, parsed)",
        "function recordDetailMeta(kind, values)",
        "data-repo-record-kind",
        "data-repo-record-number",
        "data-repo-record-detail",
        "data-repo-record-back",
        "data-repo-record-body",
        "const recordPath = `${config.dir}/${number}/${recordFile}`;",
        "fetchRepoJson(repoLiveUrl(repo, \"blob\", { path: recordPath, ...refParams }))",
        "loadRepoRecordDetail(state.selectedRepo, kind, number)",
        "loadRepoCollection(state.selectedRepo, kind, `[data-repo-${kind}]`);",
    ):
        assert marker in dashboard_js

    assert "fetchJson(`${repoApiBase(repo)}/issues" not in dashboard_js
    assert "fetchJson(`${repoApiBase(repo)}/pulls" not in dashboard_js
    assert "fetchJson(`${repoApiBase(repo)}/discussions" not in dashboard_js


def test_issue_and_pull_detail_pages_match_github_conversation_layout():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    detail = dashboard_js[
        dashboard_js.index("function renderRepoRecordDetail")
        : dashboard_js.index("async function loadRepoRecordDetail")
    ]

    assert "data-repo-record-hero" in detail
    assert "data-repo-record-conversation" in detail
    assert "data-repo-record-sidebar" in detail
    assert 'data-repo-record-tab="conversation"' in detail
    assert 'data-repo-record-tab="commits"' in detail
    # The Checks tab was once a hardcoded 0 with nothing behind it. It is back
    # only because there is now something behind it: the count and the panel
    # both read the live executor snapshot loaded into parsed.pullChecks, so
    # the tab cannot silently regress to a decorative zero again.
    assert 'data-repo-record-tab="checks"' in detail
    assert 'tabCounter("data-repo-pull-check-count", pullCheckCount)' in detail
    assert "const pullCheckCount = (parsed.pullChecks?.runs || []).length;" in detail
    assert 'data-repo-record-panel="checks"' in detail
    assert "renderRepoPullChecks(parsed.pullChecks)" in detail
    assert 'data-repo-record-tab="files"' in detail
    assert "Reviewers" in detail
    assert "Assignees" in detail
    assert "Milestone" in detail
    assert "Notifications" in detail
    assert "Participants" in detail


def test_dashboard_pull_detail_reads_committed_patch_for_files_changed():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        'const PULL_METADATA_BRANCH = "forkmesh/pulls";',
        "async function resolveRepoPullMetadataCommit(repo)",
        'async function loadRepoPullPatch(repo, number, metadataCommit = "", values = {})',
        "function parsePatchStats(patch)",
        "function renderRepoPullFiles(files, viewed = new Set(), options = {})",
        'function renderRepoPullPatch(patch, viewed = new Set(), mode = "unified")',
        "data-repo-pull-file-list",
        "data-repo-pull-patch",
        ".forkmesh/pulls/${number}/changes.patch",
        "ref: commit,",
        "loadRepoPullPatch(repo, number, pullMetadataCommit, values),",
        "renderRepoPullFiles(files, viewed, { mode: layout.explorer })",
        "renderRepoPullPatch(pullPatch.patch, viewed, effectivePullDiffMode(layout.diff))",
    ):
        assert marker in dashboard_js

    assert ".forkmesh/pulls/${number}/changes.patch" in dashboard_js
    assert 'String(values?.status || "open").toLowerCase() !== "open"' in dashboard_js
    assert "fetchJson(`${repoApiBase(repo)}/pulls" not in dashboard_js


def test_dashboard_pull_timeline_includes_commits_and_live_branch_agents():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    # The conversation is a chronological activity feed, not merely the signed
    # review-comment files. Pull-specific commits are read from the metadata
    # branch, and a branch-matched organization agent contributes safe lifecycle
    # status rows.
    for marker in (
        "function parsePullCommitMbox(mbox)",
        'path: `.forkmesh/pulls/${number}/commits.mbox`, ref: metadataRef,',
        "function buildRepoPullTimeline(values, conversation, commits, agents)",
        "function pullAgentActivityEvent(session, number, headBranch)",
        "async function refreshRepoPullActivity(repo, number, agentsOnly = false)",
        "function scheduleRepoPullActivityRefresh(repo, number, active)",
        "data-repo-pull-timeline-count",
        "data-repo-pull-commit-count",
        "data-repo-pull-commit-phrase",
        "data-repo-pull-commits",
        "repoAgentStatusSnapshot(session)",
    ):
        assert marker in dashboard_js

    refresh = dashboard_js[
        dashboard_js.index("async function refreshRepoPullActivity")
        : dashboard_js.index("async function loadRepoPullConversation")
    ]
    assert "loadRepoPullCommits" in refresh
    assert "loadRepoPullAgentActivity" in refresh
    # A working agent is watched every few seconds; an otherwise idle pull page
    # only re-checks the (worker-side) session list, so it never sits on a
    # fast timer pointed at the mirrors.
    assert "document.hidden ? 15000 : 3000" in refresh
    assert "document.hidden ? 120000 : 30000" in refresh
    assert "refreshRepoPullActivity(repo, number, !active);" in refresh
    # The header no longer hard-codes "1 commit", and both it and the Commits
    # tab follow the branch while an agent pushes to it.
    assert "wants to merge 1 commit into" not in dashboard_js
    assert "renderRepoPullCommits(commits)" in refresh
    assert "pullCommitPhrase(commits.length)" in refresh
    # Only signed-in members can read organization sessions, so a signed-out
    # visitor never starts the watch at all.
    assert 'if (kind === "pulls" && state.session?.sessionToken) {' in dashboard_js


def test_dashboard_pull_commits_follow_the_live_head_branch():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    loader = dashboard_js[
        dashboard_js.index("async function loadRepoPullCommits")
        : dashboard_js.index("function pullAgentStatusIsActive")
    ]

    # An agent keeps pushing to the head branch after the pull was opened, so
    # the branch tip is read first and the immutable creation head is only the
    # fallback. Either way the bounded history is kept only when it still
    # reaches the pull's recorded creation base.
    assert "const branch = pullHeadBranchRef(values);" in loader
    assert 'String(values?.status || "open").toLowerCase() === "open"' in loader
    assert "refs.push(head);" in loader
    assert "loadRepoPullBranchCommits(repo, ref, base)" in loader
    assert 'const base = immutableGitCommit(values?.creationBaseOid);' in loader

    # mbox folds long headers across continuation lines. The unfolding replace
    # has to match a real newline, not the two-character "\\n" escape.
    mbox = dashboard_js[
        dashboard_js.index("function parsePullCommitMbox")
        : dashboard_js.index("function renderRepoPullCommits")
    ]
    assert r'replace(/\n[ \t]+/g, " ")' in mbox
    assert r'replace(/\\n[ \\t]+/g, " ")' not in mbox

    branch_reader = dashboard_js[
        dashboard_js.index("async function loadRepoPullBranchCommits")
        : dashboard_js.index("async function loadRepoPullCommits")
    ]
    assert "baseIndex >= 0 ? history.slice(0, baseIndex) : []" in branch_reader

    # "<contributor>:<branch>" heads name a ref only by their branch segment.
    resolver = dashboard_js[
        dashboard_js.index("function pullHeadBranchRef")
        : dashboard_js.index("async function loadRepoPullBranchCommits")
    ]
    assert 'head.slice(head.lastIndexOf(":") + 1)' in resolver


def test_dashboard_never_loads_a_diff_for_non_open_pulls():
    guard = 'String(values?.status || "open").toLowerCase() !== "open"'
    assert guard in _read(PUBLIC / "dashboard.js")
    assert guard in _read(PUBLIC / "dashboard/js/06-repo-content.js")


def test_dashboard_pull_metadata_reads_pin_the_dedicated_branch_commit():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    resolver = dashboard_js[
        dashboard_js.index("const PULL_METADATA_BRANCH")
        : dashboard_js.index("function isMissingMirrorFolder")
    ]
    records = dashboard_js[
        dashboard_js.index("async function loadRepoRecordsFromMirror")
        : dashboard_js.index("function renderRepoCollectionPagination")
    ]
    detail = dashboard_js[
        dashboard_js.index("async function loadRepoRecordDetail")
        : dashboard_js.index("function setRepoTabCount")
    ]
    patch = dashboard_js[
        dashboard_js.index("async function loadRepoPullPatch")
        : dashboard_js.index("// A pull's conversation")
    ]
    conversation = dashboard_js[
        dashboard_js.index("async function loadRepoPullConversation")
        : dashboard_js.index("// A discussion's replies")
    ]

    assert 'const PULL_METADATA_BRANCH = "forkmesh/pulls";' in resolver
    assert 'repoLiveUrl(repo, "tree", {' in resolver
    assert 'path: ".forkmesh/pulls"' in resolver
    assert "ref: PULL_METADATA_BRANCH" in resolver
    assert (
        "const commit = immutableGitCommit("
        "data?.commit || data?.analysis?.commit);"
    ) in resolver
    assert "`${repoApiBase(repo)}/branches`" not in resolver
    assert 'throw new Error("pull_metadata_unavailable")' in resolver
    assert "if (!repo || repo.isPrivate)" in resolver

    assert "{ ref: await resolveRepoPullMetadataCommit(repo) }" in records
    assert "Object.assign(blobs, await fetchRepoBlobs(" in records
    assert "paths.slice(offset, offset + 50), refParams" in records
    assert "fetchMissingRepoBlobs(" in records
    assert "await resolveRepoPullMetadataCommit(repo)" in detail
    assert "? { ref: pullMetadataCommit }" in detail
    assert "ref: commit," in patch
    assert "readRepoPullFolder(repo, number, commit)" in conversation
    assert "{ ref: commit }" in conversation
    folder = dashboard_js[
        dashboard_js.index("function readRepoPullFolder")
        : dashboard_js.index("async function repoPullFolderLacks")
    ]
    assert "ref: commit," in folder

    # Pull readers must never ask the mirror to guess a mutable/default ref.
    # The single exception is a range query (compare/merge-preview), which
    # resolves both endpoints from explicit base/head and must therefore
    # suppress repoLiveUrl's ambient branch injection rather than pin a ref of
    # its own — an ambient ref there is an unknown parameter the node rejects.
    for source in (records, detail, patch, conversation):
        cursor = 0
        while (found := source.find('ref: ""', cursor)) != -1:
            preceding = source[max(0, found - 200):found]
            assert (
                'repoLiveUrl(repo, "compare"' in preceding
                or 'repoLiveUrl(repo, "merge-preview"' in preceding
            ), f"unpinned ref near: {source[max(0, found - 200):found + 20]}"
            cursor = found + 1


def test_dashboard_reloads_pulls_from_the_latest_metadata_commit():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    activation = dashboard_js[
        dashboard_js.index("function activateRepoTab")
        : dashboard_js.index("function repoPathParts")
    ]
    collection = dashboard_js[
        dashboard_js.index("async function loadRepoCollection")
        : dashboard_js.index("function sessionOwnsRepo")
    ]

    # Pull metadata moves on forkmesh/pulls without changing the selected code
    # branch. Returning to the list must therefore perform another live read,
    # even though the tab's lazy-load marker is already set.
    assert 'else if (tab === "pulls"' in activation
    assert 'state.repoRecordDetail?.kind !== "pulls"' in activation
    assert (
        'loadRepoCollection(state.selectedRepo, "pulls", '
        '"[data-repo-pulls]")'
    ) in activation

    # Every explicit list load must discard the session's immutable-tip cache
    # before resolveRepoPullMetadataCommit() runs. Otherwise Back/Cancel and a
    # tab revisit still resolve the ledger commit observed on the first load.
    assert 'if (kind === "pulls") {' in collection
    assert (
        "delete state.repoPullMetadataCommits["
        "repoPullMetadataCacheKey(repo)];"
    ) in collection
    assert collection.index(
        "delete state.repoPullMetadataCommits["
    ) < collection.index("await loadRepoRecordsFromMirror(repo, config)")


def test_dashboard_reports_an_empty_collection_as_empty_not_as_an_outage():
    # adhoc #1723: every repository read routes through healthy mirrors, so
    # "the folder does not exist" and "no mirror answered" arrive on the same
    # code path. The site printed the outage copy ("unavailable until a
    # reachable mirror host serves the .forkmesh/pulls/ folder") while the
    # mirrors were up and simply had nothing to serve.
    dashboard_js = _read(PUBLIC / "dashboard.js")
    missing = dashboard_js[
        dashboard_js.index("function isMissingMirrorFolder")
        : dashboard_js.index("const repoCollectionConfig")
    ]
    records = dashboard_js[
        dashboard_js.index("async function loadRepoRecordsFromMirror")
        : dashboard_js.index("function renderRepoCollectionPagination")
    ]
    # The Worker collapses to 404 not_found only once every endpoint it reached
    # agreed; a 503 mirror_unavailable is still a real outage and must not be
    # read as an empty collection.
    assert 'message.includes("not_found")' in missing
    assert 'message.includes("_absent")' in missing
    assert "mirror_unavailable" not in missing
    # An absent pull ledger branch is zero pull requests, not an outage; a
    # failed branch listing still propagates.
    assert 'throw new Error("pull_metadata_absent")' in dashboard_js
    assert "if (isMissingMirrorFolder(error)) return [];" in records


def test_dashboard_commit_history_opens_live_commit_detail_not_inbox_route():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        "async function loadRepoCommitDetail(repo, hash)",
        "function renderRepoCommitDetail(repo, data)",
        "function renderRepoCommitDiff(diff, imageDiffs)",
        "function renderRepoCommitFiles(files)",
        "fetchRepoJson(repoLiveUrl(repo, \"commit\", { path: hash }))",
        "data-repo-commit-detail",
        "data-repo-commit-back",
        "data-repo-commit-files",
        "data-repo-commit-diff",
        "renderRepoCommitDiff(data.diff, data.imageDiffs)",
        "const commitHash = commitButton.dataset.dashboardCommitHash || \"\";",
        "loadRepoCommitDetail(state.selectedRepo, commitHash);",
    ):
        assert marker in dashboard_js

    assert "data-repo-commit-truncated" not in dashboard_js
    assert "MAX_DIFF_LINES" not in dashboard_js
    assert "Diff truncated for display" not in dashboard_js
    assert "fetchJson(`${repoApiBase(repo)}/commits" not in dashboard_js


def test_dashboard_repository_tab_counts_wait_for_live_data_and_update_mirrors():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    repo_count = dashboard_js[
        dashboard_js.index("function repoCount")
        : dashboard_js.index("function repoDefaultBranch")
    ]
    mirror_loader = dashboard_js[
        dashboard_js.index("async function loadRepoMirrors")
        : dashboard_js.index("function loadRepoFeaturePanels")
    ]

    assert "function tabCountLabel(value)" in dashboard_js
    assert "return null;" in repo_count
    assert "tabCountLabel(meta.count)" in render
    # Tab badges carry the -tab-count hook so live counts (open issues, served
    # pulls/discussions, mirrors) update in place without a full re-render.
    assert 'data-dashboard-repo-tab-count="${tab}"' in render
    assert 'data-dashboard-repo-count="mirrors"' in render
    assert "updateRepoLiveCounts(repo, data.counts);" in dashboard_js
    assert "updateRepoLiveCounts(repo, { mirrors: mirrorCount });" in mirror_loader
    for fake_action in ("Watch <span", "Fork <span", "Star <span"):
        assert fake_action not in render


def test_dashboard_formats_catalog_millisecond_timestamps():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    formatter = dashboard_js[
        dashboard_js.index("function parseFlexibleDate")
        : dashboard_js.index("function formatSize")
    ]

    assert "const numeric = Number(value);" in formatter
    assert "1000000000000" in formatter
    assert "new Date(numeric" in formatter
    assert "function formatDate(value) {" in formatter
    assert "function formatTimeAgo(value) {" in formatter


def test_worker_routes_public_history_through_direct_https():
    route = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _route")
        : ENTRY_TEXT.index("async def _git_host")
    ]
    assert "host_match = REPO_HOST_RE.match" in route
    assert REPO_HOST_ROUTE_RE in URLS_TEXT
    assert "return await _https_mirror_proxy(" in route


def test_worker_dropped_commit_comment_inbox_route():
    # Commit commenting was removed; the /commits inbox endpoint is gone and
    # public history still routes through the direct HTTPS mirror proxy.
    assert "REPO_COMMITS_RE" not in URLS_TEXT
    assert "commits_handler" not in ENTRY_TEXT
    assert REPO_HOST_ROUTE_RE in URLS_TEXT
    assert REPO_DIRECT_BROWSE_GATE in ENTRY_TEXT
    assert {"history", "commit"} <= _repo_host_read_actions()
    assert "return await _https_mirror_proxy(" in ENTRY_TEXT


def test_worker_routes_repo_about_catalog_update_for_source_owner():
    assert 'REPO_ABOUT_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/about$")' in URLS_TEXT
    assert "async def repo_about_handler(env, request, owner, repo):" in ENTRY_TEXT
    assert "about_match = REPO_ABOUT_RE.match(url.path)" in ENTRY_TEXT
    assert "return await repo_about_handler(self.env, request, owner, repo)" in ENTRY_TEXT
    assert "_catalog_record_matches_identity(record, owner, repo)" in ENTRY_TEXT
    assert 'record["description"] = description' in ENTRY_TEXT
    assert "await purge_catalog_related_caches()" in ENTRY_TEXT[
        ENTRY_TEXT.index("async def repo_about_handler")
        : ENTRY_TEXT.index("async def repo_mirrors_handler")
    ]


def test_dashboard_repository_go_to_file_and_add_file_controls_are_present():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    assert "Add file" in render
    for marker in (
        "data-repo-file-finder-open",
        "data-repo-file-finder",
        "data-repo-file-finder-input",
        "data-repo-file-finder-results",
        "function openRepoFileFinder(trigger)",
        "function closeRepoFileFinder({ restoreFocus = true } = {})",
        "async function buildRepoFileIndex(repo)",
        "function renderRepoFileFinderResults(query = \"\")",
        "function moveRepoFileFinderSelection(delta)",
        "MAX_REPO_FILE_FINDER_RESULTS",
        "while (queue.length && files.length < MAX_REPO_FILE_FINDER_RESULTS",
        "fetchRepoJson(repoLiveUrl(repo, \"tree\", { path }))",
        "data-repo-file-finder-result",
        "loadRepositoryBlob(state.selectedRepo, selected.dataset.repoFileFinderPath || \"\")",
        "event.key.toLowerCase() === \"t\"",
        "openRepoFileFinder(fileFinderOpenButton);",
        "openRepoFileFinder(event.target);",
        "event.key === \"Escape\"",
        "closeRepoFileFinder();",
        "event.key === \"Enter\"",
    ):
        assert marker in dashboard_js


def test_dashboard_repository_branch_button_lists_live_remote_branches():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    click_handler_start = dashboard_js.index('const branchButton = event.target.closest("[data-repo-branch-button]")')
    click_handler = dashboard_js[
        click_handler_start
        : dashboard_js.index('const fileFinderOpenButton = event.target.closest("[data-repo-file-finder-open]")', click_handler_start)
    ]

    for marker in (
        "function repoBranchList(repo)",
        "function repoBranchCount(repo)",
        "function repoBranchQuery(repo)",
        "function setRepoBranchQuery(repo, query)",
        "function renderRepoBranchButton(branch)",
        "function renderRepoBranchSummary(repo)",
        "function renderRepoBranchToolbar(repo, branch)",
        "function renderRepoBranchMenu(repo, branches, open)",
        "async function toggleRepoBranchMenu(repo, button)",
        "fetchRepoJson(`${repoApiBase(repo)}/branches`)",
        "filterRepoBranches(repo, branches)",
        "branches.map((branch) =>",
        "data-repo-branch-button",
        "data-repo-branch-summary",
        "data-repo-branch-count",
        "data-repo-branch-menu",
        "data-repo-branch-close",
        "data-repo-branch-search",
        "data-repo-branch-name",
        "data-repo-branch-current",
        "data-repo-branch-default",
        "Switch branches",
        "Find a branch...",
        "View all branches",
        "aria-haspopup=\"menu\"",
        "aria-expanded=\"false\"",
        "role=\"menu\"",
        "role=\"menuitemradio\"",
    ):
        assert marker in dashboard_js

    assert "renderRepoBranchToolbar(repo, branch)" in render
    assert "toggleRepoBranchMenu(state.selectedRepo, branchButton);" in click_handler


def test_dashboard_repository_branches_page_lists_every_branch():
    # adhoc #2095: the "N Branches" count beside the branch picker used to be
    # an inert <span>, so the website was the one client with no way to see
    # every branch of a repository. It is now a link to /owner/repo/branches
    # (a real, refreshable route), and so is the dropdown's "View all
    # branches" footer.
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]

    assert "branches" in static_routes.REPO_TAB_ROUTES
    assert static_routes.looks_like_repo_route("/owner/repo/branches")

    # The page is a tab panel like Commits, reachable from the toolbar count.
    assert 'data-dashboard-repo-tab-panel="branches"' in render
    assert "data-repo-branches-total" in render
    assert "data-repo-branches-search" in render
    assert "<div data-repo-branches></div>" in render
    assert (
        'data-repo-branch-summary data-repo-branches-open' in dashboard_js
    )
    assert (
        'event.target.closest("[data-repo-branches-open], [data-repo-branch-view-all]")'
        in dashboard_js
    )
    assert 'activateRepoTab("branches");' in dashboard_js

    for marker in (
        "async function fetchRepoBranches(repo, { refresh = false } = {})",
        "async function loadRepoBranches(repo, { refresh = false } = {})",
        "function renderRepoBranchesList(repo)",
        "function renderRepoBranchesRow(repo, branch, current, defaultBranch)",
        "function repoBranchesSorted(repo, branches)",
        "function filterRepoBranchesPage(repo, branches)",
        "function repoBranchesQuery(repo)",
        "function setRepoBranchesQuery(repo, query)",
        'if (tab === "branches") loadRepoBranches(state.selectedRepo);',
        'event.target?.matches?.("[data-repo-branches-search]")',
    ):
        assert marker in dashboard_js

    # The dropdown and the page share one listing, and every row carries the
    # tip commit the mirror already reports.
    assert "fetchRepoBranches(repo);" in dashboard_js
    assert "message: branch.message || branch.subject" in dashboard_js
    assert "author: branch.author || branch.authorName" in dashboard_js

    # Before the live listing lands, the count is the catalog's published
    # branchCount rather than a hardcoded-looking "1 Branch".
    assert 'const published = repoCount(repo, ["branchCount"]);' in dashboard_js


def test_dashboard_repository_branch_selection_drives_live_mirror_requests():
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        "function repoLiveUrl(repo, action, params = {})",
        'query.set("ref", repoSelectedBranch(repo));',
        'return `${repoApiBase(repo)}/${action}?${query.toString()}`;',
        "fetchRepoJson(repoLiveUrl(repo, \"tree\", { path }))",
        "fetchRepoJson(repoLiveUrl(repo, \"blob\", { path }))",
        "fetchRepoJson(repoLiveUrl(repo, \"tree\", { path: config.dir, ...refParams }))",
        "fetchRepoJson(repoLiveUrl(repo, \"blob\", { path: recordPath, ...refParams }))",
        "fetchRepoJson(repoLiveUrl(repo, \"history\"))",
        "fetchRepoJson(repoLiveUrl(repo, \"commit\", { path: hash }))",
        "resetRepoFileFinder(state.selectedRepo);",
        "loadRepositoryTree(state.selectedRepo, \"\");",
        "loadRepoFeaturePanels(state.selectedRepo);",
        'event.target?.matches?.("[data-repo-branch-search]")',
        "setRepoBranchQuery(state.selectedRepo, event.target.value || \"\");",
        "updateRepoBranchControls(state.selectedRepo, event.target.closest(\"[data-repo-branch-control]\"));",
    ):
        assert marker in dashboard_js


def test_dashboard_repository_code_explorer_switches_between_tree_and_file_review():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    tree_loader = dashboard_js[
        dashboard_js.index("async function loadRepositoryTree")
        : dashboard_js.index("async function loadRepositoryBlob")
    ]
    blob_loader = dashboard_js[
        dashboard_js.index("async function loadRepositoryBlob")
        : dashboard_js.index("async function loadRepoCollection")
    ]

    for marker in (
        "data-repo-code-workspace",
        "data-repo-code-explorer",
        "data-repo-explorer-tree",
        "data-repo-code-main",
        "data-repo-tree-panel",
        "data-repo-root-toolbar",
        "data-repo-focus-actions",
        "data-repo-content-grid",
    ):
        assert marker in render

    assert "function renderRepoExplorer(repo, path, entries)" in dashboard_js
    assert "function setRepoExplorerFocusMode(active)" in dashboard_js
    assert 'data-repo-code-explorer class="hidden' in render
    assert 'treePanel?.classList.remove("hidden");' in tree_loader
    assert 'viewer?.classList.add("hidden");' in tree_loader
    assert 'readmePanel?.classList.toggle("hidden", Boolean(path));' in tree_loader
    assert "setRepoExplorerFocusMode(Boolean(path));" in tree_loader
    assert 'treePanel?.classList.add("hidden");' in blob_loader
    assert 'readmePanel?.classList.add("hidden");' in blob_loader
    assert "setRepoExplorerFocusMode(repoPathParts(path).length > 1);" in blob_loader
    assert 'data-repo-file-toolbar' in blob_loader
    assert "Blame" in blob_loader
    assert "Raw" in blob_loader
    assert "function highlightCodeLine(line, path)" in dashboard_js
    assert "highlightCodeLine(line, path)" in dashboard_js[
        dashboard_js.index("function renderRepoFileRows")
        : dashboard_js.index("function setRepoFileMode")
    ]
    file_rows = dashboard_js[
        dashboard_js.index("function renderRepoFileRows")
        : dashboard_js.index("function setRepoFileMode")
    ]
    assert "border-r border-border/70" not in file_rows
    assert "text-sky-300" in dashboard_js
    assert "text-emerald-300" in dashboard_js
    assert "text-amber-300" in dashboard_js


def test_dashboard_repository_file_toolbar_actions_are_wired_without_local_drafts():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    blob_loader = dashboard_js[
        dashboard_js.index("async function loadRepositoryBlob")
        : dashboard_js.index("async function loadRepoCollection")
    ]

    for marker in (
        'data-repo-file-mode="code"',
        'data-repo-file-mode="blame"',
        'data-repo-file-action="raw"',
        'data-repo-file-action="copy"',
        'data-repo-file-action="download"',
        'data-repo-file-action="fullscreen"',
        'data-repo-file-action="edit"',
        "data-repo-file-editor",
        "repoFileEditRequest(repo, {",
        "${repoApiBase(repo)}/file-edit",
        "bindRepoFileToolbar(viewer, repo, path, content, lines",
        "function setRepoFileMode(viewer, mode, lines, path, meta)",
        "function bindRepoFileToolbar(viewer, repo, path, content, lines, meta)",
        "function copyTextToClipboard(text)",
        "return true;",
        "return false;",
        "function downloadRepoFile(path, content)",
        "window.setTimeout(() => URL.revokeObjectURL(url), 30000);",
        "function toggleRepoFileFullscreen(viewer)",
    ):
        assert marker in dashboard_js

    # Markdown files open on their rendered preview; everything else on code.
    assert "setRepoFileMode(viewer, markdownPreview ? \"preview\" : \"code\", lines, path, meta);" in blob_loader
    assert 'aria-label="Copy file contents"' in blob_loader
    assert "const copied = await copyTextToClipboard(content);" in dashboard_js
    assert 'updateRepoFileStatus(viewer, "Could not copy file contents.", "bad");' in dashboard_js
    assert "renderRepoFileRows(lines, path, mode, meta)" in dashboard_js

    for marker in (
        'aria-label="Edit local draft"',
        'data-repo-file-action="copy-draft"',
        'data-repo-file-action="download-draft"',
        "Local draft only",
    ):
        assert marker not in dashboard_js


def test_dashboard_repository_blob_viewer_previews_media_csv_pdf_and_binary():
    dashboard_js = _read(PUBLIC / "dashboard.js")
    blob_loader = dashboard_js[
        dashboard_js.index("async function loadRepositoryBlob")
        : dashboard_js.index("async function loadRepoCollection")
    ]

    for marker in (
        "function repoPreviewKindFromPath(path)",
        "function repoRawUrl(repo, path)",
        "function repoPreviewMeta(data, repo, path)",
        "function renderRepoMediaPreview(meta)",
        "function renderRepoCsvPreview(meta)",
        "function renderRepoUnsupportedBinary(meta)",
        "function renderRepoPreview(viewer, repo, path, data)",
        'data-repo-media-preview',
        'data-repo-csv-preview',
        'data-repo-binary-preview',
        "<video",
        "<audio",
        "<iframe",
        'href="${escapeHtml(meta.rawUrl)}"',
        'image/avif',
        'image/svg+xml',
        'video/mp4',
        'video/quicktime',
        'video/webm',
        'audio/mpeg',
        'audio/flac',
        'application/pdf',
        'text/csv',
        'text/tab-separated-values',
    ):
        assert marker in dashboard_js

    assert "if (renderRepoPreview(viewer, repo, path, data)) return;" in blob_loader
    # Markdown files open on their rendered preview; everything else on code.
    assert "setRepoFileMode(viewer, markdownPreview ? \"preview\" : \"code\", lines, path, meta);" in blob_loader


def test_worker_routes_raw_repository_blobs_through_direct_https_gateway():
    gateway = (
        ROOT.parent / "tools" / "mirror_gateway.py"
    ).read_text(encoding="utf-8")
    assert REPO_HOST_ROUTE_RE in URLS_TEXT
    assert REPO_DIRECT_BROWSE_GATE in ENTRY_TEXT
    assert {"blob", "raw"} <= _repo_host_read_actions()
    assert "return await _https_mirror_proxy(" in ENTRY_TEXT
    assert "upstream.body," in ENTRY_TEXT
    assert 'response_headers["X-ForkMesh-Served-By"] = endpoint["node"]' in ENTRY_TEXT
    assert "The private endpoint origin remains masked" in ENTRY_TEXT
    # The node gateway streams `git cat-file blob` directly to the masked edge
    # response. It does not base64-encode/reassemble media in a Durable Object.
    assert 'if operation == "raw":' in gateway
    assert "stream = repository.raw_spec(query)" in gateway
    assert "stream=stream" in gateway
    assert 'kind="process"' in gateway
    assert '["cat-file", "blob", object_name]' in gateway
    assert 'if guessed_type in active_types:' in gateway
    assert 'disposition = "attachment"' in gateway


def test_direct_https_gateway_routes_live_repository_branches():
    gateway = (
        ROOT.parent / "tools" / "mirror_gateway.py"
    ).read_text(encoding="utf-8")
    qt_mirrors = (
        ROOT.parent / "desktop" / "src" / "MainWindowReleases.cpp"
    ).read_text(encoding="utf-8")

    for marker in (
        '"tree": repository.tree',
        '"blob": repository.blob',
        '"history": repository.history',
        '"commit": repository.commit',
        '"branches": repository.branches',
        '"stats": repository.stats',
        '"sizes": repository.sizes',
        '"tree": frozenset({"path", "ref"})',
        # The Calendar reads every branch and tag over a time range, so the
        # history route accepts that scope alongside a single ref.
        '"history": frozenset({"ref", "all", "since", "until", "path"})',
    ):
        assert marker in gateway
    assert REPO_HOST_ROUTE_RE in URLS_TEXT
    assert REPO_DIRECT_BROWSE_GATE in ENTRY_TEXT

    # The compatibility RepoHost was removed altogether. The ordinary desktop
    # reads signed health and branch state from each saved Host directly.
    assert not (ROOT.parent / "desktop" / "src" / "RepoHost.cpp").exists()
    assert not (ROOT.parent / "desktop" / "src" / "RepoHost.h").exists()
    assert 'QStringLiteral("/health")' in qt_mirrors
    assert 'QStringLiteral("/v1/public/repositories/%1/%2/branches")' in qt_mirrors


def test_direct_gateway_streams_raw_repository_blobs_without_json_base64_cap():
    gateway = (
        ROOT.parent / "tools" / "mirror_gateway.py"
    ).read_text(encoding="utf-8")

    for marker in (
        "def raw_spec(self, query:",
        '["cat-file", "-s", object_name]',
        '["cat-file", "blob", object_name]',
        "content_length=size",
        'kind="process"',
        "process = subprocess.Popen(",
        "selector = selectors.DefaultSelector()",
        "chunk = os.read(process.stdout.fileno(), 256 * 1024)",
    ):
        assert marker in gateway


def test_dashboard_network_chat_uses_real_room_integration_without_mock_messages():
    dashboard = _read(PUBLIC / "dashboard" / "chat" / "index.html")
    chat_js = _read(PUBLIC / "dashboard-chat.js")
    visible = _strip_html_comments(dashboard)

    assert 'src="/dashboard-chat.js?v=${version}"' in dashboard
    assert "CHAT_WS_PATH =\n" in chat_js
    assert "`/api/repo/${encodeURIComponent(ROOM_OWNER)}`" in chat_js
    assert (
        "`/${encodeURIComponent(ROOM_REPO)}/rooms/"
        "${encodeURIComponent(ACTIVE_ROOM)}/ws`"
    ) in chat_js
    # The room key is fetched from the relay (server-derived from DATA_KEY), not a
    # public baked-in constant.
    assert 'forkmesh-shared-room-key-v1' not in chat_js
    assert "`/api/chat/room-key?owner=${encodeURIComponent(ROOM_OWNER)}`" in chat_js
    assert "`&repo=${encodeURIComponent(ROOM_REPO)}`" in chat_js
    assert "`&room=${encodeURIComponent(ACTIVE_ROOM)}`" in chat_js
    assert 'PUBLIC_WORLD_GENERAL_ROOM = "world-general"' in chat_js
    assert "fetchRoomPassphrase" in chat_js
    assert "deriveRoomKey" in chat_js
    assert "encryptObject" in chat_js
    assert "decryptObject" in chat_js
    assert "WebSocket" in chat_js
    assert "Chat mock data removed" not in dashboard
    assert "const fullMessages" not in dashboard
    assert "const sideMessages" not in dashboard
    assert "notificationToggle" in visible
    assert "notificationModal" in visible
    assert "data-notification-count" in visible
    for mock in (
        "alice-node",
        "relay-eu-1",
        "devbox-kr",
        "sysop-eu",
        "morning - anyone else seeing",
        "Cloudflare had a blip",
    ):
        assert mock not in visible
        assert mock not in chat_js


def test_dashboard_deep_link_assets_binding_is_wired_up():
    # entry.py's /dashboard/* fallback (run_worker_first) calls
    # self.env.ASSETS.fetch(...) to serve the prebuilt SPA shell for deep links
    # like /dashboard/owner/repo. Without an explicit binding name, env.ASSETS
    # is undefined and that call throws, 500ing every such request.
    assert WRANGLER["assets"].get("binding") == "ASSETS"
    assert 'self.env.ASSETS.fetch(' in ENTRY_TEXT


def test_clean_marketing_routes_target_static_pages():
    for redirect in (
        "/docs /docs/ 308",
        "/docs.html /docs/ 308",
        "/blog/ /blog 308",
        "/blogs /blog 308",
    ):
        assert redirect in REDIRECTS
    for rewrite in (
        "/desktop /desktop.html 200",
        "/blog /blog.html 200",
        "/pricing /pricing.html 200",
    ):
        assert rewrite not in REDIRECTS

    run_worker_first = WRANGLER["assets"]["run_worker_first"]
    for route in ("/dashboard.js",):
        assert route not in run_worker_first
    for route in ("/blog", "/docs", "/desktop", "/network"):
        assert route in run_worker_first
        assert '"%s",' % route.lstrip("/") in STATIC_ROUTES_TEXT
    assert "/*.html" in run_worker_first
    assert (PUBLIC / "network.html").is_file()
    for route, asset in (
        ("/dashboard", "dashboard/index.html"),
        ("/dashboard/repos", "dashboard/repos/index.html"),
        ("/dashboard/network", "dashboard/network/index.html"),
        ("/dashboard/chat", "dashboard/chat/index.html"),
        ("/dashboard/settings", "dashboard/settings/index.html"),
        ("/dashboard/profile", "dashboard/profile/index.html"),
        ("/dashboard/profile/repositories", "dashboard/profile/repositories/index.html"),
    ):
        assert f'"{route}": "{asset}"' in STATIC_ROUTES_TEXT
    assert "/" in run_worker_first
    assert "/dashboard" not in run_worker_first
    assert "!/dashboard" in run_worker_first
    assert "/dashboard/*" in run_worker_first
    assert WRANGLER["assets"]["html_handling"] == "drop-trailing-slash"
    for page in ("repos", "network", "chat", "settings", "profile", "profile/repositories"):
        assert ("!/dashboard/" + page) in run_worker_first
    assert "/dashboard.html" not in run_worker_first


def test_repo_shortcut_is_not_a_redirects_rule_so_assets_are_not_hijacked():
    # A /:owner/:repo rule in _redirects matches real two-segment static assets
    # (e.g. /assets/logo.png, /favicon/site.webmanifest) because Cloudflare always
    # applies _redirects before serving a matching static file - that 308'd those
    # assets and broke the deploy's public-asset check. The shortcut must be
    # Worker-owned with explicit asset-prefix exceptions, so guard against the
    # redirect rule's return.
    rules = [
        line for line in REDIRECTS.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    for rule in rules:
        assert ":owner" not in rule
        assert ":repo" not in rule
        assert "/dashboard?repo=" not in rule
        # Self-referential 200 rewrites for asset dirs are no-ops (a rewrite to the
        # same path); real files serve directly once nothing else hijacks them.
        assert not rule.startswith("/assets/*")
        assert not rule.startswith("/favicon/*")


def test_repo_shortcut_urls_are_worker_owned_not_404_page_scripted():
    html = _read(WWW_PUBLIC / "404.html")

    assert "window.location.replace" not in html
    assert "window.location.pathname.split" not in html
    assert 'var dashboardPath = "/dashboard/' not in html
    assert "looks_like_repo_route" in ENTRY_TEXT
    assert 'if looks_like_repo_route(url.path):' in ENTRY_TEXT


def test_dashboard_restores_feature_tab_on_hard_refresh():
    # Regression test: a page load (not just a client-side tab click or a
    # Back/Forward step) at a feature-tab URL like /owner/repo/issues must
    # restore that tab instead of silently falling back to Code. This was
    # broken because REPO_TAB_ROUTES/repoRouteParts() were referenced by the
    # popstate handler but never defined, and the initial-render path
    # (renderRepoDetail, run from init() on a hard load) never consulted the
    # URL's tab segment at all.
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "function repoRouteParts()" in dashboard_js
    assert 'const REPO_TAB_ROUTES = ["branches", "commits", "insights", "sizemap", "releases", "issues", "projects", "pulls", "discussions", "mirrors", "actions", "security"];' in dashboard_js

    render_start = dashboard_js.index("function renderRepoDetail(repo)")
    render_body = dashboard_js[render_start:dashboard_js.index("\n  function findRepository(key)")]
    assert "const routeParts = repoRouteParts();" in render_body
    # adhoc #182: the owner-only Agents tab is only ever a recognized route for
    # the account that can see it, so tab-route membership goes through
    # repoTabRoutesFor(repo) (REPO_TAB_ROUTES + "agents" when owner/admin)
    # instead of the bare REPO_TAB_ROUTES constant.
    # adhoc #61: the restored tab is computed once (initialTab) so the Code
    # tree warm-up below it can be gated to background mode when it isn't Code.
    assert 'const initialTab = compareRoute\n      ? "pulls"\n      : repoTabRoutesFor(repo).includes(routeKind) ? routeKind : "code";' in render_body
    assert "setRepoTab(initialTab);" in render_body


def test_dashboard_hard_refresh_preserves_tab_through_404_bounce():
    # Regression test: a hard refresh on /owner/repo/issues doesn't hit a
    # static asset, so Cloudflare serves 404.html, which bounces the browser
    # to /dashboard/owner/repo/issues. renderRepoDetail used to normalize that
    # back to a bare /owner/repo URL (it only checked whether the current path
    # *started with* the bare repo path, which is never true for the
    # /dashboard/-prefixed bounce path), silently discarding the /issues
    # segment before the tab-restore logic ever read it back out of the URL.
    dashboard_js = _read(PUBLIC / "dashboard.js")

    render_start = dashboard_js.index("function renderRepoDetail(repo)")
    render_body = dashboard_js[render_start:dashboard_js.index("\n  function findRepository(key)")]

    assert "routeMatchesRepo" in render_body
    assert 'repoTabRoutesFor(repo).includes(routeKind)\n          ? `${repoLinkUrl(repo)}/${routeKind}`' in render_body
    assert "navigateHistory(detailPath);" in render_body
    # The old bare-collapse call must be gone.
    assert "const detailPath = repoLinkUrl(repo);" not in render_body


def test_desktop_client_install_page_exists():
    html = _read(WWW_PUBLIC / "desktop.html")

    assert "<title>Install ForkMesh Desktop" in html
    # The install page owns the platform-specific sections; homepage CTAs point
    # to the page rather than duplicating per-OS buttons.
    for anchor in ('id="macos"', 'id="windows"', 'id="linux"'):
        assert anchor in html
    assert "curl -fsSL https://forkmesh.com/install.sh | bash" in html
    for installer in (
        "forkmesh-windows-x86_64-setup.exe",
        "forkmesh-macos-arm64.dmg",
        "forkmesh-macos-x86_64.dmg",
        "forkmesh-linux-x86_64",
        "forkmesh-linux-x86_64.AppDir.tar.gz",
        "forkmesh-linux-x86_64.AppImage",
        "forkmesh-linux-arm64.AppImage",
    ):
        assert installer in html
    assert "Not published in ${manifest.tag}" in html
    assert "enables only packages" in html
    assert "crypto.subtle.verify" in html
    assert "wsl --install" not in html
    assert "builds the Qt client from source" not in html


def test_homepage_links_to_installer_and_signed_direct_downloads():
    index = _read(WWW_PUBLIC / "index.html")
    downloads = _read(WWW_PUBLIC / "homepage-downloads.js")

    assert 'href="/desktop"' in index
    assert 'id="install-os-buttons"' not in index
    for href in ("/desktop#macos", "/desktop#windows", "/desktop#linux"):
        assert href not in index
    assert '<section id="downloads"' in index
    assert 'data-home-download-list' in index
    assert '/homepage-downloads.js?v=${version}' in index
    assert '"homepage-downloads.js",' in _read(ROOT / "src" / "dashboard_shell.py")
    assert 'crypto.subtle.verify("Ed25519"' in downloads
    assert "/releases/blob/sha256/" in downloads
    assert "${asset.blob_sha256}" in downloads
    assert "forkmesh-android-universal.apk" in downloads
    assert "forkmesh-linux-x86_64.AppImage" in downloads
    assert "forkmesh-windows-x86_64-setup.exe" in downloads
    assert 'const apiOrigin = "https://app.forkmesh.com"' in downloads
    assert '"Windows desktop", "x86_64"' in downloads
    assert "forkmesh-macos-arm64.dmg" in downloads
    assert '"macOS desktop", "Apple silicon"' in downloads
    assert '["forkmesh-macos-x86_64.dmg", ["macOS desktop", "Intel"]]' in downloads
    assert "assetsByName.get(assetName)" in downloads
    assert "/^[0-9a-f]{64}$/" in downloads
    assert "Number(asset.size) <= 0" in downloads
    # Each card names the release it belongs to, and the status names the
    # newest signed release even while `latest` is held back behind it.
    assert "downloadLink(asset, label, manifest.tag)" in downloads
    assert 'version.className = "home-download-version"' in downloads
    assert "#downloads .home-download-version" in index
    assert 'encodeURIComponent("desktop/CMakeLists.txt")' in downloads
    assert "verifiedManifest(`candidate-${match[1]}`)" in downloads
    assert "candidate.tag === `v${match[1]}`" in downloads
    assert "`Latest version ${newest}`" in downloads


def test_homepage_node_selector_band_is_gone_and_network_stays_linked():
    # The interactive node-selector band -- the one that needed `hidden lg:block`
    # to keep its beam and hairline bars off small screens -- was removed when
    # the capabilities deck and the "everywhere" device row replaced it. Guard
    # the removal so the band cannot come back without that mobile handling.
    index = _read(WWW_PUBLIC / "index.html")

    assert '<section\n        id="network"' not in index
    assert "ForkMesh interactive node selector" not in index
    assert "top-[-0.5px] h-px" not in index

    # The active-nodes page still has to be reachable from the landing page;
    # the section that replaced the band carries the link now.
    everywhere = index[
        index.index('<section id="everywhere"')
        : index.index('<section id="support-forkmesh"')
    ]
    assert 'href="/network"' in everywhere


def test_homepage_mobile_mockups_do_not_overflow_by_default():
    index = _read(WWW_PUBLIC / "index.html")

    assert "-mr-56" not in index
    assert "relative top-6 w-full" in index
    assert "relative -right-8 top-11" not in index

    # The hero loop covers the section (object-fit: cover) rather than sizing
    # it, so the scene can never push wider than the viewport, and the laptop
    # shot in the hero is capped at the width of its column.
    scene_styles = index[index.index("      .hero-background {") :]
    scene_styles = scene_styles[: scene_styles.index("}")]
    assert "object-fit: cover;" in scene_styles
    assert 'src="/assets/hero-new.mp4"' in index
    assert "/assets/desktop-mockup/desktop-app.webp" in index


def test_fork_button_opens_the_list_of_people_who_forked():
    # The fork icon is a disclosure, not a mock stat: it carries a real count
    # from GET /api/repo/o/r/forks and opens a popover listing the forks.
    dashboard_js = _read(PUBLIC / "dashboard.js")
    render = dashboard_js[
        dashboard_js.index("function renderRepoDetail")
        : dashboard_js.index("function findRepository")
    ]
    assert "data-repo-fork-wrap" in render
    assert "data-repo-fork-menu" in render
    assert "data-repo-fork-list" in render
    assert 'aria-label="Show who forked this repository"' in render
    # No mock number behind the badge any more.
    assert ":fork`" not in render

    loader = dashboard_js[
        dashboard_js.index("async function loadRepoForks")
        : dashboard_js.index("function setRepoCollectionCounts")
    ]
    assert "`${repoApiBase(repo)}/forks`" in loader
    assert "data-repo-fork-count" in loader
    assert "Nobody has forked this repository yet." in loader

    click_handler = dashboard_js[
        dashboard_js.index(
            'const forkButton = event.target.closest("[data-repo-action-fork]")')
        :
    ][:900]
    assert "loadRepoForks(state.selectedRepo)" in click_handler
    assert 'forkButton.setAttribute("aria-expanded"' in click_handler
