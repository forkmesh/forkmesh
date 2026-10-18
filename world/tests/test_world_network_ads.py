#!/usr/bin/env python3
"""ForkMesh Ads fallback in the World Town Square panel.

The community-voted placement is governance-backed and always wins. The
first-party ad network is only asked after that request resolved empty, in the
same proximity-gated moment, and never on World's boot path. The source-text
checks pin that structure; the node checks run the real functions lifted out
of world.js against stubs.
"""

from pathlib import Path
import json
import re
import shutil
import subprocess

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]
WORLD = REPOSITORY / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")

SERVE = '"/api/ads/serve?placement=world"'
POLICY = "https://forkmesh.com/docs/operations/ad-network/"


def block(source, header):
    """Return the brace-balanced block that starts at `header`."""
    start = source.index(header)
    # Skip the parameter list first: `({ refetch = true } = {})` has braces.
    index = source.index("(", start)
    parens = 0
    while True:
        parens += {"(": 1, ")": -1}.get(source[index], 0)
        if parens == 0:
            break
        index += 1
    depth = 0
    index = source.index("{", index)
    in_string = ""
    while index < len(source):
        char = source[index]
        if in_string:
            if char == "\\":
                index += 1
            elif char == in_string:
                in_string = ""
        elif source.startswith("//", index):
            index = source.index("\n", index)
            continue
        elif source.startswith("/*", index):
            index = source.index("*/", index) + 2
            continue
        elif char in "\"'`":
            in_string = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
        index += 1
    raise AssertionError(f"unbalanced block for {header}")


SETTLE = block(APP, "  settleSponsoredPlacement(result, {")
LOAD = block(APP, "  loadNetworkAd() {")
CLICK = block(APP, "  recordNetworkAdClick(id) {")
HIDE = block(APP, "  hideNetworkAd() {")
RENDER = block(APP, "  renderNetworkAd(container) {")
RENDER_COMMUNITY = block(APP, "  renderCommunityPlacement() {")
NORMALIZE = block(APP, "function normalizeNetworkAd(payload) {")


def test_serve_request_is_anonymous_cached_and_lives_only_in_the_helper():
    assert f"const NETWORK_AD_SERVE_PATH = {SERVE};" in APP
    # The URL exists once, and the constant is read once: inside the loader.
    assert APP.count("/api/ads/serve") == 1
    assert APP.count("NETWORK_AD_SERVE_PATH") == 2
    assert "this.fetchJSON(NETWORK_AD_SERVE_PATH, {" in LOAD
    assert "auth: false," in LOAD
    assert "timeout: 4000," in LOAD
    assert "maxAge: NETWORK_AD_MAX_AGE_MS," in LOAD
    assert "const NETWORK_AD_MAX_AGE_MS = 30 * 60 * 1000;" in APP
    # A failed serve call is an empty slot: no notice, no retry loop.
    assert "() => null," in LOAD
    assert "this.notice(" not in LOAD
    assert "setTimeout" not in LOAD and "setInterval" not in LOAD


def test_network_is_asked_only_after_the_community_request_resolved_empty():
    assert APP.count("this.loadNetworkAd()") == 1
    assert "this.loadNetworkAd()" in SETTLE
    guard = SETTLE.index("if (this.communityPlacement) {")
    early_return = SETTLE.index("return;", guard)
    fallback = SETTLE.index("if (resolved) void this.loadNetworkAd();")
    assert guard < early_return < fallback
    assert 'const resolved = result?.status === "fulfilled";' in SETTLE
    assert "normalizeCommunityPlacement(result.value)" in SETTLE
    # Both call sites hand over the settled community request and nothing
    # else, so the network call can never run in parallel with it.
    assert APP.count("this.settleSponsoredPlacement(") == 2
    assert (
        "this.settleSponsoredPlacement(placement, { keepOnError: true });"
        in block(APP, "  loadSocialWorldData() {")
    )
    assert "this.settleSponsoredPlacement(placementResult);" in block(
        APP, "  async loadWorldData({ forceMirrors = false } = {}) {"
    )
    # normalizeCommunityPlacement is no longer applied anywhere else.
    assert APP.count("normalizeCommunityPlacement(") == 2


