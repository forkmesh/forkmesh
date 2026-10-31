import json

from test_notes import ROOT, FakeRuntime, call


class CommentRuntime(FakeRuntime):
    def __init__(self):
        super().__init__()
        self.db.executescript(
            (ROOT / "migrations" / "0296_note_comments.sql").read_text())
        self.db.execute("PRAGMA foreign_keys=ON")
        self.users["carol"] = "user-carol"
        self.pings = []
        self.room = []
        self.peers = []

    async def notify(self, recipient, actor, note_id, comment_id, kind, title,
                     excerpt):
        self.pings.append((recipient, actor, kind, comment_id, title, excerpt))

    async def presence(self, note_id, frame):
        self.room.append((note_id, frame))
        return list(self.peers)


def shared_note(runtime, role="editor", markdown="Alpha beta gamma beta"):
    note = call(runtime.use("POST", "alice", {
        "title": "Plan", "markdown": markdown}), "/api/notes")["data"]["note"]
    call(runtime.use("POST", "alice", {"type": "user", "name": "bob", "role": role}),
         f"/api/notes/{note['id']}/shares")
    return note["id"]


def test_threads_replies_and_sealed_quotes():
    runtime = CommentRuntime()
    note_id = shared_note(runtime, role="viewer")
    path = f"/api/notes/{note_id}/comments"
    created = call(runtime.use("POST", "bob", {
        "body": "Is this right?", "quote": "beta", "prefix": "Alpha ",
        "suffix": " gamma", "start": 6, "end": 10}), path)
    assert created["status"] == 201
    thread = created["data"]["comment"]
    assert thread["author"] == "bob"
    assert (thread["quote"], thread["start"], thread["end"]) == ("beta", 6, 10)
    assert thread["parentId"] == ""
    assert created["data"]["canResolve"] is False
    assert runtime.events[-1] == (note_id, {"type": "note-comments", "actor": "bob"})

    stored = runtime.db.execute(
        "SELECT data FROM note_comments WHERE comment_id=?", (thread["id"],)).fetchone()
    assert json.loads(stored["data"])["quote"] == "beta"

    reply = call(runtime.use("POST", "alice", {
        "body": "Yes", "parentId": thread["id"], "quote": "ignored", "start": 3}), path)
    assert reply["status"] == 201
    assert reply["data"]["comment"]["parentId"] == thread["id"]
    assert reply["data"]["comment"]["quote"] == ""
    assert reply["data"]["comment"]["start"] == 0

    nested = call(runtime.use("POST", "bob", {
        "body": "Deeper", "parentId": reply["data"]["comment"]["id"]}), path)
    assert nested["status"] == 404

    listed = call(runtime.use("GET", "alice"), path)["data"]
    assert [item["body"] for item in listed["comments"]] == ["Is this right?", "Yes"]
    assert listed["people"] == ["alice", "bob"]
    assert listed["canResolve"] is True

    whole = call(runtime.use("POST", "bob", {"body": "General thought"}), path)
    assert whole["data"]["comment"]["quote"] == ""

    kinds = [row["kind"] for row in runtime.db.execute(
        "SELECT kind FROM note_events WHERE note_id=?", (note_id,)).fetchall()]
    assert kinds.count("comment") == 3


def test_invalid_bodies_and_outsiders_are_refused():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/comments"
    assert call(runtime.use("POST", "bob", {"body": "   "}), path)["status"] == 400
    assert call(runtime.use("POST", "bob", {"body": "x" * 4001}), path)["status"] == 400
    assert call(runtime.use("POST", "carol", {"body": "hi"}), path)["status"] == 404
    assert call(runtime.use("GET", "carol"), path)["status"] == 404
    assert call(runtime.use("GET"), path)["status"] == 404
    call(runtime.use("PATCH", "alice", {"baseVersion": 1, "visibility": "public"}),
         f"/api/notes/{note_id}")
    assert call(runtime.use("GET"), path)["status"] == 401
    assert call(runtime.use("GET", "carol"), path)["status"] == 404


