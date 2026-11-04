import json

from test_notes import call
from test_notes_comments import CommentRuntime, shared_note


def anchor(**extra):
    return {"quote": "beta", "prefix": "Alpha ", "suffix": " gamma", "start": 6, "end": 10,
            **extra}


def test_reactions_are_sealed_listed_with_comments_and_announced():
    runtime = CommentRuntime()
    note_id = shared_note(runtime, role="viewer")
    path = f"/api/notes/{note_id}/reactions"
    added = call(runtime.use("POST", "bob", anchor(emoji=" 👍 ")), path)
    assert added["status"] == 201
    reaction = added["data"]["reaction"]
    assert (reaction["author"], reaction["emoji"], reaction["quote"]) == ("bob", "👍", "beta")
    assert (reaction["prefix"], reaction["suffix"], reaction["start"], reaction["end"]) == (
        "Alpha ", " gamma", 6, 10)
    assert added["data"]["comments"] == []
    assert runtime.events[-1] == (note_id, {"type": "note-comments", "actor": "bob"})
    stored = runtime.db.execute(
        "SELECT data FROM note_reactions WHERE reaction_id=?", (reaction["id"],)).fetchone()
    assert json.loads(stored["data"]) == {
        "emoji": "👍", "quote": "beta", "prefix": "Alpha ", "suffix": " gamma"}

    call(runtime.use("POST", "alice", {"emoji": "🎉"}), path)
    listed = call(runtime.use("GET", "alice"), f"/api/notes/{note_id}/comments")["data"]
    assert [(item["author"], item["emoji"], item["quote"]) for item in listed["reactions"]] == [
        ("bob", "👍", "beta"), ("alice", "🎉", "")]
    assert "bob" in listed["people"]


def test_the_same_reaction_twice_is_kept_once():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/reactions"
    first = call(runtime.use("POST", "bob", anchor(emoji="❤️")), path)["data"]["reaction"]
    again = call(runtime.use("POST", "bob", anchor(emoji="❤️")), path)
    assert again["status"] == 200
    assert again["data"]["reaction"]["id"] == first["id"]
    other = call(runtime.use("POST", "alice", anchor(emoji="❤️")), path)
    assert other["status"] == 201
    assert len(other["data"]["reactions"]) == 2


def test_reactions_refuse_bad_emoji_and_outsiders():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/reactions"
    for emoji in ("", "   ", "ok", "👍 👍", "x" * 3, "😀" * 17, "\x01", "<b>", "é"):
        answer = call(runtime.use("POST", "bob", {"emoji": emoji}), path)
        assert (answer["status"], answer["data"]["error"]) == (400, "invalid_reaction")
    assert call(runtime.use("POST", "carol", {"emoji": "👍"}), path)["status"] == 404
    assert call(runtime.use("POST", None, {"emoji": "👍"}), path)["status"] in (401, 404)
    assert call(runtime.use("GET", "bob"), path)["status"] == 405
    assert call(runtime.use("PATCH", "bob", {"emoji": "👍"}), f"{path}/{'0' * 32}")["status"] == 405


def test_reaction_delete_permissions_and_removal_with_the_note():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/reactions"
    alice = call(runtime.use("POST", "alice", anchor(emoji="👀")), path)["data"]["reaction"]
    bob = call(runtime.use("POST", "bob", anchor(emoji="👀")), path)["data"]["reaction"]
    refused = call(runtime.use("DELETE", "bob"), f"{path}/{alice['id']}")
    assert (refused["status"], refused["data"]["error"]) == (403, "reaction_author_required")
    missing = call(runtime.use("DELETE", "bob"), f"{path}/{'0' * 32}")
    assert (missing["status"], missing["data"]["error"]) == (404, "reaction_not_found")
    left = call(runtime.use("DELETE", "bob"), f"{path}/{bob['id']}")
    assert left["status"] == 200
    assert [item["author"] for item in left["data"]["reactions"]] == ["alice"]
    again = call(runtime.use("POST", "bob", anchor(emoji="👀")), path)["data"]["reaction"]
    assert call(runtime.use("DELETE", "alice"), f"{path}/{again['id']}")["status"] == 200
    call(runtime.use("DELETE", "alice"), f"/api/notes/{note_id}")
    assert runtime.db.execute("SELECT COUNT(*) FROM note_reactions").fetchone()[0] == 0


def test_reactions_are_capped_per_note(monkeypatch):
    import notes
    monkeypatch.setattr(notes, "MAX_REACTIONS", 2)
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/reactions"
    assert call(runtime.use("POST", "bob", {"emoji": "👍"}), path)["status"] == 201
    assert call(runtime.use("POST", "bob", {"emoji": "🎉"}), path)["status"] == 201
    full = call(runtime.use("POST", "bob", {"emoji": "😮"}), path)
    assert (full["status"], full["data"]["error"]) == (409, "too_many_reactions")


def test_comments_key_moves_with_reactions():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    presence = f"/api/notes/{note_id}/presence"
    before = call(runtime.use("GET", "alice"), presence)["data"]["commentsKey"]
    assert before == "0:0"
    added = call(runtime.use("POST", "bob", {"emoji": "👍"}),
                 f"/api/notes/{note_id}/reactions")["data"]["reaction"]
    during = call(runtime.use("GET", "alice"), presence)["data"]["commentsKey"]
    assert during != before
    call(runtime.use("DELETE", "bob"), f"/api/notes/{note_id}/reactions/{added['id']}")
    assert call(runtime.use("GET", "alice"), presence)["data"]["commentsKey"] == before
