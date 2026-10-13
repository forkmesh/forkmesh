import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "desktop/src"
REPO_DETAIL = (SRC / "MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
SHARED = (SRC / "MainWindowShared.cpp").read_text(encoding="utf-8")
CALENDAR = (SRC / "MainWindowCalendar.cpp").read_text(encoding="utf-8")
PULLS = (SRC / "MainWindowPulls.cpp").read_text(encoding="utf-8")
SOURCE_CONTROL = (SRC / "MainWindowSourceControl.cpp").read_text(encoding="utf-8")
HEADER = (SRC / "MainWindow.h").read_text(encoding="utf-8")
WINDOW_TESTS = (ROOT / "desktop/tests/test_window_resize.cpp").read_text(
    encoding="utf-8"
)

GUI_GIT = (
    "runGitCapture(",
    "runGitCaptureDirect(",
    "runGitCaptureInput(",
    "gitCaptureStdout(",
    "gitRefResolves(",
    "gitOutput(",
    "worktreeBranchCommit(",
    "mirrorBranchCommit(",
    "mirrorHeadBranch(",
    "upstreamPublishesThroughMirror(",
    "cachedWorktreeListPorcelain(",
    "cachedRepoRemotesRaw(",
    "readGit(",
    "listRepoBranches(",
    "readBranchesTagsGit(",
    "commitFileStats(",
    "diffImageBlob(",
    "repositoryCalendarEvents(",
    "QProcess::execute",
    "waitForFinished(",
    "waitForGit(",
    "processEvents(",
)


def _body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index("\n}\n", start) + 3]


def _flat(text: str) -> str:
    return re.sub(r"\s+", " ", text)


def _without_workers(body: str) -> str:
    out = []
    rest = body
    while "runOffThread<" in rest:
        head, tail = rest.split("runOffThread<", 1)
        out.append(head)
        depth = 0
        at = tail.index("{")
        for at in range(at, len(tail)):
            depth += {"{": 1, "}": -1}.get(tail[at], 0)
            if depth == 0:
                break
        rest = tail[at + 1 :]
    out.append(rest)
    return "".join(out)


def _assert_no_gui_git(body: str, where: str) -> None:
    for call in GUI_GIT:
        assert call not in body, f"{where} calls {call} on the GUI thread"


def _gui_part(source: str, signature: str) -> str:
    return _without_workers(_body(source, signature))


def test_branch_list_is_read_from_ref_files_and_ordered_by_a_worker():
    branches = _body(REPO_DETAIL, "QStringList MainWindow::repoBranches() const")
    _assert_no_gui_git(branches, "repoBranches")
    assert "readBranchRefFiles(dir)" in branches
    assert "requestRepoBranchOrder(dir)" in branches

    order = _body(REPO_DETAIL, "void MainWindow::requestRepoBranchOrder(")
    assert "runOffThread<BranchOrderLoad>(" in order
    assert "load.branches = listRepoBranches(dir);" in order
    assert "m_branchOrderPendingDir = dir;" in order
    _assert_no_gui_git(_without_workers(order), "requestRepoBranchOrder")

    files = _body(REPO_DETAIL, "MainWindow::BranchRefFiles MainWindow::readBranchRefFiles(")
    _assert_no_gui_git(files, "readBranchRefFiles")
    assert '"/reftable"' in files
    assert "packed-refs" in files

    apply = _body(REPO_DETAIL, "void MainWindow::applyRepoBranchOrder(")
    assert "load.dir != repoGitDir()" in apply
    _assert_no_gui_git(apply, "applyRepoBranchOrder")


def test_default_branch_lookups_never_ask_git_on_the_gui_thread():
    fast = _body(REPO_DETAIL, "QString MainWindow::repoDefaultBranchFast() const")
    _assert_no_gui_git(fast, "repoDefaultBranchFast")
    assert "readBranchRefFiles(dir)" in fast

    gui = _flat(_body(REPO_DETAIL, "QString MainWindow::repoDefaultBranch(const QStringList"))
    assert "repoGitDir(), m_repoBranch, false);" in gui
    choose = _body(REPO_DETAIL, "QString MainWindow::chooseDefaultBranch(")
    assert "if (branch.isEmpty() && askGit) {" in choose

    head = _body(REPO_DETAIL, "QString MainWindow::repoHeadBranch() const")
    _assert_no_gui_git(head, "repoHeadBranch")


