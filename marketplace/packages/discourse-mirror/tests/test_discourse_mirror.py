import importlib.util
import json
import subprocess
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
TOOL = PACKAGE / "content" / "mirror" / "discourse_mirror.py"
SPEC = importlib.util.spec_from_file_location("discourse_mirror", TOOL)
dm = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dm)

PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 64


def post(topic_id, number, username, cooked, **extra):
    return {
        "id": topic_id * 100 + number, "post_number": number, "post_type": 1, "username": username,
        "name": username.title(), "created_at": f"2026-01-0{min(number, 9)}T10:00:00.000Z",
        "updated_at": f"2026-01-0{min(number, 9)}T10:00:00.000Z", "cooked": cooked,
        "avatar_template": "/user_avatar/{size}.png", "reads": 4, "reply_count": 0,
        "actions_summary": [{"id": 2, "count": number}], **extra,
    }


class FakeForum:
    def __init__(self):
        self.robots = "User-agent: *\nDisallow: /admin/\n"
        self.requests = []
        self.rate_limited = 0
        self.block_topics = False
        self.page_size = 2
        self.first_posts = 2
        self.categories = [
            {"id": 1, "name": "General", "slug": "general", "color": "0088CC",
             "description_text": "Everything else", "position": 0},
            {"id": 2, "name": "Help", "slug": "help", "parent_category_id": 1, "position": 1},
        ]
        self.topics = {}

    def add_topic(self, topic_id, title, posts, category_id=1, pinned=False, bumped="2026-01-05T10:00:00.000Z"):
        self.topics[topic_id] = {"id": topic_id, "title": title, "slug": title.lower().replace(" ", "-"),
                                 "category_id": category_id, "pinned": pinned, "bumped_at": bumped,
                                 "tags": [{"id": 1, "name": "tips", "slug": "tips"}], "posts": posts}

    def listing_row(self, topic):
        return {key: topic[key] for key in ("id", "title", "slug", "category_id", "pinned", "bumped_at", "tags")} | {
            "posts_count": len(topic["posts"]),
            "highest_post_number": max(p["post_number"] for p in topic["posts"]),
            "created_at": topic["posts"][0]["created_at"], "last_posted_at": topic["bumped_at"],
            "views": 10 * topic["id"], "like_count": len(topic["posts"]), "reply_count": len(topic["posts"]) - 1,
            "pinned_globally": topic["pinned"], "visible": topic.get("visible", True),
            "excerpt": "Rules &amp; <b>tips</b>&hellip;" if topic["pinned"] else "",
            "last_poster_username": topic["posts"][-1]["username"],
            "posters": [{"user_id": self.user_id(p["username"]), "description": "Original Poster" if i == 0 else "Frequent Poster"}
                        for i, p in enumerate(topic["posts"][:5])],
        }

    def user_id(self, username):
        return sum(ord(c) for c in username)

    def answer(self, path, query):
        if path == "/robots.txt":
            return 200, "text/plain", self.robots.encode()
        if path == "/site.json":
            return 200, "application/json", json.dumps({"categories": self.categories}).encode()
        if path == "/about.json":
            posts = sum(len(t["posts"]) for t in self.topics.values())
            return 200, "application/json", json.dumps({"about": {
                "title": "Example Forum", "description": "A forum for tests",
                "stats": {"topics_count": len(self.topics), "posts_count": posts}}}).encode()
        if path == "/latest.json":
            if self.rate_limited:
                self.rate_limited -= 1
                return 429, "application/json", b'{"errors":["slow down"]}'
            ordered = sorted(self.topics.values(), key=lambda t: (not t["pinned"], t["bumped_at"]), reverse=False)
            ordered = [t for t in ordered if t["pinned"]] + sorted(
                (t for t in ordered if not t["pinned"]), key=lambda t: t["bumped_at"], reverse=True)
            page = int(query.get("page", ["0"])[0])
            chunk = ordered[page * self.page_size:(page + 1) * self.page_size]
            more = (page + 1) * self.page_size < len(ordered)
            names = {p["username"] for t in chunk for p in t["posts"]}
            body = {"users": [{"id": self.user_id(n), "username": n, "name": n.title()} for n in sorted(names)],
                    "topic_list": {"topics": [self.listing_row(t) for t in chunk]}}
            if more:
                body["topic_list"]["more_topics_url"] = f"/latest?page={page + 1}"
            return 200, "application/json", json.dumps(body).encode()
        parts = path.strip("/").split("/")
        if parts[0] == "t":
            if self.block_topics:
                return 403, "text/html", b"<html>checking your browser</html>"
            topic_id = int(parts[1].removesuffix(".json"))
            topic = self.topics.get(topic_id)
            if topic is None:
                return 404, "application/json", b'{"errors":["not found"],"error_type":"not_found"}'
            if len(parts) == 3 and parts[2] == "posts.json":
                wanted = {int(v) for v in query.get("post_ids[]", [])}
                posts = [p for p in topic["posts"] if p["id"] in wanted]
                return 200, "application/json", json.dumps({"post_stream": {"posts": posts}}).encode()
            body = self.listing_row(topic) | {"details": {
                "participants": [{"username": "alice"}], "links": [{"url": "https://example.org"}]}, "post_stream": {
                "posts": topic["posts"][:self.first_posts], "stream": [p["id"] for p in topic["posts"]]}}
            return 200, "application/json", json.dumps(body).encode()
        if parts[0] == "uploads" and path.endswith(".png"):
            return 200, "image/png", PNG
        if parts[0] == "uploads" and path.endswith(".svg"):
            return 200, "image/svg+xml", b"<svg onload=alert(1)></svg>"
        return 404, "application/json", b'{"errors":["not found"]}'


