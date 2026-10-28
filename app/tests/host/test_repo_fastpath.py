"""Node-local repository read fast path (forkmesh_host.repo_fastpath).

A stub gateway on loopback stands in for tools/mirror_gateway.py; the
query-building test compares against the Worker's own router function so the
fast path keeps sending the gateway exactly what the routed read sends.
"""

from __future__ import annotations

import asyncio
import importlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

import pytest

from forkmesh_host.repo_fastpath import (
    IMMUTABLE,
    RepoFastPath,
    configured_base_url,
    gateway_query,
)

COMMIT = "c" * 40
TREE = {"ok": True, "commit": COMMIT, "entries": [
    {"name": "README.md", "type": "blob", "size": 5}], "truncated": False}


class StubGateway(BaseHTTPRequestHandler):
    seen: list = []
    status = 200
    raw_type = ("image/png", "logo.png")

    def log_message(self, *_args):
        pass

    def do_GET(self):  # noqa: N802
        StubGateway.seen.append(self.path)
        parts = urlsplit(self.path)
        if parts.path == "/health":
            return self._json({"ok": True, "node": "fmnext-n9"})
        if parts.path == "/v1/public/repositories/forkmesh/forkmesh/branches":
            return self._json({"ok": True, "defaultBranch": "main",
                               "branches": [{"name": "main",
                                             "commit": COMMIT}]})
        if parts.path.startswith("/v1/public/repositories/forkmesh/"
                                 "forkmesh/") and StubGateway.status == 200:
            op = parts.path.rsplit("/", 1)[1]
            if op == "raw":
                body = b"\x89PNG-bytes"
                media, name = StubGateway.raw_type
                self.send_response(200)
                self.send_header("Content-Type", media)
                self.send_header("Content-Disposition",
                                 'inline; filename="%s"' % name)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
                return None
            return self._json(dict(TREE, op=op, query=parts.query))
        return self._json({"ok": False, "error": "not_found"},
                          404 if StubGateway.status == 200 else
                          StubGateway.status)

    def _json(self, value, status=200):
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


@pytest.fixture()
def gateway():
    StubGateway.seen = []
    StubGateway.status = 200
    StubGateway.raw_type = ("image/png", "logo.png")
    server =ThreadingHTTPServer(("127.0.0.1", 0), StubGateway)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield "http://127.0.0.1:%d" % server.server_address[1]
    server.shutdown()
    server.server_close()


def scope(path, query="", headers=(), method="GET"):
    return {"type": "http", "method": method, "path": path,
            "raw_path": path.encode(), "query_string": query.encode(),
            "headers": [(k.encode(), v.encode()) for k, v in headers]}


def _fast_path(base):
    import httpx
    return RepoFastPath(base, client=httpx.AsyncClient(
        base_url=base, timeout=httpx.Timeout(20.0, connect=10.0),
        trust_env=False))


def call(base, sc):
    async def main():
        fast = _fast_path(base)
        sent = []

        async def send(message):
            sent.append(message)
        try:
            handled = await fast.handle(sc, send)
        finally:
            await fast.aclose()
        return handled, sent
    handled, sent = asyncio.run(main())
    if not handled:
        return None
    start = sent[0]
    headers = {k.decode(): v.decode() for k, v in start["headers"]}
    body = b"".join(m.get("body", b"") for m in sent[1:])
    return start["status"], headers, body


def test_tree_read_is_served_from_the_gateway_verbatim(gateway):
    status, headers, body = call(gateway, scope(
        "/api/repo/forkmesh/forkmesh/tree", "path=&ref=main&fmv=17"))
    assert status == 200
    data = json.loads(body)
    assert data["op"] == "tree"
    # fmv (a browser cache-buster) never reaches the gateway.
    assert data["query"] == "path=&ref=main"
    assert headers["x-forkmesh-served-by"] == "fmnext-n9"
    assert headers["x-forkmesh-local-reads"] == "1"
    assert headers["cache-control"] == "no-cache"
    assert headers["etag"].startswith('"fm-')
    assert headers["content-length"] == str(len(body))
    # A ref was given, so no default-branch label is added.
    assert "x-forkmesh-ref" not in headers


def test_ref_less_tree_names_the_default_branch(gateway):
    status, headers, _body = call(gateway, scope(
        "/api/repo/forkmesh/forkmesh/tree", "path="))
    assert status == 200
    assert headers["x-forkmesh-ref"] == "main"