def test_mentions_ping_collaborators_only_and_replies_ping_the_thread():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/comments"
    created = call(runtime.use("POST", "alice", {
        "body": "@bob please check, cc @carol and @nobody, mail a@bob.example"}), path)
    comment = created["data"]["comment"]
    assert comment["mentions"] == ["bob"]
    assert [(ping[0], ping[2]) for ping in runtime.pings] == [("bob", "mention")]
    assert runtime.pings[0][4] == "Plan"

    runtime.pings.clear()
    call(runtime.use("POST", "bob", {"body": "Done", "parentId": comment["id"]}), path)
    assert [(ping[0], ping[2]) for ping in runtime.pings] == [("alice", "reply")]

    runtime.pings.clear()
    call(runtime.use("PATCH", "bob", {"body": "Done, @alice"}),
         f"{path}/{runtime.db.execute('SELECT comment_id FROM note_comments WHERE parent_id<>?', ('',)).fetchone()['comment_id']}")
    assert [(ping[0], ping[2]) for ping in runtime.pings] == [("alice", "mention")]


def test_edit_resolve_and_delete_permissions():
    runtime = CommentRuntime()
    note_id = shared_note(runtime, role="viewer")
    call(runtime.use("POST", "alice", {"type": "user", "name": "carol", "role": "editor"}),
         f"/api/notes/{note_id}/shares")
    path = f"/api/notes/{note_id}/comments"
    thread = call(runtime.use("POST", "bob", {"body": "Mine"}), path)["data"]["comment"]
    other = call(runtime.use("POST", "alice", {"body": "Owner's"}), path)["data"]["comment"]
    reply = call(runtime.use("POST", "carol", {"body": "Reply", "parentId": thread["id"]}),
                 path)["data"]["comment"]

    assert call(runtime.use("PATCH", "carol", {"body": "Hijack"}),
                f"{path}/{thread['id']}")["status"] == 403
    edited = call(runtime.use("PATCH", "bob", {"body": "Mine, edited"}), f"{path}/{thread['id']}")
    assert edited["data"]["comment"]["body"] == "Mine, edited"
    assert edited["data"]["comment"]["editedAt"] > 0

    assert call(runtime.use("PATCH", "bob", {"resolved": True}),
                f"{path}/{other['id']}")["status"] == 403
    assert call(runtime.use("PATCH", "carol", {"resolved": True}),
                f"{path}/{reply['id']}")["status"] == 404
    resolved = call(runtime.use("PATCH", "bob", {"resolved": True}), f"{path}/{thread['id']}")
    assert resolved["data"]["comment"]["resolvedBy"] == "bob"
    assert resolved["data"]["comment"]["editedAt"] == edited["data"]["comment"]["editedAt"]
    reopened = call(runtime.use("PATCH", "carol", {"resolved": False}), f"{path}/{thread['id']}")
    assert reopened["data"]["comment"]["resolvedAt"] == 0
    kinds = [row["kind"] for row in runtime.db.execute(
        "SELECT kind FROM note_events WHERE note_id=? ORDER BY id", (note_id,)).fetchall()]
    assert kinds[-2:] == ["resolve", "reopen"]

    assert call(runtime.use("DELETE", "carol"), f"{path}/{other['id']}")["status"] == 403
    deleted = call(runtime.use("DELETE", "alice"), f"{path}/{thread['id']}")
    assert deleted["status"] == 200
    assert [item["id"] for item in deleted["data"]["comments"]] == [other["id"]]
    assert call(runtime.use("DELETE", "alice"), f"{path}/{thread['id']}")["status"] == 404
    assert call(runtime.use("GET", "alice"), f"{path}/{other['id']}")["status"] == 405


