import ast
import asyncio
import importlib.util
import re
import sqlite3
import sys
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
REPO = APP.parent
WORLD_SRC = REPO / "world" / "src"
sys.path.insert(0, str(APP / "src"))
sys.path.insert(0, str(WORLD_SRC))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tasks_api = _load(
    "forkmesh_trash_world_office_tasks", WORLD_SRC / "world_office_tasks.py")
world_fixture = _load(
    "forkmesh_trash_world_fixture",
    REPO / "world" / "tests" / "test_world_office_tasks.py")

DAY_MS = 24 * 60 * 60 * 1000
TASKS = tasks_api.UNIVERSAL_PREFIX


class Runtime(world_fixture.FakeRuntime):
    def __init__(self):
        super().__init__()
        self.running_sessions = set()
        self.cancelled = []
        self.bot_creator = None
        self.context = {
            "via": "signed-key", "node": "laptop", "bot": "",
            "agent": "claude", "ip": "203.0.113.9",
            "userAgent": "ForkMeshMCP/0.2", "client": "other-client",
        }

    def request_context(self):
        return dict(self.context)

    async def agent_session_running(self, org_bi, session_id):
        return session_id in self.running_sessions

    async def cancel_agent_session(self, org_bi, session_id):
        self.cancelled.append(session_id)
        return True

    async def delegated_principal(self, org_bi):
        if self.bot_creator is None:
            return None
        if not self.bot_creator:
            return {"accountBi": "", "role": "", "permission": ""}
        role, permission = self.memberships.get(self.bot_creator, ("", ""))
        return {
            "accountBi": self.users[self.bot_creator]["bi"],
            "role": role,
            "permission": permission,
        }

    def call(self, method, actor, path, data=None, query=None):
        return asyncio.run(tasks_api.handle(
            self.use(method, actor, data, query=query), path))

    def create(self, actor="alice", title="Trash me", **fields):
        body = {"title": title, "details": "details", **fields}
        response = self.call("POST", actor, TASKS, body)
        assert response["status"] in (200, 201), response
        return response["data"]["task"]["id"]

    def listed(self, actor="alice", query=None):
        response = self.call("GET", actor, TASKS, query=query)
        assert response["status"] == 200, response
        return [task["id"] for task in response["data"]["tasks"]]

    def trash(self, actor="alice"):
        response = self.call("GET", actor, TASKS, query={"status": "deleted"})
        assert response["status"] == 200, response
        return response["data"]["tasks"]

    def count(self, table, task_id):
        return self.db.execute(
            "SELECT COUNT(*) FROM " + table + " WHERE task_id=?",
            (task_id,)).fetchone()[0]


def _qa_board_sql():
    source = (WORLD_SRC / "world_qa.py").read_text(encoding="utf-8")
    for node in ast.walk(ast.parse(source)):
        if (
            isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and node.value.startswith(
                "SELECT task_id,department,team,qa_status,qa_reviewed_at,data")
        ):
            return node.value
    raise AssertionError("World QA board query not found")


def _complete(runtime, task_id, actor="bob"):
    started = runtime.call("POST", actor, f"{TASKS}/{task_id}/start", {})
    assert started["status"] == 200, started
    done = runtime.call("POST", actor, f"{TASKS}/{task_id}/complete", {})
    assert done["status"] == 200, done


