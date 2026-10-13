#!/usr/bin/env python3
"""Closing (and reopening) an issue from the website, with or without a comment.

The lifecycle controls existed but the header button was inert: it rendered
``data-repo-issue-status="closed"`` while handleWebIssueAction only recognized
``"close"``, so the click fell through ``if (!type) return`` and nothing was
signed - and had it matched, it would have posted ``status: "close"``, a value
neither the Qt client nor the Go mirror treats as closed (applyIssueProjection
copies the string verbatim). The controls now route every spelling through one
normalizer, so these tests pin the tokens the DOM actually emits against the
function that consumes them rather than merely asserting both exist.
"""

import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ISSUE_IO = (ROOT / "public/dashboard/js/03-issues-profile-io.js").read_text()
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
WIRING = (ROOT / "public/dashboard/js/08-repo-detail-network.js").read_text()
INTAKE = (ROOT.parent / "server/intake.go").read_text()
ISSUE_VIEW = (ROOT.parent / "desktop/src/MainWindowIssues.cpp").read_text()


def _slice(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def _function(source, name):
    start = source.index(f"function {name}(")
    cursor = source.index("{", start)
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(f"unterminated JavaScript function: {name}")


def _attribute_tokens(source, attribute):
    """Every value a rendered ``attribute="..."`` can carry.

    A static value is read straight off; an interpolated one contributes its
    ternary branches, which is what the click handler will receive.
    """
    tokens = set()
    for match in re.finditer(re.escape(attribute) + '="', source):
        cursor = match.end()
        if not source.startswith("${", cursor):
            tokens.add(source[cursor:source.index('"', cursor)])
            continue
        expression = source[cursor + 2:source.index("}", cursor)]
        branches = re.findall(r'[?:]\s*"([a-z-]*)"', expression)
        assert branches, f"no ternary branches in {attribute}: {expression}"
        tokens.update(branches)
    assert tokens, f"{attribute} is never rendered"
    return tokens


def _normalize(tokens):
    """Run the shipped normalizer over ``tokens`` in node."""
    script = (
        _function(ISSUE_IO, "webIssueStatusForAction")
        + "\nconsole.log(JSON.stringify("
        + json.dumps(sorted(tokens))
        + ".map(webIssueStatusForAction)));"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return dict(zip(sorted(tokens), json.loads(result.stdout)))


def test_normalizer_maps_every_spelling_to_a_canonical_qt_status():
    resolved = _normalize([
        "close", "closed", "Closed", " closed ", "open", "reopen", "reopened",
        "comment", "vote", "delete-issue", "subscribe", "",
    ])
    assert resolved["close"] == "closed"
    assert resolved["closed"] == "closed"
    # Case and stray whitespace come from hand-authored markup, not from a
    # signed payload, so they normalize rather than silently doing nothing.
    assert resolved["Closed"] == "closed"
    assert resolved[" closed "] == "closed"
    assert resolved["open"] == "open"
    assert resolved["reopen"] == "open"
    assert resolved["reopened"] == "open"
    # Everything else is not a lifecycle action; "" is how a plain comment,
    # a vote or a deletion is told apart from a close.
    for other in ("comment", "vote", "delete-issue", "subscribe", ""):
        assert resolved[other] == ""


def test_every_lifecycle_control_emits_a_token_the_handler_resolves():
    # The regression: a control renaming its token turned the close button into
    # a no-op, because nothing tied the markup to the handler's comparisons.
    # Closing lives in the comment box, as on GitHub; there is no separate
    # header button left to drift out of step with the handler.
    assert 'data-repo-issue-status="' not in REPO_UI

    comment_tokens = _attribute_tokens(REPO_UI, "data-repo-issue-comment-action")
    assert comment_tokens == {"comment", "close", "open"}
    resolved = _normalize(comment_tokens)
    # The plain Comment button must NOT move the issue's status.
    assert resolved["comment"] == ""
    assert resolved["close"] == "closed"
    assert resolved["open"] == "open"


def test_status_actions_are_routed_through_the_normalizer():
    handler = _slice(
        ISSUE_IO,
        "async function handleWebIssueAction(action, target = \"\")",
        "async function handleWebIssueTitleSubmit(form)",
    )
    assert "const nextStatus = webIssueStatusForAction(action);" in handler
    assert 'type = "status";' in handler
    assert "fields = { status: nextStatus };" in handler
    # The old shape signed whatever verb the button used as the status itself.
    assert 'fields = { status: action }' not in handler
    # Closing from the detail view also moves the cached list row, its
    # Open/Closed filter and the tab badge.
    assert 'if (type === "status") syncWebIssueListStatus(number, fields.status);' in handler
    # "Close issue" with an empty comment box is the lifecycle change alone,
    # routed through the same normalizing handler.
    submit = _slice(
        ISSUE_IO,
        "async function handleIssueCommentSubmit(repo, form)",
        "function optimisticWebIssueEvent(event)",
    )
    assert "if (!body && webIssueStatusForAction(requestedAction)) {" in submit
    assert "await handleWebIssueAction(requestedAction);" in submit
    assert "event.submitter?.dataset.repoIssueCommentAction" in WIRING


def test_signed_status_event_refuses_a_value_the_mirror_cannot_project():
    submit = _slice(
        ISSUE_IO,
        "async function submitWebIssueEvent(repo, number, type, fields = {})",
        "async function setWebIssueSubscription",
    )
    guard = _slice(submit, 'if (type === "status") {', 'if (type === "edit") {')
    assert "event.status = webIssueStatusForAction(event.status);" in guard
    assert 'if (!event.status) throw new Error("unsupported_issue_action");' in guard
    # The mirror stores the event's status string as-is, which is why an
    # unrecognized value can never be allowed to reach it.
    assert 'issue["status"] = stringValue(event["status"])' in INTAKE
    assert 'status != "closed"' in INTAKE
    # Same two values the desktop client writes.
    assert 'QStringLiteral("closed")' in ISSUE_VIEW
    assert 'closing ? "closed" : "open"' in ISSUE_VIEW


def test_comment_composer_offers_the_lifecycle_button_in_both_directions():
    form = _slice(
        REPO_UI,
        "function renderIssueCommentForm(number)",
        "function renderDiscussionReplyForm(number)",
    )
    assert 'const issueOpen = String(detail?.parsed?.values?.status || "open") !== "closed";' in form
    assert 'data-repo-issue-comment-action="${issueOpen ? "close" : "open"}"' in form
    assert "Close with comment" in form
    assert "Reopen with comment" in form
    # Both buttons submit the one form; the submitter names the action.
    assert form.count('type="submit"') == 2


def test_close_with_comment_posts_the_comment_then_the_status_event():
    handler = _slice(
        ISSUE_IO,
        "async function handleIssueCommentSubmit(repo, form)",
        "  function syncWebIssueListStatus(number, status)",
    )
    assert "await submitWebIssueComment(repo, number, body);" in handler
    assert "const nextStatus = webIssueStatusForAction(action);" in handler
    assert "if (nextStatus) {" in handler
    assert '"status",' in handler
    assert "{ status: nextStatus }," in handler
    assert "syncWebIssueListStatus(number, nextStatus);" in handler
    # The comment is already accepted when the status event runs, so its
    # failure is reported on its own rather than as a failed comment - the
    # generic "could not send the comment" invites a duplicate resend.
    assert "Comment sent, but the issue stayed" in handler
    assert "Comment sent and the issue is closed." in handler
    assert "Comment sent and the issue is reopened." in handler
    # A lifecycle submit re-renders the detail, so the outcome is written to
    # the composer that survives, not the one this handler captured - which
    # means the hint is resolved live and set AFTER the re-render.
    assert 'const hint = document.querySelector("[data-repo-issue-comment-hint]");' in handler
    assert (
        "actionButtons.forEach((button) => { button.disabled = false; });\n"
        "      renderCurrentWebIssueDetail();\n"
        "      setHint("
    ) in handler


def test_closing_moves_the_cached_list_row_and_its_counts():
    sync = _function(ISSUE_IO, "syncWebIssueListStatus")
    assert "const view = state.issuesView;" in sync
    assert "row.status = status;" in sync
    assert "row.state = status;" in sync
    assert 'setRepoTabCount("issues", view.openCount);' in sync
    assert 'setRepoCollectionCounts("issues", view.openCount, view.closedCount);' in sync
    # Undefined counts (a deep-linked issue whose list was never loaded) must
    # not be coerced to 0 and shipped to the badge.
    assert "Number.isFinite(view.openCount) && Number.isFinite(view.closedCount)" in sync
    # A reopened issue leaves the lazily paged closed history.
    assert "view.closedPaths.delete(Number(number));" in sync
    # The tallies the badge is moved from are recorded when the list loads.
    assert "state.issuesView.openCount = openIssues;" in REPO_UI
    assert "state.issuesView.closedCount = closedIssues;" in REPO_UI
