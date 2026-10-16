import ast
import asyncio
import base64
import importlib.util
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORLD_SRC = ROOT / "world" / "src"
ENTRY = ROOT / "app" / "src" / "entry.py"
ROUTES = WORLD_SRC / "world_office_routes.py"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pings = _load("forkmesh_world_task_pings", WORLD_SRC / "world_task_pings.py")
task_email = _load("forkmesh_world_task_email_for_pings",
                   WORLD_SRC / "world_task_email.py")
office = _load("forkmesh_world_office_tasks_for_pings",
               ROOT / "world" / "tests" / "test_world_office_tasks.py")
tasks_api = office.tasks_api
catalog = _load("forkmesh_catalog_for_task_pings",
                ROOT / "app" / "src" / "catalog.py")

NOW = office.NOW
SECRET = "SECRET-SESSION-TOKEN-0123456789"


def _entry_namespace():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    want_funcs = {"organization_task_ping_copy"}
    want_assigns = {
        "MAX_NODE_NAME", "ORGANIZATION_TASK_ACTION_COPY",
        "ORGANIZATION_TASK_CHECKIN_COPY", "ORGANIZATION_TASK_STATUS_COPY",
    }
    body = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            if {t.id for t in node.targets
                    if isinstance(t, ast.Name)} & want_assigns:
                body.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in want_funcs:
            body.append(node)
    ns = {"clean_string": catalog.clean_string}
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=body, type_ignores=[])), str(ENTRY), "exec"), ns)
    return ns


class Clock:
    def __init__(self, now):
        self.value = now

    def now(self):
        return self.value