def test_delete_moves_a_task_to_trash_and_hides_it_from_every_read():
    runtime = Runtime()
    keep = runtime.create(title="Keep me", assignee="bob")
    task_id = runtime.create(title="Trash me", assignee="bob")
    replied = runtime.call(
        "POST", "bob", f"{TASKS}/{task_id}/replies", {"body": "on it"})
    assert replied["status"] in (200, 201)
    _complete(runtime, task_id)
    reviewed_at = runtime.now_ms
    runtime.db.execute(
        "UPDATE organization_tasks SET qa_reviewed_at=?,qa_status='passed' "
        "WHERE task_id=?", (reviewed_at, task_id))
    runtime.db.execute(
        "INSERT INTO organization_task_qa_reviews "
        "(task_id,org_bi,reviewer_bi,verdict,data,reviewed_at) "
        "VALUES (?,?,?,?,?,?)",
        (task_id, "org-bi", "bi-carol", "passed", "sealed", reviewed_at))
    runtime.db.commit()
    board_before = runtime.db.execute(_qa_board_sql(), ("org-bi", 50)).fetchall()
    assert task_id in {row["task_id"] for row in board_before}
    assert set(runtime.listed()) == {keep, task_id}

    deleted = runtime.call(
        "DELETE", "bob", f"{TASKS}/{task_id}", {"reason": "duplicate"})
    assert deleted["status"] == 200, deleted
    assert deleted["data"]["trashed"] is True
    assert deleted["data"]["restorable"] is True
    assert deleted["data"]["purgeAt"] == (
        runtime.now_ms + tasks_api.TRASH_RETENTION_MS)

    assert runtime.listed() == [keep]
    assert runtime.listed("bob") == [keep]
    assert runtime.listed(query={"department": "general"}) == [keep]
    listing = runtime.call("GET", "alice", TASKS)["data"]
    assert len(listing["tasks"]) == 1
    legacy = asyncio.run(tasks_api.handle(
        runtime.use("GET", "alice"), tasks_api.LEGACY_MARKETING_PREFIX))
    assert task_id not in {task["id"] for task in legacy["data"]["tasks"]}
    for method, suffix in (
            ("GET", ""), ("PATCH", ""), ("GET", "/history"),
            ("GET", "/replies"), ("POST", "/undo"), ("POST", "/start")):
        response = runtime.call(
            method, "alice", f"{TASKS}/{task_id}{suffix}",
            {"title": "x"} if method == "PATCH" else {})
        assert response["status"] == 404, (method, suffix, response)
    board = runtime.db.execute(_qa_board_sql(), ("org-bi", 50)).fetchall()
    assert task_id not in {row["task_id"] for row in board}

    for table in (
            "organization_tasks", "organization_task_responses",
            "organization_task_qa_reviews", "organization_task_events"):
        assert runtime.count(table, task_id) >= 1, table

    trash = runtime.trash()
    assert [task["id"] for task in trash] == [task_id]
    assert trash[0]["deletedAt"] == runtime.now_ms
    assert trash[0]["deletedBy"] == "bob"
    assert trash[0]["deleteReason"] == "duplicate"
    assert trash[0]["canRestore"] is True
    assert runtime.trash("carol")[0]["canRestore"] is False
    marketing_trash = asyncio.run(tasks_api.handle(
        runtime.use("GET", "alice", query={"status": "deleted"}),
        tasks_api.LEGACY_MARKETING_PREFIX))
    assert marketing_trash["status"] == 400


