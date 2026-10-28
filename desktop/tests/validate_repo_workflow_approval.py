#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = (ROOT / "src/MainWindowRepoWorkflows.cpp").read_text(encoding="utf-8")
TEAMMATES = (ROOT / "src/MainWindowTeammates.cpp").read_text(encoding="utf-8")
DIAGNOSTICS = (ROOT / "src/MainWindowLocalControlDiagnostics.cpp").read_text(encoding="utf-8")
PHONE = (ROOT / "src/MainWindowLocalControlWorkflows.cpp").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    begin = source.index(signature)
    return source[begin:source.index("\n}\n", begin)]


def gated(body: str, start: str) -> bool:
    return "repoWorkflowApproved(" in body and body.index("repoWorkflowApproved(") < body.index(start)


def main() -> None:
    for signature in ("void MainWindow::checkRepoWorkflowSchedules()",
                      "void MainWindow::triggerRepoWorkflowsForEvent("):
        body = function_body(WORKFLOWS, signature)
        assert gated(body, "startRepoWorkflow("), (
            f"{signature} starts a committed workflow the user has not approved")

    schedules = function_body(TEAMMATES, "void MainWindow::checkTeammateSchedules()")
    due = schedules[schedules.index("scheduleIsDue(teammate, now)"):]
    assert gated(due, "runTeammate("), (
        "a scheduled special agent from a checkout runs without approval")

    events = function_body(TEAMMATES, "void MainWindow::triggerTeammatesForEvent(")
    matched = events[events.index("scheduleMatchesEvent(teammate, eventName)"):]
    assert gated(matched, "runTeammate("), (
        "an event-triggered special agent from a checkout runs without approval")

    step = WORKFLOWS[WORKFLOWS.index("if (rw::isDesktopStepType(type)) {"):]
    step = step[:step.index("process->start(")]
    assert "forkmesh::sourceDir()" in step and "QString script = checkout" not in step, (
        "desktop steps must run this desktop's own tools/free_disk_space.py")
    assert "m_repositories" not in step

    cleanup = function_body(DIAGNOSTICS, "void MainWindow::runLocalControlFreeDiskSpace(")
    assert "forkmesh::sourceDir()" in cleanup and "m_repositories" not in cleanup, (
        "the phone's disk cleanup must run this desktop's own tools/free_disk_space.py")
    phone = PHONE[PHONE.index('method == QLatin1String("workflows.run")'):]
    assert gated(phone, "runNativePackageRepositoryWorkflowAction("), (
        "a phone can run a committed workflow the desktop has not approved")
    print("repo workflow approval contract passed")


if __name__ == "__main__":
    main()
