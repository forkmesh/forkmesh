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


def _node(body):
    script = """
const state = { issuesView: { items: [], query: "" }, issueRelationIndex: null };
const requests = [];
let answers = {};
function repoKey(repo) { return repo; }
function repoLiveUrl(repo, action, params = {}) {
  return `${action}?${new URLSearchParams(params).toString()}`;
}
async function fetchRepoJson(url) {
  requests.push(url);
  const key = Object.keys(answers).find((prefix) => url.startsWith(prefix));
  if (!key || answers[key] === null) throw Object.assign(new Error("not_found"), { status: 404 });
  return answers[key];
}
function formatRecordDate(value) { return value ? `date:${value}` : "unknown"; }
function localFilterQueries(query) { return [String(query || "").trim().toLowerCase()].filter(Boolean); }
let rendered = 0;
function renderRepoIssues() { rendered += 1; }
function $(selector) { return selector.includes("issue-list-header") ? {} : null; }
%s
(async () => { %s })();
""" % ("\n".join(_function(CONTENT, name) for name in (
        "issueFromIndex", "fetchRepoIssueIndex", "loadRepoIssueRelationIndex",
        "issueMatchesQuery", "issueMatchesSearch", "issueSearchState", "runIssueSearch",
        "issueJsonPath", "issueSourceUrlSafe", "repoIssueSourceUrl"))
        + "\nconst ISSUE_SEARCH_DELAY_MS = 0;\nconst repoIssueSources = new Map();", body)
    result = subprocess.run(["node", "-e", script], capture_output=True, text=True,
                            check=True)
    return json.loads(result.stdout)


def test_issue_page_reads_only_its_related_issues_then_the_full_index():
    out = _node("""
answers = { "issue-index?related=484": { ok: true, issues: [{ number: 3, title: "Parent", dependsOn: [] }], related: 484 } };
const related = await loadRepoIssueRelationIndex("o/r", 484);
answers = { "issue-index?related=": null, "issue-index?": { ok: true, issues: [{ number: 9, title: "All" }] } };
state.issueRelationIndex = null;
const fallback = await loadRepoIssueRelationIndex("o/r", 485);
console.log(JSON.stringify({ requests, related: related.map((i) => i.number), fallback: fallback.map((i) => i.number) }));
""")
    assert out["requests"] == ["issue-index?related=484", "issue-index?related=485", "issue-index?"]
    assert out["related"] == [3] and out["fallback"] == [9]


def test_the_issue_page_learns_its_forge_original_from_either_index():
    out = _node("""
answers = { "issue-index?related=7": { ok: true, issues: [], related: 7,
  sourceUrl: "https://github.com/example/forum/issues/70" } };
await loadRepoIssueRelationIndex("o/r", 7);
const fromRelated = repoIssueSourceUrl("o/r", 7, []);
const fromList = repoIssueSourceUrl("o/r", 8, [issueFromIndex({ number: 8,
  sourceUrl: "https://github.com/example/forum/issues/80" })]);
const unsafe = repoIssueSourceUrl("o/r", 9, [{ number: 9, sourceUrl: "javascript:alert(1)" }]);
console.log(JSON.stringify({ fromRelated, fromList, unsafe }));
""")
    assert out == {"fromRelated": "https://github.com/example/forum/issues/70",
                   "fromList": "https://github.com/example/forum/issues/80",
                   "unsafe": ""}


def test_free_text_search_asks_the_gateway_for_bodies_and_comments():
    out = _node("""
state.issuesView = { repo: "o/r", ref: "c".repeat(40), query: "censor", items: [
  issueFromIndex({ number: 2, title: "Censorship report", author: "a", meta: "open", events: [] }),
  issueFromIndex({ number: 1, title: "Other", author: "b", meta: "open", events: [] }),
] };
answers = { "issue-search?": { ok: true, numbers: [1] } };
const before = state.issuesView.items.filter((i) => issueMatchesSearch(i, "censor")).map((i) => i.number);
await new Promise((resolve) => setTimeout(resolve, 20));
const after = state.issuesView.items.filter((i) => issueMatchesSearch(i, "censor")).map((i) => i.number);
console.log(JSON.stringify({ requests, before, after, rendered }));
""")
    assert out["before"] == [2]
    assert out["after"] == [2, 1]
    assert out["requests"] == ["issue-search?q=censor&ref=" + "c" * 40]
    assert out["rendered"] == 1


