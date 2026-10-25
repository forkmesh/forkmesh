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

REPO = Path(__file__).resolve().parents[2]
QT_SRC = REPO / "desktop" / "src"
QT_TASKS = (
    REPO / "marketplace" / "packages" / "tasks" / "content" / "desktop" / "TasksPage.cpp"
).read_text(encoding="utf-8")
QT_STORE = (QT_SRC / "OrganizationTaskStore.cpp").read_text(encoding="utf-8")
QT_BRIDGE = (QT_SRC / "MainWindowTaskBridge.cpp").read_text(encoding="utf-8")
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
    shown = _function(QT_TASKS, "void TasksPage::refresh()")
    assert "if (!m_organizationTasksLoaded)" in shown
    assert "refreshOrganizationTasks();" in shown


def test_the_startup_read_is_what_the_first_open_renders():
    badge = _function(QT_BRIDGE, "void MainWindow::refreshOrganizationTaskBadge(")
    assert "refreshOnce();" in badge
    once = _function(QT_STORE, "void OrganizationTaskStore::refreshOnce(")
    # A catalog already in hand means the launch has had its one read.
    assert "m_loading || m_loaded" in once
    # The whole payload is kept, not just the open count it needs for the badge.
    apply = _function(QT_STORE, "void OrganizationTaskStore::applyPayload(")
    assert "m_payload = payload;" in apply
    builder = _function(QT_TASKS, "QWidget *TasksPage::buildOrganizationTasksSection(")
    assert "applyOrganizationTasks(m_store->payload());" in builder


def test_the_refresh_button_is_the_way_back_to_the_relay():
    builder = _function(QT_TASKS, "QWidget *TasksPage::buildOrganizationTasksSection(")
    refresh = builder[
        builder.index('VerticalIconButton(QStringLiteral("Refresh"),'):
    ]
    assert "&TasksPage::refreshOrganizationTasks" in refresh[:600]


def test_task_edit_applies_the_authoritative_patch_response_immediately():
    assert "patchOrganizationTask(task, body);" in _function(
        QT_TASKS, "void TasksPage::editOrganizationTask()")
    edit = _function(QT_TASKS, "void TasksPage::patchOrganizationTask(")
    assert "settleOrganizationTaskEdit(taskId, task, ok, payload, error," in edit
    settle = _function(QT_TASKS, "bool TasksPage::settleOrganizationTaskEdit(")
    assert "*updated = replaceOrganizationTask(changed);" in settle
    # A list request is only a defensive fallback when the PATCH response did
    # not include the changed row. It must not gate the visible edit, because a
    # concurrent catalog read makes refreshOrganizationTasks() a no-op.
    assert "else if (!updated)\n                refreshOrganizationTasks();" in edit


def test_task_edit_shows_on_save_and_survives_a_stale_catalog_read():
    edit = _function(QT_TASKS, "void TasksPage::patchOrganizationTask(")
    # The row changes before the PATCH is even sent, and rolls back on failure.
    optimistic = edit.index("replaceOrganizationTask(optimistic);")
    assert optimistic < edit.index("requestOrganizationTasks(")
    settle = _function(QT_TASKS, "bool TasksPage::settleOrganizationTaskEdit(")
    assert "replaceOrganizationTask(previous);" in settle
    assert "m_organizationTaskLocalEdits.insert(taskId, changed);" in settle
    # A catalog read issued before the write must not put old values back.
    apply = _function(QT_TASKS, "void TasksPage::applyOrganizationTasks(")
    assert "overlayOrganizationTaskLocalEdits();" in apply
    overlay = _function(
        QT_TASKS, "void TasksPage::overlayOrganizationTaskLocalEdits()")
    assert "m_organizationTaskEditsInFlight.contains(id)" in overlay
    assert 'QStringLiteral("updatedAt")' in overlay


def test_ticking_a_task_box_saves_like_an_edit():
    toggle = _function(QT_TASKS, "void TasksPage::toggleOrganizationTaskCheckbox(")
    optimistic = toggle.index("replaceOrganizationTask(optimistic);")
    assert optimistic < toggle.index("requestOrganizationTasks(")
    assert "m_organizationTaskLocalEdits.insert(taskId, optimistic);" in toggle
    assert "settleOrganizationTaskEdit(taskId, task, ok, payload, error," in toggle
    allowed = _function(
        QT_TASKS, "bool TasksPage::organizationTaskCheckboxToggleable(")
    assert "m_organizationTasksCanManage" in allowed
    assert "taskIsClosed(task)" in allowed
    assert 'QLatin1String("active")' in allowed


def test_nothing_in_the_tasks_page_runs_on_a_timer():
    pieces = QT_TASKS.split("QTimer::singleShot(")
    for before, retry in zip(pieces, pieces[1:]):
        assert "kTaskBackoffKey" in before[-900:]
        assert "refreshOrganizationTasks" not in retry[:300]
    assert QT_TASKS.count("new QTimer(") == 1
    arm = _function(QT_TASKS, "void TasksPage::updateOrganizationTaskSpinTimer(")
    assert "new QTimer(" in arm
    assert "m_organizationTasksTable->isVisible()" in arm
    assert "organizationTaskRowsHaveWorkingAgent()" in arm
    assert "m_organizationTaskSpinTimer->stop();" in arm
    tick = _function(QT_TASKS, "void TasksPage::tickOrganizationTaskSpinners(")
    assert "m_organizationTasksTable->isVisible()" in tick
    assert "if (!anyWorking && m_organizationTaskSpinTimer)" in tick
    assert "requestOrganizationTasks" not in tick
    assert "refreshOrganizationTasks" not in tick
