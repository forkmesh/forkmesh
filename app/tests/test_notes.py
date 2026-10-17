"""Collaborative note ACL, version, publishing, and attachment contract."""

import asyncio
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import notes


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0120_notes.sql").read_text())
        self.db.executescript(
            (ROOT / "migrations" / "0121_note_views.sql").read_text())
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT)")
        self.db.executescript(
            (ROOT / "migrations" / "0271_note_events.sql").read_text())
        self.db.execute(
            "CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,name TEXT)")
        self.users = {"alice": "user-alice", "bob": "user-bob"}
        self.orgs = {"acme": "org-acme"}
        self.method_value = "GET"
        self.actor = ""
        self.reader = "reader-one"
        self.body = {}
        self.clock = 2_100_000_000_000
        self.ids = 0
        self.events = []
        self.query = {}
        self.searches = []

    def use(self, method, actor="", body=None, query=None):
        self.method_value = method
        self.actor = actor
        self.body = body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def query_params(self): return self.query
    def same_origin(self): return True
    def now(self):
        self.clock += 1
        return self.clock
    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"
    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {"status": status, "data": data, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data):
        bi = self.users.get(self.actor, "")
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def seal(self, value): return json.dumps(value)
    async def open(self, value): return json.loads(value) if value else None
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return cursor
    @staticmethod
    def changes(cursor): return cursor.rowcount
    async def viewer(self, note_id):
        return f"{self.reader}:{note_id}" if self.reader else ""
    async def principal(self, kind, name):
        mapping = self.users if kind == "user" else self.orgs
        return (mapping.get(name, ""), name if name in mapping else "")
    async def account_name(self, account_bi):
        return {bi: name for name, bi in self.users.items()}.get(account_bi, "")
    async def repository_access(self, owner, repo, _actor):
        return (owner, repo) == ("alice", "project")
    async def broadcast(self, note_id, frame):
        self.events.append((note_id, frame))
    async def realtime(self, *_args):
        return self.response({"ok": True, "websocket": True})
    async def audit(self, *_args): pass
    async def search_accounts(self, query, limit):
        self.searches.append(query)
        return sorted(name for name in self.users if query in name)[:limit]
    async def member_organizations(self, account_bi):
        rows = self.db.execute(
            "SELECT name FROM org_members WHERE member_bi=? ORDER BY name",
            (account_bi,)).fetchall()
        return [row["name"] for row in rows]


def call(runtime, path):
    return asyncio.run(notes.handle(runtime, path))


def test_note_lifecycle_acl_versions_and_repository_links():
    runtime = FakeRuntime()
    created = call(runtime.use("POST", "alice", {
        "title": "Launch plan", "markdown": "# One", "visibility": "public",
    }), "/api/notes")
    assert created["status"] == 201
    note = created["data"]["note"]
    note_id = note["id"]
    assert note["version"] == 1
    assert note["visibility"] == "public"

    # Published reads are anonymous, but an anonymous listing stays closed.
    public = call(runtime.use("GET"), f"/api/notes/{note_id}")
    assert public["status"] == 200
    assert public["data"]["note"]["markdown"] == "# One"
    assert "shares" not in public["data"]["note"]
    assert "links" not in public["data"]["note"]
    assert call(runtime.use("GET"), "/api/notes")["status"] == 401

    shared = call(runtime.use("POST", "alice", {
        "type": "user", "name": "bob", "role": "editor",
    }), f"/api/notes/{note_id}/shares")
    assert shared["status"] == 200
    assert shared["data"]["shares"][0]["name"] == "bob"

    edited = call(runtime.use("PATCH", "bob", {
        "baseVersion": 1, "markdown": "# Two",
    }), f"/api/notes/{note_id}")
    assert edited["status"] == 200
    assert edited["data"]["note"]["version"] == 2
    assert runtime.events[-1][1]["type"] == "note-updated"

    conflict = call(runtime.use("PATCH", "alice", {
        "baseVersion": 1, "markdown": "stale",
    }), f"/api/notes/{note_id}")
    assert conflict["status"] == 409
    assert conflict["data"]["note"]["markdown"] == "# Two"

    attached = call(runtime.use("POST", "bob", {
        "owner": "alice", "repo": "project", "kind": "pull", "number": 7,
    }), f"/api/notes/{note_id}/links")
    assert attached["status"] == 200
    assert attached["data"]["links"][0]["number"] == 7

    versions = call(runtime.use("GET", "bob"),
                    f"/api/notes/{note_id}/versions")
    assert [item["version"] for item in versions["data"]["versions"]] == [2, 1]
    assert [item["author"] for item in versions["data"]["versions"]] == [
        "bob", "alice"]
    assert all(item["createdAt"] for item in versions["data"]["versions"])
    runtime.users["carol"] = "user-carol"
    outsider = call(runtime.use("GET", "carol"),
                    f"/api/notes/{note_id}/versions")
    assert outsider["status"] == 200
    assert all("author" not in item for item in outsider["data"]["versions"])
    restored = call(runtime.use("POST", "bob", {
        "version": 1, "baseVersion": 2,
    }), f"/api/notes/{note_id}/versions")
    assert restored["status"] == 200
    assert restored["data"]["note"]["version"] == 3
    assert restored["data"]["note"]["markdown"] == "# One"