def test_unchanged_read_answers_304(gateway):
    _status, headers, _body = call(gateway, scope(
        "/api/repo/forkmesh/forkmesh/blob", "path=README.md&ref=main"))
    etag = headers["etag"]
    for spelling in (etag, "W/" + etag, etag[:-1] + '-zstd"'):
        status, again, body = call(gateway, scope(
            "/api/repo/forkmesh/forkmesh/blob", "path=README.md&ref=main",
            headers=[("if-none-match", spelling)]))
        assert status == 304 and body == b""
        assert again["etag"] == etag


def test_commit_pinned_read_is_immutable(gateway):
    _status, headers, _body = call(gateway, scope(
        "/api/repo/forkmesh/forkmesh/blob",
        "path=README.md&ref=" + COMMIT))
    assert headers["cache-control"] == IMMUTABLE
    _status, headers, body = call(gateway, scope(
        "/api/repo/forkmesh/forkmesh/raw", "path=logo.png&ref=" + COMMIT))
    assert body == b"\x89PNG-bytes"
    assert headers["content-type"] == "image/png"
    assert headers["content-disposition"] == 'inline; filename="logo.png"'
    assert headers["cache-control"] == IMMUTABLE


def test_raw_active_content_is_a_sandboxed_download(gateway):
    for media, name in (("application/atom+xml", "feed.atom"),
                        ("application/rdf+xml", "x.rdf"),
                        ("application/mathml+xml", "x.mml"),
                        ("text/html; charset=utf-8", "x.html"),
                        ("image/svg+xml", "x.svg"),
                        ("multipart/x-mixed-replace; boundary=x", "x.bin")):
        StubGateway.raw_type = (media, name)
        for ref in ("main", COMMIT):
            status, headers, _body = call(gateway, scope(
                "/api/repo/forkmesh/forkmesh/raw",
                "path=%s&ref=%s" % (name, ref)))
            assert status == 200
            assert headers["content-type"] == "application/octet-stream", media
            assert headers["content-disposition"] == "attachment", media
            assert headers["content-security-policy"] == \
                "default-src 'none'; sandbox"


def test_every_fast_path_answer_carries_the_sandbox_policy(gateway):
    _status, headers, _body = call(gateway, scope(
        "/api/repo/forkmesh/forkmesh/raw", "path=logo.png&ref=main"))
    assert headers["content-type"] == "image/png"
    assert headers["content-security-policy"] == "default-src 'none'; sandbox"
    for query in ("path=README.md&ref=main", "path=README.md&ref=" + COMMIT):
        _status, headers, _body = call(gateway, scope(
            "/api/repo/forkmesh/forkmesh/blob", query))
        assert headers["content-security-policy"] == \
            "default-src 'none'; sandbox"


def test_active_content_rule_matches_the_worker_router():
    from forkmesh_host.repo_fastpath import active_content_type
    worker = importlib.import_module("edge_routing").active_content_type
    for media in ("", "text/html", "TEXT/HTML; charset=utf-8",
                  "application/atom+xml", "application/xhtml+xml",
                  "image/svg+xml", "text/javascript", "application/ecmascript",
                  "multipart/related", "image/png", "application/pdf",
                  "text/plain; charset=utf-8", "application/json",
                  "application/octet-stream"):
        assert active_content_type(media) == worker(media), media


def run_on_one_fast_path(base, steps):
    from forkmesh_host import repo_views
    repo_views._public.clear()

    async def main():
        fast = _fast_path(base)
        answers = []

        async def serve(sc):
            sent = []

            async def send(message):
                sent.append(message)
            handled = await fast.handle(sc, send)
            if not handled:
                return None
            headers = {k.decode(): v.decode() for k, v in sent[0]["headers"]}
            return sent[0]["status"], headers, b"".join(
                m.get("body", b"") for m in sent[1:])
        try:
            for step in steps:
                answers.append(await step(fast, serve))
        finally:
            await fast.aclose()
        return answers
    try:
        return asyncio.run(main())
    finally:
        repo_views._public.clear()


PINNED = ("/api/repo/forkmesh/forkmesh/blob", "path=README.md&ref=" + COMMIT)
OTHER_PINNED = ("/api/repo/forkmesh/forkmesh/tree", "path=src&ref=" + COMMIT)


def entries_for(fast, prefix="/v1/public/repositories/forkmesh/forkmesh/"):
    return [key for key in fast.immutable._entries if key.startswith(prefix)]


def test_memory_hit_after_the_repository_went_private_falls_through(gateway):
    async def first(fast, serve):
        return await serve(scope(*PINNED))

    async def hidden(fast, serve):
        StubGateway.status = 404
        return await serve(scope(*PINNED)), entries_for(fast)

    answers = run_on_one_fast_path(gateway, [first, hidden])
    assert answers[0][0] == 200
    assert answers[1] == (None, [])


