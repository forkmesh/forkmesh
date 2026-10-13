"""world.next /health/mobile on the host (forkmesh_host.world_health).

The answer must pass the app's own ``world_mobile`` status probe
(app/src/entry.py ``_mobile_world_status_probe``), which is run here
unchanged against the host's response.
"""

from __future__ import annotations

import asyncio
import importlib
import json
import re
from pathlib import Path

import pytest

from forkmesh_host import world_health
from forkmesh_host.asgi import HostASGI, WorkerFirstRules
from forkmesh_host.env import Env

REPO = Path(__file__).resolve().parents[3]
WORLD_ORIGIN = "https://world.next.forkmesh.com"
FINGERPRINT = "ab" * 32
SIZES = {
    "/world/index.html": 4_000,
    # The staged build: world.js is the small entry shell, and the authored
    # runtime is published as world-runtime.js.
    "/world/world.js": 3 * 1024,
    "/world/world-runtime.js": 1_300 * 1024,
    "/world/world-scene.js": 780 * 1024,
    "/world/vendor/three-0.184.0/three.module.min.js": 300 * 1024,
    "/world/vendor/three-0.184.0/three.core.min.js": 400 * 1024,
}


def world_health_dependencies():
    return {dependency[0] for dependency in
            world_health.RUNTIME_DEPENDENCIES.values()}


def make_dist(root: Path, sizes=None):
    for path, size in (sizes or SIZES).items():
        file = root / path.lstrip("/")
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(b"x" * size)
    return root


class _Runtime:
    sealed = False

    def __init__(self):
        self.env = Env({"WORLD_ORIGIN": WORLD_ORIGIN,
                        "APP_ORIGIN": "https://app.next.forkmesh.com",
                        "DEPLOY_FINGERPRINT": FINGERPRINT})

    async def fetch(self, request):  # pragma: no cover - must not be reached
        raise AssertionError("reached the Worker entry for %s" % request.url)


def asgi_get(app, host, path="/health/mobile", method="GET"):
    sent = []

    async def receive():
        return {"type": "http.request", "body": b"", "more_body": False}

    async def send(message):
        sent.append(message)
    scope = {"type": "http", "method": method, "path": path,
             "raw_path": path.encode(), "query_string": b"",
             "headers": [(b"host", host.encode())], "scheme": "https",
             "http_version": "1.1", "client": ("127.0.0.1", 1)}
    asyncio.run(app(scope, receive, send))
    start = sent[0]
    headers = {k.decode(): v.decode() for k, v in start["headers"]}
    body = b"".join(m.get("body", b"") for m in sent[1:])
    return start["status"], headers, body


@pytest.fixture
def host_app(tmp_path, monkeypatch):
    monkeypatch.setenv("FORKMESH_HOST_WORLD_DIST", str(make_dist(tmp_path / "dist")))
    return HostASGI(_Runtime(), serve_assets=False, cron=False,
                    worker_first=WorkerFirstRules([])), tmp_path / "dist"


class _ProbeHeaders:
    def __init__(self, headers):
        self._headers = {k.lower(): v for k, v in headers.items()}

    def get(self, name):
        return self._headers.get(name.lower())


class _ProbeResponse:
    def __init__(self, status, headers, body):
        self.status = status
        self.headers = _ProbeHeaders(headers)
        self._body = body

    async def text(self):
        return self._body.decode()


@pytest.fixture(scope="module")
def entry(staged_src):
    del staged_src
    return importlib.import_module("entry")


def run_probe(entry, monkeypatch, status, headers, body):
    """entry.py's own world_mobile probe against one host response."""
    seen = {}

    async def fake_fetch(resource, init, timeout_seconds):
        seen["url"] = resource
        return _ProbeResponse(status, headers, body)
    monkeypatch.setattr(entry, "js_fetch_with_timeout", fake_fetch)

    class ProbeEnv:
        WORLD_ORIGIN = globals()["WORLD_ORIGIN"]
    result = asyncio.run(entry._mobile_world_status_probe(ProbeEnv()))
    assert seen["url"] == WORLD_ORIGIN + "/health/mobile"
    return result


def test_contracts_match_world_worker_and_the_app_probe(entry):
    source = (REPO / "world" / "worker.js").read_text()
    block = source[source.index("MOBILE_WORLD_RUNTIME_ASSETS"):]
    block = block[:block.index("]);")]
    contracts = []
    for item in re.findall(r"\{(.*?)\}", block, re.S):
        path = re.search(r'path:\s*"([^"]+)"', item).group(1)
        kind = re.search(r'contentType:\s*"([^"]+)"', item).group(1)
        numbers = []
        for key in ("minimumBytes", "maximumBytes"):
            expr = re.search(key + r":\s*([0-9_ *]+)", item).group(1)
            value = 1
            for factor in expr.replace("_", "").split("*"):
                value *= int(factor.strip())
            numbers.append(value)
        contracts.append((path, kind, *numbers))
    # Same paths and types as world/worker.js; the only deliberate change is
    # that world.js is now the staged entry shell, and the worker's
    # world.js bounds moved to the runtime the shell loads.
    ours = dict((c[0], c) for c in world_health.MOBILE_WORLD_RUNTIME_ASSETS)
    for path, kind, minimum, maximum in contracts:
        if path == "/world/world.js":
            runtime = world_health.RUNTIME_DEPENDENCIES[path]
            assert runtime[1:] == (kind, minimum, maximum)
            assert ours[path][1] == kind and ours[path][3] <= 64 * 1024
        else:
            assert ours[path] == (path, kind, minimum, maximum)
    assert tuple(c[0] for c in contracts) == entry.MOBILE_WORLD_HEALTH_ASSETS


