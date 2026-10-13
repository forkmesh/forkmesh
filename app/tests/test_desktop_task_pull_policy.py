#!/usr/bin/env python3
"""The desktop reads the private task catalog on demand, never on a timer.

One read a launch fills the Tasks badge, the first open of the Tasks page
renders that same read instead of asking again, and after that only Refresh —
or the operator's own create/edit/complete/delete — goes back to the relay.
Before this, every visit to the section re-pulled the whole catalog and the
startup read threw its task array away to keep a single count.
"""

from pathlib import Path


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

QT_SRC = Path(__file__).resolve().parents[2] / "desktop" / "src"
QT_TASKS = (QT_SRC / "MainWindowTasks.cpp").read_text(encoding="utf-8")
QT_CHAT = "".join(
    (QT_SRC / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
QT_MAIN = (QT_SRC / "MainWindow.cpp").read_text(encoding="utf-8")


def _function(source, signature):
    body = source[source.index(signature):]
    return body[:body.index("\n}\n")]


def test_startup_arms_one_read_not_a_poll():
    # A single-shot job after launch, not a repeating timer.
    assert QT_MAIN.count("refreshOrganizationTaskBadge();") == 1
    armed = QT_MAIN[:QT_MAIN.index("refreshOrganizationTaskBadge();")]
    assert "QTimer::singleShot(25000" in armed[-400:]


def test_reopening_the_tasks_section_reuses_the_catalog():
    section = _function(QT_CHAT, "void MainWindow::showSection(")
    branch = section[section.index("index == kOrganizationTasksSectionIndex"):]
    branch = branch[:branch.index("else if")]
    assert "if (!m_organizationTasksLoaded)" in branch
    assert "refreshOrganizationTasks();" in branch


def test_the_startup_read_is_what_the_first_open_renders():
    badge = _function(QT_TASKS, "void MainWindow::refreshOrganizationTaskBadge(")
    # A catalog already in hand means the launch has had its one read.
    assert "m_organizationTasksLoading || m_organizationTasksLoaded" in badge
    # The whole payload is kept, not just the open count it needs for the badge.
    assert "m_organizationTasksStartupPayload = payload;" in badge
    builder = _function(QT_TASKS, "QWidget *MainWindow::buildOrganizationTasksSection(")
    assert "applyOrganizationTasks(startup);" in builder


def test_the_refresh_button_is_the_way_back_to_the_relay():
    builder = _function(QT_TASKS, "QWidget *MainWindow::buildOrganizationTasksSection(")
    refresh = builder[
        builder.index('VerticalIconButton(QStringLiteral("Refresh"),'):
    ]
    assert "&MainWindow::refreshOrganizationTasks" in refresh[:600]


def test_task_edit_applies_the_authoritative_patch_response_immediately():
    edit = _function(QT_TASKS, "void MainWindow::editOrganizationTask()")
    assert "settleOrganizationTaskEdit(taskId, task, ok, payload, error," in edit
    settle = _function(QT_TASKS, "bool MainWindow::settleOrganizationTaskEdit(")
    assert "*updated = replaceOrganizationTask(changed);" in settle
    # A list request is only a defensive fallback when the PATCH response did
    # not include the changed row. It must not gate the visible edit, because a
    # concurrent catalog read makes refreshOrganizationTasks() a no-op.
    assert "else if (!updated)\n                refreshOrganizationTasks();" in edit


def test_task_edit_shows_on_save_and_survives_a_stale_catalog_read():
    edit = _function(QT_TASKS, "void MainWindow::editOrganizationTask()")
    # The row changes before the PATCH is even sent, and rolls back on failure.
    optimistic = edit.index("replaceOrganizationTask(optimistic);")
    assert optimistic < edit.index("requestOrganizationTasks(")
    settle = _function(QT_TASKS, "bool MainWindow::settleOrganizationTaskEdit(")
    assert "replaceOrganizationTask(previous);" in settle
    assert "m_organizationTaskLocalEdits.insert(taskId, changed);" in settle
    # A catalog read issued before the write must not put old values back.
    apply = _function(QT_TASKS, "void MainWindow::applyOrganizationTasks(")
    assert "overlayOrganizationTaskLocalEdits();" in apply
    overlay = _function(
        QT_TASKS, "void MainWindow::overlayOrganizationTaskLocalEdits()")
    assert "m_organizationTaskEditsInFlight.contains(id)" in overlay
    assert 'QStringLiteral("updatedAt")' in overlay


def test_ticking_a_task_box_saves_like_an_edit():
    toggle = _function(QT_TASKS, "void MainWindow::toggleOrganizationTaskCheckbox(")
    optimistic = toggle.index("replaceOrganizationTask(optimistic);")
    assert optimistic < toggle.index("requestOrganizationTasks(")
    assert "m_organizationTaskLocalEdits.insert(taskId, optimistic);" in toggle
    assert "settleOrganizationTaskEdit(taskId, task, ok, payload, error," in toggle
    allowed = _function(
        QT_TASKS, "bool MainWindow::organizationTaskCheckboxToggleable(")
    assert "m_organizationTasksCanManage" in allowed
    assert "organizationTaskIsClosed(task)" in allowed
    assert 'QLatin1String("active")' in allowed


def test_nothing_in_the_tasks_page_runs_on_a_timer():
    assert "QTimer::singleShot" not in QT_TASKS
    assert QT_TASKS.count("new QTimer(") == 1
    arm = _function(QT_TASKS, "void MainWindow::updateOrganizationTaskSpinTimer(")
    assert "new QTimer(" in arm
    assert "m_organizationTasksTable->isVisible()" in arm
    assert "organizationTaskRowsHaveWorkingAgent()" in arm
    assert "m_organizationTaskSpinTimer->stop();" in arm
    tick = _function(QT_TASKS, "void MainWindow::tickOrganizationTaskSpinners(")
    assert "m_organizationTasksTable->isVisible()" in tick
    assert "if (!anyWorking && m_organizationTaskSpinTimer)" in tick
    assert "requestOrganizationTasks" not in tick
    assert "refreshOrganizationTasks" not in tick
