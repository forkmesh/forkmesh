"""Merge box, branch update, ledger re-pinning and repository tab strip."""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
CONTENT = (ROOT / "app/public/dashboard/js/06-repo-content.js").read_text(encoding="utf-8")
NETWORK = (ROOT / "app/public/dashboard/js/08-repo-detail-network.js").read_text(encoding="utf-8")
EXPLORER = (ROOT / "app/public/dashboard/js/05-repo-list-explorer.js").read_text(encoding="utf-8")
SHELL = (ROOT / "app/public/dashboard/shell.html").read_text(encoding="utf-8")
ROUTES = (ROOT / "app/src/route_handlers.py").read_text(encoding="utf-8")
SSR = (ROOT / "app/host/forkmesh_host/repo_ssr.py").read_text(encoding="utf-8")
DOCS = (ROOT / "docs/app/pull-merge-box.md").read_text(encoding="utf-8")

needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is required")

BASE = "2" * 40
HEAD = "3" * 40
NEW_HEAD = "a" * 40
TIP_A = "5" * 40
TIP_B = "6" * 40


def _function(name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), CONTENT)
    assert match, name
    end = CONTENT.index("\n  }\n", match.start())
    return CONTENT[match.start():end + 4]


def _between(start, end):
    first = CONTENT.index(start)
    return CONTENT[first:CONTENT.index(end, first)]


def _harness():
    parts = [
        _function("immutableGitCommit"),
        _function("pullReviewPolicy"),
        _function("pullHeadBranchRef"),
        _function("repoPullMergeContext"),
        _function("repoPullHasPatchPayload"),
        _between("  const PULL_CHECK_STATES = {", "  function renderRepoPullChecks("),
        _between("  function repoPullUpdateContext(", "  // Owner-authored contributor reward estimate."),
    ]
    return "\n".join(parts)


PRELUDE = r"""
const REPO_READ_ONLY_MIRROR_HINT = "read only";
const formatCount = (value) => String(Number(value) || 0);
const escapeHtml = (value) => String(value ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const repoLinkUrl = () => "/alice/widgets";
const repoRecordUrl = (repo, kind, number, section) => `/alice/widgets/pull/${number}${section === "conversation" ? "" : `/${section}`}`;
const repoApiBase = () => "/api/repo/alice/widgets";
const repoCommitUrl = (repo, hash) => `/alice/widgets/commits/${hash}`;
const repoLiveUrl = (repo, action, params) => `/api/repo/alice/widgets/${action}?${new URLSearchParams(params)}`;
const blobText = (blob) => String(blob?.content || "");
const parseFrontMatter = (text) => {
  const values = {};
  const match = /^---\n([\s\S]*?)\n---\n?([\s\S]*)$/.exec(text);
  (match ? match[1] : "").split("\n").forEach((line) => {
    const colon = line.indexOf(":");
    if (colon > 0) values[line.slice(0, colon).trim()] = line.slice(colon + 1).trim();
  });
  return { values, body: match ? match[2].trim() : text };
};
const buildRepoPullTimeline = () => [];
let requestCounter = 0;
const createRepoPullMergeRequestId = () => `dashboard_${String(++requestCounter).padStart(4, "0")}`;
const pullDetailIsCurrent = () => true;
const refreshed = [];
const refreshRepoPullActivity = (repo, number) => refreshed.push(number);
const orgAgentRequest = async () => ({ ok: true });
globalThis.CSS = { escape: (value) => value };
globalThis.location = { origin: "https://forkmesh.test", search: "" };
globalThis.document = { querySelector: () => null };
let confirmAnswer = true;
globalThis.window = {
  setTimeout: (resolve) => resolve(),
  confirm: () => confirmAnswer,
  lucide: null,
};
globalThis.state = { session: { sessionToken: "token", nodeName: "alice" }, repoRecordDetail: null };

const world = {
  tips: [],
  record: null,
  readiness: null,
  conversation: [],
  replies: [],
  sent: [],
};
async function resolveRepoPullMetadataCommit(repo, options = {}) {
  if (!options.fresh) throw new Error("the merge pins must be read fresh");
  return world.tips.length > 1 ? world.tips.shift() : world.tips[0];
}
async function fetchRepoJson(url) {
  if (url.includes("pull.md")) return { content: world.record };
  throw Object.assign(new Error("not_found"), { status: 404, code: "not_found" });
}
async function loadRepoPullPatch() { return null; }
async function loadRepoPullConversation() { return world.conversation; }
async function loadRepoPullCommits() { return []; }
async function loadRepoPullReadiness(repo, number, values, options = {}) {
  if (!options.fresh) throw new Error("readiness must be re-tried fresh");
  return typeof world.readiness === "function" ? world.readiness() : world.readiness;
}
async function loadRepoPullMergeBase() { return { available: false }; }
globalThis.fetch = async (url, init) => {
  const body = JSON.parse(init.body);
  world.sent.push({ url, body });
  const reply = world.replies.shift() || ["merged"];
  const ok = (payload) => ({ ok: true, status: 200, json: async () => payload });
  if (reply[0] === "merged" || reply[0] === "updated") {
    return ok({
      ok: true, status: reply[0], published: true, requestId: body.requestId,
      baseBefore: body.expectedBaseOid, head: body.expectedHeadOid,
      pullsBefore: body.expectedPullsOid, baseAfter: "8".repeat(40),
      pullsAfter: "9".repeat(40), headAfter: "c".repeat(40),
    });
  }
  return { ok: false, status: reply[1] || 409, json: async () => ({ ok: false, status: "failed", error: reply[0], requestId: body.requestId }) };
};

const BASE = "%(base)s";
const HEAD = "%(head)s";
function pullMd(extra = "") {
  return `---\nschema: forkmesh-pull-v1\nnumber: 7\ntitle: Leaderboard\nbase: main\nhead: bob/leaderboard\nstatus: open\nauthor: bob-key\nauthorName: bob\nderive: branch\ncreationBaseOid: ${"4".repeat(40)}\ncreationHeadOid: ${HEAD}\n${extra}---\n\nBody\n`;
}
function readiness(overrides = {}) {
  return { available: true, baseOid: BASE, headOid: HEAD, mergeBaseOid: "4".repeat(40), ahead: 1, behind: 0, clean: true, conflicts: [], ...overrides };
}
const approval = { type: "review", author: "carol-key", state: "approved" };
function detail(overrides = {}) {
  const parsed = {
    values: parseFrontMatter(pullMd()).values,
    pullMetadataCommit: "%(tip_a)s",
    pullReadiness: readiness(),
    pullMergeBase: { available: false },
    pullConversation: [approval],
    pullCommits: [],
    pullMergeAuthorized: true,
    ...overrides,
  };
  state.repoRecordDetail = { repo: { owner: "alice", name: "widgets" }, kind: "pulls", number: 7, parsed };
  return parsed;
}
""" % {"base": BASE, "head": HEAD, "tip_a": TIP_A}


