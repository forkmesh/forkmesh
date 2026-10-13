"""Working-tree diffs and draft generation must never wait for Git on the UI."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "desktop/src/MainWindowSourceControl.cpp").read_text(
    encoding="utf-8"
)
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
PULLS_SOURCE = (ROOT / "desktop/src/MainWindowPulls.cpp").read_text(
    encoding="utf-8"
)


def _section(start: str, end: str) -> str:
    first = SOURCE.index(start)
    return SOURCE[first : SOURCE.index(end, first)]


def test_combined_patch_git_reads_are_single_flight_on_a_worker():
    reader = _section(
        "ScmCombinedPatchRead readScmCombinedPatch(",
        "// Everything probeSourceControlOutgoing() reads",
    )
    render = _section(
        "void MainWindow::renderScmCombinedDiff()",
        "bool MainWindow::swapScmDiffFileInPlace(",
    )
    all_repositories = _section(
        "void MainWindow::showChangedRepositoriesDiff()",
        "void MainWindow::clearRangeFilesInSourceControl(",
    )

    assert "gitCaptureStdout" in reader
    assert "runGitCapture(" in reader
    assert "runOffThread<ScmCombinedPatchRead>(" in render
    assert "return readScmCombinedPatch(dir);" in render
    assert SOURCE.count("readScmCombinedPatch(") == 3
    assert "runOffThread<QList<ScmRepositoryPatchRead>>(" in all_repositories
    assert "return readChangedRepositoryPatches(checkouts);" in all_repositories
    assert "gitCaptureStdout" not in render
    assert "runGitCapture(" not in render
    assert "m_scmPatchReadInFlight" in render
    assert render.index("m_scmSectionKeys.clear()") < render.index(
        "if (m_scmPatchReadInFlight)"
    )
    assert "patchGeneration == m_scmPatchGeneration" in render
    assert "sourceControlGitDir() == dir" in render
    assert "bool m_scmPatchReadInFlight = false" in HEADER
    assert "quint64 m_scmPatchGeneration = 0" in HEADER


def test_unchanged_status_poll_does_not_starve_slow_workers():
    refresh = _section(
        "void MainWindow::refreshSourceControl(bool force)",
        "void MainWindow::updateScmCommitControlVisibility()",
    )
    unchanged = refresh.index("if (!force && scanKey == m_scmStatusCache)")
    invalidation = refresh.index("++m_scmPatchGeneration", unchanged)
    assert refresh.index("return;", unchanged) < invalidation


def test_automatic_on_device_draft_is_coalesced_and_stale_safe():
    worker = _section(
        "QStringList scmDiffScopeArgsFromGit(",
        "} // namespace\n\nvoid MainWindow::autoFillScmMessage()",
    )
    auto_fill = _section(
        "void MainWindow::autoFillScmMessage()",
        "void MainWindow::finishScmGeneration()",
    )

    assert "gitCaptureStdout" in worker
    assert "runGitCapture(" in worker
    assert "runOffThread<QString>(" in auto_fill
    assert "scmHeuristicCommitMessageFromGit(dir, duration, 0)" in auto_fill
    assert SOURCE.count("scmHeuristicCommitMessageFromGit(") == 3
    assert "gitCaptureStdout" not in auto_fill
    assert "runGitCapture(" not in auto_fill
    assert "m_scmAutoFillInFlight" in auto_fill
    assert "m_scmAutoFillPending" in auto_fill
    assert "requestGeneration == m_scmMessageReadGeneration" in auto_fill
    assert "patchGeneration == m_scmPatchGeneration" in auto_fill
    assert "m_scmStatusGeneration" not in auto_fill
    assert "sourceControlGitDir() == dir" in auto_fill
    assert "bool m_scmAutoFillInFlight = false" in HEADER
    assert "bool m_scmAutoFillPending = false" in HEADER


def test_generate_button_reads_both_local_and_ai_context_off_thread():
    context_reader = _section(
        "QString scmContextDiffFromGit(",
        "QStringList scmDiffScopeArgsFromGit(",
    )
    generate = _section(
        "void MainWindow::generateScmMessage()",
        "QWidget *MainWindow::buildRepoSecurityTab()",
    )

    assert "gitCaptureStdout" in context_reader
    assert "runGitCapture(" in context_reader
    assert generate.count("runOffThread<QString>(") == 2
    assert "scmHeuristicCommitMessageFromGit(dir, duration," in generate
    assert "scmContextDiffFromGit(dir, duration)" in generate
    assert SOURCE.count("scmContextDiffFromGit(") == 2
    assert "gitCaptureStdout" not in generate
    assert "runGitCapture(" not in generate
    assert "patchGeneration == m_scmPatchGeneration" in generate
    assert "m_scmStatusGeneration" not in generate
    assert "requestGeneration != m_scmMessageReadGeneration" in generate
    assert "quint64 m_scmMessageReadGeneration = 0" in HEADER


def test_file_clicks_are_queued_until_the_async_patch_is_rendered():
    scroll = _section(
        "void MainWindow::scrollScmDiffToFile(",
        "QTreeWidgetItem *MainWindow::scmFindItem(",
    )
    assert "m_scmPendingScrollKey = requestedKey" in scroll
    assert "m_scmActiveSectionKey = requestedKey" in scroll
    assert "m_lastSourceControlDiffPath = path" in scroll
    assert "!diffStreamInProgress(m_scmDiff)" in scroll
    assert "m_scmActiveSectionKey = m_scmSectionKeys.at(index)" in PULLS_SOURCE

    render = _section(
        "void MainWindow::renderScmCombinedDiff()",
        "bool MainWindow::swapScmDiffFileInPlace(",
    )
    reset = render.index(
        "m_scmStickySection.clear()", render.index("++m_scmDiffRenders")
    )
    assert reset < render.index("setDiffHtml(m_scmDiff, body")