@pytest.fixture
def forum(tmp_path, monkeypatch):
    for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        monkeypatch.setenv(name, str(tmp_path / "home"))
    monkeypatch.delenv("FORKMESH_HOME", raising=False)
    monkeypatch.delenv("FORKMESH_PROJECTS_DIR", raising=False)
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    monkeypatch.setenv("FORKMESH_DISCOURSE_MIRROR_QUIET", "1")
    fake = FakeForum()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            fake.requests.append(self.path)
            status, kind, body = fake.answer(parsed.path, urllib.parse.parse_qs(parsed.query))
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            if status == 429:
                self.send_header("Retry-After", "1")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    fake.origin = f"http://127.0.0.1:{server.server_address[1]}"
    fake.config = dm.Config(tmp_path / "data")
    fake.entry = {"name": "example", "origin": fake.origin, "delaySeconds": 0, "revisitPerRun": 0}
    yield fake
    server.shutdown()


def seed(fake):
    origin = fake.origin
    fake.add_topic(10, "Welcome", [post(10, 1, "admin", "<p>Read the rules.</p>")], pinned=True)
    fake.add_topic(11, "Setting up", [
        post(11, 1, "alice", f'<p>See <a href="{origin}/t/welcome/10">the rules</a> and '
                             f'<a href="/t/welcome/10/1">post one</a>.</p>'
                             f'<p><img src="/uploads/default/shot.png" alt="screenshot" width="40" height="20" onerror="x()"></p>'
                             f'<p><img src="https://other.example/pic.png" alt="elsewhere"></p>'
                             '<script>alert(1)</script><p onclick="steal()">Hi <a href="javascript:alert(1)">x</a></p>'),
        post(11, 2, "bob", "<p>Thanks</p>", reply_to_post_number=1),
        post(11, 3, "carol", "<p>Same here</p>"),
        post(11, 4, "dave", "<p><img src=\"/uploads/default/logo.svg\" alt=\"logo\"></p>"),
        post(11, 5, "erin", "<aside class=\"quote\"><blockquote><p>quoted</p></blockquote></aside>"),
    ], category_id=2, bumped="2026-01-06T10:00:00.000Z")
    fake.add_topic(12, "Old news", [post(12, 1, "frank", "<p>Archive</p>")], bumped="2026-01-02T10:00:00.000Z")


def sync(fake, sleeps=None, **overrides):
    entry = {**fake.entry, **overrides}
    forum = fake.config.resolved(entry)
    mirror = dm.Mirror(forum, sleep=(sleeps.append if sleeps is not None else (lambda _: None)))
    return mirror.sync(full=overrides.get("full", False), max_topics=overrides.get("max_topics")), Path(forum["repository"])


