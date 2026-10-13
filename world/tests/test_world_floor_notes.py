import ast
import asyncio
import hmac
import json
import math
import re
import sqlite3
import subprocess
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest
from world_scene_source import world_scene_source


REPOSITORY = Path(__file__).resolve().parents[2]
ROUTES = REPOSITORY / "world" / "src" / "world_routes.py"
DURABLE = REPOSITORY / "world" / "src" / "world_durable_objects.py"
ENTRY = REPOSITORY / "app" / "src" / "entry.py"
RUNTIME = REPOSITORY / "app" / "src" / "app_runtime.py"
MIGRATION = REPOSITORY / "app" / "migrations" / "0215_world_floor_notes.sql"
DOWN = REPOSITORY / "app" / "migrations" / "down" / "0215_world_floor_notes.sql"
PUBLIC = REPOSITORY / "world" / "public" / "world"
MODULE = PUBLIC / "world-floor-notes.js"
APP = (PUBLIC / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


class FakeDate:
    value = 1_800_000_000_000

    @classmethod
    def now(cls):
        return cls.value


class TooLarge(BaseException):
    pass


class Request:
    def __init__(self, method="GET", url="https://world.test/api/world/floor-notes",
                 body=None, account=""):
        self.method = method
        self.url = url
        self.body = body
        self.account = account


class WorldObject:
    def __init__(self, sent):
        self.sent = sent

    async def fetch(self, request):
        self.sent.append(request)
        return type("Response", (), {"status": 200})()


class WorldNamespace:
    def __init__(self):
        self.sent = []

    def idFromName(self, name):
        return name

    def get(self, _id):
        return WorldObject(self.sent)


class Env:
    WORLD_ORIGIN = "https://world.test"

    def __init__(self):
        self.FORKMESH_WORLD = WorldNamespace()


def _select(tree, predicate):
    return [node for node in tree.body if predicate(node)]


def _load_handler(database, pings, admins, accounts, blocked):
    routes = ast.parse(ROUTES.read_text(encoding="utf-8"))
    entry = ast.parse(ENTRY.read_text(encoding="utf-8"))
    selected = _select(routes, lambda node: (
        isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and (node.name.startswith("_world_floor_note")
             or node.name == "world_floor_notes_handler")
    ) or (
        isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name)
                and target.id.startswith("WORLD_FLOOR_NOTE")
                for target in node.targets)
    ))
    selected += _select(entry, lambda node: (
        isinstance(node, ast.FunctionDef)
        and node.name in ("notification_mentions", "valid_node_name")
    ) or (
        isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name)
                and target.id in ("NODE_NAME_RE", "MENTION_RE", "MAX_NODE_NAME")
                for target in node.targets)
    ))

    def rows(cursor):
        names = [column[0] for column in cursor.description or []]
        return [dict(zip(names, row)) for row in cursor.fetchall()]

    async def d1_all(_env, sql, *args):
        return rows(database.execute(sql, args))

    async def d1_first(_env, sql, *args):
        found = rows(database.execute(sql, args))
        return found[0] if found else None

    async def d1_run(_env, sql, *args):
        database.execute(sql, args)

    async def encrypt_row(_env, value):
        return "enc:" + json.dumps(value)[::-1]

    async def decrypt_row(_env, stored):
        return json.loads(str(stored)[4:][::-1])

    async def blind_index(_env, value):
        return "bi:" + value

    async def session(_env, request, _data=None):
        name = request.account
        return ("bi:" + name, {"name": name}) if name else ("", None)

    async def is_admin(_env, name):
        return name in admins

    async def enqueue_notification(_env, recipient, kind, title, **fields):
        pings.append({"recipient": recipient, "kind": kind, "title": title,
                      **fields})
        return True

    async def noop(*_args, **_kwargs):
        return None

    async def bounded(request, _limit=None):
        return request.body

    async def block(_env, _tokens):
        return {"targetType": "ip"} if blocked else None

    async def tokens(_env, _request):
        return {}

    namespace = {
        "re": re, "math": math, "hmac": hmac, "json": json,
        "Date": FakeDate, "parse_qs": parse_qs, "urlparse": urlparse,
        "ensure_schema": noop, "method_name": lambda request: request.method,
        "_request_origin_allowed": lambda _env, _request: True,
        "bounded_json_request": bounded, "RequestBodyTooLarge": TooLarge,
        "json_response": lambda payload, status=200, **_kw: {
            "status": status, "payload": payload},
        "_account_session_record": session, "_is_admin": is_admin,
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
        "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
        "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
        "blind_index": blind_index,
        "enqueue_notification": enqueue_notification,
        "_world_active_manual_block": block,
        "_world_moderation_tokens": tokens,
        "_account_session_secret": lambda _env: b"floor-note-secret",
        "_normalized_http_origin": lambda value: value,
        "JsRequest": type("JsRequest", (), {
            "new": staticmethod(lambda url, init: {"url": url, **init})}),
        "to_js": lambda value: value,
    }
    module = ast.Module(body=selected, type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), str(ROUTES), "exec"),
         namespace)
    counter = iter(range(1, 10_000))
    namespace["_world_floor_note_id"] = lambda: "%032x" % next(counter)
    return namespace


