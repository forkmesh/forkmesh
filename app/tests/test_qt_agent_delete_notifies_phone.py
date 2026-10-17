from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "desktop" / "src"


def _section(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    return text[begin : text.index(end, begin)]


def test_delete_stored_agent_session_notifies_phone():
    launch = (SRC / "MainWindowAgentLaunch.cpp").read_text(encoding="utf-8")
    body = _section(
        launch,
        "bool MainWindow::deleteStoredAgentSession(",
        "void MainWindow::purgeSessionState(",
    )
    tail = body[body.index("m_agentStore->recycleSession(snapshot)") :]
    assert "scheduleAgentSessionsPush();" in tail
    assert tail.rindex("scheduleAgentSessionsPush();") < tail.rindex("return true;")


def test_recycle_bin_restore_notifies_phone():
    for name, start, end in (
        (
            "MainWindowAgents.cpp",
            "void MainWindow::restoreAllRecycledAgentsInOpenRepo(",
            "flashMessage(",
        ),
        (
            "MainWindowSettings.cpp",
            "connect(restoreButton, &QPushButton::clicked",
            "showLocalNotice(",
        ),
        (
            "MainWindowLocalControlAgentOps.cpp",
            'audit(QStringLiteral("Restoring deleted agents',
            "binReply();",
        ),
    ):
        text = (SRC / name).read_text(encoding="utf-8")
        assert "scheduleAgentSessionsPush();" in _section(text, start, end), name