def test_healthy_dist_passes_the_app_probe(host_app, entry, monkeypatch):
    app, _ = host_app
    status, headers, body = asgi_get(app, "world.next.forkmesh.com")
    assert status == 200
    assert headers["x-forkmesh-worker"] == "world"
    assert headers["x-forkmesh-deploy-fingerprint"] == FINGERPRINT
    assert headers["content-type"] == "application/json; charset=utf-8"
    assert headers["cache-control"] == "no-store"
    payload = json.loads(body)
    assert payload["ok"] is True and payload["reason"] is None
    assert [a["bytes"] for a in payload["assets"]] == [
        size for path, size in SIZES.items()
        if path not in world_health_dependencies()]
    assert run_probe(entry, monkeypatch, status, headers, body) == (True, "")


def test_missing_or_wrong_sized_asset_fails_like_the_worker(host_app, entry,
                                                            monkeypatch):
    app, dist = host_app
    (dist / "world" / "world-scene.js").unlink()
    status, headers, body = asgi_get(app, "world.next.forkmesh.com:443")
    assert status == 503
    payload = json.loads(body)
    assert payload["reason"] == "/world/world-scene.js returned HTTP 404"
    ok, reason = run_probe(entry, monkeypatch, status, headers, body)
    assert ok is False and "HTTP 503" in reason and "world-scene.js" in reason
    make_dist(dist)
    (dist / "world" / "vendor" / "three-0.184.0" / "three.core.min.js").write_bytes(
        b"x" * (600 * 1024))
    status, headers, body = asgi_get(app, "world.next.forkmesh.com")
    assert status == 503
    assert json.loads(body)["reason"].endswith("returned an unexpected byte length")


def test_content_type_contract(tmp_path):
    make_dist(tmp_path)
    result = world_health.check_asset(
        tmp_path, ("/world/index.html", "javascript", 1, 10 ** 6))
    assert result["ok"] is False
    assert result["reason"] == "/world/index.html returned an invalid content type"
    escape = world_health.check_asset(
        tmp_path, ("/../outside.js", "javascript", 1, 10 ** 6))
    assert escape["ok"] is False and escape["status"] == 404


def test_only_the_world_host_and_get_head(host_app):
    app, _ = host_app
    # The App host's /health/mobile is not the World's: it reaches the Worker
    # (the stub raises there, which the front end turns into a 500).
    status, headers, _ = asgi_get(app, "app.next.forkmesh.com")
    assert status == 500 and "x-forkmesh-worker" not in headers
    status, headers, body = asgi_get(app, "world.next.forkmesh.com",
                                     method="HEAD")
    assert status == 200 and body == b"" and headers["x-forkmesh-worker"] == "world"
    status, headers, _ = asgi_get(app, "world.next.forkmesh.com", method="POST")
    assert status == 405 and headers["allow"] == "GET, HEAD"


def test_world_js_item_also_requires_the_runtime_it_loads(tmp_path):
    """The shell alone isn't a working World: a missing or truncated
    world-runtime.js must fail the /world/world.js item, while the payload
    keeps exactly the five paths the app's probe expects."""
    sizes = dict(SIZES)
    sizes["/world/world-runtime.js"] = 10 * 1024  # truncated runtime
    payload = world_health.health_payload(make_dist(tmp_path, sizes))
    assert payload["ok"] is False
    item = next(a for a in payload["assets"] if a["path"] == "/world/world.js")
    assert item["ok"] is False and "world-runtime.js" in item["reason"]
    assert [a["path"] for a in payload["assets"]] == [
        c[0] for c in world_health.MOBILE_WORLD_RUNTIME_ASSETS]


def test_bundled_world_scene_passes_and_an_oversized_one_fails(tmp_path):
    payload = world_health.health_payload(make_dist(tmp_path / "ok", SIZES))
    item = next(a for a in payload["assets"]
                if a["path"] == "/world/world-scene.js")
    assert item["ok"] is True and item["bytes"] == 780 * 1024
    assert "/world/world-scene.js" not in world_health.RUNTIME_DEPENDENCIES
    sizes = dict(SIZES)
    sizes["/world/world-scene.js"] = 5 * 1024 * 1024
    payload = world_health.health_payload(make_dist(tmp_path / "big", sizes))
    item = next(a for a in payload["assets"]
                if a["path"] == "/world/world-scene.js")
    assert item["ok"] is False
    assert item["reason"].endswith("returned an unexpected byte length")