class Relay:
    def __init__(self, runtime=None, roster=None):
        self.runtime = runtime
        self.clock = Clock(NOW)
        self.rows = {}
        self.pushes = []
        self.muted = {}
        self.roster = roster or {}
        self.ns = self._namespace()

    def _namespace(self):
        entry = _entry_namespace()
        ns = {
            "world_task_pings": pings,
            "clean_string": catalog.clean_string,
            "valid_node_name": lambda value: bool(re.fullmatch(
                r"[a-z](?:[a-z0-9-]{0,62}[a-z0-9])?", str(value or ""))),
            "MAX_NODE_NAME": entry["MAX_NODE_NAME"],
            "organization_task_ping_copy": entry[
                "organization_task_ping_copy"],
            "Date": self.clock,
            "d1_first": self.d1_first,
            "d1_all": self.d1_all,
            "d1_run": self.d1_run,
            "blind_index": self.blind_index,
            "decrypt_row": self.decrypt_row,
            "enqueue_notification": self.enqueue_notification,
            "_ping_delivery_settings": self.delivery_settings,
        }
        tree = ast.parse(ROUTES.read_text(encoding="utf-8"))
        wanted = {"_TaskPingStore", "_organization_task_participants",
                  "_task_ping_recipients", "notify_organization_task_update",
                  "_task_deletion_plan",
                  "notify_organization_task_deletions"}
        body = [node for node in tree.body
                if getattr(node, "name", "") in wanted]
        assert {node.name for node in body} == wanted
        exec(compile(ast.fix_missing_locations(
            ast.Module(body=body, type_ignores=[])), str(ROUTES), "exec"), ns)
        return ns

    async def delivery_settings(self, _env, recipient, _now):
        return set(self.muted.get(recipient, ())), False

    async def blind_index(self, _env, value):
        return "bi:" + str(value)

    async def decrypt_row(self, _env, value):
        if isinstance(value, str) and value.startswith("sealed:"):
            return await self.runtime.open(value)
        return value

    async def d1_first(self, _env, sql, *args):
        assert "FROM orgs" in sql
        return {"name": "forkmesh"}

    async def d1_all(self, _env, sql, *args):
        if "FROM org_members" in sql:
            roster = self.roster or {
                name: role for name, (role, _perm)
                in self.runtime.memberships.items()}
            return [{"name": name, "role": role}
                    for name, role in roster.items()]
        if "FROM organization_task_events" in sql:
            if self.runtime is None:
                return []
            return await self.runtime.d1_all(sql, *args)
        if "FROM notifications" in sql:
            recipient_bi, since, limit = args
            rows = [row for row in self.rows.values()
                    if row["recipient_bi"] == recipient_bi
                    and row["ts"] >= since]
            rows.sort(key=lambda row: row["ts"], reverse=True)
            return [dict(row) for row in rows[:limit]]
        raise AssertionError(sql)

    async def d1_run(self, _env, sql, *args):
        assert sql.startswith("DELETE FROM notifications")
        dedupe_bi, recipient_bi = args
        row = self.rows.get(dedupe_bi)
        if row and row["recipient_bi"] == recipient_bi:
            del self.rows[dedupe_bi]

    async def enqueue_notification(self, _env, recipient, kind, title,
                                   body="", href="", actor="", source="",
                                   dedupe="", meta=None, push=True):
        if actor and actor == recipient:
            return False
        if kind in self.muted.get(recipient, ()):
            return False
        dedupe_bi = "n:" + recipient + ":" + dedupe
        prior = self.rows.get(dedupe_bi)
        self.rows[dedupe_bi] = {
            "dedupe_bi": dedupe_bi,
            "recipient_bi": "bi:" + recipient,
            "recipient": recipient,
            "ts": self.clock.value,
            "read_at": prior["read_at"] if prior else 0,
            "data": {"kind": kind, "title": title, "body": body,
                     "href": href, "actor": actor, "source": source,
                     "meta": json.loads(json.dumps(meta))},
        }
        self.pushes.append((recipient, title, body, bool(push)))
        return True

    def inbox(self, recipient):
        return [row["data"] for row in sorted(
            self.rows.values(), key=lambda row: row["ts"])
            if row["recipient"] == recipient]

    def buzzes(self, recipient):
        return [push for push in self.pushes
                if push[0] == recipient and push[3]]

    def notifier(self):
        async def notify(org_bi, actor, task_id, task, action, **kwargs):
            await self.ns["notify_organization_task_update"](
                None, org_bi, actor, task_id, task, action, **kwargs)
        return notify

    async def update(self, actor, task, action, changes=None, **kwargs):
        return await self.ns["notify_organization_task_update"](
            None, "org-bi", actor, task["id"], task, action,
            changes=changes, **kwargs)


def run(coro):
    return asyncio.run(coro)


def _task(task_id="a" * 32, title="Fix the login bug", created_by="alice",
          assignee="bob"):
    return {"id": task_id, "title": title, "createdBy": created_by,
            "assignee": assignee, "assigneeKind": "user",
            "status": "idle", "priority": 50, "department": "engineering"}


ROSTER = {"alice": "owner", "jett": "owner", "bob": "member",
          "carol": "member", "dana": "member", "erin": "member"}


