"""Organization-scoped, encrypted Office marketing task persistence."""

import asyncio
import base64
import importlib.util
import hashlib
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[2] / "app"
SRC = ROOT / "src"
sys.path.insert(0, str(SRC))

spec = importlib.util.spec_from_file_location(
    "forkmesh_world_office_tasks", ROOT.parent / "world" / "src" / "world_office_tasks.py")
tasks_api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tasks_api)

NOW = 2_100_000_000_000


def test_task_image_thumbnails_are_tiny_and_image_only():
    valid = "data:image/webp;base64,aGVsbG8="
    result = tasks_api._attachments([
        {
            "name": "shot.webp",
            "mime": "image/webp",
            "size": 2048,
            "thumbnail": valid,
        },
        {
            "name": "notes.txt",
            "mime": "text/plain",
            "size": 50,
            "thumbnail": valid,
        },
        {
            "name": "bad.png",
            "mime": "image/png",
            "size": 50,
            "thumbnail": "data:text/html;base64,PHNjcmlwdD4=",
        },
    ])
    assert result[0]["thumbnail"] == valid
    assert "thumbnail" not in result[1]
    assert "thumbnail" not in result[2]


def run_async_test(function):
    def wrapped(*args, **kwargs):
        return asyncio.run(function(*args, **kwargs))
    return wrapped


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0075_world_office_marketing_tasks.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" / "0096_world_office_marketing_proofs.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" / "0101_world_office_marketing_initiatives.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" / "0105_organization_tasks.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" / "0108_organization_tasks_general_bot.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" /
             "0112_organization_task_global_priority.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" /
             "0113_organization_task_priority_scale.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" /
             "0114_remove_organization_task_limit.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" /
             "0113_task_images_mailtrap_webhook.sql")
            .read_text(encoding="utf-8"))
        # The migration ends with the deployment's schema fingerprint stamp;
        # this in-memory database has no schema_meta, so only the DDL is run.
        self.db.executescript(
            (ROOT / "migrations" / "0180_organization_task_events.sql")
            .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" /
             "0230_organization_task_reminder_events.sql")
            .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0207_org_task_progress.sql")
            .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0222_organization_task_trash.sql")
            .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" /
             "0237_organization_task_persistent_reminders.sql")
            .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
        self.request_method = "GET"
        self.request_data = {}
        self.query_data = {}
        self.actor = ""
        self.same_origin_request = True
        self.now_ms = NOW
        self.ids = 0
        self.audits = []
        self.organization_exists = True
        self.users = {
            name: {"bi": "bi-" + name, "name": name, "active": True}
            for name in (
                "alice",
                "mary",
                "wendy",
                "bob",
                "carol",
                "eve",
                "rootadmin",
                "inactive",
            )
        }
        self.users["inactive"]["active"] = False
        self.users["rootadmin"]["is_admin"] = True
        self.memberships = {
            "alice": ("owner", "admin"),
            "mary": ("member", "maintain"),
            "wendy": ("member", "write"),
            "bob": ("member", "read"),
            "carol": ("member", "read"),
            "inactive": ("member", "read"),
        }
        self.teams = {
            "engineering": {"alice", "bob"},
            "marketing": {"alice", "bob", "carol"},
            "quality-assurance": {"alice", "carol"},
        }

    def use(self, method, actor="", data=None, same_origin=True, query=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        self.same_origin_request = same_origin
        self.query_data = dict(query or {})
        return self

    def method(self):
        return self.request_method

    def now(self):
        return self.now_ms

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def same_origin(self):
        return self.same_origin_request

    def query(self, name):
        return self.query_data.get(name, "")

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    def binary_response(self, data, mime, name):
        return {
            "status": 200,
            "data": data,
            "mime": mime,
            "name": name,
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return (
            (self.request_data, "")
            if isinstance(self.request_data, dict)
            else (None, "invalid_json")
        )

    async def session(self, _data):
        user = self.users.get(self.actor)
        if not user or not user["active"]:
            return "", None
        return user["bi"], {"name": user["name"]}

    async def organization(self):
        if not self.organization_exists:
            return "org-bi", None
        return "org-bi", {"name": "forkmesh"}

    async def membership(self, _org_bi, name):
        return self.memberships.get(name, ("", ""))

    async def active_user(self, name):
        user = self.users.get(name)
        if not user or not user["active"]:
            return None
        return {"bi": user["bi"], "name": name}

    async def eligible_users(self):
        return sorted(
            name for name, user in self.users.items() if user["active"]
        )

    async def organization_members(self, _org_bi):
        return [
            {"bi": self.users[name]["bi"], "name": name}
            for name in sorted(self.memberships)
            if self.users.get(name, {}).get("active")
        ]

    async def organization_member(self, org_bi, name):
        return next(
            (
                member
                for member in await self.organization_members(org_bi)
                if member["name"] == name
            ),
            None,
        )

    async def organization_teams(self, _org_bi):
        return [
            {"team": team, "permission": "read", "members": len(members)}
            for team, members in sorted(self.teams.items())
        ]

    async def team_member(self, _org_bi, team, name):
        return name in self.teams.get(team, set())

    async def marketing_members(self, _org_bi):
        return [
            {"bi": self.users[name]["bi"], "name": name}
            for name in ("bob", "carol")
        ]

    async def marketing_member(self, org_bi, name):
        return next(
            (
                member
                for member in await self.marketing_members(org_bi)
                if member["name"] == name
            ),
            None,
        )

    async def marketing_attendance(self, members, _now):
        return [{
            "date": "2036-07-18",
            "label": "FRI 07/18",
            "hours": [
                {"member": member["name"], "hours": 1.25}
                for member in members
            ],
        }]

    async def seal(self, value):
        return "sealed:" + base64.urlsafe_b64encode(
            json.dumps(value, sort_keys=True).encode()).decode()

    async def open(self, value):
        if not str(value or "").startswith("sealed:"):
            raise ValueError("not sealed")
        return json.loads(base64.urlsafe_b64decode(
            str(value)[7:].encode()).decode())

    async def blind(self, value):
        return hashlib.sha256(
            ("test-blind:" + str(value)).encode()
        ).hexdigest()

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append({
            "actor": actor,
            "action": action,
            "target_type": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    async def d1_all(self, sql, *args):
        return [
            dict(row) for row in self.db.execute(sql, args).fetchall()
        ]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return {"meta": {"changes": cursor.rowcount}}


async def create_task(runtime, assignee="bob", title="Write launch post",
                      details="Coordinate the release digest"):
    return await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": title,
            "details": details,
            "assignee": assignee,
        }),
        tasks_api.PREFIX,
    )


@run_async_test
async def test_start_uses_optional_private_engineering_notifier():
    runtime = FakeRuntime()
    notices = []

    async def notify(org_bi, actor, task_id, task):
        notices.append((org_bi, actor, task_id, task))

    runtime.notify_engineering_task_started = notify
    created = await create_task(runtime)
    task = created["data"]["task"]
    started = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/start",
    )
    assert started["status"] == 200
    assert len(notices) == 1
    assert notices[0][1] == "bob"
    assert notices[0][2] == task["id"]
    assert notices[0][3]["title"] == "Write launch post"
    assert notices[0][3]["status"] == "active"


@run_async_test
async def test_create_and_lifecycle_emit_private_task_activity():
    runtime = FakeRuntime()
    notices = []

    async def notify(org_bi, actor, task_id, task, action, **_kwargs):
        notices.append((actor, task_id, task["title"], action))

    runtime.notify_organization_task_activity = notify
    created = await create_task(runtime)
    task = created["data"]["task"]
    await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/start",
    )
    await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/stop",
    )
    assert [notice[3] for notice in notices] == [
        "created", "started", "stopped",
    ]


@run_async_test
async def test_universal_tasks_are_org_private_routable_and_marketing_compatible():
    runtime = FakeRuntime()
    generic = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "title": "Fix the engineering HUD",
            "details": "Keep this inside the organization.",
            "department": "engineering",
            "team": "engineering",
            "destination": "department",
            "assigneeKind": "user",
            "assignee": "bob",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert generic["status"] == 201
    task = generic["data"]["task"]
    assert task["department"] == "engineering"
    assert task["team"] == "engineering"
    assert task["assigneeKind"] == "user"
    assert task["qa"]["status"] == "unknown"

    org_view = await tasks_api.handle(
        runtime.use("GET", "carol"),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert [item["id"] for item in org_view["data"]["tasks"]] == [task["id"]]
    assert org_view["data"]["privacyBoundary"] == (
        "organization-private-encrypted-at-rest"
    )
    filtered = await tasks_api.handle(
        runtime.use(
            "GET", "alice", query={"department": "engineering"}),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert [item["id"] for item in filtered["data"]["tasks"]] == [task["id"]]

    legacy_view = await tasks_api.handle(
        runtime.use("GET", "alice"),
        tasks_api.LEGACY_MARKETING_PREFIX,
    )
    assert legacy_view["data"]["tasks"] == []
    outsider = await tasks_api.handle(
        runtime.use("GET", "eve"),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert outsider["status"] == 403
    assert outsider["data"]["error"] == "org_member_required"


@run_async_test
async def test_task_catalog_pages_bound_each_worker_request():
    runtime = FakeRuntime()
    for title in ("First", "Second", "Third"):
        created = await create_task(runtime, title=title)
        assert created["status"] == 201

    original_page_size = tasks_api.TASK_LIST_PAGE_SIZE
    tasks_api.TASK_LIST_PAGE_SIZE = 2
    try:
        first = await tasks_api.handle(
            runtime.use("GET", "alice"), tasks_api.UNIVERSAL_PREFIX)
        assert first["status"] == 200
        assert len(first["data"]["tasks"]) == 2
        assert first["data"]["offset"] == 0
        assert first["data"]["hasMore"] is True
        assert first["data"]["nextOffset"] == 2

        second = await tasks_api.handle(
            runtime.use("GET", "alice", query={"offset": "2"}),
            tasks_api.UNIVERSAL_PREFIX,
        )
        assert second["status"] == 200
        assert len(second["data"]["tasks"]) == 1
        assert second["data"]["offset"] == 2
        assert second["data"]["hasMore"] is False
        assert second["data"]["nextOffset"] is None

        invalid = await tasks_api.handle(
            runtime.use("GET", "alice", query={"offset": "lots"}),
            tasks_api.UNIVERSAL_PREFIX,
        )
        assert invalid["status"] == 400
        assert invalid["data"]["error"] == "invalid_task_offset"
    finally:
        tasks_api.TASK_LIST_PAGE_SIZE = original_page_size


@run_async_test
async def test_global_priorities_are_manager_owned_projected_and_sorted():
    runtime = FakeRuntime()
    low = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Lower priority",
            "assignee": "bob",
            "priority": 80,
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    high = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Highest priority",
            "assignee": "bob",
            "priority": 1,
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    denied = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "title": "Member cannot self-promote",
            "assignee": "bob",
            "priority": 2,
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert denied["status"] == 403
    assert denied["data"]["error"] == "manager_required"
    listing = await tasks_api.handle(
        runtime.use("GET", "carol"), tasks_api.UNIVERSAL_PREFIX)
    assert [
        (task["title"], task["priority"])
        for task in listing["data"]["tasks"]
    ] == [
        ("Highest priority", 1),
        ("Lower priority", 80),
    ]
    reprioritized = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"priority": 3}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{low['data']['task']['id']}",
    )
    assert reprioritized["status"] == 200
    assert reprioritized["data"]["task"]["priority"] == 3
    assert high["data"]["task"]["priority"] == 1
    assert runtime.audits[-1]["action"] == (
        "organization.task_priority_changed")
    clamped = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"priority": 500}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{low['data']['task']['id']}",
    )
    assert clamped["data"]["task"]["priority"] == 99
    defaulted = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Normal priority",
            "assignee": "bob",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert defaulted["data"]["task"]["priority"] == 50