def git_log(repository):
    return subprocess.run(["git", "-C", str(repository), "log", "--format=%s"], capture_output=True,
                          text=True, check=True).stdout.splitlines()


def test_first_copy_writes_data_pages_and_a_commit(forum):
    seed(forum)
    result, repo = sync(forum)
    assert result["error"] is None and result["fetched"] == 3 and result["pending"] == 0
    topic = json.loads((repo / "topics" / "11.json").read_text())
    assert [p["post_number"] for p in topic["posts"]] == [1, 2, 3, 4, 5]
    assert "avatar_template" not in topic["posts"][0]
    assert any("/t/11/posts.json" in path and "post_ids" in path for path in forum.requests)
    meta = json.loads((repo / "mirror.json").read_text())
    assert meta["title"] == "Example Forum" and meta["topics"] == 3 and meta["posts"] == 7
    assert meta["source"] == forum.origin
    page = (repo / "site" / "t" / "11.html").read_text()
    assert "<script" not in page and "onclick" not in page and "onerror" not in page
    assert "javascript:" not in page and "alert" not in page
    assert 'href="10.html"' in page and 'href="10.html#p1"' in page
    uploads = json.loads((repo / "uploads" / "index.json").read_text())
    local = uploads[f"{forum.origin}/uploads/default/shot.png"]
    assert local and (repo / "uploads" / local).read_bytes() == PNG
    assert f'src="../../uploads/{local}"' in page and 'width="40"' in page
    assert uploads[f"{forum.origin}/uploads/default/logo.svg"] is None
    assert "https://other.example/pic.png" not in uploads
    assert 'class="remote-image" href="https://other.example/pic.png"' in page and "<img" in page
    assert page.count("<img") == 1
    assert "Content-Security-Policy" in page and 'href="#p1"' in page
    index = (repo / "site" / "index.html").read_text()
    assert "General" in index and "Help" in index and "Setting up" in index
    assert (repo / "site" / "c" / "2.html").read_text().count("Setting up") == 1
    assert (repo / "README.md").read_text().startswith("# Example Forum")
    assert git_log(repo) == ["Mirror Example Forum: 3 topics updated"]
    assert result["commit"]


def test_a_later_run_fetches_only_what_changed(forum):
    seed(forum)
    sync(forum)
    forum.requests.clear()
    result, repo = sync(forum)
    assert result["fetched"] == 0 and result["commit"] == ""
    assert not any(path.startswith("/t/") for path in forum.requests)
    forum.topics[12]["posts"].append(post(12, 2, "gina", "<p>Bump</p>"))
    forum.topics[12]["bumped_at"] = "2026-01-09T10:00:00.000Z"
    forum.requests.clear()
    result, repo = sync(forum)
    assert result["fetched"] == 1
    assert [path for path in forum.requests if path.startswith("/t/")] == ["/t/12.json"]
    assert len(json.loads((repo / "topics" / "12.json").read_text())["posts"]) == 2
    assert len(git_log(repo)) == 2


def test_revisits_drop_topics_the_forum_removed_and_their_pictures(forum):
    seed(forum)
    _, repo = sync(forum)
    local = json.loads((repo / "uploads" / "index.json").read_text())[f"{forum.origin}/uploads/default/shot.png"]
    del forum.topics[11]
    result, repo = sync(forum, revisitPerRun=10)
    assert result["removed"] == 1 and result["error"] is None
    assert not (repo / "topics" / "11.json").exists()
    assert not (repo / "site" / "t" / "11.html").exists()
    assert not (repo / "uploads" / local).exists()
    assert "11" not in {str(row["id"]) for row in json.loads((repo / "topics" / "index.json").read_text())}


def test_a_blocked_answer_never_removes_topics(forum):
    seed(forum)
    sync(forum)
    forum.block_topics = True
    result, repo = sync(forum, revisitPerRun=10)
    assert result["removed"] == 0 and "without removing" in result["error"]
    assert sorted(p.name for p in (repo / "topics").glob("1*.json")) == ["10.json", "11.json", "12.json"]


