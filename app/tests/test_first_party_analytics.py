#!/usr/bin/env python3
"""Every page loads the shared, default-off first-party analytics controller."""

import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest


PROJECT = Path(__file__).resolve().parents[2]
APP_PUBLIC = PROJECT / "app" / "public"
WWW_PUBLIC = PROJECT / "www" / "public"
WORLD_PUBLIC = PROJECT / "world" / "public"
PUBLIC_DIRS = (
    ("app", APP_PUBLIC),
    ("www", WWW_PUBLIC),
    ("world", WORLD_PUBLIC),
)
ENTRY = PROJECT / "app" / "src" / "entry.py"
ADMIN_CONSOLE = ENTRY.with_name("admin_console.py")
WORLD_WORKER = PROJECT / "world" / "worker.js"
PACKAGE_SDK = (PROJECT / "marketplace" / "packages" / "analytics" / "content"
               / "sdk" / "analytics-sdk.js")


def _loads_controller(html):
    return bool(re.search(r'<script\b[^>]*\bsrc="/analytics\.js"[^>]*>', html))


def _function(js, name):
    match = re.search(r"  function %s\(.*?\n  }\n" % re.escape(name), js, re.S)
    assert match, name
    return match.group(0)


def test_site_controller_is_consent_gated_and_first_party():
    js = (WWW_PUBLIC / "analytics.js").read_text(encoding="utf-8")
    # Off by default: the single automatic start needs a stored explicit grant.
    assert 'if (storedConsent() === "granted" && !privacySignalEnabled()) start();' in js
    start = _function(js, "start")
    assert 'storedConsent() !== "granted"' in start and "privacySignalEnabled()" in start
    assert "navigator.globalPrivacyControl === true" in js
    assert "navigator.doNotTrack" in js
    # The SDK is fetched only from inside start(), never at page load.
    assert js.count("loadSdk().then(") == 1 and "loadSdk().then(" in start
    assert 'const SDK_SRC = "/analytics-sdk.js";' in js
    assert 'const PROJECT_KEY = "site";' in js
    assert "apiHost: location.origin" in js
    # No third-party analytics host, loader, or key survives.
    lowered = js.lower()
    for needle in ("posthog", "phc_", "b.forkmesh.com", "https://", "googletagmanager",
                   "segment.com", "plausible.io"):
        assert needle not in lowered, needle
    # Withdrawn consent stops the SDK, even one still loading.
    stop = _function(js, "stop")
    assert "sdk.optOut();" in stop and "sdk.shutdown();" in stop
    assert "if (!library || !started) return;" in start
    assert "PRODUCT_ACTIONS.has(action)" in js
    assert 'sdk.capture("forkmesh_product_action", {' in js
    assert "stage: PRODUCT_ACTIONS.get(action)," in js
    assert '"home-pro-membership"' in js
    assert '"repository-clone-copied"' in js
    assert "data-forkmesh-product-action" in js
    assert "location.search" not in js
    assert "document.referrer" not in js


def test_private_surfaces_send_only_a_coarse_category():
    js = (WWW_PUBLIC / "analytics.js").read_text(encoding="utf-8")
    surfaces = re.search(r"const PUBLIC_SURFACES = new Set\(\[(.*?)\]\);", js, re.S)
    assert surfaces
    public = set(re.findall(r'"([a-z0-9-]+)"', surfaces.group(1)))
    # Anything that can render private names, decrypted chat, or notes is
    # deliberately absent from the detailed set.
    assert public == {"home", "documentation", "blog", "public-page"}
    assert not public & {"dashboard", "chat", "world", "network", "repository",
                         "account"}
    start = _function(js, "start")
    for option in ("capturePageleave", "autocapture", "heatmaps",
                   "sessionRecording", "surveys"):
        assert "%s: detailed," % option in start, option
    assert "maskUrlQuery: true" in start and "sanitizeProperties," in start
    # /api/client-errors stays the operational collector.
    assert "captureErrors: false" in start
    sanitize = _function(js, "sanitizeProperties")
    assert "if (PUBLIC_SURFACES.has(surface)) return result;" in sanitize
    assert "delete result[key]" in sanitize
    assert "result.$pathname = `/(${surface})`;" in sanitize
    stripped = re.search(r"const PRIVATE_SURFACE_PROPERTIES =\s*/(.*?)/;", js, re.S)
    assert stripped
    for name in ("current_url", "pathname", "title", "referrer", "el_text", "elements"):
        assert name in stripped.group(1), name


def test_site_sdk_is_the_package_sdk():
    site = (WWW_PUBLIC / "analytics-sdk.js").read_text(encoding="utf-8")
    package = PACKAGE_SDK.read_text(encoding="utf-8")
    assert site.endswith(package) and site.startswith("// GENERATED copy of ")
    assert "/api/analytics/_/capture" in package
    assert "posthog" not in package.lower()


def test_privacy_page_discloses_and_controls_optional_analytics():
    html = (WWW_PUBLIC / "privacy.html").read_text(encoding="utf-8")
    assert "First-party product analytics is disabled by default" in html
    assert "no analytics company or other third party receives anything" in html
    assert "no IP address is stored" in html
    assert "fixed action names" in html
    assert "copy a clone command" in html
    assert "only a coarse page category" in html
    assert "everything you type masked" in html
    assert "PostHog" not in html and "b.forkmesh.com" not in html
    assert 'id="analytics-consent-status"' in html
    assert 'id="analytics-consent-deny"' in html
    assert 'id="analytics-consent-grant"' in html
    assert "ForkMeshAnalytics?.setConsent?.(true)" in html
    assert "ForkMeshAnalytics?.setConsent?.(false)" in html


