#!/usr/bin/env python3
"""The web Issues count stays right after filing a new issue (issue #716).

A web-filed issue is numbered at once but only reaches the mirror after the
owner node drains its inbox. Two paths got the count wrong in that window:

* the submit form recounted the loaded rows, so a list that had never loaded
  badged "1", and the list's own Open strip (issuesView.openCount) never moved;
* opening the Issues tab races refreshServedCounts against loadRepoIssues, and
  the served tally - which cannot know about the submission yet - overwrote the
  list's count, leaving the badge and "N Open" one short of the rows shown.

The shipped functions run here against stubbed badges, because "the count moves
by one and stays there" is behaviour, not source text.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
ISSUES_IO = (JS / "03-issues-profile-io.js").read_text(encoding="utf-8")
REPO_CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()


def _function(source, name):
    start = source.index(f"  function {name}(")
    end = source.index("\n  }\n", start) + len("\n  }\n")
    return source[start:end]


HARNESS = r'''
const badges = {};
const collection = {};
const state = { issuesView: {} };
function setRepoTabCount(tab, count) { badges[tab] = count; }
function setRepoCollectionCounts(kind, open, closed) {
  collection[kind] = { open, closed };
}
const $ = (selector) => (selector.includes('"issues"')
  ? { textContent: badges.seedText ?? "" } : null);

__FUNCTIONS__

const out = {};
const scenario = process.argv[2];
if (scenario === "loaded-list") {
  // The list loaded 11 open / 4 closed from the mirror; then one is filed.
  Object.assign(state.issuesView, { openCount: 11, closedCount: 4, unservedOpen: 0 });
  countFiledWebIssues(1);
  out.afterFiling = { badge: badges.issues, strip: state.issuesView.openCount,
                      collection: collection.issues };
  // Re-opening the tab: the served counts still describe the pre-filing tree.
  applyServedCounts({ openIssues: 11, closedIssues: 4 });
  out.afterServed = { badge: badges.issues, strip: state.issuesView.openCount,
                      collection: collection.issues };
} else if (scenario === "unloaded-list") {
  // Straight to New issue: the badge carries the served seed, no list loaded.
  badges.seedText = "1,204";
  countFiledWebIssues(1);
  out.badge = badges.issues;
  out.strip = state.issuesView.openCount ?? null;
  applyServedCounts({ openIssues: 1204, closedIssues: 30 });
  out.afterServed = badges.issues;
} else if (scenario === "converged") {
  // A later list load found the submission in the mirror's folder listing.
  Object.assign(state.issuesView, { openCount: 12, closedCount: 4, unservedOpen: 0 });
  applyServedCounts({ openIssues: 12, closedIssues: 4 });
  out.badge = badges.issues;
  out.strip = state.issuesView.openCount;
} else if (scenario === "import") {
  Object.assign(state.issuesView, { openCount: 5, closedCount: 0 });
  countFiledWebIssues(3);
  countFiledWebIssues(0);
  out.badge = badges.issues;
  out.unserved = state.issuesView.unservedOpen;
}
process.stdout.write(JSON.stringify(out));
'''


def _run(scenario):
    functions = "\n".join((
        _function(ISSUES_IO, "countFiledWebIssues"),
        _function(REPO_CONTENT, "applyServedCounts"),
    ))
    completed = subprocess.run(
        ["node", "--input-type=module", "-", scenario],
        input=HARNESS.replace("__FUNCTIONS__", functions),
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(completed.stdout)


node_only = pytest.mark.skipif(
    shutil.which("node") is None, reason="Node.js is unavailable")


@node_only
def test_filing_moves_badge_strip_and_header_by_one():
    result = _run("loaded-list")
    assert result["afterFiling"] == {
        "badge": 12, "strip": 12, "collection": {"open": 12, "closed": 4}}


@node_only
def test_a_stale_served_refresh_keeps_the_new_issue_counted():
    result = _run("loaded-list")
    assert result["afterServed"] == {
        "badge": 12, "strip": 12, "collection": {"open": 12, "closed": 4}}


@node_only
def test_filing_before_the_list_loads_bumps_the_shown_badge():
    result = _run("unloaded-list")
    # Not "1": the badge already showed the repository's open issues.
    assert result["badge"] == 1205
    assert result["strip"] is None
    assert result["afterServed"] == 1205


@node_only
def test_a_converged_submission_is_not_counted_twice():
    result = _run("converged")
    assert result == {"badge": 12, "strip": 12}


@node_only
def test_a_csv_import_counts_every_filed_issue_once():
    result = _run("import")
    assert result == {"badge": 8, "unserved": 3}


def test_submit_paths_move_the_count_instead_of_recounting_rows():
    assert COMPOSE.count("countFiledWebIssues(") == 2
    assert 'setRepoTabCount("issues", state.issuesView.items.filter(' not in COMPOSE
    assert 'setRepoTabCount("issues", state.issuesView.items.filter(' not in DASHBOARD_JS
    load = REPO_CONTENT[REPO_CONTENT.index("  async function loadRepoIssues(repo)"):
                        REPO_CONTENT.index("  async function fetchIssuePage(")]
    # Every list load re-derives the unserved tally from the folder listing.
    assert "state.issuesView.unservedOpen = notYetServed;" in load
    assert "state.issuesView.unservedOpen = state.issuesView.items.filter(" in load