def test_restore_within_thirty_days_and_purge_after_with_an_injected_clock():
    runtime = Runtime()
    task_id = runtime.create(assignee="bob")
    runtime.call("POST", "bob", f"{TASKS}/{task_id}/replies", {"body": "hi"})
    assert runtime.call("DELETE", "alice", f"{TASKS}/{task_id}")["status"] == 200

    runtime.now_ms += 29 * DAY_MS
    early = asyncio.run(tasks_api.purge_trash(runtime, runtime.now_ms))
    assert early["purged"] == 0
    wrong = runtime.call("POST", "carol", f"{TASKS}/{task_id}/restore", {})
    assert wrong["status"] == 403
    restored = runtime.call("POST", "bob", f"{TASKS}/{task_id}/restore", {})
    assert restored["status"] == 200, restored
    assert restored["data"]["task"]["deletedAt"] == 0
    assert task_id in runtime.listed()
    assert runtime.audits[-1]["action"] == "organization.task_restore"
    again = runtime.call("POST", "bob", f"{TASKS}/{task_id}/restore", {})
    assert again["status"] == 409
    assert again["data"]["error"] == "task_not_deleted"
    missing = runtime.call("POST", "bob", f"{TASKS}/{'f' * 32}/restore", {})
    assert missing["status"] == 404

    assert runtime.call("DELETE", "alice", f"{TASKS}/{task_id}")["status"] == 200
    runtime.now_ms += tasks_api.TRASH_RETENTION_MS
    expired = runtime.call("POST", "bob", f"{TASKS}/{task_id}/restore", {})
    assert expired["status"] == 410
    purged = asyncio.run(tasks_api.purge_trash(runtime, runtime.now_ms))
    assert purged["purged"] == 1
    for table in (
            "organization_tasks", "organization_task_responses",
            "organization_task_events", "organization_task_checkins",
            "organization_task_attachments", "organization_task_qa_reviews"):
        assert runtime.count(table, task_id) == 0, table
    assert asyncio.run(
        tasks_api.purge_trash(runtime, runtime.now_ms))["purged"] == 0


def test_purge_is_bounded_per_run_and_skips_live_and_recent_trash():
    runtime = Runtime()
    live = runtime.create(title="live")
    old = [runtime.create(title="old %d" % index) for index in range(30)]
    batch = runtime.call("POST", "alice", f"{TASKS}/delete", {"ids": old[:15]})
    assert batch["status"] == 200, batch
    batch = runtime.call("POST", "alice", f"{TASKS}/delete", {"ids": old[15:]})
    assert batch["status"] == 200, batch
    runtime.now_ms += tasks_api.TRASH_RETENTION_MS - DAY_MS
    recent = runtime.create(title="recent")
    assert runtime.call("DELETE", "alice", f"{TASKS}/{recent}")["status"] == 200
    runtime.now_ms += DAY_MS
    first = asyncio.run(tasks_api.purge_trash(runtime, runtime.now_ms, 1000))
    assert first["purged"] == tasks_api.TRASH_PURGE_BATCH
    second = asyncio.run(tasks_api.purge_trash(runtime, runtime.now_ms))
    assert second["purged"] == 30 - tasks_api.TRASH_PURGE_BATCH
    third = asyncio.run(tasks_api.purge_trash(runtime, runtime.now_ms))
    assert third["purged"] == 0
    assert runtime.count("organization_tasks", live) == 1
    assert runtime.count("organization_tasks", recent) == 1
    assert [task["id"] for task in runtime.trash()] == [recent]


def test_permissions_member_owner_outsider_and_agent_token():
    runtime = Runtime()
    alice_task = runtime.create("alice", "owner work", assignee="carol")
    wendy_task = runtime.create("wendy", "my own work")
    bob_task = runtime.create("alice", "bob work", assignee="bob")

    assert runtime.call(
        "DELETE", "eve", f"{TASKS}/{alice_task}")["status"] == 403
    assert runtime.call(
        "DELETE", "eve", f"{TASKS}/{alice_task}")["data"]["error"] == (
            "org_member_required")
    assert runtime.call(
        "DELETE", "wendy", f"{TASKS}/{alice_task}")["status"] == 403
    assert runtime.call(
        "DELETE", "wendy", f"{TASKS}/{wendy_task}")["status"] == 200
    assert runtime.call(
        "POST", "carol", f"{TASKS}/{wendy_task}/restore", {})["status"] == 403
    assert runtime.call(
        "POST", "mary", f"{TASKS}/{wendy_task}/restore", {})["status"] == 200
    assert runtime.call(
        "DELETE", "mary", f"{TASKS}/{wendy_task}")["status"] == 200

    runtime.bot_creator = "bob"
    assert runtime.call(
        "DELETE", "alice", f"{TASKS}/{alice_task}")["status"] == 403
    assert runtime.call(
        "DELETE", "alice", f"{TASKS}/{bob_task}")["status"] == 200
    runtime.bot_creator = ""
    assert runtime.call(
        "POST", "alice", f"{TASKS}/{bob_task}/restore", {})["status"] == 403
    runtime.bot_creator = "alice"
    assert runtime.call(
        "POST", "alice", f"{TASKS}/{bob_task}/restore", {})["status"] == 200
    assert runtime.call(
        "DELETE", "alice", f"{TASKS}/{alice_task}")["status"] == 200


