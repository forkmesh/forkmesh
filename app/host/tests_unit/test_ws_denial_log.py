import asyncio
import logging
import socket
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

uvicorn = pytest.importorskip("uvicorn")
ws_client = pytest.importorskip("websockets.asyncio.client")
from websockets.exceptions import InvalidStatus  # noqa: E402

from forkmesh_host import asgi  # noqa: E402


class _Deny:
    _header_list = staticmethod(asgi.HostASGI._header_list)
    _deny = asgi.HostASGI._deny

    def __init__(self, deny):
        self.deny = deny

    async def __call__(self, scope, receive, send):
        if scope["type"] != "websocket":
            return
        await receive()
        if self.deny:
            await self._deny(scope, send, asgi.HostASGI._plain_response(
                503, "sealed\n"))
            return
        await send({"type": "websocket.accept"})
        await send({"type": "websocket.close", "code": 1000})


class _Capture(logging.Handler):
    def __init__(self):
        super().__init__()
        self.messages = []

    def emit(self, record):
        self.messages.append(record.getMessage())


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


async def _serve_and_connect(app):
    port = _free_port()
    config = uvicorn.Config(app, host="127.0.0.1", port=port,
                            ws="websockets-sansio", lifespan="off",
                            log_config=None, access_log=False)
    server = uvicorn.Server(config)
    task = asyncio.ensure_future(server.serve())
    while not server.started:
        await asyncio.sleep(0.01)
    status = None
    try:
        async with ws_client.connect("ws://127.0.0.1:%d/x" % port):
            status = 101
    except InvalidStatus as error:
        status = error.response.status_code
    await asyncio.sleep(0.05)
    server.should_exit = True
    await task
    return status


def _run(app):
    capture = _Capture()
    logger = logging.getLogger("uvicorn.error")
    logger.addHandler(capture)
    logger.setLevel(logging.INFO)
    try:
        status = asyncio.run(_serve_and_connect(app))
    finally:
        logger.removeHandler(capture)
    return status, capture.messages


def test_http_denial_is_not_logged_as_handshake_error():
    asgi.install_handshake_log_filter()
    status, messages = _run(_Deny(True))
    assert status == 503
    assert asgi.HANDSHAKE_LOG_MESSAGE not in messages


def test_filter_keeps_real_missing_handshake():
    async def silent(scope, receive, send):
        if scope["type"] == "websocket":
            await receive()

    asgi.install_handshake_log_filter()
    status, messages = _run(silent)
    assert status == 500
    assert asgi.HANDSHAKE_LOG_MESSAGE in messages
