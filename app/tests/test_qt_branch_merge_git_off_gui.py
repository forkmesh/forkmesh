import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "desktop/src"
BRANCHES = (SRC / "MainWindowBranches.cpp").read_text(encoding="utf-8")
CLI = (SRC / "MainWindowAgentCliSession.cpp").read_text(encoding="utf-8")
PULLS = (SRC / "MainWindowPulls.cpp").read_text(encoding="utf-8")
HEADER = (SRC / "MainWindow.h").read_text(encoding="utf-8")

GUI_GIT = (
    "runGitCapture(",
    "runGitCaptureInput(",
    "runGitCaptureBackgrounded(",
    "gitCaptureStdout(",
    "gitRefResolves(",
    "checkoutReleasingWorktree(",
    "cachedWorktreeListPorcelain(",
    "readWorktreePathForBranch(",
    "mergeBaseIntoLinkedWorktree(",
    "unmergedFilesIn(",
    "checkedOutBranch(",
    "readBehindAhead(",
    "commitResolvedMerge(",
    "QProcess::execute(",
    "waitForFinished(",
)


def _body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index("\n}\n", start) + 3]


def _without_workers(body: str) -> str:
    out = []
    rest = body
    while "runOffThread<" in rest:
        head, tail = rest.split("runOffThread<", 1)
        out.append(head)
        at = tail.index("[")
        depth = 0
        for at in range(tail.index("{", at), len(tail)):
            depth += {"{": 1, "}": -1}.get(tail[at], 0)
            if depth == 0:
                break
        rest = tail[at + 1 :]
    out.append(rest)
    return "".join(out)


def _without_offthread_editor_commits(body: str) -> str:
    out = []
    rest = body
    while "runMergeConflictEditor(" in rest:
        head, tail = rest.split("runMergeConflictEditor(", 1)
        out.append(head)
        depth = 1
        at = 0
        while depth:
            depth += {"(": 1, ")": -1}.get(tail[at], 0)
            at += 1
        call = tail[: at - 1]
        assert call.rstrip().endswith("true"), "merge editor commit must run off-thread"
        rest = tail[at:]
    out.append(rest)
    return "".join(out)


def _assert_gui_git_free(source: str, signature: str) -> str:
    body = _body(source, signature)
    gui = _without_offthread_editor_commits(_without_workers(body))
    for call in GUI_GIT:
        assert call not in gui, f"{signature} calls {call} on the GUI thread"
    return body


def _assert_static_worker(source: str, signature: str) -> None:
    body = _body(source, signature)
    stripped = re.sub(r"\b(step|flow|report|summary|read|removal|prep|result|job)\.\w+", "", body)
    assert "m_" not in stripped, f"{signature} touches window state"
    for gui in ("logSystem(", "setRepoDetailNotice(", "flashMessage("):
        assert gui not in body, f"{signature} touches the GUI"


GUI_FUNCTIONS = (
    (CLI, "void MainWindow::updateAllAgentWorktreesFromMain()"),
    (CLI, "void MainWindow::applyAgentWorktreesUpdate("),
    (BRANCHES, "void MainWindow::updateBranchFromBase(const QString &branch)"),
    (BRANCHES, "void MainWindow::applyBranchUpdateStep(BranchGitStep step)"),
    (BRANCHES, "void MainWindow::openBranchMergeEditor(const QString &branch,"),
    (BRANCHES, "void MainWindow::applyBranchEditorStep(BranchGitStep step,"),
    (BRANCHES, "void MainWindow::showBranchGitStep(const BranchGitStep &step)"),
    (BRANCHES, "void MainWindow::requestMergeWorktreeIntoMain("),
    (BRANCHES, "void MainWindow::mergeWorktreeIntoMain(const QString &branchArg,"),
    (BRANCHES, "void MainWindow::applyWorktreeMergeStep("),
    (BRANCHES, "void MainWindow::finishWorktreeMerge("),
    (BRANCHES, "void MainWindow::removeWorktree(const QString &worktreePath,"),
    (BRANCHES, "void MainWindow::deleteWorktreeBranchAndAgent(const QString &worktreePath,"),
    (BRANCHES, "void MainWindow::updateWorktreeSelection(const QString &branch,"),
    (BRANCHES, "void MainWindow::updateWorktreeFromMain(const QString &worktreePath,"),
    (BRANCHES, "void MainWindow::editWorktreeConflicts(const QString &worktreePath,"),
    (BRANCHES, "void MainWindow::resolveWorktreeConflicts()"),
    (BRANCHES, "void MainWindow::commitWorktreeChanges(const QString &worktreePath,"),
    (BRANCHES, "void MainWindow::promptNewBranch()"),
    (BRANCHES, "void MainWindow::deleteBranch(const QString &branch)"),
    (BRANCHES, "void MainWindow::deleteRemoteBranch(const QString &branch)"),
    (BRANCHES, "void MainWindow::confirmDeleteRemoteBranch("),
    (BRANCHES, "void MainWindow::deleteMeshTrackingBranch("),
    (BRANCHES, "void MainWindow::deleteMergedBranches()"),
    (BRANCHES, "void MainWindow::confirmDeleteMergedBranches("),
    (BRANCHES, "void MainWindow::createPullFromBranch(const QString &branch,"),
    (BRANCHES, "void MainWindow::pullBaseIntoAllBranches()"),
    (BRANCHES, "void MainWindow::fixBranchConflictsWithAgent("),
    (BRANCHES, "void MainWindow::startBranchConflictFix("),
    (BRANCHES, "QString MainWindow::branchWorkDir(const QString &branch) const"),
)


