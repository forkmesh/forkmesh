#!/usr/bin/env python3
"""Static guardrails for explicit-only organization task linkage."""

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
HEADER = (ROOT / "src/MainWindow.h").read_text(encoding="utf-8")
AGENTS = "".join(
    (ROOT / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
TASKS = (ROOT / "src/MainWindowTaskBridge.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main() -> None:
    declaration = between(
        HEADER,
        "int startAdHocAgentForRepo(",
        "void requestCloudflareWorkspaceName(",
    )
    assert "createOrganizationTask" not in declaration

    issue_launch = between(
        AGENTS,
        "int MainWindow::startAgentForIssue(",
        "const Issue *MainWindow::looperPickNext(",
    )
    assert "session.orgTask = false;" in issue_launch

    prompt_launch = between(
        AGENTS,
        "int MainWindow::startAdHocAgentForRepo(",
        "void MainWindow::startAgentFromComposer(",
    )
    assert "session.orgTask = !orgTaskId.trimmed().isEmpty();" in prompt_launch

    reconciliation = between(
        TASKS,
        "void MainWindow::linkExistingAgentsToOrganizationTasks(",
        "void MainWindow::startOrganizationTaskAgent(",
    )
    assert "openOrgTaskForSession" not in reconciliation


if __name__ == "__main__":
    main()