def test_no_network_ad_request_on_the_boot_path():
    watch = block(APP, "  startSocialCircleWatch({ refetch = true } = {}) {")
    assert "void this.loadSocialWorldData();" in watch
    # The only entry points are the proximity-gated social load and the
    # explicit full refresh, which bootstrap never calls.
    assert APP.count("this.loadSocialWorldData()") == 1
    assert "Bootstrap never calls this cross-district fan-out." in APP
    for name in ("loadNetworkAd", "settleSponsoredPlacement", "/api/ads/"):
        for header in ("  async bootstrap(", "  async connectedCallback("):
            if header in APP:
                assert name not in block(APP, header)


def test_label_never_claims_community_review():
    assert (
        'const NETWORK_AD_LABEL = "Sponsored · reviewed by ForkMesh";' in APP
    )
    assert "label: NETWORK_AD_LABEL," in NORMALIZE
    # The shared panel class is the only place the word may appear.
    for source in (RENDER, NORMALIZE):
        prose = source.replace("world-community-placement", "")
        assert "ommunity" not in prose
    assert (
        'container.setAttribute("aria-label", "Sponsored, reviewed by ForkMesh");'
        in RENDER
    )
    # The community render restores its own aria-label and owns the modifier.
    assert '"Community-reviewed contextual placement",' in RENDER_COMMUNITY
    assert '"world-community-placement--network",' in RENDER_COMMUNITY
    assert "!placement && Boolean(this.networkAd)," in RENDER_COMMUNITY
    assert "this.renderNetworkAd(container);" in RENDER_COMMUNITY
    assert ".world-community-placement--network" in CSS
    assert (
        ".world-community-placement--network .world-community-placement-label"
        in CSS
    )


def test_render_escapes_every_api_string_and_links_safely():
    assert f'"{POLICY}";' in APP
    for field in ("label", "sponsor", "headline", "body", "url", "cta"):
        assert f"escapeHTML(ad.{field})" in RENDER
    # No API string reaches the template without the escape helper.
    assert not re.search(r"\$\{\s*ad\.(?!sponsor \?|body \?)", RENDER)
    assert 'rel="sponsored noopener noreferrer"' in RENDER
    assert RENDER.count('referrerpolicy="no-referrer"') == 2
    assert RENDER.count('target="_blank"') == 2
    assert "escapeHTML(NETWORK_AD_POLICY_URL)" in RENDER
    assert ">Why this ad?</a>" in RENDER
    assert "<img" not in RENDER and "<script" not in RENDER
    # Empty slot: collapsed, no placeholder text.
    assert "container.hidden = true;" in RENDER
    assert "container.replaceChildren();" in RENDER


def test_click_is_fire_and_forget_without_credentials():
    assert "fetch(`/api/ads/${id}/click`, {" in CLICK
    assert 'method: "POST",' in CLICK
    assert 'body: JSON.stringify({ placement: "world" }),' in CLICK
    assert 'credentials: "omit",' in CLICK
    assert "keepalive: true," in CLICK
    assert "}).catch(() => {});" in CLICK
    assert "Authorization" not in CLICK
    assert "await" not in CLICK
    assert "this.recordNetworkAdClick(ad.id)" in RENDER
    assert "preventDefault" not in RENDER


def test_hide_is_local_for_seven_days():
    assert (
        'const NETWORK_AD_HIDDEN_KEY = "forkmesh.world.networkAdHiddenUntil.v1";'
        in APP
    )
    assert "const NETWORK_AD_HIDE_MS = 7 * 24 * 60 * 60 * 1000;" in APP
    assert "localStorage.setItem(" in HIDE
    assert "fetch" not in HIDE
    assert "data-world-network-ad-hide" in RENDER
    # A hidden slot does not spend a request either.
    assert LOAD.index("networkAdHiddenUntil() > Date.now()") < LOAD.index(
        "this.fetchJSON("
    )


NODE = shutil.which("node")

