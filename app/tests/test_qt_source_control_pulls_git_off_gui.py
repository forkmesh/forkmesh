import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "desktop/src"
SCM = (SRC / "MainWindowSourceControl.cpp").read_text(encoding="utf-8")
PULLS = (SRC / "MainWindowPulls.cpp").read_text(encoding="utf-8")
LANDING = (SRC / "MainWindowAgentLanding.cpp").read_text(encoding="utf-8")
FILES = (SRC / "MainWindowFiles.cpp").read_text(encoding="utf-8")
HEADER = (SRC / "MainWindow.h").read_text(encoding="utf-8")

GUI_GIT = (
    "runGitCapture(",
    "runGitCaptureDirect(",
    "gitCaptureStdout(",
    "runSourceControlMutation(",
    "scmMutate(",
    "stagedCommitAllowed(",
    "waitForGit(",
    "processEvents(",
    "waitForFinished(",
)

WORKER_MARKERS = ("runOffThread<", "queueScmMutation(", "QThread::create(")


def _body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index("\n}\n", start) + 3]


def _flat(text: str) -> str:
    return re.sub(r"\s+", " ", text)


def _next_marker(rest: str):
    hits = [(rest.find(m), m) for m in WORKER_MARKERS if m in rest]
    return min(hits) if hits else (-1, None)


def _without_workers(body: str) -> str:
    out = []
    rest = body
    at, marker = _next_marker(rest)
    while marker is not None:
        out.append(rest[:at])
        tail = rest[at + len(marker) :]
        depth = 0
        i = tail.index("{")
        for i in range(i, len(tail)):
            depth += {"{": 1, "}": -1}.get(tail[i], 0)
            if depth == 0:
                break
        rest = tail[i + 1 :]
        at, marker = _next_marker(rest)
    out.append(rest)
    return "".join(out)


def _assert_no_gui_git(body: str, where: str) -> None:
    for call in GUI_GIT:
        assert call not in body, f"{where} calls {call} on the GUI thread"


def test_source_control_actions_run_git_on_workers():
    for signature in (
        "void MainWindow::scmStagePath(const QString &path)",
        "void MainWindow::scmUnstagePath(const QString &path)",
        "void MainWindow::scmDiscardPath(const QString &path, bool untracked)",
        "void MainWindow::scmIgnorePath(const QString &path)",
        "void MainWindow::scmStageAll()",
        "void MainWindow::scmUnstageAll()",
        "void MainWindow::scmDiscardAll()",
        "void MainWindow::scmStageAllCommitAndPush()",
        "void MainWindow::undoLatestOutgoingCommit()",
        "void MainWindow::confirmScmRevertPathToBase(const QString &path)",
        "void MainWindow::commitWorkingTree(const QString &message,",
        "void MainWindow::showChangedRepositoriesDiff()",
    ):
        body = _body(SCM, signature)
        assert any(m in body for m in WORKER_MARKERS), signature
        _assert_no_gui_git(_without_workers(body), signature)


def test_mutation_retry_sleep_only_runs_off_the_gui_thread():
    callers = [
        m.start() for m in re.finditer(r"runSourceControlMutation\(", SCM)
    ]
    allowed = (
        _body(SCM, "static bool runSourceControlMutation(")
        + _body(SCM, "MainWindow::ScmMutationResult MainWindow::scmMutate(")
        + _body(SCM, "MainWindow::runScmRevertPathToBase(const ScmRevertRequest &request)")
    )
    assert len(callers) == allowed.count("runSourceControlMutation(")
    assert "static ScmMutationResult scmMutate(" in HEADER
    assert "static ScmRevertResult runScmRevertPathToBase(" in HEADER