@run_async_test
async def test_follow_up_references_parent_and_inherits_assignee_and_routing():
    runtime = FakeRuntime()
    parent_response = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Investigate production error",
            "department": "engineering",
            "destination": "agent",
            "assigneeKind": "agent",
            "repository": "forkmesh/forkmesh",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    parent = parent_response["data"]["task"]
    denied = await tasks_api.handle(
        runtime.use("POST", "carol", {
            "title": "Unauthorized continuation",
            "parentTaskId": parent["id"],
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert denied["status"] == 403

    follow_up = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Verify the production repair",
            "parentTaskId": parent["id"],
            # Client attempts cannot redirect a follow-up to someone else.
            "assigneeKind": "user",
            "assignee": "bob",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert follow_up["status"] == 201
    task = follow_up["data"]["task"]
    assert task["parentTaskId"] == parent["id"]
    assert task["assigneeKind"] == "agent"
    assert task["assignee"] == "agent"
    assert task["department"] == "engineering"
    assert task["destination"] == "agent"
    assert task["repository"] == "forkmesh/forkmesh"
    assert runtime.audits[-1]["details"]["followUp"] is True


@run_async_test
async def test_member_can_submit_exact_non_custodial_sol_bounty_bid():
    runtime = FakeRuntime()
    response = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "kind": "bid",
            "title": "Improve the lobby onboarding",
            "details": "Add a concise first-visit checklist.",
            "bountyAmountSol": "0.125000000",
            "department": "community",
            "destination": "department",
            # A bid cannot impersonate this supplied assignee.
            "assigneeKind": "user",
            "assignee": "carol",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert response["status"] == 201
    task = response["data"]["task"]
    assert task["kind"] == "bid"
    assert task["createdBy"] == "bob"
    assert task["assignee"] == "bob"
    assert task["assigneeKind"] == "user"
    assert task["bountyRequest"] == {
        "currency": "SOL",
        "amountSol": "0.125",
        "lamports": 125_000_000,
        "status": "requested",
    }
    stored = runtime.db.execute(
        "SELECT data FROM organization_tasks WHERE task_id=?",
        (task["id"],),
    ).fetchone()
    assert "Improve the lobby onboarding" not in stored["data"]
    assert runtime.audits[-1]["action"] == "organization.task_created"
    assert runtime.audits[-1]["details"]["kind"] == "bid"
    assert runtime.audits[-1]["details"]["bountyAmountSol"] == "0.125"


@run_async_test
async def test_bounty_bid_rejects_invalid_or_imprecise_sol_amounts():
    for amount in (
        "",
        "0",
        "-1",
        "1e-3",
        "0.0000000001",
        "1000000.000000001",
        "not-sol",
    ):
        runtime = FakeRuntime()
        response = await tasks_api.handle(
            runtime.use("POST", "bob", {
                "kind": "bid",
                "title": "Invalid bounty",
                "bountyAmountSol": amount,
                "department": "general",
                "destination": "department",
            }),
            tasks_api.UNIVERSAL_PREFIX,
        )
        assert response["status"] == 400, amount
        assert response["data"]["error"] == "invalid_bounty_request"
        count = runtime.db.execute(
            "SELECT COUNT(*) FROM organization_tasks"
        ).fetchone()[0]
        assert count == 0


@run_async_test
async def test_universal_tasks_route_to_agents_and_private_qa():
    runtime = FakeRuntime()
    denied_assignment = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "title": "Assign somebody else",
            "department": "engineering",
            "assigneeKind": "user",
            "assignee": "carol",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert denied_assignment["status"] == 403
    assert denied_assignment["data"]["error"] == "cannot_assign_other_member"

    missing_repo = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Run an agent",
            "department": "engineering",
            "assigneeKind": "agent",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert missing_repo["status"] == 400
    assert missing_repo["data"]["error"] == "repository_required"

    agent = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Run the private verification",
            "details": "Agent task details",
            "department": "engineering",
            "destination": "repository",
            "repository": "forkmesh/forkmesh",
            # A legacy per-vendor kind still routes to the one general bot.
            "assigneeKind": "codex",
            "sendToQa": True,
            "howToTest": "Open the World and verify the result.",
            "attachments": [
                {"name": "hud-notes.md", "mime": "text/markdown", "size": 842},
                {
                    "name": "world.png",
                    "mime": "image/png",
                    "size": 4096,
                    "thumbnail": "data:image/png;base64,aGVsbG8=",
                },
            ],
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert agent["status"] == 201
    task = agent["data"]["task"]
    assert task["destination"] == "agent"
    assert task["assignee"] == "agent"
    assert task["assigneeKind"] == "agent"
    assert task["qa"]["requestedAt"] == runtime.now_ms
    assert task["repository"] == "forkmesh/forkmesh"
    assert task["attachments"] == [
        {"name": "hud-notes.md", "mime": "text/markdown", "size": 842},
        {
            "name": "world.png",
            "mime": "image/png",
            "size": 4096,
            "thumbnail": "data:image/png;base64,aGVsbG8=",
        },
    ]
    stored = runtime.db.execute(
        "SELECT data FROM organization_tasks WHERE task_id=?",
        (task["id"],),
    ).fetchone()[0]
    assert "Run the private verification" not in stored
    assert "Agent task details" not in stored


@run_async_test
async def test_task_image_is_encrypted_and_served_only_to_org_members():
    runtime = FakeRuntime()
    png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwC"
        "AAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
    )
    created = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Verify the status board",
            "assignee": "alice",
            "attachments": [{
                "name": "status.png",
                "mime": "image/png",
                "size": len(png),
                "file": base64.b64encode(png).decode("ascii"),
            }],
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert created["status"] == 201
    task = created["data"]["task"]
    attachment = task["attachments"][0]
    assert attachment["url"].startswith(
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/attachments/")
    sealed = runtime.db.execute(
        "SELECT data FROM organization_task_attachments "
        "WHERE attachment_id=?",
        (attachment["id"],),
    ).fetchone()[0]
    assert base64.b64encode(png).decode("ascii") not in sealed

    image = await tasks_api.handle(
        runtime.use("GET", "alice"), attachment["url"])
    assert image["status"] == 200
    assert image["data"] == png
    assert image["mime"] == "image/png"
    outsider = await tasks_api.handle(
        runtime.use("GET", "eve"), attachment["url"])
    assert outsider["status"] == 403


@run_async_test
async def test_task_image_rejects_active_or_spoofed_content():
    runtime = FakeRuntime()
    response = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Bad image",
            "assignee": "alice",
            "attachments": [{
                "name": "not-really.png",
                "mime": "image/png",
                "size": 12,
                "file": base64.b64encode(b"<svg></svg>").decode("ascii"),
            }],
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_task_image"


@run_async_test
async def test_queued_bot_tasks_return_to_the_task_list():
    runtime = FakeRuntime()
    created = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Queue the general bot",
            "department": "engineering",
            "destination": "repository",
            "repository": "forkmesh/forkmesh",
            "assigneeKind": "agent",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert created["status"] == 201
    task_id = created["data"]["task"]["id"]
    linked = await tasks_api.handle(
        runtime.use("PATCH", "mary", {
            "agentSessionId": "0123456789abcdef0123456789abcdef",
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert linked["status"] == 200
    assert linked["data"]["task"]["agentSessionId"]

    # A reader who did not create the task cannot pull it off the node queue.
    forbidden = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/return",
    )
    assert forbidden["status"] == 403

    cancelled = []

    async def cancel(org_bi, session_id):
        cancelled.append(session_id)
        return True

    runtime.cancel_agent_session = cancel
    returned = await tasks_api.handle(
        runtime.use("POST", "mary", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/return",
    )
    assert returned["status"] == 200
    task = returned["data"]["task"]
    assert task["assigneeKind"] == "unassigned"
    assert task["destination"] == "department"
    assert task["assignee"] == ""
    assert task["agentSessionId"] == ""
    assert cancelled == ["0123456789abcdef0123456789abcdef"]
    assert any(
        entry["action"] == "organization.task_returned"
        for entry in runtime.audits
    )

    # A task that is already back on the list is no longer queued anywhere.
    again = await tasks_api.handle(
        runtime.use("POST", "mary", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/return",
    )
    assert again["status"] == 409
    assert again["data"]["error"] == "task_not_queued"


@run_async_test
async def test_org_authorization_manager_roles_and_filtered_reads():
    runtime = FakeRuntime()

    unauthenticated = await tasks_api.handle(
        runtime.use("GET"), tasks_api.PREFIX)
    assert unauthenticated["status"] == 401

    # Platform status is deliberately not part of the authorization adapter.
    # A platform administrator outside the organization cannot infer tasks.
    outsider = await tasks_api.handle(
        runtime.use("GET", "rootadmin"), tasks_api.PREFIX)
    assert outsider["status"] == 403
    assert outsider["data"]["error"] == "org_member_required"
    admin_bypass = await tasks_api.handle(
        runtime.use("POST", "rootadmin", {
            "title": "Platform admin is not an org manager",
            "assignee": "rootadmin",
        }),
        tasks_api.PREFIX,
    )
    assert admin_bypass["status"] == 403

    write_only = await tasks_api.handle(
        runtime.use("POST", "wendy", {
            "title": "Not allowed",
            "assignee": "wendy",
        }),
        tasks_api.PREFIX,
    )
    assert write_only["status"] == 403

    bob_task = await create_task(runtime, "bob", "Bob launch task")
    assert bob_task["status"] == 201
    carol_task = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Carol community task",
            "assignee": "carol",
        }),
        tasks_api.PREFIX,
    )
    assert carol_task["status"] == 201

    manager_list = await tasks_api.handle(
        runtime.use("GET", "mary"), tasks_api.PREFIX)
    assert manager_list["status"] == 200
    assert manager_list["cache_control"].startswith("no-store")
    assert manager_list["data"]["actor"] == "mary"
    assert manager_list["data"]["canManage"] is True
    assert manager_list["data"]["serverNow"] == NOW
    assert {task["assignee"] for task in manager_list["data"]["tasks"]} == {
        "bob", "carol",
    }
    assert "members" in manager_list["data"]
    assert "inactive" not in manager_list["data"]["members"]
    assert manager_list["data"]["members"] == ["bob", "carol"]
    # Managers can assign only to Marketing members, but private Marketing
    # roster/attendance/proof data stays hidden unless the actor is on Marketing.
    assert manager_list["data"]["marketingMembers"] == []
    assert manager_list["data"]["attendanceDays"] == []
    assert manager_list["data"]["proofs"] == []

    bob_list = await tasks_api.handle(
        runtime.use("GET", "bob"), tasks_api.PREFIX)
    assert bob_list["status"] == 200
    assert bob_list["data"]["actor"] == "bob"
    assert bob_list["data"]["canManage"] is False
    assert "members" not in bob_list["data"]
    assert [task["assignee"] for task in bob_list["data"]["tasks"]] == ["bob"]
    assert bob_list["data"]["marketingMembers"] == ["bob", "carol"]
    assert bob_list["data"]["attendanceDays"][0]["hours"][0] == {
        "member": "bob",
        "hours": 1.25,
    }

    inactive_assignee = await create_task(runtime, "inactive")
    assert inactive_assignee["status"] == 400
    assert inactive_assignee["data"]["error"] == (
        "assignee_not_marketing_member"
    )

    external_task = await create_task(runtime, "eve", "External assignment")
    assert external_task["status"] == 400
    assert external_task["data"]["error"] == "assignee_not_marketing_member"
    external_view = await tasks_api.handle(
        runtime.use("GET", "eve"), tasks_api.PREFIX)
    assert external_view["status"] == 403
    assert external_view["data"]["error"] == "org_member_required"
    external_manage = await tasks_api.handle(
        runtime.use("POST", "eve", {
            "title": "No management bypass",
            "assignee": "eve",
        }),
        tasks_api.PREFIX,
    )
    assert external_manage["status"] == 403


@run_async_test
async def test_marketing_proofs_are_encrypted_and_marketing_team_only():
    runtime = FakeRuntime()
    url = "https://social.example/posts/forkmesh-launch"
    label = "Launch announcement"
    denied = await tasks_api.handle(
        runtime.use("POST", "mary", {"url": url, "label": label}),
        f"{tasks_api.PREFIX}/proofs",
    )
    assert denied["status"] == 403
    assert denied["data"]["error"] == "marketing_team_only"

    insecure = await tasks_api.handle(
        runtime.use("POST", "bob", {"url": "http://example.com/post"}),
        f"{tasks_api.PREFIX}/proofs",
    )
    assert insecure["status"] == 400
    assert insecure["data"]["error"] == "invalid_social_url"

    created = await tasks_api.handle(
        runtime.use("POST", "bob", {"url": url, "label": label}),
        f"{tasks_api.PREFIX}/proofs",
    )
    assert created["status"] == 201
    assert created["data"]["proof"]["member"] == "bob"
    assert created["data"]["proof"]["url"] == url
    assert created["data"]["proof"]["label"] == label
    stored = runtime.db.execute(
        "SELECT url_bi,data FROM world_office_marketing_proofs"
    ).fetchone()
    assert url not in stored[0]
    assert url not in stored[1]
    assert label not in stored[1]
    assert url not in json.dumps(runtime.audits)
    assert "social.example" not in json.dumps(runtime.audits)

    marketing_view = await tasks_api.handle(
        runtime.use("GET", "carol"),
        f"{tasks_api.PREFIX}/proofs",
    )
    assert marketing_view["status"] == 200
    assert marketing_view["data"]["proofs"][0]["url"] == url
    outside_view = await tasks_api.handle(
        runtime.use("GET", "eve"),
        f"{tasks_api.PREFIX}/proofs",
    )
    assert outside_view["status"] == 403


@run_async_test
async def test_marketing_initiatives_are_encrypted_deduplicated_and_team_private():
    runtime = FakeRuntime()
    issue = {
        "owner": "forkmesh",
        "repo": "forkmesh",
        "number": 541,
        "title": "Update globe navigation",
    }

    denied = await tasks_api.handle(
        runtime.use("POST", "wendy", issue),
        f"{tasks_api.PREFIX}/initiatives",
    )
    assert denied["status"] == 403
    assert denied["data"]["error"] == "marketing_team_only"

    created = await tasks_api.handle(
        runtime.use("POST", "alice", issue),
        f"{tasks_api.PREFIX}/initiatives",
    )
    assert created["status"] == 201
    initiative = created["data"]["initiative"]
    assert initiative["repo"] == "forkmesh/forkmesh"
    assert initiative["number"] == 541
    assert initiative["href"] == "/forkmesh/forkmesh/issues/541"

    duplicate = await tasks_api.handle(
        runtime.use("POST", "bob", issue),
        f"{tasks_api.PREFIX}/initiatives",
    )
    assert duplicate["status"] == 200
    assert duplicate["data"]["existing"] is True
    assert duplicate["data"]["initiative"]["id"] == initiative["id"]

    stored = runtime.db.execute(
        "SELECT source_bi,data FROM world_office_marketing_initiatives"
    ).fetchone()
    assert "forkmesh" not in stored[0]
    assert issue["title"] not in stored[1]
    assert "forkmesh" not in json.dumps(runtime.audits)
    assert issue["title"] not in json.dumps(runtime.audits)

    marketing_view = await tasks_api.handle(
        runtime.use("GET", "carol"),
        f"{tasks_api.PREFIX}/initiatives",
    )
    assert marketing_view["status"] == 200
    assert marketing_view["data"]["initiatives"][0]["number"] == 541
    outside_view = await tasks_api.handle(
        runtime.use("GET", "wendy"),
        f"{tasks_api.PREFIX}/initiatives",
    )
    assert outside_view["status"] == 403

    manager_board = await tasks_api.handle(
        runtime.use("GET", "alice"),
        tasks_api.PREFIX,
    )
    assert manager_board["status"] == 200
    assert manager_board["data"]["initiatives"] == []
    marketing_board = await tasks_api.handle(
        runtime.use("GET", "bob"),
        tasks_api.PREFIX,
    )
    assert marketing_board["data"]["initiatives"][0]["title"] == issue["title"]


@run_async_test
async def test_assignee_only_server_timer_checkins_and_single_active_task():
    runtime = FakeRuntime()
    first = await create_task(runtime, "bob", "First task")
    first_id = first["data"]["task"]["id"]
    second = await create_task(runtime, "bob", "Second task")
    second_id = second["data"]["task"]["id"]

    manager_cannot_start = await tasks_api.handle(
        runtime.use("POST", "alice", {}),
        f"{tasks_api.PREFIX}/{first_id}/start",
    )
    assert manager_cannot_start["status"] == 403
    assert manager_cannot_start["data"]["error"] == "assignee_only"

    started = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{first_id}/start",
    )
    assert started["status"] == 200
    started_task = started["data"]["task"]
    assert started_task["status"] == "active"
    assert started_task["startedAt"] == NOW
    assert (
        NOW + tasks_api.CHECKIN_MIN_MS
        <= started_task["nextCheckinAt"]
        <= NOW + tasks_api.CHECKIN_MAX_MS
    )

    duplicate_start = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{second_id}/start",
    )
    assert duplicate_start["status"] == 409
    assert duplicate_start["data"]["error"] == "active_task_exists"

    runtime.now_ms += 5000
    running = await tasks_api.handle(
        runtime.use("GET", "bob"),
        f"{tasks_api.PREFIX}/{first_id}",
    )
    assert running["data"]["task"]["elapsedMs"] == 5000

    invalid_checkin = await tasks_api.handle(
        runtime.use("POST", "bob", {"state": "fine"}),
        f"{tasks_api.PREFIX}/{first_id}/checkin",
    )
    assert invalid_checkin["status"] == 400

    checked_in = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "state": "needs_help",
            "note": "Waiting for review",
        }),
        f"{tasks_api.PREFIX}/{first_id}/checkin",
    )
    checked_task = checked_in["data"]["task"]
    assert checked_task["lastCheckin"] == {
        "state": "needs_help",
        "note": "Waiting for review",
        "at": runtime.now_ms,
    }
    assert (
        runtime.now_ms + tasks_api.CHECKIN_MIN_MS
        <= checked_task["nextCheckinAt"]
        <= runtime.now_ms + tasks_api.CHECKIN_MAX_MS
    )

    other_member = await tasks_api.handle(
        runtime.use("POST", "carol", {"state": "going_well"}),
        f"{tasks_api.PREFIX}/{first_id}/checkin",
    )
    assert other_member["status"] == 403

    runtime.now_ms += 4000
    stopped = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{first_id}/stop",
    )
    stopped_task = stopped["data"]["task"]
    assert stopped_task["status"] == "idle"
    assert stopped_task["elapsedMs"] == 9000
    assert stopped_task["startedAt"] == 0
    assert stopped_task["nextCheckinAt"] == 0

    stopped_checkin = await tasks_api.handle(
        runtime.use("POST", "bob", {"state": "going_well"}),
        f"{tasks_api.PREFIX}/{first_id}/checkin",
    )
    assert stopped_checkin["status"] == 409

    now_allowed = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{second_id}/start",
    )
    assert now_allowed["status"] == 200