def test_running_agent_and_qa_guards_need_an_owner_force():
    runtime = Runtime()
    queued = runtime.create(title="queued run")
    runtime.db.execute(
        "UPDATE organization_tasks SET agent_session_id=? WHERE task_id=?",
        ("0123456789abcdef0123", queued))
    runtime.db.commit()
    runtime.running_sessions.add("0123456789abcdef0123")
    blocked = runtime.call("DELETE", "mary", f"{TASKS}/{queued}")
    assert blocked["status"] == 409
    assert blocked["data"]["error"] == "task_delete_blocked"
    assert blocked["data"]["blockedBy"] == ["agent_running"]
    assert blocked["data"]["forceRequires"] == "owner"
    not_owner = runtime.call("DELETE", "mary", f"{TASKS}/{queued}", {"force": True})
    assert not_owner["status"] == 403
    assert not_owner["data"]["error"] == "force_requires_owner"
    bad_force = runtime.call("DELETE", "alice", f"{TASKS}/{queued}", {"force": "yes"})
    assert bad_force["status"] == 400
    forced = runtime.call("DELETE", "alice", f"{TASKS}/{queued}", {"force": True})
    assert forced["status"] == 200, forced
    assert runtime.cancelled == ["0123456789abcdef0123"]
    audit = runtime.audits[-1]
    assert audit["action"] == "organization.task_delete"
    assert audit["details"]["force"] is True
    assert audit["details"]["overrode"] == ["agent_running"]

    desktop = runtime.create(title="desktop run")
    sealed = runtime.db.execute(
        "SELECT data FROM organization_tasks WHERE task_id=?",
        (desktop,)).fetchone()[0]
    data = asyncio.run(runtime.open(sealed))
    data["agent"] = {"provider": "claude", "status": "running"}
    runtime.db.execute(
        "UPDATE organization_tasks SET data=? WHERE task_id=?",
        (asyncio.run(runtime.seal(data)), desktop))
    runtime.db.commit()
    owner_blocked = runtime.call("DELETE", "alice", f"{TASKS}/{desktop}")
    assert owner_blocked["data"]["blockedBy"] == ["agent_running"]
    assert owner_blocked["data"]["canForce"] is True

    in_qa = runtime.create(title="in qa", assignee="bob")
    _complete(runtime, in_qa)
    qa_blocked = runtime.call("DELETE", "bob", f"{TASKS}/{in_qa}")
    assert qa_blocked["status"] == 409
    assert qa_blocked["data"]["blockedBy"] == ["qa_in_progress"]
    assert qa_blocked["data"]["canForce"] is False
    assert qa_blocked["data"]["request"] == {
        "method": "DELETE", "path": f"/api/tasks/{in_qa}", "user": "bob",
        "at": runtime.now_ms, "via": "signed-key", "node": "laptop",
        "bot": "", "agent": "claude", "ip": "203.0.113.9",
        "userAgent": "ForkMeshMCP/0.2", "client": "other-client",
    }
    denied = runtime.audits[-1]
    assert denied["action"] == "organization.task_delete"
    assert denied["outcome"] == "denied"
    assert denied["target"] == in_qa
    assert denied["details"]["error"] == "task_delete_blocked"
    assert denied["details"]["blockedBy"] == "qa_in_progress"
    assert denied["details"]["agent"] == "claude"
    assert "ip" not in denied["details"]
    runtime.db.execute(
        "UPDATE organization_tasks SET qa_reviewed_at=?,qa_status='failed' "
        "WHERE task_id=?", (runtime.now_ms, in_qa))
    runtime.db.commit()
    assert runtime.call("DELETE", "bob", f"{TASKS}/{in_qa}")["status"] == 200


