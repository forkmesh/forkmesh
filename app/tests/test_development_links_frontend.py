from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
WIRING = (ROOT / "public/dashboard/js/08-repo-detail-network.js").read_text()
needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")

FUNCTIONS = ("issueReferenceNumbers", "developmentProse", "pullClosingIssueNumbers",
             "pullOwnIssueNumbers", "issueDevelopmentPullStates", "issueLinkedPullNumbers",
             "pullLinkedIssueNumbers", "developmentLinkComment")
CONSTANTS = ("PULL_CLOSING_REFERENCE", "ISSUE_PULL_LINK", "ISSUE_PULL_LINK_LINE")


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    cursor = text.index("{", text.index(")", match.start()))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():cursor + 1]
        cursor += 1


def _const(text, name):
    start = text.index("  const %s =" % name)
    return text[start:text.index(";\n", start) + 2]


def run(expression):
    code = "\n".join([_const(REPO_UI, name) for name in CONSTANTS]
                     + [_function(REPO_UI, name) for name in FUNCTIONS])
    script = "%s\nprocess.stdout.write(JSON.stringify(%s));" % (code, expression)
    out = subprocess.run(["node", "-"], input=script, check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def comment(event_id, body, **extra):
    return {"type": "comment", "id": event_id, "body": body, **extra}


@needs_node
def test_pull_comments_with_closing_keywords_link_issues():
    got = run("""Array.from(pullOwnIssueNumbers(
      { title: "Tidy the header", body: "Follows up on #4." },
      [{ body: "fixes #71" }, { body: "Resolves: #32 and closes issue #8" },
       { body: "see `fixes #99`" }, { body: "prefixes #55" }],
      [{ body: "Fixed #6 in the parser" }]))""")
    assert sorted(got) == [4, 6, 8, 32, 71]


@needs_node
def test_issue_comments_link_and_unlink_pull_requests_in_order():
    events = [
        {"type": "open", "id": "o", "body": "Broken since PR #2 landed."},
        comment("a", "Linked pull request #5\nLinked pull request #9"),
        comment("b", "Unlinked pull request #9"),
        comment("c", "Fixed by #11"),
        comment("d", "fixes #12"),
        comment("e", "Linked pull request #13"),
        {"type": "delete", "id": "x", "target": "e"},
        comment("f", "Resolved in PR #14"),
        {"type": "edit", "id": "y", "target": "f", "body": "Never mind."},
    ]
    states = run("Array.from(issueDevelopmentPullStates(%s))" % json.dumps(events))
    assert dict(states) == {5: True, 9: False, 11: True}


@needs_node
def test_both_pages_agree_on_the_links():
    pulls = [
        {"number": 3, "title": "Fix login", "body": "Fixes #40"},
        {"number": 7, "title": "Unrelated", "body": ""},
        {"number": 8, "title": "Mentions it", "body": "Relates to #40"},
        {"number": 5, "title": "Linked by hand", "body": ""},
    ]
    issue_events = [comment("a", "Linked pull request #5"), comment("b", "Unlinked pull request #8")]
    issue_page = run("issueLinkedPullNumbers(40, %s, %s)" % (
        json.dumps(issue_events), json.dumps(pulls)))
    assert issue_page == [5, 3]
    assert run("issueLinkedPullNumbers(40, %s, null)" % json.dumps(issue_events)) == [5]

    issues = [{"number": 40, "events": issue_events}, {"number": 41, "events": []}]
    for pull in pulls:
        own = "Array.from(pullOwnIssueNumbers(%s))" % json.dumps(pull)
        pull_page = run("pullLinkedIssueNumbers(%d, %s, %s)" % (
            pull["number"], own, json.dumps(issues)))
        assert (40 in pull_page) == (pull["number"] in issue_page)


@needs_node
def test_the_picker_writes_the_lines_the_rules_read():
    body = run("developmentLinkComment([5, 9], true) + '\\n' + developmentLinkComment([3], false)")
    assert body == "Linked pull request #5\nLinked pull request #9\nUnlinked pull request #3"
    states = run("Array.from(issueDevelopmentPullStates([{ type: 'comment', id: 'a', body: %s }]))"
                 % json.dumps(body))
    assert dict(states) == {5: True, 9: True, 3: False}


def test_development_sections_open_a_picker_on_both_pages():
    assert 'sidebarSection("Development", developmentValue, "development"' in REPO_UI
    assert 'data-repo-development="pulls"' in REPO_UI
    assert 'state.session?.nodeName ? "development" : ""' in REPO_UI
    pickers = REPO_UI[REPO_UI.index("let issuePicker = null;"):]
    assert 'saving.finally(focusOpener)' in pickers
    assert "submitWebIssueComment(" in pickers
    assert "openIssuePicker(field)" in WIRING
    review = _function(REPO_UI, "handlePullReviewSubmit")
    assert "linkPullCommentIssues(repo, number, body)" in review
