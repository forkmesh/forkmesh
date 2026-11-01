import asyncio
import base64
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives import serialization

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import forum_records as records  # noqa: E402
import urls  # noqa: E402

NOW = 1_800_000_000_000
NODE = shutil.which("node")
JS = ROOT / "public" / "dashboard" / "js" / "09-repo-forum.js"


def b64url(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


class Signer:
    def __init__(self):
        self.key = Ed25519PrivateKey.generate()
        self.pub = b64url(self.key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw))

    def event(self, kind, topic="", post="", content=None, ts=NOW, repo="ada/forum", number=[0]):
        number[0] += 1
        event = {"type": kind, "topic": topic, "post": post, "content": content or {}, "author": self.pub,
                 "ts": ts, "id": "%s-test%04d" % (kind, number[0]), "sig": ""}
        event["sig"] = b64url(self.key.sign(records.canonical(repo, event).encode("utf-8")))
        return event


class Repo:
    def __init__(self, files=None):
        self.files = dict(files or {})

    async def read(self, path):
        value = self.files.get(path)
        return json.loads(json.dumps(value)) if value is not None else None

    async def apply(self, event, account="ada", moderator=False, now=NOW):
        clean = records.clean_event(event, now)
        changes, result = await records.apply_event(
            self.read, clean, {"account": account, "moderator": moderator, "now": now, "repoTitle": "forum"})
        self.files.update(changes)
        return changes, result


def run(coroutine):
    return asyncio.run(coroutine)


@pytest.fixture
def ada():
    return Signer()


@pytest.fixture
def bob():
    return Signer()


def test_events_are_checked_before_anything_else(ada):
    good = ada.event("post", "n1", "", {"body": "hi"})
    assert records.clean_event(good, NOW)["type"] == "post"
    bad = [
        dict(good, type="explode"),
        dict(good, topic="../1"),
        dict(good, post="l1"),
        dict(good, ts=NOW - 3_600_000),
        dict(good, ts=True),
        dict(good, sig="short"),
        dict(good, author="x" * 10),
        dict(good, id="bad id"),
        dict(good, content={"body": ""}),
        dict(good, content={"body": "hi", "extra": 1}),
        dict(good, content={"body": "x" * 32001}),
        ada.event("topic", "n1", "", {"title": "t", "body": "b", "category": 1}),
        ada.event("like", "n1", "", {}),
        ada.event("topic", "", "", {"title": "t", "body": "b", "category": 1, "tags": ["a b"]}),
        ada.event("category", "", "", {"id": 0, "name": "x", "color": "red", "description": "", "parent": 0, "position": 0}),
        ada.event("silence", "", "", {"account": "Bob!", "days": 3}),
    ]
    for event in bad:
        with pytest.raises(records.RecordError):
            records.clean_event(event, NOW)


def test_a_new_forum_starts_with_its_first_topic_and_replies(ada, bob):
    repo = Repo()
    _changes, result = run(repo.apply(ada.event("topic", content={
        "title": "Hello world", "body": "First post", "category": 0, "tags": ["intro"]})))
    assert result == {"topic": "n1", "post": "l1"}
    forum = repo.files["forum.json"]
    assert forum["kind"] == "forkmesh-forum" and forum["source"]["type"] == "forkmesh"
    assert forum["counts"]["topics"] == 1 and forum["counts"]["posts"] == 1
    header = repo.files["topics/n1/topic.json"]
    assert header["title"] == "Hello world" and header["slug"] == "hello-world" and header["posts"] == []
    assert repo.files["topics/n1/local.json"]["posts"][0]["raw"] == "First post"
    assert [row["id"] for row in repo.files["latest/1.json"]["rows"]] == ["n1"]
    category = records.NATIVE_CATEGORY_BASE
    assert [row["id"] for row in repo.files["c/%d/1.json" % category]["rows"]] == ["n1"]
    assert repo.files["categories.json"][0]["topic_count"] == 1
    assert repo.files["tag/intro/1.json"]["rows"][0]["id"] == "n1"
    events = [path for path in repo.files if path.startswith("events/")]
    assert len(events) == 1 and repo.files[events[0]]["authorName"] == "ada"

    _changes, result = run(repo.apply(bob.event("post", "n1", "", {"body": "Welcome!", "replyTo": "l1"}), account="bob"))
    assert result == {"topic": "n1", "post": "l2"}
    posts = repo.files["topics/n1/local.json"]["posts"]
    assert [post["key"] for post in posts] == ["l1", "l2"] and posts[1]["reply_to"] == "l1"
    row = repo.files["native.json"]["topics"]["n1"]
    assert row["posts_count"] == 2 and row["reply_count"] == 1 and row["last_poster_username"] == "bob"
    assert [poster["username"] for poster in row["posters"]] == ["ada", "bob"]
    assert repo.files["forum.json"]["counts"]["posts"] == 2


