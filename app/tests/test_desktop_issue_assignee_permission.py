#!/usr/bin/env python3
"""Assignment is offered only where the assignment can be saved.

Assigning an issue appends a signed event to that repository's own issue log, so
it lands only on a machine hosting a writable checkout. The sidebar used to
offer the whole control anyway on a mirror: the "Assign yourself" link opened
the editor, Save called through to IssueStore::setAssignees(), and the reporter
got a failure toast with their selection thrown away (issue #638).

The control now closes before the edit and says why, which means three things
have to stay true: every way into the editor is gated, the editor's own buttons
are disabled with the rest of the host-only controls, and the read-only sidebar
still says who is assigned - it just doesn't invite a change it can't keep.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ISSUES = (ROOT / "desktop/src/MainWindowIssues.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")

NOTICE = "Assignees are editable on the repository host."
REFUSAL = (
    "        setIssueInlineNotice(kIssueAssigneesHostOnlyNotice, true,\n"
    "                             forkmesh::kLocalOperationNoticeKind);\n"
    "        return;\n"
)


def _function(name: str) -> str:
    start = ISSUES.index("void MainWindow::%s(" % name)
    return ISSUES[start : ISSUES.index("\n}\n", start)]


def test_one_sentence_explains_the_refusal_everywhere():
    # One string, so the tooltip, the sidebar hint and all three refusals agree.
    assert ISSUES.count(NOTICE) == 1  # the constant, and no hand-copied variant
    assert ISSUES.count("kIssueAssigneesHostOnlyNotice") >= 6


def test_every_way_into_the_assignee_editor_is_gated():
    # The gear, which is also disabled, and the inline editor the sidebar link
    # opens: both refuse before showing an editor whose Save cannot land.
    for name in ("editIssueAssignees", "pickIssueAssignees"):
        body = _function(name)
        assert "if (!issueStoreForCurrentRepo().canWrite()) {" in body
        assert REFUSAL in body
        # The refusal comes before anything that starts an edit.
        assert body.index("canWrite()") < body.index("m_issueAssigneesEdit")

    # The sidebar link runs its own "add me" step afterwards, so it has to stop
    # when the editor refused to open rather than typing into a hidden page.
    link = ISSUES[ISSUES.index("connect(m_issueAssigneesValue, &QLabel::linkActivated") :]
    link = link[: link.index("});")]
    assert "m_issueAssigneesStack->currentIndex() != 1" in link
    assert link.index("editIssueAssignees();") < link.index("currentIndex() != 1")


def test_the_save_refuses_in_the_same_words_and_keeps_the_stored_list():
    body = _function("saveIssueAssigneesInline")
    assert body.index("canWrite()") < body.index("store.setAssignees(")
    assert REFUSAL in body
    # A checkout that disappeared mid-edit puts the issue's own list back
    # instead of leaving an unsaved selection on screen.
    assert 'm_issueAssigneesEdit->setText(m_issueAssigneesShown.join(", "));' in body
    assert "cancelIssueSidebarEditors();" in body
    # A write refused on a host that can write is a defect, not guidance, so it
    # keeps reporting to the central error log.
    assert '"Could not update assignees." : error, true);' in body


def test_the_editor_widgets_are_disabled_with_the_other_host_only_controls():
    state = _function("updateIssueActionState")
    gated = state[state.index("for (QPushButton *b :") : state.index("b->setEnabled(writable && haveIssue);")]
    for button in ("m_issueAssigneesButton", "m_issueAssigneesSelfButton",
                   "m_issueAssigneesSaveButton"):
        assert button in gated
    assert "m_issueAssigneesEdit->setEnabled(writable && haveIssue);" in state
    assert ("m_issueAssigneesButton->setToolTip(\n"
            "            writable ? QString() : kIssueAssigneesHostOnlyNotice);") in state
    # Writability changes with the selected repository, not only with the issue,
    # so the value label is re-rendered from here too.
    assert "renderIssueAssigneesValue(m_issueAssigneesShown);" in state
    for member in ("m_issueAssigneesSelfButton", "m_issueAssigneesSaveButton",
                   "m_issueAssigneesShown"):
        assert member in HEADER


def test_the_read_only_sidebar_still_names_the_assignees():
    body = _function("renderIssueAssigneesValue")
    # Who is assigned is public information; only the invitation to change it
    # is host-only, and the hint replaces it.
    assert "for (const QString &a : std::as_const(m_issueAssigneesShown)) {" in body
    assert ('lines << (writable ? QStringLiteral("No one - <a href=\'#\'>Assign yourself</a>")\n'
            '                           : QStringLiteral("No one"));') in body
    assert 'lines << QStringLiteral("<i>%1</i>").arg(kIssueAssigneesHostOnlyNotice);' in body
    assert "if (hostOnly)" in body
    # Both render paths go through it, so neither can re-introduce the link.
    assert ISSUES.count("No one - <a href='#'>Assign yourself</a>") == 2
    assert "renderIssueAssigneesValue({});" in ISSUES
    assert "renderIssueAssigneesValue(issue.assignees);" in ISSUES


def test_repeat_renders_do_not_redraw_avatars():
    # updateIssueActionState() runs after every issue action and each avatar is
    # a paint plus a PNG round trip, so the markup is rebuilt only on a change.
    body = _function("renderIssueAssigneesValue")
    assert "if (key == m_issueAssigneesRenderKey)" in body
    assert body.index("m_issueAssigneesRenderKey = key;") < body.index("roundedAvatar(")
    # Everything the markup reads is in the key, or a change would not repaint.
    key = body[body.index("const QString key =") : body.index("if (key ==")]
    assert "writable ? 1 : 0" in key
    assert "hostOnly ? 1 : 0" in key
    assert "m_issueAssigneesShown.join(QLatin1Char(','))" in key