def test_comment_cap_and_removal_with_the_note():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)
    path = f"/api/notes/{note_id}/comments"
    first = call(runtime.use("POST", "alice", {"body": "One"}), path)["data"]["comment"]
    runtime.db.executemany(
        "INSERT INTO note_comments(comment_id,note_id,author_bi,created_at,updated_at,data) "
        "VALUES(?,?,?,?,?,?)",
        [(f"{index:032x}", note_id, "user-alice", 1, 1, "{}") for index in range(10_000, 10_999)])
    assert call(runtime.use("POST", "alice", {"body": "Too many"}), path)["status"] == 409
    assert call(runtime.use("DELETE", "alice"), f"/api/notes/{note_id}")["status"] == 200
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM note_comments WHERE comment_id=?", (first["id"],)).fetchone()["n"] == 0


def test_presence_registers_polls_and_reports_the_comments_key():
    runtime = CommentRuntime()
    note_id = shared_note(runtime, role="viewer")
    path = f"/api/notes/{note_id}/presence"
    runtime.peers = [
        {"clientId": "desk-1", "name": "bob", "state": "viewing", "via": "poll"},
        {"clientId": "web-1", "name": "alice", "state": "editing", "via": "live"},
    ]
    answer = call(runtime.use("POST", "bob", {"clientId": "desk-1", "state": "editing"}), path)
    assert answer["status"] == 200
    assert runtime.room[-1] == (note_id, {"clientId": "desk-1", "name": "bob", "state": "viewing"})
    assert [peer["clientId"] for peer in answer["data"]["peers"]] == ["web-1"]
    assert answer["data"]["commentsKey"] == "0:0"

    call(runtime.use("POST", "alice", {"body": "Hi"}), f"/api/notes/{note_id}/comments")
    read = call(runtime.use("GET", "alice", query={"clientId": "web-1"}), path)
    assert runtime.room[-1] == (note_id, None)
    assert [peer["clientId"] for peer in read["data"]["peers"]] == ["desk-1"]
    assert read["data"]["commentsKey"].startswith("1:")

    left = call(runtime.use("POST", "alice", {"clientId": "web-1", "state": "left"}), path)
    assert runtime.room[-1][1]["state"] == "left"
    assert left["status"] == 200
    assert call(runtime.use("POST", "bob", {"clientId": "bad id!", "state": "viewing"}),
                path)["status"] == 400
    assert call(runtime.use("POST", "bob", {"clientId": "desk-1", "state": "typing"}),
                path)["status"] == 400
    assert call(runtime.use("POST", "carol", {"clientId": "c", "state": "viewing"}),
                path)["status"] == 404
    assert call(runtime.use("GET"), path)["status"] in (401, 404)


class FakeSocket:
    def __init__(self, role="editor", actor="ana"):
        self.attachment = {"id": "s", "start": 0, "count": 0, "role": role, "actor": actor}
        self.sent = []

    def deserializeAttachment(self):
        return type("Attachment", (), dict(self.attachment))()

    def serializeAttachment(self, value):
        self.attachment = dict(value)

    def send(self, payload):
        self.sent.append(json.loads(payload))

    def close(self, *_args):
        pass


def note_room_class():
    import ast
    import asyncio as _asyncio
    source = (ROOT / "src" / "entry.py").read_text()
    tree = ast.parse(source)
    nodes = [node for node in tree.body if (
        isinstance(node, ast.ClassDef) and node.name == "ForkMeshNoteRoom") or (
        isinstance(node, ast.FunctionDef) and node.name in ("_ws_attachment", "_ws_attr"))]
    clock = {"now": 1_000_000}
    namespace = {
        "json": json, "asyncio": _asyncio,
        "DurableObject": object,
        "Date": type("Date", (), {"now": staticmethod(lambda: clock["now"])}),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "to_js": lambda value: value,
        "_maintenance_socket_closed": lambda *_args: False,
    }
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "entry.py", "exec"), namespace)
    return namespace["ForkMeshNoteRoom"], clock


