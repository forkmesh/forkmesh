#!/usr/bin/env python3
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLOUD = (ROOT / "src/MainWindowAgentCloud.cpp").read_text(encoding="utf-8")
AGENTS = (ROOT / "src/MainWindowAgents.cpp").read_text(encoding="utf-8")
FEED = (ROOT / "src/MainWindowAgentFeed.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str | None = None) -> str:
    begin = source.index(start)
    finish = source.index(end, begin) if end is not None else len(source)
    return source[begin:finish]


def main() -> None:
    refresh = between(
        CLOUD,
        "void MainWindow::refreshCloudAgentSessions(bool force)",
        "void MainWindow::applyCloudAgentSessions",
    )
    assert "(!m_agentTable && !forkmesh::agentfeed::watched())" in refresh
    assert "const bool watching = forkmesh::agentfeed::watched();" in refresh
    assert "watching ? kCloudAgentWatchRefreshMs : kCloudAgentActiveRefreshMs" in refresh
    assert "now - m_cloudAgentFetchedAtMs.value(kCloudAccountKey) < maxAge" in refresh
    windows = {
        name: eval(expr)
        for name, expr in re.findall(
            r"constexpr qint64 (kCloudAgent\w*RefreshMs) = ([\d\s*]+);", CLOUD
        )
    }
    assert set(windows) == {
        "kCloudAgentRefreshMs",
        "kCloudAgentActiveRefreshMs",
        "kCloudAgentWatchRefreshMs",
    }, windows
    assert (
        windows["kCloudAgentWatchRefreshMs"]
        < windows["kCloudAgentActiveRefreshMs"]
        < windows["kCloudAgentRefreshMs"]
    ), windows
    assert windows["kCloudAgentWatchRefreshMs"] >= 5000, windows

    apply = between(
        CLOUD,
        "void MainWindow::applyCloudAgentSessions",
        "void MainWindow::injectCloudAgentSessions()",
    )
    assert "feedAgentActivity(id, event);" in apply

    assert "m_cloudAgentWatchTimer" not in AGENTS
    host = between(FEED, "void MainWindow::installAgentActivityHost()")
    tick = between(host, "m_cloudAgentWatchTimer = new QTimer(this);", "m_cloudAgentWatchTimer->start();")
    assert "if (forkmesh::agentfeed::watched())" in tick
    assert "refreshCloudAgentSessions();" in tick

    describe = between(host, "const auto describe =", "const auto describeMachine =")
    assert "if (session.cloud)" in describe
    assert "web run on %2" in describe
    assert "web run queued for %2" in describe

    print("cloud agent watch contract ok")


if __name__ == "__main__":
    main()
