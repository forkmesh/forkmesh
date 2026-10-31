"""GitHub-style pull review surface and owner-only mirror merge contracts."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONTENT = (
    ROOT / "app/public/dashboard/js/06-repo-content.js"
).read_text(encoding="utf-8")
NETWORK = (
    ROOT / "app/public/dashboard/js/08-repo-detail-network.js"
).read_text(encoding="utf-8")
HELPERS = (
    ROOT / "app/public/dashboard/js/02-helpers.js"
).read_text(encoding="utf-8")


def test_pull_tabs_are_real_isolated_panels():
    for name in ("conversation", "commits", "files", "badge"):
        assert f'data-repo-record-tab="{name}"' in CONTENT
        assert f'data-repo-record-panel="{name}"' in CONTENT
    assert 'aria-selected="true"' in CONTENT
    assert "panel.dataset.repoRecordPanel !== tab" in NETWORK
    assert "scrolls its section into view" not in NETWORK


def test_pull_collection_is_sorted_by_most_recent_activity():
    loader = CONTENT[
        CONTENT.index("async function loadRepoRecordsFromMirror")
        : CONTENT.index("function renderRepoCollectionPagination")
    ]
    assert "values.updatedAt || values.createdAt || values.ts" in loader
    assert "stamp(b) - stamp(a) || Number(b.number) - Number(a.number)" in loader


def test_changed_files_use_left_navigation_and_persistent_viewed_state():
    assert "data-repo-pull-file-list" in CONTENT
    assert "data-repo-pull-file-tree" in CONTENT
    assert "data-repo-diff-explorer-toggle" in CONTENT
    assert "data-lucide=\"folder\"" in CONTENT
    assert "pullViewedStorageKey" in CONTENT
    assert "localStorage.setItem(key" in CONTENT
    assert "data-repo-pull-viewed-summary" in CONTENT
    assert 'aria-pressed="${isViewed ? "true" : "false"}"' in CONTENT
    assert "toggleRepoPullViewed(" in NETWORK


def test_explorer_offers_hierarchy_and_flat_layouts_with_a_filter():
    # Hierarchy mode collapses single-child directory chains so a deep tree does
    # not spend an indent level per empty folder.
    assert "const collapseChain = (name, node)" in CONTENT
    assert 'label += `/${childName}`' in CONTENT
    assert 'data-repo-pull-file-tree data-mode="list"' in CONTENT
    assert 'data-repo-pull-file-tree data-mode="tree"' in CONTENT
    assert 'data-repo-pull-explorer-mode' in CONTENT
    assert "data-repo-pull-file-filter" in CONTENT
    assert "pullLayoutStorageKey" in CONTENT
    assert "setRepoPullExplorerMode(" in NETWORK
    assert "refreshRepoPullExplorer()" in NETWORK


def test_diff_pane_supports_unified_and_split_layouts():
    assert "function renderDiffFileRowsSplit(rows, options = {})" in CONTENT
    assert "function renderDiffFileRowsUnified(rows, options = {})" in CONTENT
    assert "function pairDiffRows(rows)" in CONTENT
    assert 'data-repo-pull-diff-mode' in CONTENT
    assert "setRepoPullDiffMode(" in NETWORK
    assert "data-repo-pull-diff-head" in CONTENT
    assert "data-repo-pull-diff-body" in CONTENT
    assert "foldRepoDiffBlock(block, true)" in CONTENT
    assert "content-visibility:auto" in CONTENT


def test_folding_a_file_keeps_its_header_in_place_both_ways():
    start = CONTENT.index("function keepPullDiffHeaderInPlace(button, change)")
    body = CONTENT[start:CONTENT.index("\n  }\n", start)]
    pin = body.index('block.style.contentVisibility = "visible"')
    no_anchor = body.index('scroller.style.overflowAnchor = "none"')
    edit = body.index("change();")
    assert pin < edit and no_anchor < edit
    assert "scroller.scrollTop += shift" in body
    assert body.index("padPullDiffTail(block, scroller") < body.index("scroller.scrollTop += shift")
    assert "data-repo-pull-diff-tail" in CONTENT
    assert 'block.style.contentVisibility = "auto"' in body[edit:]
    assert "scroller.style.overflowAnchor = anchor" in body[edit:]
    assert "keepPullDiffHeaderInPlace(block.firstElementChild" in CONTENT
    assert "toggleRepoPullViewed(state.selectedRepo" in NETWORK
    assert "keepPullDiffHeaderInPlace(pullDiffCollapseButton" in NETWORK

def test_diff_lines_support_inline_range_comments_and_review_batches():
    # Both layouts render semantic line-number controls. Pointer movement owns
    # range extension, while the resulting editor stays in a row-local slot.
    assert "data-repo-pull-diff-line" in CONTENT
    assert "data-repo-pull-inline-slot" in CONTENT
    assert "function beginPullDiffSelection(button" in CONTENT
    assert "function extendPullDiffSelection(button)" in CONTENT
    assert 'document.addEventListener("pointerover"' in NETWORK
    assert "data-repo-pull-inline-comment-form" in CONTENT
    assert 'data-repo-pull-inline-action="batch"' in CONTENT
    assert 'data-repo-pull-inline-action="now"' in CONTENT

    # Drafts survive diff re-renders in state and can be submitted together.
    assert "repoPullReviewDrafts" in CONTENT
    assert "data-repo-pull-review-batch-submit" in CONTENT
    assert "submitPullReviewBatch(state.selectedRepo)" in NETWORK
    assert "submitWebPullEvent(repo, number, fields.type, fields)" in CONTENT

    # Loaded and optimistic line comments share a compact gutter marker that
    # expands the thread under its code row.
    assert "data-repo-pull-inline-thread-toggle" in CONTENT
    assert 'data-lucide="user-round"' in CONTENT
    assert "repoPullInlineOpenThreads" in CONTENT
    assert "values.side || \"\"" in CONTENT


def test_explorer_highlight_follows_the_diff_pane_scroll():
    assert "function syncRepoPullActiveFileFromScroll()" in CONTENT
    assert "syncRepoPullActiveFileFromScroll()" in NETWORK
    # The shell scrolls an inner <section>, so the listener has to capture.
    assert "{ capture: true, passive: true }" in NETWORK


def test_branch_backed_pull_diff_is_reconstructed_from_creation_oids():
    assert 'repoLiveUrl(repo, "compare"' in CONTENT
    assert "values.creationBaseOid" in CONTENT
    assert "values.creationHeadOid" in CONTENT
    assert "loadRepoPullPatch(repo, number, pullMetadataCommit, values)" in CONTENT


def test_files_panel_uses_full_width_and_tracks_selected_file():
    assert "data-repo-record-layout" in CONTENT
    assert 'sidebar?.classList.toggle("hidden", !conversation)' in NETWORK
    assert 'setRepoPullActiveFile(path, { revealDiff: true })' in NETWORK
    assert 'button.setAttribute("aria-current", "true")' in CONTENT


def test_pull_review_approval_and_owner_only_mirror_merge_are_available():
    assert 'data-repo-pull-review-action="approved"' in CONTENT
    assert "loadRepoPullMergeAuthorization" in CONTENT
    assert 'profile?.viewerRole === "owner"' in CONTENT
    merge_auth = CONTENT[CONTENT.index("async function loadRepoPullMergeAuthorization"):]
    assert "if (sessionOwnsRepo(repo)) return true;" in merge_auth[:600]
    assert "data-repo-pull-merge" in CONTENT
    assert "expectedBaseOid" in CONTENT
    assert "expectedHeadOid" in CONTENT
    assert "expectedPullsOid" in CONTENT
    assert "handleRepoPullMerge(state.selectedRepo)" in NETWORK


def test_an_unmet_peer_gate_warns_the_owner_instead_of_locking_them_out():
    # The merge box renders the override only for an owner, as a separate,
    # distinctly worded control behind a confirmation, never a dead button.
    assert "Merge locked" not in CONTENT
    assert "canOverride: canAct && reviewMissing && !busy," in CONTENT
    assert "data-repo-pull-merge-override" in CONTENT
    assert '>Merge without review</button>' in CONTENT
    assert "Independent review required" in CONTENT
    # A request asks for the waiver only when the owner chose it and the
    # gate is actually unmet; the same request id never changes meaning.
    assert "...(waive ? { overrideReview: true } : {})" in CONTENT
    assert "overrideReview === true && !pullReviewPolicy(" in CONTENT
    assert "window.confirm(" in CONTENT
    assert "handleRepoPullMergeOverride(state.selectedRepo)" in NETWORK


def test_pull_sections_are_addressable_urls():
    # A link copied from Files changed reopens on Files changed
    # (/owner/repo/pulls/93/files); Conversation stays on the bare record URL.
    assert 'const REPO_RECORD_SECTIONS = [\n    "conversation", "commits", "checks", "files", "badge",\n  ];' in HELPERS
    assert "function normalizeRepoRecordSection(value)" in HELPERS
    assert "function repoRecordUrl(repo, kind, number, section)" in HELPERS
    assert 'return suffix === "conversation" ? url : `${url}/${suffix}`;' in HELPERS
    # The deep link is parsed back out of the path on load...
    assert "normalizeRepoRecordSection(routeParts[4])" in NETWORK
    assert "repoRecordUrl(repo, recordRoute.kind, recordRoute.number, recordRoute.section)" in NETWORK
    # ...the open tab is written into the address bar...
    assert "setRepoRecordSection(recordTabButton.dataset.repoRecordTab" in NETWORK
    assert "state.repoRecordSection" in NETWORK
    # ...and Back/Forward between two sections of the same record is a tab
    # switch, not another mirror read.
    assert "if (sameRecord) setRepoRecordSection(section);" in NETWORK
    # The rendered tab chrome follows the section the URL asked for.
    assert "normalizeRepoRecordSection(state.repoRecordSection)" in CONTENT
    assert "const recordTabAttrs = (name) =>" in CONTENT
    assert "const recordPanelClass = (name) =>" in CONTENT


def test_pull_header_names_the_source_and_target_repositories():
    # pull.md has no head-repository field: the record lives in the target
    # repo, and a cross-node pull only carries "<contributor>:" on head.
    assert "const branchChip = (repoLabel, branch) =>" in CONTENT
    assert 'const baseRepoLabel = repoDisplayName(repo, `${repo.owner || "owner"}/${repo.name || "repo"}`);' in CONTENT
    assert 'headRef.slice(0, headRef.lastIndexOf(":")).trim()' in CONTENT
    assert "branchChip(baseRepoLabel, baseBranch)} from ${branchChip(headRepoLabel, headBranch)}" in CONTENT
    assert '<span class="opacity-75">${escapeHtml(repoLabel)}:</span>${escapeHtml(branch)}' in CONTENT


def test_inline_threads_follow_their_code_across_patchsets_or_say_outdated():
    """Issue #483: review conversations stay on the right lines or expose an
    explicit outdated state when the diff is re-cut."""
    IO = (ROOT / "app/public/dashboard/js/03-issues-profile-io.js").read_text(encoding="utf-8")
    # New inline comments carry the signed patchset anchor: the head commit the
    # reviewer's diff was cut against and the exact text of the selected lines.
    assert "function pullCurrentPatchsetId()" in CONTENT
    assert "function pullAnchorLinesFor(path, side, lineStart, lineEnd)" in CONTENT
    assert "if (patchset) fields.patchset = patchset;" in CONTENT
    assert "if (anchorLines) fields.anchorLines = anchorLines;" in CONTENT
    # ...and both join the signed content (same rule as PullEvent::hasAnchor).
    assert "(fields.patchset || fields.anchorLines)" in IO
    assert "cleanBody, fields.suggestionPatch || \"\", ...anchorFields," in IO
    assert '"appliedCommit", "patchset", "anchorLines", "target"]' in IO
    # Loaded threads are placed against the diff on screen: current, moved to
    # where their text now sits, or outdated when the text left the diff.
    assert "function placePullInlineComment(event, lines)" in CONTENT
    assert "function nearestPullAnchorMatch(lines, anchorLines, preferred)" in CONTENT
    for placement in ("current", "outdated", "legacy"):
        assert f'placed("{placement}"' in CONTENT
    assert 'placed(at === lineStart ? "current" : "moved", at)' in CONTENT
    assert "pullInlineThreadKey(path, anchor.side, anchor.currentLineEnd)" in CONTENT
    # Outdated threads stay visible at the top of their file with an explicit
    # badge rather than vanishing or landing on unrelated lines.
    assert "data-repo-pull-outdated-threads" in CONTENT
    assert "data-repo-pull-thread-placement" in CONTENT
    assert "renderPullOutdatedThreads(path, groupPullInlineComments(path).get(pullOutdatedThreadKey(path)))" in CONTENT
    assert 'if (anchor.placement === "outdated") return "Outdated";' in CONTENT
    assert "Moved from line" in CONTENT
    # The conversation timeline names the placement too.
    assert "pullPlacementLabel(placePullInlineComment(ev))" in CONTENT


def test_web_merge_waits_out_transient_refusals_with_the_same_request():
    # forkmesh/forkmesh#268: the first 503 after a push ended the attempt with
    # a message blaming the mirrors' build, although waiting a minute for the
    # relay's pins to catch up was all the merge needed.
    import json
    import subprocess

    start = CONTENT.index("  const PULL_MUTATION_RETRY_ERRORS = new Set([")
    end = CONTENT.index("  const PULL_MERGE_REPIN_ERRORS")
    script = CONTENT[start:end] + r"""
      let clock = 0;
      const sent = [];
      globalThis.Date = { now: () => clock };
      globalThis.window = { setTimeout: (resolve, ms) => { clock += ms; resolve(); } };
      globalThis.state = { session: { sessionToken: "token" } };
      const reply = (status, body) => ({ ok: status < 300, status, json: async () => body });
      async function run(replies) {
        sent.length = 0;
        clock = 0;
        globalThis.fetch = async (url, init) => {
          sent.push(JSON.parse(init.body).requestId);
          return replies.length > 1 ? replies.shift() : replies[0];
        };
        const waits = [];
        const { response, payload, waiting } = await postRepoPullMutation(
          "/api/repo/forkmesh/forkmesh/pulls/268/merge", { requestId: "req-1" },
          (accepted) => waits.push(accepted));
        return { status: response.status, error: payload.error || payload.status, waiting, sent: sent.length, same: sent.every((id) => id === "req-1"), waits };
      }
      const out = [];
      out.push(await run([
        reply(503, { error: "merge_node_unavailable", reason: "refs_catching_up" }),
        reply(503, { error: "merge_status_pending" }),
        reply(202, { status: "processing", requestId: "req-1" }),
        reply(200, { status: "merged", requestId: "req-1" }),
      ]));
      out.push(await run([reply(503, { error: "merge_node_unavailable", reason: "no_merge_executor" })]));
      out.push(await run([reply(409, { error: "stale_base" })]));
      out.push(await run([reply(503, { error: "merge_node_unavailable", reason: "refs_catching_up" })]));
      out.push(await run([reply(202, { status: "processing", requestId: "req-1" })]));
      console.log(JSON.stringify(out));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        capture_output=True, text=True, timeout=30, check=True)
    merged, no_executor, refused, gave_up, still_running = json.loads(
        result.stdout)
    assert merged == {"status": 200, "error": "merged", "waiting": False,
                      "sent": 4, "same": True,
                      "waits": [False, False, True]}
    # Nothing a retry can fix is retried.
    assert no_executor["sent"] == 1 and not no_executor["waiting"]
    assert refused["sent"] == 1 and refused["error"] == "stale_base"
    # A fleet that stays unavailable, or a job that never finishes, ends
    # after the bounded wait instead of polling forever.
    assert gave_up["waiting"] and gave_up["error"] == "merge_node_unavailable"
    assert gave_up["sent"] < 40
    assert still_running["waiting"] and still_running["error"] == "processing"


