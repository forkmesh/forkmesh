#!/usr/bin/env python3
"""Every repository read is addressed to the repo's public address, never to
the mirror that published the catalog row we resolved.

Production regression: the flagship's largest mirror group has no online
working-copy holder, so sourceOfTruth() picks the freshest source-of-truth row
in it — a long-retired node (mirror11) whose row still says source:"local-node"
while its cloneUrl points at forkmesh/forkmesh. The page linked correctly to
/forkmesh/forkmesh (adhoc #132) but repoApiBase kept naming the physical owner,
so the repo page fanned ~30 reads at /api/repo/mirror11/forkmesh/*. Only that
one node ever served that path, so every read answered 503
"mirror_unavailable" while three healthy mirrors served the identical bytes
under the public address.
"""

from pathlib import Path
import json
import subprocess

from _dashboard_bundle import assembled_dashboard_js


PUBLIC = Path(__file__).resolve().parents[1] / "public"
DASHBOARD_JS = assembled_dashboard_js()


def _run_with_exports(body):
    """Exercise the real assembled browser helpers under node."""
    transformed = DASHBOARD_JS.replace(
        "  void bootDashboard();",
        """  globalThis.__dashboardExports = {
    state,
    groupRepositories,
    sourceOfTruth,
    findRepository,
    repoApiBase,
    repoLinkUrl,
    repoPublicOwner,
    directMirrorOriginsForRepo,
    directRepoRequestCandidates,
    hydratePortableMirrorSession,
    ensurePortableControllerSession,
    fetchJson,
    readSession,
  };""",
    )
    assert "__dashboardExports" in transformed, "export hook did not apply"
    script = "const SOURCE = " + json.dumps(transformed) + ";\n" + """
const assert = require("assert");
const vm = require("vm");
global.location = {
  origin: "https://app.forkmesh.com",
  hostname: "app.forkmesh.com",
  pathname: "/dashboard",
  search: "",
};
const browserStorage = new Map();
global.localStorage = {
  getItem(key) { return browserStorage.has(key) ? browserStorage.get(key) : null; },
  setItem(key, value) { browserStorage.set(key, String(value)); },
  removeItem(key) { browserStorage.delete(key); },
};
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
  groupRepositories,
  sourceOfTruth,
  findRepository,
  repoApiBase,
  repoLinkUrl,
  repoPublicOwner,
  directMirrorOriginsForRepo,
  directRepoRequestCandidates,
  hydratePortableMirrorSession,
  ensurePortableControllerSession,
  fetchJson,
  readSession,
} = global.__dashboardExports;
""" + body
    result = subprocess.run(
        ["node"], input=script, check=True, capture_output=True, text=True)
    assert result.stderr == ""


# The shape of the live catalog's 27-member flagship group: healthy mirrors
# that are all remote clones, and the only source-of-truth rows are retired
# nodes. Every row's cloneUrl names the repository they are copies of.
_PRODUCTION_SHAPED_GROUP = """
const repos = [
  { owner: "mirror19", name: "forkmesh", source: "remote-clone",
    cloneUrl: "https://app.forkmesh.com/forkmesh/forkmesh",
    mirrorUrl: "https://mirror37.forkmesh.com/forkmesh/forkmesh.git",
    rootCommit: "23f5d0c3", liveHost: true, cloneOnline: true, lastSync: 9000 },
  { owner: "mirror18", name: "forkmesh", source: "remote-clone",
    cloneUrl: "https://app.forkmesh.com/forkmesh/forkmesh",
    mirrorUrl: "https://mirror36.forkmesh.com/forkmesh/forkmesh.git",
    rootCommit: "23f5d0c3", liveHost: true, cloneOnline: true, lastSync: 8000 },
  // Retired: offline for weeks, but still the group's only "local-node" row,
  // so sourceOfTruth() names it.
  { owner: "mirror11", name: "forkmesh", source: "local-node",
    cloneUrl: "https://forkmesh.com/forkmesh/forkmesh",
    rootCommit: "23f5d0c3", liveHost: false, cloneOnline: false, lastSync: 100 },
];
state.repositories = repos;
const origin = sourceOfTruth(groupRepositories(repos)[0]);
"""


