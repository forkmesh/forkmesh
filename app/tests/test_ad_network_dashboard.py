#!/usr/bin/env python3
"""ForkMesh Ads on the web dashboard: the /dashboard/ads page and the `web` slot.

Source-text contracts. The Worker side is covered by test_ad_network.py; this
file pins the wiring of the page and, more importantly, the privacy rules the
slot must keep: an anonymous serve call, plain-text rendering, a labelled and
dismissible slot, and a destination that opens without a referrer.
"""

import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402


PUBLIC = ROOT / "public"
DASHBOARD = PUBLIC / "dashboard"
ADS_JS = (DASHBOARD / "js" / "09-ads.js").read_text(encoding="utf-8")
BOOT_JS = (DASHBOARD / "js" / "10-boot.js").read_text(encoding="utf-8")
STATE_JS = (DASHBOARD / "js" / "01-state.js").read_text(encoding="utf-8")
ADS_VIEW = (DASHBOARD / "partials" / "views" / "ads.html").read_text(encoding="utf-8")
MAIN_PARTIAL = (DASHBOARD / "partials" / "main.html").read_text(encoding="utf-8")
SIDEBAR = (DASHBOARD / "partials" / "sidebar.html").read_text(encoding="utf-8")
BUNDLE = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))

SLOT_MARKER = 'data-forkmesh-ad-slot="web"'


def function_source(name):
    """The text of one top-level fragment function, up to the next one."""
    match = re.search(
        r"^  (?:async )?function %s\(.*?(?=^  (?:async )?function |\Z)" % re.escape(name),
        ADS_JS, re.M | re.S)
    assert match, name
    return match.group(0)


def test_ads_page_is_a_composed_dashboard_page():
    page = dashboard_shell.PAGES["ads"]
    assert page["route"] == "/dashboard/ads"
    assert page["view"] == "ads"
    assert page["section"] == "ads"
    assert page["nav"] == "ads"
    assert page["asset"] == "dashboard/ads/index.html"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/dashboard/ads"] == page["asset"]
    assert "/dashboard/ads/index.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    run_worker_first = WRANGLER["assets"]["run_worker_first"]
    assert "!/dashboard/ads" in run_worker_first
    assert "!/dashboard/ads/" in run_worker_first


def test_fragment_boot_state_and_sidebar_are_wired():
    assert "09-ads.js" in dashboard_bundle.FRAGMENTS
    assert '"ads": initAdsPage,' in BOOT_JS
    assert "function initAdsPage()" in ADS_JS
    assert "adsView: {" in STATE_JS
    # The slot is started from boot for every page, after the page's own init.
    boot = BOOT_JS[BOOT_JS.index("async function bootDashboard()"):]
    assert boot.index("PAGE_INITS[currentPage()]") < boot.index("initAdSlot();")
    assert 'data-nav="ads"' in SIDEBAR
    assert 'href="/dashboard/ads"' in SIDEBAR
    link = SIDEBAR[SIDEBAR.index('data-nav="ads"'):]
    link = link[:link.index("</a>")]
    assert 'data-lucide="megaphone"' in link
    assert link.rstrip().endswith("Ads")
    # The shipped bundle is the fragments, rebuilt.
    assert ADS_JS in BUNDLE


def test_built_ads_document_has_exactly_one_ads_view():
    built = PUBLIC / dashboard_shell.PAGES["ads"]["asset"]
    assert built.is_file(), "run tools/build_dashboard_assets.py"
    html = built.read_text(encoding="utf-8")
    assert html.count('data-view="ads"') == 1
    assert html.count("<section data-view=") == 1
    assert 'data-page="ads"' in html
    assert ADS_VIEW.count("<section data-view=") == 1
    # The form pieces the page script drives.
    for marker in (
        "data-ads-form", "data-ads-preview", "data-ads-attestation",
        "data-ads-submit", "data-ads-save-draft", "data-ads-list",
        "data-ads-review", "data-ads-tiers", "data-ads-prohibited",
        "data-forkmesh-membership",
    ):
        assert marker in html, marker
    assert ("I confirm this ad follows the ForkMesh Ads policy and is honest "
            "about what it promotes") in html
    assert "There is no ad billing yet" in html
    # Owner and reviewer halves ship hidden; the script reveals them.
    assert re.search(r"<div data-ads-owner hidden", html)
    assert re.search(r"<div data-ads-review hidden", html)


def test_slot_ships_hidden_in_every_composed_document():
    assert MAIN_PARTIAL.count(SLOT_MARKER) == 1
    slot = re.search(r"<aside %s[^>]*>" % re.escape(SLOT_MARKER), MAIN_PARTIAL)
    assert slot and " hidden" in slot.group(0)
    # The profile and repo documents also serve the public /@name and
    # /owner/repo pages, which are edge-cached and identical for all viewers:
    # the slot must be there, empty, and filled client-side only.
    for page_id in ("profile", "repo", "home", "ads"):
        html = (PUBLIC / dashboard_shell.PAGES[page_id]["asset"]).read_text(encoding="utf-8")
        assert html.count(SLOT_MARKER) == 1, page_id
        assert re.search(
            r"<aside %s[^>]*></aside>" % re.escape(SLOT_MARKER), html), page_id
    # The ads page itself and the chat document never load an ad.
    assert 'const ADS_SLOT_SKIPPED_PAGES = new Set(["ads", "chat"]);' in ADS_JS
    assert "ADS_SLOT_SKIPPED_PAGES.has(currentPage())" in function_source("initAdSlot")


