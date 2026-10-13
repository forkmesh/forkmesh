from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
QT_SRC = ROOT / "desktop" / "src"
HEADER = QT_SRC / "MainWindow.h"
TEAMMATES = QT_SRC / "MainWindowTeammates.cpp"


def _body(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def test_teammate_runs_leave_the_current_surface_visible():
    header = HEADER.read_text(encoding="utf-8")
    source = TEAMMATES.read_text(encoding="utf-8")
    team_run = _body(
        source,
        "void MainWindow::runTeammateTeamNow()",
        "void MainWindow::loadTeammateBriefing()",
    )
    schedules = _body(
        source,
        "void MainWindow::checkTeammateSchedules()",
        "void MainWindow::triggerTeammatesForEvent",
    )

    # Launches should leave a person in Team (or wherever they were), with the
    # row's explicit Agent link remaining the route to an agent detail page.
    assert "bool switchToTab = false" in header
    assert "/*switchToTab=*/false" in team_run
    assert "/*switchToTab=*/false" in schedules