def test_repo_reads_go_to_the_public_address_not_the_retired_mirror():
    _run_with_exports(_PRODUCTION_SHAPED_GROUP + """
// Precondition: the group really does resolve to the retired node's row.
assert.equal(origin.owner, "mirror11");

// The link and the fetch must name the same public address. Before the fix
// these disagreed: /forkmesh/forkmesh rendered, /api/repo/mirror11/forkmesh
// was read, and every read 503'd.
assert.equal(repoPublicOwner(origin), "forkmesh");
assert.equal(repoLinkUrl(origin), "/forkmesh/forkmesh");
assert.equal(repoApiBase(origin), "/api/repo/forkmesh/forkmesh");
assert(!repoApiBase(origin).includes("mirror11"));
""")


def test_a_mirror_addressed_visit_still_reads_the_public_address():
    # A direct visit to a mirror's own catalog address (an indexed link, or a
    # shared URL) resolves to the group and must not fan reads at that mirror.
    _run_with_exports(_PRODUCTION_SHAPED_GROUP + """
const resolved = findRepository("mirror11/forkmesh");
assert(resolved, "mirror address must still resolve to the repository");
assert.equal(repoApiBase(resolved), "/api/repo/forkmesh/forkmesh");
""")


def test_a_repo_with_no_public_alias_keeps_its_own_address():
    # No canonical identity to move to: a plainly owned repo must keep
    # addressing itself, or every read would 404.
    _run_with_exports("""
const repos = [
  { owner: "someone", name: "notes", source: "local-node",
    cloneUrl: "", rootCommit: "aaa", liveHost: true, lastSync: 1 },
];
state.repositories = repos;
const origin = sourceOfTruth(groupRepositories(repos)[0]);
assert.equal(origin.owner, "someone");
assert.equal(repoApiBase(origin), "/api/repo/someone/notes");
""")


def test_repo_list_logo_reads_the_public_address_too():
    # The list's logo endpoint used to build its own /api/repo/<owner>/ path
    # from the physical owner, bypassing repoApiBase entirely.
    logo = DASHBOARD_JS[
        DASHBOARD_JS.index("function nativeRepositoryLogoEndpoint(")
        : DASHBOARD_JS.index("function nativeRepositoryLogoEndpoint(") + 400
    ]
    assert "repoApiBase(repo)" in logo
    assert "/api/repo/${encodeURIComponent(owner)}" not in logo


def test_repo_reads_use_direct_mirror_origins_without_changing_public_path():
    _run_with_exports(_PRODUCTION_SHAPED_GROUP + """
assert.deepEqual(directMirrorOriginsForRepo(origin), [
  "https://mirror37.forkmesh.com",
  "https://mirror36.forkmesh.com",
]);
assert.deepEqual(
  directRepoRequestCandidates(
    "/api/repo/forkmesh/forkmesh/tree?path=app&ref=main&fmv=abc&viewer=alice",
  ),
  [
    "https://mirror37.forkmesh.com/api/repo/forkmesh/forkmesh/tree?path=app&ref=main",
    "https://mirror36.forkmesh.com/api/repo/forkmesh/forkmesh/tree?path=app&ref=main",
  ],
);
// Collaboration uses the same public repo path and direct origin selection.
assert.deepEqual(
  directRepoRequestCandidates(
    "/api/repo/forkmesh/forkmesh/issues", { mutation: true },
  ),
  [
    "https://mirror37.forkmesh.com/api/repo/forkmesh/forkmesh/issues",
    "https://mirror36.forkmesh.com/api/repo/forkmesh/forkmesh/issues",
  ],
);
assert.deepEqual(
  directRepoRequestCandidates("/api/repo/forkmesh/forkmesh/star"), [],
);

// Once the same page is opened directly, its local node is the first choice;
// the mirror remains usable without routing its own reads through a peer.
location.origin = "https://mirror36.forkmesh.com";
location.hostname = "mirror36.forkmesh.com";
assert.equal(
  directRepoRequestCandidates("/api/repo/forkmesh/forkmesh/tree")[0],
  "https://mirror36.forkmesh.com/api/repo/forkmesh/forkmesh/tree",
);
""")


def test_direct_mirror_hydrates_the_portable_login_before_page_boot():
    _run_with_exports("""
location.origin = "https://mirror37.forkmesh.com";
location.hostname = "mirror37.forkmesh.com";
let calls = 0;
global.fetch = async (url, options) => {
  calls += 1;
  assert.equal(url, "/api/accounts/sessions");
  assert.equal(options.credentials, "same-origin");
  assert.equal(options.cache, "no-store");
  assert(options.signal, "mirror session hydration must be time-bounded");
  return {
    ok: true,
    async json() {
      return {
        account: {
          nodeName: "jett",
          status: "active",
          emailVerified: true,
          kind: "user",
        },
        portable: true,
        controllerIndependent: true,
      };
    },
  };
};
(async () => {
  const session = await hydratePortableMirrorSession();
  assert.equal(calls, 1);
  assert.equal(session.nodeName, "jett");
  assert.equal(session.sessionToken, "cookie");
  assert.equal(session.controllerIndependent, true);
  assert.equal(readSession().nodeName, "jett");
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
""")