def test_every_event_kind_renders_a_title_and_a_short_body():
    cases = [
        ("updated", [{"field": "progress", "from": 40, "to": 60}],
         "Task updated: Fix", "Progress 40% → 60% by @jett"),
        ("updated", [{"field": "priority", "from": 50, "to": 5}],
         "Task updated: Fix", "Priority 50 → 5 by @jett"),
        ("updated", [{"field": "assignee", "kind": "user", "to": "dana",
                      "from": "bob"}],
         "Task updated: Fix", "Assigned to @dana (was @bob) by @jett"),
        ("updated", [{"field": "assignee", "kind": "unassigned", "to": "",
                      "from": "bob"}],
         "Task updated: Fix", "Unassigned (was @bob) by @jett"),
        ("updated", [{"field": "title", "from": "Old name", "to": "New"}],
         "Task updated: Fix", "Renamed from “Old name” by @jett"),
        ("updated", [{"field": "details"}],
         "Task updated: Fix", "Description edited by @jett"),
        ("updated", [{"field": "routing", "department": "marketing",
                      "team": "social"}],
         "Task updated: Fix", "Moved to marketing / social by @jett"),
        ("updated", [{"field": "howToTest"}],
         "Task updated: Fix", "Test steps edited by @jett"),
        ("updated", [{"field": "parent", "to": "b" * 32}],
         "Task updated: Fix", "Parent task set by @jett"),
        ("updated", [{"field": "workItems",
                      "summary": "attached feat/pings"}],
         "Task updated: Fix", "Work items: attached feat/pings by @jett"),
        ("started", None, "Task updated: Fix", "Started by @jett"),
        ("stopped", None, "Task updated: Fix", "Stopped by @jett"),
        ("completed", None, "Task updated: Fix", "Marked done by @jett"),
        ("undone", None, "Task updated: Fix", "Marked undone by @jett"),
        ("qa requested", None, "Task updated: Fix", "Sent to QA by @jett"),
        ("returned", None, "Task updated: Fix",
         "Returned to the task list by @jett"),
        ("qa failed", None, "Task updated: Fix", "QA failed by @jett"),
        ("commented", [{"field": "comment", "excerpt": "x" * 200}],
         "Task updated: Fix", "New comment by @jett: " + "x" * 80 + "…"),
        ("checked in blocked", [{"field": "checkin", "state": "blocked",
                                 "excerpt": "waiting on review"}],
         "Task updated: Fix",
         "Check-in (blocked) by @jett: waiting on review"),
        ("created", None, "New task: Fix",
         "Created by @jett · assigned to @bob, priority 50"),
        ("deleted", None, "Task deleted: Fix", "Deleted by @jett"),
    ]
    for action, changes, title, body in cases:
        relay = Relay(roster=ROSTER)
        filed = run(relay.update("jett", _task(), action, changes,
                                 participants=[]))
        assert filed >= 2, action
        ping = relay.inbox("bob")[0]
        assert ping["kind"] == "organization_task_update"
        assert ping["title"].startswith(title), (action, ping["title"])
        assert ping["body"] == body, (action, ping["body"])
        assert ping["href"] == (
            "/dashboard/tasks/" if action == "deleted"
            else "/dashboard/tasks/?task=" + "a" * 32)
        assert ping["meta"]["taskId"] == "a" * 32
        assert relay.buzzes("bob"), action


def test_recipients_are_the_task_people_and_owners_never_the_actor():
    relay = Relay(roster=dict(ROSTER, zed="member", guest="guest"))
    run(relay.update("jett", _task(), "updated",
                     [{"field": "assignee", "kind": "user", "to": "bob",
                       "from": "erin"}],
                     participants=["carol", "outsider", "jett"]))
    got = {row["recipient"] for row in relay.rows.values()}
    assert got == {"alice", "bob", "carol", "erin"}

    relay = Relay(roster=ROSTER)
    run(relay.update("bob", _task(), "commented",
                     [{"field": "comment", "excerpt": "hi @carol"}],
                     participants=["carol", "dana"], skip=["carol"]))
    got = {row["recipient"] for row in relay.rows.values()}
    assert got == {"alice", "jett", "dana"}

    relay = Relay(roster=ROSTER)
    run(relay.update("claude-bot", _task(assignee=""), "completed",
                     participants=[]))
    assert {row["recipient"] for row in relay.rows.values()} == {
        "alice", "jett"}


class _TaskEmail:
    def __init__(self, fail=False):
        self.calls = []
        self.fail = fail
        self.recipients = task_email.recipients

    async def send_activity(self, _env, org_bi, org, actor, task_id, action,
                            changes, names, meta, details):
        if self.fail:
            raise RuntimeError("smtp down")
        self.calls.append({"org": org, "actor": actor, "action": action,
                           "names": names, "title": meta["taskTitle"],
                           "details": details})
        return len(names)

    async def send_deleted(self, _env, org_bi, org, actor, plan, reason):
        self.calls.append({"org": org, "actor": actor, "action": "deleted",
                           "plan": [(name, [task["id"] for task in tasks])
                                    for name, tasks in plan],
                           "reason": reason})
        return len(plan)


