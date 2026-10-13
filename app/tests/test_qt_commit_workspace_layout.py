from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
QT_SRC = ROOT / "desktop" / "src"
HEADER = QT_SRC / "MainWindow.h"
REPO_DETAIL = QT_SRC / "MainWindowRepoDetail.cpp"
SOURCE_CONTROL = QT_SRC / "MainWindowSourceControl.cpp"


def _body(source: str, start: str, end: str) -> str:
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j]


def test_commit_workspace_uses_one_right_detail_pane():
    header = HEADER.read_text(encoding="utf-8")
    detail = REPO_DETAIL.read_text(encoding="utf-8")
    build = _body(
        detail,
        "QWidget *MainWindow::buildRepoCommitsTab()",
        "void MainWindow::loadFileSearchIndex()",
    )

    assert "kCommitWorkspaceChangesPage = 0" in header
    assert "kCommitWorkspaceCommitPage = 1" in header
    assert "QWidget *m_scmControlsPanel = nullptr;" in header
    assert "m_gitFilesSlot->addWidget(scmPanel);" in build
    assert "m_gitHistorySlot->addWidget(listPage);" in build
    assert "leftSplit->addWidget(m_gitFilesSlot);" in build
    assert "leftSplit->addWidget(m_gitHistorySlot);" in build
    assert "workspaceSplit->addWidget(leftSplit);" in build
    assert "workspaceSplit->addWidget(m_commitsStack);" in build
    assert "m_commitsStack->addWidget(changesPage);" in build
    assert "m_commitsStack->addWidget(detailPage);" in build
    assert "commitsVSplit" not in build
    assert "outerSplit" not in build
    assert "Select a commit to view its diff" not in build


def test_working_tree_diff_controls_stay_above_changes_list():
    source_control = SOURCE_CONTROL.read_text(encoding="utf-8")
    build = _body(
        source_control,
        "QWidget *MainWindow::buildSourceControlPanel()",
        "void MainWindow::refreshSourceControl()",
    )
    show_one = _body(
        source_control,
        "void MainWindow::showScmDiff(",
        "void MainWindow::showScmDiffAll(",
    )
    show_all = _body(
        source_control,
        "void MainWindow::showScmDiffAll(",
        "void MainWindow::scmSelectAdjacentChange(",
    )

    assert "m_scmControlsPanel = new QWidget;" in build
    # The dense toolbar wraps its buttons onto extra rows via a FlowLayout so
    # every action stays visible instead of scrolling horizontally off the
    # edge, but it must still sit above the changes list in the root layout.
    assert "new FlowLayout(m_scmControlsPanel" in build
    assert "root->addWidget(m_scmControlsPanel);" in build
    assert build.index("root->addWidget(m_scmControlsPanel);") < build.index(
        "root->addWidget(m_scmTree, 1);")
    assert "root->addLayout(composeRow);" in build
    assert build.index("root->addLayout(composeRow);") < build.index(
        "root->addWidget(m_scmControlsPanel);"
    )
    assert "m_scmDiff = new QTextBrowser" not in build
    assert "root->addWidget(m_scmTree, 1);" in build
    assert "setCommitWorkspacePage(kCommitWorkspaceChangesPage)" in show_one
    assert "setCommitWorkspacePage(kCommitWorkspaceChangesPage)" in show_all


def test_commit_detail_switches_to_commit_page():
    detail = REPO_DETAIL.read_text(encoding="utf-8")
    show_commit = _body(
        detail,
        "void MainWindow::showCommit(const QString &hash)",
        "static void refreshCommitMessageLabel",
    )
    render_commit = _body(
        detail,
        "void MainWindow::renderCommitDetail(",
        "void MainWindow::loadRepoInsights()",
    )

    assert "setCommitWorkspacePage(kCommitWorkspaceCommitPage)" in show_commit
    assert "setCommitWorkspacePage(kCommitWorkspaceCommitPage)" in render_commit
