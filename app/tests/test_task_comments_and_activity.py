#!/usr/bin/env python3
"""Comments, @mention pings, the check-in button, and one task's own history.

The task catalog could already be read on three surfaces, but the only thing a
member could do to a task from the web was look at it: no comment box, no way
to ping the person who needed to see it, no check-in control, and nowhere that
answered "what has happened to this task?". These tests pin the contract that
the relay records every task event, pings on comments and check-ins, and that
the dashboard drawer and the World board both render the resulting timeline.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
WORLD_PUBLIC = ROOT.parent / "world" / "public" / "world"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import dashboard_shell  # noqa: E402

from _dashboard_bundle import assembled_dashboard_js  # noqa: E402

TASKS_API = (
    ROOT.parent / "world" / "src" / "world_office_tasks.py"
).read_text(encoding="utf-8")
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
SCHEMA = (SRC / "schema.py").read_text(encoding="utf-8")
WORLD_TASKS = (WORLD_PUBLIC / "world-office-tasks.js").read_text(
    encoding="utf-8")
WORLD = (WORLD_PUBLIC / "world.js").read_text(encoding="utf-8")
WORLD_CSS = (WORLD_PUBLIC / "world.css").read_text(encoding="utf-8")
TASKS_VIEW = (PUBLIC / dashboard_shell.view_path("tasks")).read_text(
    encoding="utf-8")
QT_TASKS = (ROOT.parent / "desktop" / "src" / "MainWindowTasks.cpp").read_text(
    encoding="utf-8")
QT_HEADER = (ROOT.parent / "desktop" / "src" / "MainWindow.h").read_text(
    encoding="utf-8")
QT_ACTIONS = (
    ROOT.parent / "desktop" / "src" / "MainWindowActions.cpp"
).read_text(encoding="utf-8")
OFFICE_ROUTES = (
    ROOT.parent / "world" / "src" / "world_office_routes.py"
).read_text(encoding="utf-8")


def test_every_task_mutation_records_one_timeline_event():
    for kind in (
        "created", "updated", "started", "stopped", "checkin", "replied",
        "completed", "reopened", "returned", "qa_requested", "deleted",
    ):
        assert '"%s"' % kind in TASKS_API, kind
    # The QA verdict is written by the QA desk, not the task service, so its
    # timeline row is recorded there.
    qa = (ROOT.parent / "world" / "src" / "world_qa.py").read_text(
        encoding="utf-8")
    assert "INSERT INTO organization_task_events " in qa
    assert "'qa_reviewed'" in qa
    # Recording a timeline row never fails the mutation it describes.
    record = TASKS_API.split("async def _record_event", 1)[1].split(
        "async def _task_history", 1)[0]
    assert "except Exception:" in record
    assert "return \"\"" in record
    purge = TASKS_API.split("async def _purge_task_rows(", 1)[1].split(
        "async def purge_trash", 1)[0]
    assert '"organization_task_events"' in purge
    assert '"DELETE FROM " + table' in purge


def test_the_timeline_is_sealed_bounded_and_reversible():
    assert "MAX_TASK_EVENTS = 250" in TASKS_API
    # Only the bounded kind, the actor blind index and the timestamp are in
    # the clear; the name, the summary and the mentions are sealed.
    insert = TASKS_API.split(
        "INSERT INTO organization_task_events ", 1)[1][:400]
    assert "(event_id,task_id,org_bi,actor_bi,kind,data,created_at)" in insert
    assert "await runtime.seal({" in TASKS_API
    assert "CREATE TABLE IF NOT EXISTS organization_task_events" in SCHEMA
    assert "trg_organization_task_event_limit" in SCHEMA
    up = ROOT / "migrations" / "0180_organization_task_events.sql"
    down = ROOT / "migrations" / "down" / "0180_organization_task_events.sql"
    assert up.is_file() and down.is_file()
    assert "DROP TABLE IF EXISTS organization_task_events" in down.read_text(
        encoding="utf-8")


def test_comments_and_checkins_ping_the_task_people_and_the_names_they_use():
    reply = TASKS_API.split("async def _reply(", 1)[1].split(
        "async def _return_to_list", 1)[0]
    assert 'now, "commented",' in reply
    assert '"field": "comment", "excerpt": body' in reply
    assert "skip=mentioned" in reply
    assert "_notify_mentions(" in reply
    checkin = TASKS_API.split("async def _checkin(", 1)[1].split(
        "async def handle(", 1)[0]
    assert '"checked in " + CHECKIN_STATE_COPY.get(state, state)' in checkin
    assert '"field": "checkin"' in checkin
    assert "_notify_mentions(" in checkin
    update = TASKS_API.split("async def _update(", 1)[1].split(
        "def _edit_changes(", 1)[0]
    assert '"updated", now' in update
    assert '"field": "progress", "from": previous, "to": progress' in update
    assert "changes=edit_changes" in update
    for verb in ("commented", "checked in going well", "checked in blocked",
                 "checked in needs help"):
        assert '"%s":' % verb in ENTRY, verb
    # The notifier lives with the rest of the office task runtime, and every
    # ping it files goes through enqueue_notification, which wakes the
    # account's open web and desktop clients and its APNs devices.
    assert "async def notify_organization_task_mentions(" in OFFICE_ROUTES
    mentions = OFFICE_ROUTES.split(
        "async def notify_organization_task_mentions(", 1)[1].split(
        "async def marketing_members(", 1)[0]
    assert "await enqueue_notification(" in mentions
    enqueue = ENTRY.split("async def enqueue_notification(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert 'await notify_account_event(env, recipient, "pings", push_record)' in enqueue
    notify = ENTRY.split("async def notify_account_event(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert "await push_account_apns(env, owner, topic, push_alert)" in notify


def test_task_history_is_readable_on_its_own_route():
    assert '("history", "activity", "events")' in TASKS_API
    assert 'return ("history", task_id, "history")' in TASKS_API
    handle = TASKS_API.split("async def handle(", 1)[1]
    assert 'if kind == "history":' in handle
    assert '"history": await _task_history(runtime, org_bi, task_id),' in handle
    # And it rides along on every single-task read, so opening one task is one
    # request rather than two.
    assert 'task["history"] = await _task_history(' in TASKS_API


def test_dashboard_detail_pane_comments_mentions_and_checks_in():
    js = assembled_dashboard_js()
    for marker in (
        "function taskTimelineEntries(task)",
        "function taskMentionHtml(value)",
        "function taskActivityHtml(task)",
        "function taskCanCheckIn(task)",
        "function taskCheckinHtml(task)",
        "async function submitTaskCheckin(task, checkinState)",
        "function insertTaskMention(input, name)",
        "function taskMentionQueryAt(input)",
        "${taskActivityHtml(task)}",
        "${taskCheckinHtml(task)}",
        'taskActionHtml("checkin", "radio", "Check in", taskCanCheckIn(task)),',
        "`/api/tasks/${taskId}/replies`",
        "`/api/tasks/${task.id}/checkin`",
    ):
        assert marker in js, marker
    # A reply event and its reply row share a timestamp, so the row wins; an
    # event whose reply scrolled out of the window is still drawn as a comment.
    assert 'entry.kind === "replied" && replyTimes.has(' in js
    for kind in (
        "created", "updated", "started", "stopped", "checkin", "completed",
        "reopened", "returned", "qa_requested", "qa_reviewed", "deleted",
    ):
        assert "    %s: { icon:" % kind in js, kind
    # Check-in notes are bounded exactly as the relay bounds them.
    assert "const TASK_CHECKIN_NOTE_MAX = 500;" in js
    assert "MAX_CHECKIN_NOTE = 500" in TASKS_API
    for rule in (".task-timeline {", ".task-mention {", ".task-mention-list {",
                 '.task-checkin-chip[data-state="blocked"]'):
        assert rule in TASKS_VIEW, rule


def test_dashboard_mention_suggestions_open_only_inside_an_at_token():
    js = assembled_dashboard_js()
    matches = js[js.index("function renderTaskMentionList(input)"):js.index(
        "function rememberTaskDraft(input)")]
    assert "query === null ? [] :" in matches
    assert "const match = /(^|[^A-Za-z0-9._%+-])@([a-z0-9-]*)$/i.exec(" in js
    # Drafts live outside the DOM so a repaint cannot eat an unposted comment.
    assert '${escapeHtml(taskCommentDrafts.get(task.id) || "")}</textarea>' in js
    # Candidates come from what the viewer can already read: no roster call.
    candidates = js[js.index("function taskMentionCandidates()"):js.index(
        "function taskMentionQueryAt(input)")]
    assert "fetch(" not in candidates and "taskRequest(" not in candidates


def test_world_board_shows_the_timeline_and_an_explicit_check_in_button():
    for marker in (
        "function taskHistoryHTML(task)",
        "const historyWindows = new Map();",
        'data-world-office-task-history="${task.id}"',
        "/history`",
        'data-world-office-task-checkin-open="${task.id}"',
        "function mentionHTML(",
        "const TASK_EVENT_LABELS = {",
    ):
        assert marker in WORLD_TASKS, marker
    # The check-in button reuses the prompt that already knows how to post one.
    assert "if (id && !showCheckin(id))" in WORLD_TASKS
    # Check-in notes carry mentions, so the prompt has somewhere to type one.
    assert "data-world-office-task-checkin-note" in WORLD
    assert "note ? { state, note } : { state }" in WORLD_TASKS
    # Comments and task descriptions render their mentions rather than escaping
    # them into plain text.
    assert "mentionHTML(reply.body, 2000, actor)" in WORLD_TASKS
    assert "mentionHTML(task.details, 4000, actor)" in WORLD_TASKS
    for rule in (".world-task-timeline {", ".world-task-mention {",
                 ".world-office-task-history {"):
        assert rule in WORLD_CSS, rule


def test_desktop_can_comment_check_in_and_read_the_timeline():
    for marker in (
        "void MainWindow::commentOnOrganizationTask()",
        "void MainWindow::checkInOnOrganizationTask()",
        "void MainWindow::loadOrganizationTaskHistory(",
        "QStringList taskMentionedNames(",
        "QString taskHistoryHtml(",
        "QString taskEventLabel(",
        'QStringLiteral("/api/tasks/%1/replies").arg(id)',
        'QStringLiteral("/api/tasks/%1/checkin").arg(id)',
        'QStringLiteral("/api/tasks/%1/history").arg(taskId)',
    ):
        assert marker in QT_TASKS, marker
    assert "m_organizationTaskCommentButton" in QT_HEADER
    assert "m_organizationTaskCheckinButton" in QT_HEADER
    assert "QHash<QString, QJsonArray> m_organizationTaskHistory;" in QT_HEADER
    # The check-in states are exactly the ones the relay accepts, in the order
    # the labels are shown, so the chosen index maps onto the right value.
    for state in ("going_well", "blocked", "needs_help"):
        assert 'QStringLiteral("%s")' % state in QT_TASKS, state
    # Checking in is offered when the relay would accept one; commenting is
    # offered on any visible task, including a finished one.
    actions = QT_TASKS.split("void MainWindow::updateOrganizationTaskActions",
                             1)[1].split("void MainWindow::", 1)[0]
    assert "m_organizationTaskCommentButton->setEnabled(selected);" in actions
    assert "selected && mine && active && !done" in actions
    # A comment by somebody else never moves the row's updatedAt, so a refresh
    # is what drops the cached timelines rather than a field comparison.
    assert "m_organizationTaskHistory.clear();" in QT_TASKS
    assert "m_organizationTaskHistory.remove(id);" in QT_TASKS


def test_desktop_raises_task_pings_from_the_worker_inbox():
    alerts = QT_ACTIONS.split("void MainWindow::refreshWebAlerts(", 1)[1].split(
        "void MainWindow::clearWebAlerts()", 1)[0]
    assert 'kind.startsWith(QLatin1String("organization_task_"))' in alerts
    assert '.value(QStringLiteral("taskId"))' in alerts
    assert "showPingTrayMessage(taskPing.title, taskPing.body);" in alerts
    # Startup never replays old pings: the baseline read only records ids.
    assert "if (loadingStartupBaseline)" in alerts


def test_any_member_can_remind_the_assignee_from_the_dashboard_and_desktop():
    assert '"start", "stop", "checkin", "remind", "complete", "qa",' in TASKS_API
    js = assembled_dashboard_js()
    for marker in (
        "function taskCanRemind(task)",
        "function taskRemindHtml(task)",
        "async function submitTaskReminder(task)",
        'taskActionHtml("remind", "bell-ring", "Remind", taskCanRemind(task)),',
        "${taskRemindHtml(task)}",
        "`/api/tasks/${task.id}/remind`",
        "const TASK_REMINDER_NOTE_MAX = 500;",
        "const TASK_REMINDER_COOLDOWN_MS = 10 * 60 * 1000;",
    ):
        assert marker in js, marker
    assert "MAX_REMINDER_NOTE = 500" in TASKS_API
    assert "REMINDER_COOLDOWN_MS = 10 * 60 * 1000" in TASKS_API
    can_remind = js[js.index("function taskCanRemind(task)"):js.index(
        "function taskReminderWaitMs(task)")]
    assert "task.assignee !== state.tasksView.actor" in can_remind
    assert "!taskIsDone(task)" in can_remind
    assert "void MainWindow::remindOrganizationTaskAssignee()" in QT_TASKS
    assert 'QStringLiteral("/api/tasks/%1/remind").arg(id)' in QT_TASKS
    assert 'QStringLiteral("Last reminder")' in QT_TASKS
    assert "m_organizationTaskRemindButton" in QT_HEADER
    actions = QT_TASKS.split("void MainWindow::updateOrganizationTaskActions",
                             1)[1].split("void MainWindow::", 1)[0]
    assert "selected && !done && !mine &&" in actions