def test_a_full_walk_checks_topics_missing_from_the_list(forum):
    seed(forum)
    sync(forum)
    del forum.topics[12]
    result, repo = sync(forum, full=True)
    assert result["removed"] == 1 and not (repo / "topics" / "12.json").exists()


def test_a_capped_run_leaves_the_rest_for_the_next(forum):
    seed(forum)
    result, repo = sync(forum, max_topics=1)
    assert result["fetched"] == 1 and result["pending"] == 2
    assert "2 still to copy" in git_log(repo)[0]
    result, repo = sync(forum)
    assert result["fetched"] == 2 and result["pending"] == 0
    assert 'href="10.html"' in (repo / "site" / "t" / "11.html").read_text()


def forum_checkout(fake, tmp_path):
    checkout = tmp_path / "project"
    checkout.mkdir()
    for args in (["init", "-q", "-b", "main"], ["-c", "user.name=t", "-c", "user.email=t@example.org",
                                                "commit", "-q", "--allow-empty", "-m", "start"]):
        subprocess.run(["git", "-C", str(checkout), *args], check=True, capture_output=True)
    (checkout / "work.txt").write_text("unsaved work")
    return checkout


def test_the_forum_layout_writes_one_read_views_into_a_repository(forum, tmp_path):
    seed(forum)
    forum.topics[12]["tags"] = [{"name": "новости"}]
    forum.first_posts = 1
    checkout = forum_checkout(forum, tmp_path)
    result, repo = sync(forum, layout="forum", repository=str(checkout))
    assert result["error"] is None and result["fetched"] == 3
    root = checkout / ".forkmesh" / "forum"
    meta = json.loads((root / "forum.json").read_text())
    assert meta["kind"] == "forkmesh-forum" and meta["format"] == 1 and meta["title"] == "Example Forum"
    assert meta["source"] == {"type": "discourse", "origin": forum.origin, "lastSync": meta["source"]["lastSync"],
                              "pending": 0}
    assert meta["counts"] == {"topics": 3, "posts": 7, "categories": 2}
    categories = {item["id"]: item for item in json.loads((root / "categories.json").read_text())}
    assert categories[1]["subcategory_ids"] == [2] and categories[1]["topic_count"] == 2
    assert categories[1]["total_topic_count"] == 3 and categories[2]["topic_count"] == 1
    assert [row["id"] for row in categories[1]["latest"]] == [11, 10, 12]
    latest = json.loads((root / "latest" / "1.json").read_text())
    assert [row["id"] for row in latest["rows"]] == [10, 11, 12] and latest["pages"] == 1
    assert latest["rows"][0]["excerpt"] == "Rules & tips\u2026"
    row = latest["rows"][1]
    assert row["posters"][0] == {"username": "alice", "name": "Alice", "description": "Original Poster"}
    assert row["views"] == 110 and row["like_count"] == 5 and row["reply_count"] == 4
    assert "fetched_at" not in row and row["origin"] == "discourse"
    assert [r["id"] for r in json.loads((root / "c" / "2" / "1.json").read_text())["rows"]] == [11]
    tags = json.loads((root / "tags.json").read_text())
    assert {tag["name"]: tag["key"] for tag in tags} == {"tips": "tips", "новости": "новости"}
    assert [r["id"] for r in json.loads((root / "tag" / "новости" / "1.json").read_text())["rows"]] == [12]
    assert json.loads((root / "top" / "all.json").read_text())["rows"][0]["id"] == 11
    topic = json.loads((root / "topics" / "11" / "topic.json").read_text())
    assert topic["chunks"] == 1 and len(topic["posts"]) == 5 and topic["link_count"] == 1
    assert topic["posts"][1]["like_count"] == 2 and topic["posts"][0]["format"] == "html"
    shot = topic["uploads"][f"{forum.origin}/uploads/default/shot.png"]
    assert (root / "uploads" / shot).read_bytes() == PNG
    assert not (checkout / "site").exists() and not (checkout / "README.md").exists()
    assert (checkout / "work.txt").read_text() == "unsaved work"
    status = subprocess.run(["git", "-C", str(checkout), "status", "--porcelain"], capture_output=True,
                            text=True, check=True).stdout
    assert status.strip() == "?? work.txt"
    assert git_log(checkout)[0] == "Mirror Example Forum: 3 topics updated"


