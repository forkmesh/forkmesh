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
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Disposition",
                                 'inline; filename="logo.png"')
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
    server = ThreadingHTTPServer(("127.0.0.1", 0), StubGateway)
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
