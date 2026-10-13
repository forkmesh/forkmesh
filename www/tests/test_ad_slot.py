"""Source-text contract for the ForkMesh Ads slot above the site footer.

The slot is privacy-critical marketing chrome: the serve call may name the
surface and nothing else, ad strings are untrusted plain text, and legal,
security and documentation pages stay ad-free. These tests pin those rules on
the delimited block in site-footer.js.
"""

import re
from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[1] / "public"
SOURCE = (PUBLIC / "site-footer.js").read_text(encoding="utf-8")
START = "// ==== ForkMesh Ads slot: start ===="
END = "// ==== ForkMesh Ads slot: end ===="


def ads_block():
    assert SOURCE.count(START) == 1
    assert SOURCE.count(END) == 1
    start = SOURCE.index(START)
    end = SOURCE.index(END)
    assert start < end
    return SOURCE[start:end]


BLOCK = ads_block()


def code_only(text):
    """The block with comments removed, so prose cannot satisfy or trip a pin."""
    return "\n".join(
        line for line in text.splitlines() if not line.strip().startswith("//")
    )


CODE = code_only(BLOCK)


def test_serve_call_names_only_the_www_placement():
    assert '"/api/ads/serve?placement=www"' in CODE
    # No other query parameter is ever appended to the serve URL.
    assert "placement=www&" not in CODE
    assert "searchParams" not in CODE
    assert 'JSON.stringify({ placement: "www" })' in CODE
    assert "/api/ads/${id}/click" in CODE


def test_both_requests_omit_credentials_and_referrer():
    fetches = re.findall(r"\.fetch\(", CODE)
    assert len(fetches) == 2
    assert CODE.count('credentials: "omit"') == 2
    assert CODE.count('referrerPolicy: "no-referrer"') >= 2
    assert '"include"' not in CODE
    assert "same-origin" not in CODE


def test_no_viewer_identity_reaches_the_ads_code():
    lowered = CODE.lower()
    for needle in (
        "authorization",
        "bearer",
        "sessiontoken",
        "forkmesh.session",
        "document.cookie",
        "restoresession",
        "forkmeshanalytics",
    ):
        assert needle not in lowered, needle
    # The only storage key the block touches is its own dismissal stamp.
    assert re.findall(r"localStorage\.\w+\(([A-Z_]+)", CODE) == [
        "ADS_HIDDEN_KEY",
        "ADS_HIDDEN_KEY",
    ]


def test_api_client_keeps_an_explicit_omit():
    # The routed fetch upgrades only an absent or "same-origin" credentials
    # mode to "include"; an explicit "omit" must keep passing through.
    client = (PUBLIC / "api-client.js").read_text(encoding="utf-8")
    assert 'if (!credentials || credentials === "same-origin") {' in client


def test_ad_strings_render_as_text_only():
    for needle in (
        "innerHTML",
        "outerHTML",
        "insertAdjacentHTML",
        "document.write",
        "createContextualFragment",
        "<img",
        "createElement(\"img\")",
        "createElement(\"iframe\")",
        "createElement(\"script\")",
    ):
        assert needle not in BLOCK, needle
    assert "node.textContent = text" in CODE
    assert 'url.protocol !== "https:"' in CODE


def test_cta_link_attributes():
    assert 'cta.target = "_blank";' in CODE
    assert 'cta.rel = "sponsored noopener noreferrer";' in CODE
    assert 'cta.referrerPolicy = "no-referrer";' in CODE
    assert "keepalive: true" in CODE


def test_label_policy_and_advertise_links():
    assert '"fm-ad-label", "Sponsored"' in CODE
    assert 'slot.setAttribute("aria-label", "Sponsored");' in CODE
    assert 'document.createElement("aside")' in CODE
    assert 'ADS_POLICY_URL = "/docs/operations/ad-network/"' in CODE
    assert '"Why this ad?"' in CODE
    assert 'ADS_ADVERTISE_URL = "https://app.forkmesh.com/dashboard/ads"' in CODE
    assert '"Advertise here"' in CODE


def test_denylist_keeps_sensitive_pages_ad_free():
    match = re.search(r"const ADS_PATH_DENYLIST = \[(.*?)\];", CODE, re.S)
    assert match
    entries = re.findall(r'"([^"]+)"', match.group(1))
    for path in ("/privacy", "/terms", "/security-report", "/404", "/docs"):
        assert path in entries, path
    assert "adsPathDenied(window.location.pathname)" in CODE
    assert 'hasAttribute("data-forkmesh-no-ads")' in CODE


def test_hide_is_remembered_locally_for_seven_days():
    assert 'ADS_HIDDEN_KEY = "forkmesh.ads.hiddenUntil"' in CODE
    assert "ADS_HIDE_MS = 7 * 24 * 60 * 60 * 1000" in CODE
    for name in ("adsHidden", "rememberAdsHidden"):
        body = CODE[CODE.index(f"function {name}()"):]
        body = body[: body.index("\n  }\n")]
        assert "try {" in body and "catch" in body, name


def test_slot_is_hidden_until_an_ad_arrives_and_deferred_until_idle():
    assert "slot.hidden = true;" in CODE
    assert ".fm-ad-slot[hidden] { display: none !important; }" in CODE
    # The slot element is created only after a valid ad was parsed.
    assert CODE.index("normalizeAd(payload.ad)") < CODE.index(
        'document.createElement("aside")'
    )
    assert "requestIdleCallback(run, { timeout: ADS_IDLE_TIMEOUT_MS })" in CODE
    assert "ADS_IDLE_TIMEOUT_MS = 1500" in CODE
    assert "footer.before(slot);" in CODE


def test_slot_is_scheduled_once_after_the_footers_mount():
    mount_all = SOURCE[SOURCE.index("function mountAll()"):]
    mount_all = mount_all[: mount_all.index("}")]
    assert mount_all.index("mountFooters();") < mount_all.index(
        "scheduleAdSlot();"
    )
    assert SOURCE.count("scheduleAdSlot();") == 1
    assert "if (adSlotScheduled) return;" in CODE
    assert 'document.querySelector("[data-forkmesh-ad-slot]")' in CODE


def test_no_static_page_ships_ad_markup():
    # The slot exists only at runtime; nothing is baked into a page, so the
    # banner-free homepage contract keeps holding.
    home = (PUBLIC / "index.html").read_text(encoding="utf-8")
    assert "fm-ad-slot" not in home
    assert "/api/ads/" not in home
