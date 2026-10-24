from __future__ import annotations

import asyncio
import json
import re
import urllib.parse
import xml.etree.ElementTree as ET

import pytest

from forkmesh_host import js as _js
from forkmesh_host import read_surfaces as rs
from forkmesh_host.repo_fastpath import RepoFastPath

COMMIT = "c" * 40
ORIGIN = "https://forum.example.org"
ATOM = "{http://www.w3.org/2005/Atom}"
THR = "{http://purl.org/syndication/thread/1.0}"
DAY = 86_400_000
START = 1_700_000_000_000
COUNTRIES = ["Iran", "China", "Russia", "Turkmenistan", "reading group", "Myanmar"]
WORDS = ("censorship circumvention measurement TLS QUIC fingerprint probing tunnel "
         "resolver blocking throttling middlebox handshake obfuscation").split()


def prose(seed, size):
    out, index, length = [], seed, 0
    while length < size:
        out.append(WORDS[index % len(WORDS)])
        length += len(out[-1]) + 1
        index = (index * 7 + 3) % 1009
    text = " ".join(out)
    return "\n\n".join(text[i:i + 400] for i in range(0, len(text), 400))


def comment(number, index, body=None, author=None):
    return {"type": "comment", "id": "forge-github-comment-%d%04d" % (number, index),
            "author": "key-%d" % (index % 9),
            "authorName": author or "github:user%d" % (index % 9),
            "ts": START + number * DAY + index * 60_000,
            "body": body if body is not None else prose(number + index, 4200)}


def record(number, title, labels=(), comments=0, status="open", body=None, extra=()):
    events = [{"type": "open", "id": "forge-github-issue-%d" % number,
               "author": "key-x", "authorName": "github:alice",
               "ts": START + number * DAY, "title": title,
               "body": body if body is not None else prose(number, 1800)}]
    events += [comment(number, index) for index in range(1, comments + 1)]
    events += list(extra)
    return {"schema": "forkmesh-issue-v1", "number": number, "title": title,
            "status": status, "author": "key-x", "authorName": "github:alice",
            "createdAt": START + number * DAY, "labels": list(labels), "events": events}


def index_item(doc, location):
    events = doc["events"]
    comments = [e for e in events if e["type"] == "comment"]
    return {"number": doc["number"], "title": doc["title"], "status": doc["status"],
            "author": doc["authorName"], "labels": doc["labels"], "deleted": False,
            "commentCount": len(comments), "createdAtMs": doc["createdAt"],
            "updatedAtMs": max(e["ts"] for e in events), "location": location}


def forum(count=60):
    files, items = {}, []
    for number in range(1, count + 1):
        title = "Thread %d about %s blocking and a fairly long descriptive title" % (
            number, COUNTRIES[number % len(COUNTRIES)])
        labels = [COUNTRIES[number % len(COUNTRIES)], COUNTRIES[(number + 2) % 6]]
        doc = record(number, title, labels, comments=number % 4)
        location = "closed" if number % 10 == 0 else "open"
        if number % 10 == 0:
            doc["status"] = "closed"
        files[rs.record_path(number, location)] = json.dumps(doc)
        items.append(index_item(doc, location))
    big = record(519, "Leak of Geedge Networks internal documents", COUNTRIES[:4], comments=98)
    files[rs.record_path(519, "open")] = json.dumps(big)
    items.append(index_item(big, "open"))
    tricky = record(600, "Escaping <b>&</b> \"quotes\"", ["Iran"], body=(
        "Look: <script>alert(1)</script> & **bold** "
        "![shot](https://github.com/user-attachments/assets/abc.png) "
        "![local](docs/map.png) [doc](docs/readme.md)\n\n"
        "سلام دنیا"),
        extra=[comment(600, 1, "Reply with <i>html</i> & ampersand", "github:alice")])
    files[rs.record_path(600, "open")] = json.dumps(tricky)
    items.append(index_item(tricky, "open"))
    files["README.md"] = ("# Forum\n\n### Example forum\nEnglish text.\n\n"
                          "### &rlm;انجمن Example\n"
                          "&rlm;متن فارسی.\n")
    return files, {"ok": True, "commit": COMMIT, "issues": items, "missing": [],
                   "openFolders": count, "closedFolders": 6, "truncated": False}


