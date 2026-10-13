#!/usr/bin/env python3
"""Static guardrails for the native organization-agent FIFO/restart contract."""

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

ROOT = Path(__file__).resolve().parents[1]
AGENTS = "".join(
    (ROOT / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
PULLS = (ROOT / "src/MainWindowPulls.cpp").read_text(encoding="utf-8")
STORE = (ROOT / "src/AgentStore.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str | None = None) -> str:
    begin = source.index(start)
    finish = source.index(end, begin) if end is not None else len(source)
    return source[begin:finish]


def main() -> None:
    sync = between(
        PULLS,
        "void MainWindow::performRelaySync()",
    )
    assert "drainOrgAgentJobsFor(repo);" in sync
    assert "reportCompletedOrgAgentJobs();" in sync

    drain = between(
        AGENTS,
        "void MainWindow::drainOrgAgentJobsFor(RepositoryRecord repo)",
        "void MainWindow::applyOrgAgentJobsPayload",
    )
    assert "m_orgAgentJobDrainsInFlight.contains(drainKey)" in drain
    assert "m_orgAgentJobDrainsPending.insert(drainKey)" in drain
    assert "m_orgAgentJobDrainsInFlight.remove(drainKey)" in drain

    result = between(
        AGENTS,
        "void MainWindow::reportOrgAgentJob(",
        "void MainWindow::reportCompletedOrgAgentJobs()",
    )
    success_gate = result.index("if (!ok)")
    clear_binding = result.index("clearOrgAgentBinding(localAgentId)")
    next_claim = result.rindex("drainOrgAgentJobsFor(repo);")
    assert success_gate < clear_binding < next_claim

    safety = between(
        AGENTS,
        "void MainWindow::runOrgAgentSafetyCheck(",
        "void MainWindow::persistOrgAgentBinding(",
    )
    assert safety.index("if (repoIndex < 0)") < safety.index(
        "m_repositories.at(repoIndex)"
    )

    assert 'obj["orgAgentJob"] = orgAgentJob;' in STORE
    assert 'obj.value("orgAgentJob").toObject()' in STORE
    assert "queuedSessionIdsOldestFirst" in STORE


if __name__ == "__main__":
    main()