@run_async_test
async def test_world_exit_stop_active_is_server_timed_idempotent_and_private():
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Stop on World exit")
    task_id = created["data"]["task"]["id"]
    started = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{task_id}/start",
    )
    assert started["status"] == 200
    runtime.now_ms += 12_345

    cross_origin = await tasks_api.handle(
        runtime.use("POST", "bob", {}, same_origin=False),
        f"{tasks_api.PREFIX}/stop-active",
    )
    assert cross_origin["status"] == 403

    stopped = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/stop-active",
    )
    assert stopped == {
        "status": 200,
        "data": {
            "ok": True,
            "stopped": True,
            "serverNow": runtime.now_ms,
        },
        "cache_control": "no-store, max-age=0, must-revalidate",
        "headers": {"x-content-type-options": "nosniff"},
    }
    row = runtime.db.execute(
        "SELECT status,elapsed_ms,started_at,next_checkin_at "
        "FROM organization_tasks WHERE task_id=?",
        (task_id,),
    ).fetchone()
    assert tuple(row) == ("idle", 12_345, 0, 0)

    repeated = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/stop-active",
    )
    assert repeated["status"] == 200
    assert repeated["data"]["stopped"] is False
    assert "task" not in repeated["data"]

    unauthenticated = await tasks_api.handle(
        runtime.use("POST", "", {}),
        f"{tasks_api.PREFIX}/stop-active",
    )
    assert unauthenticated["status"] == 401


