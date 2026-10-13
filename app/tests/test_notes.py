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

    def use(self, method, actor="", body=None):
        self.method_value = method
        self.actor = actor
        self.body = body or {}
        return self

    def method(self): return self.method_value
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