def test_a_gateway_404_rechecks_visibility_before_the_next_memory_hit(gateway):
    async def warm(fast, serve):
        await serve(scope(*PINNED))
        return await serve(scope(*PINNED))

    async def miss(fast, serve):
        StubGateway.status = 404
        return await serve(scope(*OTHER_PINNED))

    async def hit(fast, serve):
        return await serve(scope(*PINNED)), entries_for(fast)

    answers = run_on_one_fast_path(gateway, [warm, miss, hit])
    assert answers[0][0] == 200
    assert answers[1] is None
    assert answers[2] == (None, [])


def test_host_read_of_a_cached_pinned_url_rechecks_visibility(gateway):
    url = ("/v1/public/repositories/forkmesh/forkmesh/blob?"
           "path=README.md&ref=" + COMMIT)

    async def first(fast, serve):
        return (await fast.read(url, immutable=True))[0]

    async def hidden(fast, serve):
        StubGateway.status = 404
        return (await fast.read(url, immutable=True))[0], entries_for(fast)

    assert run_on_one_fast_path(gateway, [first, hidden]) == [200, (404, [])]


def test_an_unreachable_gateway_withholds_but_keeps_memory_entries(gateway):
    async def first(fast, serve):
        return await serve(scope(*PINNED))

    async def down(fast, serve):
        StubGateway.status = 503
        return await serve(scope(*PINNED)), len(entries_for(fast))

    answers = run_on_one_fast_path(gateway, [first, down])
    assert answers[0][0] == 200
    assert answers[1] == (None, 1)


def test_other_repositories_keep_their_memory_entries():
    from forkmesh_host.repo_fastpath import ImmutableCache, url_repository
    cache = ImmutableCache()
    for key in ("/v1/public/repositories/forkmesh/forkmesh/blob?ref=a",
                "/v1/public/repositories/forkmesh/forkmesh-2/blob?ref=a",
                "/v1/public/repositories/forkmesh/forkmesh/tree?ref=a"):
        cache.put(key, "application/json", b"{}")
    cache.drop_prefix("/v1/public/repositories/forkmesh/forkmesh/")
    assert list(cache._entries) == [
        "/v1/public/repositories/forkmesh/forkmesh-2/blob?ref=a"]
    assert cache.bytes == 2
    assert url_repository(
        "/v1/public/repositories/a%2Db/r.x/tree?path=") == ("a-b", "r.x")
    assert url_repository("/v1/public/repositories/a/r") is None


def test_anything_but_200_falls_through_to_the_worker(gateway):
    # Unknown/private repository: the gateway's public route says 404.
    assert call(gateway, scope("/api/repo/someone/private/tree",
                               "path=")) is None
    StubGateway.status = 503
    assert call(gateway, scope("/api/repo/forkmesh/forkmesh/tree",
                               "path=")) is None


def test_requests_the_fast_path_never_takes(gateway):
    before = len(StubGateway.seen)
    for sc in (
        # private-view token
        scope("/api/repo/forkmesh/forkmesh/tree", "path=&viewer=a&sig=b"),
        # not a read the gateway serves publicly
        scope("/api/repo/forkmesh/forkmesh/issues"),
        scope("/api/repo/forkmesh/forkmesh/release-blob", "sha256=ab"),
        scope("/api/repo/forkmesh/forkmesh/tree", method="POST"),
        scope("/api/repo/forkmesh/forkmesh/tree/extra"),
        scope("/api/repo/../forkmesh/tree"),
    ):
        assert call(gateway, sc) is None
    assert len(StubGateway.seen) == before


def test_unreachable_gateway_falls_through():
    assert call("http://127.0.0.1:9", scope(
        "/api/repo/forkmesh/forkmesh/tree", "path=")) is None


def test_configuration():
    assert configured_base_url({}) == ""
    assert configured_base_url({"FORKMESH_NODE_ID": "n1"}) == \
        "http://127.0.0.1:8790"
    assert configured_base_url({"FORKMESH_NODE_ID": "n1",
                                "FORKMESH_HOST_REPO_GATEWAY": "off"}) == ""
    assert configured_base_url({"FORKMESH_HOST_REPO_GATEWAY":
                                "http://127.0.0.1:1/"}) == \
        "http://127.0.0.1:1"


@pytest.fixture(scope="module")
def entry(staged_src):
    del staged_src
    return importlib.import_module("entry")