def test_a_deleted_task_pings_but_leaves_email_to_the_deletion_notice():
    relay = Relay(roster=ROSTER)
    mail = relay.ns["task_email"] = _TaskEmail()
    filed = run(relay.update("jett", _task(), "deleted", participants=[]))
    assert filed == 2
    assert mail.calls == []


def test_deletion_notices_reach_creator_and_assignee_once_each():
    relay = Relay(roster=ROSTER)
    mail = relay.ns["task_email"] = _TaskEmail()
    first = _task("a" * 32, created_by="alice", assignee="bob")
    second = _task("b" * 32, created_by="carol", assignee="bob")
    third = _task("c" * 32, created_by="jett", assignee="outsider")
    bot = dict(_task("d" * 32, created_by="jett", assignee="dana"),
               assigneeKind="agent")
    sent = run(relay.ns["notify_organization_task_deletions"](
        None, "org-bi", "jett", [first, second, third, bot], "cleanup"))
    assert sent == 3
    call, = mail.calls
    assert call["org"] == "forkmesh"
    assert call["reason"] == "cleanup"
    assert call["plan"] == [
        ("alice", ["a" * 32]),
        ("bob", ["a" * 32, "b" * 32]),
        ("carol", ["b" * 32]),
    ]
    mail.calls.clear()
    assert run(relay.ns["notify_organization_task_deletions"](
        None, "org-bi", "alice", [_task(assignee="alice")], "")) == 0
    assert mail.calls == []


def test_every_task_activity_is_emailed_alongside_its_ping():
    relay = Relay(roster=ROSTER)
    mail = relay.ns["task_email"] = _TaskEmail()
    run(relay.update("alice", _task(), "created", participants=[]))
    call, = mail.calls
    assert call["action"] == "created"
    assert call["names"] == ["alice", "bob", "jett"]
    assert call["title"] == "Fix the login bug"
    assert call["details"].startswith('@alice created "Fix the login bug"')

    relay = Relay(roster=ROSTER)
    mail = relay.ns["task_email"] = _TaskEmail()
    run(relay.update("bob", _task(), "commented",
                     [{"field": "comment", "excerpt": "hi @carol"}],
                     participants=["carol"], skip=["carol"]))
    assert mail.calls[0]["names"] == ["alice", "carol", "jett"]
    assert {row["recipient"] for row in relay.rows.values()} == {
        "alice", "jett"}

    relay = Relay(roster={"alice": "owner"})
    mail = relay.ns["task_email"] = _TaskEmail()
    run(relay.update("alice", _task(assignee=""), "created",
                     participants=[]))
    assert relay.rows == {}
    assert mail.calls[0]["names"] == ["alice"]


def test_a_failing_task_email_never_loses_the_ping():
    relay = Relay(roster=ROSTER)
    relay.ns["task_email"] = _TaskEmail(fail=True)
    filed = run(relay.update("jett", _task(), "updated",
                             [{"field": "progress", "from": 1, "to": 2}],
                             participants=[]))
    assert filed == 2
    assert {row["recipient"] for row in relay.rows.values()} == {
        "alice", "bob"}


def test_changes_inside_a_minute_coalesce_into_one_ping_per_task():
    relay = Relay(roster=ROSTER)
    task = _task()
    run(relay.update("dana", task, "updated",
                     [{"field": "progress", "from": 40, "to": 50}],
                     participants=[]))
    relay.clock.value += 10_000
    run(relay.update("dana", task, "updated",
                     [{"field": "progress", "from": 50, "to": 60}],
                     participants=[]))
    relay.clock.value += 10_000
    run(relay.update("dana", task, "updated",
                     [{"field": "priority", "from": 50, "to": 5}],
                     participants=[]))
    relay.clock.value += 10_000
    run(relay.update("jett", task, "commented",
                     [{"field": "comment", "excerpt": "looks good"}],
                     participants=[]))
    inbox = relay.inbox("bob")
    assert len(inbox) == 1
    assert inbox[0]["title"] == "Task updated: Fix the login bug"
    assert inbox[0]["body"].startswith(
        "4 changes: progress 60%, priority 5, comment by @dana and @jett")
    assert inbox[0]["body"].endswith("Comment: looks good")
    assert len(relay.buzzes("bob")) == 1
    assert inbox[0]["meta"]["count"] == 4

    relay.clock.value += pings.WINDOW_MS
    run(relay.update("dana", task, "started", participants=[]))
    assert len(relay.inbox("bob")) == 2
    assert len(relay.buzzes("bob")) == 2


