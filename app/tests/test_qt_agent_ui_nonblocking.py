"""Agent navigation and diagnostics rendering must yield to the Qt event loop."""

from pathlib import Path


# MainWindowAgents.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_AGENT_SOURCES = (
    "MainWindowAgents.cpp",
    "MainWindowAgentsInternal.h",
    "MainWindowAgentSync.cpp",
    "MainWindowAgentUsage.cpp",
    "MainWindowAgentList.cpp",
    "MainWindowAgentDetail.cpp",
    "MainWindowAgentLaunch.cpp",
    "MainWindowAgentCliSession.cpp",
    "MainWindowAgentEvents.cpp",
    "MainWindowAgentTranscript.cpp",
    "MainWindowAgentLanding.cpp",
    "MainWindowAgentActions.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
AGENTS = "".join(
    (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
ACTIONS = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
INTERNAL = (ROOT / "desktop/src/MainWindowInternal.h").read_text(encoding="utf-8")
SETTINGS = (ROOT / "desktop/src/MainWindowSettings.cpp").read_text(encoding="utf-8")
TRANSCRIPT = (ROOT / "desktop/src/ClaudeTranscriptView.cpp").read_text(
    encoding="utf-8"
)


def _section(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first : source.index(end, first)]


def test_agent_transcript_replay_is_time_sliced_and_stale_safe():
    start = _section(
        AGENTS,
        "void MainWindow::renderTranscriptForSession(int sessionId)",
        "void MainWindow::renderTranscriptSlice(quint64 generation)",
    )
    slices = _section(
        AGENTS,
        "void MainWindow::renderTranscriptSlice(quint64 generation)",
        "void MainWindow::loadEarlierTranscriptEvents()",
    )

    assert "m_agentTranscriptRenderEvents" not in start + slices
    assert "m_agentTranscriptRenderCursor = 0" in start
    assert "m_streamEvents.value(sessionId)" in slices
    assert "QTimer::singleShot(0" in start
    assert "QElapsedTimer budget" in slices
    assert "budget.elapsed() < 8" in slices
    assert "generation != m_agentTranscriptRenderGeneration" in slices
    assert "sessionId != m_selectedAgentSessionId" in slices
    assert "QTimer::singleShot(0" in slices
    assert "renderTranscriptSlice(quint64 generation)" in HEADER


def test_terminal_cards_shape_only_a_bounded_tail():
    helper = _section(
        TRANSCRIPT,
        "QString terminalOutputTail(const QString &text)",
        "// A command's live stdout/stderr behaves like a small terminal",
    )
    terminal = _section(
        TRANSCRIPT,
        "class TerminalOutputView : public QScrollArea",
        "// One transcript row:",
    )

    assert "kMaxChars = 20000" in helper
    assert "kMaxLines = 32" in helper
    assert "open Raw for the full stream" in helper
    assert "terminalOutputTail(text)" in terminal
    assert "linkifiedTerminalHtml(m_displayText" in terminal
    assert "linkifiedTerminalHtml(text" not in terminal


def test_agent_selection_does_not_wait_for_git_or_image_decode():
    metadata = _section(
        AGENTS,
        "void MainWindow::refreshAgentDetailMeta(int sessionId",
        "void MainWindow::applyAgentMetaHtml(",
    )
    worktree = _section(
        AGENTS,
        "QString MainWindow::cachedSessionWorktreeForUi(",
        "bool MainWindow::isStreamTranscriptSession(",
    )
    images = _section(
        AGENTS,
        "void MainWindow::renderAgentPromptImages(int sessionId)",
        "QIcon MainWindow::agentThumbnail(",
    )
    actions = _section(
        AGENTS,
        "void MainWindow::updateAgentFilesTabState(int sessionId)",
        "#ifdef FORKMESH_WINDOW_TESTS",
    )

    assert "cachedSessionWorktreeForUi(" in metadata
    assert "cachedSessionWorktree(" not in metadata
    assert "runOffThread<QString>(" in worktree
    assert "backgroundWorktrees(repoLocal).value(branch)" in worktree
    assert "runOffThread<QHash<QString, QImage>>(" in images
    assert "decodeAgentPromptStripImage(path, bounds)" in images
    assert "QImageReader reader" not in images
    assert "repoDefaultBranchFast()" not in actions
    assert "runGitCapture(" not in actions


def test_agent_table_has_no_content_resizing_selection_reentry():
    build = _section(
        AGENTS,
        "QWidget *MainWindow::buildAgentsTab()",
        "bool MainWindow::agentSessionHasLiveTransport(",
    )
    refresh = _section(
        AGENTS,
        "void MainWindow::refreshAgentTable()",
        "void MainWindow::applyAgentRowCells(",
    )

    assert "setSectionResizeMode(kAgentIdColumn, QHeaderView::Fixed)" in build
    assert "QHeaderView::ResizeToContents" not in build
    # Multi-row selection now updates the selection model directly. It must
    # stay inside the table's signal blocker; selectRow() would synchronously
    # re-enter the detail renderer and discard the other selected rows.
    assert "m_agentTable->selectRow(selRow)" not in refresh
    assert refresh.index("selection->setCurrentIndex(current") < refresh.index(
        "block.unblock()")
    assert "QTimer::singleShot(0, this, [this, selectedId, keep]" in refresh
    assert "m_selectedAgentSessionId = selectedId" not in refresh


def test_external_transcript_read_and_render_are_both_asynchronous():
    render = _section(
        AGENTS,
        "void MainWindow::renderExternalTranscript(int sessionId, bool full)",
        "static constexpr qint64 kScannerIdleMs",
    )

    assert "runOffThread<ExternalTranscriptRead>(" in render
    assert "ClaudeSessionScan::readEvents" in render
    assert "renderExternalTranscriptSlice(generation)" in render
    assert "QElapsedTimer budget" in render
    assert "QTimer::singleShot(" in render
    assert "renderExternalTranscriptSlice(quint64 generation)" in HEADER


def test_periodic_external_session_scan_runs_on_a_worker():
    scan = _section(
        AGENTS,
        "void MainWindow::scanExternalClaudeSessions()",
        "void MainWindow::applyExternalClaudeScan()",
    )
    tick = _section(
        AGENTS,
        "void MainWindow::onExternalClaudeTick()",
        "void MainWindow::renderExternalTranscript(int sessionId, bool full)",
    )

    assert "runOffThread<QList<ExternalClaudeSession>>(" in scan
    assert "ClaudeSessionScan::scan(repoPath" in scan
    assert "generation != m_externalClaudeScanGeneration" in scan
    assert "ClaudeSessionScan::scan(" not in tick
    assert "bool m_externalClaudeScanInFlight = false" in HEADER


def test_cold_action_snapshot_verification_runs_on_a_worker():
    queue = _section(
        ACTIONS,
        "void MainWindow::processActionQueue()",
        "ActionRunner *MainWindow::runnerForRun(",
    )

    assert "m_actionQueueVerificationInFlight" in queue
    assert "runOffThread<ActionQueueVerification>(" in queue
    assert "workflowContentAt(mirror" in queue
    assert "bindActionRepositoryState(&result.verified" in queue
    assert "finishActionQueueVerification" in queue
    assert "bool m_actionQueueVerificationInFlight = false" in HEADER


def test_workflow_scan_carries_snapshot_hashes_back_to_the_gui():
    scan = _section(
        ACTIONS,
        "void MainWindow::queueWorkflowsForCommit(",
        "void MainWindow::applyWorkflowScan(",
    )
    apply = _section(
        ACTIONS,
        "void MainWindow::applyWorkflowScan(",
        "void MainWindow::drainDeferredActionWorkflowScans()",
    )

    assert "scan.snapshotOk = ActionStore::repositoryStateDigest(" in scan
    assert "run.repositoryTree = scan.repositoryTree" in apply
    assert "run.executionDigest = scan.executionDigest" in apply
    assert "bindActionRepositoryState(&run" not in apply


def test_log_views_render_in_event_loop_slices_without_nested_pumps():
    main_log = _section(
        SETTINGS,
        "void MainWindow::rebuildNetworkLogView()",
        "void MainWindow::openFullLogForCategory(",
    )
    popout = _section(
        SETTINGS,
        "void MainWindow::showNetworkLogPopout()",
        "void MainWindow::updateNetworkLogPopoutStatus()",
    )

    assert "renderNetworkLogSlice(generation)" in main_log
    assert "kMaxLinesPerSlice = 16" in main_log
    assert "QTimer::singleShot(0" in main_log
    assert "renderNetworkLogPopoutSlice(generation)" in popout
    assert "kMaxLinesPerSlice = 24" in popout
    assert "processEvents" not in popout
    assert "kNetworkLogSegmentSize = 96" in INTERNAL
