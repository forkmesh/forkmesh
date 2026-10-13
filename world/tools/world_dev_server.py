#!/usr/bin/env python3
"""Local World dev server: local frontend, production backend.

Serves the checkout's ``world/public/world/`` frontend and bundled
``marketplace/`` World packages (no caching, so edits show on refresh), then
transparently proxies every other request — ``/api/*``, the ``/api/world/ws``
WebSocket, favicons, ``/assets/*``, login — to the main ForkMesh server. Because
the whole origin is one local host, the World's in-world login stores a real
``forkmesh.session`` for 127.0.0.1 and every Bearer-authenticated call reaches
production: you test local World changes against live accounts, presence and
repositories.

Run it from anywhere inside the repo:

    python3 world/tools/world_dev_server.py

then open http://127.0.0.1:8788/world/ — or just click "World" in the Qt
desktop app, which probes this port and prefers the local copy when the
``X-ForkMesh-World-Dev`` marker header answers.

Options: ``--port`` (default 8788), ``--upstream`` (default
https://app.forkmesh.com), ``--world-dir``, and ``--marketplace-dir``. Stdlib
only; no dependencies.
"""

import argparse
import http.client
import http.server
import mimetypes
import socket
import ssl
import sys
import threading
from pathlib import Path
from urllib.parse import urlsplit, urlparse

DEFAULT_PORT = 8788
DEFAULT_UPSTREAM = "https://app.forkmesh.com"
DEFAULT_WORLD_DIR = (
    Path(__file__).resolve().parents[2] / "world" / "public" / "world"
)
DEFAULT_MARKETPLACE_DIR = Path(__file__).resolve().parents[2] / "marketplace"
DEV_MARKER_HEADER = "X-ForkMesh-World-Dev"

# End-to-end headers only; these are connection-scoped and must not be
# replayed on the upstream leg (RFC 9110 §7.6.1).
HOP_BY_HOP_HEADERS = frozenset({
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailers",
    "transfer-encoding",
    "upgrade",
})


def is_world_path(path):
    """True for URL paths the local checkout serves instead of the proxy."""
    return path == "/world" or path == "/world/" or path.startswith("/world/")


def local_world_file(world_dir, path):
    """Resolve a ``/world/...`` URL path to a file inside ``world_dir``.

    Returns ``None`` for the proxy to handle when the path escapes the
    directory or the file does not exist locally (a prod-only asset).
    """
    if not is_world_path(path):
        return None
    relative = path[len("/world"):].lstrip("/")
    root = Path(world_dir).resolve()
    candidate = (root / relative).resolve() if relative else root
    if candidate.is_dir():
        candidate = candidate / "index.html"
    if root != candidate and root not in candidate.parents:
        return None
    return candidate if candidate.is_file() else None


def local_marketplace_file(marketplace_dir, path):
    """Resolve a ``/marketplace/...`` URL inside the package source tree."""
    if not path.startswith("/marketplace/"):
        return None
    relative = path[len("/marketplace/"):]
    root = Path(marketplace_dir).resolve()
    candidate = (root / relative).resolve()
    if root != candidate and root not in candidate.parents:
        return None
    return candidate if candidate.is_file() else None


def upstream_request_headers(headers, local_host, upstream):
    """Filter browser headers for the upstream leg of the proxy.

    Drops hop-by-hop headers, pins ``Host``, and rewrites ``Origin``/
    ``Referer`` from the local origin to the upstream one so same-origin
    checks (``world_websocket_origin_allowed``) keep passing.
    """
    up = urlsplit(upstream)
    result = []
    for name, value in headers:
        lower = name.lower()
        if lower in HOP_BY_HOP_HEADERS or lower == "host":
            continue
        if lower == "origin":
            if urlsplit(value).netloc == local_host:
                value = upstream
        elif lower == "referer":
            parsed = urlsplit(value)
            if parsed.netloc == local_host:
                value = f"{up.scheme}://{up.netloc}{parsed.path}"
        result.append((name, value))
    result.append(("Host", up.netloc))
    return result


def loopback_host_allowed(host, port):
    return str(host or "") in (f"127.0.0.1:{port}", f"localhost:{port}")


def rewrite_location(value, upstream, local_origin):
    """Point absolute upstream redirects back at the local origin."""
    if value == upstream or value.startswith(upstream + "/"):
        return local_origin + value[len(upstream):]
    return value


class WorldDevHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    upstream = DEFAULT_UPSTREAM
    world_dir = None
    marketplace_dir = None

    def handle_any(self):
        if not loopback_host_allowed(
                self.headers.get("Host"), self.server.server_port):
            self.close_connection = True
            self.send_error(421)
            return
        try:
            if (self.headers.get("Upgrade") or "").lower() == "websocket":
                self.tunnel_websocket()
                return
            request_path = self.path.split("?")[0]
            local = self.command in ("GET", "HEAD") and (
                local_world_file(self.world_dir, request_path)
                or local_marketplace_file(self.marketplace_dir, request_path)
            )
            if local:
                self.serve_local(local)
            else:
                self.proxy_http()
        except (ConnectionError, ssl.SSLError, socket.timeout, OSError) as exc:
            self.close_connection = True
            try:
                self.send_error(502, explain=f"upstream error: {exc}")
            except OSError:
                pass

    do_GET = do_HEAD = do_POST = do_PUT = do_DELETE = do_PATCH = \
        do_OPTIONS = handle_any

    def serve_local(self, file_path):
        body = file_path.read_bytes()
        content_type = (mimetypes.guess_type(str(file_path))[0]
                        or "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header(DEV_MARKER_HEADER, "1")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def proxy_http(self):
        up = urlsplit(self.upstream)
        conn_class = (http.client.HTTPSConnection if up.scheme == "https"
                      else http.client.HTTPConnection)
        conn = conn_class(up.netloc, timeout=30)
        body = None
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            body = self.rfile.read(length)
        local_host = self.headers.get("Host") or f"127.0.0.1:{self.server.server_port}"
        headers = dict(upstream_request_headers(
            self.headers.items(), local_host, self.upstream))
        try:
            conn.request(self.command, self.path, body=body, headers=headers)
            response = conn.getresponse()
            payload = response.read()
            self.send_response(response.status, response.reason)
            local_origin = f"http://{local_host}"
            for name, value in response.getheaders():
                lower = name.lower()
                if lower in HOP_BY_HOP_HEADERS or lower == "content-length":
                    continue
                if lower == "location":
                    value = rewrite_location(value, self.upstream, local_origin)
                self.send_header(name, value)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(payload)
        finally:
            conn.close()

    def tunnel_websocket(self):
        """Blind byte tunnel for /api/world/ws (and any other Upgrade)."""
        up = urlsplit(self.upstream)
        port = up.port or (443 if up.scheme == "https" else 80)
        raw = socket.create_connection((up.hostname, port), timeout=30)
        if up.scheme == "https":
            raw = ssl.create_default_context().wrap_socket(
                raw, server_hostname=up.hostname)
        local_host = self.headers.get("Host") or f"127.0.0.1:{self.server.server_port}"
        lines = [f"{self.command} {self.path} HTTP/1.1"]
        lines += [f"{name}: {value}" for name, value in upstream_request_headers(
            self.headers.items(), local_host, self.upstream)]
        # upstream_request_headers strips hop-by-hop headers; restore the two
        # that make this an Upgrade request.
        lines += ["Connection: Upgrade", "Upgrade: websocket", "", ""]
        raw.sendall("\r\n".join(lines).encode("latin-1"))
        raw.settimeout(None)
        self.connection.settimeout(None)

        def pump(source, sink):
            try:
                while True:
                    chunk = source.recv(65536)
                    if not chunk:
                        break
                    sink.sendall(chunk)
            except OSError:
                pass
            finally:
                for endpoint in (source, sink):
                    try:
                        endpoint.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass

        upward = threading.Thread(
            target=pump, args=(self.connection, raw), daemon=True)
        upward.start()
        pump(raw, self.connection)
        upward.join(timeout=5)
        raw.close()
        self.close_connection = True

    def log_message(self, fmt, *args):
        sys.stderr.write("world-dev: %s\n" % (fmt % args))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--upstream", default=DEFAULT_UPSTREAM,
                        help="main ForkMesh server (default %(default)s)")
    parser.add_argument(
        "--world-dir",
        default=str(DEFAULT_WORLD_DIR),
        help="local World frontend directory (default %(default)s)")
    parser.add_argument(
        "--marketplace-dir",
        default=str(DEFAULT_MARKETPLACE_DIR),
        help="local marketplace package directory (default %(default)s)")
    options = parser.parse_args(argv)
    upstream = options.upstream.rstrip("/")
    if urlparse(upstream).scheme not in ("http", "https"):
        parser.error("--upstream must be an http(s) URL")
    WorldDevHandler.upstream = upstream
    WorldDevHandler.world_dir = options.world_dir
    WorldDevHandler.marketplace_dir = options.marketplace_dir
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", options.port), WorldDevHandler)
    print(f"Local World: http://127.0.0.1:{options.port}/world/ "
          f"(frontend from {options.world_dir}, packages from "
          f"{options.marketplace_dir}, everything else → {upstream})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