def test_mutation_queue_serializes_and_drops_repeat_clicks():
    queue = _flat(_body(SCM, "bool MainWindow::queueScmMutation("))
    assert "bool duplicate = m_scmMutationRunning && m_scmMutationRunningKey == key;" in queue
    assert "if (job.key == key) duplicate = true;" in queue
    assert "if (duplicate) { flashMessage(" in queue
    push = _body(SCM, "void MainWindow::scmStageAllCommitAndPush()")
    assert 'QStringLiteral("stage-all-commit-push|") + dir' in push
    rows = _flat(_body(SCM, "void MainWindow::showChangedRepositoriesDiff()"))
    assert rows.count("if (m_scmAllReposView) showChangedRepositoriesDiff(); else refreshSourceControl(true);") == 3
    pump = _flat(_body(SCM, "void MainWindow::pumpScmMutationQueue()"))
    assert "if (m_scmMutationRunning || m_scmMutationQueue.isEmpty()) return;" in pump
    assert "runOffThread<ScmMutationResult>(" in pump
    assert pump.index("apply(result);") < pump.index("pumpScmMutationQueue();", pump.index("apply(result);"))


def test_commit_checks_ratchet_on_worker_and_asks_in_apply():
    commit = _body(SCM, "void MainWindow::commitWorkingTree(const QString &message,")
    flat = _flat(commit)
    assert "if (m_scmCommitInFlight) {" in flat
    assert "RepoStatsStore::stagedCommitAllowed(dir, &ratchetReason)" in commit
    assert "\"Nothing is staged. Stage all changes and commit?\"" in commit
    assert "if (sourceControlGitDir() == dir && repoIndex == m_repoDetailIndex)" in flat
    assert "void commitWorkingTree(const QString &message," in HEADER
    files = _flat(_body(FILES, "void MainWindow::commitFilesTabChanges()"))
    assert "commitWorkingTree(message, [this, message](bool ok) {" in files


def test_revert_worker_is_static_and_apply_checks_the_open_branch():
    run = _body(SCM, "MainWindow::runScmRevertPathToBase(const ScmRevertRequest &request)")
    assert "m_" not in run
    apply = _flat(_body(SCM, "void MainWindow::applyScmRevertPathToBase("))
    assert "m_branchDiffBranch == request.branch" in apply
    assert "for (const QString &key : {absolute, worktreeTabKey(absolute)})" in apply
    assert "if (stillOpen) closeRepoFileTabsUnder(request.path, false);" in apply
    assert apply.index("const bool stillOpen") < apply.index("closeRepoFileTabsUnder(")
    _assert_no_gui_git(apply, "applyScmRevertPathToBase")
    confirm = _flat(_body(SCM, "void MainWindow::confirmScmRevertPathToBase(const QString &path)"))
    assert "if (m_scmRevertInFlight) {" in confirm
    assert "runOffThread<ScmRevertResult>(" in confirm


def test_size_map_lists_ignored_files_on_a_worker():
    pruned = _body(SCM, "QSet<QString> MainWindow::sizeMapPrunedPaths(const QString &path) const")
    _assert_no_gui_git(pruned, "sizeMapPrunedPaths")
    ready = _body(SCM, "bool MainWindow::sizeMapIgnoredPathsReady(")
    assert "runOffThread<QSet<QString>>(" in ready
    assert "if (m_sizeMapIgnoredReading) return false;" in _flat(ready)
    assert "m_sizeMapIgnoredResumes.append(std::move(resume));" in ready
    assert "for (const std::function<void()> &resume : resumes)" in ready
    for signature in (
        "void MainWindow::refreshSizeMapTab(bool force, bool allowElevation)",
        "void MainWindow::rescanSizeMapElevated(bool upfront)",
    ):
        body = _body(SCM, signature)
        assert "sizeMapIgnoredPathsReady(" in body
        assert body.index("sizeMapIgnoredPathsReady(") < body.index("sizeMapPrunedPaths(path)")


def test_agent_pull_runner_ref_publishes_on_workers():
    land = _body(LANDING, "void MainWindow::landAgentPullForSession(")
    _assert_no_gui_git(land.replace("store.createPull(", ""), "landAgentPullForSession")
    assert "publishAgentPullRunnerRef(session, repo, pr);" in land
    publish = _body(LANDING, "void MainWindow::publishAgentPullRunnerRef(")
    _assert_no_gui_git(_without_workers(publish), "publishAgentPullRunnerRef")
    flat = _flat(publish)
    assert flat.index("m_explicitActionPushes.insert(explicitKey);") < flat.index(
        "QStringLiteral(\"push\")"
    )
    assert "const int ri = repoIndexFor(owner, name);" in flat