def test_likes_edits_and_deletes_respect_who_wrote_what(ada, bob):
    repo = Repo()
    run(repo.apply(ada.event("topic", content={"title": "T", "body": "B", "category": 0})))
    run(repo.apply(bob.event("post", "n1", "", {"body": "reply"}), account="bob"))
    run(repo.apply(bob.event("like", "n1", "l1"), account="bob"))
    assert repo.files["topics/n1/likes.json"] == {"l1": ["bob"]}
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(ada.event("like", "n1", "l1")))
    assert error.value.code == "own_post"
    run(repo.apply(bob.event("unlike", "n1", "l1"), account="bob"))
    assert repo.files["topics/n1/likes.json"] == {}
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(ada.event("edit", "n1", "l2", {"body": "hijack"})))
    assert error.value.code == "not_yours"
    run(repo.apply(bob.event("edit", "n1", "l2", {"body": "better reply"}), account="bob"))
    assert repo.files["topics/n1/local.json"]["posts"][1]["raw"] == "better reply"
    run(repo.apply(ada.event("edit", "n1", "l1", {"title": "Renamed"})))
    assert repo.files["topics/n1/topic.json"]["title"] == "Renamed"
    assert repo.files["latest/1.json"]["rows"][0]["title"] == "Renamed"
    run(repo.apply(bob.event("delete", "n1", "l2"), account="bob"))
    assert repo.files["topics/n1/local.json"]["posts"][1]["deleted"] is True
    run(repo.apply(ada.event("delete", "n1", "l1")))
    assert repo.files["native.json"]["topics"]["n1"]["deleted"] is True
    assert repo.files["latest/1.json"]["rows"] == []


def test_operators_pin_close_move_hide_and_silence(ada, bob):
    repo = Repo()
    run(repo.apply(ada.event("topic", content={"title": "T", "body": "B", "category": 0})))
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(bob.event("close", "n1"), account="bob"))
    assert error.value.code == "not_moderator"
    run(repo.apply(ada.event("pin", "n1", "", {"globally": True}), moderator=True))
    assert repo.files["latest/1.json"]["rows"][0]["pinned_globally"] is True
    run(repo.apply(ada.event("close", "n1"), moderator=True))
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(bob.event("post", "n1", "", {"body": "late"}), account="bob"))
    assert error.value.code == "topic_closed"
    run(repo.apply(ada.event("post", "n1", "", {"body": "operator note"}), moderator=True))
    run(repo.apply(ada.event("category", "", "", {"id": 0, "name": "News", "color": "e45735", "description": "d",
                                                   "parent": 0, "position": 1}), moderator=True))
    news = records.NATIVE_CATEGORY_BASE + 1
    assert any(item["id"] == news and item["color"] == "E45735" for item in repo.files["categories.json"])
    assert repo.files["local.json"]["categories"][str(news)]["name"] == "News"
    run(repo.apply(ada.event("move", "n1", "", {"category": news}), moderator=True))
    assert repo.files["native.json"]["topics"]["n1"]["category_id"] == news
    assert repo.files["c/%d/1.json" % news]["rows"][0]["id"] == "n1"
    assert repo.files["c/%d/1.json" % records.NATIVE_CATEGORY_BASE]["rows"] == []
    run(repo.apply(ada.event("hide", "n1", "l2"), moderator=True))
    assert repo.files["topics/n1/state.json"]["hidden"] == ["l2"]
    run(repo.apply(ada.event("unlist", "n1"), moderator=True))
    assert repo.files["latest/1.json"]["rows"] == []
    run(repo.apply(ada.event("silence", "", "", {"account": "bob", "days": 2}), moderator=True))
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(bob.event("topic", content={"title": "x", "body": "y", "category": 0}), account="bob"))
    assert error.value.code == "silenced"
    run(repo.apply(ada.event("unsilence", "", "", {"account": "bob"}), moderator=True))
    run(repo.apply(bob.event("topic", content={"title": "x", "body": "y", "category": news}), account="bob"))
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(ada.event("category-delete", "", "", {"id": news}), moderator=True))
    assert error.value.code == "category_in_use"
    run(repo.apply(ada.event("site", "", "", {"title": "Our forum", "description": "Talk"}), moderator=True))
    assert repo.files["forum.json"]["title"] == "Our forum" and repo.files["local.json"]["title"] == "Our forum"


