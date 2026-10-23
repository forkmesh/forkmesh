import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_safe_force_push import (  # noqa: E402,F401
    _branch,
    _commit,
    _open_pull,
    _push,
    _rewrite,
    _updates,
    forge,
    run,
)

ROOT = Path(__file__).resolve().parents[2]
CONTENT = (ROOT / "app/public/dashboard/js/06-repo-content.js").read_text(
    encoding="utf-8")


def test_a_fast_forward_push_names_the_open_pulls_of_its_branch(forge):
    handler, source, git_dir, _app = forge
    _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    assert "pulls" not in _updates(handler)["updates"][0]
    _open_pull(git_dir, source, 1, "feature")
    _open_pull(git_dir, source, 2, "feature", status="closed")
    _open_pull(git_dir, source, 3, "carol:feature")
    run(["git", "checkout", "-q", "feature"], source)
    _commit(source, "more.txt")
    assert _push(source, "origin", "feature").returncode == 0
    update = _updates(handler)["updates"][0]
    assert update["pulls"] == [1]
    assert update["commits"] == 1 and "forced" not in update


def test_a_forced_push_still_names_its_pulls(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    _open_pull(git_dir, source, 1, "feature")
    run(["git", "checkout", "-q", "feature"], source)
    _rewrite(source)
    assert _push(source, "--force", "origin", "feature").returncode == 0
    update = _updates(handler)["updates"][0]
    assert update["forced"] is True and update["pulls"] == [1]


def test_the_default_branch_and_new_branches_look_up_no_pulls(forge):
    handler, source, git_dir, _app = forge
    _open_pull(git_dir, source, 1, "main")
    _open_pull(git_dir, source, 2, "fresh")
    run(["git", "checkout", "-q", "main"], source)
    _commit(source, "main-more.txt")
    assert _push(source, "origin", "main").returncode == 0
    assert "pulls" not in _updates(handler)["updates"][0]
    _branch(source, "fresh")
    assert _push(source, "origin", "fresh").returncode == 0
    assert "pulls" not in _updates(handler)["updates"][0]


def _function_source(name, prefix="  function "):
    start = CONTENT.index(prefix + name + "(")
    end = CONTENT.index("\n  }\n", start) + 4
    return CONTENT[start:end]


def _node(script):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    return json.loads(subprocess.run(
        [node, "-e", script], capture_output=True, text=True, check=True,
        timeout=30).stdout)


HELPERS = """
const immutableGitCommit = (value) => /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/.test(String(value || "").toLowerCase()) ? String(value).toLowerCase() : "";
const escapeHtml = (value) => String(value ?? "").replace(/[&<>"]/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"})[c]);
const formatCount = (value) => String(value);
const parsePatchStats = (patch) => patch ? [{path: "a.txt", adds: 1, dels: 0}] : [];
const repoLiveUrl = (repo, operation, query) => JSON.stringify([operation, query]);
const calls = [];
"""


def test_a_synced_pull_reads_its_diff_from_the_pushed_head():
    synced = "b" * 40
    base = "c" * 40
    script = HELPERS + _function_source("pullSyncedHead") + _function_source(
        "loadRepoPullSyncedPatch", "  async function ") + """
const fetchRepoJson = async (url) => { calls.push(JSON.parse(url)); return {patch: "diff --git a/a.txt b/a.txt"}; };
(async () => {
  const values = {headOid: "%s", syncedHeadOid: "%s", headBaseOid: "%s"};
  const patch = await loadRepoPullSyncedPatch({}, values);
  const replay = await loadRepoPullSyncedPatch({}, {...values, headOid: "%s"});
  console.log(JSON.stringify({patch, replay, calls, head: pullSyncedHead(values)}));
})();
""" % (synced, synced, base, "d" * 40)
    result = _node(script)
    assert result["head"] == synced
    assert result["patch"]["synced"] == synced
    assert result["patch"]["files"] == [{"path": "a.txt", "adds": 1, "dels": 0}]
    assert result["calls"] == [["compare", {"base": base, "head": synced,
                                            "ref": ""}]]
    assert result["replay"] is None


def test_synchronize_events_render_pushes_and_mark_older_reviews():
    script = HELPERS + _function_source("pullEventTypeMeta") + _function_source(
        "pullReviewSummary") + _function_source(
            "renderPullConversationEvent") + """
const renderIssueTimelineEvent = (icon, who, author, html, ts, attrs = "") => `<div ${attrs} data-icon="${icon}">${who} ${html}</div>`;
const pullEventAnchorLabel = () => "";
const pullAgentStatusIsActive = () => false;
const pushed = {type: "synchronize", author: "bob", authorName: "bob", ref: "refs/heads/pr/bob/fix", before: "%s", after: "%s", commits: 2, forced: false, compareUrl: "/o/r/compare/a...b", ts: 1};
const forced = {...pushed, forced: true, commits: 1};
const summary = pullReviewSummary([
  {type: "review", author: "k1", authorName: "alice", state: "approved"},
  pushed,
  {type: "review", author: "k2", authorName: "carol", state: "approved"},
]);
console.log(JSON.stringify({
  pushed: renderPullConversationEvent(pushed),
  forced: renderPullConversationEvent(forced),
  label: pullEventTypeMeta(pushed).label,
  summary,
}));
""" % ("a" * 40, "b" * 40)
    result = _node(script)
    assert "pushed 2 commits to" in result["pushed"]
    assert "pr/bob/fix" in result["pushed"] and "bbbbbbb" in result["pushed"]
    assert "data-repo-pull-synchronize-compare" in result["pushed"]
    assert 'href="/o/r/compare/a...b"' in result["pushed"]
    assert "data-repo-pull-synchronize" in result["pushed"]
    assert "force-pushed" in result["forced"]
    assert "aaaaaaa" in result["forced"] and "bbbbbbb" in result["forced"]
    assert result["label"] == "pushed"
    rows = {row["authorName"]: row for row in result["summary"]}
    assert rows["alice"]["olderCommit"] is True
    assert rows["carol"]["olderCommit"] is False


def test_the_conversation_loader_links_synchronize_events_to_a_compare():
    loader = CONTENT[
        CONTENT.index("async function loadRepoPullConversation")
        : CONTENT.index("// A discussion's replies are an append-only")
    ]
    assert '(values.type === "force-push" || values.type === "synchronize")' in loader
    assert 'String(values.forced || "") === "true"' in loader
    commits = _function_source("loadRepoPullCommits", "  async function ")
    assert commits.index("loadRepoPullCompareCommits(repo, syncedBase, syncedHead)") < commits.index("commits.mbox")
    patch = _function_source("loadRepoPullPatch", "  async function ")
    assert patch.index("loadRepoPullSyncedPatch(repo, values)") < patch.index("changes.patch")
