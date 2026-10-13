#!/usr/bin/env python3
"""Guard that issue faces are the real ones, and that they arrive on time.

Everyone's uploaded picture rides in the user directory, which the desktop
fetches over the network — so it lands well after the issue list and the open
issue have been painted. Two things went wrong with that (issue #699):

* the sidebar's Assignees value and the assignee icon on a list row read the
  seeded stand-in portrait directly, so an assignee with a real picture never
  wore it, however long the directory had been loaded;
* every other surface resolved the real picture correctly but cached what it
  drew — the list bakes pixmaps into its row items, the two sidebar values
  skip re-rendering when the names have not moved — and nothing invalidated
  those caches when the directory finally arrived. The faces only corrected
  themselves when an unrelated edit forced a full reload, which is why they
  "showed up when I assigned the issue".

The rules: every issue face resolves through issueTimelineAvatar(), the Issues
tab asks for the directory when it opens, and a directory that brings new
pictures re-resolves the faces already on screen.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ISSUES = (ROOT / "src/MainWindowIssues.cpp").read_text(encoding="utf-8")
MESSAGES = (ROOT / "src/MainWindowMessages.cpp").read_text(encoding="utf-8")
REPO_DETAIL = (ROOT / "src/MainWindowRepoDetail.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def code_only(region: str) -> str:
    """The region with its // comments dropped, for "does not call" checks."""
    return "\n".join(
        line.split("//")[0] for line in region.splitlines())


def main() -> None:
    # The list row's assignee icon and its cache.
    assignee_icon = between(
        ISSUES,
        "QIcon MainWindow::issueAssigneeAvatar(",
        "\n// True when `name` is one of the names",
    )
    assert "issueTimelineAvatar(QString(), key, 18)" in assignee_icon
    assert "forkMeshAvatarPng" not in assignee_icon

    # The detail sidebar's Assignees value.
    sidebar = between(
        ISSUES,
        "void MainWindow::renderIssueAssigneesValue(",
        "\nvoid MainWindow::editIssueAssignees()",
    )
    assert "issueTimelineAvatar(QString(), a, 18)" in sidebar
    assert "forkMeshAvatarPng" not in sidebar

    # The re-resolve pass over what is already drawn.
    refresh = between(
        ISSUES,
        "void MainWindow::refreshIssueAvatars()",
        "\nvoid MainWindow::reloadIssues()",
    )
    # Stand-ins cached before the directory landed must not survive it.
    assert "m_assigneeAvatarCache.clear();" in refresh
    # Both sidebar values key their markup on the names, which have not moved.
    assert "m_issueReporterRenderKey.clear();" in refresh
    assert "m_issueAssigneesRenderKey.clear();" in refresh
    assert "renderIssueReporterValue(m_issueReporterShown);" in refresh
    assert "renderIssueAssigneesValue(m_issueAssigneesShown);" in refresh
    # The list carries its faces in the row items, not in the model.
    assert "IssueAuthorAvatarRole" in refresh
    assert "IssueAssigneeAvatarRole" in refresh
    # The thread is re-faced in place: rebuilding it would scroll the reader
    # back to the top for what is a pixmap change.
    assert "kIssueAvatarSideProperty" in refresh
    refresh_code = code_only(refresh)
    assert "renderIssueThread" not in refresh_code
    assert "refreshIssueList" not in refresh_code

    # The timeline labels carry who they were drawn for, so the pass can find
    # them without the events they came from.
    for builder in ("addIssueThreadCard", "addIssueActivityRow"):
        start = ISSUES.index("void MainWindow::%s(" % builder)
        region = ISSUES[start:start + 2000]
        assert "kIssueAvatarAuthorIdProperty" in region, builder
        assert "kIssueAvatarAuthorNameProperty" in region, builder
        assert "kIssueAvatarSideProperty" in region, builder

    # A directory that brings new pictures re-faces the issue views.
    merge = between(
        MESSAGES,
        "void MainWindow::mergeChatUserDirectory(",
        "\nvoid MainWindow::",
    )
    assert "refreshIssueAvatars();" in merge
    assert merge.index("avatarChanged") < merge.index("refreshIssueAvatars();")
    # And so does a picture pushed over the socket.
    on_avatar = between(
        MESSAGES,
        "void MainWindow::onAvatar(",
        "\nvoid MainWindow::",
    )
    assert "refreshIssueAvatars();" in on_avatar

    # Opening the Issues tab asks for the directory instead of waiting for
    # some other surface to happen to fetch it.
    built = between(
        REPO_DETAIL,
        "void MainWindow::ensureRepoDetailTabBuilt(",
        "\nvoid MainWindow::",
    )
    assert "refreshChatUserDirectory();" in built
    clicked = between(
        REPO_DETAIL,
        "        if (id == 2) {",
        "        else if (id == 3) {",
    )
    assert "refreshChatUserDirectory();" in clicked


if __name__ == "__main__":
    main()
    print("issue directory avatar contract ok")