def test_replies_to_an_imported_topic_live_beside_it(ada):
    header = {"id": 11, "title": "Imported", "slug": "imported", "category_id": 2, "tags": [], "bumped_at": "2026-01-01T00:00:00.000Z",
              "posts_count": 5, "reply_count": 4, "views": 10, "like_count": 1, "pinned": False, "pinned_globally": False,
              "closed": False, "archived": False, "visible": True, "posters": [{"username": "alice"}],
              "posts": [{"post_number": 1, "cooked": "<p>x</p>"}], "chunks": 1}
    repo = Repo({
        "forum.json": records.new_forum("Imported forum") | {"source": {"type": "discourse", "origin": "https://forum.example.org"}},
        "categories.json": [{"id": 2, "name": "Other", "parent_category_id": 1}, {"id": 1, "name": "World", "parent_category_id": None}],
        "topics/11/topic.json": header,
        "latest/1.json": {"page": 1, "pages": 3, "more": True, "rows": [{"id": 12, "bumped_at": "2026-02-01"}, {"id": 11}]},
    })
    _changes, result = run(repo.apply(ada.event("post", "11", "", {"body": "from ForkMesh"})))
    assert result["post"] == "l1"
    state = repo.files["topics/11/state.json"]
    assert state["local_posts"] == 1 and state["last_poster_username"] == "ada"
    latest = repo.files["latest/1.json"]
    assert [row["id"] for row in latest["rows"]] == [11, 12] and latest["pages"] == 3 and latest["more"] is True
    assert latest["rows"][0]["posts_count"] == 6
    assert [row["id"] for row in repo.files["c/2/1.json"]["rows"]] == [11]
    assert [row["id"] for row in repo.files["c/1/1.json"]["rows"]] == [11]
    assert "topics/11/topic.json" not in _changes
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(ada.event("edit", "11", "1", {"body": "rewrite"})))
    assert error.value.code == "not_editable"
    run(repo.apply(ada.event("delete", "11", "1"), moderator=True))
    assert repo.files["topics/11/state.json"]["deleted"] == ["1"]


def test_the_same_event_is_saved_once(ada):
    repo = Repo()
    event = ada.event("topic", content={"title": "T", "body": "B", "category": 0})
    run(repo.apply(event))
    with pytest.raises(records.RecordError) as error:
        run(repo.apply(event))
    assert error.value.code == "duplicate"


