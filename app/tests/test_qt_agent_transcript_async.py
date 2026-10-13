"""Guard the GUI call sites that consume asynchronously persisted transcripts."""

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
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")


def _section(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first : source.index(end, first)]


def test_periodic_owner_snapshot_reads_transcripts_off_the_gui_thread():
    read = _section(
        AGENTS,
        "void MainWindow::pushAgentSessionsForRepo(",
        "void MainWindow::pushAgentSessionsForRepoWithTranscripts(",
    )
    seal = _section(
        AGENTS,
        "void MainWindow::pushAgentSessionsForRepoWithTranscripts(",
        "void MainWindow::scheduleAgentSessionsPush(",
    )

    assert "runOffThread<QHash<int, QString>>" in read
    assert "store->readLog(session)" in read
    assert "m_agentSnapshotReadsInFlight.contains(pushKey)" in read
    assert "m_agentSnapshotReadsPending.insert(pushKey)" in read
    assert "m_agentSnapshotReadsPending.remove(pushKey)" in read
    assert "readLog(" not in seal
    assert "MirrorCrypto::sealOwnerPayload(" in seal


def test_completed_org_result_waits_for_exact_log_off_the_gui_thread():
    report = _section(
        AGENTS,
        "void MainWindow::reportCompletedOrgAgentJobs()",
        "void MainWindow::drainAgentPromptsFor(",
    )

    assert "m_orgAgentJobsInFlight.insert(token)" in report
    assert "runOffThread<QString>" in report
    assert "store->readLog(snapshot)" in report
    assert report.index("runOffThread<QString>") < report.index(
        "store->readLog(snapshot)"
    )
    assert "m_orgAgentJobsInFlight.remove(token)" in report


def test_cli_start_has_no_redundant_synchronous_event_reload():
    start = _section(
        AGENTS,
        "void MainWindow::startCliTranscript(",
        "QString MainWindow::sessionWorkdir(",
    )

    assert "ensureStreamEventsLoaded(sid)" not in start
    assert "void MainWindow::ensureStreamEventsLoaded(int sessionId)" not in AGENTS
    assert "void ensureStreamEventsLoaded(int sessionId);" not in HEADER
    assert "ensureStreamEventsLoadedAsync(sessionId)" in AGENTS