def _run(script):
    source = PRELUDE + _harness() + "\n(async () => {\n" + script + "\n})().catch((error) => { console.error(error); process.exit(1); });"
    result = subprocess.run(
        ["node", "--input-type=module", "-e", source],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@needs_node
def test_a_moved_ledger_is_re_pinned_and_the_merge_retried_once_with_a_new_request():
    # The owner's report: the pull's author commented a minute before the
    # merge, forkmesh/pulls moved, and the mirror answered stale_pull_metadata.
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail();
      world.record = pullMd();
      world.readiness = readiness();
      world.conversation = [approval];
      world.tips = ["%s", "%s"];
      world.replies = [["stale_pull_metadata"], ["merged"]];
      await handleRepoPullMerge(repo);
      console.log(JSON.stringify({ state: parsed.pullMerge.state, sent: world.sent.map((item) => item.body), pin: parsed.pullMetadataCommit, refreshed }));
    """ % (TIP_A, TIP_B))
    first, second = out["sent"]
    assert out["state"] == "merged"
    assert first["expectedPullsOid"] == TIP_A
    assert second["expectedPullsOid"] == TIP_B
    assert first["requestId"] != second["requestId"]
    assert first["expectedHeadOid"] == second["expectedHeadOid"] == HEAD
    assert "overrideReview" not in first and "overrideReview" not in second
    assert out["pin"] == TIP_B
    assert out["refreshed"] == [7]


@needs_node
def test_the_preflight_pins_the_served_tip_so_a_stale_page_does_not_fail_at_all():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail();
      world.record = pullMd();
      world.readiness = readiness();
      world.conversation = [approval];
      world.tips = ["%s"];
      world.replies = [["merged"]];
      await handleRepoPullMerge(repo);
      console.log(JSON.stringify({ state: parsed.pullMerge.state, sent: world.sent.map((item) => item.body.expectedPullsOid) }));
    """ % TIP_B)
    assert out == {"state": "merged", "sent": [TIP_B]}


@needs_node
def test_a_genuine_concurrent_change_stops_and_says_what_changed():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail();
      world.record = pullMd();
      world.conversation = [approval];
      world.tips = ["%s"];
      world.readiness = readiness({ headOid: "%s" });
      await handleRepoPullMerge(repo);
      const model = repoPullMergeModel(repo, 7, parsed);
      console.log(JSON.stringify({ merge: parsed.pullMerge, sent: world.sent.length, footer: model.footer, canMerge: model.canMerge }));
    """ % (TIP_B, NEW_HEAD))
    assert out["sent"] == 0
    assert out["merge"]["state"] == "changed"
    assert out["merge"]["changes"] == [
        "New commits were pushed: the head moved from 3333333 to aaaaaaa."]
    assert out["footer"]["tone"] == "warn"
    assert out["footer"]["changes"] == out["merge"]["changes"]
    assert out["canMerge"] is True


@needs_node
def test_a_second_stale_answer_is_reported_instead_of_looping():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail();
      world.record = pullMd();
      world.readiness = readiness();
      world.conversation = [approval];
      world.tips = ["%s"];
      world.replies = [["stale_pull_metadata"], ["stale_pull_metadata"], ["merged"]];
      await handleRepoPullMerge(repo);
      const failed = parsed.pullMerge;
      const model = repoPullMergeModel(repo, 7, parsed);
      await handleRepoPullMerge(repo);
      console.log(JSON.stringify({ failed, label: model.retry, ids: world.sent.map((item) => item.body.requestId), state: parsed.pullMerge.state }));
    """ % TIP_A)
    assert out["failed"]["state"] == "failed"
    assert out["failed"]["error"] == "stale_pull_metadata"
    assert out["failed"]["resumable"] is False
    assert "Merge again" in out["failed"]["message"] or "newest state" in out["failed"]["message"]
    assert out["label"] is True
    # The Retry button never replays a request the relay already answered:
    # every attempt after a terminal answer carries a fresh request id.
    assert len(out["ids"]) == 3 and len(set(out["ids"])) == 3
    assert out["state"] == "merged"