@run_async_test
async def test_assignee_or_manager_can_complete_and_trash_waits_for_qa():
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Finish and retain")
    task_id = created["data"]["task"]["id"]
    started = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{task_id}/start",
    )
    assert started["status"] == 200
    runtime.now_ms += 7_500

    outsider_done = await tasks_api.handle(
        runtime.use("POST", "carol", {}),
        f"{tasks_api.PREFIX}/{task_id}/complete",
    )
    assert outsider_done["status"] == 403
    completed = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "completionNote": (
                "Implemented the requested flow and ran the focused tests."
            ),
        }),
        f"{tasks_api.PREFIX}/{task_id}/complete",
    )
    assert completed["status"] == 200
    assert completed["data"]["task"]["status"] == "done"
    assert completed["data"]["task"]["elapsedMs"] == 7_500
    assert completed["data"]["task"]["completedAt"] == runtime.now_ms
    assert completed["data"]["task"]["destination"] == "qa"
    assert completed["data"]["task"]["qa"]["status"] == "unknown"
    assert completed["data"]["task"]["qa"]["requestedAt"] == runtime.now_ms
    assert completed["data"]["task"]["qa"]["howToTest"]
    assert completed["data"]["task"]["completionNote"] == (
        "Implemented the requested flow and ran the focused tests."
    )
    sealed = runtime.db.execute(
        "SELECT data FROM organization_tasks WHERE task_id=?",
        (task_id,),
    ).fetchone()[0]
    assert "Implemented the requested flow" not in sealed
    assert runtime.audits[-1]["details"]["hasCompletionNote"] is True

    revised = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "completionNote": (
                "Implemented, reviewed the diff, and passed focused QA."
            ),
        }),
        f"{tasks_api.PREFIX}/{task_id}/complete",
    )
    assert revised["status"] == 200
    assert revised["data"]["task"]["completedAt"] == runtime.now_ms
    assert revised["data"]["task"]["completionNote"] == (
        "Implemented, reviewed the diff, and passed focused QA."
    )
    assert runtime.audits[-1]["action"] == (
        "organization.task_completion_note_updated"
    )

    restart = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{task_id}/start",
    )
    assert restart["status"] == 409
    assert restart["data"]["error"] == "task_completed"

    outsider_delete = await tasks_api.handle(
        runtime.use("DELETE", "carol", {}),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert outsider_delete["status"] == 403
    assignee_delete = await tasks_api.handle(
        runtime.use("DELETE", "bob", {}),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert assignee_delete["status"] == 409
    assert assignee_delete["data"]["blockedBy"] == ["qa_in_progress"]
    manager_force = await tasks_api.handle(
        runtime.use("DELETE", "mary", {"force": True}),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert manager_force["status"] == 403
    assert manager_force["data"]["error"] == "force_requires_owner"
    deleted = await tasks_api.handle(
        runtime.use("DELETE", "alice", {"force": True}),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert deleted["status"] == 200
    assert deleted["data"]["deleted"] is True
    assert deleted["data"]["restorable"] is True
    assert await runtime.d1_first(
        "SELECT deleted_at FROM organization_tasks WHERE task_id=?",
        task_id,
    ) == {"deleted_at": runtime.now_ms}
    assert {
        item["action"] for item in runtime.audits
    }.issuperset({
        "office.marketing_task_completed",
        "organization.task_delete",
    })
    assert runtime.audits[-1]["details"]["overrode"] == ["qa_in_progress"]


@run_async_test
async def test_member_replies_are_encrypted_and_bounded_per_task():
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Private reply window")
    task_id = created["data"]["task"]["id"]

    for index in range(tasks_api.MAX_TASK_REPLIES + 5):
        saved = await tasks_api.handle(
            runtime.use("POST", "carol", {"body": f"reply {index}"}),
            f"{tasks_api.PREFIX}/{task_id}/replies",
        )
        assert saved["status"] == 201

    outsider = await tasks_api.handle(
        runtime.use("GET", "eve"),
        f"{tasks_api.PREFIX}/{task_id}/replies",
    )
    assert outsider["status"] == 403
    listed = await tasks_api.handle(
        runtime.use("GET", "carol"),
        f"{tasks_api.PREFIX}/{task_id}/replies",
    )
    task = listed["data"]["task"]
    assert task["replyCount"] == tasks_api.MAX_TASK_REPLIES
    assert len(task["replies"]) == tasks_api.MAX_TASK_REPLIES
    assert task["replies"][0]["body"] == "reply 5"
    assert task["replies"][-1]["body"] == "reply 54"
    sealed = runtime.db.execute(
        "SELECT data FROM organization_task_responses WHERE task_id=?",
        (task_id,),
    ).fetchall()
    assert len(sealed) == tasks_api.MAX_TASK_REPLIES
    assert all("reply 54" not in row[0] for row in sealed)


@run_async_test
async def test_a_comment_shows_who_has_read_it():
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Read receipts")
    task_id = created["data"]["task"]["id"]
    task_path = f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}"

    posted = await tasks_api.handle(
        runtime.use("POST", "carol", {"body": "Can you look?"}),
        task_path + "/replies")
    assert posted["status"] == 201
    task = posted["data"]["task"]
    assert task["replies"][-1]["readBy"] == []
    assert task["history"][-1]["kind"] == "replied"
    assert task["history"][-1]["createdAt"] == task["replies"][-1]["createdAt"]

    own = await tasks_api.handle(runtime.use("GET", "carol"), task_path)
    assert own["data"]["task"]["replies"][-1]["readBy"] == []

    runtime.now_ms += 1000
    history = await tasks_api.handle(
        runtime.use("GET", "bob"), task_path + "/history")
    readers = history["data"]["replies"][-1]["readBy"]
    assert [reader["name"] for reader in readers] == ["bob"]
    assert readers[0]["at"] == runtime.now_ms
    sealed = runtime.db.execute(
        "SELECT data FROM organization_tasks WHERE task_id=?",
        (task_id,)).fetchone()[0]
    assert "bob" not in sealed

    runtime.now_ms += 1000
    again = await tasks_api.handle(runtime.use("GET", "bob"), task_path)
    assert again["data"]["task"]["replies"][-1]["readBy"][0]["at"] == (
        readers[0]["at"])

    runtime.now_ms += 1000
    newer = await tasks_api.handle(
        runtime.use("POST", "carol", {"body": "Any news?"}),
        task_path + "/replies")
    replies = newer["data"]["task"]["replies"]
    assert [r["name"] for r in replies[0]["readBy"]] == ["bob"]
    assert replies[-1]["readBy"] == []


@run_async_test
async def test_mark_undone_clears_live_qa_and_restores_route_history():
    runtime = FakeRuntime()
    created = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Reopen after QA",
            "department": "engineering",
            "destination": "personal",
            "assigneeKind": "user",
            "assignee": "bob",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    task_id = created["data"]["task"]["id"]
    completed = await tasks_api.handle(
        runtime.use("POST", "bob", {"completionNote": "Keep this note"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/complete",
    )
    assert completed["status"] == 200

    row = await runtime.d1_first(
        "SELECT * FROM organization_tasks WHERE task_id=?", task_id)
    current = await runtime.open(row["data"])
    current["qaReviewer"] = "carol"
    await runtime.d1_run(
        "UPDATE organization_tasks SET qa_status='failed',"
        "qa_reviewer_bi=?,qa_reviewed_at=?,data=? WHERE task_id=?",
        "bi-carol",
        runtime.now_ms + 1,
        await runtime.seal(current),
        task_id,
    )
    await runtime.d1_run(
        "INSERT INTO organization_task_qa_reviews "
        "(task_id,org_bi,reviewer_bi,verdict,data,reviewed_at) VALUES (?,?,?,?,?,?)",
        task_id,
        "org-bi",
        "bi-carol",
        "failed",
        await runtime.seal({"reason": "needs a follow-up"}),
        runtime.now_ms + 1,
    )

    undone = await tasks_api.handle(
        runtime.use("POST", "carol"),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/undo",
    )
    task = undone["data"]["task"]
    assert undone["status"] == 200
    assert task["status"] == "idle"
    assert task["destination"] == "personal"
    assert task["completionNote"] == "Keep this note"
    assert task["qa"] == {
        "status": "unknown",
        "reviewer": "",
        "reviewedAt": 0,
        "requestedAt": 0,
        "howToTest": task["qa"]["howToTest"],
    }
    assert task["completionHistory"][-1]["qaStatus"] == "failed"
    assert task["completionHistory"][-1]["undoneBy"] == "carol"
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_task_qa_reviews WHERE task_id=?",
        (task_id,),
    ).fetchone()[0] == 0
    resumed = await tasks_api.handle(
        runtime.use("POST", "bob"),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/start",
    )
    assert resumed["status"] == 200
    assert resumed["data"]["task"]["status"] == "active"
    assert resumed["data"]["task"]["destination"] == "personal"
    completed_again = await tasks_api.handle(
        runtime.use("POST", "bob", {"completionNote": "Second pass"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/complete",
    )
    history = completed_again["data"]["task"]["completionHistory"]
    assert history[-2]["note"] == "Keep this note"
    assert history[-1]["note"] == "Second pass"


@run_async_test
async def test_encrypted_copy_same_origin_reassignment_and_metadata_only_audit():
    runtime = FakeRuntime()
    denied_origin = await tasks_api.handle(
        runtime.use(
            "POST",
            "alice",
            {"title": "No cross-site writes", "assignee": "bob"},
            same_origin=False,
        ),
        tasks_api.PREFIX,
    )
    assert denied_origin["status"] == 403
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_tasks"
    ).fetchone()[0] == 0

    title = "Distinctive confidential launch phrase"
    details = "Partner sequence stays inside the encrypted task row"
    created = await create_task(runtime, "bob", title, details)
    task_id = created["data"]["task"]["id"]

    started = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{task_id}/start",
    )
    assert started["status"] == 200
    active_reassign = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"assignee": "carol"}),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert active_reassign["status"] == 409

    note = "Blocked on a private campaign dependency"
    checkin = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "state": "blocked",
            "note": note,
        }),
        f"{tasks_api.PREFIX}/{task_id}/checkin",
    )
    assert checkin["status"] == 200

    task_storage = runtime.db.execute(
        "SELECT data FROM organization_tasks"
    ).fetchone()[0]
    checkin_storage = runtime.db.execute(
        "SELECT data FROM organization_task_checkins"
    ).fetchone()[0]
    assert title not in task_storage
    assert details not in task_storage
    assert note not in checkin_storage

    audit_dump = json.dumps(runtime.audits, sort_keys=True)
    assert title not in audit_dump
    assert details not in audit_dump
    assert note not in audit_dump
    assert all(
        set(item["details"]).issubset({
            "assigned", "fields", "state", "source"})
        for item in runtime.audits
    )

    await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.PREFIX}/{task_id}/stop",
    )
    reassigned = await tasks_api.handle(
        runtime.use("PATCH", "mary", {
            "assignee": "carol",
            "details": "New bounded details",
        }),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert reassigned["status"] == 200
    assert reassigned["data"]["task"]["assignee"] == "carol"

    agent = await tasks_api.handle(
        runtime.use("PATCH", "mary", {
            "assigneeKind": "agent",
            "repository": "forkmesh/forkmesh",
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert agent["status"] == 200
    assert agent["data"]["task"]["assigneeKind"] == "agent"
    assert agent["data"]["task"]["assignee"] == "agent"
    assert agent["data"]["task"]["destination"] == "agent"
    assert agent["data"]["task"]["repository"] == "forkmesh/forkmesh"

    returned = await tasks_api.handle(
        runtime.use("PATCH", "mary", {"assigneeKind": "unassigned"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert returned["status"] == 200
    assert returned["data"]["task"]["assigneeKind"] == "unassigned"
    assert returned["data"]["task"]["assignee"] == ""
    assert returned["data"]["task"]["destination"] == "department"

    missing_repository = await tasks_api.handle(
        runtime.use("PATCH", "mary", {
            "assigneeKind": "agent",
            "repository": "",
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert missing_repository["status"] == 400
    assert missing_repository["data"]["error"] == "repository_required"

    member_update = await tasks_api.handle(
        runtime.use("PATCH", "bob", {"title": "Not allowed"}),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert member_update["status"] == 403


@run_async_test
async def test_delete_purges_the_promoted_legacy_row_so_it_cannot_return():
    """A deleted task must not be restored by the legacy Marketing backfill.

    schema.py replays "INSERT OR IGNORE INTO organization_tasks ... SELECT ...
    FROM world_office_marketing_tasks" whenever the schema fingerprint changes,
    so a surviving legacy row silently resurrects a deleted task.
    """
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Promoted from Marketing")
    task_id = created["data"]["task"]["id"]
    promoted = runtime.db.execute(
        "SELECT task_id,org_bi,status,assignee_bi,active_assignee_bi,data,"
        "created_by_bi,created_at,updated_at FROM organization_tasks "
        "WHERE task_id=?",
        (task_id,),
    ).fetchone()
    runtime.db.execute(
        "INSERT INTO world_office_marketing_tasks ("
        "task_id,org_bi,status,assignee_bi,active_assignee_bi,data,"
        "created_by_bi,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
        tuple(promoted),
    )
    runtime.db.execute(
        "INSERT INTO world_office_marketing_checkins ("
        "checkin_id,task_id,org_bi,account_bi,state,data,created_at) "
        "VALUES (?,?,?,?,?,?,?)",
        ("c" * 32, task_id, "org-bi", "bi-bob", "going_well", "sealed", NOW),
    )
    runtime.db.commit()

    deleted = await tasks_api.handle(
        runtime.use("DELETE", "mary", {}),
        f"{tasks_api.PREFIX}/{task_id}",
    )
    assert deleted["status"] == 200
    assert deleted["data"]["deleted"] is True
    runtime.now_ms += tasks_api.TRASH_RETENTION_MS
    purged = await tasks_api.purge_trash(runtime, runtime.now_ms)
    assert purged["purged"] == 1
    for table in (
            "organization_tasks",
            "world_office_marketing_tasks",
            "world_office_marketing_checkins",
    ):
        assert runtime.db.execute(
            "SELECT COUNT(*) FROM " + table + " WHERE task_id=?",
            (task_id,),
        ).fetchone()[0] == 0


@run_async_test
async def test_desktop_prompt_task_records_and_seals_agent_run_provenance():
    """A prompt-launched task explains which bot ran it, and how (adhoc #18)."""

    runtime = FakeRuntime()
    opened = await tasks_api.handle(
        runtime.use("POST", "wendy", {
            "title": "Add a task toggle to the composer",
            "details": "Prompt typed in the desktop",
            "department": "engineering",
            "repository": "forkmesh/forkmesh",
            "assigneeKind": "claude",
            "agent": {
                "provider": "claude-code",
                "startedBy": "claude-code@Workstation",
                "model": "opus",
                "mode": "Auto mode",
                "strength": "XHIGH",
                "sessionId": "418",
                "status": "queued",
                "finishedBy": "",
            },
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert opened["status"] == 201
    task = opened["data"]["task"]
    assert task["assigneeKind"] == "agent"
    assert task["agentSessionId"] == "418"
    assert task["agent"] == {
        "provider": "claude-code",
        "startedBy": "claude-code@workstation",
        "finishedBy": "",
        "model": "opus",
        "mode": "Auto mode",
        "strength": "xhigh",
        "sessionId": "418",
        "status": "queued",
    }
    stored = runtime.db.execute(
        "SELECT data FROM organization_tasks WHERE task_id=?",
        (task["id"],),
    ).fetchone()[0]
    assert "claude-code@workstation" not in stored
    assert "Auto mode" not in stored

    waiting = await tasks_api.handle(
        runtime.use("POST", "wendy", {"status": "waiting"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/agent-status",
    )
    assert waiting["status"] == 200
    assert waiting["data"]["task"]["agent"]["status"] == "waiting"
    denied_status = await tasks_api.handle(
        runtime.use("POST", "carol", {"status": "running"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/agent-status",
    )
    assert denied_status["status"] == 403

    # The bot has no member assignee_bi, so the desktop that opened the task —
    # a plain member without manager rights — reports the run as finished.
    done = await tasks_api.handle(
        runtime.use("POST", "wendy", {
            "completionNote": "claude-code@workstation finished with success.",
            "agent": {
                "finishedBy": "claude-code@workstation",
                "model": "sonnet",
                "status": "success",
            },
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/complete",
    )
    assert done["status"] == 200
    finished = done["data"]["task"]
    assert finished["status"] == "done"
    # The launch stamp survives; only the fields the finish reported change.
    assert finished["agent"]["startedBy"] == "claude-code@workstation"
    assert finished["agent"]["finishedBy"] == "claude-code@workstation"
    assert finished["agent"]["model"] == "sonnet"
    assert finished["agent"]["mode"] == "Auto mode"
    assert finished["agent"]["strength"] == "xhigh"
    assert finished["agent"]["status"] == "success"


@run_async_test
async def test_agent_status_batch_reports_a_whole_fleet_in_one_write():
    """adhoc #1618: a restart published one request per session, and the relay
    rate-limited its own desktop. The batch reports every run at once, with the
    per-task ownership gate applied to each entry."""

    runtime = FakeRuntime()
    ids = []
    for index in range(3):
        opened = await tasks_api.handle(
            runtime.use("POST", "wendy", {
                "title": "Fleet run %d" % index,
                "department": "engineering",
                "repository": "forkmesh/forkmesh",
                "assigneeKind": "agent",
                "agent": {"provider": "claude-code", "sessionId": str(index),
                          "status": "queued"},
            }),
            tasks_api.UNIVERSAL_PREFIX,
        )
        assert opened["status"] == 201
        ids.append(opened["data"]["task"]["id"])

    # A plain member's task, which the reporting desktop does not own.
    others = await tasks_api.handle(
        runtime.use("POST", "carol", {
            "title": "Someone else's run",
            "department": "engineering",
            "repository": "forkmesh/forkmesh",
            "assigneeKind": "agent",
            "agent": {"provider": "codex", "sessionId": "9", "status": "queued"},
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert others["status"] == 201
    foreign = others["data"]["task"]["id"]

    batch = await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": ids[0], "status": "running"},
            {"task": ids[1], "status": "waiting"},
            {"task": ids[2], "status": "success"},
            {"task": foreign, "status": "failed"},
            {"task": "f" * 32, "status": "running"},
            {"task": ids[0], "status": "stopped"},   # duplicate: ignored
            {"task": ids[1], "status": "sleeping"},  # not a run state
            {"task": "not-a-task-id", "status": "running"},
        ]}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    assert batch["status"] == 200
    results = {row["task"]: row for row in batch["data"]["results"]}
    assert results[ids[0]]["ok"] and results[ids[1]]["ok"] and results[ids[2]]["ok"]
    # Not this desktop's task, a task that does not exist, and a status that is
    # not a run state each fail alone rather than failing the whole write.
    assert results[foreign] == {"task": foreign, "ok": False, "error": "forbidden"}
    assert results["f" * 32]["error"] == "task_not_found"
    assert len(batch["data"]["results"]) == 5
    assert results[ids[0]]["ok"] is True  # the first entry won, not the dup

    for task_id, expected in zip(ids, ("running", "waiting", "success")):
        current = await tasks_api.handle(
            runtime.use("GET", "wendy", {}),
            f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
        )
        assert current["data"]["task"]["agent"]["status"] == expected
    foreign_now = await tasks_api.handle(
        runtime.use("GET", "carol", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{foreign}",
    )
    assert foreign_now["data"]["task"]["agent"]["status"] == "queued"

    for body in ({}, {"statuses": []}, {"statuses": "running"},
                 {"statuses": [{"task": "nope", "status": "running"}]}):
        refused = await tasks_api.handle(
            runtime.use("POST", "wendy", body),
            f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
        )
        assert refused["status"] == 400, body
    oversized = await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": ids[0], "status": "running"}
        ] * (tasks_api.MAX_AGENT_STATUS_BATCH + 1)}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    assert oversized["status"] == 400
    assert oversized["data"]["error"] == "agent_status_batch_too_large"
    not_allowed = await tasks_api.handle(
        runtime.use("GET", "wendy", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    assert not_allowed["status"] == 405

    # An unrelated member still cannot close somebody else's agent run out.
    other = await tasks_api.handle(
        runtime.use("POST", "bob", {"title": "Another agent run",
                                    "department": "engineering",
                                    "repository": "forkmesh/forkmesh",
                                    "assigneeKind": "codex"}),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert other["status"] == 201
    denied = await tasks_api.handle(
        runtime.use("POST", "carol", {"completionNote": "not mine"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{other['data']['task']['id']}/complete",
    )
    assert denied["status"] == 403
    assert denied["data"]["error"] == "assignee_or_manager_only"


@run_async_test
async def test_hand_opened_task_accepts_the_run_a_desktop_starts_on_it():
    """The Tasks page's "Start agent with prompt settings" button binds a local
    run to a task that already exists, so that task has no run provenance when
    the first state arrives. Seeding it is what lets the board answer "who is
    working this?" for a task nobody launched from a prompt."""

    runtime = FakeRuntime()
    opened = await tasks_api.handle(
        runtime.use("POST", "wendy", {
            "title": "Written by hand on the board",
            "details": "No agent ran when this was filed",
            "department": "engineering",
            "repository": "forkmesh/forkmesh",
            "assigneeKind": "agent",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert opened["status"] == 201
    task = opened["data"]["task"]
    assert task["agent"] is None

    started = await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [{
            "task": task["id"],
            "status": "running",
            "agent": {
                "provider": "claude-code",
                "startedBy": "claude-code@Workstation",
                "model": "opus",
                "mode": "Auto",
                "strength": "HIGH",
                "sessionId": "1515",
                # A status smuggled into the provenance never wins over the
                # state the entry actually reports.
                "status": "success",
            },
        }]}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    assert started["status"] == 200
    assert started["data"]["results"][0]["ok"] is True
    current = await tasks_api.handle(
        runtime.use("GET", "wendy", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}",
    )
    agent = current["data"]["task"]["agent"]
    assert agent["provider"] == "claude-code"
    assert agent["startedBy"] == "claude-code@workstation"
    assert agent["model"] == "opus"
    assert agent["mode"] == "Auto"
    assert agent["strength"] == "high"
    assert agent["sessionId"] == "1515"
    assert agent["status"] == "running"

    # A later state keeps the launch stamp rather than blanking it.
    finished = await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": task["id"], "status": "success"},
        ]}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    assert finished["status"] == 200
    settled = await tasks_api.handle(
        runtime.use("GET", "wendy", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}",
    )
    assert settled["data"]["task"]["agent"]["sessionId"] == "1515"
    assert settled["data"]["task"]["agent"]["status"] == "success"

    # Seeding provenance is not a way around the ownership gate.
    denied = await tasks_api.handle(
        runtime.use("POST", "carol", {"statuses": [{
            "task": task["id"],
            "status": "failed",
            "agent": {"provider": "codex", "sessionId": "77"},
        }]}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    assert denied["data"]["results"][0]["error"] == "forbidden"
    unchanged = await tasks_api.handle(
        runtime.use("GET", "wendy", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}",
    )
    assert unchanged["data"]["task"]["agent"]["sessionId"] == "1515"


@run_async_test
async def test_agent_run_provenance_is_bounded_and_edit_safe():
    runtime = FakeRuntime()
    opened = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Bounded provenance",
            "department": "engineering",
            "repository": "forkmesh/forkmesh",
            "assigneeKind": "codex",
            "agent": {
                "startedBy": "codex@" + "n" * 200,
                "model": "<script>gpt</script>",
                "sessionId": "no spaces or markup <b>",
                "strength": "high",
            },
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert opened["status"] == 201
    task = opened["data"]["task"]
    assert len(task["agent"]["startedBy"]) <= tasks_api.MAX_AGENT_FIELD
    assert "<" not in task["agent"]["model"]
    # A reference that cannot be a bounded opaque token is dropped, not stored.
    assert task["agent"]["sessionId"] == ""
    assert task["agentSessionId"] == ""

    # A manager editing the copy never rewrites who ran the task or how.
    edited = await tasks_api.handle(
        runtime.use("PATCH", "alice", {
            "title": "Bounded provenance, retitled",
            "assignee": "alice",
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}",
    )
    assert edited["status"] == 200
    assert edited["data"]["task"]["agent"]["strength"] == "high"

    # A task with no agent record reports none rather than an empty shell.
    plain = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Human task",
            "department": "engineering",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert plain["data"]["task"]["agent"] is None


def test_migration_has_conditional_active_constraint_and_bounded_tables():
    migration = (
        ROOT / "migrations" / "0075_world_office_marketing_tasks.sql"
    ).read_text(encoding="utf-8")
    assert "CREATE UNIQUE INDEX" in migration
    assert "WHERE status = 'active' AND active_assignee_bi <> ''" in migration
    assert "trg_world_office_marketing_task_limit" in migration
    assert "trg_world_office_marketing_checkin_limit" in migration


@run_async_test
async def test_task_history_does_not_cap_new_owner_or_agent_work():
    runtime = FakeRuntime()
    trigger = runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE type='trigger' "
        "AND name='trg_organization_task_limit'"
    ).fetchone()
    assert trigger is None

    runtime.db.executemany(
        "INSERT INTO organization_tasks "
        "(task_id,org_bi,data,created_by_bi,created_at,updated_at) "
        "VALUES (?,?,?,?,?,?)",
        [
            (
                f"{index:032x}",
                "org-bi",
                "sealed:e30=",
                "bi-alice",
                NOW - index,
                NOW - index,
            )
            for index in range(1, 2001)
        ],
    )
    runtime.ids = 3000
    created = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Queue after a long durable history",
            "department": "engineering",
            "destination": "agent",
            "assigneeKind": "agent",
            "repository": "forkmesh/forkmesh",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert created["status"] == 201
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_tasks WHERE org_bi='org-bi'"
    ).fetchone()[0] == 2001


def test_entry_wiring_is_http_only_and_has_no_admin_bypass():
    # The Default class moved into app_runtime.py and the Office route bodies
    # into world_office_routes.py, both to keep the Worker's Python startup
    # source small; this wiring contract spans all three.
    entry = (
        (SRC / "entry.py").read_text(encoding="utf-8")
        + (SRC / "app_runtime.py").read_text(encoding="utf-8")
        + (ROOT.parent / "world" / "src" / "world_office_routes.py").read_text(
            encoding="utf-8")
    )
    module = (ROOT.parent / "world" / "src" / "world_office_tasks.py").read_text(encoding="utf-8")
    assert 'world_office_tasks = _LazyModule("world_office_tasks")' in entry
    assert '"/api/world/office/marketing-tasks"' in entry
    assert "_OfficeMarketingTasksRuntime" in entry
    assert "OFFICE_MARKETING_ORG" in entry
    assert "is_admin" not in module
    assert "DurableObject" not in module
    assert "WebSocket" not in module


@run_async_test
async def test_task_timeline_records_everything_that_happens_to_one_task():
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Ship the digest")
    task_id = created["data"]["task"]["id"]
    assert [entry["kind"] for entry in created["data"]["task"]["history"]] == [
        "created",
    ]

    for path, body in (
            (f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/start", {}),
            (
                f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/checkin",
                {"state": "blocked", "note": "waiting on review"},
            ),
            (
                f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/replies",
                {"body": "picking this back up"},
            ),
            (f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/stop", {}),
            (
                f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/complete",
                {"completionNote": "digest is out"},
            ),
    ):
        runtime.now_ms += 1000
        response = await tasks_api.handle(
            runtime.use("POST", "bob", body), path)
        assert response["status"] in (200, 201), (path, response)

    history = await tasks_api.handle(
        runtime.use("GET", "bob"),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/history",
    )
    assert history["status"] == 200
    entries = history["data"]["history"]
    assert [entry["kind"] for entry in entries] == [
        "created", "started", "checkin", "replied", "stopped", "completed",
    ]
    # Oldest first, so a client renders the timeline top-down without sorting.
    assert entries == sorted(entries, key=lambda entry: entry["createdAt"])
    assert {entry["actor"] for entry in entries} == {"alice", "bob"}
    summaries = {entry["kind"]: entry["summary"] for entry in entries}
    assert summaries["checkin"] == "blocked — waiting on review"
    assert summaries["replied"] == "picking this back up"
    assert summaries["completed"] == "digest is out"

    # The actor name and the summary are sealed; only the bounded kind, the
    # account blind index and the timestamp are readable in the clear.
    stored = runtime.db.execute(
        "SELECT kind,actor_bi,data FROM organization_task_events "
        "WHERE task_id=?", (task_id,)).fetchall()
    assert stored
    for row in stored:
        assert row["kind"] in tasks_api.TASK_EVENT_KINDS
        assert row["actor_bi"].startswith("bi-")
        assert "waiting on review" not in row["data"]
        assert "picking this back up" not in row["data"]


@run_async_test
async def test_task_history_is_org_private_and_bounded_per_task():
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Bounded timeline")
    task_id = created["data"]["task"]["id"]
    for index in range(tasks_api.MAX_TASK_EVENTS + 5):
        runtime.now_ms += 1
        await tasks_api.handle(
            runtime.use("POST", "carol", {"body": f"comment {index}"}),
            f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/replies",
        )
    history = await tasks_api.handle(
        runtime.use("GET", "bob"),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/history",
    )
    entries = history["data"]["history"]
    assert len(entries) == tasks_api.MAX_TASK_EVENTS
    assert entries[-1]["summary"] == (
        f"comment {tasks_api.MAX_TASK_EVENTS + 4}")

    stranger = await tasks_api.handle(
        runtime.use("GET", "eve"),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/history",
    )
    assert stranger["status"] == 403
    assert stranger["data"]["error"] == "org_member_required"

    not_allowed = await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/history",
    )
    assert not_allowed["status"] == 405


@run_async_test
async def test_deleting_a_task_takes_its_timeline_with_it():
    runtime = FakeRuntime()
    created = await create_task(runtime, "bob", "Temporary work")
    task_id = created["data"]["task"]["id"]
    await tasks_api.handle(
        runtime.use("POST", "bob", {"body": "on it"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/replies",
    )
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_task_events WHERE task_id=?",
        (task_id,)).fetchone()[0] > 0
    deleted = await tasks_api.handle(
        runtime.use("DELETE", "alice"),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert deleted["status"] == 200
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_task_events WHERE task_id=?",
        (task_id,)).fetchone()[0] > 0
    runtime.now_ms += tasks_api.TRASH_RETENTION_MS
    await tasks_api.purge_trash(runtime, runtime.now_ms)
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_task_events WHERE task_id=?",
        (task_id,)).fetchone()[0] == 0


@run_async_test
async def test_comments_and_checkins_ping_the_floor_and_the_names_they_use():
    runtime = FakeRuntime()
    activity = []
    mentions = []

    async def notify(org_bi, actor, task_id, task, action, **_kwargs):
        activity.append((actor, action))

    async def notify_mentions(
            org_bi, actor, task_id, task, names, context, excerpt, ts):
        mentions.append({
            "actor": actor,
            "names": list(names),
            "context": context,
            "excerpt": excerpt,
            "ts": ts,
            "title": task["title"],
        })

    runtime.notify_organization_task_activity = notify
    runtime.notify_organization_task_mentions = notify_mentions

    created = await create_task(runtime, "bob", "Fix the login bug")
    task_id = created["data"]["task"]["id"]
    activity.clear()

    commented = await tasks_api.handle(
        runtime.use("POST", "carol", {
            "body": "@mary can you look? cc @carol and support@forkmesh.com",
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/replies",
    )
    assert commented["status"] == 201
    assert activity == [("carol", "commented")]
    # The author is dropped from their own mention list and an email address
    # is never mistaken for a handle.
    assert mentions[-1]["names"] == ["mary"]
    assert mentions[-1]["context"] == "comment"
    assert mentions[-1]["title"] == "Fix the login bug"
    assert mentions[-1]["ts"] == runtime.now_ms

    activity.clear()
    await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/start",
    )
    runtime.now_ms += 1000
    checked_in = await tasks_api.handle(
        runtime.use("POST", "bob", {
            "state": "needs_help",
            "note": "stuck on the redirect, @wendy any ideas?",
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/checkin",
    )
    assert checked_in["status"] == 200
    assert activity == [
        ("bob", "started"),
        ("bob", "checked in needs help"),
    ]
    assert mentions[-1]["names"] == ["wendy"]
    assert mentions[-1]["context"] == "check-in"


@run_async_test
async def test_any_member_can_remind_the_assignee_to_check_in():
    runtime = FakeRuntime()
    reminders = []

    async def notify_reminder(
            org_bi, actor, task_id, task, recipient, note, ts):
        reminders.append({
            "actor": actor,
            "task_id": task_id,
            "recipient": recipient,
            "note": note,
            "ts": ts,
            "title": task["title"],
        })
        return {"pingId": "a" * 64, "phones": 1}

    runtime.notify_organization_task_reminder = notify_reminder
    created = await create_task(runtime, "bob", "Fix the login bug")
    task_id = created["data"]["task"]["id"]
    assert created["data"]["task"]["lastReminder"] is None
    path = f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/remind"
    before = await tasks_api._task(runtime, "org-bi", task_id)

    reminded = await tasks_api.handle(
        runtime.use("POST", "carol", {"note": "Any news before standup?"}),
        path,
    )
    assert reminded["status"] == 200
    assert reminded["data"]["task"]["lastReminder"] == {
        "at": runtime.now_ms,
        "by": "carol",
        "to": "bob",
        "note": "Any news before standup?",
        "delivered": True,
        "phones": 1,
        "seenAt": 0,
        "seenVia": "",
    }
    assert "reminderError" not in reminded["data"]
    sent = reminded["data"]["task"]["history"][-1]
    assert sent["kind"] == "reminded"
    assert sent["actor"] == "carol"
    assert sent["mentions"] == ["bob"]
    assert sent["summary"] == (
        "Reminded @bob — ping sent, pushed to 1 phone: "
        "“Any news before standup?”")
    assert reminders == [{
        "actor": "carol",
        "task_id": task_id,
        "recipient": "bob",
        "note": "Any news before standup?",
        "ts": runtime.now_ms,
        "title": "Fix the login bug",
    }]
    after = await tasks_api._task(runtime, "org-bi", task_id)
    assert after["updated_at"] == before["updated_at"]
    assert "standup" not in str(after["data"])
    assert runtime.audits[-1]["action"] == "organization.task_reminder_sent"
    assert runtime.audits[-1]["details"] == {"noteLength": 24}

    runtime.now_ms += tasks_api.REMINDER_COOLDOWN_MS - 1
    too_soon = await tasks_api.handle(
        runtime.use("POST", "mary", {}), path)
    assert too_soon["status"] == 429
    assert too_soon["data"]["error"] == "reminder_too_soon"
    assert too_soon["data"]["retryAt"] == (
        runtime.now_ms + 1)
    assert len(reminders) == 1

    runtime.now_ms += 1
    again = await tasks_api.handle(runtime.use("POST", "mary", {}), path)
    assert again["status"] == 200
    assert again["data"]["task"]["lastReminder"]["by"] == "mary"
    assert again["data"]["task"]["lastReminder"]["note"] == ""
    assert reminders[-1]["recipient"] == "bob"

    runtime.now_ms += tasks_api.REMINDER_COOLDOWN_MS
    self_reminder = await tasks_api.handle(
        runtime.use("POST", "bob", {}), path)
    assert self_reminder["status"] == 409
    assert self_reminder["data"]["error"] == "cannot_remind_yourself"

    outsider = await tasks_api.handle(runtime.use("POST", "eve", {}), path)
    assert outsider["status"] == 403
    assert len(reminders) == 2

    agent = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Let the bot do it",
            "department": "engineering",
            "assigneeKind": "agent",
            "repository": "forkmesh/forkmesh",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    agent_id = agent["data"]["task"]["id"]
    refused = await tasks_api.handle(
        runtime.use("POST", "carol", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{agent_id}/remind",
    )
    assert refused["status"] == 409
    assert refused["data"]["error"] == "task_has_no_assignee"

    await tasks_api.handle(
        runtime.use("POST", "bob", {"completionNote": "Shipped"}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/complete",
    )
    finished = await tasks_api.handle(
        runtime.use("POST", "carol", {}), path)
    assert finished["status"] == 409
    assert finished["data"]["error"] == "task_done"
    assert len(reminders) == 2


@run_async_test
async def test_a_reminder_records_when_the_assignee_saw_it():
    runtime = FakeRuntime()
    read = {}

    async def notify_reminder(
            org_bi, actor, task_id, task, recipient, note, ts):
        return {"pingId": "b" * 64, "phones": 0}

    async def ping_read_at(recipient, ping_id):
        return read.get((recipient, ping_id), 0)

    runtime.notify_organization_task_reminder = notify_reminder
    runtime.reminder_ping_read_at = ping_read_at
    created = await create_task(runtime, "bob", "Fix the login bug")
    task_id = created["data"]["task"]["id"]
    task_path = f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}"
    await tasks_api.handle(
        runtime.use("POST", "carol", {}), task_path + "/remind")

    unseen = await tasks_api.handle(runtime.use("GET", "carol"), task_path)
    assert unseen["data"]["task"]["lastReminder"]["seenAt"] == 0
    assert unseen["data"]["task"]["history"][-1]["summary"] == (
        "Reminded @bob — ping sent")

    read[("bob", "b" * 64)] = runtime.now_ms + 5
    runtime.now_ms += 60_000
    seen = await tasks_api.handle(runtime.use("GET", "carol"), task_path)
    reminder = seen["data"]["task"]["lastReminder"]
    assert reminder["seenAt"] == read[("bob", "b" * 64)]
    assert reminder["seenVia"] == "ping"
    event = seen["data"]["task"]["history"][-1]
    assert event["kind"] == "reminder_seen"
    assert event["actor"] == "bob"
    assert event["summary"] == "@bob opened the reminder ping"

    again = await tasks_api.handle(runtime.use("GET", "bob"), task_path)
    assert again["data"]["task"]["lastReminder"]["seenVia"] == "ping"
    assert [
        entry["kind"] for entry in again["data"]["task"]["history"]
    ].count("reminder_seen") == 1

    runtime.now_ms += tasks_api.REMINDER_COOLDOWN_MS
    await tasks_api.handle(
        runtime.use("POST", "mary", {}), task_path + "/remind")
    history = await tasks_api.handle(
        runtime.use("GET", "bob"), task_path + "/history")
    assert history["data"]["history"][-1]["summary"] == (
        "@bob opened the task after the reminder")
    final = await tasks_api.handle(runtime.use("GET", "mary"), task_path)
    assert final["data"]["task"]["lastReminder"]["by"] == "mary"
    assert final["data"]["task"]["lastReminder"]["seenVia"] == "task"
    assert final["data"]["task"]["lastReminder"]["seenAt"] == runtime.now_ms


@run_async_test
async def test_a_reminder_that_was_not_filed_says_so():
    runtime = FakeRuntime()

    async def notify_reminder(
            org_bi, actor, task_id, task, recipient, note, ts):
        return False

    runtime.notify_organization_task_reminder = notify_reminder
    created = await create_task(runtime, "bob", "Fix the login bug")
    task_id = created["data"]["task"]["id"]
    reminded = await tasks_api.handle(
        runtime.use("POST", "carol", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/remind")
    assert reminded["status"] == 200
    assert reminded["data"]["reminderError"] == "ping_not_filed"
    assert reminded["data"]["task"]["lastReminder"]["delivered"] is False
    assert reminded["data"]["task"]["history"][-1]["summary"] == (
        "Reminder to @bob could not be delivered")


@run_async_test
async def test_a_mention_in_task_copy_pings_when_the_task_is_opened_or_edited():
    runtime = FakeRuntime()
    mentions = []

    async def notify_mentions(
            org_bi, actor, task_id, task, names, context, excerpt, ts):
        mentions.append((actor, list(names), context))

    runtime.notify_organization_task_mentions = notify_mentions

    created = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Ship the digest",
            "details": "@mary owns the copy, @alice reviews.",
            "assignee": "bob",
        }),
        tasks_api.PREFIX,
    )
    task_id = created["data"]["task"]["id"]
    # The author is never pinged about their own words.
    assert mentions == [("alice", ["mary"], "task")]

    mentions.clear()
    edited = await tasks_api.handle(
        runtime.use("PATCH", "alice", {
            "details": "Reassigning: @wendy has context.",
        }),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert edited["status"] == 200
    assert mentions == [("alice", ["wendy"], "task")]
    # An edit is on the timeline but is deliberately not an activity ping.
    history = await tasks_api.handle(
        runtime.use("GET", "alice"),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/history",
    )
    assert [entry["kind"] for entry in history["data"]["history"]] == [
        "created", "updated",
    ]


@run_async_test
async def test_progress_is_editor_owned_clamped_audited_and_full_when_done():
    runtime = FakeRuntime()
    created = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Ship progress bars",
            "assignee": "wendy",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert created["status"] == 201
    task = created["data"]["task"]
    assert task["progress"] == 0
    path = f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}"

    denied = await tasks_api.handle(
        runtime.use("PATCH", "bob", {"progress": 40}), path)
    assert denied["status"] == 403
    assert denied["data"]["error"] == "forbidden"
    unchanged = await tasks_api.handle(runtime.use("GET", "alice"), path)
    assert unchanged["data"]["task"]["progress"] == 0

    for editor, value in (("wendy", 25), ("mary", 40), ("alice", 55)):
        edited = await tasks_api.handle(
            runtime.use("PATCH", editor, {"progress": value}), path)
        assert edited["status"] == 200, editor
        assert edited["data"]["task"]["progress"] == value
    audit = runtime.audits[-1]
    assert audit["action"] == "organization.task_progress_changed"
    assert audit["actor"] == "alice"
    assert audit["target"] == task["id"]
    assert audit["details"] == {"progress": 55, "previous": 40}
    history = await tasks_api.handle(runtime.use("GET", "alice"), path)
    assert any(
        entry.get("kind") == "updated"
        and "progress 55%" in str(entry.get("summary"))
        for entry in history["data"]["task"]["history"]
    )

    high = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"progress": 500}), path)
    assert high["data"]["task"]["progress"] == 100
    low = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"progress": -7}), path)
    assert low["data"]["task"]["progress"] == 0
    rounded = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"progress": "33.6"}), path)
    assert rounded["data"]["task"]["progress"] == 34
    for bad in ("lots", None, True, float("nan")):
        refused = await tasks_api.handle(
            runtime.use("PATCH", "alice", {"progress": bad}), path)
        assert refused["status"] == 400, bad
        assert refused["data"]["error"] == "invalid_progress"
    mixed = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"progress": 10, "title": "x"}), path)
    assert mixed["status"] == 403

    listing = await tasks_api.handle(
        runtime.use("GET", "carol"), tasks_api.UNIVERSAL_PREFIX)
    listed = next(
        item for item in listing["data"]["tasks"] if item["id"] == task["id"])
    assert listed["progress"] == 34

    completed = await tasks_api.handle(
        runtime.use("POST", "wendy", {}), path + "/complete")
    assert completed["status"] == 200
    assert completed["data"]["task"]["progress"] == 100
    reopened = await tasks_api.handle(
        runtime.use("POST", "alice", {}), path + "/undo")
    assert reopened["status"] == 200
    assert reopened["data"]["task"]["progress"] == 34


@run_async_test
async def test_manager_can_move_parent_routing_and_how_to_test():
    runtime = FakeRuntime()
    prefix = tasks_api.UNIVERSAL_PREFIX
    parent = await tasks_api.handle(
        runtime.use("POST", "alice", {"title": "Parent", "assignee": "bob"}),
        prefix)
    child = await tasks_api.handle(
        runtime.use("POST", "alice", {"title": "Child", "assignee": "bob"}),
        prefix)
    parent_id = parent["data"]["task"]["id"]
    child_id = child["data"]["task"]["id"]
    moved = await tasks_api.handle(
        runtime.use("PATCH", "alice", {
            "parentTaskId": parent_id,
            "department": "infrastructure",
            "team": "infrastructure",
            "howToTest": "Open the Hosts page",
        }),
        f"{prefix}/{child_id}")
    assert moved["status"] == 200, moved
    task = moved["data"]["task"]
    assert task["parentTaskId"] == parent_id
    assert task["department"] == "infrastructure"
    assert task["team"] == "infrastructure"
    assert task["qa"]["howToTest"] == "Open the Hosts page"
    cycle = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"parentTaskId": child_id}),
        f"{prefix}/{parent_id}")
    assert cycle["status"] == 400
    assert cycle["data"]["error"] == "invalid_parent_task"
    itself = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"parentTaskId": child_id}),
        f"{prefix}/{child_id}")
    assert itself["data"]["error"] == "invalid_parent_task"
    bad_department = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"department": "nowhere"}),
        f"{prefix}/{child_id}")
    assert bad_department["data"]["error"] == "invalid_task_routing"
    cleared = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"parentTaskId": ""}),
        f"{prefix}/{child_id}")
    assert cleared["data"]["task"]["parentTaskId"] == ""
    denied = await tasks_api.handle(
        runtime.use("PATCH", "bob", {"department": "security"}),
        f"{prefix}/{child_id}")
    assert denied["status"] == 403


@run_async_test
async def test_work_items_are_validated_deduplicated_sealed_and_kept():
    runtime = FakeRuntime()
    prefix = tasks_api.UNIVERSAL_PREFIX
    item = {
        "repository": "forkmesh/src",
        "branch": "feat/task-work-items",
        "worktree": "/home/wendy/src/.worktrees/012-work-items",
        "node": "Wendy-Desk",
    }
    created = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Attach worktrees",
            "assignee": "wendy",
            "workItems": [item, dict(item, worktree="/elsewhere")],
        }),
        prefix)
    assert created["status"] == 201, created
    task = created["data"]["task"]
    assert task["workItems"] == [dict(
        item, node="wendy-desk", worktree="/elsewhere")]
    path = f"{prefix}/{task['id']}"
    raw = runtime.db.execute(
        "SELECT data FROM organization_tasks WHERE task_id=?",
        (task["id"],)).fetchone()[0]
    assert "task-work-items" not in raw

    for bad in (
        {"branch": "../main"},
        {"branch": "-rf"},
        {"branch": "feature.lock"},
        {"branch": "has space"},
        {"branch": "tab\tname"},
        {"branch": "x" * 201},
        {"branch": ""},
        {"branch": "ok", "repository": "nope"},
        {"branch": "ok", "worktree": "relative/path"},
        {"branch": "ok", "worktree": "/a/../b"},
        {"branch": "ok", "worktree": "/" + "a" * 400},
        {"branch": "ok", "node": "Bad Node"},
    ):
        refused = await tasks_api.handle(
            runtime.use("PATCH", "wendy", {"workItems": [bad]}), path)
        assert refused["status"] == 400, bad
        assert refused["data"]["error"] == "invalid_work_item", bad
    too_many = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"workItems": [
            {"branch": "b%d" % index} for index in range(21)
        ]}),
        path)
    assert too_many["data"]["error"] == "too_many_work_items"

    denied = await tasks_api.handle(
        runtime.use("PATCH", "bob", {"workItems": []}), path)
    assert denied["status"] == 403
    second = {"branch": "fix/other", "worktree": "", "node": "", "repository": ""}
    edited = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"workItems": [
            task["workItems"][0], second,
        ]}),
        path)
    assert edited["status"] == 200, edited
    assert edited["data"]["task"]["workItems"][1] == second
    assert runtime.audits[-1]["action"] == (
        "organization.task_work_items_changed")
    assert runtime.audits[-1]["details"] == {"workItems": 2, "previous": 1}
    assert any(
        entry["kind"] == "updated"
        and "attached fix/other" in entry["summary"]
        for entry in edited["data"]["task"]["history"]
    )

    renamed = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"title": "Attach all worktrees"}),
        path)
    assert renamed["data"]["task"]["workItems"] == (
        edited["data"]["task"]["workItems"])
    listing = await tasks_api.handle(runtime.use("GET", "carol"), prefix)
    listed = next(
        entry for entry in listing["data"]["tasks"]
        if entry["id"] == task["id"])
    assert len(listed["workItems"]) == 2

    started = await tasks_api.handle(
        runtime.use("POST", "wendy", {}), path + "/start")
    assert started["status"] == 200
    while_active = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"workItems": [second]}), path)
    assert while_active["status"] == 200
    assert while_active["data"]["task"]["workItems"] == [second]
    completed = await tasks_api.handle(
        runtime.use("POST", "wendy", {}), path + "/complete")
    assert completed["data"]["task"]["workItems"] == [second]
    reopened = await tasks_api.handle(
        runtime.use("POST", "alice", {}), path + "/undo")
    assert reopened["data"]["task"]["workItems"] == [second]

    deleted = await tasks_api.handle(runtime.use("DELETE", "alice"), path)
    assert deleted["status"] == 200
    assert deleted["data"]["workItems"] == [second]
    assert runtime.audits[-1]["details"]["workItems"] == 1