def test_official_app_does_not_add_a_portable_session_request():
    _run_with_exports("""
let calls = 0;
global.fetch = async () => { calls += 1; throw new Error("unexpected fetch"); };
(async () => {
  assert.equal(await hydratePortableMirrorSession(), null);
  assert.equal(calls, 0);
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
""")


def test_official_app_refreshes_a_preexisting_session_for_mirror_handoff_once():
    _run_with_exports("""
localStorage.setItem("forkmesh.session", JSON.stringify({
  nodeName: "jett",
  sessionToken: "cookie",
}));
let calls = 0;
global.fetch = async (url, options) => {
  calls += 1;
  assert.equal(url, "/api/accounts/sessions");
  assert.equal(options.credentials, "same-origin");
  assert.equal(options.cache, "no-store");
  return {
    ok: true,
    headers: { get(name) {
      return name === "x-forkmesh-portable-session" ? "issued" : null;
    } },
  };
};
(async () => {
  const refreshed = await ensurePortableControllerSession();
  assert.equal(calls, 1);
  assert.equal(refreshed.nodeName, "jett");
  assert(Number(refreshed.refreshedAt) > 0);
  assert.equal(await ensurePortableControllerSession(), null);
  assert.equal(calls, 1, "fresh portable grant must not create a poll");
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
""")


def test_official_app_refreshes_a_legacy_bearer_before_direct_navigation():
    _run_with_exports("""
localStorage.setItem("forkmesh.session", JSON.stringify({
  nodeName: "jett",
  sessionToken: "v2.legacy-session.legacy-secret",
}));
let captured = null;
global.fetch = async (url, options) => {
  captured = { url, options };
  return {
    ok: true,
    headers: { get(name) {
      return name === "x-forkmesh-portable-session" ? "issued" : null;
    } },
  };
};
(async () => {
  const refreshed = await ensurePortableControllerSession(true);
  assert.equal(captured.url, "/api/accounts/sessions");
  assert.equal(
    captured.options.headers.authorization,
    "Bearer v2.legacy-session.legacy-secret",
  );
  assert.equal(captured.options.credentials, "same-origin");
  assert(Number(refreshed.refreshedAt) > 0);
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
""")


def test_public_catalog_fetch_omits_session_credentials_for_fast_snapshot():
    _run_with_exports("""
state.session = { nodeName: "jett", sessionToken: "cookie" };
let captured = null;
global.fetch = async (url, options) => {
  captured = { url, options };
  return {
    ok: true,
    status: 200,
    async json() { return { ok: true, repositories: [] }; },
  };
};
(async () => {
  await fetchJson("/api/repositories");
  assert.equal(captured.url, "/api/repositories");
  assert.equal(captured.options.credentials, "omit");
  assert.equal(captured.options.headers.authorization, undefined);
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
""")


def test_repo_boot_uses_recent_catalog_before_background_refresh():
    dashboard = DASHBOARD_JS
    for marker in (
        'const REPOSITORY_CATALOG_STORAGE_MAX_MS = 5 * 60 * 1000;',
        'const stored = fresh ? null : readStoredRepositoryCatalog();',
        'renderRepositories(stored, state.session);',
        'void refreshRepositoryCatalog().catch(() => {});',
        'preconnectRememberedDirectMirrors();',
    ):
        assert marker in dashboard


def test_direct_mirror_discards_a_stale_origin_local_session():
    _run_with_exports("""
location.origin = "https://mirror37.forkmesh.com";
location.hostname = "mirror37.forkmesh.com";
localStorage.setItem("forkmesh.session", JSON.stringify({ nodeName: "old-user" }));
state.session = { nodeName: "old-user" };
global.fetch = async () => ({
  ok: false,
  status: 401,
  async json() { return { error: "unauthorized" }; },
});
(async () => {
  assert.equal(await hydratePortableMirrorSession(), null);
  assert.equal(readSession(), null);
  assert.equal(state.session, null);
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
""")
