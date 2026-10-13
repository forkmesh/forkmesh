#!/usr/bin/env python3
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLOUD = (ROOT / "src/MainWindowAgentCloud.cpp").read_text(encoding="utf-8")
FEED = (ROOT / "src/MainWindowAgentFeed.cpp").read_text(encoding="utf-8")
SYNC = (ROOT / "src/MainWindowRepoSync.cpp").read_text(encoding="utf-8")
RELAY = (ROOT.parent / "app/src/agent_streams.py").read_text(encoding="utf-8")


def between(source: str, start: str, end: str | None = None) -> str:
    begin = source.index(start)
    finish = source.index(end, begin) if end is not None else len(source)
    return source[begin:finish]


def main() -> None:
    pump = between(CLOUD, "void MainWindow::pumpCloudAgentStream(bool rang)",
                   "void MainWindow::readCloudAgentStream()")
    assert "const int id = m_selectedAgentSessionId;" in pump
    assert "isCloudSession(id)" in pump
    assert 'it->status == QLatin1String("running")' in pump
    assert "m_agentsPage && m_agentsPage->isVisible()" in pump
    assert "!isMinimized()" in pump
    assert "m_cloudAgentStream.follow(" in pump
    assert "m_cloudAgentStreamEvents.clear();" in pump

    read = between(CLOUD, "void MainWindow::readCloudAgentStream()",
                   "void MainWindow::applyCloudAgentStreamBatch(")
    assert "m_cloudAgentStream.sessionId()" in read
    assert "forkmesh::agents::cloudAgentReadProof()" in read
    assert 'QStringLiteral("after")' in read
    assert "generation != m_cloudAgentStream.generation()" in read

    url = between(CLOUD, "QUrl MainWindow::cloudAgentStreamUrl(",
                  "void MainWindow::pumpCloudAgentStream(")
    assert "cloudAgentSessionUrl(cloud)" in url
    assert 'QStringLiteral("/stream")' in url

    shown = between(CLOUD, "int MainWindow::cloudAgentStreamShownEvents(",
                    "QUrl MainWindow::cloudAgentStreamUrl(")
    assert "m_cloudAgentStream.sessionId() != sessionId" in shown
    assert 'it->status != QLatin1String("running")' in shown

    render = between(CLOUD, "void MainWindow::renderCloudAgentTranscript(int sessionId)",
                     "int MainWindow::cloudAgentStreamShownEvents(")
    assert "const int streamed = cloudAgentStreamShownEvents(sessionId);" in render
    assert "m_renderedCloudStreamEvents == streamed" in render
    assert "m_agentTranscript->handleEvent(m_cloudAgentStreamEvents.at(i)" in render

    apply = between(CLOUD, "void MainWindow::applyCloudAgentSessionSet(",
                    "void MainWindow::announceCloudAgentArrivals(")
    assert "pumpCloudAgentStream();" in apply

    host = between(FEED, "void MainWindow::installAgentActivityHost()")
    tick = between(host, "m_cloudAgentWatchTimer = new QTimer(this);",
                   "m_cloudAgentWatchTimer->start();")
    assert "pumpCloudAgentStream();" in tick

    topic = re.search(r'^TOPIC = "([^"]+)"$', RELAY, re.M).group(1)
    doorbell = between(SYNC, 'if (topic == QLatin1String("%s")) {' % topic,
                       "return;")
    assert "pumpCloudAgentStream(/*rang=*/true);" in doorbell

    print("cloud agent stream contract: ok")


if __name__ == "__main__":
    main()
