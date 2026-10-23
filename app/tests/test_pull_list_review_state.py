"""The pull request list shows each pull's review state, not its branches.

A row carries an "Approved", "Changes requested" or "Review required" chip
derived with the merge box's own rule (pullReviewPolicy: each reviewer's latest
review counts, the author's never does) from the reviews the .forkmesh/pulls
listing carries (the gateway's ``pullReviews``), so the list reads no per-pull
folder. The "main <- branch" pill is gone from the row. The node's first view
(forkmesh_host.repo_views) draws the same chip from the same listing.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))

from forkmesh_host import repo_views  # noqa: E402

CONTENT = (ROOT / "public" / "dashboard" / "js" / "06-repo-content.js").read_text(
    encoding="utf-8")

pytestmark = pytest.mark.skipif(shutil.which("node") is None,
                                reason="node not installed")


def _slice(start, end):
    i = CONTENT.index(start)
    return CONTENT[i:CONTENT.index(end, i)]


POLICY = _slice("  function pullReviewPolicy(events", "  function renderPullReviewers(")
LIST = _slice("  function pullStateIconHtml(", "  // Shared unified-diff parser")

STUBS = r"""
const REPO_COLLECTION_PAGE_SIZE = 25;
const state = { repoCollectionPages: {}, selectedRepo: { owner: "alice", name: "garden" } };
const escapeHtml = (v) => String(v ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;")
  .replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;");
const formatCount = (n) => new Intl.NumberFormat("en-US").format(Number(n) || 0);
const formatRecordDateTime = () => ({ iso: "", label: "" });
const formatTimeAgo = () => "2 days ago";
const userNameHtml = (name) => escapeHtml(name);
const issueActorAvatarHtml = () => "";
const renderRepoCollectionPagination = () => "";
const repoRecordUrl = (repo, kind, number) => `/alice/garden/pull/${number}`;
const repoCollectionConfig = {};
"""

# (reviews in file order, pull author key, status, draft) -> expected state
CASES = [
    ([], "bob", "open", False, "review_required"),
    ([], "bob", "closed", False, None),
    ([], "bob", "open", True, None),
    ([["carol", "approved"]], "bob", "open", False, "approved"),
    # A later approval supersedes the same reviewer's request for changes.
    ([["carol", "changes_requested"], ["carol", "approved"]], "bob", "open",
     False, "approved"),
    # ... and a later request supersedes their approval.
    ([["carol", "approved"], ["carol", "changes_requested"]], "bob", "open",
     False, "changes_requested"),
    # Any reviewer's outstanding request wins over another's approval.
    ([["carol", "approved"], ["dave", "changes_requested"]], "bob", "open",
     False, "changes_requested"),
    # A plain comment clears that reviewer's earlier verdict, as in the box.
    ([["carol", "approved"], ["carol", "commented"]], "bob", "open", False,
     "review_required"),
    # The author's own approval never counts.
    ([["bob", "approved"]], "bob", "open", False, "review_required"),
    ([["carol", "approved"]], "bob", "merged", False, "approved"),
    ([["carol", "changes_requested"]], "bob", "closed", False,
     "changes_requested"),
]


def _node(body):
    result = subprocess.run(["node", "-e", STUBS + POLICY + LIST + body],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_latest_review_per_reviewer_wins_in_the_dashboard_and_first_view():
    cases = [list(case[:4]) for case in CASES]
    js = _node("console.log(JSON.stringify(%s.map(([r, a, s, d]) => "
               "pullListReviewState(r, a, s, d))));" % json.dumps(cases))
    for (reviews, author, status, draft, expected), got in zip(CASES, js):
        py = repo_views.review_state(reviews, author, status, draft)
        assert (got or {}).get("state") == expected, (reviews, got)
        assert py == got, (reviews, py, got)
    # An older mirror's listing has no pullReviews: no chip at all.
    assert _node("console.log(JSON.stringify(pullListReviewState(undefined, 'bob', 'open')));") is None
    assert repo_views.review_state(None, "bob", "open") is None


def _strip_icon(markup):
    markup = re.sub(r'<i data-lucide="([a-z-]+)" class="h-3 w-3"></i>', r"[\1]", markup)
    return re.sub(r'<svg [^>]*class="lucide lucide-([a-z-]+) h-3 w-3">.*?</svg>',
                  r"[\1]", markup)


def test_first_view_chip_is_the_dashboards_chip():
    reviews = [
        {"state": "approved", "approvals": 2, "blockers": 0},
        {"state": "changes_requested", "approvals": 1, "blockers": 1},
        {"state": "review_required", "approvals": 0, "blockers": 0},
        None,
    ]
    js = _node("console.log(JSON.stringify(%s.map(pullListReviewChipHtml)));"
               % json.dumps(reviews))
    for review, markup in zip(reviews, js):
        assert _strip_icon(repo_views._review_chip(review)).strip() == \
            _strip_icon(markup)
    assert 'data-repo-pull-review="approved"' in js[0]
    assert "text-success" in js[0] and "[check]Approved<" in _strip_icon(js[0])
    assert 'title="Approved by 2 reviewers"' in js[0]
    assert "text-destructive" in js[1]
    assert "[file-diff]Changes requested<" in _strip_icon(js[1])
    assert "text-muted-foreground" in js[2] and "Review required" in js[2]
    assert js[3] == ""


def test_list_row_has_the_chip_and_no_branch_line():
    item = {"number": 7, "title": "Fix it", "state": "open", "author": "bob",
            "base": "main", "head": "bob:feature-x", "meta": "open · main ← feature-x",
            "review": {"state": "approved", "approvals": 1, "blockers": 0}}
    html = _node("console.log(JSON.stringify(renderRepoRecordList(%s, "
                 "{ itemLabel: 'pull request' }, 'pulls')));" % json.dumps([item]))
    assert 'data-repo-pull-review="approved"' in html
    assert "feature-x" not in html and "arrow-left" not in html
    assert "←" not in html
    meta = html[html.index("#7"):]
    assert meta.index("opened") < meta.index("data-repo-pull-review")


def _ssr_rows():
    open_row = {"number": 7, "title": "Fix it", "state": "open",
                "author": "bob", "ts": "", "base": "main", "head": "feature-x",
                "review": {"state": "changes_requested", "approvals": 0,
                           "blockers": 1}}
    return [open_row]


def test_first_view_row_has_the_chip_and_no_branch_line():
    class Bundle:
        owner, repo = "alice", "garden"
    html = repo_views._list(Bundle(), "pulls", _ssr_rows())
    assert 'data-repo-pull-review="changes_requested"' in html
    assert "Changes requested</span>" in html
    assert "feature-x" not in html and "&larr;" not in html


def test_listing_reviews_reach_the_rows_without_per_pull_reads():
    sys.path.insert(0, str(ROOT / "tests" / "host"))
    import asyncio
    import test_repo_views as views_test

    class WithReviews(views_test.FakeFastPath):
        async def read(self, url, immutable=False):
            status, body, data = await super().read(url, immutable)
            if "path=.forkmesh%2Fpulls" in url and "tree" in url and status == 200:
                data = dict(data, pullReviews={
                    "4": [["carol", "changes_requested"], ["carol", "approved"]]})
                body = json.dumps(data).encode()
            return status, body, data

    for cache in (repo_views._bundles, repo_views._latest,
                  repo_views._building, repo_views._public):
        cache.clear()
    fast = WithReviews()
    bundle = asyncio.run(repo_views.build(fast, "forkmesh", "forkmesh", "pulls"))
    rows = {row["number"]: row for row in bundle.model["rows"]}
    assert rows[4]["review"]["state"] == "approved"
    # #3 is merged with no reviews: nothing to show. #7 is branch-only, so
    # the ledger's listing does not speak for it.
    assert rows[3]["review"] is None
    assert "review" not in rows[7]
    # No pull folder was listed to find its reviews.
    assert not any(re.search(r"path=\.forkmesh%2Fpulls%2F\d+(&|$)", url)
                   for url, _ in fast.urls)
    html = repo_views.render(bundle)
    assert html.count("data-repo-pull-review=") == 1
    assert 'data-repo-pull-review="approved"' in html