def test_branch_and_merge_actions_keep_git_off_the_gui_thread():
    for source, signature in GUI_FUNCTIONS:
        _assert_gui_git_free(source, signature)


def test_git_steps_are_static_workers():
    for decl in (
        "static AgentWorktreeUpdateSummary updateAgentWorktreesFromBase(",
        "static WorktreeMergeReport mergeBaseIntoLinkedWorktree(",
        "static BranchGitStep readBranchUpdate(BranchGitFlow flow);",
        "static BranchGitStep startBranchMergeEditor(BranchGitFlow flow);",
        "static WorktreeMergeStep prepareWorktreeMerge(",
        "static WorktreeMergeStep mergeBranchIntoBase(",
        "static WorktreeRemoval removeWorktreeAndBranch(",
        "static PullAllBranchesResult fastForwardInactiveBranches(",
        "static BranchFixPrep prepareBranchConflictFix(",
        "static QString readWorktreePathForBranch(",
    ):
        assert decl in HEADER, decl
    for source, signature in (
        (CLI, "MainWindow::AgentWorktreeUpdateSummary MainWindow::updateAgentWorktreesFromBase("),
        (BRANCHES, "MainWindow::WorktreeMergeReport\nMainWindow::mergeBaseIntoLinkedWorktree("),
        (BRANCHES, "MainWindow::BranchGitStep MainWindow::readBranchUpdate("),
        (BRANCHES, "MainWindow::BranchGitStep MainWindow::mergeBaseIntoBranchCheckout("),
        (BRANCHES, "MainWindow::BranchGitStep MainWindow::startBranchMergeEditor("),
        (BRANCHES, "MainWindow::BranchGitStep MainWindow::finishBranchMergeEditor("),
        (BRANCHES, "MainWindow::WorktreeMergeStep\nMainWindow::prepareWorktreeMerge("),
        (BRANCHES, "MainWindow::WorktreeMergeStep\nMainWindow::mergeBranchIntoBase("),
        (BRANCHES, "MainWindow::WorktreeRemoval MainWindow::removeWorktreeAndBranch("),
        (BRANCHES, "MainWindow::BranchFixPrep MainWindow::prepareBranchConflictFix("),
    ):
        _assert_static_worker(source, signature)


def test_update_all_agent_branches_runs_one_worker_and_coalesces():
    body = _body(CLI, "void MainWindow::updateAllAgentWorktreesFromMain()")
    assert "if (m_updateAllAgentWorktreesRunning)" in body
    assert "runOffThread<AgentWorktreeUpdateSummary>(" in body
    assert "updateAgentWorktreesFromBase(jobs, unresolved)" in body
    apply = _body(CLI, "void MainWindow::applyAgentWorktreesUpdate(")
    assert "m_updateAllAgentWorktreesRunning = false;" in apply
    assert "reloadAgents(AgentReloadScan::Rescan);" in apply