@needs_node
def test_an_unconfirmed_request_is_resumed_with_the_same_id():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail();
      world.record = pullMd();
      world.readiness = readiness();
      world.conversation = [approval];
      world.tips = ["%s"];
      world.replies = [["merge_state_unavailable", 503]];
      const realNow = Date.now;
      let clock = 0;
      Date.now = () => (clock += 200000);
      await handleRepoPullMerge(repo);
      Date.now = realNow;
      const first = parsed.pullMerge;
      world.replies = [["merged"]];
      await handleRepoPullMerge(repo);
      console.log(JSON.stringify({ first, ids: world.sent.map((item) => item.body.requestId), state: parsed.pullMerge.state }));
    """ % TIP_A)
    assert out["first"]["resumable"] is True
    assert len(set(out["ids"])) == 1
    assert out["state"] == "merged"


@needs_node
def test_the_owner_override_is_explicit_confirmed_and_only_sent_when_needed():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail({ pullConversation: [] });
      world.record = pullMd();
      world.readiness = readiness();
      world.conversation = [];
      world.tips = ["%s"];
      const before = repoPullMergeModel(repo, 7, parsed);
      confirmAnswer = false;
      await handleRepoPullMergeOverride(repo);
      const declined = world.sent.length;
      confirmAnswer = true;
      world.replies = [["merged"]];
      await handleRepoPullMergeOverride(repo);
      console.log(JSON.stringify({ canMerge: before.canMerge, canOverride: before.canOverride, footer: before.footer.text, review: before.rows[0], declined, sent: world.sent.map((item) => item.body) }));
    """ % TIP_A)
    assert out["canMerge"] is False and out["canOverride"] is True
    assert "override the rule" in out["footer"]
    assert out["review"]["title"] == "Independent review required"
    assert out["review"]["actions"][0]["kind"] == "copy-review"
    assert out["declined"] == 0
    assert out["sent"][0]["overrideReview"] is True


