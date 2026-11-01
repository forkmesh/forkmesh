import json

from test_notes import ROOT, call
from test_notes_comments import CommentRuntime


class PageRuntime(CommentRuntime):
    def __init__(self):
        super().__init__()
        self.db.executescript((ROOT / "migrations" / "0297_note_pages.sql").read_text())


def note(runtime, actor="alice", title="Plan", markdown="Body", **extra):
    created = call(runtime.use("POST", actor, {"title": title, "markdown": markdown, **extra}),
                   "/api/notes")
    return created


def entry(entry_id, kind="note", **fields):
    return {"id": entry_id, "kind": kind, "depth": 0, **fields}


def test_note_emoji_is_validated_sealed_and_listed():
    runtime = PageRuntime()
    created = note(runtime, emoji="🚀")
    assert created["status"] == 201
    note_id = created["data"]["note"]["id"]
    assert created["data"]["note"]["emoji"] == "🚀"
    stored = runtime.db.execute("SELECT data FROM notes WHERE note_id=?", (note_id,)).fetchone()
    assert json.loads(stored["data"])["emoji"] == "🚀"
    for bad in ("a", "1", "🚀 x", "x" * 20, "<b>", "é"):
        assert call(runtime.use("PATCH", "alice", {"baseVersion": 1, "emoji": bad}),
                    f"/api/notes/{note_id}")["data"]["error"] == "invalid_emoji"
    for good in ("👩🏽‍💻", "🇺🇦", "1️⃣", "❤️"):
        version = call(runtime.use("GET", "alice"), f"/api/notes/{note_id}")["data"]["note"]["version"]
        saved = call(runtime.use("PATCH", "alice", {"baseVersion": version, "emoji": good}),
                     f"/api/notes/{note_id}")
        assert saved["data"]["note"]["emoji"] == good
    version = saved["data"]["note"]["version"]
    kept = call(runtime.use("PATCH", "alice", {"baseVersion": version, "markdown": "New"}),
                f"/api/notes/{note_id}")
    assert kept["data"]["note"]["emoji"] == "❤️"
    listed = call(runtime.use("GET", "alice"), "/api/notes")["data"]["notes"][0]
    assert listed["emoji"] == "❤️"
    cleared = call(runtime.use("PATCH", "alice", {"baseVersion": version + 1, "emoji": ""}),
                   f"/api/notes/{note_id}")
    assert cleared["data"]["note"]["emoji"] == ""
    restored = call(runtime.use("POST", "alice", {"version": 2, "baseVersion": version + 2}),
                    f"/api/notes/{note_id}/versions")
    assert restored["data"]["note"]["emoji"] == "👩🏽‍💻"


def test_page_lifecycle_outline_and_locked_entries():
    runtime = PageRuntime()
    mine = note(runtime, title="Mine", emoji="📌")["data"]["note"]["id"]
    secret = note(runtime, actor="carol", title="Secret")["data"]["note"]["id"]
    refused = call(runtime.use("POST", "alice", {"title": "OTF", "outline": [entry("aaaaaaaa", noteId=secret)]}),
                   "/api/notes/pages")
    assert refused["status"] == 404
    outline = [
        entry("section01", "section", label="Week one"),
        {**entry("noteent1", noteId=mine), "depth": 1},
        entry("linkent1", "link", url="https://ntc.party/", label="NTC"),
    ]
    created = call(runtime.use("POST", "alice", {
        "title": "OTF", "emoji": "🛰️", "accent": "purple", "layout": "continuous",
        "description": "Prep", "outline": outline}), "/api/notes/pages")
    assert created["status"] == 201
    page = created["data"]["page"]
    page_id = page["id"]
    assert (page["role"], page["accent"], page["layout"], page["entries"]) == ("owner", "purple", "continuous", 3)
    assert page["outline"][1]["noteId"] == mine
    stored = runtime.db.execute("SELECT data FROM note_pages WHERE page_id=?", (page_id,)).fetchone()
    assert json.loads(stored["data"])["title"] == "OTF"

    for bad in ({"accent": "green"}, {"layout": "grid"}, {"outline": [entry("short", "section", label="x")]},
                {"outline": [entry("linkent2", "link", url="http://x.test")]},
                {"outline": [{**entry("deepent1", "section", label="x"), "depth": 4}]},
                {"outline": [entry("sectionx", "section", label="a"), entry("sectionx", "section", label="b")]}):
        answer = call(runtime.use("PATCH", "alice", {"baseVersion": 1, **bad}), f"/api/notes/pages/{page_id}")
        assert answer["status"] == 400, bad
    assert call(runtime.use("PATCH", "alice", {"baseVersion": 1, "outline": [
        entry(f"entry{index:04d}", "section", label="x") for index in range(301)]}),
        f"/api/notes/pages/{page_id}")["data"]["error"] == "too_many_entries"

    call(runtime.use("POST", "alice", {"type": "user", "name": "bob", "role": "editor"}),
         f"/api/notes/pages/{page_id}/shares")
    bob_secret = note(runtime, actor="bob", title="Bob's")["data"]["note"]["id"]
    grown = call(runtime.use("PATCH", "bob", {"baseVersion": 1, "outline": outline + [
        entry("bobnote1", noteId=bob_secret)]}), f"/api/notes/pages/{page_id}")
    assert grown["status"] == 200 and grown["data"]["page"]["version"] == 2
    assert call(runtime.use("PATCH", "bob", {"baseVersion": 1, "title": "x"}),
                f"/api/notes/pages/{page_id}")["status"] == 409

    seen = call(runtime.use("GET", "alice", query={"content": "1"}), f"/api/notes/pages/{page_id}")["data"]
    assert set(seen["notes"]) == {mine}
    assert seen["notes"][mine]["emoji"] == "📌" and seen["notes"][mine]["markdown"] == "Body"
    assert [item["kind"] for item in seen["page"]["outline"]] == ["section", "note", "link", "note"]
    plain = call(runtime.use("GET", "bob"), f"/api/notes/pages/{page_id}")["data"]
    assert set(plain["notes"]) == {bob_secret}
    assert "markdown" not in plain["notes"][bob_secret]

    assert [page["id"] for page in call(runtime.use("GET", "bob"), "/api/notes/pages")["data"]["pages"]] == [page_id]
    assert call(runtime.use("GET", "carol"), f"/api/notes/pages/{page_id}")["status"] == 404
    assert call(runtime.use("GET", "carol"), "/api/notes/pages")["data"]["pages"] == []
    assert call(runtime.use("GET"), "/api/notes/pages")["status"] == 401
    assert call(runtime.use("DELETE", "bob"), f"/api/notes/pages/{page_id}")["status"] == 403
    assert call(runtime.use("GET", "bob"), f"/api/notes/pages/{page_id}/shares")["status"] == 404
    assert call(runtime.use("DELETE", "alice"), f"/api/notes/pages/{page_id}")["status"] == 200
    assert call(runtime.use("GET", "alice"), f"/api/notes/{mine}")["status"] == 200
    assert runtime.db.execute("SELECT COUNT(*) AS n FROM note_page_shares").fetchone()["n"] == 0


