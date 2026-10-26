#!/usr/bin/env python3
"""Tasks are paged, not truncated, on every surface that lists them.

The World board used to render ``tasks.slice(0, 100)`` and the Qt table used to
paint one row per task with no bound at all, so an organization past its first
hundred tasks either lost the rest silently or paid for every row at once. All
three surfaces — World, Qt desktop, and the dashboard's own Tasks page — now
render one hundred rows a page with every further page reachable. The Worker
uses smaller transport pages because decrypting a private row is the expensive
operation; clients combine those pages before applying their display pager.
"""

import sys
from pathlib import Path

# MainWindowAgents.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_AGENT_SOURCES = (
    "MainWindowAgents.cpp",
    "MainWindowAgentsInternal.h",
    "MainWindowAgentSync.cpp",
    "MainWindowAgentUsage.cpp",
    "MainWindowAgentList.cpp",
    "MainWindowAgentDetail.cpp",
    "MainWindowAgentLaunch.cpp",
    "MainWindowAgentCliSession.cpp",
    "MainWindowAgentEvents.cpp",
    "MainWindowAgentTranscript.cpp",
    "MainWindowAgentLanding.cpp",
    "MainWindowAgentActions.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
WORLD_PUBLIC = ROOT.parent / "world" / "public" / "world"
SRC = ROOT / "src"
TOOLS = ROOT / "tools"
QT_SRC = ROOT.parent / "desktop" / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import dashboard_shell  # noqa: E402
import build_app_plugins  # noqa: E402

from _dashboard_bundle import assembled_dashboard_js  # noqa: E402

WORLD_TASKS = (WORLD_PUBLIC / "world-office-tasks.js").read_text(encoding="utf-8")
WORLD_TASKS_API = (
    ROOT.parent / "world" / "src" / "world_office_tasks.py"
).read_text(encoding="utf-8")
WORLD_TASKS_RUNTIME = (
    ROOT.parent / "world" / "src" / "world_office_routes.py"
).read_text(encoding="utf-8")
WORLD = (WORLD_PUBLIC / "world.js").read_text(encoding="utf-8")
WORLD_CSS = (WORLD_PUBLIC / "world.css").read_text(encoding="utf-8")
QT_TASKS = (
    ROOT.parent / "marketplace" / "packages" / "tasks" / "content" / "desktop"
    / "TasksPage.cpp"
).read_text(encoding="utf-8")
QT_TASK_STORE = (QT_SRC / "OrganizationTaskStore.cpp").read_text(encoding="utf-8")
QT_TASK_BRIDGE = (QT_SRC / "MainWindowTaskBridge.cpp").read_text(encoding="utf-8")
TASKS_VIEW = (
    ROOT.parent / "marketplace" / "packages" / "tasks" / "content" / "web"
    / "view.html"
).read_text(encoding="utf-8")


def test_world_board_pages_the_catalog_instead_of_dropping_it():
    # The truncating slice is gone: the controller keeps the whole catalog the
    # relay returns and cuts a page out of it at render time. (The remaining
    # 100-slice in this file bounds the Marketing member list, not tasks.)
    assign = WORLD_TASKS[WORLD_TASKS.index("tasks = Array.isArray(payload?.tasks)"):]
    assert ".slice(0, 100)" not in assign[:assign.index("announceAgentTasks")]
    assert "const TASK_PAGE_SIZE = 100;" in WORLD_TASKS
    assert "const MAX_TASKS = 2000;" in WORLD_TASKS
    assert ".slice(0, MAX_TASKS)" in WORLD_TASKS
    assert "async function fetchTaskCatalog(options)" in WORLD_TASKS
    assert "page?.hasMore !== true" in WORLD_TASKS
    assert "?offset=${offset}" in WORLD_TASKS
    assert "TASK_LIST_PAGE_SIZE = 25" in WORLD_TASKS_API
    assert "parse_qs(urlparse(str(self.request.url)).query)" in (
        WORLD_TASKS_RUNTIME
    )
    assert "URL(self.request.url).searchParams" not in WORLD_TASKS_RUNTIME
    for contract in (
        "function taskPageCount(",
        "function currentTaskPage(",
        "function taskPagerHTML(",
        "pageTasks.map(taskBoardHTML)",
        'data-world-task-page="prev"',
        'data-world-task-page="next"',
    ):
        assert contract in WORLD_TASKS, contract