@needs_node
def test_merge_readiness_names_each_blocker_and_its_action():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const rows = (parsed) => Object.fromEntries(repoPullMergeModel(repo, 7, parsed).rows.map((row) => [row.id, row]));
      const behind = rows(detail({ pullReadiness: readiness({ behind: 80 }) }));
      const conflict = rows(detail({ pullReadiness: readiness({ behind: 3, clean: false, conflicts: ["a.py", "b.py"] }) }));
      const patch = detail({
        values: { ...parseFrontMatter(pullMd()).values, derive: "", head: "bob:leaderboard", headOid: "7".repeat(40) },
        pullReadiness: readiness({ behind: 80, headOid: "7".repeat(40) }),
        pullPatch: { patch: "diff --git a/x b/x" },
      });
      const fork = rows(patch);
      const visitor = repoPullMergeModel(repo, 7, detail({ pullMergeAuthorized: false, pullReadiness: readiness({ behind: 5 }) }));
      const writer = repoPullMergeModel(repo, 7, detail({ pullMergeAuthorized: false, pullUpdateAuthorized: true, pullReadiness: readiness({ behind: 5 }) }));
      const visitorStatus = repoPullMergeStatus(repoPullMergeModel(repo, 7, detail({ pullMergeAuthorized: false, pullConversation: [{ type: "review", state: "approved", author: "k".repeat(43), authorName: "carol" }] })));
      const checks = rows(detail({
        pullChecks: { available: true, controlAvailable: true, node: "mirror-1", runs: [{ id: 9, workflow: "ci", status: "failed" }] },
        pullRequiredChecks: { known: true, names: ["ci", "lint"] },
      }));
      const strangerChecks = rows(detail({
        pullMergeAuthorized: false,
        pullChecks: { available: true, controlAvailable: true, node: "mirror-1", runs: [{ id: 9, workflow: "ci", status: "failed" }] },
      }));
      const nodeless = rows(detail({
        pullChecks: { available: true, controlAvailable: true, runs: [{ id: 9, workflow: "ci", status: "failed" }] },
      }));
      const hostile = rows(detail({
        values: { ...parseFrontMatter(pullMd()).values, head: "x;touch${IFS}pwned" },
        pullReadiness: readiness({ behind: 3, clean: false, conflicts: ["a.py"] }),
      }));
      const ready = repoPullMergeModel(repo, 7, detail({ pullChecks: { available: true, runs: [{ id: 1, workflow: "ci", status: "success" }] } }));
      const conflictModel = repoPullMergeModel(repo, 7, detail({ pullReadiness: readiness({ clean: false, conflicts: ["a.py"] }) }));
      console.log(JSON.stringify({ behind: behind.branch, conflict: conflict.branch, fork: fork.branch, visitor: { rows: visitor.rows.find((row) => row.id === "branch"), footer: visitor.footer, canMerge: visitor.canMerge }, writer: { rows: writer.rows.find((row) => row.id === "branch"), canMerge: writer.canMerge, authorized: writer.authorized }, visitorStatus, checks: checks.checks, strangerChecks: strangerChecks.checks, nodeless: nodeless.checks, hostile: hostile.branch, ready: { canMerge: ready.canMerge, canOverride: ready.canOverride, footer: ready.footer }, conflictModel: { canMerge: conflictModel.canMerge, canOverride: conflictModel.canOverride } }));
    """)
    assert out["behind"]["title"] == "80 commits behind main"
    assert out["behind"]["actions"] == [{"kind": "update", "label": "Update branch", "busy": False}]
    assert "does not block the merge" in out["behind"]["detail"]
    assert out["conflict"]["tone"] == "bad"
    assert out["conflict"]["conflicts"] == ["a.py", "b.py"]
    assert "git merge origin/main" in out["conflict"]["detail"]
    assert out["conflict"]["actions"] == [{"kind": "update", "label": "Update branch", "busy": False}]
    assert out["fork"]["actions"] == []
    assert out["fork"]["tone"] == "good"
    assert out["fork"]["title"] == "Merges cleanly into main"
    assert "No update is needed" in out["fork"]["detail"]
    assert "replays it onto the newest main" in out["fork"]["detail"]
    assert out["writer"]["rows"]["actions"] == [{"kind": "update", "label": "Update branch", "busy": False}]
    assert out["writer"]["canMerge"] is False and out["writer"]["authorized"] is False
    assert out["visitorStatus"]["label"] != "Ready to merge"
    assert out["visitorStatus"]["tone"] != "good"
    assert out["visitor"]["rows"]["actions"] == []
    assert "The author can bring main in" in out["visitor"]["rows"]["detail"]
    assert out["visitor"]["canMerge"] is False
    assert "Only an owner of the organization" in out["visitor"]["footer"]["text"]
    assert out["checks"]["tone"] == "bad"
    assert {"kind": "rerun", "label": "Re-run failed", "runId": 9,
            "node": "mirror-1"} in out["checks"]["actions"]
    assert all(a["kind"] != "rerun" for a in out["strangerChecks"]["actions"])
    assert all(a["kind"] != "rerun" for a in out["nodeless"]["actions"])
    assert "git switch 'x;touch${IFS}pwned'" in out["hostile"]["detail"]
    assert "Required before merging: ci, lint." in out["checks"]["detail"]
    assert "Not yet passed: ci, lint." in out["checks"]["detail"]
    assert out["ready"] == {"canMerge": True, "canOverride": False, "footer": {
        "tone": "good", "method": "merge",
        "text": "Ready to merge into main. An online mirror lands the exact reviewed commits with a merge commit."}}
    assert out["conflictModel"] == {"canMerge": False, "canOverride": False}


@needs_node
def test_update_branch_re_pins_and_retries_once_then_refreshes_the_page():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail({ pullReadiness: readiness({ behind: 4 }) });
      world.record = pullMd();
      world.readiness = readiness({ behind: 4 });
      world.conversation = [approval];
      world.tips = ["%s", "%s"];
      world.replies = [["stale_pull_metadata"], ["updated"]];
      await handleRepoPullUpdate(repo);
      const updated = parsed.pullUpdate.state;
      const note = repoPullMergeModel(repo, 7, parsed).rows.find((row) => row.id === "branch").note;
      world.readiness = readiness({ behind: 0 });
      world.sent.length = 0;
      await handleRepoPullUpdate(repo);
      console.log(JSON.stringify({ updated, note, current: parsed.pullUpdate.state, urls: world.sent.map((item) => item.url), refreshed }));
    """ % (TIP_A, TIP_B))
    assert out["updated"] == "updated"
    assert out["note"]["tone"] == "good"
    assert out["note"]["text"].startswith("Synced this branch with main · ")
    assert out["note"]["text"].endswith(". Checks re-run on the new head.")
    assert out["current"] == "current"
    assert out["urls"] == []
    assert out["refreshed"] == [7]