def test_the_merge_panel_says_no_mirror_has_taken_the_job_yet():
    assert "Waiting for an online mirror to pick this up (no mirror has accepted it yet)." in CONTENT
    assert "parsed.pullMerge = repoPullMutationWaitState(parsed.pullMerge, accepted, payload);" in CONTENT


def _function_source(name):
    start = CONTENT.index("  function %s(" % name)
    end = CONTENT.index("\n  }\n", start) + 4
    return CONTENT[start:end]


def test_reviews_before_a_force_push_are_marked_as_on_an_older_commit():
    import json
    import shutil
    import subprocess

    import pytest

    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = _function_source("pullReviewerKey") + _function_source("pullReviewSummary") + """
const events = [
  {type: "review", author: "k1", authorName: "alice", state: "approved"},
  {type: "review", author: "k2", authorName: "carol", state: "commented"},
  {type: "force-push", author: "bob", before: "a", after: "b"},
  {type: "review", author: "k2", authorName: "carol", state: "approved"},
];
console.log(JSON.stringify(pullReviewSummary(events)));
"""
    output = subprocess.run([node, "-e", script], capture_output=True,
                            text=True, check=True, timeout=30).stdout
    rows = {row["authorName"]: row for row in json.loads(output)}
    assert rows["alice"]["olderCommit"] is True
    assert rows["carol"]["olderCommit"] is False
    assert rows["carol"]["state"] == "approved"


