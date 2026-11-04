from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()


def _issue_detail_renderer():
    return DASHBOARD_JS.split("  function renderRepoIssueDetail(repo, number, parsed) {", 1)[1].split(
        "\n  function ", 1)[0]


def test_the_issue_author_sees_the_title_and_description_editors():
    block = _issue_detail_renderer()
    assert "const canEditText = canManage || webSessionAuthored(openEvent || { author: values.author, authorName: values.authorName });" in block
    for needle in (
        "${canEditText ? `<form data-repo-issue-title-form",
        "${canEditText ? `<button type=\"button\" data-repo-issue-title-edit",
        "${canEditText && hasBody ? `<button type=\"button\" data-repo-issue-description-edit",
        "</div>${canEditText ? descriptionForm : \"\"}`",
        "interactiveTasks: canEditText })",
    ):
        assert needle in block, needle
    assert "canManage ? `<form data-repo-issue-title-form" not in block


def test_relay_lets_the_issue_opener_retitle_it():
    call = ('        if not ok and event_type == "title":\n'
            "            ok = await _issue_comment_owned_by_submitter(\n"
            "                env, owner, repo, number, event, opening=True)\n")
    assert call in ENTRY
    gate = ENTRY.split("    owner_event_types = {", 1)[1]
    assert gate.index('event_type in ("edit", "delete")') < gate.index(call.strip())
    helper = ENTRY.split("async def _issue_comment_owned_by_submitter", 1)[1].split("\nasync def ", 1)[0]
    assert 'if item.get("type") != "open":' in helper
    assert 'not opening and (not target or target == "self")' in helper
    assert 'event.get("authorVerified")' in helper


def test_a_replayed_title_is_answered_as_already_applied():
    assert 'ISSUE_REPLAY_CHECKED_EVENT_TYPES = frozenset({"comment", "edit", "title", "vote"})' in ENTRY
