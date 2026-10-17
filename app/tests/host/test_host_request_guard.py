"""ASGI front-door guards (audit 2026-09-24): Host allowlist (H5), body cap
(H6), signed edge client (M5), uvicorn limits."""

from __future__ import annotations

import asyncio
import logging
import time
import tracemalloc

import pytest

from forkmesh_host import js as _js
from forkmesh_host import asgi, request_guard, serve
from forkmesh_host.asgi import HostASGI, WorkerFirstRules

MiB = 1024 * 1024


class _Env:
    FORKMESH_PLATFORM = "vps"
    APP_ORIGIN = "https://app.next.forkmesh.com"
    WORLD_ORIGIN = "https://world.next.forkmesh.com"
    WWW_EDGE_SECRET = "edge-secret-for-tests"


class _Runtime:
    def __init__(self, env=None, read_body=True):
        self.env = env or _Env()
        self.sealed = False
        self.durable = None
        self.seen = []
        self.read_body = read_body

    async def fetch(self, request):
        self.seen.append(request)
        size = 0
        if self.read_body and request.method not in ("GET", "HEAD"):
            # The Worker reads the whole body, like request.json() does.
            size = len(await request.arrayBuffer())
        return _js.Response(("ok %d" % size).encode(), 200, "",
                            _js.Headers({"content-type": "text/plain"}))


def _app(runtime=None, monkeypatch=None):
    if monkeypatch is not None:
        monkeypatch.delenv("FORKMESH_PUBLIC_HOSTS", raising=False)
    return HostASGI(runtime or _Runtime(), serve_assets=False,
                    worker_first=WorkerFirstRules([]))


def _scope(path="/api/x", host="app.next.forkmesh.com", method="GET",
           client=("127.0.0.1", 50000), headers=()):
    raw = [(b"host", host.encode())] if host is not None else []
    raw += [(k.encode(), v.encode()) for k, v in headers]
    return {"type": "http", "method": method, "path": path,
            "raw_path": path.encode(), "query_string": b"", "headers": raw,
            "client": client, "scheme": "http", "http_version": "1.1"}


def _call(app, scope, chunks=(), counter=None):
    sent = []
    iterator = iter(chunks)

    async def receive():
        if counter is not None:
            counter["n"] += 1
        try:
            chunk = next(iterator)
        except StopIteration:
            return {"type": "http.request", "body": b"", "more_body": False}
        return {"type": "http.request", "body": chunk, "more_body": True}

    async def send(message):
        sent.append(message)

    asyncio.run(app(scope, receive, send))
    status = sent[0]["status"]
    body = b"".join(m.get("body", b"") for m in sent[1:])
    return status, body


# -- Host allowlist ------------------------------------------------------------

@pytest.mark.parametrize("host", ["app.next.forkmesh.com",
                                  "world.next.forkmesh.com:443",
                                  "APP.NEXT.FORKMESH.COM."])
def test_public_hosts_pass(host, monkeypatch):
    status, _ = _call(_app(monkeypatch=monkeypatch), _scope(host=host))
    assert status == 200


@pytest.mark.parametrize("host", ["localhost", "127.0.0.1:8080", "[::1]:8080",
                                  "203.0.113.11", "evil.example",
                                  "app.forkmesh.com", ""])
def test_other_hosts_are_misdirected(host, monkeypatch):
    runtime = _Runtime()
    status, _ = _call(_app(runtime, monkeypatch),
                      _scope(host=host or None))
    assert status == 421
    assert runtime.seen == []  # the Worker never saw it


def test_demo_bootstrap_shape_is_refused(monkeypatch):
    """H5: a local Host header on the login path never reaches the Worker."""
    runtime = _Runtime()
    status, _ = _call(_app(runtime, monkeypatch), _scope(
        "/api/login", host="localhost:8787", method="POST"))
    assert status == 421 and runtime.seen == []