@needs_node
def test_update_requests_are_typed_and_pin_the_current_tips():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail({ pullReadiness: readiness({ behind: 4 }) });
      world.record = pullMd();
      world.readiness = readiness({ behind: 4 });
      world.conversation = [approval];
      world.tips = ["%s"];
      world.replies = [["merge_conflict"]];
      await handleRepoPullUpdate(repo);
      console.log(JSON.stringify({ sent: world.sent, update: parsed.pullUpdate }));
    """ % TIP_A)
    (sent,) = out["sent"]
    assert sent["url"] == "/api/repo/alice/widgets/pulls/7/update"
    assert sent["body"]["type"] == "forkmesh.pull-update-v1"
    assert sent["body"]["expectedBaseOid"] == BASE
    assert sent["body"]["expectedHeadOid"] == HEAD
    assert sent["body"]["expectedPullsOid"] == TIP_A
    assert out["update"]["state"] == "failed"
    assert "Resolve them locally" in out["update"]["message"]


def test_the_relay_replays_a_used_request_id_so_the_page_must_mint_a_new_one():
    # Root cause, part two: the relay keeps a terminal answer per request id
    # (idempotency), and the old Retry button resent the same id, so it could
    # only ever read back the stored stale_pull_metadata failure.
    assert 'if row.get("status") in ("succeeded", "failed"):' in ROUTES
    assert "previous.resumable === true ? previous.request : null" in CONTENT
    assert "existing.requestId || createRepoPullMergeRequestId()" not in CONTENT
    assert "PULL_MERGE_RESUMABLE_ERRORS.has(error)" in CONTENT


def test_merge_pins_bypass_the_page_bundle_and_the_session_cache():
    # Root cause, part one: the pull page's bundle (repo_views) is served
    # stale-while-revalidate for up to five minutes, and its forkmesh/pulls
    # read pinned the page to an old ledger tip even after a reload.
    fetch = _function("fetchRepoJson")
    assert "const remembered = options.canonical || options.fresh;" in fetch
    assert "const seeded = remembered ? null : await seededRepoRead(path);" in fetch
    assert "const early = remembered ? null : await earlyRepoRead(path);" in fetch
    resolve = _function("resolveRepoPullMetadataCommit")
    assert resolve.index("if (options.fresh) {") < resolve.index("const cached = immutableGitCommit")
    refresh = _function("refreshRepoPullMergePins")
    assert "resolveRepoPullMetadataCommit(repo, { fresh: true })" in refresh
    assert "loadRepoPullReadiness(repo, number, values, { fresh: true })" in refresh
    assert "{ fresh: Boolean(options.fresh) }" in _function("loadRepoPullReadiness")


def test_merge_box_replaces_the_three_panels_and_is_wired():
    assert "data-repo-pull-merge-slot" in CONTENT
    assert "data-repo-pull-readiness-slot" not in CONTENT
    assert "renderRepoPullMergeControl" not in CONTENT
    assert "handleRepoPullCheckRerun(state.selectedRepo, pullCheckRerunButton.dataset.repoPullCheckRerun)" in NETWORK
    assert 'option value="squash" disabled' in CONTENT
    assert 'option value="rebase" disabled' in CONTENT
    for selector in (".fm-merge-box", ".fm-merge-row", ".fm-merge-footer", ".fm-merge-method select"):
        assert selector in SHELL
    assert ".fm-merge-footer { display: grid; grid-template-columns: minmax(0, 1fr);" in SHELL
    assert 'data-repo-pull-merge-slot class="min-w-0 sm:ml-14"' in CONTENT


def test_tab_strip_keeps_its_scroll_and_widths():
    render = NETWORK[NETWORK.index("  function renderRepoDetail(repo) {"):NETWORK.index("  function renderRepoTitleRow(")]
    assert render.index("const tabStripScroll =") < render.index("detail.innerHTML = `")
    assert "if (tabStrip && tabStripScroll > 0) tabStrip.scrollLeft = tabStripScroll;" in render
    assert render.index("setRepoTab(initialTab);") < render.index("tabStrip.scrollLeft = tabStripScroll")
    assert '<span data-repo-tab-label="${escapeHtml(meta.label)}">' in render
    assert "reserveDiscussionsBadge" in render
    tab_state = EXPLORER[EXPLORER.index("  function setRepoTab(tab) {"):EXPLORER.index("  function activateRepoTab(")]
    assert "const tabChanged = state.activeRepoTab !== tab;" in tab_state
    assert 'if (tabChanged) keepRepoActiveTabVisible(detail.querySelector("[data-repo-tabs]"));' in tab_state
    keep = EXPLORER[EXPLORER.index("  function keepRepoActiveTabVisible("):EXPLORER.index("  function setRepoTab(")]
    assert "scrollIntoView" not in keep
    assert "[data-repo-tab-label]::after" in SHELL
    assert "content: attr(data-repo-tab-label);" in SHELL
    assert "overscroll-behavior-x: contain;" in SHELL
    assert "scrollbar-width: none;" in SHELL
    set_count = _function("setRepoTabCount")
    assert 'badge.classList.remove("hidden", "invisible");' in set_count
    assert "if (badge.textContent !== label) badge.textContent = label;" in set_count


def test_first_view_tab_strip_draws_what_the_dashboard_draws():
    assert '("security", "Security", "shield", "")' in SSR
    assert "'shield': (" in SSR
    assert '<span data-repo-tab-label="%s">%s</span>' in SSR
    assert "data-repo-tabs" in SSR


def test_the_behaviour_is_documented():
    for phrase in ("stale_pull_metadata", "stale-while-revalidate", "Update branch",
                   "overrideReview", "scrollLeft"):
        assert phrase in DOCS, phrase


def test_tab_strips_scroll_sideways_only():
    """The repository and pull request tab strips scroll horizontally only:
    their tabs overlap the border by 1px, which otherwise leaves a 1px vertical
    scroll that makes the strip jump during a sideways swipe on phones."""
    from pathlib import Path
    shell = (Path(__file__).resolve().parents[1] / "public" / "dashboard" / "shell.html").read_text()
    start = shell.index('[data-repo-github-header] [role="tablist"],\n      [data-repo-record-tabs] {')
    rule = shell[start:shell.index("}", start)]
    assert "overflow-y: hidden;" in rule
    assert "scrollbar-width: none;" in rule


def test_tab_strips_snap_back_from_any_vertical_offset():
    DASHBOARD_JS = (ROOT / "app/public/dashboard/js/05-repo-list-explorer.js").read_text(encoding="utf-8")
    assert 'strip.matches("[data-repo-tabs], [data-repo-record-tabs]")' in DASHBOARD_JS
    assert "strip.scrollTop = 0;" in DASHBOARD_JS


PROGRESS_FETCH = r"""
const delays = [];
window.setTimeout = (resolve, delay) => { delays.push(delay); resolve(); };
const seen = [];
function progressReply(body, phase, steps) {
  const at = (index) => 1000 + index * 400;
  return {
    ok: false, status: 202,
    json: async () => ({
      ok: true, status: "processing", requestId: body.requestId,
      progress: {
        state: "processing", phase, message: "", node: "mirror9",
        startedAt: at(0), updatedAt: at(steps.length - 1),
        steps: steps.map((step, index) => ({ phase: step, at: at(index) })),
      },
    }),
  };
}
"""


@needs_node
def test_merge_progress_follows_the_mirror_and_backs_off_one_two_three_seconds():
    out = _run(PROGRESS_FETCH + r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail();
      world.record = pullMd();
      world.readiness = readiness();
      world.conversation = [approval];
      world.tips = ["%s"];
      const script = [
        (body) => ({ ok: false, status: 503, json: async () => ({ error: "merge_node_unavailable", reason: "no_healthy_mirror" }) }),
        (body) => progressReply(body, "claimed", ["claimed"]),
        (body) => progressReply(body, "fetching", ["claimed", "fetching"]),
        (body) => progressReply(body, "fetching", ["claimed", "fetching"]),
        (body) => progressReply(body, "fetching", ["claimed", "fetching"]),
        (body) => progressReply(body, "verifying", ["claimed", "fetching", "verifying"]),
        null,
      ];
      const original = globalThis.fetch;
      globalThis.fetch = async (url, init) => {
        const body = JSON.parse(init.body);
        const model = repoPullMergeModel(repo, 7, parsed).footer.progress;
        if (model) seen.push({ headline: model.headline, state: model.state, current: model.steps.find((step) => step.state === "current")?.phase || "", node: model.node });
        const next = script.shift();
        return next ? next(body) : original(url, init);
      };
      await handleRepoPullMerge(repo);
      const done = repoPullMergeModel(repo, 7, parsed);
      console.log(JSON.stringify({ delays, seen, merge: parsed.pullMerge, done: done.footer.progress, html: renderRepoPullMutationProgress(done.footer.progress) }));
    """ % TIP_A)
    # One retry wait for the unclaimed job, then 1s/2s/3s per phase, reset on
    # every new phase; nothing waits after the terminal answer.
    assert out["delays"] == [1000, 1000, 1000, 2000, 3000, 1000]
    headlines = [item["headline"] for item in out["seen"]]
    assert headlines[0] == "Handing this to an online mirror…"
    assert headlines[1] == "Waiting for an online mirror…"
    assert headlines[2] == "mirror9 picked this up…"
    assert "Reading the base and head commits…" in headlines
    assert headlines[-1] == "Verifying the reviewed commits…"
    assert out["seen"][-1]["current"] == "verifying"
    assert out["seen"][-1]["node"] == "mirror9"
    assert out["merge"]["state"] == "merged"
    assert out["merge"]["commit"] == "8" * 40
    done = out["done"]
    assert done["state"] == "done"
    assert done["headline"] == "Merged into main"
    assert {step["state"] for step in done["steps"]} == {"done"}
    assert done["commit"] == {"oid": "8" * 40, "label": "Merged as", "href": "/alice/widgets/commits/" + "8" * 40}
    fetching = next(step for step in done["steps"] if step["phase"] == "fetching")
    assert fetching["ms"] == 400
    assert "Merged as <a" in out["html"] and "<code>8888888</code>" in out["html"]
    assert "data-repo-pull-merge-elapsed" not in out["html"]