def make_room(sockets):
    room_class, clock = note_room_class()
    room = room_class()
    room.env = None
    room._peers = lambda: list(sockets)
    return room, clock


def test_room_tracks_state_and_announces_leaving():
    import asyncio as _asyncio
    editor, viewer = FakeSocket("editor", "ana"), FakeSocket("viewer", "bo")
    room, _clock = make_room([editor, viewer])
    _asyncio.run(room.webSocketMessage(editor, json.dumps(
        {"type": "presence", "clientId": "web-ana", "state": "editing"})))
    assert viewer.sent[-1]["state"] == "editing"
    assert editor.attachment["clientId"] == "web-ana"
    _asyncio.run(room.webSocketMessage(viewer, json.dumps(
        {"type": "presence", "clientId": "web-bo", "state": "editing"})))
    assert editor.sent[-1]["state"] == "viewing"
    _asyncio.run(room.webSocketMessage(viewer, json.dumps(
        {"type": "draft", "clientId": "web-bo", "markdown": "x"})))
    assert editor.sent[-1]["type"] == "presence"
    _asyncio.run(room.webSocketClose(editor, 1000, "", True))
    assert viewer.sent[-1] == {"type": "leave", "clientId": "web-ana", "name": "ana"}


def test_room_polled_presence_is_relayed_and_expires():
    listener = FakeSocket("viewer", "bo")
    listener.attachment.update({"clientId": "web-bo", "state": "viewing"})
    room, clock = make_room([listener])
    peers = room._presence({"clientId": "desk-1", "name": "cy", "state": "editing"})
    assert listener.sent[-1] == {"type": "presence", "clientId": "desk-1", "name": "cy",
                                 "line": 0, "column": 0, "state": "editing"}
    assert {(peer["clientId"], peer["state"], peer["via"]) for peer in peers} == {
        ("web-bo", "viewing", "live"), ("desk-1", "editing", "poll")}
    clock["now"] += 21_000
    assert [peer["clientId"] for peer in room._presence({})] == ["web-bo"]
    room._presence({"clientId": "desk-1", "name": "cy", "state": "viewing"})
    room._presence({"clientId": "desk-1", "name": "cy", "state": "left"})
    assert listener.sent[-1] == {"type": "leave", "clientId": "desk-1", "name": "cy"}
    assert [peer["clientId"] for peer in room._presence({})] == ["web-bo"]


def test_listing_marks_notes_changed_by_others_since_my_last_view():
    runtime = CommentRuntime()
    note_id = shared_note(runtime)

    def listed(actor):
        notes = call(runtime.use("GET", actor), "/api/notes")["data"]["notes"]
        return next(note for note in notes if note["id"] == note_id)

    mine = listed("alice")
    assert (mine["unread"], mine["lastEditedBy"], mine["myLastViewedAt"]) == (False, "alice", 0)
    assert listed("bob")["unread"] is True

    call(runtime.use("POST", "bob", {"kind": "view"}), f"/api/notes/{note_id}/activity")
    seen = listed("bob")
    assert seen["unread"] is False and seen["myLastViewedAt"] > 0

    call(runtime.use("PATCH", "alice", {"baseVersion": 1, "markdown": "Changed"}), f"/api/notes/{note_id}")
    changed = listed("bob")
    assert (changed["unread"], changed["lastEditedBy"]) == (True, "alice")
    assert listed("alice")["unread"] is False

    call(runtime.use("POST", "bob", {"kind": "view"}), f"/api/notes/{note_id}/activity")
    call(runtime.use("POST", "alice", {"body": "Look here"}), f"/api/notes/{note_id}/comments")
    commented = listed("bob")
    assert (commented["unread"], commented["unreadComments"], commented["openComments"]) == (True, 1, 1)
    single = call(runtime.use("GET", "bob"), f"/api/notes/{note_id}")["data"]["note"]
    assert single["unreadComments"] == 1 and single["lastEditedBy"] == "alice"
