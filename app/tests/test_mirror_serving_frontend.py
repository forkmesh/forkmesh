#!/usr/bin/env python3
"""Live dashboard surfaces in-place mirror serving (adhoc #61): a repo whose own
host is down but whose mirror is online must read as available ("served by
mirror"), not "host offline", on both the repo card and the repo detail page."""

from pathlib import Path
import json
import subprocess

from _dashboard_bundle import assembled_dashboard_js


PUBLIC = Path(__file__).resolve().parents[1] / "public"
# dashboard.js is built from ordered public/dashboard/js/*.js fragments before
# deploy (see src/dashboard_bundle.py).
DASHBOARD_JS = assembled_dashboard_js()


def test_dashboard_defines_group_liveness_helpers():
    # The availability verdict comes from the worker's group-liveness flag
    # (cloneOnline), falling back to liveHost for older payloads.
    assert "function repoIsLive(repo)" in DASHBOARD_JS
    assert "function repoServedByMirror(repo)" in DASHBOARD_JS
    assert "repo?.cloneOnline ?? repo?.liveHost" in DASHBOARD_JS


def test_repo_card_uses_group_liveness_not_raw_livehost():
    card = DASHBOARD_JS[
        DASHBOARD_JS.index("function repositoryCard(group)")
        : DASHBOARD_JS.index("function updateRepositoryPagination(")
    ]
    assert "const live = repoIsLive(origin);" in card
    assert "const viaMirror = repoServedByMirror(origin);" in card
    # A repo served by a live mirror reads as "via mirror" / "served by mirror",
    # never a flat "offline".
    assert "viaMirror && !onInstance ? \"via mirror\"" in card
    assert "viaMirror ? \"served by mirror\"" in card


def test_repo_card_groups_mirrors_under_source_of_truth():
    # The list shows one card per logical repo (grouped by root commit, name
    # fallback), named after the source of truth (the node that published from a
    # local working copy, source === "local-node"), not each mirror separately.
    assert "function groupRepositories(" in DASHBOARD_JS
    source_of_truth = DASHBOARD_JS[
        DASHBOARD_JS.index("function sourceOfTruth(group)")
        : DASHBOARD_JS.index("function sourceOfTruth(group)") + 500
    ]
    assert '(m.source || "").trim() === "local-node"' in source_of_truth
    assert "|| group.primary" in source_of_truth

    card = DASHBOARD_JS[
        DASHBOARD_JS.index("function repositoryCard(group)")
        : DASHBOARD_JS.index("function updateRepositoryPagination(")
    ]
    # Clone command and Browse target name the source of truth; the visible
    # title names that group's logical owner (organization/account), not the
    # machine the bytes happen to be published from.
    assert "const origin = sourceOfTruth(group);" in card
    assert "const key = repoKey(origin);" in card
    assert "const label = repoLabelParts(groupDisplayName(group));" in card
    assert "const displayOwner = label.owner;" in card
    assert 'escapeHtml(displayOwner)}/</span>' in card
    assert 'escapeHtml(origin.name || "repository")' in card
    assert "cloneUrl(origin)" in card
    # A grouped repo advertises how many nodes mirror it.
    assert "const nodeCount = group.members.length;" in card
    assert "${nodeCount} nodes" in card


def test_remote_clone_only_group_uses_clone_url_canonical_identity():
    # Exercise the actual assembled browser helpers: when the only rows are
    # mirror-owned remote clones, the card/detail URL identity is inferred from
    # cloneUrl while the mirror rows remain aliases for direct route lookup.
    transformed = DASHBOARD_JS.replace(
        "  void bootDashboard();",
        """  globalThis.__dashboardExports = {
    state,
    groupRepositories,
    sourceOfTruth,
    findRepository,
    repositoryCard,
    cloneUrl,
    repoPathUrl,
  };""",
    )
    script = "const SOURCE = " + json.dumps(transformed) + ";\n" + """
const assert = require("assert");
const vm = require("vm");
global.location = { origin: "https://forkmesh.test", hostname: "forkmesh.test", pathname: "/dashboard", search: "" };
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
  groupRepositories,
  sourceOfTruth,
  findRepository,
  repositoryCard,
  cloneUrl,
  repoPathUrl,
} = global.__dashboardExports;
const repos = [
  {
    owner: "mirror3",
    name: "forkmesh",
    source: "remote-clone",
    cloneUrl: "https://forkmesh.com/mainnode/forkmesh",
    rootCommit: "abc",
    liveHost: true,
    cloneOnline: true,
    lastSync: 2000,
  },
  {
    owner: "mirror4",
    name: "forkmesh",
    source: "remote-clone",
    cloneUrl: "https://forkmesh.com/mainnode/forkmesh.git",
    rootCommit: "abc",
    liveHost: true,
    cloneOnline: true,
    lastSync: 1000,
  },
];
state.repositories = repos;
const group = groupRepositories(repos)[0];
const origin = sourceOfTruth(group);
assert.equal(origin.owner, "mainnode");
assert.equal(origin.name, "forkmesh");
assert.equal(origin.liveHost, false);
assert.equal(origin.cloneOnline, true);
assert.equal(cloneUrl(origin), "https://forkmesh.test/mainnode/forkmesh");
assert.equal(repoPathUrl(origin), "/mainnode/forkmesh");
assert.equal(findRepository("mainnode/forkmesh").owner, "mainnode");
assert.equal(findRepository("mirror3/forkmesh").owner, "mainnode");
const card = repositoryCard(group);
assert(card.includes("mainnode/"));
assert(!card.includes("mirror3/</span>forkmesh"));
"""
    result = subprocess.run(
        ["node"],
        input=script,
        check=True,
        capture_output=True,
        text=True,
    )
    assert result.stderr == ""