@needs_node
def test_waiting_for_a_mirror_says_so_with_the_elapsed_time_and_what_to_do():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const now = 1_000_000;
      const request = { requestId: "dashboard_0001" };
      const model = (job) => repoPullMutationProgressModel(repo, "merge", { state: "processing", request, startedAt: now - 20000, ...job }, "main", "bob/leaderboard", now);
      const fresh = model({ queued: true, queuedSince: now - 3000 });
      const late = model({ queued: true, queuedSince: now - 20000 });
      const offline = model({ queued: false, offline: true, progress: { phase: "testing", node: "mirror9", steps: [{ phase: "claimed", at: 1 }, { phase: "testing", at: 5 }] } });
      const update = repoPullMutationProgressModel(repo, "update", { state: "processing", request, startedAt: now, queued: false, progress: { phase: "pushing", node: "mirror9", steps: [] } }, "main", "bob/leaderboard", now);
      console.log(JSON.stringify({ fresh, late, offline, update, html: renderRepoPullMutationProgress(late) }));
    """)
    assert out["fresh"]["state"] == "waiting"
    assert out["fresh"]["headline"] == "Waiting for an online mirror…"
    assert out["fresh"]["lateText"] == "Waiting for an online mirror to pick this up (no mirror has accepted it yet)."
    assert out["late"]["headline"] == out["fresh"]["lateText"]
    assert "single-server instance the node may be restarting" in out["late"]["detail"]
    assert "retrying automatically" in out["late"]["detail"]
    assert out["offline"]["headline"] == "Waiting for mirror9 to answer again…"
    assert [step["state"] for step in out["offline"]["steps"]][:6] == ["done", "done", "done", "done", "done", "current"]
    assert out["update"]["headline"] == "Updating bob/leaderboard…"
    assert "checking" not in [step["phase"] for step in out["update"]["steps"]]
    html = out["html"]
    assert 'data-repo-pull-merge-now role="status" aria-live="polite"' in html
    assert 'data-repo-pull-merge-elapsed data-since="980000" aria-hidden="true"' in html
    assert 'class="fm-merge-spinner" aria-hidden="true"' in html
    assert 'data-late-text="Waiting for an online mirror to pick this up (no mirror has accepted it yet)."' in html


@needs_node
def test_a_failed_merge_marks_the_refused_step_and_keeps_merge_again():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail({ pullMerge: {
        state: "failed", error: "merge_conflict", resumable: false, request: { requestId: "dashboard_0001" },
        message: "The exact reviewed commits do not apply cleanly to the base branch.",
        progress: { phase: "fetching", node: "mirror9", steps: [{ phase: "claimed", at: 1 }, { phase: "fetching", at: 2 }] },
      } });
      const model = repoPullMergeModel(repo, 7, parsed);
      const quiet = repoPullMergeModel(repo, 7, detail({ pullMerge: { state: "failed", message: "No online mirror could take this merge in the last few minutes. Try again shortly." } }));
      console.log(JSON.stringify({ progress: model.footer.progress, retry: model.retry, canMerge: model.canMerge, html: renderRepoPullMergeBox(repo, 7, parsed), quiet: quiet.footer }));
    """)
    progress = out["progress"]
    assert progress["state"] == "failed"
    assert progress["headline"].startswith("The exact reviewed commits do not apply cleanly")
    states = {step["phase"]: step["state"] for step in progress["steps"]}
    assert states["testing"] == "failed"
    assert states["fetching"] == "done" and states["checking"] == "done"
    assert states["committing"] == "todo"
    assert out["retry"] is True and out["canMerge"] is True
    assert "Merge again" in out["html"]
    assert 'data-state="failed"' in out["html"]
    # A failure no mirror ever held has no steps to show: the plain line stays.
    assert "progress" not in out["quiet"]
    assert out["quiet"]["tone"] == "bad"