def test_world_pager_is_rendered_and_styled():
    assert "data-world-organization-task-pager" in WORLD
    assert "data-world-organization-task-pager" in WORLD_TASKS
    assert ".world-task-pager {" in WORLD_CSS


def test_narrowing_the_world_board_returns_to_the_first_page():
    # A search, a filter, or a re-sort changes which tasks are visible, so the
    # board must not keep showing page 9 of a set that is now one page long.
    assert WORLD_TASKS.count("workPage = 0;") >= 5
    # And a page that no longer exists clamps down instead of painting empty.
    assert "Math.min(Math.max(0, workPage), taskPageCount(visible) - 1)" in WORLD_TASKS


def test_qt_table_paints_one_page_and_searches_the_whole_catalog():
    assert "constexpr int kOrganizationTaskPageSize = 100;" in QT_TASKS
    assert "void TasksPage::renderOrganizationTaskRows(" in QT_TASKS
    # The row's Qt::UserRole is the task's absolute index, not its row number,
    # so selecting a row on page 3 still resolves to the right task.
    assert "item->setData(Qt::UserRole, index);" in QT_TASKS
    # The search filters every task, then the page slices the matches.
    assert "matches.append(index);" in QT_TASKS
    assert "m_organizationTaskShownRows" in QT_TASKS
    assert "void OrganizationTaskStore::requestCatalog(" in QT_TASK_STORE
    assert 'QStringLiteral("/api/tasks?offset=%1")' in QT_TASK_STORE
    assert 'payload.value(QStringLiteral("hasMore")).toBool()' in QT_TASK_STORE
    # The old hide-rows-in-place search (which could only ever hide rows the
    # table already held) is gone.
    assert "setRowHidden(" not in QT_TASKS


def test_qt_table_loads_more_rows_when_scrolled_to_the_bottom():
    assert "constexpr int kOrganizationTaskLoadAheadRows = 10;" in QT_TASKS
    assert "void TasksPage::appendOrganizationTaskRows(int count)" in QT_TASKS
    assert "&QScrollBar::valueChanged" in QT_TASKS
    assert "appendOrganizationTaskRows(kOrganizationTaskPageSize);" in QT_TASKS
    assert "scroll for more" in QT_TASKS
    assert "m_organizationTaskPrevPageButton" not in QT_TASKS
    assert "m_organizationTaskNextPageButton" not in QT_TASKS
    render = QT_TASKS[
        QT_TASKS.index("void TasksPage::renderOrganizationTaskRows("):
    ]
    render = render[:render.index("\n}\n")]
    assert "verticalScrollBar()->setValue(scrollValue);" in render
    assert "selectedMatch / kOrganizationTaskPageSize + 1" in render


def test_qt_task_details_render_and_expand_image_attachments():
    assert "taskImageAttachments(" in QT_TASK_BRIDGE
    for marker in (
        "requestOrganizationTaskAttachment(",
        "forkmesh-task-image:",
        "QScrollArea",
        "taskDetailHtml(task, m_organizationTaskImageData",
    ):
        assert marker in QT_TASKS
    assert "kOrgTaskAttachmentProof" in QT_TASK_STORE


def test_dashboard_serves_a_tasks_page_wired_into_the_shell():
    assert "tasks" not in dashboard_shell.PAGES
    meta = next(row for row in build_app_plugins._web_entries()
                if row["item"].package_id == "tasks")
    assert meta["route"] == "/dashboard/tasks"
    assert meta["asset"] == "dashboard-plugins/tasks/default/index.html"
    assert 'data-view="tasks"' in TASKS_VIEW
    assert 'data-nav="tasks"' in build_app_plugins._navigation([meta])
    assert 'href="/dashboard/tasks"' in build_app_plugins._navigation([meta])


def test_dashboard_tasks_page_reads_the_private_catalog_and_pages_it():
    js = assembled_dashboard_js()
    assert "const TASKS_PAGE_SIZE = 100;" in js
    assert '"tasks": initTasksPage,' in js
    assert "async function fetchOrganizationTaskCatalog()" in js
    assert 'fetchJson(`/api/tasks${separator}`, { fresh: true })' in js
    assert "page?.hasMore !== true" in js
    # Paging is over the filtered set, and the page buttons are real controls.
    assert "Math.ceil(matches.length / TASKS_PAGE_SIZE)" in js
    for marker in ("data-tasks-list", "data-tasks-pages", "data-tasks-prev",
                   "data-tasks-next", "data-tasks-search", "data-tasks-filter"):
        assert marker in TASKS_VIEW, marker
        assert marker in js, marker