def test_a_read_ping_is_not_folded_and_the_next_one_does_not_buzz():
    relay = Relay(roster=ROSTER)
    task = _task()
    run(relay.update("jett", task, "started", participants=[]))
    for row in relay.rows.values():
        row["read_at"] = relay.clock.value
    relay.clock.value += 5_000
    run(relay.update("jett", task, "stopped", participants=[]))
    assert len(relay.inbox("bob")) == 2
    assert len(relay.buzzes("bob")) == 1


def test_a_board_wide_reprioritisation_is_one_digest_per_recipient():
    relay = Relay(roster=ROSTER)
    for index in range(25):
        task = _task(task_id="%032x" % (index + 1),
                     title="Task number %d" % index)
        run(relay.update("jett", task, "updated",
                         [{"field": "priority", "from": 50, "to": index + 1}],
                         participants=[]))
        relay.clock.value += 500
    for recipient in ("alice", "bob"):
        inbox = relay.inbox(recipient)
        assert len(inbox) == 1, recipient
        digest = inbox[0]
        assert digest["meta"]["digest"] is True
        assert digest["title"] == "Tasks updated: 25 tasks"
        assert digest["body"].startswith("25 changes to 25 tasks by @jett")
        assert "Task number 0: priority 1" in digest["body"]
        assert digest["href"] == "/dashboard/tasks/"
        assert len(relay.buzzes(recipient)) <= 2
    assert relay.inbox("jett") == []


def test_a_muted_tasks_kind_files_nothing_and_others_still_hear():
    relay = Relay(roster=ROSTER)
    relay.muted["bob"] = {"organization_task_update"}
    run(relay.update("jett", _task(), "started", participants=[]))
    assert relay.inbox("bob") == []
    assert len(relay.inbox("alice")) == 1


def test_the_catalog_offers_tasks_on_by_default_without_email():
    source = (ROOT / "app" / "src" / "ping_settings.py").read_text(
        encoding="utf-8")
    assert '("organization_task_update", "organizations", "Tasks")' in source
    entry = ENTRY.read_text(encoding="utf-8")
    kinds = entry.split("NOTIFICATION_KINDS = frozenset({", 1)[1].split(
        "})", 1)[0]
    assert '"organization_task_update"' in kinds
    defaults = entry.split("NOTIFICATION_EMAIL_DEFAULTS", 1)[1][:4000]
    assert "organization_task_update" not in defaults


WORK_ITEM = {
    "repository": "forkmesh/src",
    "branch": "feat/launch-post",
    "worktree": "/home/bob/src/.worktrees/launch-post",
    "node": "bob-desk",
}