@needs_node
def test_polling_slows_while_the_tab_is_hidden_and_stops_on_a_terminal_answer():
    out = _run(r"""
      globalThis.document = { visibilityState: "hidden", querySelector: () => null };
      const waits = [];
      window.setTimeout = (fn, ms) => { waits.push(ms); fn(); return 1; };
      let calls = 0;
      globalThis.fetch = async (url, init) => {
        calls += 1;
        const body = JSON.parse(init.body);
        if (calls < 3) return { ok: false, status: 202, json: async () => ({ ok: true, status: "processing", requestId: body.requestId }) };
        return { ok: false, status: 409, json: async () => ({ ok: false, status: "failed", error: "merge_conflict", requestId: body.requestId }) };
      };
      const result = await postRepoPullMutation("/x", { requestId: "dashboard_0001" });
      console.log(JSON.stringify({ calls, waits, error: result.payload.error }));
    """)
    assert out["calls"] == 3
    assert out["waits"] == [1000, 10000, 2000, 10000]
    assert out["error"] == "merge_conflict"


@needs_node
def test_nothing_polls_or_ticks_when_no_merge_is_in_flight():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      let intervals = 0;
      window.setInterval = () => { intervals += 1; return 1; };
      let fetches = 0;
      globalThis.fetch = async () => { fetches += 1; throw new Error("no fetch expected"); };
      const parsed = detail();
      const model = repoPullMergeModel(repo, 7, parsed);
      const html = renderRepoPullMergeBox(repo, 7, parsed);
      updateRepoPullMergePanel(repo, 7, parsed);
      startRepoPullMutationTicker();
      console.log(JSON.stringify({ progress: model.footer.progress || null, html, intervals, fetches }));
    """)
    assert out["progress"] is None
    assert "data-repo-pull-merge-progress" not in out["html"]
    assert 'data-repo-pull-merge-panel role="status" aria-live="polite"' in out["html"]
    assert out["intervals"] == 0
    assert out["fetches"] == 0


@needs_node
def test_update_branch_shows_the_same_progress_and_the_new_head():
    out = _run(PROGRESS_FETCH + r"""
      const repo = { owner: "alice", name: "widgets" };
      const parsed = detail({ pullReadiness: readiness({ behind: 4 }) });
      world.record = pullMd();
      world.readiness = readiness({ behind: 4 });
      world.conversation = [approval];
      world.tips = ["%s"];
      world.replies = [["updated"]];
      const script = [(body) => progressReply(body, "pushing", ["claimed", "fetching", "verifying", "testing", "committing", "pushing"]), null];
      const original = globalThis.fetch;
      globalThis.fetch = async (url, init) => {
        const row = repoPullMergeModel(repo, 7, parsed).rows.find((item) => item.id === "branch");
        if (row.progress) seen.push(row.progress.headline);
        const next = script.shift();
        return next ? next(JSON.parse(init.body)) : original(url, init);
      };
      await handleRepoPullUpdate(repo);
      const row = repoPullMergeModel(repo, 7, parsed).rows.find((item) => item.id === "branch");
      console.log(JSON.stringify({ seen, delays, progress: row.progress, html: renderRepoPullMergeBox(repo, 7, parsed) }));
    """ % TIP_A)
    assert out["seen"] == ["Handing this to an online mirror…", "Updating bob/leaderboard…"]
    assert out["delays"] == [1000]
    assert out["progress"]["state"] == "done"
    assert out["progress"]["commit"]["label"] == "New head"
    assert out["progress"]["commit"]["oid"] == "c" * 40
    assert 'data-repo-pull-merge-progress="update"' in out["html"]


def test_progress_respects_reduced_motion_and_reads_as_one_live_line():
    assert "@keyframes fm-merge-spin" in SHELL
    reduced = SHELL[SHELL.index(".fm-merge-spinner {"):]
    reduced = reduced[reduced.index("@media (prefers-reduced-motion: reduce)"):]
    assert ".fm-merge-spinner { animation: none;" in reduced[:400]
    render = _function("renderRepoPullMergeBox")
    assert '${footer.progress ? "" : \'role="status" aria-live="polite" aria-atomic="true" \'}' in render
    patch = _function("patchRepoPullMutationProgress")
    assert "if (now.textContent !== freshNow.textContent) now.textContent = freshNow.textContent;" in patch
    poll = _function("postRepoPullMutation")
    assert "started += await waitForRepoPullMutationVisible();" in poll
    assert "if (!pullDetailIsCurrent(repo, number)) return;" in patch
    assert "PULL_MUTATION_POLL_MS[Math.min(polls, PULL_MUTATION_POLL_MS.length - 1)]" in poll
    assert "const PULL_MUTATION_POLL_MS = [1000, 2000, 3000];" in CONTENT


@needs_node
def test_a_merged_or_closed_pull_shows_a_done_box_instead_of_merge_rows():
    out = _run(r"""
      const repo = { owner: "alice", name: "widgets" };
      const merged = detail({ values: parseFrontMatter(pullMd()).values });
      merged.values.status = "merged";
      const mergedHtml = renderRepoPullMergeBox(repo, 7, merged);
      const closed = detail();
      closed.values.status = "closed";
      const closedHtml = renderRepoPullMergeBox(repo, 7, closed);
      console.log(JSON.stringify({ mergedHtml, closedHtml }));
    """)
    merged = out["mergedHtml"]
    assert 'data-repo-pull-done="merged"' in merged
    assert "Pull request successfully merged and closed" in merged
    assert "bob/leaderboard is in main" in merged
    for row in ("review", "checks", "branch"):
        assert 'data-repo-pull-merge-row="%s"' % row not in merged
    assert "data-repo-pull-merge " not in merged and "Merge pull request" not in merged
    closed = out["closedHtml"]
    assert 'data-repo-pull-done="closed"' in closed
    assert "Closed with unmerged commits" in closed


@needs_node
def test_the_merge_lands_in_the_timeline_where_it_happened():
    harness = "\n".join([
        _function("pullTimestamp"),
        _function("pullHeadBranchRef"),
        _function("buildRepoPullTimeline"),
        _function("repoPullMergedEvent"),
    ])
    source = r"""