def test_the_forum_layout_splits_long_topics_and_removes_what_left(forum, tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "FORUM_POSTS_PER_CHUNK", 2)
    monkeypatch.setattr(dm, "FORUM_PAGE_SIZE", 2)
    seed(forum)
    checkout = forum_checkout(forum, tmp_path)
    _, _ = sync(forum, layout="forum", repository=str(checkout))
    root = checkout / ".forkmesh" / "forum"
    topic = json.loads((root / "topics" / "11" / "topic.json").read_text())
    assert topic["chunks"] == 3 and [p["post_number"] for p in topic["posts"]] == [1, 2]
    assert [p["post_number"] for p in json.loads((root / "topics" / "11" / "posts" / "2.json").read_text())["posts"]] == [5]
    assert json.loads((root / "latest" / "2.json").read_text())["rows"][0]["id"] == 12
    del forum.topics[11]
    forum.categories = forum.categories[:1]
    result, _ = sync(forum, layout="forum", repository=str(checkout), revisitPerRun=10)
    assert result["removed"] == 1
    assert not (root / "topics" / "11").exists() and not (root / "c" / "2").exists()
    assert not (root / "latest" / "2.json").exists()
    assert not (root / "tag" / "tips" / "2.json").exists()


def test_the_forum_layout_needs_an_existing_checkout(forum, tmp_path):
    seed(forum)
    with pytest.raises(dm.MirrorError, match="not a Git checkout"):
        sync(forum, layout="forum", repository=str(tmp_path / "missing"))


def test_ci_mode_reads_its_settings_from_the_repository_and_commits_the_forum(forum, tmp_path, capsys):
    seed(forum)
    checkout = forum_checkout(forum, tmp_path)
    settings = checkout / ".forkmesh" / "forum" / "source" / "settings.json"
    settings.parent.mkdir(parents=True)
    settings.write_text(json.dumps({"origin": forum.origin, "uploads": False, "delaySeconds": 0,
                                    "ignoreRobots": True, "maxTopicsPerRun": 2}))
    sleeps = []
    original = dm.Client.__init__

    def recording(self, entry, sleep=None):
        original(self, entry, sleep=sleeps.append)
        assert entry["delaySeconds"] == 1.0 and not entry.get("ignoreRobots")

    dm.Client.__init__ = recording
    try:
        assert dm.main(["ci", "--workspace", str(checkout), "--max-minutes", "5"]) == 0
    finally:
        dm.Client.__init__ = original
    out = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert out["fetched"] == 2 and out["pending"] == 1 and out["uploads"] == 0
    root = checkout / ".forkmesh" / "forum"
    assert json.loads((root / "forum.json").read_text())["counts"]["topics"] == 2
    assert git_log(checkout)[0].endswith("2 topics updated, 1 still to copy")
    assert (checkout / "work.txt").read_text() == "unsaved work"


def test_ci_mode_needs_settings(tmp_path, capsys):
    assert dm.main(["ci", "--workspace", str(tmp_path)]) == 1
    assert "does not name a forum" in capsys.readouterr().err


def test_a_run_stops_at_its_time_limit_and_keeps_what_it_copied(forum, tmp_path):
    seed(forum)
    checkout = forum_checkout(forum, tmp_path)
    entry = forum.config.resolved({**forum.entry, "layout": "forum", "repository": str(checkout)})
    mirror = dm.Mirror(entry, sleep=lambda _: None)
    mirror.deadline = 0.0
    result = mirror.sync()
    assert result["fetched"] == 0 and result["stoppedAtTimeLimit"] and result["pending"] == 3
    assert result["error"] is None and result["commit"]


