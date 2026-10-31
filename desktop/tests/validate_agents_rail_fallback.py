#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAUNCH = (ROOT / "src/MainWindowAgentLaunch.cpp").read_text(encoding="utf-8")
NAVIGATION = (ROOT / "src/MainWindowNavigation.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> None:
    overview = between(LAUNCH, "void MainWindow::openAgentsOverview()",
                       "void MainWindow::openRepoAgentsList()")
    assert "repoIndexFor(s.owner, s.name) >= 0" in overview, \
        "agents outside the repository list must not be picked with no repository open"
    assert "openRepoDetail(0, kRepoAgentsTab)" in overview, \
        "with no repository open, Agents must open the first repository on its Agents tab"
    assert overview.index("openRepoDetail(0, kRepoAgentsTab)") < \
        overview.index("m_repoDetailStack->setCurrentIndex(kRepoAgentsTab)"), \
        "the repository must be opened before its Agents tab is shown"

    rail = between(NAVIGATION, "connect(m_agentsNavButton, &QPushButton::clicked",
                   "m_agentDotMatrix = new AgentDotMatrix")
    assert "openAgentsOverview()" in rail
    assert "m_agentsNavButton->setChecked(false)" in rail, \
        "a rail click that opens nothing must not leave Agents checked"
    assert "showSection(kNetworkReposSectionIndex)" in rail
    assert "showPageNotice(" in rail, \
        "a rail click with no repository must say why nothing opened"
    print("agents rail fallback contract ok")


if __name__ == "__main__":
    main()
