#!/usr/bin/env python3
"""A just-filed web issue does not keep "reloading" (issue #675).

After a submit the detail opens on the browser's optimistic copy and polls the
mirror until it serves the record. Every miss used to repaint the loading state
and re-render the detail, so the page flashed as if reloading until the mirror
converged. The shipped loader runs here against a lagging mirror.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


CONTENT = (
    Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"
    / "06-repo-content.js"
).read_text(encoding="utf-8")


def _region(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


LOADER = _region(
    CONTENT,
    "  function showOptimisticRepoRecord",
    "  function setRepoTabCount",
)


HARNESS = r'''
const timers = [];
globalThis.window = {
  setTimeout: (fn, delay) => { timers.push({ fn, delay }); return timers.length; },
  clearTimeout: (id) => { if (timers[id - 1]) timers[id - 1].cancelled = true; },
  lucide: null,
};
const writes = [];
let commentBox = { value: "" };
let html = "";
const container = {
  get innerHTML() { return html; },
  set innerHTML(value) { html = value; writes.push(value); commentBox = { value: "" }; },
  querySelector: (selector) => {
    if (selector.startsWith("[data-repo-record-detail")) {
      return html.includes("data-repo-record-detail") ? {} : null;
    }
    if (selector === "[data-repo-issue-comment-body]") {
      return html.includes("data-repo-record-detail") ? commentBox : null;
    }
    return null;
  },
};
const state = { repoRecordDetail: null, repoRecordConvergeTimer: 0, repoRecordConvergeAttempt: 0 };
const $ = () => container;
const repoCollectionConfig = { issues: { itemLabel: "issue", dir: ".forkmesh/issues", file: (n) => `issue-${n}.json` } };
const records = { "source/forkmesh:issues:645": { values: { title: "Filed" } } };
const optimisticRepoRecords = () => records;
const optimisticRepoRecordKey = (repo, kind, number) => `source/forkmesh:${kind}:${number}`;
const forgetOptimisticRepoRecord = (repo, kind, number) => { delete records[optimisticRepoRecordKey(repo, kind, number)]; };
const repoKey = (repo) => `${repo.owner}/${repo.name}`.toLowerCase();
const stopRepoPullActivityRefresh = () => {};
const loadingHtml = (text) => `Loading: ${text}`;
const escapeHtml = (value) => String(value);
const renderRepoRecordDetail = (repo, kind, number, parsed) =>
  `<article data-repo-record-detail="${kind}">#${number} ${parsed.values?.title || parsed.title}</article>`;
let mirrorMisses = Number(process.argv[3] || 0);
let fetches = 0;
const fetchRepoJson = async () => {
  fetches += 1;
  if (mirrorMisses > 0) { mirrorMisses -= 1; throw new Error("not_found"); }
  return { text: "{}" };
};
const repoLiveUrl = () => "url";
const issueJsonCandidatePaths = () => ["open/645/issue-645.json"];
const fetchRepoIssueBlob = () => fetchRepoJson();
const issueDependencyNumbers = () => [];
const loadRepoIssueSummaries = async () => [];
const loadRepoIssueRelationIndex = async () => [];
const loadRepoIssueSubIssues = async () => [];
const issueMentionsOf = () => [];
const issueDetailParsed = () => ({ title: "Filed", body: "", values: { title: "Filed (mirror)" } });
const withPendingIssueStatus = (repo, number, parsed) => parsed;
const blobText = (blob) => blob.text;
const parseIssueJson = () => ({});
const loadRepoIssueMutationAuthorization = async () => false;
const loadWebIssueSubscription = async () => false;
const loadFederatedReplies = () => {};
const setRepoIssueRecordOpen = () => {};
const setRepoRecordOpen = () => {};
const mountIssueStickyHeader = () => {};
const primeRepoViewReads = async () => {};
const prepareCommentReactions = () => "";
const loadCommentReactions = async () => null;

__LOADER__

const flush = () => new Promise((resolve) => setTimeout(resolve, 0));
const fire = async () => {
  const timer = timers.filter((t) => !t.cancelled && !t.fired).shift();
  if (!timer) return null;
  timer.fired = true;
  timer.fn();
  for (let i = 0; i < 10; i += 1) await flush();
  return timer.delay;
};
const pending = () => timers.filter((t) => !t.cancelled && !t.fired).length;
const repo = { owner: "Source", name: "ForkMesh" };
const out = {};
const scenario = process.argv[2];

await loadRepoRecordDetail(repo, "issues", 645);
commentBox.value = "half-written comment";
if (scenario === "converges") {
  out.delays = [];
  let delay;
  while ((delay = await fire()) !== null) out.delays.push(delay);
  out.final = html;
  out.comment = commentBox.value;
  out.forgotten = !("source/forkmesh:issues:645" in records);
} else if (scenario === "left") {
  state.repoRecordDetail = null;
  container.innerHTML = "<ul>issue list</ul>";
  await fire();
  out.final = html;
  out.pending = pending();
} else if (scenario === "reopened") {
  await loadRepoRecordDetail(repo, "issues", 645);
  out.pending = pending();
}
out.writes = writes;
out.fetches = fetches;
process.stdout.write(JSON.stringify(out));
'''


def _run(scenario, misses=0):
    completed = subprocess.run(
        ["node", "--input-type=module", "-", scenario, str(misses)],
        input=HARNESS.replace("__LOADER__", LOADER),
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(completed.stdout)


pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="Node.js is unavailable"
)


def test_polling_a_lagging_mirror_never_repaints_the_detail():
    result = _run("converges", misses=4)
    assert not any("Loading" in write for write in result["writes"])
    # The optimistic render, then exactly one swap to the mirror's record.
    assert result["writes"] == [
        '<article data-repo-record-detail="issues">#645 Filed</article>',
        '<article data-repo-record-detail="issues">#645 Filed (mirror)</article>',
    ]
    assert result["delays"] == [1500, 3000, 6000, 12000, 24000]
    assert result["fetches"] == 5
    assert result["comment"] == "half-written comment"
    assert result["forgotten"]


def test_the_backoff_is_capped():
    assert max(_run("converges", misses=8)["delays"]) == 30000


def test_leaving_the_detail_stops_the_poll():
    result = _run("left", misses=10)
    assert result["final"] == "<ul>issue list</ul>"
    assert result["fetches"] == 0
    assert result["pending"] == 0


def test_reopening_the_record_keeps_a_single_poll():
    assert _run("reopened", misses=10)["pending"] == 1