def test_listing_carries_sidebar_status_shares_and_view_counts():
    runtime = FakeRuntime()
    published = call(runtime.use("POST", "alice", {
        "title": "Launch plan", "markdown": "# One", "visibility": "public",
    }), "/api/notes")["data"]["note"]
    draft = call(runtime.use("POST", "alice", {
        "title": "Draft", "markdown": "wip",
    }), "/api/notes")["data"]["note"]
    call(runtime.use("POST", "alice", {
        "type": "user", "name": "bob", "role": "editor",
    }), f"/api/notes/{published['id']}/shares")

    # Two anonymous readers, one of them twice: distinct readers and total
    # reads are tracked separately, and neither counts the owner's own opens.
    for reader in ("reader-one", "reader-one", "reader-two"):
        runtime.reader = reader
        assert call(runtime.use("GET"),
                    f"/api/notes/{published['id']}")["status"] == 200
    runtime.reader = "reader-three"
    call(runtime.use("GET", "alice"), f"/api/notes/{published['id']}")

    listed = call(runtime.use("GET", "alice"), "/api/notes")["data"]["notes"]
    by_id = {note["id"]: note for note in listed}
    assert by_id[published["id"]]["visibility"] == "public"
    assert by_id[published["id"]]["views"] == 3
    assert by_id[published["id"]]["readers"] == 2
    assert [(share["name"], share["role"])
            for share in by_id[published["id"]]["shares"]] == [("bob", "editor")]
    assert by_id[draft["id"]]["visibility"] == "private"
    assert by_id[draft["id"]]["shares"] == []
    assert by_id[draft["id"]]["views"] == 0

    # A collaborator's own sidebar shows the same note with their role.
    shared_view = call(runtime.use("GET", "bob"), "/api/notes")["data"]["notes"]
    assert [note["id"] for note in shared_view] == [published["id"]]
    assert shared_view[0]["role"] == "editor"
    assert shared_view[0]["views"] == 3


def test_listing_search_matches_title_and_body():
    runtime = FakeRuntime()
    plan = call(runtime.use("POST", "alice", {
        "title": "Launch plan", "markdown": "Ship the beta",
    }), "/api/notes")["data"]["note"]
    retro = call(runtime.use("POST", "alice", {
        "title": "Retro", "markdown": "The LAUNCH went well",
    }), "/api/notes")["data"]["note"]
    call(runtime.use("POST", "alice", {
        "title": "Groceries", "markdown": "milk",
    }), "/api/notes")

    def search(text, actor="alice"):
        found = call(runtime.use("GET", actor, query={"query": text}), "/api/notes")
        return found["status"], [note["id"] for note in found["data"].get("notes", [])]

    assert search("launch") == (200, [retro["id"], plan["id"]])
    assert search(" BETA ") == (200, [plan["id"]])
    assert search("nothing here") == (200, [])
    assert len(search("")[1]) == 3
    assert "markdown" not in call(runtime.use("GET", "alice", query={"query": "beta"}),
                                  "/api/notes")["data"]["notes"][0]
    assert search("x" * 201)[0] == 400
    assert search("launch", actor="bob") == (200, [])