def test_every_surface_reports_the_same_open_and_closed_counts():
    # adhoc #56: the desktop said "84 open" while the web only ever said how
    # many rows the current page held, which read as two different catalogs.
    # All three surfaces now state open, closed, and the page they are on, and
    # they count "done" the same way (the relay forces status='done' whenever
    # completed_at > 0, so a status test alone is enough on the World board).
    js = assembled_dashboard_js()
    assert "function taskStateCounts()" in js
    assert 'task.completedAt > 0 || task.status === "done"' in js
    assert "${formatCount(counts.open)} open · ${formatCount(counts.closed)} closed" in js
    assert "· page ${state.tasksView.page} of ${pages}" in js
    assert "Tasks · ${openCount} open · ${tasks.length - openCount} closed${narrowed}" in WORLD_TASKS
    assert '"%1 open \\xC2\\xB7 %2 closed%3"' in QT_TASKS


def test_qt_task_list_is_a_headerless_icon_strip():
    # No table header, no grid, no banding: one column, and a delegate that
    # paints status / department / repository / assignee / QA as glyphs on the
    # left with the title on the right (adhoc #56).
    assert "class OrganizationTaskRowDelegate : public QStyledItemDelegate" in QT_TASKS
    assert "m_organizationTasksTable = new QTableWidget(0, 1);" in QT_TASKS
    assert "m_organizationTasksTable->horizontalHeader()->hide();" in QT_TASKS
    assert "m_organizationTasksTable->setShowGrid(false);" in QT_TASKS
    assert "m_organizationTasksTable->setAlternatingRowColors(false);" in QT_TASKS
    assert "m_organizationTasksTable->setFrameShape(QFrame::NoFrame);" in QT_TASKS
    for role in (
        "kTaskStatusRole",
        "kTaskDepartmentRole",
        "kTaskRepositoryRole",
        "kTaskAssigneeKindRole",
        "kTaskQaRole",
    ):
        assert "item->setData(%s," % role in QT_TASKS, role
    assert "kTaskCreatedAtRole," in QT_TASKS
    # Creation age is visible without opening the task, while the light hover
    # card spells each icon strip field out with its own symbol.
    assert "taskCreatedAgo" in QT_TASKS
    assert 'QStringLiteral(" ago")' in QT_TASKS
    assert "background-color:#f6f8fa" in QT_TASKS
    for label in ("Status", "Priority", "Department", "Repository", "Assignee", "QA"):
        assert 'QStringLiteral("%s")' % label in QT_TASKS
    # Priority and assignee lost their in-row editors with the columns; the
    # edit dialog is reachable from the row itself instead.
    assert "&QTableWidget::itemDoubleClicked" in QT_TASKS
    theme = (QT_SRC / "Theme.h").read_text(encoding="utf-8")
    assert theme.count("#organizationTasksTable {") == 2  # dark + light


def test_qt_task_detail_can_add_the_task_to_the_prompt_box():
    # The task detail's own "add to prompt" affordance: one click hands the
    # selected task's title, id, repository and free-text blocks to the footer
    # prompt box instead of making the operator retype them.
    assert 'QStringLiteral("Add to prompt"), QStringLiteral("plus"));' in QT_TASKS
    assert "&TasksPage::addOrganizationTaskToPrompt" in QT_TASKS
    body = QT_TASKS[QT_TASKS.index("void TasksPage::addOrganizationTaskToPrompt("):]
    body = body[:body.index("\nvoid TasksPage::")]
    assert 'QStringLiteral("prompt.append")' in body
    command = QT_TASK_BRIDGE[QT_TASK_BRIDGE.index('command == QLatin1String("prompt.append")'):]
    assert "appendTextToActivePrompt(text);" in command[:400]
    # The prompt text itself is shared with the one-click launch below, so the
    # hand-edited prompt and the one an agent is handed cannot drift apart.
    assert "organizationTaskPromptText(task)" in body
    prompt_text = QT_TASKS[QT_TASKS.index("QString organizationTaskPromptText("):]
    prompt_text = prompt_text[:prompt_text.index("\n}\n")]
    assert '"[task:%1] %2"' in prompt_text
    assert '"Repository: %1"' in prompt_text
    assert '"How to test: %1"' in prompt_text
    # Copying text out of the board mutates nothing, so it needs a selection
    # only — not the manage right the writes require.
    assert "m_organizationTaskPromptButton->setEnabled(selected);" in QT_TASKS


