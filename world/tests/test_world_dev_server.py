#!/usr/bin/env python3
"""Contract for the local World dev server (tools/world_dev_server.py).

The server exists so the Qt desktop's World button can open a local copy of
the World frontend that stays authenticated against the main ForkMesh server:
only ``/world/*`` is served from the checkout, everything else (login, APIs,
the presence WebSocket) is proxied upstream with the origin rewritten so the
Worker's same-origin checks keep passing.
"""

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "app"
MODULE = ROOT.parent / "world" / "tools" / "world_dev_server.py"
WORLD_DIR = ROOT.parent / "world" / "public" / "world"
MARKETPLACE_DIR = ROOT.parent / "marketplace"

spec = importlib.util.spec_from_file_location("world_dev_server", MODULE)
dev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev)

UPSTREAM = "https://app.forkmesh.com"


def test_only_world_paths_are_served_locally():
    assert dev.is_world_path("/world")
    assert dev.is_world_path("/world/")
    assert dev.is_world_path("/world/world.js")
    assert not dev.is_world_path("/api/world/context")
    assert not dev.is_world_path("/worldwide")
    assert not dev.is_world_path("/")


def test_local_world_file_resolution_and_traversal():
    index = dev.local_world_file(WORLD_DIR, "/world/")
    assert index == WORLD_DIR / "index.html"
    assert dev.local_world_file(WORLD_DIR, "/world") == index
    assert (dev.local_world_file(WORLD_DIR, "/world/world.js")
            == WORLD_DIR / "world.js")
    # Prod-only assets fall through to the proxy instead of 404ing locally.
    assert dev.local_world_file(WORLD_DIR, "/world/not-in-checkout.js") is None
    # Path traversal cannot escape the world directory even though a real
    # worker file exists one level up.
    assert dev.local_world_file(WORLD_DIR, "/world/../src/entry.py") is None
    assert dev.local_world_file(WORLD_DIR, "/world/%2e%2e/entry.py") is None


def test_local_marketplace_package_resolution_and_traversal():
    module = dev.local_marketplace_file(
        MARKETPLACE_DIR,
        "/marketplace/packages/x-banner-board/content/world/index.js",
    )
    assert module == (
        MARKETPLACE_DIR
        / "packages"
        / "x-banner-board"
        / "content"
        / "world"
        / "index.js"
    )
    assert dev.local_marketplace_file(
        MARKETPLACE_DIR, "/world/world-scene.js") is None
    assert dev.local_marketplace_file(
        MARKETPLACE_DIR, "/marketplace/../world/worker.js") is None


def test_upstream_headers_rewrite_origin_and_host():
    headers = dev.upstream_request_headers(
        [
            ("Host", "127.0.0.1:8788"),
            ("Origin", "http://127.0.0.1:8788"),
            ("Referer", "http://127.0.0.1:8788/world/"),
            ("Authorization", "Bearer token-1"),
            ("Connection", "keep-alive"),
            ("Transfer-Encoding", "chunked"),
        ],
        "127.0.0.1:8788",
        UPSTREAM,
    )
    as_dict = dict(headers)
    # Same-origin checks upstream (world_websocket_origin_allowed) must see
    # the upstream origin, not the local one.
    assert as_dict["Origin"] == UPSTREAM
    assert as_dict["Referer"] == "https://app.forkmesh.com/world/"
    assert as_dict["Host"] == "app.forkmesh.com"
    assert as_dict["Authorization"] == "Bearer token-1"
    names = {name.lower() for name, _ in headers}
    assert "connection" not in names
    assert "transfer-encoding" not in names


def test_foreign_origin_is_not_laundered_into_the_upstream_origin():
    headers = dict(dev.upstream_request_headers(
        [
            ("Host", "127.0.0.1:8788"),
            ("Origin", "https://evil.example"),
            ("Referer", "https://evil.example/attack"),
        ],
        "127.0.0.1:8788",
        UPSTREAM,
    ))
    assert headers["Origin"] == "https://evil.example"
    assert headers["Referer"] == "https://evil.example/attack"


def test_rebound_host_is_refused_before_serving_or_proxying():
    import socket
    import types

    class Handler(dev.WorldDevHandler):
        world_dir = str(WORLD_DIR)
        marketplace_dir = str(MARKETPLACE_DIR)
        upstream = "http://upstream.invalid"

        def log_message(self, fmt, *args):
            pass

    server = types.SimpleNamespace(server_port=8788)

    def get(host):
        client, served = socket.socketpair()
        client.settimeout(10)
        client.sendall((
            "GET /world/ HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
            % host).encode("latin-1"))
        Handler(served, ("127.0.0.1", 50000), server)
        served.close()
        response = b""
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            response += chunk
        client.close()
        return int(response.split(b" ", 2)[1])

    assert get("attacker.example:8788") == 421
    assert get("127.0.0.1:8788") == 200
    assert get("localhost:8788") == 200


def test_location_rewrites_point_back_at_the_local_origin():
    local = "http://127.0.0.1:8788"
    assert (dev.rewrite_location(UPSTREAM + "/login", UPSTREAM, local)
            == local + "/login")
    assert dev.rewrite_location(UPSTREAM, UPSTREAM, local) == local
    assert (dev.rewrite_location("https://other.example/x", UPSTREAM, local)
            == "https://other.example/x")
    assert dev.rewrite_location("/dashboard", UPSTREAM, local) == "/dashboard"


def test_defaults_match_the_desktop_probe():
    # The Qt World button probes http://127.0.0.1:8788/world/ and requires
    # this marker header before preferring the local copy.
    assert dev.DEFAULT_PORT == 8788
    assert dev.DEFAULT_UPSTREAM == UPSTREAM
    assert dev.DEFAULT_WORLD_DIR == WORLD_DIR
    assert dev.DEFAULT_MARKETPLACE_DIR == MARKETPLACE_DIR
    assert dev.DEV_MARKER_HEADER == "X-ForkMesh-World-Dev"
