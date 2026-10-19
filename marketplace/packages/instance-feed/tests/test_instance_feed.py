import asyncio
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import instance_feed  # noqa: E402


ACCOUNTS = {"alice": "Alice", "bob": "bob", "carol": "carol", "admin": "admin"}


class Runtime:
    def __init__(self, installed=("alice", "bob", "admin")):
        instance_feed._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.installed = set(installed)
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.query = {}
        self.clock = 1_789_561_860_000
        self.ids = 0
        self.audits = []
        self.notes = []
        self.origin_ok = True

    def use(self, method, actor="alice", body=None, query=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def query_params(self): return dict(self.query)
    def same_origin(self): return self.origin_ok
    def now(self): self.clock += 1000; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        if self.actor not in ACCOUNTS:
            return "", None
        return "bi:" + self.actor, {"name": self.actor, "nameCase": ACCOUNTS[self.actor]}
    async def account_plugin_installed(self, account_bi):
        return account_bi.removeprefix("bi:") in self.installed
    async def is_admin(self, name): return name == "admin"
    async def account(self, name):
        name = str(name).lower()
        return ("bi:" + name, name) if name in ACCOUNTS else ("", "")
    async def notify(self, recipient, kind, title, body="", href="", dedupe="", meta=None, push=True):
        self.notes.append((recipient, kind, title, href, dedupe))
        return True
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append(args)


def call(runtime, path):
    return asyncio.run(instance_feed.handle(runtime, path))


def post(runtime, text, actor="alice", **extra):
    response = call(runtime.use("POST", actor=actor, body=dict(text=text, **extra)),
                    "/api/instance-feed/posts")
    assert response["status"] == 201, response
    return response["data"]["post"]


def timeline(runtime, actor="alice", **query):
    response = call(runtime.use("GET", actor=actor, query=query), "/api/instance-feed")
    assert response["status"] == 200, response
    return response["data"]


def test_not_installed_and_signed_out():
    runtime = Runtime(installed=())
    assert timeline(runtime) == {"ok": True, "installed": False, "actor": "alice"}
    response = call(runtime.use("POST", body={"text": "hi"}), "/api/instance-feed/posts")
    assert response["status"] == 409
    assert call(runtime.use("GET", actor="mallory"), "/api/instance-feed")["status"] == 401
    assert call(runtime.use("PUT"), "/api/instance-feed/posts")["status"] == 405
    assert call(runtime.use("GET"), "/api/instance-feed/nope")["status"] == 404
    assert call(runtime.use("GET"), "/api/other")["status"] == 404


def test_cross_origin_write_refused():
    runtime = Runtime()
    runtime.origin_ok = False
    response = call(runtime.use("POST", body={"text": "hi"}), "/api/instance-feed/posts")
    assert response["status"] == 403


def test_timeline_is_shared_newest_first_and_sealed():
    runtime = Runtime()
    first = post(runtime, "Hello instance")
    second = post(runtime, "Morning all", actor="bob")
    assert first["authorName"] == "Alice" and first["author"] == "alice"
    data = timeline(runtime, actor="bob")
    assert data["installed"] is True and data["maxLength"] == 500
    assert [item["id"] for item in data["posts"]] == [second["id"], first["id"]]
    assert data["posts"][1]["mine"] is False and data["posts"][0]["mine"] is True
    assert data["posts"][1]["canDelete"] is False
    stored = runtime.db.execute("SELECT data FROM instance_feed_posts WHERE post_id=?",
                                (first["id"],)).fetchone()["data"]
    assert stored.startswith("sealed:")


def test_text_validation_and_content_warning():
    runtime = Runtime()
    assert call(runtime.use("POST", body={"text": "   "}), "/api/instance-feed/posts")["status"] == 400
    too_long = call(runtime.use("POST", body={"text": "x" * 501}), "/api/instance-feed/posts")
    assert too_long["status"] == 400 and too_long["data"]["error"] == "text_too_long"
    warned = post(runtime, "line one\r\n\r\n\r\n\r\nline two", spoiler="  spoilers  for   the finale ")
    assert warned["text"] == "line one\n\nline two"
    assert warned["spoiler"] == "spoilers for the finale"
    assert len(post(runtime, "y" * 500)["text"]) == 500


def test_pagination_cursor():
    runtime = Runtime()
    ids = [post(runtime, "post %d" % index)["id"] for index in range(5)]
    page = timeline(runtime, limit="2")
    assert [item["id"] for item in page["posts"]] == ids[:2:-1][:2]
    seen = [item["id"] for item in page["posts"]]
    while page["next"]:
        page = timeline(runtime, limit="2", before=page["next"])
        seen += [item["id"] for item in page["posts"]]
    assert seen == ids[::-1]


def test_replies_threads_and_notifications():
    runtime = Runtime()
    root = post(runtime, "What should we build next?")
    reply = post(runtime, "@alice a feed!", actor="bob", replyTo=root["id"])
    nested = post(runtime, "Agreed", actor="admin", replyTo=reply["id"])
    later = post(runtime, "Second answer", actor="admin", replyTo=root["id"])
    assert reply["replyToAuthor"] == "alice"
    data = timeline(runtime)
    assert [item["id"] for item in data["posts"]] == [root["id"]]
    assert data["posts"][0]["replies"] == 2
    thread = call(runtime.use("GET"), "/api/instance-feed/posts/" + nested["id"])["data"]
    assert [item["id"] for item in thread["ancestors"]] == [root["id"], reply["id"]]
    assert thread["post"]["id"] == nested["id"] and thread["replies"] == []
    thread = call(runtime.use("GET"), "/api/instance-feed/posts/" + root["id"])["data"]
    assert [item["id"] for item in thread["replies"]] == [reply["id"], nested["id"], later["id"]]
    recipients = [(note[0], note[2]) for note in runtime.notes]
    assert ("alice", "bob replied to your post") in recipients
    assert ("bob", "admin replied to your post") in recipients
    assert not any(name == "alice" and "mentioned" in title for name, title in recipients)
    assert all(note[1] == "plugin_activity" for note in runtime.notes)
    assert runtime.notes[0][3] == "/dashboard/instance-feed?post=" + reply["id"]
    missing = call(runtime.use("POST", body={"text": "x", "replyTo": "f" * 32}),
                   "/api/instance-feed/posts")
    assert missing["status"] == 404


def test_mentions_notify_installed_accounts_only():
    runtime = Runtime()
    post(runtime, "hey @Bob and @carol and @nobody and @alice, email me@bob.dev")
    assert [note[0] for note in runtime.notes] == ["bob"]
    assert runtime.notes[0][2] == "Alice mentioned you"


def test_favourites_and_boosts():
    runtime = Runtime()
    original = post(runtime, "Boost me", actor="bob")
    path = "/api/instance-feed/posts/" + original["id"]
    liked = call(runtime.use("POST"), path + "/favourite")["data"]["post"]
    assert liked["favourites"] == 1 and liked["favourited"] is True
    call(runtime.use("POST"), path + "/favourite")
    assert call(runtime.use("GET"), path)["data"]["post"]["favourites"] == 1
    boosted = call(runtime.use("POST"), path + "/boost")["data"]["post"]
    assert boosted["boosts"] == 1 and boosted["boosted"] is True
    call(runtime.use("POST"), path + "/boost")
    data = timeline(runtime, actor="admin")
    assert data["posts"][0]["type"] == "boost"
    assert data["posts"][0]["booster"] == "alice" and data["posts"][0]["boosterName"] == "Alice"
    assert data["posts"][0]["post"]["id"] == original["id"]
    assert data["posts"][0]["post"]["boosts"] == 1 and data["posts"][0]["post"]["boosted"] is False
    assert [note[2] for note in runtime.notes] == ["Alice favourited your post", "Alice boosted your post"]
    boost_id = data["posts"][0]["id"]
    assert call(runtime.use("POST"), "/api/instance-feed/posts/" + boost_id + "/boost")["status"] == 404
    assert call(runtime.use("GET"), "/api/instance-feed/posts/" + boost_id)["data"]["post"]["id"] == original["id"]
    undone = call(runtime.use("DELETE"), path + "/boost")["data"]["post"]
    assert undone["boosts"] == 0 and undone["boosted"] is False
    unliked = call(runtime.use("DELETE"), path + "/favourite")["data"]["post"]
    assert unliked["favourites"] == 0
    assert [item["type"] for item in timeline(runtime)["posts"]] == ["post"]


def test_delete_own_or_as_admin():
    runtime = Runtime()
    mine = post(runtime, "mine")
    theirs = post(runtime, "theirs", actor="bob")
    call(runtime.use("POST", actor="bob"), "/api/instance-feed/posts/" + mine["id"] + "/boost")
    call(runtime.use("POST", actor="bob"), "/api/instance-feed/posts/" + mine["id"] + "/favourite")
    assert call(runtime.use("DELETE"), "/api/instance-feed/posts/" + theirs["id"])["status"] == 403
    assert call(runtime.use("DELETE"), "/api/instance-feed/posts/" + mine["id"])["data"]["deleted"]
    assert runtime.db.execute("SELECT COUNT(*) FROM instance_feed_posts WHERE boost_of=?",
                              (mine["id"],)).fetchone()[0] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM instance_feed_favourites").fetchone()[0] == 0
    assert timeline(runtime, actor="admin")["posts"][0]["canDelete"] is True
    assert call(runtime.use("DELETE", actor="admin"), "/api/instance-feed/posts/" + theirs["id"])["status"] == 200
    assert timeline(runtime)["posts"] == []


def test_profile_view_includes_replies_and_boosts():
    runtime = Runtime()
    root = post(runtime, "root", actor="bob")
    post(runtime, "my reply", replyTo=root["id"])
    call(runtime.use("POST"), "/api/instance-feed/posts/" + root["id"] + "/boost")
    post(runtime, "my post")
    data = timeline(runtime, actor="bob", author="Alice")
    assert [item["type"] for item in data["posts"]] == ["post", "boost", "post"]
    assert data["profile"]["name"] == "alice" and data["profile"]["displayName"] == "Alice"
    assert data["profile"]["posts"] == 2
    assert call(runtime.use("GET", query={"author": "bad name"}), "/api/instance-feed")["status"] == 404


def test_posting_burst_limit():
    runtime = Runtime()
    for index in range(instance_feed.BURST_LIMIT):
        post(runtime, "spam %d" % index)
    response = call(runtime.use("POST", body={"text": "one more"}), "/api/instance-feed/posts")
    assert response["status"] == 429


def test_export_and_purge_account_data():
    runtime = Runtime()
    mine = post(runtime, "keep a copy", spoiler="cw")
    theirs = post(runtime, "bob's", actor="bob")
    reply = post(runtime, "reply to alice", actor="bob", replyTo=mine["id"])
    call(runtime.use("POST"), "/api/instance-feed/posts/" + theirs["id"] + "/favourite")
    call(runtime.use("POST"), "/api/instance-feed/posts/" + theirs["id"] + "/boost")
    call(runtime.use("POST", actor="bob"), "/api/instance-feed/posts/" + mine["id"] + "/boost")
    exported = call(runtime.use("GET"), "/api/instance-feed/export")["data"]
    assert exported["filename"] == "forkmesh-instance-feed-alice.json"
    payload = exported["export"]
    assert [item["text"] for item in payload["posts"]] == ["keep a copy"]
    assert payload["posts"][0]["spoiler"] == "cw"
    assert [item["postId"] for item in payload["boosts"]] == [theirs["id"]]
    assert [item["postId"] for item in payload["favourites"]] == [theirs["id"]]
    assert asyncio.run(instance_feed.account_has_data(runtime, "bi:alice")) is True
    asyncio.run(instance_feed.purge_account_data(runtime, "bi:alice", "alice"))
    assert asyncio.run(instance_feed.account_has_data(runtime, "bi:alice")) is False
    data = timeline(runtime, actor="bob")
    assert [item["id"] for item in data["posts"]] == [theirs["id"]]
    assert data["posts"][0]["favourites"] == 0 and data["posts"][0]["boosts"] == 0
    orphan = call(runtime.use("GET", actor="bob"), "/api/instance-feed/posts/" + reply["id"])["data"]
    assert orphan["ancestors"] == [] and orphan["post"]["replyToAuthor"] == ""


def test_schema_down_reverses_up():
    runtime = Runtime()
    for statement in instance_feed.DATA_SCHEMA_UP:
        runtime.db.execute(statement)
    for statement in instance_feed.DATA_SCHEMA_DOWN:
        runtime.db.execute(statement)
    names = runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE '%instance_feed%'").fetchall()
    assert names == []


def test_install_and_uninstall_through_the_account_lifecycle(monkeypatch):
    sys.path.insert(0, str(REPO / "app" / "src"))
    sys.path.insert(0, str(REPO / "app" / "tools"))
    sys.path.insert(0, str(REPO / "marketplace" / "tools"))
    import build_app_plugins
    import plugin_data
    namespace = {}
    exec(build_app_plugins.registry_source(), namespace)
    monkeypatch.setattr(plugin_data, "REGISTRY", namespace["PLUGINS"])
    monkeypatch.setattr(plugin_data, "_module", lambda plugin: (
        instance_feed if plugin == "instance-feed" else None))
    monkeypatch.setattr(plugin_data, "_ACCOUNT_SCHEMA_READY", False)

    class Lifecycle(Runtime):
        _plugin = "instance-feed"

        async def account_plugin_installed(self, account_bi):
            return await plugin_data.account_package_installed(self, account_bi, self._plugin)

    runtime = Lifecycle()
    runtime.db.execute("CREATE TABLE org_plugin_installs (org_bi TEXT, plugin TEXT)")
    runtime.db.execute("CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT)")

    def lifecycle(method, actor, path):
        return asyncio.run(plugin_data.handle(runtime.use(method, actor=actor), path))

    assert timeline(runtime)["installed"] is False
    registry = plugin_data.REGISTRY
    monkeypatch.setattr(plugin_data, "REGISTRY", {
        key: value for key, value in registry.items() if key != "instance-feed"})
    unavailable = lifecycle("POST", "alice", "/api/plugins/@account/instance-feed/install")
    assert unavailable["status"] == 404
    assert unavailable["data"]["error"] == "plugin_not_found"
    monkeypatch.setattr(plugin_data, "REGISTRY", registry)
    account = lifecycle("GET", "alice", "/api/plugins/@account")
    entry = next(row for row in account["data"]["packages"] if row["id"] == "instance-feed")
    assert entry["installed"] is False
    assert entry.get("unchosen") is True
    for actor in ("alice", "bob"):
        assert lifecycle("POST", actor, "/api/plugins/@account/instance-feed/install")["status"] == 200
    account = lifecycle("GET", "alice", "/api/plugins/@account")
    entry = next(row for row in account["data"]["packages"] if row["id"] == "instance-feed")
    assert entry["installed"] is True
    assert "unchosen" not in entry
    mine = post(runtime, "hello")
    theirs = post(runtime, "hi alice", actor="bob", replyTo=mine["id"])
    call(runtime.use("POST", actor="bob"), "/api/instance-feed/posts/" + mine["id"] + "/favourite")
    assert lifecycle("POST", "alice", "/api/plugins/@account/instance-feed/uninstall")["status"] == 200
    assert timeline(runtime)["installed"] is False
    assert asyncio.run(instance_feed.account_has_data(runtime, "bi:alice")) is False
    account = lifecycle("GET", "alice", "/api/plugins/@account")
    entry = next(row for row in account["data"]["packages"] if row["id"] == "instance-feed")
    assert entry["installed"] is False
    assert "unchosen" not in entry
    remaining = timeline(runtime, actor="bob", author="bob")["posts"]
    assert [item["id"] for item in remaining] == [theirs["id"]]
    assert runtime.db.execute("SELECT COUNT(*) FROM instance_feed_favourites").fetchone()[0] == 0


def test_manifest_matches_worker_and_page():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["worker"] == {"module": "instance_feed", "api": instance_feed.PREFIX,
                                  "outboundOrigins": []}
    assert manifest["id"] == instance_feed.PLUGIN_ID
    assert manifest["dataLifecycle"]["scope"] == "account"
    page = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
    view = (PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
    assert 'registerPage("instance-feed"' in page and 'data-view="instance-feed"' in view
    assert view.count("fm-btn-cta") == 2
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    assert {"id": "instance-feed", "version": manifest["version"],
            "manifest": "packages/instance-feed/manifest.json"} in catalog["packages"]