def test_qt_task_detail_starts_an_agent_with_the_prompt_box_settings():
    # One click from a task to a running agent: the task is attached to the
    # prompt box, the run takes that box's live settings, and the session is
    # bound back to the task so status and completion report against it.
    assert "Start agent with prompt settings" in QT_TASKS
    assert (
        'addAction(&m_organizationTaskStartAgentButton,\n'
        '              QStringLiteral("Start agent"), QStringLiteral("rocket"));'
        in QT_TASKS
    )
    assert "&TasksPage::startOrganizationTaskAgentFromPrompt" in QT_TASKS
    page = QT_TASKS[
        QT_TASKS.index("void TasksPage::startOrganizationTaskAgentFromPrompt("):
    ]
    page = page[:page.index("\nvoid TasksPage::")]
    assert 'QStringLiteral("task.startAgent")' in page
    assert "organizationTaskPromptText(task)" in page
    body = QT_TASK_BRIDGE[
        QT_TASK_BRIDGE.index("void MainWindow::startOrganizationTaskAgent("):
    ]
    body = body[:body.index("\nvoid MainWindow::")]
    # The task goes into the prompt box first, and the box is what gets sent:
    # anything already typed there rides along and the operator is left looking
    # at exactly what the agent was handed.
    assert "addOrganizationTaskToPrompt();" in body
    assert "m_issueQuickAdd->toPlainText().trimmed()" in body
    # Every setting comes from the prompt box rather than a fixed default.
    assert "m_quickAddAgentProvider" in body
    assert "selectedModelComboValue(m_quickAddClaudeModel)" in body
    assert "m_quickAddCreatePr->isChecked()" in body
    assert "m_quickAddImages" in body
    # Neither of the two non-agent prompt modes can work a task.
    assert "quickAddProviderIsPromptAction(provider)" in body
    assert "agentIsCloudflareAiProvider(provider)" in body
    # And the run is bound to this task instead of opening a second one.
    assert "/*orgTaskId=*/id" in body
    agents = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_AGENT_SOURCES
    )
    launch = agents[agents.index("int MainWindow::startAdHocAgentForRepo("):]
    launch = launch[:launch.index("\n}\n")]
    assert "session.orgTaskId = orgTaskId.trimmed();" in launch
    # The reverse link: the detail names the desktop run carrying this task.
    assert "localAgentRunLabelForTask(" in QT_TASKS
    assert "session.orgTaskId != taskId" in QT_TASKS
    assert 'command == QLatin1String("task.agentSessions")' in QT_TASK_BRIDGE


def test_qt_created_tasks_appear_immediately_and_open():
    # POST /api/tasks returns the canonical new task, so Qt should render that
    # response directly instead of waiting for a second network request. Any
    # search/page state that could hide it is reset, and selecting by id opens
    # the existing detail pane before navigation reaches Tasks.
    body = QT_TASKS[
        QT_TASKS.index("void TasksPage::openCreatedOrganizationTask("):
    ]
    body = body[:body.index("\n}\n")]
    for marker in (
        "m_organizationTasks.insert(0, task);",
        "m_organizationTasksSearchMirror.clear();",
        "m_organizationTaskShownRows = 0;",
        "renderOrganizationTaskRows(taskId);",
        "showPage();",
        "m_organizationTasksTable->scrollToItem(item);",
    ):
        assert marker in body, marker

    # Every Qt create affordance uses the same immediate-open path: the task
    # dialog (also used by note conversion), footer quick-add, and follow-ups.
    assert QT_TASKS.count("openCreatedOrganizationTask(") == 4
    assert "openCreatedOrganizationTask(" in QT_TASK_BRIDGE
    composer = QT_TASKS[QT_TASKS.index("void TasksPage::composeOrganizationTask("):]
    composer = composer[:composer.index("\nvoid TasksPage::")]
    assert 'payload.value(QStringLiteral("task")).toObject()' in composer
    assert "openCreatedOrganizationTask(task);" in composer
    assert "refreshOrganizationTasks();" not in composer