HARNESS = """
const store = new Map();
globalThis.localStorage = {
  getItem: (key) => (store.has(key) ? store.get(key) : null),
  setItem: (key, value) => store.set(key, String(value)),
};
%(constants)s
%(functions)s
const COMMUNITY = {
  label: "Community-reviewed placement",
  tracking: "none",
  behavioralTargeting: false,
  sensitiveTargeting: false,
  personalDataUsed: false,
};
const AD = {
  id: "0123456789abcdef0123456789abcdef",
  label: "Sponsored",
  headline: "Ship builds you can audit",
  body: "Reproducible CI for open-source teams.",
  cta: "See how",
  sponsor: "Example Cooperative",
  url: "https://sponsor.example.org/builds",
};
function makeApp(serve) {
  const requests = [];
  const app = {
    %(methods)s,
    communityPlacement: null,
    networkAd: null,
    networkAdLoad: null,
    destroyed: false,
    renders: 0,
    requests,
    renderCommunityPlacement() { this.renders += 1; },
    async whenInstancePlugin(_id, request) { return request(); },
    fetchJSON(path, options) {
      requests.push({ path, options });
      return serve();
    },
  };
  return app;
}
const out = {};
out.normalize = {
  valid: normalizeNetworkAd({ ok: true, ad: AD }),
  nullAd: normalizeNetworkAd({ ok: true, ad: null }),
  notOk: normalizeNetworkAd({ ok: false, ad: AD }),
  badId: normalizeNetworkAd({ ok: true, ad: { ...AD, id: "ABC" } }),
  noHeadline: normalizeNetworkAd({ ok: true, ad: { ...AD, headline: "  " } }),
  http: normalizeNetworkAd({ ok: true, ad: { ...AD, url: "http://a.example.org/" } }),
  script: normalizeNetworkAd({ ok: true, ad: { ...AD, url: "javascript:alert(1)" } }),
  local: normalizeNetworkAd({ ok: true, ad: { ...AD, url: "https://localhost/x" } }),
  long: normalizeNetworkAd({
    ok: true,
    ad: {
      ...AD,
      headline: "h".repeat(200),
      body: "b".repeat(400),
      cta: "c".repeat(90),
      sponsor: "s".repeat(90),
    },
  }),
  bare: normalizeNetworkAd({ ok: true, ad: { id: AD.id, headline: "Hi", url: AD.url } }),
};
(async () => {
  const withCommunity = makeApp(async () => ({ ok: true, ad: AD }));
  withCommunity.networkAd = { stale: true };
  withCommunity.settleSponsoredPlacement({
    status: "fulfilled",
    value: {
      ...COMMUNITY,
      placements: [{
        label: COMMUNITY.label,
        tracking: "none",
        proposalId: "f".repeat(32),
        destinationUrl: "https://coop.example.org/",
        sponsor: "Coop",
        copy: "Hello",
      }],
    },
  });
  await withCommunity.networkAdLoad;
  out.withCommunity = {
    requests: withCommunity.requests.length,
    placement: Boolean(withCommunity.communityPlacement),
    networkAd: withCommunity.networkAd,
  };

  const empty = makeApp(async () => ({ ok: true, ad: AD }));
  empty.settleSponsoredPlacement({
    status: "fulfilled",
    value: { ...COMMUNITY, placements: [] },
  });
  empty.settleSponsoredPlacement({
    status: "fulfilled",
    value: { ...COMMUNITY, placements: [] },
  });
  await empty.networkAdLoad;
  out.empty = {
    requests: empty.requests,
    headline: empty.networkAd && empty.networkAd.headline,
    label: empty.networkAd && empty.networkAd.label,
    renders: empty.renders,
    loadCleared: empty.networkAdLoad === null,
  };

  const failedKeep = makeApp(async () => ({ ok: true, ad: AD }));
  failedKeep.settleSponsoredPlacement(
    { status: "rejected", reason: new Error("offline") },
    { keepOnError: true },
  );
  const failedDrop = makeApp(async () => ({ ok: true, ad: AD }));
  failedDrop.settleSponsoredPlacement({ status: "rejected" });
  out.failed = {
    keep: failedKeep.requests.length,
    drop: failedDrop.requests.length,
  };

  const serveFails = makeApp(async () => { throw new Error("503"); });
  serveFails.settleSponsoredPlacement({
    status: "fulfilled",
    value: { ...COMMUNITY, placements: [] },
  });
  await serveFails.networkAdLoad;
  out.serveFails = {
    requests: serveFails.requests.length,
    networkAd: serveFails.networkAd,
  };

  const hidden = makeApp(async () => ({ ok: true, ad: AD }));
  hidden.hideNetworkAd();
  const hiddenUntil = Number(store.get(NETWORK_AD_HIDDEN_KEY));
  hidden.settleSponsoredPlacement({
    status: "fulfilled",
    value: { ...COMMUNITY, placements: [] },
  });
  await hidden.networkAdLoad;
  out.hidden = {
    requests: hidden.requests.length,
    days: Math.round((hiddenUntil - Date.now()) / 86400000),
  };
  console.log(JSON.stringify(out));
})();
"""


