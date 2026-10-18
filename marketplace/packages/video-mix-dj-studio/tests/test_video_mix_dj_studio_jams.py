import asyncio
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import video_mix_dj_studio as jam  # noqa: E402


ACCOUNTS = {"alice", "bob", "cara", "dan"}
SONG = "dQw4w9WgXcQ"
OTHER = "9bZkp7q19f0"


class Runtime:
    def __init__(self):
        jam._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.clock = 1_789_561_860_000
        self.ids = 0
        self.notes = []
        self.audits = []
        self.origin_ok = True

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return self.origin_ok
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = "bi:" + self.actor if self.actor in ACCOUNTS else ""
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    async def account_exists(self, name): return name in ACCOUNTS
    async def notify(self, recipient, kind, title, **kwargs): self.notes.append((recipient, kind, title, kwargs))
    async def audit(self, *args): self.audits.append(args)
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor


def call(runtime, path):
    return asyncio.run(jam.handle(runtime, path))


def api(runtime, method, path, actor="alice", body=None):
    return call(runtime.use(method, actor, body), "/api/video-mix-dj-studio" + path)


def test_owner_creates_a_jam_invites_friends_and_they_join():
    runtime = Runtime()
    made = api(runtime, "POST", "/jams", body={"title": "Friday set", "members": ["Bob", "@cara", "ghost", "alice", "bad name!"]})
    assert made["status"] == 200, made
    session = made["data"]["jam"]
    assert session["title"] == "Friday set"
    assert session["myStatus"] == "owner"
    assert made["data"]["invited"] == ["bob", "cara"]
    assert made["data"]["skipped"] == ["ghost", "bad name!"]
    assert [(m["name"], m["status"]) for m in session["members"]] == [("alice", "owner"), ("bob", "invited"), ("cara", "invited")]
    assert [note[0] for note in runtime.notes] == ["bob", "cara"]
    assert runtime.notes[0][3]["href"] == "/dashboard/video-mix-dj-studio?jam=" + session["id"]
    listed = api(runtime, "GET", "/jams", actor="bob")["data"]["jams"]
    assert listed == [{"id": session["id"], "title": "Friday set", "owner": "alice", "myStatus": "invited", "members": 1}]
    peek = api(runtime, "GET", "/jams/" + session["id"], actor="bob")["data"]["jam"]
    assert peek["myStatus"] == "invited" and peek["queue"] == [] and peek["decks"] == {}
    assert api(runtime, "POST", "/jams/" + session["id"] + "/queue", actor="bob", body={"videoId": SONG})["status"] == 403
    joined = api(runtime, "POST", "/jams/" + session["id"] + "/join", actor="bob")["data"]["jam"]
    assert joined["myStatus"] == "joined"
    assert api(runtime, "GET", "/jams", actor="bob")["data"]["jams"][0]["members"] == 2
    assert api(runtime, "GET", "/jams/" + session["id"], actor="dan")["status"] == 404


def test_members_share_the_queue_and_decks():
    runtime = Runtime()
    jam_id = api(runtime, "POST", "/jams", body={"members": ["bob"]})["data"]["jam"]["id"]
    api(runtime, "POST", "/jams/" + jam_id + "/join", actor="bob")
    queued = api(runtime, "POST", "/jams/" + jam_id + "/queue", actor="bob",
                 body={"videoId": SONG, "title": "  Never   Gonna ", "author": "Rick"})["data"]["jam"]
    assert queued["queue"] == [{"id": queued["queue"][0]["id"], "videoId": SONG, "title": "Never Gonna", "author": "Rick", "addedBy": "bob"}]
    assert api(runtime, "POST", "/jams/" + jam_id + "/queue", actor="bob", body={"videoId": "nope"})["data"]["error"] == "invalid_video"
    api(runtime, "POST", "/jams/" + jam_id + "/queue", body={"videoId": OTHER})
    deck = api(runtime, "PUT", "/jams/" + jam_id + "/decks/a", body={
        "videoId": SONG, "title": "Never Gonna", "playing": True, "position": 42.123456, "rate": 3})["data"]["jam"]["decks"]["a"]
    assert deck["videoId"] == SONG and deck["playing"] is True and deck["position"] == 42.123
    assert deck["rate"] == 1.16 and deck["by"] == "alice" and deck["at"] > 0
    seen = api(runtime, "GET", "/jams/" + jam_id, actor="bob")["data"]["jam"]
    assert seen["decks"]["a"] == deck
    assert [item["videoId"] for item in seen["queue"]] == [SONG, OTHER]
    assert seen["now"] > deck["at"]
    first = seen["queue"][0]["id"]
    after = api(runtime, "DELETE", "/jams/" + jam_id + "/queue/" + first)["data"]["jam"]
    assert [item["videoId"] for item in after["queue"]] == [OTHER]
    assert api(runtime, "PUT", "/jams/" + jam_id + "/decks/c", body={})["data"]["error"] == "invalid_deck"
    stored = runtime.db.execute("SELECT data FROM vmdj_jam_queue").fetchone()[0]
    assert stored.startswith("sealed:")