def test_viewers_cannot_edit_and_org_insiders_can_read():
    runtime = PageRuntime()
    page_id = call(runtime.use("POST", "alice", {"title": "Team"}), "/api/notes/pages")["data"]["page"]["id"]
    call(runtime.use("POST", "alice", {"type": "user", "name": "bob", "role": "viewer"}),
         f"/api/notes/pages/{page_id}/shares")
    assert call(runtime.use("PATCH", "bob", {"baseVersion": 1, "title": "x"}),
                f"/api/notes/pages/{page_id}")["status"] == 404
    runtime.db.execute("INSERT INTO org_members(org_bi,member_bi,name,role) VALUES('org-acme','user-carol','acme','member')")
    call(runtime.use("POST", "alice", {"type": "organization", "name": "acme", "role": "editor"}),
         f"/api/notes/pages/{page_id}/shares")
    assert call(runtime.use("GET", "carol"), f"/api/notes/pages/{page_id}")["data"]["page"]["role"] == "editor"


def test_sharing_a_page_can_share_its_notes_without_lowering_roles():
    runtime = PageRuntime()
    first = note(runtime, title="One")["data"]["note"]["id"]
    second = note(runtime, title="Two")["data"]["note"]["id"]
    foreign = note(runtime, actor="carol", title="Carol's")["data"]["note"]["id"]
    call(runtime.use("POST", "carol", {"type": "user", "name": "alice", "role": "viewer"}),
         f"/api/notes/{foreign}/shares")
    call(runtime.use("POST", "alice", {"type": "user", "name": "bob", "role": "editor"}),
         f"/api/notes/{second}/shares")
    page_id = call(runtime.use("POST", "alice", {"title": "Pack", "outline": [
        entry("entryone", noteId=first), entry("entrytwo", noteId=second),
        entry("entryfor", noteId=foreign)]}), "/api/notes/pages")["data"]["page"]["id"]
    shared = call(runtime.use("POST", "alice", {"type": "user", "name": "bob", "role": "viewer",
                                                 "includeNotes": True}),
                  f"/api/notes/pages/{page_id}/shares")
    assert shared["data"]["sharedNotes"] == [first]
    roles = {row["note_id"]: row["role"] for row in runtime.db.execute(
        "SELECT note_id,role FROM note_shares WHERE principal_bi='user-bob'").fetchall()}
    assert roles == {first: "viewer", second: "editor"}
    seen = call(runtime.use("GET", "bob"), f"/api/notes/pages/{page_id}")["data"]["notes"]
    assert set(seen) == {first, second}
    upgraded = call(runtime.use("POST", "alice", {"type": "user", "name": "bob", "role": "editor",
                                                  "includeNotes": True}),
                    f"/api/notes/pages/{page_id}/shares")
    assert upgraded["data"]["sharedNotes"] == [first]
    plain = call(runtime.use("POST", "alice", {"type": "user", "name": "carol", "role": "viewer"}),
                 f"/api/notes/pages/{page_id}/shares")
    assert plain["data"]["sharedNotes"] == []