def test_merge_flow_revalidates_the_repository_before_touching_the_ui():
    apply = _body(BRANCHES, "void MainWindow::applyWorktreeMergeStep(")
    assert "const bool here = repoGitDir() == dir;" in apply
    assert "if (!here) {" in apply
    assert "openBranchMergeEditor(branch, [this, flow, done]" in apply
    finish = _body(BRANCHES, "void MainWindow::finishWorktreeMerge(")
    assert "if (repoGitDir() != dir) {" in finish
    assert "done(landed);" in finish
    merge = _body(BRANCHES, "void MainWindow::mergeWorktreeIntoMain(const QString &branchArg,")
    assert "if (m_worktreeMergeRunning) {" in merge
    assert "m_queuedWorktreeMerges.append(" in merge
    assert "runNextQueuedWorktreeMerge();" in merge
    runner = _body(BRANCHES, "void MainWindow::runNextQueuedWorktreeMerge()")
    assert "m_queuedWorktreeMerges.takeFirst()" in runner
    assert "bindRepoDetailToRepo(index)" in runner
    closed = apply[apply.index("    if (!here) {\n        const QString message ="):]
    closed = closed[: closed.index("\n    }\n")]
    assert "done(false);" in closed
    assert "GitKeepAlive" not in merge


def test_branch_flows_hold_one_busy_flag_and_check_the_repo():
    for signature in (
        "void MainWindow::updateBranchFromBase(const QString &branch)",
        "void MainWindow::openBranchMergeEditor(const QString &branch,",
    ):
        body = _body(BRANCHES, signature)
        assert "if (m_branchMergeFlowBusy) {" in body
        assert "m_branchMergeFlowBusy = true;" in body
    show = _body(BRANCHES, "void MainWindow::showBranchGitStep(const BranchGitStep &step)")
    assert "const bool here = repoGitDir() == step.flow.dir;" in show


def test_merge_editor_commit_runs_on_a_worker():
    editor = _body(PULLS, "bool MainWindow::runMergeConflictEditor(")
    assert "if (commitOffThread) {" in editor
    assert "runOffThread<QPair<bool, QString>>(" in editor
    assert "QPointer<QDialog> guard(&dlg);" in editor
    assert "dlg.committing = true;" in editor
    for signature in (
        "void MainWindow::applyBranchUpdateStep(BranchGitStep step)",
        "void MainWindow::applyBranchEditorStep(BranchGitStep step,",
        "void MainWindow::editWorktreeConflicts(const QString &worktreePath,",
    ):
        assert "true);" in _body(BRANCHES, signature)


def test_worktree_lookups_on_the_gui_thread_read_files():
    lister = _body(BRANCHES, "static bool worktreeListForCaller(")
    assert "QCoreApplication::instance()->thread()" in lister
    assert "worktreeListFromFiles(repoPath, out)" in lister
    member = _body(BRANCHES, "QString MainWindow::worktreePathForBranch(QString repoPath,")
    assert "worktreeListForCaller(repoPath, &out)" in member
    panel = _body(BRANCHES, "void MainWindow::loadWorktreesPanel()")
    assert "cachedWorktreeListPorcelain(" not in panel
    workdir = _body(BRANCHES, "QString MainWindow::branchWorkDir(const QString &branch) const")
    assert "headBranchFromFile(localPath) == branch" in workdir


def test_selection_conflict_probe_is_generation_guarded():
    body = _body(BRANCHES, "void MainWindow::updateWorktreeSelection(const QString &branch,")
    assert "++m_worktreeConflictProbeGen" in body
    assert "probe != m_worktreeConflictProbeGen" in body


def test_window_test_hook_waits_for_the_async_merge():
    hook = HEADER[HEADER.index("bool testMergeBranchAndCleanUp(const QString &branch)"):]
    hook = hook[: hook.index("\n    }\n")]
    assert "mergeWorktreeIntoMain(branch," in hook
    assert "std::make_shared<MergeWait>()" in hook
    assert "testSpinWhile([state] { return !state->finished; });" in hook
    assert "&finished" not in hook
    pull = HEADER[HEADER.index("void testPullBaseIntoAllBranches()"):]
    assert "testSpinWhile([this] { return m_pullAllBranchesRunning; });" in pull[:200]
    click = _body(BRANCHES, "bool MainWindow::testClickBranchPull()")
    assert click.count("testSpinWhile([this] { return m_branchMergeFlowBusy; });") == 2