def test_repo_list_paginates_grouped_repos_not_raw_mirrors():
    pager = DASHBOARD_JS[
        DASHBOARD_JS.index("function updateRepositoryPagination(")
        : DASHBOARD_JS.index("function updateRepositoryPagination(") + 800
    ]
    # Pagination and the count summary operate on grouped logical repos.
    assert "const total = state.filteredGroups.length;" in pager
    assert "const visible = state.filteredGroups.slice(start, end);" in pager


def test_repo_detail_reflects_mirror_serving():
    header = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderHeaderContext(")
        : DASHBOARD_JS.index("// ---- Public-profile mode")
    ]
    assert "const live = repoIsLive(repo);" in header
    assert "const viaMirror = repoServedByMirror(repo);" in header
    # The shared top-bar badge keys off the group verdict; the redundant title
    # row was removed from the repository content itself.
    assert 'repoAvailability.textContent = servedBy' in header
    assert 'const servedBy = String(state.repoServedBy?.name || "").trim();' in header
    assert '? "served by mirror"' in header
    assert '? "mirror online"' in header
    assert ': "mirror offline"' in header


def test_served_by_status_moves_to_the_repository_header():
    served_by = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoServedBy(node, tookMs)")
        : DASHBOARD_JS.index("function repoExplorerRowClass(")
    ]
    assert 'renderHeaderContext("explore")' in served_by
    assert "data-repo-served-by" not in served_by

    mirrors = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoMirrors(repo, options = {})")
        : DASHBOARD_JS.index("function renderRepoRelease(")
    ]
    # The tree/blob request may set servedBy before /mirrors has loaded; after
    # the mirror data arrives, refresh the shared header status.
    assert "renderRepoServedBy(state.repoServedBy.name, state.repoServedBy.tookMs)" in mirrors


def test_release_asset_links_preserve_the_manifest_filename():
    releases = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoRelease(")
        : DASHBOARD_JS.index("async function loadRepoReleases(")
    ]
    assert (
        "releases/blob/sha256/${encodeURIComponent(sha)}?filename="
        "${encodeURIComponent(name)}" in releases
    )
    assert 'download="${escapeHtml(name)}"' in releases


def test_live_mirror_rows_show_node_version_under_name():
    rows = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderMirrorRow(mirror, servedBy, refMirror)")
        : DASHBOARD_JS.index("function renderRepoLiveMirrorList(")
    ]
    assert "mirror.version || mirror.appVersion || mirror.clientVersion" in rows
    assert 'rawVersion[0].toLowerCase() === "v"' in rows
    assert 'class="min-w-0 truncate">${version ? escapeHtml(version)' in rows
    assert "version unknown" in rows
    # A machine cabinet/row is named by the node, never by the user account
    # that owns its source publication.
    assert "mirror.node || mirror.machineName || mirror.name" in rows
    assert "mirror.owner || mirror.node" not in rows


