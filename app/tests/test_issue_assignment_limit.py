from pathlib import Path

from test_issue_assignment_deadline import BUNDLE, CONTENT, ISSUES_IO, _function, _node

ROOT = Path(__file__).resolve().parents[1]
DOC = (ROOT.parent / "docs" / "app" / "issue-assignment-limit.md").read_text(encoding="utf-8")


def test_assignment_limit_comes_from_issues_json():
    out = _node("""
const read = (text) => repoCollabSettingsFromBlobs(text === null ? {} : { ".forkmesh/issues.json": { content: text } }).assignmentLimit;
console.log(JSON.stringify([
  read('{"assignmentLimit": 2}'), read('{"assignmentLimit": "3"}'), read('{"assignmentLimit": 0}'),
  read('{"assignmentLimit": -1}'), read('{"assignmentLimit": 1.5}'), read('{"assignmentLimit": "many"}'),
  read('{"assignmentLimit": 5000}'), read('{}'), read("not json"), read(null),
]));
""")
    assert out == [2, 3, 0, 1, 1, 1, 100, 1, 1, 1]


def test_an_issue_with_your_linked_pull_request_frees_you_for_another():
    out = _node("""
const issues = [
  { number: 1, status: "open", assignees: ["Alice"], events: [] },
  { number: 2, status: "open", assignees: [], events: [{ type: "claim", assignee: "alice", claimState: "assigned" }] },
  { number: 3, status: "open", assignees: ["alice"], events: [] },
  { number: 4, status: "closed", assignees: ["alice"], events: [] },
  { number: 5, status: "open", assignees: ["bob"], events: [] },
  { number: 6, status: "open", assignees: ["alice"], events: [{ type: "claim", assignee: "alice", claimState: "released" }] },
  { number: 7, status: "open", assignees: ["alice"], deleted: true, events: [] },
  { number: 8, status: "open", assignees: ["alice"], events: [] },
  { number: 9, status: "open", assignees: ["alice"], events: [] },
];
const pulls = [
  { number: 20, author: "alice", state: "open", body: "Fixes #1" },
  { number: 21, author: "alice (unverified)", state: "merged", body: "Closes #3" },
  { number: 22, author: "alice", state: "closed", body: "Fixes #8" },
  { number: 23, author: "bob", state: "open", body: "Fixes #9" },
];
console.log(JSON.stringify({
  held: issueAssignmentsWithoutPull("ALICE", issues, pulls),
  except: issueAssignmentsWithoutPull("alice", issues, pulls, 2),
  noPulls: issueAssignmentsWithoutPull("alice", issues, null),
  nobody: issueAssignmentsWithoutPull("", issues, pulls),
  one: issueAssignmentLimitMessage([8]),
  many: issueAssignmentLimitMessage([2, 8, 9]),
}));
""")
    assert out["held"] == [2, 8, 9]
    assert out["except"] == [8, 9]
    assert out["noPulls"] == [1, 2, 3, 8, 9]
    assert out["nobody"] == []
    assert out["one"] == ("You're already assigned to #8. Link a pull request to it "
                          "(or unassign yourself there) before taking another issue.")
    assert out["many"].startswith("You're already assigned to #2, #8 and #9. Link a pull request to them")


def test_the_claim_checks_the_limit_and_keeps_lists_current():
    action = _function(ISSUES_IO, "handleWebIssueAction")
    assert 'action === "claim" && !detail.parsed?.issueMutationAuthorized' in action
    assert action.index("webIssueClaimBlockers(repo, number)") < action.index("submitWebIssueEvent(")
    assert "syncWebIssueListClaim(number, result.event)" in action
    blockers = _function(ISSUES_IO, "webIssueClaimBlockers")
    assert "loadRepoCollabSettings(repo)" in blockers
    assert "loadRepoIssueRelationIndex(repo)" in blockers and "loadRepoDevelopmentPulls(repo)" in blockers
    assert "if (!Array.isArray(pulls)) return [];" in blockers
    assert "held.length >= limit ? held : []" in blockers


def test_the_comment_form_has_an_assign_button():
    form = _function(CONTENT, "renderIssueCommentForm")
    assert 'data-repo-issue-claim="claim" data-repo-issue-assign-button' in form
    assert "issueOpen && !viewerAssigned" in form
    assert "${assignButton}${canManage ?" in form
    assert "bg-cta" not in form.split("assignButton = ")[1].split(";")[0]
    assert "data-repo-issue-assign-button" in BUNDLE
    assert "function webIssueClaimBlockers(" in BUNDLE


def test_docs_describe_the_rule():
    assert '{"assignmentLimit": 1}' in DOC and ".forkmesh/issues.json" in DOC
    assert "linked" in DOC and "Assign" in DOC