def test_browsed_ref_recovery_is_read_on_workers_and_applied_on_the_gui():
    assert "recoverMissingBrowsedRef" not in REPO_DETAIL + HEADER
    assert "static BrowsedRefRecovery readBrowsedRefRecovery(" in HEADER
    read = _body(REPO_DETAIL, "MainWindow::readBrowsedRefRecovery(")
    assert "gitRefResolves(dir, gone)" in read
    apply = _body(REPO_DETAIL, "bool MainWindow::applyBrowsedRefRecovery(")
    _assert_no_gui_git(apply, "applyBrowsedRefRecovery")
    assert "m_repoBranch != gone" in apply
    assert "recovery.dir != repoGitDir()" in apply

    overview = _body(REPO_DETAIL, "MainWindow::OverviewLoad MainWindow::readRepoOverview(")
    assert "readBrowsedRefRecovery(dir, ref, request.recoverBase)" in overview
    assert "applyBrowsedRefRecovery(load.recovery)" in _body(
        REPO_DETAIL, "void MainWindow::applyRepoOverview("
    )
    branches_read = _body(REPO_DETAIL, "MainWindow::readBranchesTagsGit(")
    assert "snap.recovery.baseResolves" in branches_read
    assert "applyBrowsedRefRecovery(snap.recovery)" in _body(
        REPO_DETAIL, "void MainWindow::applyBranchesTags("
    )


def test_branch_and_tag_snapshot_reads_worktrees_and_remotes_only_on_a_worker():
    assert REPO_DETAIL.count("readBranchesTagsGit(snap)") == 1
    load = _body(REPO_DETAIL, "void MainWindow::loadBranchesAndTags()")
    assert "[snap] { return readBranchesTagsGit(snap); }" in load
    _assert_no_gui_git(_without_workers(load), "loadBranchesAndTags")


def test_file_tree_lists_on_a_worker_and_drops_stale_results():
    load = _body(REPO_DETAIL, "void MainWindow::loadRepoFileTree()")
    assert "runOffThread<RepoFileTreeLoad>(" in load
    assert "generation != m_repoFileTreeGen" in load
    assert "m_repoFileTreeReloadPending = true;" in load
    _assert_no_gui_git(_without_workers(load), "loadRepoFileTree")
    read = _body(REPO_DETAIL, "MainWindow::readRepoFileTree(")
    assert '"ls-tree", "-r", "-l", "-z", ref' in read
    assert "m_" not in re.sub(r"\bload\.\w+", "", read)
    _assert_no_gui_git(
        _body(REPO_DETAIL, "void MainWindow::applyRepoFileTree("), "applyRepoFileTree"
    )
    assert "bool testRepoFileTreeLoading() const" in HEADER


def test_commit_issue_closures_read_git_on_a_worker():
    scan = _body(REPO_DETAIL, "void MainWindow::applyCommitIssueClosures()")
    assert "runOffThread<CommitClosureScan>(" in scan
    assert "dir != repoGitDir()" in scan
    assert "ref != currentRef()" in scan
    assert "m_commitClosureScanPending = true;" in scan
    _assert_no_gui_git(_without_workers(scan), "applyCommitIssueClosures")
    read = _body(REPO_DETAIL, "MainWindow::readCommitClosureScan(")
    assert '"rev-parse", ref' in read
    _assert_no_gui_git(
        _body(REPO_DETAIL, "void MainWindow::closeIssuesNamedByCommits("),
        "closeIssuesNamedByCommits",
    )