def test_private_note_is_hidden_and_organization_share_grants_read():
    runtime = FakeRuntime()
    created = call(runtime.use("POST", "alice", {
        "title": "Private", "markdown": "secret",
    }), "/api/notes")
    note_id = created["data"]["note"]["id"]
    assert call(runtime.use("GET"), f"/api/notes/{note_id}")["status"] == 404

    call(runtime.use("POST", "alice", {
        "type": "organization", "name": "acme", "role": "viewer",
    }), f"/api/notes/{note_id}/shares")
    runtime.db.execute(
        "INSERT INTO org_members(org_bi,member_bi,name) VALUES(?,?,?)",
        ("org-acme", "user-bob", "bob"))
    runtime.db.commit()
    visible = call(runtime.use("GET", "bob"), f"/api/notes/{note_id}")
    assert visible["status"] == 200
    assert visible["data"]["note"]["role"] == "viewer"
    denied = call(runtime.use("PATCH", "bob", {
        "baseVersion": 1, "markdown": "nope",
    }), f"/api/notes/{note_id}")
    assert denied["status"] == 404


def test_share_suggestions_leave_out_owner_and_existing_shares():
    runtime = FakeRuntime()
    runtime.users.update({"albert": "user-albert", "carla": "user-carla"})
    runtime.orgs.update({"alpaca": "org-alpaca", "beta": "org-beta"})
    for org_bi, name in (("org-alpaca", "alpaca"), ("org-beta", "beta")):
        runtime.db.execute(
            "INSERT INTO org_members(org_bi,member_bi,name) VALUES(?,?,?)",
            (org_bi, "user-alice", name))
    runtime.db.commit()
    note_id = call(runtime.use("POST", "alice", {"title": "Plan"}),
                   "/api/notes")["data"]["note"]["id"]
    path = f"/api/notes/{note_id}/shares"

    found = call(runtime.use("GET", "alice", query={"query": "@AL"}), path)
    assert found["status"] == 200
    assert found["data"]["suggestions"] == [
        {"type": "user", "name": "albert"},
        {"type": "organization", "name": "alpaca"},
    ]

    call(runtime.use("POST", "alice", {
        "type": "user", "name": "albert", "role": "editor",
    }), path)
    found = call(runtime.use("GET", "alice", query={"query": "a"}), path)
    names = [(item["type"], item["name"]) for item in found["data"]["suggestions"]]
    assert names == [
        ("organization", "alpaca"), ("user", "carla"), ("organization", "beta")]
    assert found["data"]["shares"][0]["name"] == "albert"

    searches = len(runtime.searches)
    empty = call(runtime.use("GET", "alice", query={"query": " "}), path)
    assert empty["data"]["suggestions"] == [] and len(runtime.searches) == searches
    plain = call(runtime.use("GET", "alice"), path)
    assert "suggestions" not in plain["data"]
    assert call(runtime.use("GET", "alice", query={"query": "a" * 64}),
                path)["status"] == 400
    assert call(runtime.use("GET", "bob", query={"query": "a"}),
                path)["status"] == 404


