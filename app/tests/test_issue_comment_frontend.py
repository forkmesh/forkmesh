#!/usr/bin/env python3
"""Commenting on an issue from the website (adhoc #388).

Issues were read-only on the web: the detail view rendered the signed comment
events already committed to the mirror, but there was no way to add one - unlike
discussions (renderDiscussionReplyForm) and pull requests (renderPullReviewForm).
These contract tests pin the composer, its wiring, and - most importantly - that
the browser signs an issue comment over exactly the bytes the Worker and the
desktop client hash, so the submission verifies instead of 401ing.
"""

import ast
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
EVENTS = ROOT / "src" / "events.py"
DASHBOARD_JS = assembled_dashboard_js()
ISSUE_STORE = (
    ROOT.parent / "desktop" / "src" / "IssueStore.cpp"
).read_text(encoding="utf-8")


def _load_issue_event_content():
    """The Worker's pure canonicalizer, lifted out of the js-importing module."""
    tree = ast.parse(EVENTS.read_text(encoding="utf-8"), filename=str(EVENTS))
    node = next(n for n in tree.body
                if isinstance(n, ast.FunctionDef) and n.name == "issue_event_content")
    namespace = {}
    exec(compile(ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[])),
                 str(EVENTS), "exec"), namespace)
    return namespace["issue_event_content"]


ISSUE_EVENT_CONTENT = _load_issue_event_content()


def _slice(start, end):
    return DASHBOARD_JS[DASHBOARD_JS.index(start):DASHBOARD_JS.index(end)]


def test_web_signs_an_issue_comment_over_body_nul_attachments():
    # An issue comment's signed content is body NUL attachments - NOT the bare
    # body a discussion or pull comment signs. With no attachments that trailing
    # NUL is still part of the hashed bytes; dropping it makes every web comment
    # fail verify_issue_event with bad_signature.
    signer = _slice("async function submitWebIssueComment",
                    "  // Mirrors DiscussionStore::contentForSigning")
    assert 'const NUL = String.fromCharCode(0);' in signer
    assert "const contentHash = await sha256HexLower(cleanBody + NUL);" in signer
    assert (
        "const canonical = `${webEventSigningPrefix(\"issue-event\", repo)}"
        "comment\\n${number}\\n${pub}\\n${ts}\\n${contentHash}`;"
    ) in signer
    # Both counterparts hash the same shape.
    assert ISSUE_EVENT_CONTENT({"type": "comment", "body": "hi"}) == "hi\x00"
    assert 'return ev.body + nul + ev.attachments.join(",");' in ISSUE_STORE


def test_web_issue_comment_posts_a_signed_event_against_the_real_number():
    # Unlike a new issue (signed with the placeholder number 0, renumbered on
    # drain), a comment binds the issue's own number so applyRemoteEvent staples
    # it onto that issue. It rides the same /issues inbox endpoint.
    signer = _slice("async function submitWebIssueComment",
                    "  // Mirrors DiscussionStore::contentForSigning")
    assert 'type: "comment",' in signer
    assert 'id: "comment-web-" + ts,' in signer
    # The Worker re-hashes body + attachments to check the signature, so both
    # must travel in the event JSON.
    assert "body: cleanBody," in signer
    assert "attachments: []," in signer
    assert "const payload = { owner: repo.owner, repo: repo.name, number, event };" in signer
    assert 'fetchRepoMutation(repo, "issues", payload)' in signer
    assert "number: 0" not in signer


def test_issue_detail_renders_a_comment_composer():
    form = _slice("function renderIssueCommentForm(number)",
                  "function renderDiscussionReplyForm(number)")
    # Signing needs an account, same gate as the discussion/pull composers.
    assert "if (!state.session?.nodeName)" in form
    assert "to join this conversation." in form
    assert "data-repo-issue-comment-form" in form
    assert 'data-repo-issue-comment-number="${escapeHtml(number)}"' in form
    assert "data-repo-issue-comment-body" in form
    assert "data-repo-issue-comment-hint" in form
    assert "data-repo-issue-comment-submit" in form


def test_record_detail_mounts_the_composer_for_numbered_issues_only():
    shared = _slice("function renderRepoRecordDetail(repo, kind, number, parsed)",
                    "function renderRepoIssueDetail(repo, number, parsed)")
    detail = _slice("function renderRepoIssueDetail(repo, number, parsed)",
                    "function setRepoIssueRecordOpen(open)")
    # Accepted issues are already numbered, including the immediate projection
    # shown while an online mirror commits the record, and all of them render
    # through the issue page; pulls and discussions never get this composer.
    assert 'if (kind === "issues") return renderRepoIssueDetail(repo, number, parsed);' in shared
    assert "renderIssueCommentForm" not in shared
    assert "${renderIssueCommentForm(number)}" in detail
    # The timeline container is always mounted for issues so an optimistic
    # comment has somewhere to land, even on an issue with no events yet.
    assert 'data-repo-issue-timeline data-empty="${timeline ? "false" : "true"}"' in detail


def test_issue_comment_form_is_wired_to_the_delegated_submit_listener():
    listener = _slice('const issueCommentForm = event.target.closest("[data-repo-issue-comment-form]");',
                      'const discussionReplyForm = event.target.closest(')
    assert "event.preventDefault();" in listener
    assert "handleIssueCommentSubmit(state.selectedRepo, issueCommentForm);" in listener


def test_issue_comment_handler_appends_optimistically_and_maps_inbox_errors():
    handler = _slice("async function handleIssueCommentSubmit(repo, form)",
                     "async function handleDiscussionReplySubmit(repo, form)")
    assert "const number = Number(form.dataset.repoIssueCommentNumber || 0);" in handler
    assert "await submitWebIssueComment(repo, number, body);" in handler
    # The comment only reaches the mirror once the owner's node drains the inbox,
    # so the timeline is appended to locally (same trick the discussion reply
    # form uses) rather than reloaded.
    assert '[data-repo-issue-timeline]' in handler
    assert "timeline.insertAdjacentHTML(\"beforeend\", renderIssueTimelineComment({" in handler
    assert 'timeline.dataset.empty = "false";' in handler
    for code in ("inbox_full", "author_quota", "issue_too_large", "bad_signature"):
        assert f'code === "{code}"' in handler


def test_timeline_comment_row_is_shared_by_history_and_new_comments():
    # The optimistic row must render identically to a drained one, so both go
    # through one renderer.
    row = _slice("function renderIssueTimelineComment(ev)",
                 "  // Full issue activity timeline")
    assert "<span>commented</span>" in row
    # A comment body is markdown, rendered by the same escaping renderer the
    # README pane uses (its contract lives in test_repo_readme_markdown.py).
    # What must never come back is the raw body reaching innerHTML.
    assert "renderRecordMarkdown(body)" in row
    assert "${body}" not in row
    timeline = _slice("function renderIssueTimeline(events)",
                      "function projectJsonPath(number)")
    assert "items.push(renderIssueTimelineComment(ev));" in timeline
