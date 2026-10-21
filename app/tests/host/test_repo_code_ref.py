"""GitHub-style /tree/<ref>/<path> and /blob/<ref>/<path> links (repo_code_ref)."""

from __future__ import annotations

import asyncio

from forkmesh_host import js, repo_code_ref, repo_ssr


class Answer:
    def __init__(self, status, data):
        self.status_code = status
        self._data = data

    def json(self):
        return self._data


class Client:
    def __init__(self, listings):
        self.listings = listings
        self.calls = []

    async def get(self, url):
        self.calls.append(url)
        if url in self.listings:
            return Answer(200, self.listings[url])
        return Answer(404, {"ok": False})


class FastPath:
    def __init__(self, listings):
        self._client = Client(listings)

    def client(self):
        return self._client


LISTING = {
    "/v1/public/repositories/alice/tools/branches": {
        "ok": True, "defaultBranch": "main",
        "branches": [{"name": "main"}, {"name": "feature/x"},
                     {"name": "dev"}]},
}


def scope(path, query=b"", method="GET"):
    return {"method": method, "path": path, "raw_path": path.encode(),
            "query_string": query}


def clear():
    repo_code_ref._listings.clear()
    repo_code_ref._pending.clear()


def test_split_takes_the_longest_known_ref_prefix():
    refs = {"main", "feature/x", "dev"}
    assert repo_code_ref.split("blob", ["main", "README.md"], refs) == (
        "main", "README.md")
    assert repo_code_ref.split("tree", ["feature", "x", "src"], refs) == (
        "feature/x", "src")
    assert repo_code_ref.split("tree", ["main"], refs) == ("main", "")
    assert repo_code_ref.split("blob", ["main"], refs) is None
    assert repo_code_ref.split("blob", ["docs", "a.md"], refs) is None
    assert repo_code_ref.split("tree", ["feature", "y"], refs) is None


def test_location_keeps_the_app_address_forms():
    assert repo_code_ref.location("/alice/tools", "blob", "main", "README.md",
                                  "main") == "/alice/tools/blob/README.md"
    assert repo_code_ref.location("/alice/tools", "tree", "main", "", "main") \
        == "/alice/tools"
    assert repo_code_ref.location("/tools", "tree", "feature/x", "a b/c",
                                  "main") == "/tools/tree/a%20b/c?ref=feature%2Fx"


def test_targets():
    assert repo_code_ref.long_target(scope("/Alice/tools/blob/main/README.md")) \
        == ("alice", "tools", "/Alice/tools", "blob", "main/README.md")
    assert repo_code_ref.long_target(scope("/alice/tools/blob/x", b"ref=dev")) \
        is None
    assert repo_code_ref.long_target(scope("/alice/tools/blob/x", method="POST")) \
        is None
    assert repo_code_ref.long_target(scope("/alice/tools/issues/1")) is None
    assert repo_code_ref.long_target(scope("/api/tools/tree/x"),
                                     frozenset({"api"})) is None
    assert repo_code_ref.short_code(scope("/tools/tree/main/src")) == (
        "/tools", "tree", "main/src")
    assert repo_code_ref.short_code(scope("/tools/src/app")) is None


def test_redirect_resolves_known_refs_and_leaves_plain_paths():
    async def main():
        clear()
        fast = FastPath(LISTING)
        go = repo_code_ref.redirect
        base = "/alice/tools"
        found = [
            await go(fast, "alice", "tools", base, "blob", "main/README.md"),
            await go(fast, "alice", "tools", base, "tree", "main/docs/guide"),
            await go(fast, "alice", "tools", base, "tree", "main"),
            await go(fast, "alice", "tools", base, "blob", "feature/x/src/a.py"),
            await go(fast, "alice", "tools", base, "tree", "dev"),
            await go(fast, "alice", "tools", base, "blob", "README.md"),
            await go(fast, "alice", "tools", base, "tree", "docs/guide"),
            await go(fast, "alice", "tools", base, "blob", "v1.0/README.md"),
            await go(fast, "bob", "tools", base, "blob", "main/README.md"),
        ]
        calls = list(fast.client().calls)
        clear()
        return found, calls
    found, calls = asyncio.run(main())
    assert found == [
        "/alice/tools/blob/README.md",
        "/alice/tools/tree/docs/guide",
        "/alice/tools",
        "/alice/tools/blob/src/a.py?ref=feature%2Fx",
        "/alice/tools?ref=dev",
        None, None, None, None]
    assert calls.count("/v1/public/repositories/alice/tools/branches") == 1


def test_a_real_folder_named_like_no_ref_stays_a_path():
    async def main():
        clear()
        listing = {"/v1/public/repositories/alice/tools/branches": {
            "defaultBranch": "trunk", "branches": [{"name": "trunk"}]}}
        fast = FastPath(listing)
        answer = await repo_code_ref.redirect(
            fast, "alice", "tools", "/alice/tools", "tree", "main/src")
        clear()
        return answer
    assert asyncio.run(main()) is None


def test_the_host_answers_a_github_link_with_a_308():
    worker = []

    async def worker_fetch(request):
        worker.append(request)
        return js.Response(b"page", 200, "",
                           js.Headers({"content-type": "text/html"}))

    async def main(path, query=b""):
        clear()
        fast = FastPath(LISTING)
        ticket = repo_ssr.start(fast, scope(path, query))
        answer = await repo_ssr.fetch(ticket, worker_fetch, "req")
        answer = await repo_ssr.finish(ticket, answer)
        clear()
        return answer

    moved = asyncio.run(main("/alice/tools/blob/main/README.md"))
    assert int(moved.status) == 308
    assert moved.headers.get("location") == "/alice/tools/blob/README.md"
    assert moved.headers.get("cache-control") == "no-store"
    assert not worker
    plain = asyncio.run(main("/alice/tools/blob/docs/README.md"))
    assert int(plain.status) == 200 and len(worker) == 1
    picked = asyncio.run(main("/alice/tools/blob/main/README.md", b"ref=dev"))
    assert int(picked.status) == 200 and len(worker) == 2


def test_a_short_address_redirects_once_its_page_names_the_owner():
    meta = ('<meta name="forkmesh-repo-short" content="tools">'
            '<meta name="forkmesh-repo-owner" content="alice">')
    page = "<html><head>%s</head><body></body></html>" % meta

    async def worker_fetch(_request):
        return js.Response(page.encode("utf-8"), 200, "",
                           js.Headers({"content-type": "text/html"}))

    async def main(path):
        clear()
        fast = FastPath(LISTING)
        ticket = repo_ssr.start(fast, scope(path))
        assert isinstance(ticket, repo_ssr.ShortTicket)
        answer = await repo_ssr.fetch(ticket, worker_fetch, "req")
        answer = await repo_ssr.finish(ticket, answer)
        clear()
        return answer

    moved = asyncio.run(main("/tools/tree/feature/x/src"))
    assert int(moved.status) == 308
    assert moved.headers.get("location") == "/tools/tree/src?ref=feature%2Fx"
    moved = asyncio.run(main("/tools/blob/main/README.md"))
    assert moved.headers.get("location") == "/tools/blob/README.md"
    plain = asyncio.run(main("/tools/blob/docs/README.md"))
    assert int(plain.status) == 200