def test_force_push_events_render_with_both_commits_and_a_compare_link():
    loader = CONTENT[
        CONTENT.index("async function loadRepoPullConversation")
        : CONTENT.index("// A discussion's replies are an append-only")
    ]
    assert 'values.type === "force-push"' in loader
    assert "repoCompareUrl(repo, values.before, values.after)" in loader
    renderer = _function_source("renderPullConversationEvent")
    assert 'ev.type === "force-push"' in renderer
    assert "data-repo-pull-force-push-compare" in renderer
    assert "data-repo-pull-review-older" in CONTENT


def test_one_person_reviewing_with_two_keys_is_one_reviewer_and_one_vote():
    import json
    import shutil
    import subprocess

    import pytest

    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = _function_source("pullReviewerKey") + _function_source("pullReviewSummary") + _function_source("pullReviewPolicy") + """
const events = [
  {type: "review", author: "key-laptop", authorName: "alice", state: "changes_requested"},
  {type: "review", author: "key-phone", authorName: "Alice", state: "changes_requested"},
  {type: "review", author: "key-bob", authorName: "bob", state: "approved"},
  {type: "review", author: "key-author", authorName: "carol", state: "approved"},
];
console.log(JSON.stringify({
  rows: pullReviewSummary(events).map((row) => row.authorName),
  policy: pullReviewPolicy(events, "carol"),
}));
"""
    output = json.loads(subprocess.run([node, "-e", script], capture_output=True,
                                       text=True, check=True, timeout=30).stdout)
    assert len([name for name in output["rows"] if name.lower() == "alice"]) == 1
    assert output["policy"] == {"approvals": 1, "blockers": 1, "ready": False}


def test_unverified_reviewers_stay_apart_and_the_author_is_excluded_by_account():
    import json
    import shutil
    import subprocess

    import pytest

    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = _function_source("pullReviewerKey") + _function_source("pullReviewSummary") + _function_source("pullReviewPolicy") + """
const events = [
  {type: "review", author: "key-anon-1", authorName: "dave (unverified)", state: "changes_requested"},
  {type: "review", author: "key-anon-2", authorName: "dave (unverified)", state: "approved"},
  {type: "review", author: "key-carol-phone", authorName: "carol", state: "approved"},
];
console.log(JSON.stringify({
  rows: pullReviewSummary(events).length,
  policy: pullReviewPolicy(events, "key-carol-laptop", "carol"),
}));
"""
    output = json.loads(subprocess.run([node, "-e", script], capture_output=True,
                                       text=True, check=True, timeout=30).stdout)
    assert output["rows"] == 3
    assert output["policy"] == {"approvals": 1, "blockers": 1, "ready": False}
