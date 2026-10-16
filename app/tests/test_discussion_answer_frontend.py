#!/usr/bin/env python3

import json
import re
import subprocess

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def _function(name):
    start = DASHBOARD_JS.index("  function " + name + "(")
    end = DASHBOARD_JS.index("\n  }\n", start) + 4
    return DASHBOARD_JS[start:end]


def test_answer_events_sign_their_target_and_carry_the_session():
    start = DASHBOARD_JS.index("async function submitWebDiscussionAnswer(")
    body = DASHBOARD_JS[start:DASHBOARD_JS.index("\n  }\n", start)]
    assert 'webEventSigningPrefix("discussion-event", repo)}answer\\n${number}' in body
    assert "sha256HexLower(cleanTarget)" in body
    assert 'type: "answer"' in body and "target: cleanTarget" in body
    assert 'sessionToken: state.session?.sessionToken || ""' in body
    assert 'fetchRepoMutation(repo, "discussions", payload)' in body


def test_only_question_discussions_can_be_answered_by_opener_or_owner():
    start = DASHBOARD_JS.index(
        "async function loadRepoDiscussionAnswerAuthorization(")
    body = DASHBOARD_JS[start:DASHBOARD_JS.index("\n  }\n", start)]
    assert "isQuestionDiscussion(values?.category)" in body
    assert "isRepoOwner(repo)" in body
    assert "opener === webIssueKeyPublic()" in body
    assert '=== "owner"' in body and '"admin"' not in body


def test_the_answer_reply_is_tagged_and_the_button_toggles():
    script = "\n".join([
        "const escapeHtml = (v) => String(v);",
        "const renderPullConversationEvent = (ev) => `<row ${ev.id}>`;",
        _function("renderRepoDiscussionConversation"),
        _function("isQuestionDiscussion"),
        "const events = [{id: 'c1'}, {id: 'c2'}];",
        "const marked = renderRepoDiscussionConversation(events,"
        " {answer: {target: 'c2'}, canMark: true, number: 4});",
        "const viewer = renderRepoDiscussionConversation(events,"
        " {answer: {target: 'c2'}, canMark: false, number: 4});",
        "console.log(JSON.stringify({marked, viewer,"
        " qa: ['Q&A', 'qa', 'Ideas'].map(isQuestionDiscussion)}));",
    ])
    out = json.loads(subprocess.run(
        ["node", "-e", script], check=True, capture_output=True, text=True,
    ).stdout)
    marked = out["marked"]
    assert marked.count("data-repo-discussion-answer-badge") == 1
    assert re.search(r'data-answer="true"[^>]*><row c2>', marked)
    assert 'data-repo-discussion-answer="c1"' in marked
    assert 'data-repo-discussion-answer="" ' in marked
    assert "Clear answer" in marked and "Mark as answer" in marked
    assert "data-repo-discussion-answer=" not in out["viewer"].replace(
        "data-repo-discussion-answer-badge", "")
    assert "data-repo-discussion-answer-badge" in out["viewer"]
    assert out["qa"] == [True, True, False]


def test_the_click_handler_routes_answer_buttons():
    assert 'event.target.closest("[data-repo-discussion-answer]")' in DASHBOARD_JS
    assert "handleDiscussionAnswerClick(state.selectedRepo, " in DASHBOARD_JS
