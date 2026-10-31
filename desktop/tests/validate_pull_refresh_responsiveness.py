#!/usr/bin/env python3
"""Guard the pull auto-refresh paths that previously blocked the GUI thread."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PULLS = (ROOT / "src" / "MainWindowPulls.cpp").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = PULLS.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    brace = PULLS.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body: {signature}")
    depth = 0
    for index in range(brace, len(PULLS)):
        if PULLS[index] == "{":
            depth += 1
        elif PULLS[index] == "}":
            depth -= 1
            if depth == 0:
                return PULLS[brace + 1 : index]
    raise AssertionError(f"unterminated body: {signature}")


action_state = function_body("void MainWindow::updatePullActionState()")
behind_worker = function_body("void MainWindow::queuePullBehindCheck(")
background_load = function_body("void MainWindow::reloadPullsInBackground()")
apply_loaded = function_body("void MainWindow::applyLoadedPulls(")

assert "isBranchBehindBase" not in action_state, (
    "updatePullActionState must never run the Git behind-base query on the GUI thread"
)
assert "runOffThread<PullBehindEntry>" in behind_worker
assert "store.isBranchBehindBase" in behind_worker
assert "pullSnapshotDigest(pulls, baseTip, repositoryKey)" in background_load
assert "snapshotDigest == m_pullSnapshotDigest" in apply_loaded
assert apply_loaded.index("snapshotDigest == m_pullSnapshotDigest") < apply_loaded.index(
    "m_currentPulls = std::move(pulls)"
), "unchanged snapshots must return before replacing models or rebuilding widgets"

REPO_SYNC = (ROOT / "src" / "MainWindowRepoSync.cpp").read_text(encoding="utf-8")
converge_start = REPO_SYNC.find('QStringLiteral("Mesh converge %1: %2")')
assert converge_start >= 0, "missing the mesh-converge completion handler"
converge_end = REPO_SYNC.find("*reconciliationFetched &&", converge_start)
assert converge_end > converge_start
converge_done = REPO_SYNC[converge_start:converge_end]
assert "reloadPullsInBackground();" in converge_done
assert "reloadPulls();" not in converge_done, (
    "the timer-driven converge completion must not load pulls on the GUI thread"
)

print("pull refresh responsiveness contract ok")
