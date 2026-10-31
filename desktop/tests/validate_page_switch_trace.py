#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRACE = (ROOT / "src/PageSwitchTrace.h").read_text(encoding="utf-8")
NAVIGATION = (ROOT / "src/MainWindowNavigation.cpp").read_text(encoding="utf-8")
REPO_DETAIL = (ROOT / "src/MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
LAUNCH = (ROOT / "src/MainWindowAgentLaunch.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> None:
    show = between(NAVIGATION, "void MainWindow::showSection(int index)",
                   "void MainWindow::showNetworkTab(")
    assert "PageSwitchScope" in show
    assert show.index("PageSwitchScope") < show.index("ensureSectionBuilt(index)"), \
        "a section switch must be timed from before its page is built"

    build = between(NAVIGATION, "void MainWindow::ensureSectionBuilt(int index)",
                    "void MainWindow::catchUpLogPage()")
    assert "PageSwitchTrace::instance().addBuild" in build

    tab_build = between(REPO_DETAIL, "void MainWindow::ensureRepoDetailTabBuilt(int index)",
                        'logStartup(QStringLiteral("  repo tab %1 built in %2ms")')
    assert "PageSwitchTrace::instance().addBuild" in tab_build

    tab_click = between(REPO_DETAIL, "connect(m_repoDetailTabs, &QButtonGroup::idClicked",
                        "ensureRepoDetailTabBuilt(id);")
    assert "PageSwitchScope" in tab_click, "repository tab clicks must be timed"

    agents = between(LAUNCH, "void MainWindow::switchToAgentsTab(int sessionId)",
                     "showSection(0);")
    assert "PageSwitchScope" in agents, "opening an agent must be timed"

    report = between(TRACE, "void report(const QString &outcome)", "QString m_name;")
    writer = report.index("m_writer.start(")
    assert report.index("QFile file(") > writer, \
        "the switch log must be written off the GUI thread"
    assert "removeEventFilter" in report, \
        "the paint watch must be removed once a switch is reported"
    print("page switch trace contract ok")


if __name__ == "__main__":
    main()
