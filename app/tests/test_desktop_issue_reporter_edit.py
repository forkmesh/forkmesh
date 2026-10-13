#!/usr/bin/env python3
"""The issue detail column can re-credit a report to the person who filed it.

Reports that arrive through ForkBot intake, the Discord bug channel or an agent
are signed by this node and credited to a name the transcriber guessed, so the
credit is wrong often enough to need fixing by hand. The sidebar's Reporter
section does that with a signed "reporter" event, under the same rules as
Assignees: it only offers the edit where the write can land, and it moves the
displayed credit without rewriting the signed opening event that
test_issue_reporter_leaderboard.py reads for provenance.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ISSUES = (ROOT / "desktop/src/MainWindowIssues.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
STORE = (ROOT / "desktop/src/IssueStore.cpp").read_text(encoding="utf-8")

NOTICE = "The reporter is editable on the repository host."
REFUSAL = (
    "        setIssueInlineNotice(kIssueReporterHostOnlyNotice, true,\n"
    "                             forkmesh::kLocalOperationNoticeKind);\n"
    "        return;\n"
)


def _function(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index("\n}\n", start)]


def test_the_sidebar_has_its_own_reporter_section():
    assert 'addMetaSection("Reporter", m_issueReporterStack, m_issueReporterButton);' in ISSUES
    # The section is a value/editor stack with a gear like the others, and the
    # gear opens the picker.
    assert "m_issueReporterStack->addWidget(m_issueReporterValue);" in ISSUES
    assert ("connect(m_issueReporterButton, &QPushButton::clicked, this,\n"
            "            &MainWindow::pickIssueReporter);") in ISSUES
    for member in ("m_issueReporterValue", "m_issueReporterStack",
                   "m_issueReporterEdit", "m_issueReporterButton",
                   "m_issueReporterSelfButton", "m_issueReporterSaveButton",
                   "m_issueReporterShown"):
        assert member in HEADER, member
    # Cancel puts the section back to the name, like every other editor.
    assert "m_issueReporterStack->setCurrentIndex(0);" in _function(
        ISSUES, "void MainWindow::cancelIssueSidebarEditors("
    )


def test_every_way_into_the_reporter_editor_is_gated():
    assert ISSUES.count(NOTICE) == 1  # the constant, and no hand-copied variant
    for name in ("editIssueReporter", "pickIssueReporter"):
        body = _function(ISSUES, "void MainWindow::%s(" % name)
        assert REFUSAL in body, name
        # The refusal comes before anything that starts an edit.
        assert body.index("canWrite()") < body.index("m_issueReporterEdit"), name
    assert REFUSAL in _function(ISSUES, "void MainWindow::saveIssueReporterInline(")

    # The value label doubles as a way in, so its "credit yourself" step stops
    # when the editor refused to open rather than typing into a hidden page.
    link = ISSUES[ISSUES.index("connect(m_issueReporterValue, &QLabel::linkActivated") :]
    link = link[: link.index("});")]
    assert link.index("editIssueReporter();") < link.index("currentIndex() != 1")

    state = _function(ISSUES, "void MainWindow::updateIssueActionState(")
    gated = state[state.index("for (QPushButton *b :") : state.index("b->setEnabled(writable && haveIssue);")]
    for button in ("m_issueReporterButton", "m_issueReporterSelfButton",
                   "m_issueReporterSaveButton"):
        assert button in gated, button
    assert "m_issueReporterEdit->setEnabled(writable && haveIssue);" in state
    # Writability changes with the selected repository, not only with the
    # issue, so the value label is re-rendered from here too.
    assert "renderIssueReporterValue(m_issueReporterShown);" in state


def test_the_save_appends_a_signed_reporter_event():
    body = _function(ISSUES, "void MainWindow::saveIssueReporterInline(")
    assert body.index("canWrite()") < body.index("store.setReporter(")
    assert "reloadIssues();" in body
    # Both render paths go through the one renderer, so neither can show a
    # stale credit after the write.
    assert "renderIssueReporterValue(issue.authorName);" in ISSUES
    assert "renderIssueReporterValue(QString());" in ISSUES

    store = _function(STORE, "bool IssueStore::setReporter(")
    assert 'ev.type = "reporter";' in store
    assert "makeSignedEvent(number, ev)" in store
    assert store.index("canWrite()") < store.index("readIssueFile")
    # An empty name would erase the credit instead of moving it.
    assert "if (name.isEmpty()) {" in store
    assert 'return ev.reporter;' in _function(
        STORE, "QString IssueStore::contentForSigning("
    )


def test_recrediting_moves_the_display_name_and_leaves_the_open_event_alone():
    fold = _function(STORE, "void IssueStore::recomputeMetadata(")
    assert 'else if (ev.type == "reporter" && !ev.reporter.isEmpty())' in fold
    assert "issue.authorName = ev.reporter;" in fold
    # Nothing else in the fold touches authorName: an issue filed on someone's
    # behalf already stores a top-level name that disagrees with its open
    # event's, and re-folding "open" here would hand it back to the node that
    # typed it (test_issue_reporter_leaderboard.py pins those 40-odd issues).
    assert fold.count("issue.authorName =") == 1

    # The timeline's opening card reads the folded name, so the sidebar, the
    # issue list and the thread all credit the same person.
    thread = _function(ISSUES, "void MainWindow::renderIssueThread(")
    assert "opened.authorName = issue.authorName;" in thread
    assert "addCard(opened, true);" in thread
    # ...and the change itself is on the record.
    assert 'else if (ev.type == "reporter")' in thread
    assert "credited this report to" in thread