@run_async_test
async def test_work_item_attach_and_detach_merge_on_the_server():
    runtime = FakeRuntime()
    prefix = tasks_api.UNIVERSAL_PREFIX
    first = {
        "repository": "forkmesh/src", "branch": "feat/one",
        "worktree": "/w/one", "node": "desk",
    }
    created = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Attach worktrees", "assignee": "wendy",
            "workItems": [first],
        }),
        prefix)
    path = f"{prefix}/{created['data']['task']['id']}"
    second = dict(first, branch="feat/two", worktree="/w/two")
    attached = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"attachWorkItems": [second]}), path)
    assert attached["status"] == 200, attached
    assert attached["data"]["task"]["workItems"] == [first, second]
    assert runtime.audits[-1]["details"] == {"workItems": 2, "previous": 1}

    moved = dict(first, worktree="/w/one-moved")
    updated = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"attachWorkItems": [moved]}), path)
    assert updated["data"]["task"]["workItems"] == [moved, second]
    audits = len(runtime.audits)
    again = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"attachWorkItems": [moved]}), path)
    assert again["status"] == 200
    assert len(runtime.audits) == audits

    detached = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {
            "detachWorkItems": [{
                "repository": "FORKMESH/src", "branch": "feat/one",
                "node": "desk",
            }],
        }),
        path)
    assert detached["data"]["task"]["workItems"] == [second]
    assert any(
        "detached feat/one" in entry["summary"]
        for entry in detached["data"]["task"]["history"])

    for body, error in (
        ({"attachWorkItems": [{"branch": "../x"}]}, "invalid_work_item"),
        ({"detachWorkItems": "feat/two"}, "invalid_work_items"),
        ({"attachWorkItems": [
            {"branch": "b%d" % index} for index in range(20)]},
         "too_many_work_items"),
    ):
        refused = await tasks_api.handle(
            runtime.use("PATCH", "wendy", body), path)
        assert refused["status"] == 400, body
        assert refused["data"]["error"] == error, body
    mixed = await tasks_api.handle(
        runtime.use("PATCH", "alice", {
            "attachWorkItems": [first], "title": "x"}),
        path)
    assert mixed["data"]["error"] == "invalid_update"
    denied = await tasks_api.handle(
        runtime.use("PATCH", "bob", {"attachWorkItems": [first]}), path)
    assert denied["status"] == 403


