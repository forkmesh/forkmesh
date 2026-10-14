"""Server-rendered repository first view (forkmesh_host.repo_ssr)."""

from __future__ import annotations

import json
import re

from forkmesh_host import repo_ssr
from forkmesh_host.paths import APP_DIR

COMMIT = "a" * 40
DATA = {
    "owner": "forkmesh", "repo": "forkmesh", "ref": "main", "commit": COMMIT,
    "servedBy": "fmnext-n1",
    "tree": {"ok": True, "commit": COMMIT, "latestCommit": {
        "hash": COMMIT, "author": "Ada", "date": "2026-09-24T00:00:00Z",
        "subject": "Ship <it>"},
        "entries": [
            {"name": "README.md", "type": "blob", "subject": "docs",
             "date": "2026-09-01T00:00:00Z"},
            {"name": "app", "type": "tree", "subject": "</script><b>x",
             "date": "2026-09-02T00:00:00Z"}]},
    "readme": {"name": "README.md", "data": {"ok": True, "encoding": "utf8",
               "content": "# Title\n\nHello *world* [docs](docs/a.md) "
                          "[bad](javascript:alert(1)) <img src=x onerror=y>\n"
                          "\n```\n<script>alert(1)</script>\n```\n- one\n- two\n"}},
}


def repo_page():
    return (APP_DIR / "public" / "dashboard" / "repo.html").read_text("utf-8")


def test_inject_renders_the_first_view_and_its_data():
    page = repo_ssr.inject(repo_page(), DATA)
    assert page is not None
    assert "Choose a repository to inspect it." not in page
    assert 'data-dashboard-tree-path="app"' in page
    assert 'href="/forkmesh/forkmesh/blob/README.md"' in page
    assert "<h1>Title</h1>" in page and "<em>world</em>" in page
    assert 'href="/forkmesh/forkmesh/blob/docs/a.md"' in page
    # Untrusted text never becomes markup or script in the rendered view.
    view = page[page.index("<div class=\"fm-ssr\""):]
    assert "javascript:" not in view
    assert "onerror" not in view
    assert "&lt;script&gt;alert(1)&lt;/script&gt;" in view
    assert "Ship &lt;it&gt;" in view
    blob = re.search(r'<script type="application/json" '
                     r'id="forkmesh-repo-ssr">(.*?)</script>', page, re.S)
    assert blob and "</script" not in blob.group(1)
    assert json.loads(blob.group(1))["tree"]["commit"] == COMMIT
    # The data precedes the early-read script that consumes it.
    assert page.index('id="forkmesh-repo-ssr"') < page.index(
        "Early repository reads")


def test_inject_leaves_other_pages_alone():
    assert repo_ssr.inject("<html><meta charset=\"UTF-8\" /></html>",
                           DATA) is None


def test_page_target():
    def scope(path, query=b"", method="GET"):
        return {"method": method, "path": path, "raw_path": path.encode(),
                "query_string": query}
    assert repo_ssr.page_target(scope("/forkmesh/forkmesh")) == (
        "forkmesh", "forkmesh")
    assert repo_ssr.page_target(scope("/Owner/Repo/")) == ("owner", "Repo")
    for path in ("/dashboard/repos", "/api/repositories", "/a/b/tree/x",
                 "/a", "/a/b.git", "/assets/logo.png"):
        assert repo_ssr.page_target(scope(path)) is None, path
    assert repo_ssr.page_target(scope("/a/b", b"viewer=x")) is None
    assert repo_ssr.page_target(scope("/a/b", method="POST")) is None
    assert repo_ssr.start(None, scope("/a/b")) is None


def test_about_micro_cache_serves_anonymous_reads_from_memory(monkeypatch):
    import asyncio
    from forkmesh_host import js

    calls = []

    async def worker_fetch(request):
        calls.append(request)
        return js.Response(b'{"ok":true,"n":%d}' % len(calls), 200, "",
                           js.Headers({"content-type": "application/json"}))

    def scope(method="GET", headers=()):
        return {"method": method, "path": "/api/repo/o/r/about",
                "raw_path": b"/api/repo/o/r/about", "query_string": b"",
                "headers": list(headers)}

    async def body(ticket):
        response = await repo_ssr.fetch(ticket, worker_fetch, "req")
        return await response._read()

    async def main():
        repo_ssr._micro.clear()
        fast = object()
        first = await body(repo_ssr.start(fast, scope()))
        again = await body(repo_ssr.start(fast, scope()))
        assert first == again == b'{"ok":true,"n":1}'
        assert len(calls) == 1
        # Signed-in visitors always reach the Worker.
        signed = repo_ssr.start(fast, scope(headers=[(b"cookie", b"s=1")]))
        assert not isinstance(signed, repo_ssr.MicroKey)
        # Stale: served at once, refreshed in the background.
        key = repo_ssr.start(fast, scope())
        stamp, *rest = repo_ssr._micro[key]
        repo_ssr._micro[key] = (stamp - repo_ssr.MICRO_FRESH_SECONDS - 1,
                                *rest)
        assert await body(key) == b'{"ok":true,"n":1}'
        await asyncio.sleep(0.01)
        assert len(calls) == 2
        assert await body(key) == b'{"ok":true,"n":2}'
        # A write through this node drops the entry.
        assert repo_ssr.start(fast, scope(method="POST")) is None
        assert key not in repo_ssr._micro
    asyncio.run(main())


def test_a_view_that_misses_its_budget_still_starts_its_bundle():
    import asyncio

    from forkmesh_host import js

    page = repo_page()

    async def main():
        never = asyncio.get_running_loop().create_future()
        ticket = repo_ssr.ViewTicket(never, "", ("forkmesh", "forkmesh", "pull", 320))
        response = js.Response(page.encode("utf-8"), 200, "",
                               {"content-type": "text/html; charset=utf-8",
                                "cache-control": "public, max-age=300"})
        answer = await repo_ssr.finish(ticket, response)
        never.cancel()
        return answer

    answer = asyncio.run(main())
    text = (asyncio.run(answer._read())).decode("utf-8")
    meta = re.search(r"window\.__forkmeshView=(\{.*?\});", text)
    assert meta
    assert json.loads(meta.group(1)) == {
        "base": "/api/repo/forkmesh/forkmesh", "view": "pull", "number": 320,
        "url": "/api/repo/forkmesh/forkmesh/page-reads?view=pull&number=320"}
    assert text.index("window.__forkmeshView=") < text.index("Early repository reads")
    assert answer.headers.get("cache-control") == "no-cache"
    assert repo_ssr.PLACEHOLDER in text
    assert repo_ssr.inject_view_reads("<html></html>", "a", "b", "issues", 0) is None
