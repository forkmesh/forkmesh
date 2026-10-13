#!/usr/bin/env python3
"""Static guardrails for branch-only agent launches and explicit PR opt-in."""

from pathlib import Path


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

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
AGENT_RUNNER = (ROOT / "src/AgentRunner.cpp").read_text(encoding="utf-8")
AGENTS = "".join(
    (ROOT / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
CHAT = "".join(
    (ROOT / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main() -> None:
    cli_start = between(
        AGENTS,
        "void MainWindow::startCliTranscript(",
        "QString MainWindow::sessionWorkdir(",
    )
    assert "session.createPr = true" not in cli_start

    composer = between(
        AGENTS,
        "void MainWindow::startAgentFromComposer()",
        "QString MainWindow::saveNewAgentPromptImage(",
    )
    assert "/*createPr=*/false" in composer

    website = between(
        AGENTS,
        "void MainWindow::startWebNewAgentForRepo(",
        "QStringList MainWindow::saveWebAgentImages(",
    )
    assert "/*createPr=*/false" in website

    create_button = between(
        AGENTS,
        "m_agentCreatePrButton = railActionButton(",
        "m_agentCreateIssueButton = railActionButton(",
    )
    assert create_button.index("requested->createPr = true") < create_button.index(
        "maybeCreatePullForStreamSession(sid,"
    )

    assert "m_quickAddCreatePr->setChecked(false);" in CHAT
    assert "Do not open a pull " in AGENT_RUNNER
    assert "request unless the user explicitly asks for one." in AGENT_RUNNER

    teammate_store = (ROOT / "src/TeammateStore.cpp").read_text(encoding="utf-8")
    assert 'QStringLiteral("open-prs")' not in teammate_store
    teammates = (ROOT / "src/MainWindowTeammates.cpp").read_text(encoding="utf-8")
    assert '"Never open a pull request: leave finished work committed on its branch' in teammates
    mcp = (ROOT / "src/MainWindowMcp.cpp").read_text(encoding="utf-8")
    assert "ForkMesh opens the pull request from this branch" not in mcp
    for config in (ROOT.parent / ".forkmesh" / "teammates").glob("*.json"):
        assert '"open-prs"' not in config.read_text(encoding="utf-8"), config


if __name__ == "__main__":
    main()