def run_harness():
    constants = "\n".join(
        re.search(rf"^const {name} =[^;]*;", APP, re.M).group(0)
        for name in (
            "NETWORK_AD_SERVE_PATH",
            "NETWORK_AD_MAX_AGE_MS",
            "NETWORK_AD_POLICY_URL",
            "NETWORK_AD_LABEL",
            "NETWORK_AD_HIDDEN_KEY",
            "NETWORK_AD_HIDE_MS",
        )
    )
    functions = "\n".join(
        [
            # The community normalizer imports this from world-data.js.
            (
                "function sanitizePresenceText(value, fallback, max = 28) {"
                " return String(value || '').slice(0, max) || fallback; }"
            ),
            block(APP, "function sanitizeNotificationText("),
            block(APP, "function safePublicHTTPSURL(value) {"),
            block(APP, "function normalizeCommunityPlacement(value) {"),
            NORMALIZE,
            block(APP, "function networkAdHiddenUntil() {"),
        ]
    )
    methods = ",\n".join(part.strip() for part in (SETTLE, LOAD, HIDE))
    script = HARNESS % {
        "constants": constants,
        "functions": functions,
        "methods": methods,
    }
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@pytest.fixture(scope="module")
def harness():
    if not NODE:
        pytest.skip("node is not installed")
    return run_harness()


def test_normalize_network_ad_accepts_only_well_formed_ads(harness):
    normalize = harness["normalize"]
    assert normalize["valid"] == {
        "id": "0123456789abcdef0123456789abcdef",
        "label": "Sponsored · reviewed by ForkMesh",
        "headline": "Ship builds you can audit",
        "body": "Reproducible CI for open-source teams.",
        "cta": "See how",
        "sponsor": "Example Cooperative",
        "url": "https://sponsor.example.org/builds",
    }
    for key in (
        "nullAd", "notOk", "badId", "noHeadline", "http", "script", "local"
    ):
        assert normalize[key] is None, key
    long = normalize["long"]
    assert [len(long[k]) for k in ("headline", "body", "cta", "sponsor")] == [
        60, 160, 24, 48,
    ]
    assert normalize["bare"]["body"] == ""
    assert normalize["bare"]["sponsor"] == ""
    assert normalize["bare"]["cta"] == "Learn more"


def test_community_placement_wins_and_costs_the_network_no_request(harness):
    assert harness["withCommunity"] == {
        "requests": 0,
        "placement": True,
        "networkAd": None,
    }


def test_empty_community_result_falls_back_with_one_anonymous_request(harness):
    empty = harness["empty"]
    # Two settles while one load is in flight still make one request.
    assert empty["requests"] == [
        {
            "path": "/api/ads/serve?placement=world",
            "options": {
                    "auth": False,
                    "credentials": "omit",
                    "timeout": 4000,
                    "maxAge": 1800000,
                },
        }
    ]
    assert empty["headline"] == "Ship builds you can audit"
    assert empty["label"] == "Sponsored · reviewed by ForkMesh"
    assert empty["renders"] == 1
    assert empty["loadCleared"] is True


def test_failed_requests_render_nothing_and_never_retry(harness):
    # A community request that failed did not "resolve empty".
    assert harness["failed"] == {"keep": 0, "drop": 0}
    assert harness["serveFails"] == {"requests": 1, "networkAd": None}


def test_hidden_slot_spends_no_request_for_seven_days(harness):
    assert harness["hidden"] == {"requests": 0, "days": 7}