def test_live_mirror_rows_include_the_source_as_a_real_node():
    rows = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderMirrorRow(mirror, servedBy, refMirror)")
        : DASHBOARD_JS.index("function renderRepoLiveMirrorList(")
    ]
    assert "failing integrity pin" in rows
    assert 'mirror.integrity === "rejected"' in rows
    assert "source of truth" in rows
    assert "isSourceOfTruth" in rows

    summary = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoLiveMirrorList(")
        : DASHBOARD_JS.index("async function loadRepoMirrors(repo, options = {})")
    ]
    assert "renderRepoSourceOfTruth" not in summary
    assert "function repoNodesWithSource" in DASHBOARD_JS
    assert "repoNodesWithSource(" in DASHBOARD_JS
    assert "Number(b?.isSourceOfTruth === true)" in DASHBOARD_JS
    assert 'mirror?.source || ""' in DASHBOARD_JS
    assert '=== "local-node"' in DASHBOARD_JS
    assert "renderMirrorRow(mirror, servedBy, refMirror)" in summary


def test_live_mirror_rows_link_to_safe_direct_repository_dashboards():
    helper = DASHBOARD_JS[
        DASHBOARD_JS.index("function mirrorDirectDashboardUrl(mirror)")
        : DASHBOARD_JS.index("function renderRepoLiveMirrorList(")
    ]
    assert "mirror?.dashboardUrl" in helper
    assert "mirror?.mirrorUrl" in helper
    assert 'url.protocol !== "https:"' in helper
    assert "url.username || url.password" in helper
    assert 'url.pathname.endsWith(".git")' in helper
    assert "state.selectedRepo?.owner" in helper
    assert "state.selectedRepo?.name" in helper
    assert 'url.searchParams.set("theme", readDashboardTheme())' in helper
    assert '/api/accounts/mirror-handoff' not in helper
    assert 'return url.toString().replace(/\\/$/, "")' in helper
    assert "data-repo-direct-mirror" in helper
    assert 'rel="noopener noreferrer"' in helper
    assert 'target="_blank"' not in helper
    assert "Open direct" in helper

    click_handler = DASHBOARD_JS[
        DASHBOARD_JS.index('const directMirrorLink = event.target.closest(')
        : DASHBOARD_JS.index('const mobileMenuToggle = event.target.closest(')
    ]
    assert "await ensurePortableControllerSession(true);" in click_handler
    assert "location.assign(destination);" in click_handler
    assert 'new URL(destination)' in click_handler
    assert '/^mirror[0-9]+\\.forkmesh\\.com$/i' in click_handler

    aside = DASHBOARD_JS[
        DASHBOARD_JS.index("<div data-repo-live-summary")
        : DASHBOARD_JS.index("<div data-repo-live-summary") + 800
    ]
    assert "Direct access" in aside
    assert "If the controller is unavailable" in aside


def test_dashboard_mirror_refresh_is_socket_driven_without_a_fallback_poll():
    polling = DASHBOARD_JS[
        DASHBOARD_JS.index("const REPO_MIRROR_MIN_REFRESH_MS")
        : DASHBOARD_JS.index("function renderRepoRelease(")
    ]
    assert "const REPO_MIRROR_MIN_REFRESH_MS = 30 * 1000" in polling
    assert "const REPO_MIRROR_LIVE_REFRESH_MS" not in polling
    assert "const liveMirrorRefreshInterval = setInterval(" not in polling
    assert 'document.addEventListener("visibilitychange", refreshOpenRepoMirrors)' \
        not in polling
    assert "function repoMirrorsTabVisible()" in polling
    assert 'document.visibilityState === "visible"' in polling
    assert 'state.activeRepoTab === "mirrors"' in polling
    assert '!panel.classList.contains("hidden")' in polling
    signal = polling[
        polling.index("function refreshOpenRepoMirrors()")
        : polling.index(
            'window.addEventListener("forkmesh:mirror-signal"',
        )
    ]
    assert "repoMirrorsTabVisible()" in signal
    assert "{ background: true, force: true }" in signal
    assert "event?.detail?.catchUp === true" in signal
    assert "clearTimeout(liveMirrorRefreshTimer)" in signal
    assert "liveMirrorRefreshTimer = setTimeout(" in signal
    assert "liveMirrorConfirmTimer" not in polling

    tabs = DASHBOARD_JS[
        DASHBOARD_JS.index("function activateRepoTab(tab)")
        : DASHBOARD_JS.index("function repoPathParts(")
    ]
    assert 'if (tab === "mirrors")' in tabs
    assert "void loadRepoMirrors(state.selectedRepo, { background: true })" \
        in tabs


def test_repo_commit_age_prefers_exact_signed_mirror_timestamp():
    summary = DASHBOARD_JS[
        DASHBOARD_JS.index("function updateRepoCommitSummary(commit, repo)")
        : DASHBOARD_JS.index("function mdSafeUrl(")
    ]
    assert "matchingMirror" in summary
    assert "mirror?.lastCommitAt" in summary
    assert "mirrorCommit === hash.trim().toLowerCase()" in summary
    assert "const date = exactMirrorDate" in summary