@pytest.mark.parametrize("operation,query", [
    ("tree", "path=&ref=main&fmv=1"),
    ("tree", "path=./src//x/&ref=dev"),
    ("tree", ""),
    ("blob", "path=README.md&ref=main&viewer=&fmv=2"),
    ("raw", "path=a%20b.png&ref=" + COMMIT),
    ("blobs", "path=a&path=b&ref=main"),
    ("history", "ref=main&all=1&since=1&until=2"),
    ("history", "path=pkg/app&ref=main"),
    ("history", "path=./pkg//app/&ref=main&limit=5"),
    ("commit", "path=" + COMMIT + "&ref=main"),
    ("compare", "base=main&head=dev&ref=x"),
    ("merge-preview", "base=main&head=dev&ref=x"),
    ("merge-preview", "base=" + COMMIT + "&head=feature%2Fx&ref="),
    ("search", "q=needle&ref=main"),
    ("stats", "ref=main"),
    ("pulse", "period=1m&person=alice&ref=main&fmv=3"),
    ("pulse", "period=all&viewer="),
    ("pulse", "view=people&ref=main"),
    ("sizes", ""),
    ("branches", "ref=main"),
])
def test_query_matches_the_worker_router(entry, operation, query):
    from urllib.parse import urlparse
    expected = entry._https_mirror_request_query(
        urlparse("https://app.example/x?" + query), operation)
    pairs = gateway_query(operation, query)
    as_dict = {}
    for key, value in pairs:
        as_dict.setdefault(key, []).append(value)
    normalized = {k: (v if isinstance(v, list) else [v])
                  for k, v in expected.items()}
    assert as_dict == normalized


def test_commit_pinned_reads_are_kept_in_memory(gateway):
    async def main():
        fast = _fast_path(gateway)
        results = []
        try:
            for _ in range(2):
                sent = []

                async def send(message, sent=sent):
                    sent.append(message)
                assert await fast.handle(scope(
                    "/api/repo/forkmesh/forkmesh/compare",
                    "base=%s&head=%s" % (COMMIT, "d" * 40)), send)
                results.append(sent)
        finally:
            await fast.aclose()
        return fast, results
    before = len(StubGateway.seen)
    fast, results = asyncio.run(main())
    compares = [p for p in StubGateway.seen[before:] if "/compare" in p]
    assert len(compares) == 1  # the second answer came from host memory
    assert fast.stats["memory"] == 1
    first, second = (b"".join(m.get("body", b"") for m in sent[1:])
                     for sent in results)
    assert first == second
    headers = {k.decode(): v.decode() for k, v in results[1][0]["headers"]}
    assert headers["cache-control"] == IMMUTABLE


def test_merge_preview_is_served_from_the_gateway(gateway):
    status, headers, body = call(gateway, scope(
        "/api/repo/forkmesh/forkmesh/merge-preview",
        "base=%s&head=feature%%2Fx&ref=" % COMMIT))
    assert status == 200
    assert headers["cache-control"] == "no-cache"
    assert (
        "/v1/public/repositories/forkmesh/forkmesh/merge-preview"
        "?base=%s&head=feature%%2Fx" % COMMIT) in StubGateway.seen
    assert json.loads(body)["op"] == "merge-preview"


def test_sizes_depth_trims_the_tree():
    from forkmesh_host.repo_fastpath import trim_sizes
    tree = {"ok": True, "name": "", "size": 10, "children": [
        {"name": "a", "size": 6, "type": "directory", "children": [
            {"name": "b", "size": 6, "type": "directory", "children": [
                {"name": "c", "size": 6, "type": "file"}]}]},
        {"name": "d", "size": 4, "type": "file"}]}
    body = json.dumps(tree).encode()
    assert trim_sizes(body, b"ref=main") == body
    assert trim_sizes(body, b"depth=99") == body
    one = json.loads(trim_sizes(body, b"ref=main&depth=1"))
    assert one["children"][0]["children"] == []
    assert one["children"][1] == {"name": "d", "size": 4, "type": "file"}
    two = json.loads(trim_sizes(body, "depth=2"))
    assert two["children"][0]["children"][0]["children"] == []
    assert two["size"] == 10 and two["ok"] is True


def test_public_reads_carry_cors_for_allowed_origins():
    import asyncio as _asyncio
    from forkmesh_host import repo_fastpath as _fp
    sent = []

    async def send(message):
        sent.append(message)

    env = {"FORKMESH_VAR_CORS_ALLOWED_ORIGINS": "https://forkmesh.com,https://app.forkmesh.com"}
    for origin, allowed in (("https://forkmesh.com", True), ("https://evil.example", False)):
        sent.clear()
        wrapped = _fp.with_cors({"headers": [(b"origin", origin.encode())]}, send, env)
        _asyncio.run(wrapped({"type": "http.response.start", "status": 200, "headers": []}))
        headers = dict(sent[0]["headers"])
        assert headers[b"vary"] == b"origin"
        assert (headers.get(b"access-control-allow-origin") == origin.encode()) is allowed