def test_loopback_health_probes_are_exempt(monkeypatch):
    runtime = _Runtime()
    app = _app(runtime, monkeypatch)
    status, _ = _call(app, _scope("/api/version", host="127.0.0.1:8080"))
    assert status == 200
    status, _ = _call(app, _scope("/health", host="127.0.0.1:8080"))
    assert status == 200
    # Only GET/HEAD /api/version, and only from loopback.
    status, _ = _call(app, _scope("/api/version", host="127.0.0.1:8080",
                                  method="POST"))
    assert status == 421
    status, _ = _call(app, _scope("/health", host="127.0.0.1",
                                  client=("203.0.113.9", 1)))
    assert status == 421


def test_public_hosts_env_adds_node_names(monkeypatch):
    monkeypatch.setenv("FORKMESH_PUBLIC_HOSTS",
                       "app.next.forkmesh.com,n2.next.forkmesh.com")
    app = HostASGI(_Runtime(), serve_assets=False,
                   worker_first=WorkerFirstRules([]))
    assert _call(app, _scope(host="n2.next.forkmesh.com"))[0] == 200
    assert _call(app, _scope(host="n4.next.forkmesh.com"))[0] == 421


def test_not_enforced_off_vps_without_hosts(monkeypatch):
    env = _Env()
    env.FORKMESH_PLATFORM = "dev"
    app = _app(_Runtime(env), monkeypatch)
    assert _call(app, _scope(host="localhost:8787"))[0] == 200


def test_websocket_with_foreign_host_is_denied(monkeypatch):
    runtime = _Runtime()
    app = _app(runtime, monkeypatch)
    sent = []
    scope = dict(_scope("/api/status/ws", host="localhost"), type="websocket")

    async def receive():
        return {"type": "websocket.connect"}

    async def send(message):
        sent.append(message)

    asyncio.run(app(scope, receive, send))
    assert sent == [{"type": "websocket.close", "code": 1008}]
    assert runtime.seen == []


def _handshake_record():
    return logging.LogRecord("uvicorn.error", logging.ERROR, __file__, 0,
                             asgi.HANDSHAKE_LOG_MESSAGE, None, None)


def test_http_denied_websocket_silences_only_its_own_handshake_error(
        monkeypatch):
    runtime = _Runtime()
    runtime.sealed = True
    app = _app(runtime, monkeypatch)
    handshake_filter = asgi.install_handshake_log_filter()
    assert asgi.install_handshake_log_filter() is handshake_filter
    assert logging.getLogger("uvicorn.error").filters.count(
        handshake_filter) == 1
    scope = dict(_scope("/api/status/ws"), type="websocket",
                 extensions={"websocket.http.response": {}})
    sent = []

    async def receive():
        return {"type": "websocket.connect"}

    async def send(message):
        sent.append(message)

    async def connection():
        await app(scope, receive, send)
        return handshake_filter.filter(_handshake_record())

    async def untouched_connection():
        return handshake_filter.filter(_handshake_record())

    async def both():
        denied = await asyncio.ensure_future(connection())
        other = await asyncio.ensure_future(untouched_connection())
        return denied, other

    denied, other = asyncio.run(both())
    assert [m["type"] for m in sent] == ["websocket.http.response.start",
                                         "websocket.http.response.body"]
    assert sent[0]["status"] == 503
    assert denied is False
    assert other is True
    unrelated = logging.LogRecord("uvicorn.error", logging.ERROR, __file__,
                                  0, "Exception in ASGI application", None,
                                  None)
    assert handshake_filter.filter(unrelated) is True


# -- body cap -----------------------------------------------------------------

def test_declared_length_over_cap_is_refused_unread(monkeypatch):
    runtime = _Runtime()
    counter = {"n": 0}
    status, _ = _call(_app(runtime, monkeypatch), _scope(
        method="POST", headers=[("content-length", str(300 * MiB))]),
        chunks=[b"x"], counter=counter)
    assert status == 413
    assert counter["n"] == 0 and runtime.seen == []