@pytest.fixture
def floor():
    database = sqlite3.connect(":memory:")
    database.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    database.execute("CREATE TABLE users (user_bi TEXT PRIMARY KEY)")
    database.executescript(MIGRATION.read_text(encoding="utf-8"))
    for name in ("alice", "bob", "carol", "dave", "root"):
        database.execute("INSERT INTO users VALUES (?)", ("bi:" + name,))
    pings, admins, accounts = [], {"root"}, {}
    state = {"blocked": False}
    namespace = _load_handler(database, pings, admins, accounts, False)
    env = Env()

    def call(method="GET", body=None, account="", query=""):
        FakeDate.value += 10_000
        request = Request(
            method, "https://world.test/api/world/floor-notes" + query,
            body, account)
        if state["blocked"]:
            async def block(_env, _tokens):
                return {"targetType": "ip"}
            namespace["_world_active_manual_block"] = block
        return asyncio.run(namespace["world_floor_notes_handler"](env, request))

    return {"call": call, "db": database, "pings": pings, "env": env,
            "ns": namespace, "state": state}


def _create(floor, account="alice", text="Meet here", **position):
    body = {"action": "create", "text": text, "x": 4.5, "y": 0, "z": -3}
    body.update(position)
    return floor["call"]("POST", body, account)


def test_guests_read_notes_but_only_signed_in_accounts_write(floor):
    listing = floor["call"]()
    assert listing["status"] == 200
    assert listing["payload"]["notes"] == []
    assert listing["payload"]["canPost"] is False
    denied = _create(floor, account="")
    assert denied["status"] == 401
    assert denied["payload"]["error"] == "sign_in_required"
    created = _create(floor)
    assert created["status"] == 201
    note = created["payload"]["note"]
    assert note["author"] == "alice"
    assert (note["x"], note["y"], note["z"]) == (4.5, 0, -3)
    public = floor["call"]()["payload"]["notes"]
    assert [item["text"] for item in public] == ["Meet here"]
    assert public[0]["mine"] is False
    stored = floor["db"].execute("SELECT data FROM world_floor_notes").fetchone()
    assert "Meet here" not in stored[0]


def test_every_new_note_is_announced_to_the_live_world(floor):
    note_id = _create(floor)["payload"]["note"]["id"]
    sent = floor["env"].FORKMESH_WORLD.sent
    assert sent[-1]["url"].endswith("/api/world/floor-notes-changed")
    body = json.loads(sent[-1]["body"])
    assert body["id"] == note_id
    signature = sent[-1]["headers"]["x-forkmesh-world-control"]
    announce = floor["ns"]["_world_floor_notes_announcement"]
    assert announce(floor["env"], body, signature, body["at"]) == note_id
    assert announce(floor["env"], body, "0" * 64, body["at"]) == ""
    assert announce(floor["env"], body, signature, body["at"] + 61_000) == ""
    forged = dict(body, id="f" * 32)
    assert announce(floor["env"], forged, signature, body["at"]) == ""


