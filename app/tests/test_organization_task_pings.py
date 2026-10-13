#!/usr/bin/env python3
"""Organization task pings name who acted, on what, and in which org."""
import ast
import importlib.util
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8") + WORLD_ROUTE_SOURCE

_catalog_spec = importlib.util.spec_from_file_location(
    "forkmesh_catalog_for_pings", ROOT / "src" / "catalog.py")
catalog = importlib.util.module_from_spec(_catalog_spec)
_catalog_spec.loader.exec_module(catalog)


def _load_ping_copy(name="organization_task_ping_copy"):
    want_funcs = {
        "organization_task_ping_copy",
        "organization_task_mention_copy",
        "organization_task_reminder_copy",
        "organization_task_persistent_reminder_copy",
    }
    want_assigns = {
        "MAX_NODE_NAME",
        "ORGANIZATION_TASK_ACTION_COPY",
        "ORGANIZATION_TASK_CHECKIN_COPY",
        "ORGANIZATION_TASK_MENTION_CONTEXT_COPY",
        "ORGANIZATION_TASK_STATUS_COPY",
    }
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if targets & want_assigns:
                body.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in want_funcs:
            body.append(node)
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)
    ns = {"clean_string": catalog.clean_string}
    exec(compile(mod, str(ENTRY), "exec"), ns)
    return ns[name]


TASK = {
    "id": "task-1",
    "title": "Fix the login bug",
    "department": "engineering",
    "team": "frontend",
    "priority": 20,
    "status": "active",
    "assignee": "dana",
    "assigneeKind": "user",
    "repository": "forkmesh/site",
}


def test_ping_copy_names_actor_action_task_and_organization():
    copy = _load_ping_copy()("forkmesh", "jett", "started", TASK)
    assert copy["title"] == '@jett started "Fix the login bug" in forkmesh'
    assert copy["body"].startswith(
        '@jett started "Fix the login bug" in the forkmesh organization')
    for fragment in (
            "engineering / frontend",
            "priority 20",
            "assigned to @dana",
            "status in progress",
            "repo forkmesh/site",
    ):
        assert fragment in copy["body"]
    assert copy["meta"]["organization"] == "forkmesh"
    assert copy["meta"]["actor"] == "jett"
    assert copy["meta"]["action"] == "started"
    assert copy["meta"]["taskId"] == "task-1"
    assert copy["meta"]["taskTitle"] == "Fix the login bug"
    assert copy["meta"]["department"] == "engineering"
    assert copy["meta"]["team"] == "frontend"
    assert copy["meta"]["priority"] == 20
    assert copy["meta"]["status"] == "active"
    assert copy["meta"]["assignee"] == "dana"
    assert copy["meta"]["repository"] == "forkmesh/site"


def test_ping_copy_survives_missing_fields_and_stays_inside_caps():
    copy = _load_ping_copy()("", "", "qa_requested", None)
    assert copy["title"] == 'a team member sent to QA "Organization task"'
    assert "in your organization" in copy["body"]
    assert copy["meta"]["action"] == "qa requested"
    assert copy["meta"]["actor"] == ""
    long_title = "x" * 400
    long = _load_ping_copy()(
        "forkmesh", "j" * 80, "created", {"title": long_title})
    assert len(long["title"]) <= 160
    assert len(long["body"]) <= 500
    assert long["title"].endswith("in forkmesh")


def test_agent_assignment_and_unassigned_tasks_read_naturally():
    agent = _load_ping_copy()("forkmesh", "jett", "created", {
        "title": "Ship the digest", "assigneeKind": "agent",
        "assignee": "agent",
    })
    assert "assigned to an agent" in agent["body"]
    assert agent["meta"]["assignee"] == "agent"
    idle = _load_ping_copy()("forkmesh", "jett", "created", {
        "title": "Ship the digest", "assigneeKind": "unassigned",
        "status": "idle",
    })
    assert "unassigned" in idle["body"]
    assert "status not started" in idle["body"]


def test_worker_notifier_resolves_the_org_name_and_the_task_people():
    runtime_method = ENTRY_TEXT.split(
        "async def notify_organization_task_activity", 1)[1].split(
            "async def notify_engineering_task_started", 1)[0]
    assert "return await notify_organization_task_update(" in runtime_method
    notifier = ENTRY_TEXT.split(
        "async def notify_organization_task_update(", 1)[1].split(
            "class _OfficeMarketingTasksRuntime", 1)[0]
    assert 'SELECT name FROM orgs WHERE org_bi=?' in notifier
    assert "SELECT name,role FROM org_members WHERE org_bi=?" in notifier
    assert "copy = organization_task_ping_copy(" in notifier
    assert "world_task_pings.deliver(" in notifier
    assert "_organization_task_participants(" in notifier
    recipients = ENTRY_TEXT.split(
        "def _task_ping_recipients(", 1)[1].split(
            "async def notify_organization_task_update(", 1)[0]
    assert 'candidates = [task.get("createdBy")]' in recipients
    assert 'role == "owner"' in recipients
    assert "name == actor" in recipients
    assert "name not in roster" in recipients


def test_dashboard_ping_modal_lists_the_organization_task_fields():
    dashboard = assembled_dashboard_js()
    assert "function organizationTaskDetailsHtml(item)" in dashboard
    assert "${organizationTaskDetailsHtml(item)}" in dashboard
    assert 'String(item.kind || "").startsWith("organization_task")' in dashboard
    for label in ("Who", "What", "Task", "Organization", "Department",
                  "Team", "Assigned to", "Priority", "Status", "Repository"):
        assert '["%s",' % label in dashboard
    assert "organization_task_activity: \"clipboard-list\"" in dashboard