async def _board_run(runtime, relay):
    handle = tasks_api.handle
    prefix = tasks_api.UNIVERSAL_PREFIX
    created = await handle(runtime.use("POST", "alice", {
        "title": "Write launch post", "details": "Draft",
        "assignee": "bob", "sessionToken": SECRET,
    }), tasks_api.PREFIX)
    task_id = created["data"]["task"]["id"]
    item = "%s/%s" % (prefix, task_id)
    steps = [
        ("PATCH", "alice", {"title": "Write the launch post",
                            "details": "Draft v2", "sessionToken": SECRET},
         item),
        ("PATCH", "alice", {"priority": 7, "sessionToken": SECRET}, item),
        ("PATCH", "alice", {"department": "security", "team": "appsec",
                            "howToTest": "Open the post"}, item),
        ("PATCH", "bob", {"workItems": [WORK_ITEM]}, item),
        ("PATCH", "bob", {"progress": 40, "sessionToken": SECRET}, item),
        ("POST", "bob", {}, item + "/start"),
        ("POST", "bob", {"state": "blocked", "note": "need the logo"},
         item + "/checkin"),
        ("POST", "carol", {"body": "I can help with copy"},
         item + "/replies"),
        ("POST", "bob", {}, item + "/stop"),
        ("POST", "bob", {"completionNote": "shipped"}, item + "/complete"),
        ("POST", "alice", {}, item + "/undo"),
        ("PATCH", "alice", {"assigneeKind": "unassigned", "assignee": ""},
         item),
        ("DELETE", "alice", {}, item),
    ]
    statuses = []
    for method, actor, data, path in steps:
        runtime.now_ms += 1000
        relay.clock.value = runtime.now_ms
        response = await handle(runtime.use(method, actor, data), path)
        statuses.append((path.rsplit("/", 1)[-1], response["status"]))
    return task_id, statuses


def test_every_board_change_reaches_the_notifier_with_what_changed():
    runtime = office.FakeRuntime()
    calls = []

    async def notify(org_bi, actor, task_id, task, action, **kwargs):
        calls.append((actor, action, kwargs.get("changes"),
                      kwargs.get("skip"), kwargs.get("participants")))

    runtime.notify_organization_task_activity = notify
    relay = Relay(runtime)
    runtime.task_participants = lambda org_bi, task_id: relay.ns[
        "_organization_task_participants"](None, org_bi, task_id)
    _, statuses = run(_board_run(runtime, relay))
    assert all(status < 300 for _step, status in statuses), statuses
    fields = [(actor, action,
               [change.get("field") for change in (changes or [])])
              for actor, action, changes, _skip, _parts in calls]
    assert fields == [
        ("alice", "created", []),
        ("alice", "updated", ["title", "details"]),
        ("alice", "updated", ["priority"]),
        ("alice", "updated", ["routing", "howToTest"]),
        ("bob", "updated", ["workItems"]),
        ("bob", "updated", ["progress"]),
        ("bob", "started", []),
        ("bob", "checked in blocked", ["checkin"]),
        ("carol", "commented", ["comment"]),
        ("bob", "stopped", []),
        ("bob", "completed", []),
        ("alice", "undone", []),
        ("alice", "updated", ["assignee"]),
        ("alice", "deleted", []),
    ]
    assert calls[2][2] == [{"field": "priority", "from": 50, "to": 7}]
    assert calls[3][2] == [
        {"field": "routing", "department": "security", "team": "appsec"},
        {"field": "howToTest"},
    ]
    assert calls[4][2] == [{
        "field": "workItems", "summary": "attached feat/launch-post",
    }]
    assert calls[5][2] == [{"field": "progress", "from": 0, "to": 40}]
    assert calls[12][2][0]["from"] == "bob"
    assert calls[-1][4] == ["carol", "bob"]


def test_the_board_run_end_to_end_pings_without_secrets():
    runtime = office.FakeRuntime()
    relay = Relay(runtime)
    runtime.notify_organization_task_activity = relay.notifier()
    runtime.task_participants = lambda org_bi, task_id: relay.ns[
        "_organization_task_participants"](None, org_bi, task_id)
    run(_board_run(runtime, relay))
    everything = json.dumps([row["data"] for row in relay.rows.values()])
    assert SECRET not in everything
    assert "sealed:" not in everything
    assert relay.inbox("alice")
    assert relay.inbox("bob")
    assert relay.inbox("carol")
    for recipient in ("alice", "bob", "carol"):
        for ping in relay.inbox(recipient):
            assert ping["actor"] != recipient
            assert ping["kind"] == "organization_task_update"