def test_direct_https_mirror_reads_bypass_the_cached_fetchjson():
    # Everything the code tab (and commits/insights) reads through the masked
    # direct-HTTPS gateway must be fetched FRESH from whichever healthy mirror
    # the router round-robins to,
    # exactly like the issue/pull/discussion/release/README readers already do.
    # The cached, account-scoped fetchJson (cache:"default", inflight dedup,
    # bearer token) could otherwise pin the page to a stale copy from when the
    # source-of-truth host was online, or a different mirror answered — so a repo
    # served entirely by its mirrors would render stale/among rotating nodes.
    # fetchRepoJson is the no-store live reader; assert the browse surfaces use
    # it and no direct-gateway path is left on fetchJson.
    fresh_reads = (
        'fetchRepoJson(repoLiveUrl(repo, "tree"',
        'fetchRepoJson(repoLiveUrl(repo, "blob"',
        'fetchRepoJson(repoLiveUrl(repo, "history"))',
        'fetchRepoJson(repoLiveUrl(repo, "commit"',
        "fetchRepoJson(`${repoApiBase(repo)}/branches`)",
        "fetchRepoJson(`${repoApiBase(repo)}/about`)",
        "fetchRepoJson(`${repoApiBase(repo)}/mirrors`)",
    )
    for read in fresh_reads:
        assert read in DASHBOARD_JS, "missing fresh live read: %s" % read
    # No direct-gateway browse read may fall back to cached fetchJson. Public
    # about and mirror-membership projections are direct too; release download
    # counters and the global catalog remain controller metadata.
    assert "fetchJson(repoLiveUrl(" not in DASHBOARD_JS
    assert "fetchJson(`${repoApiBase(repo)}/branches`)" not in DASHBOARD_JS
    assert "fetchJson(`${repoApiBase(repo)}/about`)" not in DASHBOARD_JS
    assert "fetchJson(`${repoApiBase(repo)}/mirrors`)" not in DASHBOARD_JS


def test_live_serving_header_is_injected_and_cache_hits_clear_body_claims():
    fragment = (
        PUBLIC / "dashboard" / "js" / "06-repo-content.js"
    ).read_text(encoding="utf-8")
    function_source = fragment[
        fragment.index("  async function fetchRepoJson("):
        fragment.index("\n\n  const PULL_METADATA_BRANCH")
    ]
    script = """
const assert = require("assert");
function directRepoRequestCandidates() { return []; }
%s
const queue = [
  {
    ok: true,
    status: 200,
    headers: { get(name) {
      return name.toLowerCase() === "x-forkmesh-served-by" ? "mirror2" : null;
    } },
    async json() { return { ok: true, servedBy: "untrusted-body" }; },
  },
  {
    ok: true,
    status: 200,
    headers: { get() { return null; } },
    async json() { return { ok: true, servedBy: "stale-cache-claim" }; },
  },
];
global.fetch = async () => queue.shift();
(async () => {
  const live = await fetchRepoJson("/live");
  assert.equal(live.servedBy, "mirror2");
  const cached = await fetchRepoJson("/cached");
  assert.equal(Object.prototype.hasOwnProperty.call(cached, "servedBy"), false);
})().catch((error) => {
  process.stderr.write(String(error && error.stack || error));
  process.exit(1);
});
""" % function_source
    result = subprocess.run(
        ["node"],
        input=script,
        check=True,
        capture_output=True,
        text=True,
    )
    assert result.stderr == ""