def test_bad_content_length_is_400(monkeypatch):
    status, _ = _call(_app(monkeypatch=monkeypatch), _scope(
        method="POST", headers=[("content-length", "12, 13")]))
    assert status == 400


def test_streamed_300mb_body_stops_at_the_cap(monkeypatch):
    """300 MB chunked, generated lazily: 413 after ~16 MiB is read, and
    memory stays around the cap, never near the body size."""
    counter = {"n": 0}
    chunk = b"\0" * MiB

    def lazy():
        for _ in range(300):
            yield chunk

    tracemalloc.start()
    try:
        status, _ = _call(_app(_Runtime(), monkeypatch),
                          _scope(method="POST"), chunks=lazy(),
                          counter=counter)
        _, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    assert status == 413
    assert counter["n"] <= 18  # 16 MiB cap + the chunk that crossed it
    assert peak < 64 * MiB


def test_worker_swallowing_the_error_still_gets_413(monkeypatch):
    class Swallow(_Runtime):
        async def fetch(self, request):
            try:
                await request.arrayBuffer()
            except Exception:
                return _js.Response(b"bad json", 400, "", _js.Headers())
            return _js.Response(b"ok", 200, "", _js.Headers())

    status, _ = _call(_app(Swallow(), monkeypatch), _scope(method="POST"),
                      chunks=(b"\0" * MiB for _ in range(40)))
    assert status == 413


def test_body_under_cap_and_pull_exception(monkeypatch):
    app = _app(_Runtime(), monkeypatch)
    status, body = _call(app, _scope(method="POST"),
                         chunks=[b"a" * MiB] * 3)
    assert status == 200 and body == b"ok %d" % (3 * MiB)
    pulls = "/api/repo/alice/demo/pulls"
    status, body = _call(app, _scope(pulls, method="POST"),
                         chunks=[b"a" * MiB] * 20)
    assert status == 200 and body == b"ok %d" % (20 * MiB)
    status, _ = _call(app, _scope(pulls, method="POST"),
                      chunks=(b"a" * MiB for _ in range(40)))
    assert status == 413
    assert request_guard.body_limit("POST", pulls + "/1") == 16 * MiB
    assert request_guard.body_limit("PUT", pulls) == 16 * MiB


def test_client_disconnect_is_not_a_server_error(monkeypatch, capsys):
    app = _app(_Runtime(), monkeypatch)
    sent = []
    messages = iter([
        {"type": "http.request", "body": b"a", "more_body": True},
        {"type": "http.disconnect"},
    ])

    async def receive():
        return next(messages)

    async def send(message):
        sent.append(message)

    asyncio.run(app(_scope("/api/runners/claim", method="POST"),
                    receive, send))
    assert sent[0]["status"] == 499
    assert app.errors == 0
    assert "worker fetch raised" not in capsys.readouterr().out


def test_too_many_fields_from_the_worker_is_400(monkeypatch, capsys):
    import urllib.parse

    class Fields(_Runtime):
        async def fetch(self, request):
            urllib.parse.parse_qs("&".join("f%d=1" % i for i in range(50)),
                                  max_num_fields=40)

    app = _app(Fields(), monkeypatch)
    status, _ = _call(app, _scope())
    assert status == 400
    assert app.errors == 0
    assert "worker fetch raised" not in capsys.readouterr().out


def test_too_many_fields_on_the_repo_fast_path_is_400(monkeypatch):
    from forkmesh_host import repo_views

    class FastPath:
        async def handle(self, scope, send):
            repo_views.parse_request(
                "&".join("f%d=1" % i for i in range(30)))

    app = _app(monkeypatch=monkeypatch)
    app.repo_fastpath = FastPath()
    status, _ = _call(app, _scope("/api/repo/alice/demo/page-reads"))
    assert status == 400