@run_async_test
async def test_concurrent_work_item_attaches_keep_both_items():
    runtime = FakeRuntime()
    prefix = tasks_api.UNIVERSAL_PREFIX
    created = await tasks_api.handle(
        runtime.use("POST", "mary", {
            "title": "Attach worktrees", "assignee": "wendy"}),
        prefix)
    task_id = created["data"]["task"]["id"]
    path = f"{prefix}/{task_id}"
    other = {
        "repository": "", "branch": "feat/other-node",
        "worktree": "", "node": "laptop",
    }
    mine = {"repository": "", "branch": "feat/mine", "worktree": "",
            "node": "desk"}
    opened = runtime.open
    raced = []

    async def open_then_race(value):
        task = await opened(value)
        if not raced and isinstance(task, dict) and "workItems" in task:
            raced.append(True)
            winner = dict(task, workItems=[other])
            runtime.db.execute(
                "UPDATE organization_tasks SET data=? WHERE task_id=?",
                (await runtime.seal(winner), task_id))
            runtime.db.commit()
        return task

    runtime.open = open_then_race
    attached = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"attachWorkItems": [mine]}), path)
    runtime.open = opened
    assert raced
    assert attached["status"] == 200, attached
    assert attached["data"]["task"]["workItems"] == [other, mine]

    async def always_race(sql, *args):
        if "AND data=?" in sql:
            return {"meta": {"changes": 0}}
        return await FakeRuntime.d1_run(runtime, sql, *args)

    runtime.d1_run = always_race
    busy = await tasks_api.handle(
        runtime.use("PATCH", "wendy", {"detachWorkItems": [mine]}), path)
    assert busy["status"] == 409
    assert busy["data"]["error"] == "task_busy"


