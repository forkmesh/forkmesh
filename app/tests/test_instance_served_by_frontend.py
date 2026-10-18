import json
import shutil
import subprocess

import pytest

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def _slice(start, end):
    return DASHBOARD_JS[DASHBOARD_JS.index(start):DASHBOARD_JS.index(end)]


HEADER = _slice("function renderHeaderContext(", "// ---- Public-profile mode")
CARD = _slice("function repositoryCard(group)", "function updateRepositoryPagination(")
HOME_ROW = _slice("function homeRepositoryRow(entry)", "function renderHomeRepositories()")
MIRRORS = _slice("async function loadRepoMirrors(repo, options = {})", "function renderRepoRelease(")

_HARNESS = r"""
const vm = require("vm");
const source = process.argv[1];
const cases = JSON.parse(process.argv[2]);
const out = [];
for (const item of cases) {
  const classes = new Set(["inline-flex"]);
  const pill = {
    textContent: "stale",
    classList: {
      add: (...names) => names.forEach((n) => classes.add(n)),
      remove: (...names) => names.forEach((n) => classes.delete(n)),
      toggle: (n, on) => (on ? classes.add(n) : classes.delete(n)),
    },
  };
  const blank = () => ({ innerHTML: "", textContent: "", classList: { add() {}, remove() {}, toggle() {} } });
  const nodes = {
    "[data-dashboard-header-context]": blank(),
    "[data-repo-header-meta]": blank(),
    "[data-repo-header-visibility]": blank(),
    "[data-repo-header-terms]": blank(),
    "[data-repo-availability-status]": pill,
  };
  const sandbox = {
    $: (sel) => nodes[sel] || null,
    state: { selectedRepo: item.repo, repoServedBy: item.servedBy, session: null },
    dashboardOnInstance: () => item.instance,
    repoIsLive: (repo) => Boolean(repo.cloneOnline ?? repo.liveHost),
    repoServedByMirror: (repo) => Boolean(repo.cloneOnline ?? repo.liveHost) && !repo.liveHost,
    repoDisplayKey: (repo) => repo.owner + "/" + repo.name,
    restoreDocumentTitle: () => {},
    repoDocumentTitle: (label) => label,
    document: { title: "" },
    repoLinkUrl: () => "/r",
    publicProfileUrl: () => "/p",
    escapeHtml: (v) => String(v),
    repositoryTermsBadge: () => "",
    currentSection: () => "explore",
    String,
  };
  vm.createContext(sandbox);
  vm.runInContext(source + "\nrenderHeaderContext('explore');", sandbox);
  out.push({ text: pill.textContent, hidden: classes.has("hidden") });
}
console.log(JSON.stringify(out));
"""


def _run(cases):
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is unavailable")
    result = subprocess.run(
        [node, "-e", _HARNESS, HEADER, json.dumps(cases)],
        capture_output=True, text=True, timeout=30, check=True)
    return json.loads(result.stdout)


REPO = {"owner": "acme-bolt", "name": "Bolt", "liveHost": True, "cloneOnline": True}


def test_header_served_by_pill_is_hidden_on_an_instance():
    instance, forkmesh_com = _run([
        {"repo": REPO, "servedBy": {"name": "forkmesh", "tookMs": 12}, "instance": True},
        {"repo": REPO, "servedBy": {"name": "forkmesh", "tookMs": 12}, "instance": False},
    ])
    assert instance == {"text": "", "hidden": True}
    assert forkmesh_com == {"text": "served by forkmesh", "hidden": False}


def test_header_pill_returns_when_the_same_page_is_not_an_instance():
    [mirror] = _run([
        {"repo": {**REPO, "liveHost": False}, "servedBy": None, "instance": False},
    ])
    assert mirror == {"text": "served by mirror", "hidden": False}


def test_repo_card_drops_the_mirror_label_on_an_instance():
    assert "const onInstance = dashboardOnInstance();" in CARD
    assert '${onInstance ? "" : `<span data-repo-card-served-by' in CARD
    assert 'viaMirror && !onInstance ? "via mirror"' in CARD


def test_home_repository_row_drops_the_serving_status_on_an_instance():
    assert 'const statusHtml = dashboardOnInstance() ? "" :' in HOME_ROW
    assert "data-home-repo-served-by" in HOME_ROW
    assert "${statusHtml}" in HOME_ROW


def test_mirror_load_leaves_the_hidden_pill_alone_on_an_instance():
    assert "if (availability && !dashboardOnInstance()) {" in MIRRORS


def test_about_rail_omits_the_direct_access_mirror_list_on_an_instance():
    assert '${dashboardOnInstance() ? "" : `<div data-repo-live-summary' in DASHBOARD_JS
