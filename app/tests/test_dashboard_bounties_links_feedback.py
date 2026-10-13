import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402


def read(rel):
    return (PUBLIC / rel).read_text(encoding="utf-8")


BOUNTIES_JS = read("dashboard/js/09-bounties.js")
LINKS_JS = read("dashboard/js/09-links.js")
FEEDBACK_JS = read("dashboard/js/09-feedback.js")
SIDEBAR = read("dashboard/partials/sidebar.html")
HEADER = read("dashboard/partials/header.html")
MODALS = read("dashboard/partials/modals.html")
TAILWIND = read("dashboard/tailwind.css")
WRANGLER = (ROOT / "wrangler.toml").read_text(encoding="utf-8")


def test_bounties_and_links_are_composed_dashboard_pages():
    for page_id, route in (
        ("bounties", "/dashboard/bounties"),
        ("links", "/dashboard/links"),
    ):
        page = dashboard_shell.PAGES[page_id]
        assert page["view"] == page_id
        assert page["nav"] == page_id
        assert page["route"] == route
        assert page["asset"] == "dashboard/%s/index.html" % page_id
        assert static_routes.DASHBOARD_PAGE_ASSETS[route] == page["asset"]
        assert "/" + page["asset"] in static_routes.BLOCKED_STATIC_HTML_PATHS
        assert '"!%s",' % route in WRANGLER
        assert '"!%s/",' % route in WRANGLER
        assert dashboard_bundle.LAZY_CHUNKS[page_id] == "09-%s.js" % page_id
        assert dashboard_shell.CHUNK_PAGES[page_id] == page_id
        assert "dashboard-page-%s.js" % page_id in (
            dashboard_shell.CACHE_BUSTED_ASSETS)
        built = read(page["asset"])
        assert '<section data-view="%s"' % page_id in built
        assert re.search(
            r'<a data-nav="%s" data-nav-link aria-current="page"' % page_id,
            built)
        assert (PUBLIC / dashboard_bundle.chunk_asset(page_id)).is_file()
    assert "09-feedback.js" in dashboard_bundle.FRAGMENTS
    assert "09-feedback.js" not in dashboard_bundle.LAZY_CHUNKS.values()


def test_sidebar_places_bounties_and_links_beside_their_neighbours():
    order = [
        SIDEBAR.index('data-nav="microjobs"'),
        SIDEBAR.index('data-nav="bounties"'),
        SIDEBAR.index('data-nav="referrals"'),
        SIDEBAR.index('data-nav="links"'),
        SIDEBAR.index('data-nav="challenges"'),
    ]
    assert order == sorted(order)
    assert 'href="/dashboard/bounties"' in SIDEBAR
    assert 'href="/dashboard/links"' in SIDEBAR


def test_feedback_dialog_posts_to_the_feedback_api_as_web():
    assert "data-feedback-modal" in MODALS
    assert 'role="dialog"' in MODALS
    assert 'maxlength="2000"' in MODALS
    assert "<button data-feedback-open" in HEADER
    assert "<button data-feedback-open" in SIDEBAR
    assert 'fetch("/api/feedback"' in FEEDBACK_JS
    assert 'source: "web", vote: "feedback", message' in FEEDBACK_JS
    assert 'params.get("feedback") !== "1"' in FEEDBACK_JS
    assert "history.replaceState" in FEEDBACK_JS
    assert 'event.key !== "Escape"' in FEEDBACK_JS
    boot = read("dashboard/js/10-boot.js")
    assert "initDashboardFeedback();" in boot


def test_bounties_page_offers_what_the_lobby_desk_did():
    for contract in (
        'kind: "bid"',
        "bountyAmountSol: amountSol",
        'destination: "department"',
        'assigneeKind: "user"',
        'bountyRequest("POST", "/api/tasks"',
        "/api/tasks?offset=",
        'task?.kind === "bid"',
        "org_member_required",
        "invalid_bounty_request",
        "/dashboard/tasks?task=",
    ):
        assert contract in BOUNTIES_JS, contract
    view = read("dashboard/partials/views/bounties.html")
    for department in (
        "general", "engineering", "product-design", "marketing", "security",
        "infrastructure", "community", "partnerships", "operations",
        "executive", "quality-assurance",
    ):
        assert '<option value="%s">' % department in view
    assert "does not reserve, custody, or transfer SOL" in view


def test_links_page_combines_submission_and_the_rewards_board():
    for contract in (
        'fetchJson("/api/world/link-kiosk"',
        'fetch("/api/world/link-kiosk", {',
        "consent: true",
        'rel="nofollow ugc noopener noreferrer"',
        "solana:${wallet}?amount=${reward.toFixed(8)}",
        'url.protocol !== "https:"',
        "paymentStatus",
        "potentialTraffic",
        "account_link_limit",
        "link_already_submitted",
    ):
        assert contract in LINKS_JS, contract
    view = read("dashboard/partials/views/links.html")
    assert 'name="consent" type="checkbox" required' in view
    for channel in ("article", "community", "social", "video", "other"):
        assert '<option value="%s">' % channel in view


def test_new_views_use_classes_the_prebuilt_stylesheet_carries():
    for utility in (
        r".aria-pressed\:bg-secondary[aria-pressed=true]",
        ".flex-col-reverse",
        ".line-clamp-2",
        ".items-stretch",
        r".sm\:flex-col",
        r".sm\:items-end",
    ):
        assert utility in TAILWIND, utility