def test_active_timer_stops_when_its_task_is_trashed():
    runtime = Runtime()
    task_id = runtime.create(assignee="bob")
    assert runtime.call(
        "POST", "bob", f"{TASKS}/{task_id}/start", {})["status"] == 200
    runtime.now_ms += 5000
    assert runtime.call("DELETE", "bob", f"{TASKS}/{task_id}")["status"] == 200
    row = runtime.db.execute(
        "SELECT status,active_assignee_bi,elapsed_ms FROM organization_tasks "
        "WHERE task_id=?", (task_id,)).fetchone()
    assert tuple(row) == ("idle", "", 5000)
    other = runtime.create(assignee="bob")
    assert runtime.call(
        "POST", "bob", f"{TASKS}/{other}/start", {})["status"] == 200


def test_batch_delete_is_all_or_nothing_and_bounded():
    runtime = Runtime()
    mine = [runtime.create("wendy", "mine %d" % index) for index in range(3)]
    theirs = runtime.create("alice", "not wendy's")

    refused = runtime.call(
        "POST", "wendy", f"{TASKS}/delete", {"ids": mine + [theirs]})
    assert refused["status"] == 403
    assert refused["data"]["error"] == "task_delete_batch_refused"
    assert refused["data"]["failures"] == [{"id": theirs, "error": "forbidden"}]
    assert refused["data"]["deleted"] == []
    assert refused["data"]["request"]["path"] == "/api/tasks/delete"
    assert refused["data"]["request"]["user"] == "wendy"
    assert "forced" not in refused["data"]
    assert runtime.audits[-1]["outcome"] == "denied"
    assert runtime.audits[-1]["details"]["batch"] is True
    assert set(runtime.listed()) == set(mine + [theirs])

    unknown = runtime.call(
        "POST", "wendy", f"{TASKS}/delete", {"ids": mine + ["e" * 32]})
    assert unknown["status"] == 404
    assert set(runtime.listed()) == set(mine + [theirs])

    too_many = runtime.call(
        "POST", "alice", f"{TASKS}/delete",
        {"ids": ["%032x" % index for index in range(1, 52)]})
    assert too_many["status"] == 400
    assert too_many["data"]["max"] == tasks_api.MAX_DELETE_BATCH
    for bad in ({"ids": []}, {"ids": "x"}, {"ids": ["nothex"]}, {}):
        assert runtime.call(
            "POST", "alice", f"{TASKS}/delete", bad)["status"] == 400
    assert runtime.call("GET", "alice", f"{TASKS}/delete")["status"] == 405

    before = len(runtime.audits)
    done = runtime.call(
        "POST", "wendy", f"{TASKS}/delete",
        {"ids": mine + [mine[0].upper()], "reason": "cleanup"})
    assert done["status"] == 200, done
    assert done["data"]["deleted"] == mine
    assert done["data"]["count"] == 3
    assert runtime.listed() == [theirs]
    audits = runtime.audits[before:]
    assert [item["action"] for item in audits] == [
        "organization.task_delete"] * 3
    assert all(item["details"]["batch"] is True for item in audits)
    assert all(item["details"]["hasReason"] is True for item in audits)
    assert {item["target"] for item in audits} == set(mine)
    assert all(item["target_type"] == "organization_task" for item in audits)


