#!/usr/bin/env python3
"""Every address ForkMesh hands a user is spelled on the apex forkmesh.com.

app.forkmesh.com, forkmesh.com and world.forkmesh.com are access layers in
front of one public site: www proxies /mcp, /@name, and the Git protocol paths
to the App worker, and 308s repository pages there. So a clone address, a
referral share link, an issue link, a profile, an MCP endpoint, or a fediverse
handle never needs to name a service subdomain -- and naming one leaks
plumbing into things people copy, paste, and post.

It also broke a claim outright: the Referrals page runs on app.forkmesh.com,
so `${location.origin}/r/<name>` handed the user
https://app.forkmesh.com/r/<name>. The microjobs verifier builds its accepted
spellings from the worker's public origin (PUBLIC_BASE_URL =
https://forkmesh.com), so it only ever looked for forkmesh.com/r/<name> --
posting the link the product gave you failed your own referral claim.

Routing is deliberately not covered here: repoApiBase and the API origin still
resolve to the subdomain that actually serves the bytes.
"""

from pathlib import Path
import json
import subprocess

import pytest

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[2]
DASHBOARD_JS = assembled_dashboard_js()
WORLD_JS = (ROOT / "world/public/world/world.js").read_text(encoding="utf-8")
MICROJOBS = (ROOT / "app/src/microjobs.py").read_text(encoding="utf-8")

# The hosts the official deployment answers on. Each one must produce apex
# addresses; anything else (self-hosted, local dev) keeps its own origin.
SERVICE_HOSTS = ["app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"]


def _dashboard_under_host(hostname, body):
    """Run `body` against the real assembled bundle, served from `hostname`."""
    transformed = DASHBOARD_JS.replace(
        "  void bootDashboard();",
        """  globalThis.__dashboardExports = {
    state,
    publicSiteOrigin,
    publicProfileUrl,
    cloneUrl,
    referralShareLink,
    repoApiBase,
  };""",
    )
    assert "__dashboardExports" in transformed, "export hook did not apply"
    script = (
        "const SOURCE = " + json.dumps(transformed) + ";\n"
        "const HOSTNAME = " + json.dumps(hostname) + ";\n" + """
const assert = require("assert");
const vm = require("vm");
global.location = {
  origin: "https://" + HOSTNAME,
  hostname: HOSTNAME,
  pathname: "/dashboard",
  search: "",
};
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: { classList: emptyClassList },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector() { return null; },
  querySelectorAll() { return []; },
  addEventListener() {},
};
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  history: { pushState() {} },
  lucide: { createIcons() {} },
  setTimeout() {},
  setInterval() {},
  clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
const {
  state,
  publicSiteOrigin,
  publicProfileUrl,
  cloneUrl,
  referralShareLink,
  repoApiBase,
} = global.__dashboardExports;
""" + body)
    result = subprocess.run(
        ["node"], input=script, check=True, capture_output=True, text=True)
    assert result.stderr == ""
    return result.stdout


@pytest.mark.parametrize("hostname", SERVICE_HOSTS)
def test_every_official_host_advertises_the_apex(hostname):
    _dashboard_under_host(hostname, """
assert.equal(publicSiteOrigin(), "https://forkmesh.com");
assert.equal(publicProfileUrl("jett"), "https://forkmesh.com/@jett");
state.repositories = [{
  owner: "forkmesh", name: "forkmesh", source: "local-node",
  cloneUrl: "https://forkmesh.com/forkmesh/forkmesh",
  rootCommit: "23f5d0c3", liveHost: true, cloneOnline: true, lastSync: 9000,
}];
assert.equal(
  cloneUrl(state.repositories[0]),
  "https://forkmesh.com/forkmesh/forkmesh",
);
""")


def test_the_referral_link_matches_what_the_verifier_accepts():
    # The regression: the page runs on app.forkmesh.com, the verifier only
    # accepts the apex spelling, so the user's own share never counted.
    out = _dashboard_under_host("app.forkmesh.com", """
state.session = { nodeName: "jett", kind: "user", email: "jett@example.com" };
console.log(referralShareLink());
""")
    assert out.strip() == "https://forkmesh.com/r/jett"

    # And the worker really does only accept that host: referral_link_variants
    # is built from one hostname, with forkmesh.com as the fallback.
    assert 'host = host or "forkmesh.com"' in MICROJOBS


def test_a_self_hosted_deployment_keeps_its_own_origin():
    # Only the official access layers collapse onto the apex. A self-hosted
    # relay must advertise itself, not forkmesh.com.
    _dashboard_under_host("git.example.test", """
assert.equal(publicSiteOrigin(), "https://git.example.test");
assert.equal(publicProfileUrl("jett"), "https://git.example.test/@jett");
""")


def test_routing_still_names_the_serving_address():
    # The apex rule covers what users read and copy. Reads keep resolving to
    # the repository's own API route -- unchanged by this pinning.
    _dashboard_under_host("app.forkmesh.com", """
state.repositories = [{
  owner: "forkmesh", name: "forkmesh", source: "local-node",
  cloneUrl: "https://forkmesh.com/forkmesh/forkmesh",
  rootCommit: "23f5d0c3", liveHost: true, cloneOnline: true, lastSync: 9000,
}];
assert.equal(
  repoApiBase(state.repositories[0]),
  "/api/repo/forkmesh/forkmesh",
);
""")


def test_no_shipped_client_builds_an_address_from_the_service_host():
    # location.origin is the subdomain the document was served from. Anything
    # user-facing must go through the canonical helper instead.
    leaks = [
        (name, line.strip())
        for name, source in (("dashboard.js", DASHBOARD_JS),
                             ("world.js", WORLD_JS))
        for line in source.splitlines()
        if "location.origin" in line
        and ("/mcp" in line or "/r/" in line or "/@" in line
             or "cloneUrl" in line)
    ]
    assert leaks == []


def test_the_world_client_advertises_the_apex_too():
    assert "const WORLD_PUBLIC_SITE_ORIGIN" in WORLD_JS
    assert "${WORLD_PUBLIC_SITE_ORIGIN}/@${encodeURIComponent(handle)}" in WORLD_JS
