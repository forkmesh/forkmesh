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


def short_data():
    return dict(DATA, owner="acme-bolt", repo="Bolt")


def test_a_canonical_short_view_names_and_links_only_the_repository():
    page = repo_ssr.inject(repo_page(), short_data(), short=True)
    view = page[page.index('<div class="fm-ssr"'):]
    assert '<h1 class="fm-ssr-title"><a href="/Bolt">Bolt</a></h1>' in view
    assert 'href="/Bolt/blob/README.md"' in view
    assert 'href="/Bolt/tree/app"' in view
    assert 'href="/Bolt/blob/docs/a.md"' in view
    assert "acme-bolt" not in re.sub(r'src="/api/repo/acme-bolt/Bolt/raw[^"]*"', "", view)
    long = repo_ssr.inject(repo_page(), short_data())
    assert ('<a href="/acme-bolt">acme-bolt</a> / <a href="/acme-bolt/Bolt">Bolt</a>'
            in long)


def test_short_targets_and_the_page_meta():
    def scope(path, query=b"", method="GET"):
        return {"method": method, "path": path, "raw_path": path.encode(),
                "query_string": query}
    assert repo_ssr.short_target(scope("/Bolt")) == "Bolt"
    assert repo_ssr.short_target(scope("/Bolt/")) == "Bolt"
    for path in ("/api", "/dashboard", "/Bolt.git", "/a/b", "/", "/Bolt/issues"):
        assert repo_ssr.short_target(scope(path)) is None, path
    assert repo_ssr.short_target(scope("/Bolt", b"ref=x")) is None
    assert repo_ssr.short_target(scope("/Bolt", method="POST")) is None
    assert isinstance(repo_ssr.start(object(), scope("/Bolt")),
                      repo_ssr.ShortTicket)
    meta = ('<meta name="forkmesh-repo-short" content="Bolt">'
            '<meta name="forkmesh-repo-owner" content="acme-bolt">')
    canonical = '<meta name="forkmesh-repo-canonical" content="acme-bolt">'
    assert repo_ssr.short_page(meta + canonical) == ("acme-bolt", "Bolt", True)
    assert repo_ssr.short_page(meta) == ("acme-bolt", "Bolt", False)
    assert repo_ssr.short_page("<html></html>") is None


def test_a_short_address_page_gets_its_first_view_with_short_links():
    import asyncio

    from forkmesh_host import js

    meta = ('<meta name="forkmesh-repo-short" content="Bolt">'
            '<meta name="forkmesh-repo-owner" content="acme-bolt">'
            '<meta name="forkmesh-repo-canonical" content="acme-bolt">')
    page = repo_page().replace("</head>", meta + "</head>", 1)

    async def main(name):
        loop = asyncio.get_running_loop()
        repo_ssr._prerendered[("acme-bolt", "Bolt")] = (loop.time(), short_data())
        try:
            ticket = repo_ssr.ShortTicket(object(), name)
            response = js.Response(page.encode("utf-8"), 200, "",
                                   {"content-type": "text/html; charset=utf-8"})
            answer = await repo_ssr.finish(ticket, response)
            return answer, (await answer._read()).decode("utf-8")
        finally:
            repo_ssr._prerendered.pop(("acme-bolt", "Bolt"), None)

    answer, text = asyncio.run(main("bolt"))
    assert answer.headers.get("cache-control") == "no-cache"
    assert '<h1 class="fm-ssr-title"><a href="/Bolt">Bolt</a></h1>' in text
    assert 'href="/Bolt/blob/README.md"' in text
    assert json.loads(re.search(
        r'id="forkmesh-repo-ssr">(.*?)</script>', text, re.S).group(1))[
            "owner"] == "acme-bolt"
    _answer, other = asyncio.run(main("Other"))
    assert repo_ssr.PLACEHOLDER in other and "fm-ssr-title" not in other


RAW = "/api/repo/acme-bolt/Bolt/raw?ref=main&path="


def test_readme_html_avatars_render_as_linked_images():
    out = repo_ssr.markdown_html(
        '<a href="https://github.com/alice" title="alice"><img src="https://github.com/alice.png?size=60" '
        'width="50" height="50" alt="alice"></a> <a href="https://github.com/bob">'
        '<img src="https://github.com/bob.png?size=60" width="50" height="50" alt="bob"></a>\n',
        "/Bolt", RAW)
    assert "undefined" not in out
    assert out.count("<img ") == 2
    assert ('<a href="https://github.com/alice"><img src="https://github.com/alice.png?size=60" '
            'alt="alice" width="50" height="50"') in out
    assert 'referrerpolicy="no-referrer"' in out


def test_readme_relative_and_centred_images_use_the_raw_endpoint():
    out = repo_ssr.markdown_html(
        '<p align="center"><img src="bolt-b/logo.png" alt="Logo" width="140" height="140"></p>\n\n'
        "![shot](./docs/a%20b.png) [![badge](https://e.example/b.svg)](https://e.example)\n",
        "/Bolt", RAW)
    assert out.startswith('<div class="text-center"><p><img src="'
                          '/api/repo/acme-bolt/Bolt/raw?ref=main&amp;path=bolt-b%2Flogo.png"')
    assert 'width="140" height="140"' in out
    assert "path=docs%2Fa%20b.png" in out
    assert '<a href="https://e.example"><img src="https://e.example/b.svg" alt="badge"' in out
    assert repo_ssr.markdown_html("![x](logo.png)", "/Bolt") == "<p>x</p>"


def test_readme_raw_html_stays_sanitized():
    out = repo_ssr.markdown_html(
        '<a href="javascript:alert(1)" onclick="x"><img src="javascript:y" onerror="z" alt="j"></a> '
        '<img src=x onerror=alert(5)//> <img src="data:image/png;base64,AAAA" alt="d"> '
        '<svg onload="alert(6)"></svg><iframe src="https://evil.example"></iframe> '
        '<img src="//evil.example/a.png" alt="p"> <a href="https://ok.example" style="color:red">ok</a> '
        '<img src="https://e.example/a.png" alt="q&quot; onload=&quot;r">\n'
        "\x01" + "9\x01 tail\n",
        "/Bolt", RAW)
    for bad in ("javascript:", "onerror", "onclick", "onload=\"", "<svg", "<iframe",
                "data:image", "//evil", "style="):
        assert bad not in out, bad
    assert '<a href="https://ok.example">ok</a>' in out
    assert 'alt="q&quot; onload=&quot;r"' in out
