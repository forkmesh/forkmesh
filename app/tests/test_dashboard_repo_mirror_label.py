import json
import subprocess
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
EXPLORER = (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8")
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
DETAIL = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
ISSUES_IO = (JS / "03-issues-profile-io.js").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
BOOT = "  void bootDashboard();"
BROWSER_STUBS = """
const assert = require("assert");
const vm = require("vm");
global.location = {
  origin: "https://git.example.test",
  hostname: "git.example.test",
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
"""


def _slice(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def _run(exports, body):
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + exports + " };")
    assert transformed != DASHBOARD_JS
    script = "const SOURCE = " + json.dumps(transformed) + ";\n" + BROWSER_STUBS + body
    result = subprocess.run(["node"], input=script, check=True, capture_output=True, text=True)
    assert result.stderr == ""
    return result.stdout


def test_one_provenance_line_serves_forks_and_mirrors():
    output = _run(
        "repoProvenanceHtml, repoReadOnlyMirror, rememberRepoMirrorAbout, repoBranchManageAllowed, state",
        """
const { repoProvenanceHtml, repoReadOnlyMirror, rememberRepoMirrorAbout, repoBranchManageAllowed, state } =
  global.__dashboardExports;
const plain = { owner: "alice", name: "project" };
const fork = { owner: "bob", name: "project", forkedFrom: "alice/project" };
const mirror = {
  owner: "forkmesh", name: "forkmesh",
  mirrorOf: "https://forkmesh.com/forkmesh/forkmesh",
  lastSyncedAt: String(Date.now() - 3 * 60000),
  upstreamCommit: "0123456789abcdef",
};
console.log(JSON.stringify({
  plain: repoProvenanceHtml(plain),
  fork: repoProvenanceHtml(fork),
  mirror: repoProvenanceHtml(mirror),
  plainReadOnly: repoReadOnlyMirror(plain),
  forkReadOnly: repoReadOnlyMirror(fork),
  mirrorReadOnly: repoReadOnlyMirror(mirror),
}));
const lazy = { owner: "forkmesh", name: "forkmesh" };
state.repoBranchManage["forkmesh/forkmesh"] = true;
const before = repoBranchManageAllowed(lazy);
const changed = rememberRepoMirrorAbout(lazy, {
  ok: true, mirrorOf: "https://forkmesh.com/forkmesh/forkmesh", readOnly: true,
  lastSyncedAt: Date.now(), upstreamCommit: "abc",
});
const unchanged = rememberRepoMirrorAbout({ owner: "a", name: "b" }, { ok: true, mirrorOf: "", readOnly: false });
console.log(JSON.stringify({ before, changed, unchanged, after: repoBranchManageAllowed(lazy), lazy: repoReadOnlyMirror(lazy) }));
""",
    )
    first, second = [json.loads(line) for line in output.strip().splitlines()]
    assert first["plain"] == ""
    assert not first["plainReadOnly"] and not first["forkReadOnly"] and first["mirrorReadOnly"]
    fork = first["fork"]
    mirror = first["mirror"]
    assert 'data-repo-provenance-line="fork"' in fork
    assert "forked from" in fork and ">alice/project</a>" in fork
    assert 'href="/alice/project"' in fork
    assert 'data-repo-provenance-line="mirror"' in mirror
    assert ">Mirror of</span>" in mirror
    assert 'href="https://forkmesh.com/forkmesh/forkmesh" target="_blank" rel="noopener"' in mirror
    assert ">forkmesh.com/forkmesh/forkmesh</a>" in mirror
    assert mirror.index("Mirror of") < mirror.index("· synced 3 minutes ago") < mirror.index("data-repo-mirror-notice")
    for html in (fork, mirror):
        assert 'class="inline-flex min-w-0 items-center gap-1.5"' in html
        assert "dashboard-accent-link" in html
        assert "text-muted-foreground" in html
    assert "data-repo-mirror-synced" in mirror and "· synced 3 minutes ago" in mirror
    assert "data-repo-mirror-notice" in mirror and "Read-only" in mirror and "upstream</a>" in mirror
    assert "data-repo-mirror-fork" in mirror and 'data-lucide="git-fork"' in mirror
    assert "fm-btn fm-btn-sm" in mirror and "fm-btn-cta" not in mirror and "bg-cta" not in mirror
    assert "data-repo-mirror-fork" not in fork and "synced" not in fork
    assert second == {"before": True, "changed": True, "unchanged": False, "after": False, "lazy": True}


def test_provenance_sits_above_the_latest_commit_row_not_in_the_header():
    header = _slice(DETAIL, "<div data-repo-github-header", 'role="tablist"')
    assert "repoProvenanceHtml(repo)" not in header
    assert "data-repo-provenance" not in header
    code = _slice(DETAIL, "<div data-repo-code-main", "<div data-repo-commit-summary")
    assert "repoProvenanceHtml(repo)" in code
    assert "<div data-repo-provenance" in code
    assert code.index("data-repo-provenance") < code.index("data-repo-tree-panel")


def test_about_response_marks_the_mirror_and_regates_rendered_controls():
    about = _slice(CONTENT, "async function loadRepoFediverse(", "applyRepoFeatures(body.features);")
    assert "rememberRepoMirrorAbout(item, body)" in about
    assert "applyRepoReadOnlyMirror(state.selectedRepo)" in about
    apply = _slice(DETAIL, "function applyRepoReadOnlyMirror(", "function renderRepoDetail(")
    assert "repoProvenanceHtml(repo)" in apply
    assert '[data-repo-file-action="edit"]' in apply
    assert "[data-repo-pull-new], [data-repo-compare-pull]" in apply
    assert "renderRepoBranchesList(repo)" in apply
    assert "renderRepoCompareMergePanel(repo)" in apply


def test_fork_button_opens_the_existing_fork_popover():
    opener = _slice(DETAIL, "function openRepoForkMenuFromMirror(", "function applyRepoReadOnlyMirror(")
    assert "setRepoForkMenuOpen(true)" in opener
    assert "[data-repo-fork-wrap]" in opener and "scrollIntoView" in opener
    assert "[data-repo-fork-create]" in opener
    click = _slice(DETAIL, '"[data-repo-mirror-fork]"', '"[data-repo-remote-fork-remove]"')
    assert "openRepoForkMenuFromMirror()" in click


def test_write_actions_are_gated_by_the_mirror_helper():
    manage = _slice(COMPOSE, "function repoBranchManageAllowed(", "function repoBranchBusy(")
    assert "!repoReadOnlyMirror(repo)" in manage
    merge = _slice(COMPOSE, "async function openRepoBranchMergeDialog(", "const dialog =")
    assert "repoReadOnlyMirror(repo)" in merge
    pulls = _slice(COMPOSE, "data-repo-pull-new class=", "New pull request</button>")
    assert "repoReadOnlyMirror(repo)" in pulls
    compare = _slice(COMPOSE, "data-repo-compare-pull class=", "Create pull request</button>")
    assert "repoReadOnlyMirror(repo)" in compare
    edit = _slice(CONTENT, 'data-repo-file-action="edit"', "</button>")
    assert "repoReadOnlyMirror(repo)" in edit and " disabled" in edit
    handler = _slice(EXPLORER, 'action === "edit"', "openRepoFileEditor(viewer")
    assert "repoReadOnlyMirror(repo)" in handler
    pull_click = _slice(DETAIL, 'closest("[data-repo-pull-new]")', "openPullCompose(")
    assert "repoReadOnlyMirror(state.selectedRepo)" in pull_click
    compare_click = _slice(DETAIL, 'closest("[data-repo-compare-pull]")', "location.href")
    assert "repoReadOnlyMirror(state.selectedRepo)" in compare_click


def test_read_only_refusals_show_the_server_message():
    assert 'error?.code === "read_only_mirror"' in _slice(
        EXPLORER, "function repoFileEditError(", "function repoFileSignInUrl(")
    assert 'code === "read_only_mirror"' in _slice(
        COMPOSE, "function webPullSubmitErrorText(", "async function handleIssueComposeSubmit(")
    assert 'error?.code === "read_only_mirror" && error?.data?.message' in COMPOSE
    assert "read_only_mirror: payload?.message" in CONTENT
    assert "error.data = data;" in _slice(CONTENT, "async function postRepoMutation(", "async function fetchRepoMutation(")
    assert "error.data = data;" in ISSUES_IO
    assert "function repoReadOnlyMirror(" in HELPERS


def test_single_org_fork_line_names_the_parent_by_its_short_address():
    output = _run(
        "repoProvenanceHtml, applyRepoAddresses, state",
        """
const { repoProvenanceHtml, applyRepoAddresses, state } = global.__dashboardExports;
const fork = { owner: "bob", name: "platform-fork", forkedFrom: "acme/platform" };
const outside = { owner: "bob", name: "other", forkedFrom: "alice/project" };
applyRepoAddresses({ owner: "acme", canonical: true });
console.log(JSON.stringify({ fork: repoProvenanceHtml(fork), outside: repoProvenanceHtml(outside) }));
""",
    )
    result = json.loads(output.strip().splitlines()[-1])
    assert ">platform</a>" in result["fork"]
    assert ">acme/platform</a>" not in result["fork"]
    assert 'href="/platform"' in result["fork"]
    assert ">alice/project</a>" in result["outside"]


def test_a_mirror_without_a_published_description_names_its_upstream():
    output = _run(
        "repoDescriptionText",
        """
const { repoDescriptionText } = global.__dashboardExports;
const mirror = { owner: "node", name: "platform", mirrorOf: "https://app.example.test/acme/platform" };
console.log(JSON.stringify({
  bare: repoDescriptionText(mirror),
  own: repoDescriptionText({ ...mirror, description: "  Own words  " }),
  grouped: repoDescriptionText({ owner: "a", name: "b" }, { ...mirror, description: "Origin words" }),
  plain: repoDescriptionText({ owner: "a", name: "b" }),
}));
""",
    )
    result = json.loads(output.strip().splitlines()[-1])
    assert result == {
        "bare": "Read-only mirror of example.test/acme/platform",
        "own": "Own words",
        "grouped": "Origin words",
        "plain": "",
    }
    assert 'repoDescriptionText(repo, origin) || "No description published."' in EXPLORER
    assert 'repoDescriptionText(repo) || "No description published."' in DETAIL