def test_the_fold_keeps_forkmesh_topics_operator_state_and_overrides(forum, tmp_path, capsys):
    seed(forum)
    checkout = forum_checkout(forum, tmp_path)
    sync(forum, layout="forum", repository=str(checkout))
    root = checkout / ".forkmesh" / "forum"
    native_category = dm.NATIVE_CATEGORY_BASE + 1
    (root / "local.json").write_text(json.dumps({"title": "Our copy", "categories": {
        str(native_category): {"name": "ForkMesh talk", "slug": "forkmesh-talk", "color": "E45735",
                               "description": "", "parent_category_id": None, "position": 9},
        "2": {"name": "Help desk"}}}))
    (root / "native.json").write_text(json.dumps({"next": 2, "topics": {"n1": {
        "id": "n1", "title": "Made here", "slug": "made-here", "category_id": native_category, "tags": [],
        "created_at": "2026-01-10T00:00:00.000Z", "bumped_at": "2026-01-10T00:00:00.000Z", "posts_count": 3,
        "reply_count": 2, "views": 0, "like_count": 0, "pinned": False, "pinned_globally": False, "closed": False,
        "archived": False, "visible": True, "posters": [{"username": "ada"}], "last_poster_username": "ada"}}}))
    (root / "topics" / "12" / "state.json").write_text(json.dumps({
        "pinned": True, "local_posts": 2, "bumped_at": "2026-01-11T00:00:00.000Z", "last_poster_username": "bob",
        "posters": [{"username": "bob"}]}))
    assert dm.main(["fold", "--workspace", str(checkout)]) == 0
    assert json.loads(capsys.readouterr().out.strip().splitlines()[-1])["commit"]
    forum_json = json.loads((root / "forum.json").read_text())
    assert forum_json["title"] == "Our copy" and forum_json["source"]["type"] == "discourse"
    assert forum_json["counts"]["topics"] == 4
    latest = [row["id"] for row in json.loads((root / "latest" / "1.json").read_text())["rows"]]
    assert latest == [10, 12, "n1", 11]
    row12 = json.loads((root / "latest" / "1.json").read_text())["rows"][1]
    assert row12["posts_count"] == 3 and row12["last_poster_username"] == "bob" and row12["pinned"] is True
    categories = {item["id"]: item for item in json.loads((root / "categories.json").read_text())}
    assert categories[native_category]["name"] == "ForkMesh talk" and categories[native_category]["topic_count"] == 1
    assert categories[2]["name"] == "Help desk"
    assert json.loads((root / "c" / str(native_category) / "1.json").read_text())["rows"][0]["origin"] == "forkmesh"
    assert git_log(checkout)[0] == "Forum: rebuild the lists"
    sync(forum, layout="forum", repository=str(checkout))
    assert json.loads((root / "forum.json").read_text())["title"] == "Our copy"
    assert "n1" in [row["id"] for row in json.loads((root / "latest" / "1.json").read_text())["rows"]]


def test_robots_txt_is_honoured_unless_the_operator_has_permission(forum):
    seed(forum)
    forum.robots = "User-agent: *\nDisallow: /\n"
    with pytest.raises(dm.MirrorError, match="robots.txt"):
        sync(forum)
    result, _ = sync(forum, ignoreRobots=True)
    assert result["fetched"] == 3


def test_rate_limits_are_waited_out(forum):
    seed(forum)
    forum.rate_limited = 1
    sleeps = []
    result, _ = sync(forum, sleeps=sleeps)
    assert result["fetched"] == 3 and 1.0 in sleeps


def test_check_reports_size_and_readiness(forum):
    seed(forum)
    report = dm.check_forum(forum.origin, sleep=lambda _: None, delay=0)
    assert report["ready"] and report["discourse"] and report["public"] and report["robotsAllow"]
    assert report["title"] == "Example Forum" and report["topics"] == 3 and report["posts"] == 7
    forum.robots = "User-agent: *\nDisallow: /t/\n"
    report = dm.check_forum(forum.origin, sleep=lambda _: None, delay=0)
    assert not report["ready"]


def test_forum_addresses():
    assert dm.normalize_origin("https://Forum.Example.org/") == "https://forum.example.org"
    assert dm.normalize_origin("https://example.org/community/") == "https://example.org/community"
    for bad in ("http://forum.example.org", "https://user:pw@example.org", "https://example.org/?x=1", "ftp://x", ""):
        with pytest.raises(dm.MirrorError):
            dm.normalize_origin(bad)