def test_issue_thread_renders_before_its_relations_arrive():
    source = _function(CONTENT, "loadRepoRecordDetail")
    assert "ISSUE_RELATIONS_WAIT_MS" in source
    assert "showRepoIssueRelationsLater" in source
    assert "await loadRepoIssueRelationIndex" not in source
    script = """
const document = { activeElement: null };
const window = {};
let issuePicker = null;
const draft = { value: "half typed" };
const container = { isConnected: true, innerHTML: "",
  contains: () => true,
  querySelector: (selector) => selector.includes("comment-body") ? draft : null };
const parsed = { body: "" };
const state = { repoRecordDetail: { parsed } };
function renderRepoRecordDetail(repo, kind, number, value) { return `rendered:${value.issueMentions.length}`; }
function mountIssueStickyHeader() {}
function paintRepoIssueDetail(target, html) { target.innerHTML = html; }
%s
%s
(async () => {
  await showRepoIssueRelationsLater("o/r", 5, parsed, container, Promise.resolve({ issueMentions: [1, 2], issueSubIssues: [], issueDependencies: [] }));
  console.log(JSON.stringify({ html: container.innerHTML, draft: draft.value }));
})();
""" % (_function(CONTENT, "repoIssueDetailBusy"), _function(CONTENT, "showRepoIssueRelationsLater"))
    out = json.loads(subprocess.run(["node", "-e", script], capture_output=True,
                                    text=True, check=True).stdout)
    assert out == {"html": "rendered:2", "draft": "half typed"}


HELPERS = (ROOT / "public" / "dashboard" / "js" / "02-helpers.js").read_text(encoding="utf-8")


def test_issue_rows_stack_recent_commenters_with_imported_pictures():
    from test_issue_timeline_people import _const
    code = "\n".join(
        [_const(HELPERS, name) for name in ("FORGE_PROFILES", "FORGE_IDENTITY_RE", "forgeAvatarSets", "forgeAvatarCommits")]
        + [_function(HELPERS, name) for name in (
            "escapeHtml", "forgeIdentity", "forgeLinkAttrs", "issueActorPixels",
            "issueActorIsBot", "issuePersonHandle", "rememberForgeAvatars", "forgeAvatarSrc",
            "issueActorAvatarHtml")]
        + [_function(CONTENT, "renderIssueRowCommenters")])
    script = """
const state = { session: null, selectedRepo: { owner: "example", name: "forum" } };
function instancePluginOn(id) { return id === "forum-list"; }
const ISSUE_PERSON_HANDLE_RE = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$/;
function repoContributorIsBot() { return false; }
function watchIssueActorAvatars() {}
function repoRawUrl(repo, path) { return `/api/repo/${repo.owner}/${repo.name}/raw?path=${encodeURIComponent(path)}`; }
%s
rememberForgeAvatars(state.selectedRepo, ["github:bob"]);
const html = renderIssueRowCommenters({ recentCommenters: ["github:Bob", "carol", "dan", "erin", "fay"] });
console.log(JSON.stringify({ html, none: renderIssueRowCommenters({}) }));
""" % code
    out = json.loads(subprocess.run(["node", "-e", script], capture_output=True, text=True,
                                    check=True).stdout)
    html = out["html"]
    assert out["none"] == ""
    assert html.count("h-5 w-5") == 4 and "fay" not in html
    assert html.count("<img") == 1
    assert 'src="/api/repo/example/forum/raw?path=.forkmesh%2Fcontributors%2Fgithub-bob.png"' in html
    assert 'loading="lazy"' in html and 'width="20" height="20"' in html
    assert "<a " not in html and "github.com" not in html
    assert 'title="Recent comments: @Bob, carol, dan, erin"' in html


def test_the_issue_list_defaults_to_newest_created_like_github():
    names = ("issueSortItems",)
    script = """
const state = { issuesView: { filters: { sort: "newest" } } };
function issueFilterState() { return state.issuesView.filters; }
%s
const items = () => [
  { number: 3, createdAtMs: 1700000000000, updatedAtMs: 1 },
  { number: 9, createdAtMs: 1600000000000, updatedAtMs: 9e12 },
  { number: 5, createdAtMs: 1700000000, updatedAtMs: 2 },
  { number: 8, createdAtMs: 1700000000000, updatedAtMs: 3 }];
console.log(JSON.stringify({
  byDefault: issueSortItems(items()).map((i) => i.number),
  unknown: issueSortItems(items(), "bogus").map((i) => i.number),
  updated: issueSortItems(items(), "recently_updated").map((i) => i.number) }));
""" % "\n".join(_function(CONTENT, name) for name in names)
    out = json.loads(subprocess.run(["node", "-e", script], capture_output=True, text=True,
                                    check=True).stdout)
    assert out == {"byDefault": [8, 5, 3, 9], "unknown": [8, 5, 3, 9],
                   "updated": [9, 8, 5, 3]}
    content = (ROOT / "public" / "dashboard" / "js" / "06-repo-content.js").read_text(encoding="utf-8")
    assert 'return instancePluginOn("forum-list") ? "newest" : "recently_updated";' in content
    assert "sort: issueDefaultSort()," in content
    assert '{ value: "newest", label: "Newest" },\n        { value: "recently_updated"' in CONTENT
    assert 'filters.sort !== issueDefaultSort()' in CONTENT
