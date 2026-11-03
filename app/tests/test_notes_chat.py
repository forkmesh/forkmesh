import asyncio
import json

from test_notes import call
from test_notes_comments import CommentRuntime, FakeSocket, make_room, shared_note


ChatRuntime = CommentRuntime


def test_chat_messages_are_sealed_listed_and_announced():
    runtime = ChatRuntime()
    note_id = shared_note(runtime, role="viewer")
    path = f"/api/notes/{note_id}/chat"
    sent = call(runtime.use("POST", "bob", {"body": "  Hello there  "}), path)
    assert sent["status"] == 201
    message = sent["data"]["message"]
    assert (message["author"], message["body"]) == ("bob", "Hello there")
    assert sent["data"]["canModerate"] is False
    assert runtime.events[-1] == (note_id, {"type": "note-chat", "actor": "bob"})
    stored = runtime.db.execute(
        "SELECT data FROM note_chat WHERE message_id=?", (message["id"],)).fetchone()
    assert json.loads(stored["data"]) == {"body": "Hello there"}

    call(runtime.use("POST", "alice", {"body": "Hi bob"}), path)
    listed = call(runtime.use("GET", "alice"), path)["data"]
    assert [(item["author"], item["body"]) for item in listed["messages"]] == [
        ("bob", "Hello there"), ("alice", "Hi bob")]
    assert listed["canModerate"] is True


def test_chat_refuses_bad_bodies_and_outsiders():
    runtime = ChatRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/chat"
    assert call(runtime.use("POST", "bob", {"body": "   "}), path)["status"] == 400
    assert call(runtime.use("POST", "bob", {"body": "x" * 2001}), path)["status"] == 400
    assert call(runtime.use("POST", "carol", {"body": "hi"}), path)["status"] == 404
    assert call(runtime.use("GET", "carol"), path)["status"] == 404
    assert call(runtime.use("GET"), path)["status"] in (401, 404)
    assert call(runtime.use("PATCH", "bob", {"body": "x"}), path)["status"] == 405


def test_chat_delete_permissions_and_removal_with_the_note():
    runtime = ChatRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/chat"
    alice = call(runtime.use("POST", "alice", {"body": "From alice"}), path)["data"]["message"]
    bob = call(runtime.use("POST", "bob", {"body": "From bob"}), path)["data"]["message"]
    assert call(runtime.use("DELETE", "bob"), f"{path}/{alice['id']}")["status"] == 403
    assert call(runtime.use("DELETE", "bob"), f"{path}/{'0' * 32}")["status"] == 404
    left = call(runtime.use("DELETE", "alice"), f"{path}/{bob['id']}")
    assert left["status"] == 200
    assert [item["body"] for item in left["data"]["messages"]] == ["From alice"]
    call(runtime.use("DELETE", "alice"), f"/api/notes/{note_id}")
    assert runtime.db.execute("SELECT COUNT(*) FROM note_chat").fetchone()[0] == 0


def test_chat_keeps_the_latest_messages_only(monkeypatch):
    import notes
    monkeypatch.setattr(notes, "MAX_CHAT", 3)
    runtime = ChatRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/chat"
    for number in range(5):
        call(runtime.use("POST", "alice", {"body": f"m{number}"}), path)
    listed = call(runtime.use("GET", "bob"), path)["data"]["messages"]
    assert [item["body"] for item in listed] == ["m2", "m3", "m4"]


def test_presence_carries_the_cursor_line_and_the_chat_key():
    runtime = ChatRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/presence"
    answer = call(runtime.use("POST", "bob", {
        "clientId": "desk-1", "state": "editing", "line": 4, "column": 2}), path)
    assert runtime.room[-1] == (note_id, {
        "clientId": "desk-1", "name": "bob", "state": "editing", "line": 4, "column": 2})
    assert answer["data"]["chatKey"] == "0:0"
    call(runtime.use("POST", "bob", {"clientId": "desk-1", "state": "viewing", "line": "x"}), path)
    assert "line" not in runtime.room[-1][1]
    call(runtime.use("POST", "alice", {"body": "Hi"}), f"/api/notes/{note_id}/chat")
    read = call(runtime.use("GET", "alice"), path)
    assert read["data"]["chatKey"].startswith("1:")


def test_room_remembers_cursor_lines_for_polling_followers():
    editor, viewer = FakeSocket("editor", "ana"), FakeSocket("viewer", "bo")
    room, _clock = make_room([editor, viewer])
    asyncio.run(room.webSocketMessage(editor, json.dumps(
        {"type": "cursor", "clientId": "web-ana", "line": 12, "column": 3})))
    assert viewer.sent[-1]["type"] == "cursor"
    assert (viewer.sent[-1]["line"], viewer.sent[-1]["column"]) == (12, 3)
    assert (editor.attachment["line"], editor.attachment["column"]) == (12, 3)
    asyncio.run(room.webSocketMessage(editor, json.dumps(
        {"type": "presence", "clientId": "web-ana", "state": "editing"})))
    assert editor.attachment["line"] == 12
    peers = room._presence({"clientId": "desk-1", "name": "cy", "state": "viewing",
                            "line": 7, "column": 1})
    assert viewer.sent[-1] == {"type": "presence", "clientId": "desk-1", "name": "cy",
                               "line": 7, "column": 1, "state": "viewing"}
    live = next(peer for peer in peers if peer["clientId"] == "web-ana")
    polled = next(peer for peer in peers if peer["clientId"] == "desk-1")
    assert (live["line"], live["column"]) == (12, 3)
    assert (polled["line"], polled["column"]) == (7, 1)


def test_room_relays_chat_events():
    listener = FakeSocket("viewer", "bo")
    room, _clock = make_room([listener])

    class Request:
        url = "https://forkmesh.internal/api/notes/x/event"
        headers = {}

        async def text(self):
            return json.dumps({"type": "note-chat", "actor": "ana"})

    namespace = room.fetch.__globals__
    namespace.setdefault("method_name", lambda _request: "POST")
    namespace.setdefault("urlparse", __import__("urllib.parse").parse.urlparse)
    namespace.setdefault("json_response", lambda data, status=200: (status, data))
    assert asyncio.run(room.fetch(Request())) == (200, {"ok": True})
    assert listener.sent[-1] == {"type": "note-chat", "actor": "ana"}