MARKDOWN_DETAILS = (
    "## Why\r\n"
    "The cores run **hot** on `/api/tasks`.\r\n"
    "\r\n"
    "| Route | Queries |\n"
    "|---|---:|\n"
    "| /api/tasks | 12 |\n"
    "\n"
    "1. Measure\n"
    "2. Cut\n"
    "\n"
    "> Keep `a < b` and `c > d` intact.\n"
    "\n"
    "```\n"
    "if (a < b) {\n"
    "\treturn;\n"
    "}\n"
    "```"
)
MARKDOWN_DETAILS_STORED = (
    "## Why\n"
    "The cores run **hot** on `/api/tasks`.\n"
    "\n"
    "| Route | Queries |\n"
    "|---|---:|\n"
    "| /api/tasks | 12 |\n"
    "\n"
    "1. Measure\n"
    "2. Cut\n"
    "\n"
    "> Keep `a < b` and `c > d` intact.\n"
    "\n"
    "```\n"
    "if (a < b) {\n"
    "    return;\n"
    "}\n"
    "```"
)


def test_multiline_text_keeps_line_breaks_and_strips_other_controls():
    assert tasks_api._multiline("a\r\nb\rc\nd", 100) == "a\nb\nc\nd"
    assert tasks_api._multiline(
        "x\x00y\x07z\x1b[31m w", 100) == "x y z [31m w"
    assert tasks_api._multiline("\n\n  keep indent  \n\n", 100) == (
        "  keep indent")
    assert tasks_api._multiline("a\n\n\n\n\n\nb", 100) == "a\n\n\nb"
    assert tasks_api._multiline(" \n\t\n", 100, "fallback") == "fallback"
    assert tasks_api._multiline("abcdef", 3) == "abc"
    assert tasks_api._multiline("<b>&</b>", 100) == "<b>&</b>"
    assert tasks_api._text("## Title\nsecond", 100) == "## Title second"