def test_offline_commits_render_terminal_state_and_abort_a_stuck_fetch():
    fetcher = DASHBOARD_JS[
        DASHBOARD_JS.index("  async function fetchRepoJson("):
        DASHBOARD_JS.index("\n\n  const PULL_METADATA_BRANCH")
    ]
    commits = DASHBOARD_JS[
        DASHBOARD_JS.index("  const REPO_COMMITS_FETCH_TIMEOUT_MS"):
        DASHBOARD_JS.index("\n\n  function renderRepoCommitFiles")
    ]
    script = """
    const assert = require("assert");
    function directRepoRequestCandidates() { return []; }
    """ + fetcher + "\n" + commits + """
const container = { innerHTML: "" };
const state = { repoHistoryPath: "" };
let aborted = false;
let cleared = false;
function $(selector) {
  assert.equal(selector, "[data-repo-commits]");
  return container;
}
function repoIsLive(repo) { return Boolean(repo.cloneOnline); }
function repoKey(repo) { return "acme/site"; }
function repoLiveUrl() { return "/history"; }
function loadingHtml(text) { return text; }
function escapeHtml(value) { return String(value); }
global.fetch = (_path, options) => new Promise((_resolve, reject) => {
  options.signal.addEventListener("abort", () => {
    aborted = true;
    reject(new Error("aborted"));
  });
});
global.window = {
  setTimeout(callback, timeout) {
    assert.equal(timeout, REPO_COMMITS_FETCH_TIMEOUT_MS);
    queueMicrotask(callback);
    return 7;
  },
  clearTimeout(timer) {
    assert.equal(timer, 7);
    cleared = true;
  },
  lucide: { createIcons() {} },
};
(async () => {
  const loading = loadRepoCommits({ cloneOnline: false });
  assert.match(container.innerHTML, /reachable mirror host/);
  assert.doesNotMatch(container.innerHTML, /Loading commits/);
  await loading;
  assert.equal(aborted, true);
  assert.equal(cleared, true);
  assert.match(container.innerHTML, /reachable mirror host/);
})().catch((error) => {
  process.stderr.write(String(error && error.stack || error));
  process.exit(1);
});
"""
    result = subprocess.run(
        ["node"],
        input=script,
        check=True,
        capture_output=True,
        text=True,
    )
    assert result.stderr == ""


def test_offline_discussions_hide_catalog_badge_and_explain_unavailable_count():
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("  function renderRepoDetail(repo)"):
        DASHBOARD_JS.index("\n\n  function findRepository(key)")
    ]
    assert "!repoIsLive(repo) || !Number.isFinite(discussionCount)" in render

    loader = DASHBOARD_JS[
        DASHBOARD_JS.index("  function discussionsUnavailableHtml(repo)"):
        DASHBOARD_JS.index("\n\n  // True when the logged-in account")
    ]
    script = """
const assert = require("assert");
""" + loader + """
const container = { innerHTML: "" };
const tabCounts = [];
const state = { repoCollectionItems: { discussions: null } };
const repoCollectionConfig = {
  discussions: { label: "Discussions", dir: ".forkmesh/discussions" },
};
let loadResult = "reject";
function $(selector) {
  assert.equal(selector, "[data-repo-discussions]");
  return container;
}
function repoCount(repo, keys) {
  for (const key of keys) {
    const value = Number(repo[key]);
    if (Number.isFinite(value) && value >= 0) return value;
  }
  return null;
}
function repoIsLive(repo) { return Boolean(repo.cloneOnline); }
function formatCount(value) { return String(value); }
function loadingHtml(value) { return value; }
function escapeHtml(value) { return String(value); }
function setRepoTabCount(tab, count) { tabCounts.push([tab, count]); }
async function primeRepoViewReads() {}
function repoViewKey(repo, view) { return view; }
function repoStaleReadMark() { return 0; }
function repoReconnectActive() { return false; }
function finishRepoRead() {}
function handleRepoReadFailure() { return false; }
function setRepoCollectionCounts() {}
function renderRepoCollection() { container.innerHTML = "rendered discussions"; }
function setRepoRecordOpen() {}
async function loadRepoRecordsFromMirror() {
  if (loadResult === "reject") throw new Error("mirror_unavailable");
  return loadResult;
}
global.window = { lucide: { createIcons() {} } };

(async () => {
  const repo = { cloneOnline: false, discussionCount: 1 };
  const unavailable = loadRepoCollection(
    repo, "discussions", "[data-repo-discussions]",
  );
  assert.deepEqual(tabCounts.at(-1), ["discussions", 0]);
  assert.match(container.innerHTML, /catalog reports 1 discussion/);
  assert.match(container.innerHTML, /cannot be fetched because no mirror is reachable/);
  await unavailable;
  assert.deepEqual(tabCounts.at(-1), ["discussions", 0]);

  loadResult = [{ number: 2 }, { number: 1 }];
  repo.cloneOnline = true;
  await loadRepoCollection(repo, "discussions", "[data-repo-discussions]");
  assert.equal(container.innerHTML, "rendered discussions");
  assert.deepEqual(tabCounts.at(-1), ["discussions", 2]);

  loadResult = [];
  await loadRepoCollection(repo, "discussions", "[data-repo-discussions]");
  assert.deepEqual(tabCounts.at(-1), ["discussions", 0]);
})().catch((error) => {
  process.stderr.write(String(error && error.stack || error));
  process.exit(1);
});
"""
    result = subprocess.run(
        ["node"],
        input=script,
        check=True,
        capture_output=True,
        text=True,
    )
    assert result.stderr == ""