def test_qt_teammate_runs_stay_in_the_agent_session_not_the_task_board():
    teammates = (QT_SRC / "MainWindowTeammates.cpp").read_text(encoding="utf-8")
    launch = teammates[teammates.index("const int sessionId = startAdHocAgentForRepo("):]
    launch = launch[:launch.index(");", launch.index("const int sessionId"))]
    # The current launcher creates an organization-task link only from a
    # non-empty orgTaskId; teammates pass the empty QString in that slot.
    assert "false, switchToTab, QString()," in launch
    assert "sendPromptToAgentSession(sessionId, prompt);" in teammates
    assert 'QStringLiteral("SESSION FEEDBACK")' in teammates
    assert 'QStringLiteral("Send feedback")' in teammates

    agents = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_AGENT_SOURCES
    )
    launcher = agents[agents.index("int MainWindow::startAdHocAgentForRepo("):]
    launcher = launcher[:launcher.index("\n}\n")]
    assert "const QString &orgTaskId" in launcher
    assert "session.orgTask = !orgTaskId.trimmed().isEmpty();" in launcher


def test_dashboard_tasks_page_never_renders_private_task_text_unescaped():
    js = assembled_dashboard_js()
    body = js[js.index("function taskRowHtml("):js.index("function renderTasksPage(")]
    for field in ("task.title", "taskAssigneeLabel(task)"):
        assert "escapeHtml(%s)" % field in body, field


def test_dashboard_tasks_resolve_the_assignee_avatar_from_the_canonical_profile():
    js = assembled_dashboard_js()
    body = js[js.index("function taskAvatarHtml("):js.index("function taskMatchesFilter(")]
    for marker in (
        "data-task-assignee-avatar",
        "function taskAvatarForName(name)",
        'fetchJson(`/api/accounts/${encodeURIComponent(account)}`)',
        "profile?.exists !== true",
        "profile?.profilePrivate === true",
        "state.session?.avatarPng",
        "function hydrateTaskAvatars()",
    ):
        assert marker in body, marker
    row = js[js.index("function taskRowHtml("):js.index("function renderTasksPage(")]
    assert "${taskAvatarHtml(task)}" in row
    assert "hydrateTaskAvatars();" in js[js.index("function renderTasksPage("):js.index("async function loadOrganizationTasks(")]


def test_the_served_package_view_carries_every_tasks_partial_style():
    partial = (PUBLIC / "dashboard" / "partials" / "views" / "tasks.html"
               ).read_text(encoding="utf-8")

    def rules(text):
        return {line.strip() for line in text.splitlines()
                if line.strip().startswith(".task-")}

    assert rules(partial)
    assert rules(partial) <= rules(TASKS_VIEW)
    assert ".task-progress-track {" in TASKS_VIEW


def test_qt_task_rows_paint_attached_image_thumbnails():
    assert "constexpr int kTaskThumbnailsRole = Qt::UserRole + 16;" in QT_TASKS
    assert "constexpr int kTaskAttachmentCountRole = Qt::UserRole + 17;" in QT_TASKS
    assert "item->setData(kTaskThumbnailsRole,\n                      taskRowThumbnails(task, m_organizationTaskImageData));" in QT_TASKS
    assert "item->setData(\n            kTaskAttachmentCountRole," in QT_TASKS
    helper = QT_TASKS[QT_TASKS.index("QVariantList taskRowThumbnails("):]
    helper = helper[:helper.index("\n}\n")]
    assert "thumbnails.size() >= kTaskRowThumbMax" in helper
    assert "taskImageDataUriBytes(" in helper
    assert "images.contains(cacheKey)" in helper
    delegate = QT_TASKS[QT_TASKS.index("class OrganizationTaskRowDelegate"):]
    delegate = delegate[:delegate.index("\n};\n")]
    assert "index.data(kTaskThumbnailsRole).toList()" in delegate
    assert "right = tileX - 10;" in delegate
    assert 'QStringLiteral("+%1").arg(hidden)' in delegate
    loader = QT_TASKS[QT_TASKS.index("void TasksPage::renderOrganizationTaskDetail("):]
    assert "refreshOrganizationTaskRowThumbnails(taskId);" in loader[:loader.index("\n}\n")]