def test_activity_records_views_history_and_actions():
    runtime = FakeRuntime()
    note = call(runtime.use("POST", "alice", {
        "title": "Plan", "markdown": "one"}), "/api/notes")["data"]["note"]
    note_id = note["id"]
    path = f"/api/notes/{note_id}/activity"
    call(runtime.use("POST", "alice", {
        "type": "user", "name": "bob", "role": "viewer"}), f"/api/notes/{note_id}/shares")
    call(runtime.use("POST", "alice", {
        "type": "user", "name": "bob", "role": "editor"}), f"/api/notes/{note_id}/shares")
    call(runtime.use("POST", "alice", {
        "type": "user", "name": "bob", "role": "editor"}), f"/api/notes/{note_id}/shares")
    call(runtime.use("PATCH", "bob", {
        "baseVersion": 1, "markdown": "two"}), f"/api/notes/{note_id}")
    call(runtime.use("PATCH", "alice", {
        "baseVersion": 2, "visibility": "public"}), f"/api/notes/{note_id}")
    call(runtime.use("POST", "alice", {
        "version": 1, "baseVersion": 3}), f"/api/notes/{note_id}/versions")
    call(runtime.use("POST", "alice", {
        "owner": "alice", "repo": "project", "kind": "issue", "number": 7,
    }), f"/api/notes/{note_id}/links")

    first = call(runtime.use("POST", "bob", {"kind": "view"}), path)
    second = call(runtime.use("POST", "bob", {"kind": "view"}), path)
    assert first["status"] == second["status"] == 200
    assert runtime.events[-1][1] == {"type": "note-activity"}
    runtime.clock += notes.VIEW_WINDOW_MS + 1
    call(runtime.use("POST", "bob", {"kind": "view"}), path)
    call(runtime.use("POST", "alice", {"kind": "view"}), path)
    call(runtime.use("POST", "alice", {
        "kind": "issue", "detail": {"owner": "alice", "repo": "project", "number": 9},
    }), path)

    payload = call(runtime.use("GET", "bob"), path)["data"]
    counts = payload["counts"]
    assert counts["views"] == 4
    assert counts["readers"] == 2
    assert counts["versions"] == 4
    assert counts["actions"] == 6
    kinds = [event["kind"] for event in payload["events"]]
    assert kinds[-1] == "create"
    assert kinds.count("edit") == 3
    assert {"share", "role", "publish", "restore", "link", "issue"} <= set(kinds)
    views = [event for event in payload["events"] if event["kind"] == "view"]
    assert [(event["actor"], event["count"]) for event in views] == [
        ("alice", 1), ("bob", 1), ("bob", 2)]
    role = next(event for event in payload["events"] if event["kind"] == "role")
    assert role["detail"] == {"type": "user", "name": "bob", "role": "editor", "from": "viewer"}
    restore = next(event for event in payload["events"] if event["kind"] == "restore")
    assert restore["detail"] == {"version": 1, "as": 4}
    issue = next(event for event in payload["events"] if event["kind"] == "issue")
    assert issue["detail"] == {"owner": "alice", "number": 9, "repo": "project"}
    times = [event["at"] for event in payload["events"]]
    assert times == sorted(times, reverse=True)

    listing = call(runtime.use("GET", "alice"), "/api/notes")["data"]["notes"][0]
    assert (listing["views"], listing["actions"], listing["version"]) == (4, 6, 4)
    single = call(runtime.use("GET", "alice"), f"/api/notes/{note_id}")["data"]["note"]
    assert (single["views"], single["actions"]) == (4, 6)

    assert call(runtime.use("POST", "bob", {"kind": "delete"}), path)["status"] == 400
    assert call(runtime.use("GET"), path)["status"] == 401
    assert call(runtime.use("GET", "carol"), path)["status"] == 401
    call(runtime.use("DELETE", "alice", {
        "type": "user", "name": "bob"}), f"/api/notes/{note_id}/shares")
    assert call(runtime.use("GET", "bob"), path)["status"] == 404
    unshared = call(runtime.use("GET", "alice"), path)["data"]["events"]
    assert unshared[0]["kind"] == "unshare"


def test_activity_keeps_the_latest_events_per_note():
    runtime = FakeRuntime()
    note_id = call(runtime.use("POST", "alice", {"title": "Plan"}),
                   "/api/notes")["data"]["note"]["id"]
    for _ in range(notes.MAX_EVENTS + 5):
        call(runtime.use("POST", "alice", {"kind": "export"}),
             f"/api/notes/{note_id}/activity")
    total = runtime.db.execute(
        "SELECT COUNT(*) FROM note_events WHERE note_id=?", (note_id,)).fetchone()[0]
    assert total == notes.MAX_EVENTS
    runtime.db.execute("PRAGMA foreign_keys=ON")
    call(runtime.use("DELETE", "alice"), f"/api/notes/{note_id}")
    assert runtime.db.execute("SELECT COUNT(*) FROM note_events").fetchone()[0] == 0