def test_leaving_removing_and_ending():
    runtime = Runtime()
    jam_id = api(runtime, "POST", "/jams", body={"members": ["bob", "cara"]})["data"]["jam"]["id"]
    for name in ("bob", "cara"):
        api(runtime, "POST", "/jams/" + jam_id + "/join", actor=name)
    assert api(runtime, "DELETE", "/jams/" + jam_id + "/members/cara", actor="bob")["status"] == 403
    invited = api(runtime, "POST", "/jams/" + jam_id + "/members", actor="bob", body={"names": ["dan"]})["data"]
    assert invited["invited"] == ["dan"]
    removed = api(runtime, "DELETE", "/jams/" + jam_id + "/members/cara")["data"]["jam"]
    assert [m["name"] for m in removed["members"]] == ["alice", "bob", "dan"]
    assert api(runtime, "POST", "/jams/" + jam_id + "/leave")["data"]["error"] == "owner_cannot_leave"
    assert api(runtime, "POST", "/jams/" + jam_id + "/leave", actor="bob")["data"]["left"] is True
    assert api(runtime, "GET", "/jams/" + jam_id, actor="bob")["status"] == 404
    assert api(runtime, "DELETE", "/jams/" + jam_id, actor="dan")["data"]["left"] is True
    assert api(runtime, "DELETE", "/jams/" + jam_id)["data"]["ended"] is True
    for table in ("vmdj_jams", "vmdj_jam_members", "vmdj_jam_queue", "vmdj_jam_decks"):
        assert runtime.db.execute("SELECT COUNT(*) FROM " + table).fetchone()[0] == 0


def test_requests_are_checked():
    runtime = Runtime()
    assert api(runtime, "GET", "/jams", actor="ghost")["status"] == 401
    assert api(runtime, "PATCH", "/jams")["status"] == 405
    assert api(runtime, "GET", "/other")["status"] == 404
    assert api(runtime, "GET", "/jams/not-an-id")["status"] == 404
    runtime.origin_ok = False
    assert api(runtime, "POST", "/jams")["status"] == 403
    runtime.origin_ok = True
    for _ in range(jam.MAX_JAMS_PER_ACCOUNT):
        assert api(runtime, "POST", "/jams")["status"] == 200
    assert api(runtime, "POST", "/jams")["data"]["error"] == "too_many_jams"


def test_purge_removes_owned_jams_and_memberships():
    runtime = Runtime()
    mine = api(runtime, "POST", "/jams", body={"members": ["bob"]})["data"]["jam"]["id"]
    theirs = api(runtime, "POST", "/jams", actor="bob", body={"members": ["alice", "cara"]})["data"]["jam"]["id"]
    api(runtime, "POST", "/jams/" + theirs + "/join")
    api(runtime, "POST", "/jams/" + mine + "/queue", body={"videoId": SONG})
    assert asyncio.run(jam.account_has_data(runtime, "bi:alice"))
    asyncio.run(jam.purge_account_data(runtime, "bi:alice", "alice"))
    assert runtime.db.execute("SELECT COUNT(*) FROM vmdj_jams WHERE jam_id=?", (mine,)).fetchone()[0] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM vmdj_jam_queue").fetchone()[0] == 0
    names = [row[0] for row in runtime.db.execute("SELECT account_name FROM vmdj_jam_members WHERE jam_id=?", (theirs,))]
    assert names == ["bob", "cara"]
    asyncio.run(jam.purge_account_data(runtime, "bi:cara", "cara"))
    names = [row[0] for row in runtime.db.execute("SELECT account_name FROM vmdj_jam_members WHERE jam_id=?", (theirs,))]
    assert names == ["bob"]


def test_manifest_declares_the_jam_worker_and_the_player_reports_audio_sources():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["worker"] == {"module": "video_mix_dj_studio", "api": jam.PREFIX, "outboundOrigins": []}
    assert manifest["entrypoints"]["worker"] == "content/worker/video_mix_dj_studio.py"
    assert manifest["dataLifecycle"] == {"scope": "account"}
    player = (PACKAGE.parents[2] / "native" / "ios" / "ForkMesh" / "Features" / "Packages" / "VideoMixDjStudioPlayer.swift").read_text(encoding="utf-8")
    assert 'if(a.fmCors)parent.postMessage({fmdjFrame:"source",src:src},"*")' in player
    assert 'post({type:"source",deck:k,id:d.id,src:m.src})' in player