def test_positions_and_text_are_bounded(floor):
    assert _create(floor, x=5000)["payload"]["error"] == "invalid_note_position"
    assert _create(floor, x="nan")["payload"]["error"] == "invalid_note_position"
    assert _create(floor, x=True)["payload"]["error"] == "invalid_note_position"
    assert _create(floor, text="   ")["payload"]["error"] == "note_text_required"
    assert _create(floor, text="x" * 501)["payload"]["error"] == "note_too_long"
    assert _create(floor, text="ok\x00\x07")["payload"]["note"]["text"] == "ok"


def test_rate_limit_and_per_account_cap(floor):
    _create(floor)
    FakeDate.value -= 8_000
    assert _create(floor)["status"] == 429
    for _ in range(19):
        assert _create(floor)["status"] == 201
    assert _create(floor)["payload"]["error"] == "note_limit_reached"


def test_mentions_ping_existing_accounts_only(floor):
    created = _create(
        floor, text="@bob and @ghost look, cc @alice @carol")
    assert created["payload"]["mentioned"] == ["bob", "carol"]
    assert sorted(ping["recipient"] for ping in floor["pings"]) == [
        "bob", "carol"]
    ping = floor["pings"][0]
    assert ping["kind"] == "mention"
    assert ping["title"] == "@alice mentioned you in a World note"
    assert ping["href"] == (
        "https://world.test/world/?note=" + created["payload"]["note"]["id"])
    assert ping["source"] == "world-floor-note"


def test_replies_form_a_conversation_that_pings_its_participants(floor):
    note_id = _create(floor, text="Lunch?")["payload"]["note"]["id"]
    floor["pings"].clear()
    first = floor["call"](
        "POST", {"action": "reply", "id": note_id, "text": "Yes"}, "bob")
    assert first["status"] == 201
    assert [ping["recipient"] for ping in floor["pings"]] == ["alice"]
    assert floor["pings"][0]["title"] == "@bob replied to a World note"
    floor["pings"].clear()
    second = floor["call"](
        "POST",
        {"action": "reply", "id": note_id, "text": "Bring @dave"},
        "carol")
    assert sorted(
        (ping["recipient"], ping["title"]) for ping in floor["pings"]) == [
        ("alice", "@carol replied to a World note"),
        ("bob", "@carol replied to a World note"),
        ("dave", "@carol mentioned you in a World note"),
    ]
    thread = floor["call"](query="?id=" + note_id, account="dave")["payload"]
    assert [reply["text"] for reply in thread["replies"]] == [
        "Yes", "Bring @dave"]
    assert thread["note"]["replyCount"] == 2
    assert [reply["author"] for reply in second["payload"]["replies"]] == [
        "bob", "carol"]


def test_only_authors_and_admins_delete(floor):
    note_id = _create(floor)["payload"]["note"]["id"]
    reply = floor["call"](
        "POST", {"action": "reply", "id": note_id, "text": "hi"}, "bob")
    reply_id = reply["payload"]["replies"][0]["id"]
    assert reply["payload"]["replies"][0]["canDelete"] is True
    assert floor["call"](
        "POST", {"action": "delete", "id": note_id}, "bob"
    )["payload"]["error"] == "not_note_author"
    removed = floor["call"](
        "POST", {"action": "delete", "id": note_id, "replyId": reply_id},
        "bob")
    assert removed["payload"]["replies"] == []
    assert removed["payload"]["note"]["replyCount"] == 0
    floor["call"](
        "POST", {"action": "reply", "id": note_id, "text": "again"}, "bob")
    admin_view = floor["call"](query="?id=" + note_id, account="root")
    assert admin_view["payload"]["note"]["canDelete"] is True
    assert floor["call"](
        "POST", {"action": "delete", "id": note_id}, "root"
    )["payload"]["deleted"] == note_id
    assert floor["db"].execute(
        "SELECT COUNT(*) FROM world_floor_note_replies").fetchone() == (0,)
    assert floor["call"](query="?id=" + note_id)["status"] == 404


def test_blocked_connections_cannot_post(floor):
    floor["state"]["blocked"] = True
    assert _create(floor)["payload"]["error"] == "world_access_blocked"


