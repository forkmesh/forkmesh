from __future__ import annotations

import asyncio
import io
import json
import shutil
import struct
import subprocess
import types
import urllib.parse

import pytest

from forkmesh_host import diff_health, js, pull_card, repo_ssr, repo_views
from forkmesh_host.paths import APP_DIR
from test_repo_views import (FakeFastPath, build, fresh_bundles,  # noqa: F401
                             http_scope, no_short_owners, run_fetch,
                             short_repo_page)

CONTENT = (APP_DIR / "public" / "dashboard" / "js" / "06-repo-content.js").read_text("utf-8")
SLACK = b"Slackbot-LinkExpanding 1.0 (+https://api.slack.com/robots)"


def _slice(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


PATCHES = {
    "mixed": "\n".join([
        "diff --git a/app/src/sync_client.py b/app/src/sync_client.py",
        "new file mode 100644",
        "--- /dev/null",
        "+++ b/app/src/sync_client.py",
        "@@ -0,0 +1,7 @@",
        "+import requests",
        "+def pull(url):",
        "+    # TODO: tidy",
        '+    response = requests.get("http://mirror.forkmesh.net/" + url, verify=False)',
        "+    try:",
        "+        return eval(response.text)",
        "+    except Exception:",
        "diff --git a/notes.md b/notes.md",
        "--- a/notes.md",
        "+++ b/notes.md",
        "@@ -1 +1 @@",
        "-console.log('removed');",
        "+<b>plain</b>",
    ]),
    "hygiene": "\n".join(
        ["diff --git a/.env b/.env", "new file mode 100644", "@@ -0,0 +1 @@",
         '+api_key = "AbCdEfGhIjKlMnOpQrStUvWxYz012345"',  # forkmesh-secret-scan:ignore-line
         "diff --git a/yarn.lock b/yarn.lock", "@@ -1 +1 @@", "-a", "+b",
         "diff --git a/dist/app.min.js b/dist/app.min.js", "@@ -1 +1 @@", "+x",
         "diff --git a/logo.png b/logo.png", "Binary files a/logo.png and b/logo.png differ",
         "diff --git a/old.py b/new.py", "similarity index 90%",
         "rename from old.py", "rename to new.py", "@@ -1 +1 @@",
         "+<<<<<<< HEAD", "+=======", "+" + "y" * 150 + " ",
         "diff --git a/tests/test_a.py b/tests/test_a.py", "deleted file mode 100644",
         "@@ -1,2 +0,0 @@", "-def test_a():", "-    pass"]),
    "busy": "\n".join(
        "diff --git a/src/f%d.js b/src/f%d.js\n@@ -1 +1,3 @@\n"
        "+                if (a && b) { fetch(\"https://api.test/%d\"); }\n"
        "+// FIXME café \U0001F600\n+const t = AbortController;" % (n, n, n)
        for n in range(30)),
    "empty": "",
}


@pytest.mark.skipif(shutil.which("node") is None, reason="Node.js is unavailable")
def test_health_scores_match_the_dashboard():
    script = "\n".join([
        _slice(CONTENT, "  function parseDiffFiles(rawText) {", "  function parsePatchStats("),
        _slice(CONTENT, "  const DIFF_LINE_FLAGS = {", "  // The review surface on the page"),
        "const patches = %s;" % json.dumps(PATCHES),
        "const out = {};",
        "for (const [name, patch] of Object.entries(patches)) {",
        "  const files = parseDiffFiles(patch).files;",
        "  out[name] = {report: diffHealthReport(files).map(({key, label, score, tone}) => ({key, label, score, tone})),",
        "    stats: files.map((f) => [f.newPath || f.oldPath || 'file', f.status, f.binary, f.adds, f.dels])};",
        "}",
        "console.log(JSON.stringify(out));",
    ])
    expected = json.loads(subprocess.run(["node", "-e", script], capture_output=True,
                                         text=True, check=True).stdout)
    for name, patch in PATCHES.items():
        files = diff_health.parse_files(patch)
        assert diff_health.report(files) == expected[name]["report"], name
        assert [[f["newPath"] or f["oldPath"] or "file", f["status"], f["binary"],
                 f["adds"], f["dels"]] for f in files] == expected[name]["stats"], name
    assert [m["score"] for m in expected["mixed"]["report"]] == [55, 100, 72, 45, 36, 97]


AI_BODY = ("### \U0001F916 Automated AI review — Cloudflare Workers AI (`m`) · head `abc`\n\n"
           "> Automated; may be wrong.\n\n### Summary\n\nAdds a **share card** for pulls.\n\n"
           "More detail.\n\n### Suggested verdict\n\nLooks fine.")


def conversation_bundle(**pull):
    conversation = [
        {"type": "comment", "id": "c1", "author": "Bob", "authorKey": "bob", "body": "Nice idea"},
        {"type": "review", "id": "r1", "author": "Carol", "authorKey": "carol",
         "state": "approved", "body": "Ship it"},
        {"type": "review", "id": "r2", "author": "Dan", "authorKey": "dan",
         "state": "approved", "body": "ok"},
        {"type": "review", "id": "r3", "author": "Dan", "authorKey": "dan",
         "state": "changes_requested", "body": "Please **cap** the [fetches](https://x)."},
        {"type": "review", "id": "r4", "author": "Alice", "authorKey": "alice",
         "state": "approved", "body": "self"},
        {"type": "line-comment", "id": "l1", "author": "Bob", "authorKey": "bob",
         "path": "x.py", "line": "2", "body": "typo"},
        {"type": "comment", "id": "c2", "author": "Erin", "authorKey": "erin", "body": "gone"},
        {"type": "comment-delete", "target": "c2", "authorKey": "erin"},
        {"type": "comment", "id": "ai", "author": "ForkBot", "authorKey": "forkbot", "body": AI_BODY},
        {"type": "commit", "id": "m", "author": "Mallory", "body": "fix"},
    ]
    model = {"number": 12, "title": "Share card", "state": "open", "author": "Alice",
             "authorKey": "alice", "commits": 3, "base": "main", "head": "card",
             "ts": "1790000000000", "body": "", "conversation": conversation,
             "patch": PATCHES["mixed"]}
    model.update(pull)
    return types.SimpleNamespace(owner="forkmesh", repo="forkmesh", number=12,
                                 version="v123", model={"pull": model})


def test_summary_condenses_reviews_comments_people_and_health():
    info = pull_card.summary(conversation_bundle())
    assert info["state"] == "open" and info["commits"] == 3
    assert info["files"] == 2 and info["additions"] == 8 and info["deletions"] == 1
    assert [m["score"] for m in info["health"]] == [55, 100, 72, 45, 36, 97]
    assert info["review"] == {"state": "changes_requested", "approvals": 1, "blockers": 1}
    assert [(r["name"], r["state"]) for r in info["reviews"]] == [
        ("Dan", "changes_requested"), ("Carol", "approved"), ("ForkBot", "ai")]
    assert info["reviews"][0]["text"] == "Please cap the fetches."
    assert info["reviews"][2]["text"] == "Adds a share card for pulls."
    assert info["comments"] == 6 and info["commenters"] == 4
    assert [p["key"] for p in info["people"]] == ["alice", "bob", "carol", "dan", "forkbot"]
    draft = pull_card.summary(conversation_bundle(draft=True, conversation=[], patch=""))
    assert draft["state"] == "draft" and "health" not in draft and draft["reviews"] == []
    assert pull_card.summary(types.SimpleNamespace(model={})) is None


def test_ai_summary_reads_the_summary_section():
    assert pull_card.ai_summary(AI_BODY) == "Adds a share card for pulls."
    assert pull_card.ai_summary("no headings") == ""
    assert pull_card.ai_summary("### Summary\n\n" + "word " * 100).endswith("…")


OG_PAGE = (
    "<html><head><title>Repository - ForkMesh</title>"
    '<link rel="canonical" href="https://forge.test/forkmesh/forkmesh">'
    '<meta property="og:url" content="https://forge.test/forkmesh/forkmesh">'
    '<meta property="og:title" content="forkmesh/forkmesh - ForkMesh">'
    '<meta property="og:description" content="repo">'
    '<meta property="og:image" content="https://forge.test/api/repo/forkmesh/forkmesh/card.png?v=4">'
    '<meta property="og:image:alt" content="forkmesh/forkmesh - ForkMesh">'
    '<meta name="twitter:title" content="forkmesh/forkmesh - ForkMesh">'
    '<meta name="twitter:description" content="repo">'
    '<meta name="twitter:image" content="https://forge.test/api/repo/forkmesh/forkmesh/card.png?v=4">'
    '<meta name="twitter:image:alt" content="forkmesh/forkmesh - ForkMesh">'
    "</head><body></body></html>")


def meta(page, attr, name):
    marker = '<meta %s="%s" content="' % (attr, name)
    return page.split(marker, 1)[1].split('"', 1)[0]


def test_share_tags_describe_the_pull_request():
    bundle = conversation_bundle(title='Fix "quotes" & <tags>')
    page = pull_card.share_tags(OG_PAGE, bundle, "https://forge.test")
    image = "https://forge.test/forkmesh/forkmesh/pull/12.png?v=v123.%s" % pull_card.CARD_REVISION
    assert meta(page, "property", "og:image") == image
    assert meta(page, "name", "twitter:image") == image
    assert meta(page, "property", "og:url") == "https://forge.test/forkmesh/forkmesh/pull/12"
    title = meta(page, "property", "og:title")
    assert title == ("Fix &quot;quotes&quot; &amp; &lt;tags&gt; · Pull Request #12 "
                     "· forkmesh/forkmesh")
    assert "<script" not in page and "<tags>" not in page
    text = meta(page, "property", "og:description")
    assert text.startswith("Alice wants to merge 3 commits into main from card")
    assert "+8 −1 in 2 files" in text and "changes requested" in text
    assert "6 comments from 4 people" in text and "code health: merge 55" in text
    assert '<link rel="canonical" href="https://forge.test/forkmesh/forkmesh/pull/12">' in page
    short = pull_card.share_tags(OG_PAGE, bundle, "https://forge.test", "/forkmesh")
    assert meta(short, "property", "og:image").startswith(
        "https://forge.test/forkmesh/pull/12.png?v=")
    assert pull_card.share_tags("<html></html>", bundle, "https://forge.test") == "<html></html>"
    assert pull_card.share_tags(OG_PAGE, bundle, "") == OG_PAGE


@pytest.mark.parametrize("path,expected", [
    ("/forkmesh/forkmesh/pull/4.png", ("forkmesh", "forkmesh", 4)),
    ("/ForkMesh/forkmesh/pulls/4.png", ("forkmesh", "forkmesh", 4)),
    ("/forkmesh/pull/4.png", (None, "forkmesh", 4)),
    ("/api/forkmesh/pull/4.png", None),
    ("/forkmesh/forkmesh/pull/0.png", None),
    ("/forkmesh/forkmesh/pull/4.jpg", None),
    ("/forkmesh/forkmesh.git/pull/4.png", None),
])
def test_card_target(path, expected):
    assert pull_card.card_target(http_scope(path)) == expected


def png_size(body):
    assert body[:8] == b"\x89PNG\r\n\x1a\n"
    return struct.unpack(">II", body[16:24])


def fetch_bytes(scope, fast, pages=None):
    holder = {}

    async def worker_fetch(request):
        holder.setdefault("asked", []).append(str(request.url))
        page = (pages or {}).get(urllib.parse.urlsplit(str(request.url)).path)
        if page is None:
            return js.Response(b"worker", 404, "", js.Headers({"content-type": "text/plain"}))
        return js.Response(page.encode(), 200, "", js.Headers(
            {"content-type": "text/html; charset=utf-8"}))

    async def main():
        ticket = repo_ssr.start(fast, scope)
        request = js.Request("https://forge.test" + scope["path"], "GET", js.Headers({}))
        response = await repo_ssr.fetch(ticket, worker_fetch, request)
        response = await repo_ssr.finish(ticket, response)
        return response, await response._read()
    response, body = asyncio.run(main())
    return response, body, holder.get("asked", [])


@pytest.fixture
def fresh_cards():
    pull_card._cards.clear()
    pull_card._avatars.clear()
    yield
    pull_card._cards.clear()
    pull_card._avatars.clear()


def test_card_route_draws_the_pull_and_falls_through_when_private(
        staged_src, fresh_cards, no_short_owners):
    response, body, _asked = fetch_bytes(http_scope("/forkmesh/forkmesh/pull/4.png"),
                                         FakeFastPath())
    assert int(response.status) == 200
    assert response.headers.get("content-type") == "image/png"
    assert response.headers.get("x-content-type-options") == "nosniff"
    assert png_size(body) == (1200, 630)
    _response, again, _asked = fetch_bytes(http_scope("/forkmesh/forkmesh/pull/4.png"),
                                           FakeFastPath())
    assert again == body
    response, body, _asked = fetch_bytes(
        http_scope("/forkmesh/pull/4.png"), FakeFastPath(), {"/forkmesh": short_repo_page()})
    assert int(response.status) == 200 and png_size(body) == (1200, 630)
    repo_views._public.clear()
    repo_views._latest.clear()
    response, body, asked = fetch_bytes(http_scope("/forkmesh/forkmesh/pull/4.png"),
                                        FakeFastPath(public=False))
    assert body == b"worker" and asked == ["https://forge.test/forkmesh/forkmesh/pull/4.png"]
    response, body, _asked = fetch_bytes(http_scope("/forkmesh/forkmesh/pull/99.png"),
                                         FakeFastPath())
    assert body == b"worker"


def avatar_png(color):
    from PIL import Image
    out = io.BytesIO()
    Image.new("RGB", (128, 128), color).save(out, "PNG")
    return out.getvalue()


def test_avatars_are_fetched_scaled_and_bots_are_skipped(staged_src, fresh_cards):
    asked = []

    async def worker_fetch(request):
        asked.append(str(request.url))
        if "/api/accounts/carol/" in str(request.url):
            return js.Response(avatar_png((200, 10, 10)), 200, "",
                               js.Headers({"content-type": "image/png"}))
        return js.Response(b"{}", 404, "", js.Headers({"content-type": "application/json"}))

    info = pull_card.summary(conversation_bundle())
    info = asyncio.run(pull_card._with_avatars(info, worker_fetch, "https://forge.test"))
    people = {p["key"]: p for p in info["people"]}
    carol = people["carol"]["avatars"]
    assert set(carol) == {48, 30} and len(carol[48]) == 48 * 48 * 4
    assert carol[48][:4] == bytes((200, 10, 10, 255))
    assert people["bob"]["avatars"] == {}
    assert not any("forkbot" in url for url in asked)
    assert "https://forge.test/api/accounts/carol/avatar?size=128" in asked
    asked.clear()
    asyncio.run(pull_card._with_avatars(pull_card.summary(conversation_bundle()),
                                        worker_fetch, "https://forge.test"))
    assert asked == []


def og_repo_page():
    page = short_repo_page()
    tags = OG_PAGE.split("<head>", 1)[1].split("</head>", 1)[0]
    tags = tags.split("</title>", 1)[1]
    return page.replace("</head>", tags + "</head>", 1)


def test_pull_page_carries_the_pull_share_tags(fresh_cards):
    pages = {"/forkmesh/forkmesh/pull/4": og_repo_page()}
    _response, text, _asked = run_fetch(
        http_scope("/forkmesh/forkmesh/pull/4", SLACK), FakeFastPath(), pages)
    assert meta(text, "property", "og:image").startswith(
        "https://forge.test/forkmesh/forkmesh/pull/4.png?v=")
    assert meta(text, "property", "og:title") == (
        "Patch pull · Pull Request #4 · forkmesh/forkmesh")
    assert "+1 −1 in 1 file" in meta(text, "property", "og:description")
    assert "<title>Patch pull · Pull Request #4" in text


def test_a_link_preview_bot_waits_for_a_cold_pull(monkeypatch, fresh_cards):
    real = repo_views.build

    async def slow_build(*args):
        await asyncio.sleep(repo_ssr.VIEW_BUDGET_SECONDS * 3)
        return await real(*args)
    monkeypatch.setattr(repo_views, "build", slow_build)
    pages = {"/forkmesh/forkmesh/pull/4": og_repo_page()}
    _response, person, _asked = run_fetch(
        http_scope("/forkmesh/forkmesh/pull/4"), FakeFastPath(), pages)
    assert "/api/repo/forkmesh/forkmesh/card.png" in person
    repo_views._latest.clear()
    _response, bot, _asked = run_fetch(
        http_scope("/forkmesh/forkmesh/pull/4", SLACK), FakeFastPath(), pages)
    assert "/forkmesh/forkmesh/pull/4.png?v=" in bot
    assert "Files changed</h3>" not in bot


def test_preview_agents():
    for agent in (SLACK, b"Twitterbot/1.0", b"facebookexternalhit/1.1",
                  b"Mozilla/5.0 (compatible; Discordbot/2.0)",
                  b"Mastodon/4.3.0 (http.rb/5.2.0; +https://mastodon.social/)",
                  b"Bluesky Cardyb/1.1", b"TelegramBot (like TwitterBot)",
                  b"WhatsApp/2.23", b"LinkedInBot/1.0"):
        assert repo_ssr.preview_agent(http_scope("/a/b", agent)), agent
    assert not repo_ssr.preview_agent(http_scope("/a/b"))


def test_plan_pull_counts_commits_and_keeps_the_author_key(monkeypatch):
    import test_repo_views as base
    ledger = dict(base.FILES[base.LEDGER])
    ledger[".forkmesh/pulls/4/commits.mbox"] = "".join(
        "From %s Mon Sep 17 00:00:00 2001\nSubject: c%d\n\n" % (c * 40, n)
        for n, c in enumerate("123"))
    ledger[".forkmesh/pulls/4/pull.md"] = base.pull_md(
        4, "Patch pull", extra="author: alice-node\ndraft: true\n")
    monkeypatch.setitem(base.FILES, base.LEDGER, ledger)
    bundle, _fast = build("pull", 4)
    pull = bundle.model["pull"]
    assert pull["commits"] == 3 and pull["authorKey"] == "alice-node"
    assert pull["draft"] is True