@run_async_test
async def test_markdown_details_notes_and_replies_keep_their_line_breaks():
    runtime = FakeRuntime()
    created = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Line one\nline two",
            "details": MARKDOWN_DETAILS,
            "assignee": "bob",
            "howToTest": "1. Open Tasks\n2. See the table",
        }),
        tasks_api.UNIVERSAL_PREFIX,
    )
    assert created["status"] == 201, created
    task = created["data"]["task"]
    assert task["title"] == "Line one line two"
    assert task["details"] == MARKDOWN_DETAILS_STORED
    assert task["qa"]["howToTest"] == "1. Open Tasks\n2. See the table"
    path = f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}"

    listing = await tasks_api.handle(
        runtime.use("GET", "carol"), tasks_api.UNIVERSAL_PREFIX)
    listed = next(
        entry for entry in listing["data"]["tasks"]
        if entry["id"] == task["id"])
    assert listed["details"] == MARKDOWN_DETAILS_STORED

    edited = await tasks_api.handle(
        runtime.use("PATCH", "alice", {
            "details": "## Re-saved\n\n- one\n- two",
            "howToTest": "Step A\nStep B",
        }),
        path,
    )
    assert edited["status"] == 200, edited
    assert edited["data"]["task"]["details"] == "## Re-saved\n\n- one\n- two"
    assert edited["data"]["task"]["qa"]["howToTest"] == "Step A\nStep B"

    unchanged = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"priority": 7}), path)
    assert unchanged["data"]["task"]["details"] == (
        "## Re-saved\n\n- one\n- two")

    reply = await tasks_api.handle(
        runtime.use("POST", "carol", {"body": "**Looks good**\n\n- a\n- b"}),
        path + "/replies",
    )
    assert reply["status"] == 201, reply
    assert reply["data"]["task"]["replies"][-1]["body"] == (
        "**Looks good**\n\n- a\n- b")

    note = "Done:\n\n| Check | Result |\n|---|---|\n| QA | ok |"
    completed = await tasks_api.handle(
        runtime.use("POST", "bob", {"completionNote": note}),
        path + "/complete",
    )
    assert completed["status"] == 200, completed
    done = completed["data"]["task"]
    assert done["completionNote"] == note
    assert done["completionHistory"][-1]["note"] == note


@run_async_test
async def test_agent_runs_without_a_task_are_published_as_presence():
    runtime = FakeRuntime()
    runtime.db.executescript(
        (ROOT / "migrations" / "0233_organization_agent_presence.sql")
        .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
    runtime.db.execute(
        "CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT, "
        "name TEXT, created_at INTEGER)")
    for name in ("alice", "wendy"):
        runtime.db.execute(
            "INSERT INTO org_members VALUES ('org-bi',?,?,?,0)",
            (runtime.users[name]["bi"], "member", name))
    runtime.db.commit()
    path = f"{tasks_api.UNIVERSAL_PREFIX}/agent-status"

    batch = await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": "", "status": "running", "agent": {
                "provider": "claude-code", "model": "opus",
                "sessionId": "168", "startedAt": NOW - 60000}},
            {"task": "", "status": "success", "agent": {"sessionId": "167"}},
            {"task": "", "status": "sleeping", "agent": {"sessionId": "166"}},
            {"task": "", "status": "running", "agent": {"sessionId": "bad!id"}},
            {"task": "", "status": "running"},
        ]}),
        path,
    )
    assert batch["status"] == 200
    assert batch["data"]["results"] == [
        {"task": "", "session": "168", "ok": True, "error": ""},
        {"task": "", "session": "167", "ok": True, "error": ""},
        {"task": "", "session": "166", "ok": False,
         "error": "invalid_agent_status"},
    ]
    stored = runtime.db.execute(
        "SELECT data FROM organization_agent_presence").fetchall()
    assert stored and all("opus" not in row["data"] for row in stored)

    listed = await tasks_api.handle(
        runtime.use("GET", "alice", {}), tasks_api.UNIVERSAL_PREFIX)
    runs = listed["data"]["agentRuns"]
    assert [(r["sessionId"], r["owner"], r["status"]) for r in runs] == [
        ("168", "wendy", "running"), ("167", "wendy", "success")]
    assert runs[0]["model"] == "opus" and runs[0]["startedAt"] == NOW - 60000
    assert runs[0]["title"] == "" and runs[0]["prose"] == ""
    assert "repository" not in runs[0]

    runtime.now_ms = NOW + tasks_api.PRESENCE_FINISHED_MS + 1
    await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": "", "status": "waiting", "agent": {"sessionId": "168"}},
        ]}),
        path,
    )
    listed = await tasks_api.handle(
        runtime.use("GET", "alice", {}), tasks_api.UNIVERSAL_PREFIX)
    assert [(r["sessionId"], r["status"]) for r in listed["data"]["agentRuns"]] \
        == [("168", "waiting")]
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_agent_presence").fetchone()[0] == 1

    runtime.db.execute("DELETE FROM org_members WHERE name='wendy'")
    runtime.db.commit()
    listed = await tasks_api.handle(
        runtime.use("GET", "alice", {}), tasks_api.UNIVERSAL_PREFIX)
    assert listed["data"]["agentRuns"] == []

    denied = await tasks_api.handle(
        runtime.use("POST", "eve", {"statuses": [
            {"task": "", "status": "running", "agent": {"sessionId": "1"}},
        ]}),
        path,
    )
    assert denied["status"] == 403

    opened = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Hand-opened agent work", "department": "engineering",
            "repository": "forkmesh/forkmesh", "assigneeKind": "agent"}),
        tasks_api.UNIVERSAL_PREFIX,
    )
    task_id = opened["data"]["task"]["id"]
    await tasks_api.handle(
        runtime.use("POST", "mary", {"statuses": [
            {"task": task_id, "status": "running",
             "agent": {"sessionId": "12"}},
        ]}),
        path,
    )
    current = await tasks_api.handle(
        runtime.use("GET", "alice", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert current["data"]["task"]["agent"]["runBy"] == "mary"


@run_async_test
async def test_run_by_is_set_by_the_server_only():
    runtime = FakeRuntime()
    opened = await tasks_api.handle(
        runtime.use("POST", "wendy", {
            "title": "Spoofed run", "department": "engineering",
            "repository": "forkmesh/forkmesh", "assigneeKind": "agent",
            "agent": {"provider": "codex", "sessionId": "4",
                      "status": "queued", "runBy": "alice"}}),
        tasks_api.UNIVERSAL_PREFIX,
    )
    task = opened["data"]["task"]
    assert "runBy" not in task["agent"]
    await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": task["id"], "status": "running",
             "agent": {"runBy": "alice"}}]}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    done = await tasks_api.handle(
        runtime.use("POST", "wendy", {"completionNote": "done",
                                      "agent": {"runBy": "alice"}}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}/complete",
    )
    current = await tasks_api.handle(
        runtime.use("GET", "wendy", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task['id']}",
    )
    assert done["status"] in (200, 201)
    assert current["data"]["task"]["agent"]["runBy"] == "wendy"


@run_async_test
async def test_agent_prose_and_titles_are_bounded_and_redacted():
    runtime = FakeRuntime()
    runtime.db.executescript(
        (ROOT / "migrations" / "0233_organization_agent_presence.sql")
        .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
    runtime.db.execute(
        "CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT, "
        "name TEXT, created_at INTEGER)")
    runtime.db.execute(
        "INSERT INTO org_members VALUES ('org-bi',?,?,?,0)",
        (runtime.users["wendy"]["bi"], "member", "wendy"))
    runtime.db.commit()
    secret = "sk-ant-api03-" + "A" * 30
    await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": "", "status": "running", "agent": {
                "sessionId": "7", "title": "Fix <b>login</b> " + "x" * 300,
                "prose": "Using key " + secret + " and password=hunter2 now. "
                         + "y" * 400}},
        ]}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    stored = runtime.db.execute(
        "SELECT data FROM organization_agent_presence").fetchone()["data"]
    assert secret not in stored and "hunter2" not in stored
    listed = await tasks_api.handle(
        runtime.use("GET", "wendy", {}), tasks_api.UNIVERSAL_PREFIX)
    run = listed["data"]["agentRuns"][0]
    assert len(run["title"]) <= tasks_api.MAX_AGENT_TITLE
    assert "<" not in run["title"]
    assert len(run["prose"]) <= tasks_api.MAX_AGENT_PROSE
    assert secret not in run["prose"] and "hunter2" not in run["prose"]
    assert run["prose"].startswith("Using key [redacted]")

    opened = await tasks_api.handle(
        runtime.use("POST", "wendy", {
            "title": "Agent work", "department": "engineering",
            "repository": "forkmesh/forkmesh", "assigneeKind": "agent"}),
        tasks_api.UNIVERSAL_PREFIX,
    )
    task_id = opened["data"]["task"]["id"]
    await tasks_api.handle(
        runtime.use("POST", "wendy", {"statuses": [
            {"task": task_id, "status": "running",
             "agent": {"sessionId": "8", "prose": "token: " + secret}}]}),
        f"{tasks_api.UNIVERSAL_PREFIX}/agent-status",
    )
    current = await tasks_api.handle(
        runtime.use("GET", "wendy", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}",
    )
    assert current["data"]["task"]["agent"]["prose"] == "[redacted]"


@run_async_test
async def test_persistent_reminder_repeats_until_the_task_is_done():
    runtime = FakeRuntime()
    pings = []

    async def notify(org_bi, task_id, task, recipient, every_minutes, ts):
        pings.append((task_id, recipient, every_minutes, ts))
        return True

    runtime.notify_organization_task_persistent_reminder = notify
    created = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Back up the keys",
            "assignee": "bob",
            "persistentReminder": {"enabled": True, "everyMinutes": 30},
        }),
        tasks_api.PREFIX,
    )
    assert created["status"] == 201 or created["status"] == 200
    task = created["data"]["task"]
    assert task["persistentReminder"] == {
        "enabled": True, "everyMinutes": 30,
        "nextAt": runtime.now_ms + 30 * 60000,
    }
    task_id = task["id"]

    early = await tasks_api.deliver_persistent_reminders(
        runtime, runtime.now_ms + 30 * 60000 - 1)
    assert early == {"due": 0, "sent": 0}
    first_at = runtime.now_ms + 30 * 60000
    first = await tasks_api.deliver_persistent_reminders(runtime, first_at)
    assert first == {"due": 1, "sent": 1}
    assert pings == [(task_id, "bob", 30, first_at)]
    again = await tasks_api.deliver_persistent_reminders(runtime, first_at)
    assert again == {"due": 0, "sent": 0}
    second_at = first_at + 30 * 60000
    await tasks_api.deliver_persistent_reminders(runtime, second_at)
    assert len(pings) == 2

    await tasks_api.handle(
        runtime.use("POST", "bob", {}),
        f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}/complete")
    done = await tasks_api.deliver_persistent_reminders(
        runtime, second_at + 10 * 30 * 60000)
    assert done == {"due": 0, "sent": 0}
    assert len(pings) == 2


@run_async_test
async def test_persistent_reminder_toggle_and_self_assigned_tasks():
    runtime = FakeRuntime()
    pings = []

    async def notify(org_bi, task_id, task, recipient, every_minutes, ts):
        pings.append(recipient)
        return True

    runtime.notify_organization_task_persistent_reminder = notify
    created = await tasks_api.handle(
        runtime.use("POST", "alice", {
            "title": "Back up the keys", "department": "engineering",
            "assigneeKind": "unassigned"}),
        tasks_api.UNIVERSAL_PREFIX,
    )
    task_id = created["data"]["task"]["id"]
    assert created["data"]["task"]["persistentReminder"]["enabled"] is False
    path = f"{tasks_api.UNIVERSAL_PREFIX}/{task_id}"

    outsider = await tasks_api.handle(
        runtime.use("PATCH", "eve", {"persistentReminder": True}), path)
    assert outsider["status"] == 403
    bad = await tasks_api.handle(
        runtime.use("PATCH", "alice", {
            "persistentReminder": {"everyMinutes": 1}}), path)
    assert bad["status"] == 400
    assert bad["data"]["error"] == "invalid_persistent_reminder_interval"

    on = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"persistentReminder": True}), path)
    assert on["status"] == 200
    reminder = on["data"]["task"]["persistentReminder"]
    assert reminder["enabled"] is True
    assert reminder["everyMinutes"] == (
        tasks_api.PERSISTENT_REMINDER_DEFAULT_MINUTES)
    assert runtime.audits[-1]["action"] == (
        "organization.task_persistent_reminder_changed")
    await tasks_api.deliver_persistent_reminders(
        runtime, reminder["nextAt"])
    assert pings == ["alice"]

    off = await tasks_api.handle(
        runtime.use("PATCH", "alice", {"persistentReminder": False}), path)
    assert off["data"]["task"]["persistentReminder"] == {
        "enabled": False, "everyMinutes": 0, "nextAt": 0}
    await tasks_api.deliver_persistent_reminders(
        runtime, reminder["nextAt"] * 2)
    assert pings == ["alice"]
