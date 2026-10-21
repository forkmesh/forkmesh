from __future__ import annotations

import asyncio
import json
import re

from forkmesh_host import js, repo_ssr, repo_views
from forkmesh_host.paths import APP_DIR

REACTIONS = {
    "body": [{"emoji": "celebrate", "count": 2, "mine": False,
              "people": [{"name": "alice", "agent": False}]}],
    "r1": [{"emoji": "eyes", "count": 1, "mine": False,
            "people": [{"name": "</script><b>", "agent": False}]}],
}


def repo_page():
    return (APP_DIR / "public" / "dashboard" / "repo.html").read_text("utf-8")


def answer(payload, status=200):
    return js.Response(json.dumps(payload).encode(), status, "",
                       js.Headers({"content-type": "application/json"}))


def test_the_first_view_reads_reactions_once_anonymously_and_remembers_them():
    calls = []

    async def worker_fetch(request):
        calls.append(request)
        return answer({"ok": True, "reactions": REACTIONS})

    async def main():
        repo_ssr._reactions.clear()
        first = repo_ssr.read_reactions(worker_fetch, "https://forge.test", "alice",
                                        "app", "pull", 4)
        second = repo_ssr.read_reactions(worker_fetch, "https://forge.test", "alice",
                                         "app", "pull", 4)
        assert first.task is second.task
        assert await repo_ssr.reactions_within(first) == REACTIONS
        third = repo_ssr.read_reactions(worker_fetch, "https://forge.test", "alice",
                                        "app", "pull", 4)
        assert await repo_ssr.reactions_within(third) == REACTIONS
        assert len(calls) == 1
        request = calls[0]
        assert str(request.url) == ("https://forge.test/api/repo/alice/app/reactions"
                                    "?kind=pull&number=4")
        assert request.headers.get("cookie") is None
        assert request.headers.get("authorization") is None
        assert repo_ssr.read_reactions(worker_fetch, "https://forge.test", "alice",
                                       "app", "pulls", 0) is None
    asyncio.run(main())


def test_a_write_through_this_node_forgets_the_repository_reactions():
    async def worker_fetch(request):
        return answer({"ok": True, "reactions": {}})

    async def main():
        repo_ssr._reactions.clear()
        read = repo_ssr.read_reactions(worker_fetch, "https://forge.test", "alice",
                                       "app", "issue", 9)
        await repo_ssr.reactions_within(read)
        assert repo_ssr._reactions
        repo_ssr.forget_reactions({"method": "GET", "path": "/api/repo/alice/app/reactions"})
        assert repo_ssr._reactions
        repo_ssr.forget_reactions({"method": "POST", "path": "/api/repo/bob/app/reactions"})
        assert repo_ssr._reactions
        repo_ssr.forget_reactions({"method": "POST", "path": "/api/repo/Alice/app/reactions"})
        assert not repo_ssr._reactions
    asyncio.run(main())


def test_a_slow_refresh_serves_the_remembered_summary_and_failures_serve_none():
    gate = {}

    async def slow(request):
        await gate["wait"]
        return answer({"ok": True, "reactions": {}})

    async def broken(request):
        return answer({"error": "not_found"}, 404)

    async def main():
        repo_ssr._reactions.clear()
        gate["wait"] = asyncio.get_running_loop().create_future()
        key = repo_ssr._reactions_key("alice", "app", "issue:9")
        repo_ssr._reactions[key] = (
            asyncio.get_running_loop().time() - repo_ssr.REACTIONS_FRESH_SECONDS - 1,
            REACTIONS)
        read = repo_ssr.read_reactions(slow, "https://forge.test", "alice", "app",
                                       "issue", 9)
        assert await repo_ssr.reactions_within(read, 0.02) == REACTIONS
        gate["wait"].set_result(None)
        await read.task
        missing = repo_ssr.read_reactions(broken, "https://forge.test", "alice", "app",
                                          "issue", 10)
        assert await repo_ssr.reactions_within(missing) is None
    asyncio.run(main())


