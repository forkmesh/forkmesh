from __future__ import annotations

import asyncio
import re

from forkmesh_host import d1_caller
from forkmesh_host import js as _js
from forkmesh_host.asgi import HostASGI, WorkerFirstRules, server_timing_headers
from forkmesh_host.d1_sqlite import D1Sqlite


class _Env:
    FORKMESH_PLATFORM = "vps"
    APP_ORIGIN = "https://app.next.forkmesh.com"
    WORLD_ORIGIN = "https://world.next.forkmesh.com"


class _Stream:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.cancelled = False

    def __aiter__(self):
        return self._iterate()

    async def _iterate(self):
        for chunk in self.chunks:
            yield chunk

    async def cancel(self):
        self.cancelled = True


class _Runtime:
    def __init__(self, queries=0, headers=None, stream=None):
        self.env = _Env()
        self.sealed = False
        self.durable = None
        self.db = D1Sqlite()
        self.queries = queries
        self.headers = headers or {}
        self.stream = stream

    async def fetch(self, request):
        with d1_caller.request_scope(request):
            for _ in range(self.queries):
                await self.db.prepare("SELECT 1 AS one").first()
        headers = {"content-type": "application/json"}
        headers.update(self.headers)
        body = self.stream if self.stream is not None else b"{}"
        return _js.Response(body, 200, "", _js.Headers(headers))


def _scope(path, method="GET"):
    return {"type": "http", "method": method, "path": path,
            "raw_path": path.encode(), "query_string": b"",
            "headers": [(b"host", b"app.next.forkmesh.com")],
            "client": ("127.0.0.1", 50000), "scheme": "http",
            "http_version": "1.1"}


def _call(runtime, path, method="GET", monkeypatch=None):
    if monkeypatch is not None:
        monkeypatch.delenv("FORKMESH_PUBLIC_HOSTS", raising=False)
    app = HostASGI(runtime, serve_assets=False,
                   worker_first=WorkerFirstRules([]))
    sent = []

    async def receive():
        return {"type": "http.request", "body": b"", "more_body": False}

    async def send(message):
        sent.append(message)

    asyncio.run(app(_scope(path, method), receive, send))
    headers = {}
    for name, value in sent[0]["headers"]:
        headers.setdefault(name.decode(), []).append(value.decode())
    body = b"".join(m.get("body", b"") for m in sent[1:])
    return sent[0]["status"], headers, body


def test_api_responses_carry_app_and_db_timing(monkeypatch):
    status, headers, body = _call(_Runtime(queries=3), "/api/world/ticket",
                                  monkeypatch=monkeypatch)
    assert status == 200 and body == b"{}"
    [timing] = headers["server-timing"]
    assert re.fullmatch(
        r'app;dur=\d+\.\d, db;dur=\d+\.\d;desc="3 queries"', timing)
    assert "timing-allow-origin" not in headers


def test_a_request_without_queries_reports_only_app_time(monkeypatch):
    _, headers, _ = _call(_Runtime(), "/api/world/context",
                          monkeypatch=monkeypatch)
    assert re.fullmatch(r"app;dur=\d+\.\d", headers["server-timing"][0])


def test_one_query_is_singular(monkeypatch):
    _, headers, _ = _call(_Runtime(queries=1), "/api/x",
                          monkeypatch=monkeypatch)
    assert headers["server-timing"][0].endswith('desc="1 query"')


def test_non_api_paths_get_no_timing(monkeypatch):
    _, headers, _ = _call(_Runtime(queries=2), "/world",
                          monkeypatch=monkeypatch)
    assert "server-timing" not in headers


def test_cors_allowed_origins_may_read_the_timing(monkeypatch):
    runtime = _Runtime(headers={
        "access-control-allow-origin": "https://world.next.forkmesh.com"})
    _, headers, _ = _call(runtime, "/api/accounts/users",
                          monkeypatch=monkeypatch)
    assert headers["timing-allow-origin"] == [
        "https://world.next.forkmesh.com"]


def test_streamed_bodies_keep_streaming_with_the_header(monkeypatch):
    stream = _Stream([b"a", b"b"])
    status, headers, body = _call(_Runtime(stream=stream), "/api/stream",
                                  monkeypatch=monkeypatch)
    assert status == 200 and body == b"ab"
    assert "server-timing" in headers and "content-length" not in headers


def test_head_keeps_the_header(monkeypatch):
    status, headers, body = _call(_Runtime(queries=1), "/api/x", "HEAD",
                                  monkeypatch=monkeypatch)
    assert status == 200 and body == b""
    assert "server-timing" in headers


def test_background_queries_do_not_count_toward_the_request():
    async def scenario():
        with d1_caller.scope():
            db = D1Sqlite()
            await db.prepare("SELECT 1").first()
            with d1_caller.scope("background x"):
                await db.prepare("SELECT 1").first()
                await db.prepare("SELECT 1").first()
            return d1_caller.timing()

    count, elapsed = asyncio.run(scenario())
    assert count == 1 and elapsed >= 0
    assert d1_caller.timing() == (0, 0.0)


def test_header_builder_keeps_an_existing_timing_allow_origin():
    response = _js.Response(b"", 200, "", _js.Headers({
        "access-control-allow-origin": "https://a.example",
        "timing-allow-origin": "*"}))
    out = dict(server_timing_headers(response, 12.34, 0, 0.0))
    assert out == {b"server-timing": b"app;dur=12.3"}