def test_no_tracked_source_still_names_the_removed_provider():
    catalog = {"tools/ecosystem_third_parties.json"}
    offenders = []
    for root in ("app/src", "app/public", "app/tools", "www/public", "world/public",
                 "tools", "conformance"):
        for path in (PROJECT / root).rglob("*"):
            if path.suffix not in (".py", ".js", ".html", ".json", ".toml"):
                continue
            if "node_modules" in path.parts:
                continue
            relative = path.relative_to(PROJECT).as_posix()
            if relative in catalog:
                continue
            if "posthog" in path.read_text(encoding="utf-8", errors="ignore").lower():
                offenders.append(relative)
    assert offenders == []


def test_all_public_html_pages_load_the_controller():
    # dashboard/partials/*.html are shell fragments (no <head>); they are
    # composed into generated dashboard assets before deploy, and the shell
    # they land in carries the snippet — so they are exempt. So is the
    # per-contact investor deck: a private link that never shows a consent
    # banner; it reports its own token-free visit to the site project.
    exempt = {("app", "pitch-deck/deck.html")}
    html_pages = [
        (owner, public, page)
        for owner, public in PUBLIC_DIRS
        for page in sorted(public.rglob("*.html"))
        if "partials" not in page.relative_to(public).parts
        and (owner, page.relative_to(public).as_posix()) not in exempt
    ]
    assert html_pages

    missing = [
        owner + "/" + page.relative_to(public).as_posix()
        for owner, public, page in html_pages
        if not _loads_controller(page.read_text(encoding="utf-8"))
    ]
    assert missing == [], "pages missing the analytics controller: %s" % missing


def test_world_routes_the_shared_controller_and_sdk_through_the_app():
    worker = WORLD_WORKER.read_text(encoding="utf-8")
    app_assets = re.search(
        r"const APP_ASSET_PATHS = new Set\(\[(.*?)\]\);", worker, re.S)
    assert app_assets
    assert '"/analytics.js"' in app_assets.group(1)
    assert '"/analytics-sdk.js"' in app_assets.group(1)


def test_worker_generated_pages_apply_their_intended_script_policy():
    admin = ADMIN_CONSOLE.read_text(encoding="utf-8")
    admin_page = re.search(
        r"^def render_admin_html\(.*?(?=^(?:async )?def |\Z)",
        admin, re.M | re.S)
    assert admin_page and "/analytics.js" in admin_page.group(0)

    # Email scanners can load the confirmation page. It intentionally has no
    # script or automatic request, and its CSP must keep that guarantee.
    entry = ENTRY.read_text(encoding="utf-8")
    verify_page = re.search(
        r"^def _verify_email_page\(.*?(?=^(?:async )?def |\Z)",
        entry, re.M | re.S)
    assert verify_page
    assert "/analytics.js" not in verify_page.group(0)
    assert '"default-src \'none\'' in verify_page.group(0)


def test_public_sections_get_full_detail_and_personal_links_stay_coarse():
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    js = (WWW_PUBLIC / "analytics.js").read_text(encoding="utf-8")
    source = _function(js, "coarseSurface")
    paths = ["/", "/hackathon/rules", "/hackathon/archive/2026-spring",
             "/market/crm", "/ecosystem/forkmesh", "/hackathon/projects/" + "a" * 32,
             "/hackathon/join/alice", "/hackathon/squad/abc123",
             "/hackathon/challenge/tok123", "/pitch-deck/abcdef123456",
             "/alice/repository", "/dashboard/crm", "/features"]
    script = source + "\nconsole.log(JSON.stringify(%s.map(coarseSurface)));" % json.dumps(paths)
    result = subprocess.run(["node", "-e", script], capture_output=True, text=True, check=True)
    assert dict(zip(paths, json.loads(result.stdout))) == {
        "/": "home", "/hackathon/rules": "public-page",
        "/hackathon/archive/2026-spring": "public-page", "/market/crm": "public-page",
        "/ecosystem/forkmesh": "public-page",
        "/hackathon/projects/" + "a" * 32: "public-page",
        "/hackathon/join/alice": "hackathon-link", "/hackathon/squad/abc123": "hackathon-link",
        "/hackathon/challenge/tok123": "hackathon-link",
        "/pitch-deck/abcdef123456": "investor-deck",
        "/alice/repository": "repository", "/dashboard/crm": "dashboard",
        "/features": "public-page"}


def test_investor_deck_reports_a_token_free_visit_to_the_site_project():
    deck = (APP_PUBLIC / "pitch-deck" / "deck.html").read_text(encoding="utf-8")
    assert not _loads_controller(deck)
    record = re.search(r"        const record = \(events\) => \{.*?\n        \};\n", deck, re.S)
    assert record
    body = record.group(0)
    assert 'key: "site"' in body and "/api/analytics/_/capture" in body
    assert "token" not in body and "if (privacySignal) return;" in body
    visit = re.search(r"        const visit = \{.*?\n        \};\n", deck, re.S)
    assert visit and "token" not in visit.group(0) and "viewer" not in visit.group(0)
    assert '$pathname: "/pitch-deck"' in visit.group(0)
    assert "record(events);" in deck