const foldPullCommentChanges = (rows) => rows || [];
const recordAuthorName = (values) => values.authorName || "";
""" + harness + r"""
const values = { status: "merged", base: "main", head: "bob/leaderboard", derive: "branch", author: "bob-key",
  authorName: "bob", ts: 1700000000, mergedAt: 1700000300000, mergedBy: "bob-key" };
const conversation = [
  { id: "a", type: "review", state: "approved", ts: 1700000200 },
  { id: "b", type: "comment", body: "thanks", ts: 1700000400000 },
];
const rows = buildRepoPullTimeline(values, conversation, [], []);
const unknown = buildRepoPullTimeline({ ...values, mergedAt: "", mergedBy: "" }, conversation, [], []);
const open = buildRepoPullTimeline({ ...values, status: "open" }, conversation, [], []);
console.log(JSON.stringify({ rows: rows.map((row) => row.type), merged: rows.find((row) => row.type === "merged"),
  unknown: unknown.map((row) => row.type), open: open.map((row) => row.type) }));
"""
    result = subprocess.run(["node", "--input-type=module", "-e", source], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    out = json.loads(result.stdout)
    assert out["rows"] == ["open", "review", "merged", "comment"]
    assert out["merged"]["authorName"] == "bob"
    assert out["merged"]["base"] == "main" and out["merged"]["head"] == "bob/leaderboard"
    assert out["unknown"] == ["open", "review", "comment", "merged"]
    assert "merged" not in out["open"]


def test_a_finished_pull_offers_comments_but_not_reviews():
    form = _function("renderPullReviewForm")
    assert 'const reviewable = String(parsed?.values?.status || "open").trim().toLowerCase() === "open";' in form
    assert '${reviewable ? `<button type="submit" data-repo-pull-review-action="changes_requested"' in form
    assert '${reviewable ? `<button type="submit" data-repo-pull-review-action="approved"' in form
    assert 'data-repo-pull-review-action="comment"' in form
    render = _function("renderPullConversationEvent")
    assert 'if (ev.type === "merged") {' in render
