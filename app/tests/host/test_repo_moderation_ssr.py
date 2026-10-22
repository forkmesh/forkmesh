from __future__ import annotations

import asyncio
import json

from forkmesh_host import js, repo_moderation, repo_ssr, repo_views
from forkmesh_host.paths import APP_DIR

OWNER, NAME = "example", "forum"
ISSUE = {
    "number": 5, "title": "Tor bridges", "status": "open",
    "events": [
        {"type": "open", "id": "open-1", "ts": 1, "author": "k1",
         "authorName": "ann", "title": "Tor bridges",
         "body": "Opening text about bridges"},
        {"type": "comment", "id": "comment-1", "ts": 2, "author": "k2",
         "authorName": "troll", "body": "BUY CHEAP PILLS"},
        {"type": "comment", "id": "forge-github-comment-9", "ts": 3,
         "author": "k3", "authorName": "github:someone",
         "body": "Off-topic mirrored rant"},
        {"type": "comment", "id": "comment-2", "ts": 4, "author": "k4",
         "authorName": "bob", "body": "Useful measurement data"},
    ],
}
DATA = {"owner": OWNER, "repo": NAME, "ref": "main", "commit": "a" * 40,
        "servedBy": "n", "tree": {"ok": True, "commit": "a" * 40,
                                  "counts": {"openIssues": 1}}}


def repo_page():
    return (APP_DIR / "public" / "dashboard" / "repo.html").read_text("utf-8")


def bundle():
    text = json.dumps(ISSUE)
    model = {"issue": repo_views.issue_summary(text, 5), "issueJson": text,
             "index": {}}
    return repo_views.Bundle(OWNER, NAME, "issue", 5, {}, model, "n", "main")


def answer(payload, status=200):
    return js.Response(json.dumps(payload).encode(), status, "",
                       js.Headers({"content-type": "application/json"}))


def state(hidden=(), authors=()):
    return repo_moderation.clean({"hidden": list(hidden),
                                  "hiddenAuthors": list(authors)})


def article(moderation):
    page = repo_ssr.inject_issue(repo_page(), DATA, bundle(),
                                 moderation=moderation)
    assert page is not None
    return page[page.index("<div data-repo-ssr"):]


def test_hidden_posts_are_not_in_the_first_view():
    plain = article(None)
    assert "BUY CHEAP PILLS" in plain and "Off-topic mirrored rant" in plain
    view = article(state(["forge-github-comment-9"], ["troll"]))
    assert "BUY CHEAP PILLS" not in view
    assert "Off-topic mirrored rant" not in view
    assert "Useful measurement data" in view
    assert "Opening text about bridges" in view
    assert view.count("This post was hidden by the moderators.") == 2


def test_hiding_the_opening_post():
    view = article(state(["body"]))
    assert "Opening text about bridges" not in view
    assert "This post was hidden by the moderators." in view
    assert "BUY CHEAP PILLS" in view


def test_the_render_cache_follows_the_moderation_state():
    shared = bundle()
    first = repo_ssr.inject_issue(repo_page(), DATA, shared)
    second = repo_ssr.inject_issue(repo_page(), DATA, shared,
                                   moderation=state([], ["troll"]))
    assert "BUY CHEAP PILLS" in first and "BUY CHEAP PILLS" not in second
    assert repo_moderation.digest(None) == repo_moderation.digest(state())


def test_the_state_is_read_anonymously_once_and_forgotten_on_writes():
    calls = []

    async def worker_fetch(request):
        calls.append(request)
        return answer({"ok": True, "hidden": ["comment-1"],
                       "hiddenAuthors": ["Troll"], "locked": True})

    async def main():
        repo_moderation._states.clear()
        read = repo_moderation.read(worker_fetch, "https://forum.test", OWNER,
                                    NAME, "issue", 5)
        found = await repo_moderation.within(read)
        assert found == {"hidden": ["comment-1"], "hiddenAuthors": ["troll"],
                         "locked": True}
        again = repo_moderation.read(worker_fetch, "https://forum.test", OWNER,
                                     NAME, "issue", 5)
        assert await repo_moderation.within(again) == found
        assert len(calls) == 1
        assert str(calls[0].url) == (
            "https://forum.test/api/repo/example/forum/moderation"
            "?kind=issue&number=5")
        assert calls[0].headers.get("cookie") is None
        assert repo_moderation.cached(OWNER, NAME, 5) == found
        assert repo_moderation.read(worker_fetch, "https://forum.test", OWNER,
                                    NAME, "pull", 5) is None
        repo_moderation.forget({"method": "GET",
                                "path": "/api/repo/example/forum/moderation"})
        assert repo_moderation._states
        repo_moderation.forget({"method": "POST",
                                "path": "/api/repo/Example/forum/moderation"})
        assert not repo_moderation._states
    asyncio.run(main())


def test_a_failed_read_serves_nothing():
    async def broken(request):
        return answer({"error": "not_found"}, 404)

    async def main():
        repo_moderation._states.clear()
        read = repo_moderation.read(broken, "https://forum.test", OWNER, NAME,
                                    "issue", 6)
        assert await repo_moderation.within(read) is None
    asyncio.run(main())
    assert repo_moderation.clean({"hidden": "x"}) is None
    assert repo_moderation.apply("not json", state(["a"])) == "not json"