def test_sanitizer_keeps_formatting_and_drops_active_content():
    origin = "https://forum.example.org"
    out = dm.sanitize(
        '<h2>Title</h2><pre><code class="lang-sh">a &lt; b</code></pre>'
        '<iframe src="https://evil.example"><p>inside</p></iframe><svg><script>x</script></svg>'
        '<a href="data:text/html,x">d</a><a href="mailto:a@example.org">m</a>'
        '<a href="https://forum.example.org/t/x/5/3">t</a><table><tr><td colspan="2;x">c</td></tr></table>'
        '<img src="/images/emoji/smile.png" alt=":smile:"><p>open',
        origin, {"https://forum.example.org/images/emoji/smile.png": "e.png"}, {5},
    )
    assert out.startswith("<h2>Title</h2><pre><code>a &lt; b</code></pre>")
    assert "iframe" not in out and "inside" not in out and "script" not in out and "svg" not in out
    assert "<a>d</a>" in out and 'href="mailto:a@example.org"' in out
    assert '<a href="5.html#p3">t</a>' in out and "colspan" not in out
    assert '<img alt=":smile:" loading="lazy" src="../../uploads/e.png">' in out
    assert out.endswith("<p>open</p>")


def test_cli_adds_lists_and_removes_without_touching_the_copy(tmp_path, capsys):
    data = tmp_path / "data"
    assert dm.main(["--data-dir", str(data), "add", "https://forum.example.org", "--name", "example",
                    "--no-uploads"]) == 0
    saved = json.loads((data / "forums.json").read_text())
    assert saved == {"schemaVersion": 1, "forums": [
        {"name": "example", "origin": "https://forum.example.org", "uploads": False}]}
    assert dm.main(["--data-dir", str(data), "add", "https://forum.example.org", "--name", "example"]) == 1
    assert dm.main(["--data-dir", str(data), "add", "https://forum.example.org", "--name", "Bad Name"]) == 1
    capsys.readouterr()
    assert dm.main(["--data-dir", str(data), "list"]) == 0
    listed = json.loads(capsys.readouterr().out)
    assert listed[0]["repository"] == str(data / "forums" / "example") and listed[0]["delaySeconds"] == 1.0
    (data / "forums" / "example").mkdir(parents=True)
    (data / "forums" / "example" / "keep.txt").write_text("x")
    assert dm.main(["--data-dir", str(data), "remove", "example"]) == 0
    assert (data / "forums" / "example" / "keep.txt").exists()
    assert json.loads((data / "forums.json").read_text())["forums"] == []


def test_the_tool_runs_as_a_script(tmp_path):
    result = subprocess.run([sys.executable, str(TOOL), "--help"], capture_output=True, text=True, check=False)
    assert result.returncode == 0 and "Discourse" in result.stdout


def test_the_package_is_listed():
    assert MANIFEST["id"] == "discourse-mirror"
    assert MANIFEST["entrypoints"]["mirror"] == "content/mirror/discourse_mirror.py"
    assert "worker" not in MANIFEST and not MANIFEST.get("contributions")
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "discourse-mirror")
    assert row == {"id": "discourse-mirror", "version": MANIFEST["version"],
                   "manifest": "packages/discourse-mirror/manifest.json"}
    plugins = json.loads((REPO / "marketplace" / "plugins.json").read_text(encoding="utf-8"))["plugins"]
    assert "discourse-mirror" in plugins
    assert (REPO / "docs" / "marketplace" / "discourse-mirror.md").is_file()


def test_no_forum_is_named_in_the_package():
    hosts = set()
    for path in [*PACKAGE.rglob("*"), REPO / "docs" / "marketplace" / "discourse-mirror.md"]:
        if path.is_file() and path.suffix in {".py", ".md", ".json", ".svg"} and "tests" not in path.parts:
            text = path.read_text(encoding="utf-8")
            hosts |= {urllib.parse.urlsplit(url).hostname for url in
                      __import__("re").findall(r"https://[A-Za-z0-9.-]+", text)}
    assert hosts <= {"forkmesh.com", "forum.example.org", "example.org", "cdn.example.org", "www.w3.org", "www.discourse.org"}