class FakeFastPath:
    def __init__(self, public=True, count=60):
        self.public = public
        self.files, self.index = forum(count)
        self.urls = []

    gateway_url = staticmethod(RepoFastPath.gateway_url)

    async def read(self, url, immutable=False):
        self.urls.append((url, immutable))
        parts = urllib.parse.urlsplit(url)
        op = parts.path.rsplit("/", 1)[1]
        query = urllib.parse.parse_qs(parts.query, keep_blank_values=True)
        if not self.public or "/example/forum/" not in parts.path:
            return 404, b"", {"ok": False, "error": "not_found"}
        if op == "tree":
            return 200, b"", {"ok": True, "commit": COMMIT, "entries": [
                {"name": "README.md", "type": "blob"}, {"name": ".forkmesh", "type": "tree"}]}
        if op == "issue-index":
            return 200, b"", self.index
        if op == "blobs":
            return 200, b"", {"ok": True, "blobs": {
                p: ({"content": self.files[p]} if p in self.files else None)
                for p in query.get("path", [])}}
        if op == "blob":
            path = query["path"][0]
            if path not in self.files:
                return 404, b"", {"ok": False}
            return 200, b"", {"ok": True, "content": self.files[path]}
        return 404, b"", {"ok": False}


def worker(home="example/forum", quota=None, plugin=True):
    calls = []

    async def fetch(request):
        path = urllib.parse.urlsplit(request.url).path
        calls.append(path)
        if path == "/api/instance":
            data = {"ok": True, "name": "example", "branded": True,
                    "plugins": [{"id": "read-surfaces", "enabled": plugin}],
                    "logo": "/instance-logo.png", "poweredBy": None,
                    "home": {"repository": home} if home else None}
        elif path == "/api/repositories":
            data = {"ok": True, "repositories": [
                {"owner": "example", "name": "forkmesh", "visibility": "public",
                 "description": "Platform source"},
                {"owner": "example", "name": "forum", "visibility": "public",
                 "description": "Forum"},
                {"owner": "alice", "name": "secret", "visibility": "private"}]}
        elif path.endswith("/mirror-quota") and quota is not None:
            data = quota
        else:
            return _js.Response(b"{}", 404, "", _js.Headers())
        return _js.Response(json.dumps(data).encode(), 200, "", _js.Headers(
            {"content-type": "application/json"}))
    fetch.calls = calls
    return fetch


@pytest.fixture(autouse=True)
def plugin_on(monkeypatch):
    from forkmesh_host import repo_ssr_issue
    monkeypatch.setattr(repo_ssr_issue, "plugins_off", lambda: frozenset())


@pytest.fixture(autouse=True)
def fresh():
    for store in (rs._heads, rs._instance, rs._listing, rs._cache):
        store.clear()
    rs._cache_bytes[0] = 0
    yield


def get(path, fast=None, fetch=None, headers=(), method="GET"):
    fast = fast or FakeFastPath()
    path, _, query = path.partition("?")
    scope = {"type": "http", "method": method, "path": urllib.parse.unquote(path),
             "raw_path": path.encode(), "query_string": query.encode(),
             "headers": [(k.encode(), v.encode()) for k, v in headers]}
    sent = []

    async def send(message):
        sent.append(message)
    handled = asyncio.run(rs.handle(fast, fetch or worker(), scope, send, ORIGIN))
    if not handled:
        return None
    start = sent[0]
    return (start["status"], {k.decode(): v.decode() for k, v in start["headers"]},
            b"".join(m.get("body", b"") for m in sent[1:]))


def feed(path, **kw):
    status, headers, body = get(path, **kw)
    assert status == 200, body
    assert headers["content-type"] == "application/atom+xml; charset=utf-8"
    return ET.fromstring(body), headers, body


