"""Static files asked for with a content version are cached for a year."""
from __future__ import annotations

import asyncio

from forkmesh_host import js as _js
from forkmesh_host.asgi import (HostASGI, VERSIONED_ASSET_CACHE,
                                WorkerFirstRules, versioned_asset_query)
from forkmesh_host.bindings import StaticAssets


class _Env:
    FORKMESH_PLATFORM = "vps"
    APP_ORIGIN = "https://app.example.test"
    WORLD_ORIGIN = "https://world.example.test"


class _Runtime:
    def __init__(self, assets):
        self.env = _Env()
        self.env.ASSETS = assets
        self.sealed = False
        self.durable = None
        self.fetched = []

    async def fetch(self, request):
        self.fetched.append(request.url)
        return _js.Response(b'{"ok":true}', 200, "", _js.Headers(
            {"content-type": "application/json",
             "cache-control": "no-store"}))


def _scope(path, query=b"", headers=()):
    return {"type": "http", "method": "GET", "path": path,
            "raw_path": path.encode(), "query_string": query,
            "headers": [(b"host", b"app.example.test"), *headers],
            "client": ("127.0.0.1", 50000), "scheme": "http",
            "http_version": "1.1"}


def _get(runtime, path, query=b"", headers=()):
    app = HostASGI(runtime, serve_assets=True,
                   worker_first=WorkerFirstRules(["/api/*"]))
    sent = []

    async def receive():
        return {"type": "http.request", "body": b"", "more_body": False}

    async def send(message):
        sent.append(message)

    asyncio.run(app(_scope(path, query, headers), receive, send))
    found = {}
    for name, value in sent[0]["headers"]:
        found[name.decode()] = value.decode()
    return sent[0]["status"], found


def _assets(tmp_path):
    (tmp_path / "app.js").write_text("console.log(1)")
    (tmp_path / "_headers").write_text(
        "/vendor/*\n  Cache-Control: public, max-age=31536000, immutable\n")
    return StaticAssets(tmp_path, "auto-trailing-slash", "none")


def test_versioned_query_is_one_v_token():
    assert versioned_asset_query({"query_string": b"v=3f9c0a1b2c3d"})
    assert versioned_asset_query({"query_string": b"v=1.2.3"})
    assert not versioned_asset_query({"query_string": b""})
    assert not versioned_asset_query({"query_string": b"v="})
    assert not versioned_asset_query({"query_string": b"v=a&x=1"})
    assert not versioned_asset_query({"query_string": b"version=abc"})
    assert not versioned_asset_query({"query_string": b"v=%3Cscript%3E"})
    assert not versioned_asset_query({"query_string": b"v=" + b"a" * 65})


def test_versioned_asset_is_immutable(tmp_path):
    status, headers = _get(_Runtime(_assets(tmp_path)), "/app.js",
                           b"v=3f9c0a1b2c3d")
    assert status == 200
    assert headers["cache-control"] == VERSIONED_ASSET_CACHE


def test_revalidated_versioned_asset_keeps_immutable(tmp_path):
    assets = _assets(tmp_path)
    etag = assets.serve("/app.js").headers.get("etag")
    status, headers = _get(_Runtime(assets), "/app.js", b"v=3f9c0a1b2c3d",
                           [(b"if-none-match", etag.encode())])
    assert status == 304
    assert headers["cache-control"] == VERSIONED_ASSET_CACHE


def test_unversioned_asset_still_revalidates(tmp_path):
    status, headers = _get(_Runtime(_assets(tmp_path)), "/app.js")
    assert status == 200
    assert headers["cache-control"] == "public, max-age=0, must-revalidate"


def test_api_response_with_v_query_is_not_touched(tmp_path):
    runtime = _Runtime(_assets(tmp_path))
    status, headers = _get(runtime, "/api/instance", b"v=abc")
    assert status == 200 and runtime.fetched
    assert headers["cache-control"] == "no-store"