def test_body_limits_are_env_tunable(monkeypatch):
    monkeypatch.setenv("FORKMESH_MAX_BODY_BYTES", "1024")
    assert request_guard.body_limit("POST", "/api/x") == 1024
    monkeypatch.setenv("FORKMESH_MAX_BODY_BYTES", "junk")
    assert request_guard.body_limit("POST", "/api/x") == 16 * MiB


# -- edge client --------------------------------------------------------------

def _edge(ts=None, ip="198.51.100.7", country="DE", method="POST",
          path="/api/attribution/capture", secret=_Env.WWW_EDGE_SECRET):
    ts = int(time.time()) if ts is None else ts
    return request_guard.edge_sign(secret, ip, country, ts, method, path)


def _edge_call(app, value, client=("127.0.0.1", 1), method="POST",
               path="/api/attribution/capture"):
    runtime = app.runtime
    runtime.seen.clear()
    headers = [("cf-connecting-ip", "104.28.0.1")]
    if value is not None:
        headers.append(("x-forkmesh-edge-client", value))
    _call(app, _scope(path, method=method, client=client, headers=headers))
    request = runtime.seen[0]
    assert request.headers.get("x-forkmesh-edge-client") is None
    return (request.headers.get("cf-connecting-ip"),
            request.headers.get("cf-ipcountry"))


def test_valid_edge_client_replaces_the_address(monkeypatch):
    app = _app(_Runtime(), monkeypatch)
    assert _edge_call(app, _edge()) == ("198.51.100.7", "DE")


def test_edge_client_replay_is_ignored(monkeypatch):
    app = _app(_Runtime(), monkeypatch)
    value = _edge()
    assert _edge_call(app, value)[0] == "198.51.100.7"
    assert _edge_call(app, value)[0] == "104.28.0.1"


@pytest.mark.parametrize("make_value", [
    lambda: _edge(secret="wrong"),
    lambda: _edge(ts=int(time.time()) - 301),
    lambda: _edge(ts=int(time.time()) + 400),
    lambda: _edge(path="/api/login"),
    lambda: _edge(method="GET"),
    lambda: _edge(country="de"),
    lambda: "v2,1.2.3.4,DE,1,abc",
    lambda: "v1,not-an-ip,DE,%d,abc" % int(time.time()),
    lambda: "garbage",
], ids=["wrong-secret", "too-old", "too-far-ahead", "other-path", "other-method",
        "lowercase-country", "unknown-version", "bad-ip", "garbage"])
def test_invalid_edge_client_keeps_caddys_address(make_value, monkeypatch):
    app = _app(_Runtime(), monkeypatch)
    assert _edge_call(app, make_value()) == ("104.28.0.1", None)


def test_edge_client_only_through_caddy(monkeypatch):
    app = _app(_Runtime(), monkeypatch)
    # A direct (non-loopback) peer: stripped, never trusted.
    assert _edge_call(app, _edge(), client=("203.0.113.5", 1))[0] \
        == "104.28.0.1"


def test_edge_client_needs_the_secret(monkeypatch):
    env = _Env()
    env.WWW_EDGE_SECRET = ""
    app = _app(_Runtime(env), monkeypatch)
    assert _edge_call(app, _edge())[0] == "104.28.0.1"


# -- uvicorn limits ------------------------------------------------------------

def test_server_limits():
    limits = serve.server_limits({})
    assert limits == {"limit_concurrency": 512,
                      "h11_max_incomplete_event_size": 64 * 1024,
                      "timeout_keep_alive": 15,
                      "ws_max_size": 4 * MiB}
    assert serve.server_limits({"FORKMESH_HOST_LIMIT_CONCURRENCY": "50"})[
        "limit_concurrency"] == 50
    import inspect

    import uvicorn
    accepted = inspect.signature(uvicorn.Config.__init__).parameters
    assert set(limits) <= set(accepted)