def test_presence_shares_status_and_motion_and_doubles_as_the_poll():
    runtime = Runtime()
    jam_id = api(runtime, "POST", "/jams", body={"members": ["bob", "cara"]})["data"]["jam"]["id"]
    api(runtime, "POST", "/jams/" + jam_id + "/join", actor="bob")
    assert api(runtime, "POST", "/jams/" + jam_id + "/presence", actor="cara", body={})["status"] == 403
    motion = [[0.5, -0.25], [9, -9], ["x", None], [1], *[[0.1, 0.1]] * 20]
    seen = api(runtime, "POST", "/jams/" + jam_id + "/presence", actor="bob", body={
        "state": "listening", "playing": True, "energy": 4, "motion": motion})["data"]["jam"]
    [bob] = seen["presence"]
    assert bob["name"] == "bob" and bob["state"] == "listening" and bob["playing"] is True
    assert bob["energy"] == 1.0 and bob["ago"] >= 0
    assert bob["motion"][:3] == [[0.5, -0.25], [2.0, -2.0], [0.0, 0.0]]
    assert len(bob["motion"]) == jam.MAX_MOTION
    other = api(runtime, "POST", "/jams/" + jam_id + "/presence", body={"state": "dancing"})["data"]["jam"]
    assert [(p["name"], p["state"]) for p in other["presence"]] == [("alice", "studio"), ("bob", "listening")]
    stored = runtime.db.execute("SELECT data FROM vmdj_jam_presence WHERE account_name='bob'").fetchone()[0]
    assert stored.startswith("sealed:")
    runtime.clock += (jam.PRESENCE_SECONDS + 1) * 1000
    assert api(runtime, "GET", "/jams/" + jam_id, actor="bob")["data"]["jam"]["presence"] == []
    peek = api(runtime, "GET", "/jams/" + jam_id, actor="cara")["data"]["jam"]
    assert peek["presence"] == [] and peek["reactions"] == []
    api(runtime, "POST", "/jams/" + jam_id + "/presence", actor="bob", body={})
    api(runtime, "POST", "/jams/" + jam_id + "/leave", actor="bob")
    assert runtime.db.execute("SELECT COUNT(*) FROM vmdj_jam_presence WHERE account_name='bob'").fetchone()[0] == 0


def test_reactions_are_short_lived_and_rate_limited():
    runtime = Runtime()
    jam_id = api(runtime, "POST", "/jams", body={"members": ["bob"]})["data"]["jam"]["id"]
    api(runtime, "POST", "/jams/" + jam_id + "/join", actor="bob")
    assert api(runtime, "POST", "/jams/" + jam_id + "/reactions", body={"kind": "Bad Kind!"})["data"]["error"] == "invalid_reaction"
    sent = api(runtime, "POST", "/jams/" + jam_id + "/reactions", body={"kind": "hearts"})["data"]["jam"]
    [heart] = sent["reactions"]
    assert heart["name"] == "alice" and heart["kind"] == "hearts" and heart["id"] and heart["ago"] >= 0
    seen = api(runtime, "GET", "/jams/" + jam_id, actor="bob")["data"]["jam"]["reactions"]
    assert [r["id"] for r in seen] == [heart["id"]]
    for _ in range(jam.MAX_REACTIONS_PER_MEMBER - 1):
        assert api(runtime, "POST", "/jams/" + jam_id + "/reactions", body={"kind": "fire"})["status"] == 200
    assert api(runtime, "POST", "/jams/" + jam_id + "/reactions", body={"kind": "fire"})["status"] == 429
    assert api(runtime, "POST", "/jams/" + jam_id + "/reactions", actor="bob", body={"kind": "fire"})["status"] == 200
    runtime.clock += (jam.REACTION_SECONDS + 1) * 1000
    assert api(runtime, "GET", "/jams/" + jam_id, actor="bob")["data"]["jam"]["reactions"] == []
    again = api(runtime, "POST", "/jams/" + jam_id + "/reactions", body={"kind": "confetti"})["data"]["jam"]["reactions"]
    assert [r["kind"] for r in again] == ["confetti"]
    assert runtime.db.execute("SELECT COUNT(*) FROM vmdj_jam_reactions").fetchone()[0] == 1
    api(runtime, "DELETE", "/jams/" + jam_id)
    for table in ("vmdj_jam_presence", "vmdj_jam_reactions"):
        assert runtime.db.execute("SELECT COUNT(*) FROM " + table).fetchone()[0] == 0


def test_purge_drops_presence_and_reactions():
    runtime = Runtime()
    theirs = api(runtime, "POST", "/jams", actor="bob", body={"members": ["alice"]})["data"]["jam"]["id"]
    api(runtime, "POST", "/jams/" + theirs + "/join")
    api(runtime, "POST", "/jams/" + theirs + "/presence", body={"energy": 0.5})
    api(runtime, "POST", "/jams/" + theirs + "/reactions", body={"kind": "fire"})
    api(runtime, "POST", "/jams/" + theirs + "/presence", actor="bob", body={})
    asyncio.run(jam.purge_account_data(runtime, "bi:alice", "alice"))
    assert [row[0] for row in runtime.db.execute("SELECT account_name FROM vmdj_jam_presence")] == ["bob"]
    assert runtime.db.execute("SELECT COUNT(*) FROM vmdj_jam_reactions").fetchone()[0] == 0
