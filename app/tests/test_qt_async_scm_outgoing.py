"""Source-control ahead counts must not synchronously run git on the GUI."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "desktop/src/MainWindowSourceControl.cpp").read_text(
    encoding="utf-8"
)
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")


def _refresh() -> str:
    return SOURCE.split(
        "void MainWindow::refreshSourceControlOutgoing()", 1
    )[1].split("\n}\n", 1)[0]


def _probe() -> str:
    return SOURCE.split(
        "ScmOutgoingProbe probeSourceControlOutgoing(", 1
    )[1].split("\n}\n", 1)[0]


def test_branch_mirror_and_ahead_probes_run_off_the_gui_thread():
    refresh = _refresh()
    probe = _probe()
    assert "QFutureWatcher<ScmOutgoingProbe>" in refresh
    assert "QtConcurrent::run" in refresh
    assert "probeSourceControlOutgoing(repo)" in refresh
    assert "mirrorBranchCommit" not in refresh
    assert "mirrorHeadBranch" not in refresh
    assert "runGitCapture(" not in refresh
    assert "runGitDetached(" not in refresh
    assert "mirrorBranchCommit" in probe
    assert "runGitCaptureDirect" in probe


def test_outgoing_probe_is_single_flight_and_generation_guarded():
    refresh = _refresh()
    assert "m_scmOutgoingProbeInFlight" in refresh
    assert "m_scmOutgoingProbePending = true" in refresh
    assert "generation != m_scmOutgoingGeneration" in refresh
    assert "repoIndex != m_repoDetailIndex" in refresh
    assert "refreshSourceControlOutgoing()" in refresh
    launch = refresh.split("const bool busy", 1)[1]
    assert launch.index("++m_scmOutgoingGeneration") < launch.index(
        "if (m_scmOutgoingProbeInFlight)"
    )
    assert launch.index("m_scmOutgoingPanel->hide()") < launch.index(
        "if (m_scmOutgoingProbeInFlight)"
    )
    assert "bool m_scmOutgoingProbeInFlight = false" in HEADER
    assert "bool m_scmOutgoingProbePending = false" in HEADER