def test_routes():
    def kind(path, method="GET"):
        return rs.target({"method": method, "path": path, "raw_path": path.encode()})
    assert kind("/example/forum/issues.atom") == ("feed", "example", "forum", "issues")
    assert kind("/Example/forum/activity.atom") == ("feed", "example", "forum", "activity")
    assert kind("/example/forum/issues/519.atom") == ("thread-feed", "example", "forum", 519)
    assert kind("/example/forum/labels/reading%20group.atom") == (
        "label-feed", "example", "forum", "reading group")
    assert kind("/example/forum/lite") == ("lite", "example", "forum")
    assert kind("/example/forum/lite/issues/7/") == ("lite-thread", "example", "forum", 7)
    assert kind("/lite") == ("lite-home",)
    assert kind("/") == ("home",)
    assert kind("/example/forum/issues.atom", "POST") is None
    assert kind("/api/bbs/issues.atom") is None
    assert kind("/example/forum/blob/main/issues.atom") is None
    assert kind("/example/forum/issues/0.atom") is None
    assert kind("/example/forum.git/lite") is None


def test_private_or_unknown_repositories_fall_through():
    assert get("/example/forum/issues.atom", FakeFastPath(public=False)) is None
    assert get("/someone/else/lite") is None
    assert get("/", fetch=worker(home=None)) is None


def test_new_threads_feed_is_valid_atom_with_threading():
    root, headers, body = feed("/example/forum/issues.atom")
    assert headers["cache-control"] == "public, max-age=60"
    assert headers["etag"].startswith('"fm-rs-') and headers["last-modified"].endswith("GMT")
    assert root.tag == ATOM + "feed"
    assert root.find(ATOM + "id").text == "tag:forum.example.org,2026:example/forum/issues"
    assert root.find(ATOM + "link[@rel='self']").get("href") == \
        ORIGIN + "/example/forum/issues.atom"
    entries = root.findall(ATOM + "entry")
    assert len(entries) == 50
    first = entries[0]
    assert first.find(ATOM + "id").text == \
        "tag:forum.example.org,2026:example/forum/issues/600"
    assert first.find(ATOM + "author/" + ATOM + "name").text == "@alice"
    assert first.find(ATOM + "author/" + ATOM + "uri").text == "https://github.com/alice"
    assert first.find(ATOM + "link[@rel='alternate']").get("href") == \
        ORIGIN + "/example/forum/issues/600"
    replies = first.find(ATOM + "link[@rel='replies']")
    assert replies.get("href") == ORIGIN + "/example/forum/issues/600.atom"
    assert replies.get(THR + "count") == "1"
    assert first.find(THR + "total").text == "1"
    assert [c.get("term") for c in first.findall(ATOM + "category")] == ["Iran"]
    big = next(e for e in entries if e.find(ATOM + "id").text.endswith("/issues/519"))
    assert big.find(THR + "total").text == "98"
    for entry in entries:
        for name in ("id", "title", "updated", "published", "author", "content"):
            assert entry.find(ATOM + name) is not None, name


def test_feed_content_is_escaped_html_without_scripts_or_remote_images():
    root, _headers, body = feed("/example/forum/issues.atom")
    entry = root.find(ATOM + "entry")
    assert entry.find(ATOM + "title").text == '#600 Escaping <b>&</b> "quotes"'
    content = entry.find(ATOM + "content")
    assert content.get("type") == "html"
    markup = content.text
    assert "<script" not in markup and "alert(1) &amp; " in markup
    assert "<strong>bold</strong>" in markup
    assert 'src="https://github.com' not in markup
    assert '<a href="https://github.com/user-attachments/assets/abc.png"' in markup
    assert 'src="%s/api/repo/example/forum/raw?path=docs%%2Fmap.png' % ORIGIN in markup
    assert 'href="%s/example/forum/blob/docs/readme.md"' % ORIGIN in markup
    assert "class=" not in markup
    assert b"<script" not in body


