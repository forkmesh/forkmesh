#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = (ROOT / "src/MainWindowRepoWorkflows.cpp").read_text(encoding="utf-8")
TEAMMATES = (ROOT / "src/MainWindowTeammates.cpp").read_text(encoding="utf-8")
DIAGNOSTICS = (ROOT / "src/MainWindowLocalControlDiagnostics.cpp").read_text(encoding="utf-8")
INSIGHTS = (ROOT / "src/MainWindowLocalControlInsights.cpp").read_text(encoding="utf-8")


def action_block(source: str, action: str) -> str:
    start = source.index(f'action == QLatin1String("{action}")')
    end = source.index("\n        if (action == QLatin1String(", start + 1)
    return source[start:end]
PHONE = (ROOT / "src/MainWindowLocalControlWorkflows.cpp").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    begin = source.index(signature)
    return source[begin:source.index("\n}\n", begin)]


def gated(body: str, start: str) -> bool:
    return "repoWorkflowApproved(" in body and body.index("repoWorkflowApproved(") < body.index(start)


def approved_first(body: str, start: str) -> bool:
    marker = "approveRepoWorkflowFileFromPhone("
    return marker in body and body.index(marker) < body.index(start)


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
    assert approved_first(phone, "runNativePackageRepositoryWorkflowAction("), (
        "a run from the paired phone approves the workflow file before it starts")
    lookup = phone[phone.index("repoWorkflowPath(repoIndex, id)"):phone.index("approveRepoWorkflowFileFromPhone(")]
    assert "repoIndex < 0 || path.isEmpty()" in lookup and "return;" in lookup, (
        "a phone run fails closed when the workflow file cannot be found")
    package = function_body(WORKFLOWS, "void MainWindow::runNativePackageRepositoryWorkflowAction(")
    saved = package[package.index("const auto saved = "):package.index('if (action == QLatin1String("save"))')]
    assert "approveRepoWorkflowFile" not in saved, (
        "a package's save callback must not approve every file it writes")
    save = action_block(package, "save")
    assert "fileContent(path) != before" in save and "wasApproved" in save, (
        "saving approves only content the user changed, or keeps an approval the file had")
    toggle = package[package.index('if (action == QLatin1String("setEnabled") || action == QLatin1String("setSchedule"))'):]
    toggle = toggle[:toggle.index('if (action == QLatin1String("openAgent"))')]
    assert "repoWorkflowFileApproved(repoIndex, listing->path)" in toggle and "if (wasApproved)" in toggle, (
        "turning a workflow on keeps an existing approval and never creates one")
    assert toggle.count("keepApproval(") >= 2
    run = action_block(package, "run")
    assert gated(run, "runTeammate(") and gated(run, "startRepoWorkflow("), (
        "a package run of a committed workflow needs the file's approval")

    for action, start in (("queueShortcut", "queueShortcut(info.absoluteFilePath()"),
                          ("runShortcut", "runShortcut(path, repoIndex)")):
        block = INSIGHTS[INSIGHTS.index(f'if (action == QLatin1String("{action}")'):]
        assert approved_first(block, start), f"the phone's {action} must record its approval before running"
    run_block = INSIGHTS[INSIGHTS.index('if (action == QLatin1String("runShortcut"))'):]
    assert approved_first(run_block, "startAdHocAgentForRepo("), (
        "the phone's prompt shortcuts record their approval before starting an agent")
    helper = function_body(WORKFLOWS, "void MainWindow::approveRepoWorkflowFileFromPhone(")
    assert "approveRepoWorkflowFile(repoIndex, path)" in helper and "logSystem(" in helper, (
        "a phone approval goes through the normal approval store and is logged")
    print("repo workflow approval contract passed")


if __name__ == "__main__":
    main()