def test_dashboard_preview_opens_the_selected_ping_detail():
    dashboard = assembled_dashboard_js()
    assert "setNotificationDropdownOpen(false);" in dashboard
    assert "!event.target.closest(\"#notificationModal\")" in dashboard


def test_comment_and_checkin_pings_read_as_sentences():
    commented = _load_ping_copy()("forkmesh", "jett", "commented", TASK)
    assert commented["title"] == (
        '@jett commented on "Fix the login bug" in forkmesh')

    checkin = _load_ping_copy()(
        "forkmesh", "dana", "checked in blocked", TASK)
    # "checked in blocked on <task>" would read as "blocked on <task>", so the
    # state is carried as its own detail instead of folded into the verb.
    assert checkin["title"] == (
        '@dana checked in on "Fix the login bug" in forkmesh')
    assert "check-in: blocked" in checkin["body"]
    for state, label in (
            ("going_well", "going well"),
            ("needs_help", "needs help"),
    ):
        copy = _load_ping_copy()(
            "forkmesh", "dana", "checked in " + label, TASK)
        assert "check-in: %s" % label in copy["body"], state


def test_mention_ping_names_the_place_and_quotes_the_copy():
    copy = _load_ping_copy("organization_task_mention_copy")(
        "forkmesh", "jett", "comment", TASK,
        "can you take a look at the redirect?",
    )
    assert copy["title"] == (
        '@jett mentioned you in a comment on "Fix the login bug" in forkmesh')
    assert "can you take a look at the redirect?" in copy["body"]
    assert copy["meta"]["action"] == "mentioned"
    assert copy["meta"]["context"] == "comment"
    assert copy["meta"]["taskId"] == "task-1"

    checkin = _load_ping_copy("organization_task_mention_copy")(
        "forkmesh", "jett", "check-in", TASK, "")
    assert "a check-in on" in checkin["title"]

    long_title = _load_ping_copy("organization_task_mention_copy")(
        "forkmesh", "j" * 80, "comment", {"title": "x" * 400}, "y" * 900)
    assert len(long_title["title"]) <= 160
    assert len(long_title["body"]) <= 500


def test_worker_mention_notifier_pings_only_current_org_members():
    notifier = ENTRY_TEXT.split(
        "async def notify_organization_task_mentions", 1)[1].split(
            "    async def marketing_members", 1)[0]
    assert "copy = organization_task_mention_copy(" in notifier
    # Named-but-not-a-member never receives an organization-private ping.
    assert "if not await self.organization_member(org_bi, recipient):" in notifier
    assert "continue" in notifier
    assert 'recipient == safe_actor' in notifier
    assert '"mention",' in notifier
    # A comment leaves the task row untouched, so the ping cannot dedupe on it.
    assert 'str(int(ts or 0))' in notifier
    assert '(task or {}).get("updatedAt")' not in notifier


def test_reminder_ping_asks_the_assignee_to_check_in():
    copy = _load_ping_copy("organization_task_reminder_copy")(
        "forkmesh", "jett", TASK, "any news before standup?")
    assert copy["title"] == (
        '@jett asked you for a check-in on "Fix the login bug" in forkmesh')
    assert "any news before standup?" in copy["body"]
    assert "Open the task and check in" in copy["body"]
    assert copy["meta"]["action"] == "reminded"
    assert copy["meta"]["context"] == "reminder"
    assert copy["meta"]["taskId"] == "task-1"

    long_title = _load_ping_copy("organization_task_reminder_copy")(
        "forkmesh", "j" * 80, {"title": "x" * 400}, "y" * 900)
    assert len(long_title["title"]) <= 160
    assert len(long_title["body"]) <= 500


def test_worker_reminder_notifier_pings_only_the_member_assignee():
    notifier = ENTRY_TEXT.split(
        "async def notify_organization_task_reminder", 1)[1].split(
            "    async def notify_organization_task_mentions", 1)[0]
    assert "copy = organization_task_reminder_copy(" in notifier
    assert "not await self.organization_member(org_bi, recipient)" in notifier
    assert "recipient == safe_actor" in notifier
    assert '"mention",' in notifier
    assert 'TASKS_HREF + "?task="' in notifier
    assert "str(int(ts or 0))" in notifier


def test_persistent_reminder_ping_names_the_task_and_cadence():
    copy = _load_ping_copy("organization_task_persistent_reminder_copy")(
        "forkmesh", TASK, 60)
    assert copy["title"] == 'Reminder: "Fix the login bug"'
    assert "repeats every 1 hour until the task is done" in copy["body"]
    assert copy["meta"]["context"] == "reminder"
    assert copy["meta"]["persistent"] is True
    daily = _load_ping_copy("organization_task_persistent_reminder_copy")(
        "", {"title": "x" * 400}, 2880)
    assert len(daily["title"]) <= 160
    assert "every 2 days" in daily["body"]


def test_persistent_reminder_notifier_requires_a_member_recipient():
    notifier = ENTRY_TEXT.split(
        "async def notify_organization_task_persistent_reminder", 1)[1].split(
            "    async def reminder_ping_read_at", 1)[0]
    assert "copy = organization_task_persistent_reminder_copy(" in notifier
    assert "not await self.organization_member(org_bi, recipient)" in notifier
    assert "force=True" in notifier