def test_activity_feed_threads_comments_under_their_thread():
    root, _h, _b = feed("/example/forum/activity.atom?limit=30")
    entries = root.findall(ATOM + "entry")
    assert len(entries) == 30
    stamps = [e.find(ATOM + "published").text for e in entries]
    assert stamps == sorted(stamps, reverse=True)
    reply = next(e for e in entries if e.find(THR + "in-reply-to") is not None)
    link = reply.find(THR + "in-reply-to")
    number = re.search(r"/issues/(\d+)/comment/", reply.find(ATOM + "id").text).group(1)
    assert link.get("ref") == "tag:forum.example.org,2026:example/forum/issues/" + number
    assert link.get("href") == ORIGIN + "/example/forum/issues/" + number
    assert link.get("type") == "text/html"
    assert "#c" in reply.find(ATOM + "link[@rel='alternate']").get("href")
    many, _h, _b = feed("/example/forum/activity.atom?limit=5000")
    assert len(many.findall(ATOM + "entry")) == 200


def test_thread_feed_has_the_post_and_every_comment():
    root, _h, _b = feed("/example/forum/issues/519.atom?limit=200")
    entries = root.findall(ATOM + "entry")
    assert len(entries) == 99
    replies = [e for e in entries if e.find(THR + "in-reply-to") is not None]
    assert len(replies) == 98
    assert {e.find(THR + "in-reply-to").get("ref") for e in replies} == {
        "tag:forum.example.org,2026:example/forum/issues/519"}
    ids = [e.find(ATOM + "id").text for e in entries]
    assert len(set(ids)) == 99
    assert any(i.endswith("/comment/forge-github-comment-5190001") for i in ids)
    assert len(feed("/example/forum/issues/519.atom")[0].findall(ATOM + "entry")) == 50
    assert get("/example/forum/issues/9999.atom")[0] == 404


def test_label_feeds():
    root, _h, _b = feed("/example/forum/labels/reading%20group.atom")
    terms = [[c.get("term") for c in e.findall(ATOM + "category")]
             for e in root.findall(ATOM + "entry")]
    assert terms and all("reading group" in t for t in terms)
    assert root.find(ATOM + "title").text == "example/forum: reading group"
    assert root.find(ATOM + "id").text == \
        "tag:forum.example.org,2026:example/forum/labels/reading%20group"
    same, _h, _b = feed("/example/forum/issues.atom?label=READING%20GROUP")
    assert len(same.findall(ATOM + "entry")) == len(terms)


def test_etag_answers_not_modified():
    _status, headers, _body = get("/example/forum/issues.atom")
    status, again, body = get("/example/forum/issues.atom",
                              headers=[("if-none-match", headers["etag"])])
    assert status == 304 and body == b"" and again["etag"] == headers["etag"]
    status, _headers, body = get("/example/forum/lite", method="HEAD")
    assert status == 200 and body == b""


def test_lite_list_stays_within_its_budget():
    status, headers, body = get("/example/forum/lite")
    assert status == 200 and headers["content-type"] == "text/html; charset=utf-8"
    assert "script-src" not in headers["content-security-policy"]
    assert "default-src 'none'" in headers["content-security-policy"]
    text = body.decode()
    assert text.count("<li>") == 50
    assert len(body) <= 30 * 1024, len(body)
    assert "<script" not in text and "http://" not in text and 'src="http' not in text
    assert len(re.search(r"<style>.*?</style>", text).group(0)) < 2048
    assert 'href="/example/forum/lite/issues/600" dir="auto"' in text
    assert "Escaping &lt;b&gt;&amp;&lt;/b&gt; &quot;quotes&quot;" in text
    assert "98 comments" in text
    assert '<link rel="alternate" type="application/atom+xml"' in text
    assert 'href="/example/forum/lite?page=2" rel="next"' in text
    second = get("/example/forum/lite?page=2")[2].decode()
    assert second.count("<li>") == 12 and 'rel="prev"' in second


def test_lite_list_filters_by_label_and_activity():
    text = get("/example/forum/lite?label=Iran")[2].decode()
    assert "<h1>example/forum: Iran</h1>" in text
    assert 'href="/example/forum/labels/Iran.atom"' in text
    rows = re.findall(r'lite/issues/(\d+)" dir', text)
    assert "600" in rows and "1" not in rows
    active = get("/example/forum/lite?sort=active")[2].decode()
    assert re.findall(r'lite/issues/(\d+)" dir', active)[0] == "600"