def test_a_native_forum_keeps_every_list_page_exact(ada, monkeypatch):
    monkeypatch.setattr(records, "PAGE_SIZE", 2)
    repo = Repo()
    for number in range(5):
        run(repo.apply(ada.event("topic", content={"title": "T%d" % number, "body": "B", "category": 0},
                                 ts=NOW + number), now=NOW + number * 1000))
    assert [row["id"] for row in repo.files["latest/1.json"]["rows"]] == ["n5", "n4"]
    assert [row["id"] for row in repo.files["latest/3.json"]["rows"]] == ["n1"]
    assert repo.files["latest/1.json"]["pages"] == 3


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_the_browser_signs_exactly_what_the_server_verifies(tmp_path):
    script = tmp_path / "sign.js"
    script.write_text(r"""
const fs = require("fs");
const { webcrypto } = require("crypto");
const source = fs.readFileSync(process.argv[2], "utf8");
const api = new Function("crypto", source + "; return { forumContentText, forumCanonical };")(webcrypto);
(async () => {
  const pair = await webcrypto.subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]);
  const raw = Buffer.from(await webcrypto.subtle.exportKey("raw", pair.publicKey)).toString("base64url");
  const content = { title: "Блокировка «сайтов» & \"quotes\"\n", body: "line\u0001\tend 😀", category: 1000000, tags: ["dpi", "ру"] };
  const event = { type: "topic", topic: "", post: "", content, author: raw, ts: 1800000000000 };
  const digest = Buffer.from(await webcrypto.subtle.digest("SHA-256", new TextEncoder().encode(api.forumContentText(content)))).toString("hex");
  const message = api.forumCanonical({ owner: "Ada", name: "Forum" }, event, digest);
  const sig = Buffer.from(await webcrypto.subtle.sign({ name: "Ed25519" }, pair.privateKey, new TextEncoder().encode(message))).toString("base64url");
  process.stdout.write(JSON.stringify({ ...event, sig, id: "topic-test-0001" }));
})();
""", encoding="utf-8")
    result = subprocess.run([NODE, str(script), str(JS)], capture_output=True, text=True, check=True)
    event = json.loads(result.stdout)
    clean = records.clean_event(event, NOW)
    message = records.canonical("ada/forum", clean).encode("utf-8")
    key = Ed25519PublicKey.from_public_bytes(base64.urlsafe_b64decode(event["author"] + "=="))
    key.verify(base64.urlsafe_b64decode(event["sig"] + "=="), message)


class FakeServer:
    def __init__(self, signer):
        self.signer = signer
        self.repo = Repo()
        self.features = {"forum": True}
        self.session = "ada"
        self.moderators = {"ada"}
        self.commits = []
        self.gate = (None, {})
        self.audits = []
        self.conflicts = 0

    def bind(self, monkeypatch):
        fake = self

        async def features(env, owner, repo):
            return dict(fake.features)

        async def bind_author(env, request, data, record):
            record["authorName"] = fake.session
            record["authorVerified"] = bool(fake.session)

        async def verify(author, sig, message):
            try:
                Ed25519PublicKey.from_public_bytes(base64.urlsafe_b64decode(author + "==")).verify(
                    base64.urlsafe_b64decode(sig + "=="), message)
            except Exception:
                return False
            return True

        class Moderation:
            @staticmethod
            async def can_moderate_repo(env, account, owner, repo):
                return account in fake.moderators

            @staticmethod
            async def submission_gate(env, request, data, owner, repo, event):
                return fake.gate

        async def read_forum(env, owner, repo, branch):
            return "a" * 40, fake.repo.read

        async def commit(env, owner, repo, branch, commit, changes, account, message):
            if fake.conflicts:
                fake.conflicts -= 1
                raise records.Conflict()
            fake.repo.files.update(changes)
            fake.commits.append((account, message, sorted(changes)))
            return "b" * 40

        async def audit(env, actor, action, kind, target, outcome, details):
            fake.audits.append(action)

        class _Date:
            @staticmethod
            def now():
                return NOW

        for name, value in {"_repo_features_get": features, "_bind_session_author": bind_author,
                            "ed25519_verify": verify, "_moderation": Moderation, "Date": _Date,
                            "_audit_sensitive_action": audit}.items():
            monkeypatch.setitem(records.__dict__, name, value)
        monkeypatch.setattr(records, "read_forum", read_forum)
        monkeypatch.setattr(records, "commit_records", commit)
        return self


def submit(event, **data):
    return run(records.submit(None, None, "Ada", "Forum", {"branch": "main", "event": event, **data}))


def test_submit_verifies_the_signature_and_commits(ada, monkeypatch):
    server = FakeServer(ada).bind(monkeypatch)
    event = ada.event("topic", content={"title": "Hello", "body": "World", "category": 0})
    result = submit(event)
    assert result["ok"] and result["topic"] == "n1" and result["commit"] == "b" * 40
    account, message, paths = server.commits[0]
    assert account == "ada" and message == "Forum: New topic in n1 by @ada"
    assert "topics/n1/local.json" in paths and any(path.startswith("events/") for path in paths)
    tampered = dict(ada.event("post", "n1", "", {"body": "x"}), content={"body": "y"})
    with pytest.raises(records.RecordError) as error:
        submit(tampered)
    assert error.value.code == "bad_signature"


