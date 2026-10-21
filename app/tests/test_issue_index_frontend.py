import json
import subprocess
from pathlib import Path

from test_issue_timeline_people import _function

ROOT = Path(__file__).resolve().parents[1]
CONTENT = (ROOT / "public" / "dashboard" / "js" / "06-repo-content.js").read_text(
    encoding="utf-8")
NAMES = (
    "loadRepoIssues", "showIndexedRepoIssues", "fetchRepoIssueIndex",
    "fetchIssuePages", "fetchIssuePage", "issueFromIndex", "parseIssueJson",
    "parseFrontMatterList", "loadRepoIssueRelationIndex", "issueJsonPath",
)
COMMIT = "c" * 40


def _record(number, title, status="open"):
    return json.dumps({"number": number, "title": title, "status": status,
                       "events": [{"type": "open", "id": "o%d" % number,
                                   "body": "body %d" % number}]})


def _run(index_answer):
    script = """
const COMMIT = %s;
const INDEX = %s;
const RECORDS = { 12: %s, 5: %s };
const requests = [];
const state = { issuesView: { filter: "open", items: [] }, issueRelationIndex: null };
const container = { innerHTML: "", querySelector: () => null };
function $() { return container; }
function setRepoIssuesRefreshing() {}
function setRepoIssueRecordOpen() {}
function loadingHtml(text) { return text; }
async function primeRepoViewReads() {}
function isMissingMirrorFolder() { return false; }
function immutableGitCommit(value) { return value; }
function repoKey(repo) { return repo; }
function repoLiveUrl(repo, action, params = {}) {
  return `${action}?${new URLSearchParams(params).toString()}`;
}
async function fetchRepoOptionalTree(repo, path) {
  requests.push(`tree:${path}`);
  return { commit: COMMIT, entries: [{ name: "open", type: "tree" }, { name: "closed", type: "tree" }] };
}
async function fetchRepoJson(url) {
  requests.push(url);
  if (url.startsWith("issue-index")) {
    if (INDEX === null) throw Object.assign(new Error("not_found"), { status: 404 });
    return INDEX;
  }
  if (url.includes("closed")) return { entries: [{ name: "5", type: "tree" }] };
  return { entries: [{ name: "12", type: "tree" }] };
}
async function fetchRepoBlobs(repo, paths) {
  requests.push(`blobs:${paths.join(",")}`);
  return Object.fromEntries(paths.map((path) => [path, { content: RECORDS[Number(path.split("/").slice(-2)[0])] }]));
}
async function fetchMissingRepoBlobs() { return {}; }
function blobText(blob) { return blob.content; }
function formatRecordDate(value) { return value ? `date:${value}` : "unknown"; }
function withOptimisticIssues(repo, items) { return items; }
function applyPendingIssueStatuses() { return new Map(); }
function pendingReopenedIssueNumbers() { return []; }
const counts = [];
function setRepoTabCount() {}
function setRepoCollectionCounts(kind, open, closed) { counts.push([open, closed]); }
let rendered = 0;
function renderRepoIssues() { rendered += 1; }
async function loadClosedIssues() {}
%s
(async () => {
  await loadRepoIssues("o/r");
  const view = state.issuesView;
  state.issuesView = {};
  const relation = await loadRepoIssueRelationIndex("o/r");
  console.log(JSON.stringify({
    requests, counts, rendered,
    items: view.items.map((issue) => [issue.number, issue.title, issue.status, issue.date]),
    closedLoaded: view.closedLoaded,
    relation: relation.map((issue) => issue.number),
  }));
})();
""" % (json.dumps(COMMIT), json.dumps(index_answer), json.dumps(_record(12, "Open one")),
       json.dumps(_record(5, "Old", "closed")),
       "\n".join(_function(CONTENT, name) for name in NAMES))
    result = subprocess.run(["node", "-e", script], capture_output=True, text=True,
                            check=True)
    return json.loads(result.stdout)


def _summary(number, title, status, location):
    return {"number": number, "title": title, "status": status, "location": location,
            "deleted": False, "dateValue": 1000 + number, "labels": [], "events": []}


def test_issue_list_reads_the_index_and_no_record_blobs():
    out = _run({"ok": True, "commit": COMMIT, "truncated": False, "missing": [],
                "openFolders": 1, "closedFolders": 1, "issues": [
                    _summary(12, "Open one", "open", "open"),
                    _summary(5, "Old", "closed", "closed")]})
    assert out["requests"][:2] == ["tree:.forkmesh/issues", "issue-index?ref=" + COMMIT]
    assert not any(str(request).startswith(("blobs:", "tree?")) for request in out["requests"])
    assert out["items"] == [[12, "Open one", "open", "date:1012"],
                            [5, "Old", "closed", "date:1005"]]
    assert out["closedLoaded"] is True and out["counts"] == [[1, 1]]
    assert out["relation"] == [12, 5]
    assert out["requests"][-1] == "issue-index?"


def test_issue_list_falls_back_to_record_blobs_on_an_older_gateway():
    out = _run(None)
    assert "issue-index?ref=" + COMMIT in out["requests"]
    assert "blobs:.forkmesh/issues/open/12/issue-12.json" in out["requests"]
    assert out["items"][0][:3] == [12, "Open one", "open"]
    assert out["closedLoaded"] is False and out["counts"] == [[1, 1]]
    assert out["relation"] == [12]


def test_truncated_index_is_not_used():
    out = _run({"ok": True, "truncated": True, "issues": []})
    assert "blobs:.forkmesh/issues/open/12/issue-12.json" in out["requests"]
