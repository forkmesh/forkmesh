#!/usr/bin/env python3
"""Typing an assignee suggests people instead of demanding an exact name.

Both assignee text fields - the desktop sidebar's inline editor and the web
"Manage issue" form - used to accept a bare comma-separated list with no hint of
who could be named (issue #688). Each now suggests the people already on the
issue first, then the wider user directory, and completes only the name after
the last comma so earlier assignees are kept.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ISSUES = (ROOT / "desktop/src/MainWindowIssues.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
REPO_UI = (ROOT / "app/public/dashboard/js/06-repo-content.js").read_text(encoding="utf-8")
WIRING = (ROOT / "app/public/dashboard/js/08-repo-detail-network.js").read_text(encoding="utf-8")


def _function(source: str, signature: str, end: str = "\n}\n") -> str:
    start = source.index(signature)
    return source[start : source.index(end, start)]


def _js_function(source: str, signature: str) -> str:
    return _function(source, signature, "\n  }\n")


COMPLETER = _function(ISSUES, "class IssueAssigneeCompleter", "\n};\n")


def test_desktop_inline_editor_has_a_list_aware_completer():
    completer = COMPLETER
    # Only the name after the last comma is matched and replaced.
    assert "QStringList splitPath(" in completer
    assert "QString pathFromIndex(" in completer
    assert "lastIndexOf(QLatin1Char(','))" in completer
    assert "m_issueAssigneesEdit->setCompleter(assigneesCompleter);" in ISSUES
    assert "QStringListModel *m_issueAssigneesSuggestionModel" in HEADER


def test_desktop_suggestions_are_the_pickers_people_and_refresh_on_open():
    body = _function(ISSUES, "void MainWindow::refreshIssueAssigneeSuggestions()")
    assert "issuePeopleFor(" in body
    assert "m_chatDirectoryUsers" in body
    edit = _function(ISSUES, "void MainWindow::editIssueAssignees()")
    # The refusal on a mirror still comes first.
    assert edit.index("canWrite()") < edit.index("refreshIssueAssigneeSuggestions();")
    assert "refreshChatUserDirectory();" in edit
    assert ("connect(this, &MainWindow::chatUserDirectoryChanged, this,\n"
            "            &MainWindow::refreshIssueAssigneeSuggestions);") in ISSUES


def test_desktop_enter_on_a_suggestion_does_not_save_the_list():
    # QCompleter forwards Enter to the field after picking, and the field's
    # returnPressed saves - so a highlighted pick is consumed in the popup.
    body = COMPLETER[COMPLETER.index("bool eventFilter(QObject *watched, QEvent *event) override") :]
    assert "watched == popup()" in body
    assert "current.isValid()" in body
    assert "edit->setText(text);" in body
    assert body.index("return true;") < body.index("return QCompleter::eventFilter(watched, event);")
    # Enter with nothing highlighted still saves, as before.
    assert ("connect(m_issueAssigneesEdit, &QLineEdit::returnPressed, this,\n"
            "            &MainWindow::saveIssueAssigneesInline);") in ISSUES


def test_web_assignees_are_picked_from_a_menu_not_typed_blind():
    # The web sidebar's Assignees section opens a picker; the hidden metadata
    # form it writes into is what gets signed.
    assert 'data-repo-issue-picker-open="${picker}"' in REPO_UI
    assert '${sidebarSection("Assignees", assigneeValue, "assignees")}' in REPO_UI
    assert '<input type="hidden" data-repo-issue-assignees' in REPO_UI
    assert "openIssuePicker(field)" in WIRING


def test_web_candidates_put_the_issue_first_then_repository_then_directory():
    body = _js_function(REPO_UI, "  function issuePickerCandidates(field)")
    order = [
        'add(name, "assigned")',
        'add(state.session?.nodeName, "you")',
        '"participant"',
        '"owner"',
        '"active"',
        '`${platformName()} user`',
    ]
    positions = [body.index(needle) for needle in order]
    assert positions == sorted(positions)
    # The directory only joins once a name is typed; it loads when the picker
    # opens.
    assert 'if (String(issuePicker?.query || "").trim())' in body
    assert 'if (field === "assignees") loadRepoIssueAssigneeDirectory();' in REPO_UI
    assert 'fetch("/api/accounts/users"' in REPO_UI


def test_web_typed_names_can_still_be_assigned_and_choices_toggle():
    options = _js_function(REPO_UI, "  function renderIssuePickerOptions()")
    assert "if (needle && !exact) rows.push(row(query.trim()" in options
    toggle = _js_function(REPO_UI, "  function toggleIssuePickerOption(value)")
    assert "issuePicker.selected.splice(index, 1);" in toggle
    assert 'issuePicker.selected.length < 10' in toggle


def test_web_enter_in_the_filter_picks_the_first_suggestion():
    assert 'event.key === "Enter" && event.target?.matches?.("[data-repo-issue-picker-search]")' in WIRING
    assert '$("[data-repo-issue-picker-option]")?.click();' in WIRING