def test_pull_range_is_cache_only_on_the_gui_thread():
    snapshot = _body(PULLS, "bool MainWindow::pullRangeSnapshot(const QString &base,")
    _assert_no_gui_git(snapshot, "pullRangeSnapshot")
    assert "loadPullRangeSnapshot(dir, base, head)" in snapshot
    assert "static PullRangeSnapshot readPullRangeSnapshot(" in HEADER
    read = _body(PULLS, "MainWindow::readPullRangeSnapshot(const QString &dir,")
    assert "m_" not in read
    load = _flat(_body(PULLS, "void MainWindow::loadPullRangeSnapshot("))
    assert "if (m_pullRangeLoading.contains(key)) return;" in load
    assert "dir != repoGitDir()" in load
    assert "m_pullRangeRerenderQueued" in load
    assert "if (fromFiles || missed == m_pullRangeMissAtMs.constEnd() ||" in snapshot
    rerender = _flat(_body(PULLS, "void MainWindow::rerenderOpenPullRange()"))
    assert "renderPullThread(current);" in rerender
    assert "QScopedValueRollback<bool> busy(m_pullActivityRefreshing, true);" in rerender
    assert "m_pullConversationBaseCount + m_pullActivityExtraCards" in rerender
    ids = _body(PULLS, "QList<int> MainWindow::runIdsForPull(PullRequest pr) const")
    _assert_no_gui_git(ids, "runIdsForPull")


def test_pull_branch_deletes_run_inside_the_delete_workers():
    for signature in (
        "void MainWindow::deletePullAndBranchAsync(",
        "void MainWindow::deleteAllClosedAndMergedPullsAndBranches()",
    ):
        body = _body(PULLS, signature)
        finished = body[body.index("connect(worker, &QThread::finished"):]
        _assert_no_gui_git(finished, signature + " finished slot")
        assert "localBranchExists(" not in finished
        work = body[body.index("QThread::create("):body.index("connect(worker, &QThread::finished")]
        assert '"branch", "-D"' in work
    single = _flat(_body(PULLS, "void MainWindow::deletePullAndBranchAsync("))
    assert "if (*branchTried) {" in single
    assert "could not \" \"be removed: %2\")" in single


def test_pull_actions_read_git_on_workers():
    for signature in (
        "void MainWindow::buildAndPreviewCurrentPull()",
        "void MainWindow::promptNewPullFromSource(const QString &sourceDir,",
        "void MainWindow::showNewPullDialog(",
        "void MainWindow::finishNewPull(",
        "void MainWindow::applyPullSuggestionFix(const QString &threadId)",
        "void MainWindow::aiFixFinish()",
        "void MainWindow::aiFixFail(const QString &message)",
        "void MainWindow::aiFixFinishBranchMerge()",
        "void MainWindow::aiFixFailBranchMerge(const QString &message)",
    ):
        _assert_no_gui_git(_without_workers(_body(PULLS, signature)), signature)
    preview = _flat(_body(PULLS, "void MainWindow::buildAndPreviewCurrentPull()"))
    assert "if (m_currentPullNumber != number || repoGitDir() != gitDir) return;" in preview
    finish = _flat(_body(PULLS, "void MainWindow::aiFixFinish()"))
    assert "if (!m_aiFix || m_aiFixFinishing) return;" in finish
    assert "if (m_aiFix != fix) return;" in finish
    source = _flat(_body(PULLS, "void MainWindow::promptNewPullFromSource(const QString &sourceDir,"))
    assert "m_repositories.at(index).owner != owner" in source
    suggestion = _flat(_body(PULLS, "void MainWindow::applyPullSuggestionFix(const QString &threadId)"))
    assert "if (m_pullSuggestionApplying) {" in suggestion
    assert suggestion.rindex("propagateRepoUpdate(repoIndex); }") < suggestion.rindex(
        "Fix applied and committed"
    )
    assert "The repository changed before the new \" \"pull request dialog" in source
    assert "nothing was \" \"created." in _flat(
        _body(PULLS, "void MainWindow::showNewPullDialog(")
    )


def test_agent_pull_log_names_a_removed_repository():
    publish = _flat(_body(LANDING, "void MainWindow::publishAgentPullRunnerRef("))
    assert "} else if (failure.isEmpty()) {" in publish
    assert "was removed from this app before \" \"its checks could be queued" in publish