def test_migration_and_down_run_on_fresh_sqlite():
    database = sqlite3.connect(":memory:")
    database.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    database.executescript(MIGRATION.read_text(encoding="utf-8"))
    tables = {row[0] for row in database.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"world_floor_notes", "world_floor_note_replies"} <= tables
    database.executescript(DOWN.read_text(encoding="utf-8"))
    tables = {row[0] for row in database.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert not {"world_floor_notes", "world_floor_note_replies"} & tables
    assert database.execute("SELECT COUNT(*) FROM schema_meta").fetchone() == (0,)


def test_route_relay_and_account_deletion_are_wired():
    runtime = RUNTIME.read_text(encoding="utf-8")
    entry = ENTRY.read_text(encoding="utf-8")
    durable = DURABLE.read_text(encoding="utf-8")
    assert '"/api/world/floor-notes", "/api/world/floor-notes/"' in runtime
    assert "world_floor_notes_handler(self.env, request)" in runtime
    assert 'world_floor_notes_handler = _world_routes.export(' in entry
    assert '"_world_floor_notes_announcement")' in entry
    assert '"/api/world/floor-notes-changed"' in durable
    assert '{"type": "floor-notes", "id": note_id}' in durable
    assert '"world_floor_notes",' in entry
    assert "(SELECT note_id FROM world_floor_notes WHERE account_bi=?)" in entry


def test_scene_reports_empty_floor_taps_and_world_mounts_notes():
    assert "onFloorTap = () => {}," in SCENE
    tap = SCENE[SCENE.index("    if (!hit) {\n      const floorPoint ="):]
    tap = tap[:tap.index("      return;\n    }\n") + 20]
    assert 'officeSceneMode === "town" && !beachSceneActive' in tap
    assert "groundPointAt(pointerStart.x, pointerStart.y)" in tap
    assert "onFloorTap: (tap) => this.floorNotes?.tap(tap)," in APP
    assert 'import("./world-floor-notes.js")' in APP
    assert 'message.type === "floor-notes"' in APP
    assert 'new URLSearchParams(location.search).get("note")' in APP
    assert "this.floorNotes?.dispose();" in APP


def test_floor_note_outcomes_are_shown_inside_the_note_panel():
    module = MODULE.read_text(encoding="utf-8")
    assert "toast" not in module
    assert "function showGone(message) {" in module
    assert 'showGone("Note deleted.");' in module
    assert "showGone(FLOOR_NOTE_ERRORS.note_not_found);" in module
    compose = module[module.index("  function compose(point) {"):]
    compose = compose[:compose.index("\n  }\n")]
    assert compose.index("showPanel();") < compose.index(
        "setStatus(FLOOR_NOTE_ERRORS.sign_in_required);")
    assert "toast:" not in APP[APP.index("  attachFloorNotes() {"):][:1500]


def test_floor_note_module_helpers():
    script = f"""
import {{ mentionQueryAt, normalizeFloorNote }} from {json.dumps(MODULE.as_uri())};
const out = {{
  mid: mentionQueryAt("hi @bo", 6),
  start: mentionQueryAt("@", 1),
  email: mentionQueryAt("me@bo", 5),
  none: mentionQueryAt("hi bob", 6),
  valid: normalizeFloorNote({{ id: "a".repeat(32), author: "Alice", text: "x", x: 1, y: 0, z: 2, mine: true }}),
  badId: normalizeFloorNote({{ id: "zz", author: "alice", text: "x", x: 1, y: 0, z: 2 }}),
  badPos: normalizeFloorNote({{ id: "a".repeat(32), author: "alice", text: "x", x: "nope", y: 0, z: 2 }}),
}};
console.log(JSON.stringify(out));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        capture_output=True, text=True, timeout=30, check=True)
    out = json.loads(result.stdout)
    assert out["mid"] == {"query": "bo", "start": 3}
    assert out["start"] == {"query": "", "start": 0}
    assert out["email"] is None
    assert out["none"] is None
    assert out["valid"]["author"] == "alice"
    assert out["valid"]["mine"] is True
    assert out["valid"]["canDelete"] is False
    assert out["badId"] is None
    assert out["badPos"] is None


def test_world_routes_imports_before_the_runtime_is_bound():
    namespace = {"__name__": "world_routes"}
    exec(compile(ROUTES.read_text(encoding="utf-8"), str(ROUTES), "exec"),
         namespace)
    assert callable(namespace["world_floor_notes_handler"])