def pull_bundle():
    model = {"pull": {
        "number": 4, "title": "Fix", "state": "open", "author": "bob",
        "ts": "2026-10-01T00:00:00Z", "base": "main", "head": "fix", "patch": "",
        "body": "Please merge", "conversation": [
            {"type": "comment", "id": "r1", "author": "carol", "ts": "", "state": "",
             "path": "", "body": "Looks good"},
            {"type": "review", "id": "", "author": "dave", "ts": "", "state": "approved",
             "path": "", "body": "ok"},
            {"type": "status", "id": "s1", "author": "dave", "ts": "", "state": "closed",
             "path": "", "body": "closing"}]}}
    return repo_views.Bundle("alice", "app", "pull", 4, {}, model, "node-a", "main")


def test_pull_first_view_draws_counts_where_the_comments_are():
    bundle = pull_bundle()
    html = repo_views.render(bundle, "", REACTIONS)
    assert html.count("data-comment-reactions=\"pull:4\"") == 2
    assert 'data-comment-id="body"' in html and 'data-comment-id="r1"' in html
    assert 'data-comment-id="s1"' not in html
    assert "&lt;/script&gt;&lt;b&gt;" in html and "</script><b>" not in html
    assert ">2</span>" in html
    plain = repo_views.render(bundle, "", None)
    assert plain != html and 'data-comment-id="body"' in plain
    assert repo_views.render(bundle, "", REACTIONS) is html


def test_discussion_first_view_has_bars_for_the_post_and_replies():
    model = {"discussion": {
        "number": 2, "title": "Idea", "category": "Ideas", "author": "bob", "ts": "",
        "body": "What if", "replies": [{"id": "r1", "type": "comment", "author": "c",
                                        "ts": "", "body": "Yes"}]}}
    bundle = repo_views.Bundle("alice", "app", "discussion", 2, {}, model, "n", "main")
    html = repo_views.render(bundle, "", REACTIONS)
    assert html.count('data-comment-reactions="discussion:2"') == 2


def test_the_page_carries_a_seed_the_dashboard_reads_before_its_own_fetch():
    page = repo_page()
    rendered = repo_ssr.inject_view(page, pull_bundle(), "", REACTIONS)
    seed = re.search(r'<script type="application/json" id="forkmesh-comment-reactions">'
                     r'(.*?)</script>', rendered)
    assert seed
    assert "</script><b>" not in seed.group(1)
    data = json.loads(seed.group(1))
    assert data == {"base": "/api/repo/alice/app", "subject": "pull:4",
                    "reactions": REACTIONS}
    assert "forkmesh-comment-reactions" not in repo_ssr.inject_view(
        page, pull_bundle(), "", None)


def test_a_view_ticket_starts_its_reaction_read_beside_the_worker():
    calls = []
    page = repo_page()

    async def worker_fetch(request):
        calls.append(str(request.url) if hasattr(request, "url") else request)
        if "/reactions" in calls[-1]:
            return answer({"ok": True, "reactions": REACTIONS})
        return js.Response(page.encode("utf-8"), 200, "",
                           js.Headers({"content-type": "text/html; charset=utf-8"}))

    class Request:
        url = "https://forge.test/alice/app/pull/4"

    async def main():
        repo_ssr._reactions.clear()
        done = asyncio.get_running_loop().create_future()
        done.set_result(pull_bundle())
        ticket = repo_ssr.ViewTicket(done, "", ("alice", "app", "pull", 4))
        response = await repo_ssr.fetch(ticket, worker_fetch, Request())
        final = await repo_ssr.finish(ticket, response)
        return (await final._read()).decode("utf-8")

    text = asyncio.run(main())
    assert any("/api/repo/alice/app/reactions?kind=pull&number=4" in c for c in calls)
    assert 'id="forkmesh-comment-reactions"' in text
    assert 'data-comment-id="r1"' in text