def test_submit_refuses_without_the_forum_or_a_session_and_follows_the_gate(ada, monkeypatch):
    server = FakeServer(ada).bind(monkeypatch)
    server.features = {"forum": False}
    with pytest.raises(records.RecordError) as error:
        submit(ada.event("topic", content={"title": "x", "body": "y", "category": 0}))
    assert error.value.code == "forum_off"
    server.features = {"forum": True}
    server.session = ""
    with pytest.raises(records.RecordError) as error:
        submit(ada.event("topic", content={"title": "x", "body": "y", "category": 0}))
    assert error.value.status == 401
    server.session = "bob"
    server.gate = ({"error": "account_blocked"}, {})
    with pytest.raises(records.RecordError) as error:
        submit(ada.event("topic", content={"title": "x", "body": "y", "category": 0}))
    assert error.value.code == "refused"
    server.gate = (None, {"hold": True})
    with pytest.raises(records.RecordError) as error:
        submit(ada.event("topic", content={"title": "x", "body": "y", "category": 0}))
    assert error.value.code == "approval_required"
    server.gate = (None, {})
    server.conflicts = 2
    assert submit(ada.event("topic", content={"title": "x", "body": "y", "category": 0}))["ok"]
    server.conflicts = 3
    with pytest.raises(records.RecordError) as error:
        submit(ada.event("topic", content={"title": "z", "body": "y", "category": 0}))
    assert error.value.code == "busy"


def test_operator_actions_are_audited(ada, monkeypatch):
    server = FakeServer(ada).bind(monkeypatch)
    submit(ada.event("topic", content={"title": "x", "body": "y", "category": 0}))
    submit(ada.event("close", "n1"))
    assert server.audits == ["forum.close"]


def test_the_forum_api_is_routed():
    assert urls.REPO_FORUM_RE.match("/api/repo/ada/forum/forum").groups() == ("ada", "forum")
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    assert "forum_match = REPO_FORUM_RE.match(url.path)" in runtime
    assert '"REPO_FORUM_RE"' in (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    source = (ROOT / "src" / "forum_records.py").read_text(encoding="utf-8")
    assert 'credential="forum-record"' in source and 'FORUM_PATH + "/" + path' in source


def test_a_truncated_file_or_folder_stops_the_write_instead_of_losing_data(monkeypatch):
    commit = "a" * 40
    tree = {"": {"commit": commit, "entries": [{"name": ".forkmesh"}]},
            ".forkmesh": {"commit": commit, "entries": [{"name": "forum"}]},
            ".forkmesh/forum": {"commit": commit, "entries": [{"name": "topics"}]},
            ".forkmesh/forum/topics": {"commit": commit, "entries": [{"name": "1"}], "truncated": True}}
    blobs = {".forkmesh/forum/topics/1/local.json": {"content": '{"posts": [', "truncated": True}}

    async def fake_tree(env, owner, repo, path, ref):
        return tree.get(path)

    async def fake_blob(env, owner, repo, path, ref):
        return blobs.get(path)

    monkeypatch.setattr(records.edits, "_tree", fake_tree)
    monkeypatch.setattr(records.edits, "_blob", fake_blob)
    _commit, read = run(records.read_forum(None, "ada", "forum", "main"))
    assert run(read("topics/9/local.json")) is None
    with pytest.raises(records.RecordError) as error:
        run(read("topics/1/local.json"))
    assert error.value.code == "forum_unreadable"
    with pytest.raises(records.RecordError) as error:
        run(records.forum_listings(None, "ada", "forum", commit, [".forkmesh/forum/topics/1/local.json"]))
    assert error.value.code == "forum_unreadable"
    listings = run(records.forum_listings(None, "ada", "forum", commit, [".forkmesh/forum/new/a.json"]))
    assert listings[".forkmesh/forum/new"] == [] and listings[".forkmesh/forum"] == [{"name": "topics"}]
    with pytest.raises(records.RecordError) as error:
        records.check_sizes({"topics/1/local.json": {"posts": ["x" * records.MAX_FILE_BYTES]}})
    assert error.value.code == "forum_full"


def test_mirrors_list_forum_folders_like_other_collaboration_records():
    source = (ROOT.parent / "tools" / "mirror_gateway.py").read_text(encoding="utf-8")
    roots = source.split("COLLABORATION_TREE_ROOTS = frozenset({", 1)[1].split("})", 1)[0]
    assert '".forkmesh/forum"' in roots