def test_user_facing_repo_reads_and_actions_run_git_on_workers():
    for signature in (
        "void MainWindow::refreshIssueFilesPanel(",
        "void MainWindow::showCommit(const QString &hash)",
        "void MainWindow::showRepoFileHistory(const QString &path)",
        "void MainWindow::showRepoFileHistoryDialog(",
        "void MainWindow::openRepoFile(const QString &path, std::function<void()> opened)",
        "void MainWindow::showRepoFileContent(",
        "void MainWindow::openRepoReadme()",
        "void MainWindow::openRepoMarkdownLink(",
        "void MainWindow::openRepoMarkdownTarget(",
        "void MainWindow::loadRepoInfo()",
        "void MainWindow::loadRepoInsights()",
        "void MainWindow::renderRepoInsights(",
        "void MainWindow::pullCurrentRepo()",
        "void MainWindow::createAndCheckoutBranch()",
        "void MainWindow::downloadCommitPatch()",
        "void MainWindow::saveCommitPatch(",
        "void MainWindow::deleteCommit(const QString &hash)",
        "void MainWindow::revertCommit(const QString &hash)",
        "void MainWindow::reassignContributorIdentity(const QString &oldName)",
        "void MainWindow::continueReassignContributorIdentity(",
        "void MainWindow::saveRepoFileEdit(",
        "void MainWindow::continueRepoFileEdit(",
        "QString MainWindow::repoFileOpBranch()",
        "void MainWindow::startRepoFileOp(",
        "void MainWindow::newRepoFileEntry(",
        "void MainWindow::renameRepoFileEntry(",
        "void MainWindow::deleteRepoFileEntry(",
        "void MainWindow::toggleCommitFilesRows(int row)",
        "void MainWindow::updateCommitsUnsyncedFilesPanel()",
        "void MainWindow::forkCurrentRepo()",
    ):
        _assert_no_gui_git(_gui_part(REPO_DETAIL, signature), signature)
    assert "prepareRepoFileOp" not in REPO_DETAIL + HEADER


def test_async_results_check_they_are_still_wanted():
    opened = _body(REPO_DETAIL, "void MainWindow::openRepoFile(const QString &path")
    assert "m_repoFileOpenWaiters.take(waitKey)" in opened
    assert "dir + QLatin1Char('\\x1f') + ref + QLatin1Char('\\x1f') + path" in opened
    assert "dir != repoGitDir()" in opened
    info = _body(REPO_DETAIL, "void MainWindow::loadRepoInfo()")
    assert "generation != m_repoInfoLoadGen" in info
    insights = _body(REPO_DETAIL, "void MainWindow::loadRepoInsights()")
    assert "generation != m_insightsLoadGen" in insights
    unsynced = _body(REPO_DETAIL, "void MainWindow::updateCommitsUnsyncedFilesPanel()")
    assert "generation != m_commitsUnsyncedFilesGen" in unsynced
    assert "hashes != m_commitsUnsyncedHashes" in unsynced
    toggle = _body(REPO_DETAIL, "void MainWindow::toggleCommitFilesRows(int row)")
    assert "m_commitFilesLoading.contains(hash)" in toggle
    history = _body(REPO_DETAIL, "void MainWindow::showRepoFileHistoryDialog(")
    assert "!diffGuard || !listGuard" in history


def test_line_jumps_wait_for_the_file_tab():
    at_line = _body(SOURCE_CONTROL, "void MainWindow::openRepoFileAtLine(")
    assert "openRepoFile(path, [this, path, line] {" in at_line
    assert "QTimer::singleShot(0, this, callback);" in _body(
        REPO_DETAIL, "void MainWindow::openRepoFile(const QString &path"
    )
    start = HEADER.index("    void testOpenRepoFile(const QString &path)")
    hook = HEADER[start : HEADER.index("\n    }\n", start)]
    assert "!m_repoFileOpenWaiters.isEmpty()" in hook


def test_calendar_repository_scan_runs_on_a_worker():
    scan = _body(CALENDAR, "void MainWindow::scanRepositoryCalendarSlice()")
    assert "runOffThread<QList<QJsonArray>>(" in scan
    assert "request != m_repositoryCalendarRequest" in scan
    _assert_no_gui_git(_without_workers(scan), "scanRepositoryCalendarSlice")


