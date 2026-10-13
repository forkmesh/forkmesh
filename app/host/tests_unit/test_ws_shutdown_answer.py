import argparse
import asyncio
import socket
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

uvicorn = pytest.importorskip("uvicorn")
ws_client = pytest.importorskip("websockets.asyncio.client")
from websockets.exceptions import ConnectionClosed, InvalidStatus  # noqa: E402

from forkmesh_host import serve  # noqa: E402
from forkmesh_host.ws_protocol import (  # noqa: E402
    DrainingWebSocketProtocol, stopping_protocol)


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class _Held:
    def __init__(self, accept_first):
        self.accept_first = accept_first
        self.reached = asyncio.Event()

    async def __call__(self, scope, receive, send):
        if scope["type"] != "websocket":
            return
        await receive()
        if self.accept_first:
            await send({"type": "websocket.accept"})
        self.reached.set()
        await asyncio.sleep(30)


async def _connect_during_shutdown(app):
    port = _free_port()
    config = uvicorn.Config(app, host="127.0.0.1", port=port,
                            ws=DrainingWebSocketProtocol, lifespan="off",
                            log_config=None, access_log=False,
                            timeout_graceful_shutdown=1)
    server = uvicorn.Server(config)
    task = asyncio.ensure_future(server.serve())
    while not server.started:
        await asyncio.sleep(0.01)

    async def client():
        try:
            async with ws_client.connect(
                    "ws://127.0.0.1:%d/api/repo/a/b/rooms/general/ws" % port,
                    open_timeout=5) as socket_:
                try:
                    await socket_.recv()
                except ConnectionClosed as closed:
                    return 101, closed.rcvd.code if closed.rcvd else None
        except InvalidStatus as error:
            return error.response.status_code, error.response.headers

    attempt = asyncio.ensure_future(client())
    await asyncio.wait_for(app.reached.wait(), 5)
    server.should_exit = True
    outcome = await asyncio.wait_for(attempt, 5)
    await task
    return outcome


def test_pending_handshake_gets_expected_degraded_503_on_shutdown():
    status, headers = asyncio.run(_connect_during_shutdown(_Held(False)))
    assert status == 503
    assert headers["Retry-After"] == "5"
    assert headers["X-Forkmesh-Expected-Degraded"] == "1"


def test_open_socket_still_closes_with_1012_on_shutdown():
    status, code = asyncio.run(_connect_during_shutdown(_Held(True)))
    assert status == 101
    assert code == 1012


def test_serve_uses_draining_protocol(monkeypatch):
    seen = {}
    monkeypatch.setattr(uvicorn, "run",
                        lambda app, **kwargs: seen.update(kwargs))
    monkeypatch.setattr(serve, "make_app", lambda args: object())
    args = argparse.Namespace(
        no_build=True, host="127.0.0.1", port=0, log_level="info",
        access_log=False)
    assert serve.cmd_serve(args) == 0
    assert seen["ws"] is DrainingWebSocketProtocol


class _HeldClaim:
    def __init__(self, stopping):
        self.stopping = stopping
        self.reached = asyncio.Event()

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return
        await receive()
        self.reached.set()
        try:
            await asyncio.wait_for(self.stopping.wait(), 30)
        except asyncio.TimeoutError:
            pass
        await send({"type": "http.response.start", "status": 200,
                    "headers": [(b"content-type", b"application/json")]})
        await send({"type": "http.response.body",
                    "body": b'{"jobs":[]}'})


def test_shutdown_wakes_a_held_request_so_the_app_exits_promptly():
    async def scenario():
        stopping = asyncio.Event()
        app = _HeldClaim(stopping)
        from uvicorn.protocols.http.h11_impl import H11Protocol
        port = _free_port()
        config = uvicorn.Config(app, host="127.0.0.1", port=port,
                                http=stopping_protocol(H11Protocol, stopping),
                                lifespan="off", log_config=None,
                                access_log=False)
        server = uvicorn.Server(config)
        task = asyncio.ensure_future(server.serve())
        while not server.started:
            await asyncio.sleep(0.01)
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        writer.write(b"POST /api/runners/claim HTTP/1.1\r\nHost: x\r\n"
                     b"Content-Length: 0\r\n\r\n")
        await writer.drain()
        await asyncio.wait_for(app.reached.wait(), 5)
        loop = asyncio.get_running_loop()
        started = loop.time()
        server.should_exit = True
        answer = await asyncio.wait_for(reader.read(), 5)
        await asyncio.wait_for(task, 5)
        writer.close()
        return answer, loop.time() - started, stopping.is_set()

    answer, elapsed, stopped = asyncio.run(scenario())
    assert answer.startswith(b"HTTP/1.1 200")
    assert b'{"jobs":[]}' in answer
    assert stopped
    assert elapsed < 2


def test_serve_sets_host_stopping_from_the_http_protocol(monkeypatch):
    seen = {}
    stopping = asyncio.Event()
    app = argparse.Namespace(
        runtime=argparse.Namespace(
            env=argparse.Namespace(HOST_STOPPING=stopping)))
    monkeypatch.setattr(uvicorn, "run",
                        lambda app, **kwargs: seen.update(kwargs))
    monkeypatch.setattr(serve, "make_app", lambda args: app)
    args = argparse.Namespace(
        no_build=True, host="127.0.0.1", port=0, log_level="info",
        access_log=False)
    assert serve.cmd_serve(args) == 0
    protocol = seen["http"]
    assert protocol.__name__.startswith("Stopping")
    instance = protocol.__new__(protocol)
    instance.cycle = None
    instance.transport = argparse.Namespace(close=lambda: None)
    instance.shutdown()
    assert stopping.is_set()