def test_lite_thread_reads_the_whole_thread_within_its_budget():
    status, _headers, body = get("/example/forum/lite/issues/519")
    assert status == 200
    text = body.decode()
    assert text.count("<article") == 99
    assert text.count('<div dir="auto">') == 99
    assert "<b>@alice</b>" in text and "<b>@user1</b>" in text
    assert 'id="c51900' in text
    bodies = sum(len(chunk.encode()) for chunk in re.findall(
        r'<div dir="auto">(.*?)</div></article>', text, re.S))
    overhead = len(body) - bodies
    chrome = overhead - 99 * len('<article id="c5190001"><p><b>@user1</b> '
                                 '<time>2023-11-14 22:13Z</time></p><div dir="auto">'
                                 '</div></article>')
    assert chrome <= 6 * 1024, chrome
    assert overhead <= 6 * 1024 + 99 * 120, overhead
    assert 'rel="prev">#60<' in text and 'rel="next">#600<' in text
    assert 'href="/example/forum/issues/519">Full view' in text
    assert 'href="/example/forum/issues/519.atom">Feed' in text
    assert "<script" not in text


def test_lite_thread_is_rtl_safe_and_keeps_images_on_the_instance():
    text = get("/example/forum/lite/issues/600")[2].decode()
    assert '<h1 dir="auto">' in text
    assert "سلام" in text
    assert "alert(1) &amp; " in text and "<script" not in text
    assert '<img src="/api/repo/example/forum/raw?path=docs%2Fmap.png' in text
    assert 'loading="lazy"' in text
    assert not re.search(r"<img[^>]+src=\"(?!/)", text)
    assert '>[shot]</a>' in text
    assert "Reply with" in text and "ampersand" in text
    assert get("/example/forum/lite/issues/4242")[0] == 404


def test_lite_home_lists_public_repositories_home_first():
    text = get("/lite")[2].decode()
    assert text.index("example/forum") < text.index("example/forkmesh")
    assert "alice/secret" not in text
    assert 'href="/example/forum/issues.atom"' in text


def test_forum_home_page():
    fetch = worker(quota={"ok": True, "target": 10, "healthy": 3, "mirrors": [],
                          "cloneUrl": "https://forum.example.org/example/forum"})
    status, headers, body = get("/", fetch=fetch)
    assert status == 200 and headers["cache-control"] == "public, max-age=60"
    text = body.decode()
    assert "<title>example</title>" in text
    assert '<section dir="auto"><h3>&rlm;' in text or '<section dir="auto"><h3>\u200f' in text
    assert text.count('<section dir="auto">') >= 3
    assert text.count("<li>") == 20 and '<li><a href="/example/forum/issues/600" dir="auto">' in text
    assert 'href="/example/forum/lite?label=reading%20group"' in text
    assert 'href="/example/forum/activity.atom"' in text
    assert "<section data-forum-mirrors><p" in text and "3 healthy mirrors of 10 wanted" in text
    assert "<script" not in text and 'src="http' not in text
    rs._cache.clear()
    plain = get("/", fetch=worker())[2].decode()
    assert "<section data-forum-mirrors></section>" in plain
    assert get("/?x=1", fetch=worker()) is None


def test_page_links_and_noscript():
    links = rs.head_links("example", "forum", 519)
    assert 'href="/example/forum/issues/519.atom"' in links
    assert 'href="/example/forum/activity.atom"' in links
    assert 'href="/example/forum/lite/issues/519"' in rs.noscript("example", "forum", 519)
    assert 'href="/example/forum/lite"' in rs.noscript("example", "forum")


def test_the_host_answers_read_surfaces_before_the_worker():
    source = (rs.__file__.rsplit("/", 1)[0] + "/asgi.py")
    text = open(source, encoding="utf-8").read()
    surfaces = text.index("read_surfaces.handle(")
    assert text.index('path.startswith("/api/repo/")') < surfaces < text.index(
        "ssr = repo_ssr.start(")