def test_diff_image_previews_never_wait_for_git_on_the_gui_thread():
    preview = _body(SHARED, "QString diffImagePreviewHtml(")
    assert "diffImageBlob(" not in preview.replace("diffImageBlobForPreview(", "")
    assert preview.count("diffImageBlobForPreview(") == 2
    guarded = _body(SHARED, "QByteArray diffImageBlobForPreview(")
    assert "if (!onGuiThread())" in guarded
    assert "QThreadPool::globalInstance()->start(" in guarded
    assert "cache.warming.contains(key)" in guarded
    prefetch = _body(SHARED, "void prefetchDiffImageBlobs(")
    assert "diffImageBlob(dir, head, file.path);" in prefetch
    assert "prefetchDiffImageBlobs(QString::fromUtf8(patch), dir, base," in REPO_DETAIL
    assert "forkmesh::ui::prefetchDiffImageBlobs(" in WINDOW_TESTS


def test_markdown_preview_pill_reads_both_sides_on_a_worker():
    opener = _body(SHARED, "void openDiffMarkdownPreview(QWidget *parent, const QUrl &url)")
    assert "QtConcurrent::run(" in opener
    assert "if (parent && !guard)" in opener
    assert "diffMarkdownPreviewSide(" not in opener
    assert "openDiffMarkdownPreview(browser, url);" in PULLS
    assert "showDiffMarkdownPreview(browser, url);" not in PULLS


def test_stale_results_never_touch_the_repo_now_on_screen():
    file_op = _body(REPO_DETAIL, "void MainWindow::startRepoFileOp(")
    assert file_op.index("const bool current = dir == repoGitDir();") < file_op.index(
        "closeRepoFileTabsUnder("
    )
    assert file_op.index("if (!current)\n                return;") < file_op.index(
        "closeRepoFileTabsUnder("
    )

    edit = _body(REPO_DETAIL, "void MainWindow::continueRepoFileEdit(")
    assert edit.index("const PullStore store = pullStoreForCurrentRepo();") < edit.index(
        "runOffThread<RepoFileEditCommit>("
    )
    worker = edit[edit.index("runOffThread<RepoFileEditCommit>(") :]
    assert worker.index("writer.createPull(") < worker.index("m_repoFileSaveRunning = false;")
    assert "if (current) {\n                    setRepoDetailNotice(" in edit
    assert "pullStoreForCurrentRepo()" not in worker
    mirror = _body(REPO_DETAIL, "bool MainWindow::sendMirrorEditPull(")
    assert "pullStoreForCurrentRepo()" not in mirror

    readme = _body(REPO_DETAIL, "void MainWindow::openRepoReadme()")
    assert "!m_openFileTabs.isEmpty()" in readme

    info = _body(REPO_DETAIL, "void MainWindow::loadRepoInfo()")
    assert "const QString fallback = repoDefaultBranchFast();" in info
    assert "setRepoBranch(wanted);" in info

    branches = _body(REPO_DETAIL, "QStringList MainWindow::repoBranches() const")
    assert branches.index("if (branches.isEmpty())\n        return branches;") < branches.index(
        "m_branchesCache = branches;"
    )
    order = _body(REPO_DETAIL, "void MainWindow::applyRepoBranchOrder(")
    assert "refreshCommitsBranchButton();" in order

    for signature in (
        "void MainWindow::deleteCommit(const QString &hash)",
        "void MainWindow::revertCommit(const QString &hash)",
    ):
        body = _body(REPO_DETAIL, signature)
        assert "const bool current = dir == repoGitDir();" in body
        assert body.index("if (!current)\n                        return;") < body.index(
            "loadCommits();"
        )
    create = _body(REPO_DETAIL, "void MainWindow::createAndCheckoutBranch()")
    assert "if (dir == repoGitDir())\n                    setRepoDetailNotice(message, true);" in create

    assert "allow(blocking-wait, pump-in-loop)" not in REPO_DETAIL