def test_schema_migration_rekey_and_cron_wiring():
    migration = (
        APP / "migrations" / "0222_organization_task_trash.sql"
    ).read_text(encoding="utf-8")
    down = (
        APP / "migrations" / "down" / "0222_organization_task_trash.sql"
    ).read_text(encoding="utf-8")
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    for name in (
            "0075_world_office_marketing_tasks.sql",
            "0105_organization_tasks.sql"):
        db.executescript(
            (APP / "migrations" / name).read_text(
                encoding="utf-8").split("INSERT INTO schema_meta")[0])
    db.executescript(migration)
    columns = {row[1] for row in db.execute("PRAGMA table_info(organization_tasks)")}
    assert {"deleted_at", "deleted_by_bi"} <= columns
    indexes = {row[1] for row in db.execute("PRAGMA index_list(organization_tasks)")}
    assert "idx_organization_tasks_trash" in indexes
    db.executescript(down)
    columns = {row[1] for row in db.execute("PRAGMA table_info(organization_tasks)")}
    assert not {"deleted_at", "deleted_by_bi"} & columns

    schema = (APP / "src" / "schema.py").read_text(encoding="utf-8")
    assert "deleted_at INTEGER NOT NULL DEFAULT 0" in schema
    assert "idx_organization_tasks_trash" in schema
    entry = (APP / "src" / "entry.py").read_text(encoding="utf-8")
    cleanup = entry.split("async def cleanup_sensitive_audit_records", 1)[1]
    cleanup = cleanup.split("\n\n\n", 1)[0]
    assert "purge_trashed_organization_tasks(env, now)" in cleanup
    purge = entry.split("async def purge_trashed_organization_tasks", 1)[1]
    assert "world_office_tasks.purge_trash(" in purge.split("\n\n\n", 1)[0]
    rekey = (REPO / "tools" / "next_rekey.py").read_text(encoding="utf-8")
    assert re.search(r'"organization_tasks": \([^)]*"deleted_by_bi"', rekey)
    assert '"t.last_used_at,t.expires_at,t.revoked_at,t.created_by_bi,"' in entry
    assert '"createdByBi": str(row.get("created_by_bi") or "")' in entry


class _Env:
    pass


def test_bot_token_acts_with_its_creators_current_rights():
    routes = _load(
        "forkmesh_trash_world_office_routes", WORLD_SRC / "world_office_routes.py")
    members = {"bi-bob": "bob", "bi-alice": "alice", "bi-gone": "gone"}
    roles = {"bob": ("member", "read"), "alice": ("owner", "admin")}

    async def d1_first(env, sql, *args):
        if "FROM org_members" in sql:
            name = members.get(args[1])
            return {"name": name} if name else None
        return None

    async def org_role(env, org_bi, name):
        return roles.get(name, ("", ""))[0]

    async def org_permission(env, org_bi, name):
        return roles.get(name, ("", ""))[1]

    async def account_row(env, name):
        if name in ("bob", "alice"):
            return "bi-" + name, {"name": name, "status": "active"}
        return "", None

    routes._bind_runtime({
        "d1_first": d1_first,
        "_org_role": org_role,
        "_org_permission": org_permission,
        "_account_row": account_row,
        "_account_kind": lambda record: "user",
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 64,
        "valid_node_name": lambda name: bool(
            re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(name or ""))),
    })

    def principal(created_by, org_bi="org-bi"):
        runtime = routes._OfficeMarketingTasksRuntime(_Env(), None, {
            "orgBi": "org-bi", "org": "forkmesh", "scopes": [
                "organization.tasks.write"], "createdByBi": created_by})
        return asyncio.run(runtime.delegated_principal(org_bi))

    assert principal("bi-bob") == {
        "accountBi": "bi-bob", "role": "member", "permission": "read"}
    assert principal("bi-alice")["role"] == "owner"
    empty = {"accountBi": "", "role": "", "permission": ""}
    assert principal("bi-gone") == empty
    assert principal("") == empty
    assert principal("bi-alice", org_bi="other-org") == empty
    session = routes._OfficeMarketingTasksRuntime(_Env(), None, None)
    assert asyncio.run(session.delegated_principal("org-bi")) is None