def test_repository_pages_point_at_the_feeds_and_the_lite_view():
    from forkmesh_host import repo_ssr
    page = ('<html><head>%s<title>x</title></head><body data-page="repo" class="a">'
            '<main></main></body></html>' % repo_ssr.CHARSET)
    issue = repo_ssr.inject_view_reads(page, "example", "forum", "issue", 519)
    assert 'type="application/atom+xml"' in issue and \
        'href="/example/forum/issues/519.atom"' in issue
    assert '<body data-page="repo" class="a"><noscript>' in issue
    assert 'href="/example/forum/lite/issues/519"' in issue
    listing = repo_ssr.inject_view_reads(page, "example", "forum", "issues", 0)
    assert 'href="/example/forum/lite"' in listing
    assert "<noscript>" not in repo_ssr.inject_view_reads(page, "example", "forum", "pulls", 0)


def test_issue_list_first_view_and_dashboard_link_the_feeds():
    from pathlib import Path
    from forkmesh_host import repo_views
    bundle = type("B", (), {"owner": "example", "repo": "forum"})()
    markup = repo_views._list(bundle, "issues", [])
    assert 'data-repo-issue-feeds><a href="/example/forum/issues.atom"' in markup
    assert "data-repo-issue-feeds" not in repo_views._list(bundle, "pulls", [])
    app = Path(rs.__file__).resolve().parents[2]
    source = (app / "public/dashboard/js/09-read-surfaces.js").read_text()
    assert "data-repo-issue-feeds" in source and "a[data-lite-link]" in source
    content = (app / "public/dashboard/js/06-repo-content.js").read_text()
    assert "+ repoIssueFeedLinks(state.selectedRepo);" in content
    sidebar = (app / "public/dashboard/partials/sidebar.html").read_text()
    assert '<a href="/lite" data-lite-link hidden>Lite</a>' in sidebar


def test_host_asgi_serves_feeds_and_the_home_page_and_passes_the_rest_on(monkeypatch):
    from forkmesh_host.asgi import HostASGI, WorkerFirstRules
    monkeypatch.delenv("FORKMESH_PUBLIC_HOSTS", raising=False)
    fetch = worker()

    class Runtime:
        env = type("Env", (), {"FORKMESH_PLATFORM": "vps",
                               "APP_ORIGIN": "https://forum.example.org"})()
        sealed = False
        durable = None

        async def fetch(self, request):
            path = urllib.parse.urlsplit(request.url).path
            if path.startswith("/api/"):
                return await fetch(request)
            fetch.calls.append("worker:" + path)
            return _js.Response(b"worker", 200, "", _js.Headers(
                {"content-type": "text/plain"}))

    app = HostASGI(Runtime(), serve_assets=False, cron=False,
                   worker_first=WorkerFirstRules([]))
    app.repo_fastpath = FakeFastPath()

    def call(path):
        sent = []

        async def receive():
            return {"type": "http.request", "body": b"", "more_body": False}

        async def send(message):
            sent.append(message)
        scope = {"type": "http", "method": "GET", "path": path, "raw_path": path.encode(),
                 "query_string": b"", "client": ("127.0.0.1", 5000), "scheme": "https",
                 "http_version": "1.1", "headers": [(b"host", b"forum.example.org")]}
        asyncio.run(app(scope, receive, send))
        headers = dict(sent[0]["headers"])
        return sent[0]["status"], headers, b"".join(m.get("body", b"") for m in sent[1:])

    status, headers, body = call("/example/forum/issues.atom")
    assert status == 200 and headers[b"content-type"].startswith(b"application/atom+xml")
    assert b"tag:forum.example.org,2026:example/forum/issues" in body
    status, _headers, body = call("/")
    assert status == 200 and b"Latest threads" in body
    status, _headers, body = call("/someone/else/lite")
    assert body == b"worker" and "worker:/someone/else/lite" in fetch.calls


def test_read_surface_pages_carry_cloudflare_email_off_markers():
    from pathlib import Path
    source = (Path(__file__).resolve().parents[2] / "host" / "forkmesh_host" / "read_surfaces.py").read_text(encoding="utf-8")
    assert "<body><!--email_off-->%s<!--/email_off--></body>" in source



def test_without_the_plugin_every_read_surface_falls_through_to_the_worker():
    for path in ("/example/forum/lite", "/example/forum/issues.atom", "/"):
        assert get(path, fetch=worker(plugin=False)) is None