def test_serve_call_is_anonymous_and_deferred():
    load = function_source("loadAdSlot")
    assert 'adsPublicRead("/api/ads/serve?placement=web")' in load
    assert "fetchJson" not in load
    public_read = function_source("adsPublicRead")
    assert 'credentials: "omit"' in public_read
    assert "authorization" not in public_read.lower()
    assert "sessionToken" not in public_read
    assert "fetchJson" not in public_read
    # Nothing between the slot entry point and the wire touches the session.
    for name in ("initAdSlot", "loadAdSlot", "reportAdClick"):
        source = function_source(name)
        assert "authorization" not in source.lower(), name
        assert "sessionToken" not in source, name
    init = function_source("initAdSlot")
    assert "requestIdleCallback" in init
    assert "window.setTimeout(run, 1500)" in init
    # A failed or empty answer leaves the slot collapsed: one request, no retry.
    assert load.count("adsPublicRead(") == 1
    assert "slot.hidden = false" in load
    assert "setInterval" not in ADS_JS


def test_slot_is_labelled_dismissible_and_opens_without_a_referrer():
    card = function_source("adCardElement")
    assert '"Sponsored"' in card
    assert 'cta.rel = "sponsored noopener noreferrer"' in card
    assert 'cta.target = "_blank"' in card
    assert 'cta.setAttribute("referrerpolicy", "no-referrer")' in card
    assert '"Why this ad?"' in card
    assert 'const ADS_POLICY_DOC_URL = "https://forkmesh.com/docs/operations/ad-network/";' in ADS_JS
    assert '"Hide"' in card
    assert 'const ADS_HIDE_KEY = "forkmesh.ads.hiddenUntil";' in ADS_JS
    assert "const ADS_HIDE_MS = 7 * 24 * 60 * 60 * 1000;" in ADS_JS
    # Report only for signed-in viewers.
    assert "live && adsSignedIn()" in card
    for reason in ("prohibited", "misleading", "offensive", "broken", "other"):
        assert ('{ id: "%s",' % reason) in ADS_JS, reason
    # Only a parsed https URL may become the link.
    safe = function_source("adsSafeDestination")
    assert 'url.protocol !== "https:"' in safe
    click = function_source("reportAdClick")
    assert "keepalive: true" in click
    assert 'credentials: "omit"' in click
    assert 'method: "POST"' in click
    assert "/click`" in click


def test_ad_text_never_reaches_the_dom_as_html():
    # API strings go through textContent (adsEl); innerHTML is reserved for
    # static markup and the shared loading spinner.
    assert "node.textContent = String(text)" in function_source("adsEl")
    for line in ADS_JS.splitlines():
        if "innerHTML" not in line:
            continue
        assert "loadingHtml(" in line or line.rstrip().endswith("`<p class=\"text-sm text-muted-foreground\">"), line
    assert "insertAdjacentHTML" not in ADS_JS
    assert "outerHTML" not in ADS_JS


def test_owner_and_reviewer_flows_follow_the_contract():
    for endpoint in (
        '"/api/ads/policy"', '"/api/ads/mine"', '"/api/ads"',
        "`/api/ads/${editing.id}`", "`/api/ads/${ad.id}/${action}`",
        "`/api/ads/${ad.id}/review`", "`/api/ads/${ad.id}/report`",
        "/api/ads/review?status=",
    ):
        assert endpoint in ADS_JS, endpoint
    request = function_source("adsRequest")
    assert 'cache: "no-store"' in request
    assert 'credentials: "same-origin"' in request
    assert "headers.authorization = `Bearer ${token}`" in request
    # Every delivery word has a plain-language explanation.
    for word in (
        "running", "paused", "scheduled", "ended", "needs-membership",
        "over-allowance", "review-expired", "draft", "pending", "rejected",
        "suspended", "withdrawn",
    ):
        assert ('"%s": { label:' % word) in ADS_JS, word
    # Every contract error code has a friendly message.
    for code in (
        "unauthorized", "forbidden", "not_found", "invalid_transition",
        "ad_suspended", "ad_limit_reached", "pending_limit_reached",
        "attestation_required", "headline_required", "sponsor_required",
        "invalid_category", "invalid_url", "url_shortener_not_allowed",
        "tracking_parameter_not_allowed", "invalid_placements",
        "invalid_schedule", "unsupported_ad_field", "invalid_reason",
        "cannot_report_own_ad", "reason_required", "creative_changed",
    ):
        assert re.search(r"^    %s: \"" % code, ADS_JS, re.M), code
    # Approve sends back the hash the reviewer actually read.
    review = function_source("submitAdsReview")
    assert "body.contentHash = ad.contentHash" in review
    assert "ADS_ERRORS.reason_required" in review
    assert "loadAdsReviewQueue()" in review
    # The queue only renders for reviewers, and names a self-review.
    assert "state.adsView.mine?.isReviewer === true" in function_source("renderAdsReview")
    assert "recorded as a self-review" in function_source("adsReviewItemElement")
    assert 'open.rel = "noopener noreferrer nofollow"' in function_source("adsReviewItemElement")
    # Submitting always carries the owner's attestation.
    assert "attestation_required" in function_source("saveAdsForm")
    assert "window.confirm(" in function_source("saveAdsForm")


def test_bundle_parses():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--check", str(PUBLIC / "dashboard.js")],
        capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
