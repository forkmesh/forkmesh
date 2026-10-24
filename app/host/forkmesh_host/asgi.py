"""ASGI front end: HTTP and WebSocket requests into ``Default.fetch``.

* HTTP: the ASGI request becomes a host ``js.Request`` (url, method,
  headers, streaming body, ``cf``), the Worker's response is streamed back.
* Static assets: for GET/HEAD, a path that ``run_worker_first`` in
  app/wrangler.toml does not claim and that names an asset in app/dist is
  answered from ``env.ASSETS`` (Workers Static Assets routing). Production
  Caddy serves those files itself; ``serve_assets=False`` turns this off.
* WebSocket: the handshake becomes a GET with ``Upgrade: websocket``. A 101
  response carrying a ``WebSocketPair`` client is accepted and frames are
  bridged both ways; anything else is sent as an HTTP denial (ASGI
  ``websocket.http.response`` extension) or a 1008 close.
* ``/readyz``: 200 when the Durable Object host is ready (in cluster mode:
  local rqlite has a leader, this node is live (fresh own heartbeat) and it
  holds quorum), else 503. It never reaches the Worker. A sealed host
  (``vault.py``) is never ready (reason ``sealed``) and answers every other
  path with ``503 {"error": "sealed"}`` (``/health``: 200, sealed).
* ``/health/mobile`` for the World host (``WORLD_ORIGIN``): answered by
  ``world_health.py`` from the files in world/dist, like world/worker.js did.
"""

from __future__ import annotations

import asyncio
import contextvars
import json
import logging
import os
import re
import time
import tomllib
import urllib.parse

from . import d1_caller
from . import error_log
from . import js as _js
from . import read_surfaces
from . import repo_ssr
from . import request_guard
from .request_policy import RequestPolicy
from .request_tally import RequestTally
from .host_metrics import HostMetrics
from .ffi import JsObject

HOP_BY_HOP = {"connection", "keep-alive", "transfer-encoding", "upgrade",
              "proxy-connection", "te", "trailer"}
_RESERVED_CLOSE = {1004, 1005, 1006, 1015}
AUTH_HEADER_BYTES = b"x-forkmesh-mesh-auth"
EDGE_HEADER_BYTES = request_guard.EDGE_HEADER.encode()
EDGE_SCOPE_KEY = "forkmesh.edge_client"
HANDSHAKE_LOG_MESSAGE = "ASGI callable returned without completing handshake."
HELD_REQUEST_PATHS = frozenset({"/api/runners/claim"})
REQUEST_CHECK_PATH = "/_host/request-policy"
# A static file asked for with a content version (``/app.js?v=3f9c…``, stamped
# by tools/build_site_assets.py and dashboard_shell) never changes under that
# URL: a new build changes the query. Browsers and the edge keep it for a
# year without revalidating. The bare URL keeps the _headers rules.
VERSIONED_ASSET_CACHE = "public, max-age=31536000, immutable"
_ASSET_VERSION_RE = re.compile(r"[A-Za-z0-9._-]{1,64}")
_denied_over_http = contextvars.ContextVar("forkmesh_ws_denied_over_http",
                                           default=False)


class HttpDenialHandshakeFilter(logging.Filter):
    def filter(self, record):
        return not (_denied_over_http.get()
                    and record.getMessage() == HANDSHAKE_LOG_MESSAGE)


def install_handshake_log_filter(logger_name="uvicorn.error"):
    logger = logging.getLogger(logger_name)
    for existing in logger.filters:
        if isinstance(existing, HttpDenialHandshakeFilter):
            return existing
    installed = HttpDenialHandshakeFilter()
    logger.addFilter(installed)
    return installed

BOOT_UNSEAL_WINDOW_SECONDS = 20.0
BOOT_UNSEAL_WAIT_SECONDS = 3.0
BOOT_UNSEAL_POLL_SECONDS = 0.05


def versioned_asset_query(scope):
    """The token of a query that is a single content version, else ""."""
    raw = scope.get("query_string") or b""
    try:
        pairs = urllib.parse.parse_qsl(raw.decode("latin-1"),
                                       keep_blank_values=True)
    except ValueError:
        return False
    if (len(pairs) == 1 and pairs[0][0] == "v"
            and _ASSET_VERSION_RE.fullmatch(pairs[0][1])):
        return pairs[0][1]
    return ""


def server_timing_headers(response, elapsed_ms, db_count, db_ms):
    value = "app;dur=%.1f" % max(0.0, elapsed_ms)
    if db_count:
        value += ', db;dur=%.1f;desc="%d quer%s"' % (
            max(0.0, db_ms), db_count, "y" if db_count == 1 else "ies")
    out = [(b"server-timing", value.encode("latin-1"))]
    headers = getattr(response, "headers", None)
    origin = headers.get("access-control-allow-origin") if headers else None
    if origin and not headers.has("timing-allow-origin"):
        out.append((b"timing-allow-origin",
                    str(origin).encode("latin-1", "replace")))
    return out


def _glob(pattern):
    return re.compile("^" + re.escape(pattern).replace(r"\*", ".*") + "$")


class WorkerFirstRules:
    """``[assets] run_worker_first`` (list form, with ``!`` negations)."""

    def __init__(self, patterns):
        if patterns is True:
            patterns = ["/*"]
        elif not patterns:
            patterns = []
        self.include = [_glob(p) for p in patterns if not p.startswith("!")]
        self.exclude = [_glob(p[1:]) for p in patterns if p.startswith("!")]

    @classmethod
    def from_toml(cls, path):
        with open(path, "rb") as handle:
            manifest = tomllib.load(handle)
        return cls((manifest.get("assets") or {}).get("run_worker_first"))

    def worker_first(self, path):
        if any(rule.match(path) for rule in self.exclude):
            return False
        return any(rule.match(path) for rule in self.include)


def _headers_from_scope(scope):
    headers = _js.Headers()
    for name, value in scope.get("headers") or []:
        headers.append(name.decode("latin-1"), value.decode("latin-1"))
    return headers


def _url_from_scope(scope, headers, trust_forwarded):
    scheme = scope.get("scheme") or "http"
    scheme = {"ws": "http", "wss": "https"}.get(scheme, scheme)
    if trust_forwarded:
        forwarded = str(headers.get("x-forwarded-proto") or "").split(",")[0]
        if forwarded.strip() in ("http", "https"):
            scheme = forwarded.strip()
    host = headers.get("host")
    if not host:
        server = scope.get("server") or ("localhost", None)
        host = server[0] + (":%d" % server[1] if server[1] else "")
    raw_path = scope.get("raw_path")
    path = raw_path.decode("latin-1") if raw_path else scope.get("path", "/")
    query = scope.get("query_string") or b""
    url = "%s://%s%s" % (scheme, host, path or "/")
    if query:
        url += "?" + query.decode("latin-1")
    return url


class HostASGI:
    def __init__(self, runtime, *, serve_assets=True, cron=True,
                 trust_forwarded=True, worker_first=None):
        self.runtime = runtime
        self.serve_assets = serve_assets
        self.cron = cron
        self.trust_forwarded = trust_forwarded
        self.rules = worker_first or WorkerFirstRules.from_toml(
            runtime.config.wrangler_toml)
        self.requests = 0
        self.errors = 0
        self.debug_endpoints = os.environ.get("FORKMESH_HOST_DEBUG") == "1"
        self.host_allowlist = request_guard.HostAllowlist()
        self.edge_verifier = request_guard.EdgeClientVerifier()
        self.request_policy = RequestPolicy()
        self.misdirected = 0
        from .repo_fastpath import RepoFastPath
        self.repo_fastpath = RepoFastPath.from_environ()
        self.request_tally = RequestTally()
        self._tally_task = None
        self.metrics = HostMetrics()
        self._lag_task = None
        runtime.host_metrics = self.metrics
        self._fleet_api = None
        install_handshake_log_filter()
        self._born = time.monotonic()
        self.boot_unseal_waits = 0

    def _booting_sealed(self):
        vault = getattr(self.runtime, "vault", None)
        if vault is None or not getattr(vault, "sealed", False):
            return False
        if self.runtime.quarantined:
            return False
        if (getattr(vault, "stats", None) or {}).get("unseals", 0):
            return False
        return time.monotonic() - self._born < BOOT_UNSEAL_WINDOW_SECONDS

    async def _still_sealed(self):
        if not self.runtime.sealed:
            return False
        if not self._booting_sealed():
            return True
        self.boot_unseal_waits += 1
        deadline = time.monotonic() + BOOT_UNSEAL_WAIT_SECONDS
        while time.monotonic() < deadline:
            await asyncio.sleep(BOOT_UNSEAL_POLL_SECONDS)
            if not self.runtime.sealed:
                return False
            if not self._booting_sealed():
                break
        return self.runtime.sealed

    async def __call__(self, scope, receive, send):
        kind = scope["type"]
        if kind == "http":
            if not str(scope.get("path", "")).startswith("/api/"):
                started = time.perf_counter()
                try:
                    await self._http(scope, receive, send)
                finally:
                    self.metrics.record(
                        scope.get("path", ""),
                        (time.perf_counter() - started) * 1000)
                return
            answered = {}

            async def tracked(message):
                if message.get("type") == "http.response.start":
                    answered["status"] = message.get("status")
                await send(message)

            started = time.perf_counter()
            try:
                await self._http(scope, receive, tracked)
            except Exception:
                answered.setdefault("status", 500)
                raise
            finally:
                self.metrics.record(scope.get("path", ""),
                                    (time.perf_counter() - started) * 1000)
                if answered.get("status"):
                    self.request_tally.record(
                        scope.get("path", ""), answered["status"],
                        (time.perf_counter() - started) * 1000)
        elif kind == "websocket":
            with d1_caller.scope():
                await self._websocket(scope, receive, send)
        elif kind == "lifespan":
            await self._lifespan(receive, send)

    # -- lifespan --------------------------------------------------------
    async def _lifespan(self, receive, send):
        while True:
            message = await receive()
            if message["type"] == "lifespan.startup":
                try:
                    self.runtime.start(cron=self.cron)
                except Exception as error:
                    await send({"type": "lifespan.startup.failed",
                                "message": repr(error)})
                    return
                self._tally_task = asyncio.ensure_future(
                    self.request_tally.run())
                self._lag_task = asyncio.ensure_future(
                    self.metrics.run_lag_probe())
                await send({"type": "lifespan.startup.complete"})
            elif message["type"] == "lifespan.shutdown":
                if self._lag_task is not None:
                    self._lag_task.cancel()
                    await asyncio.gather(self._lag_task,
                                         return_exceptions=True)
                    self._lag_task = None
                if self._tally_task is not None:
                    self._tally_task.cancel()
                    await asyncio.gather(self._tally_task,
                                         return_exceptions=True)
                    self._tally_task = None
                await self.request_policy.close()
                try:
                    await self.runtime.stop()
                finally:
                    await send({"type": "lifespan.shutdown.complete"})
                return

    # -- request building ------------------------------------------------
    def _request(self, scope, receive, method, body):
        headers = _headers_from_scope(scope)
        url = _url_from_scope(scope, headers, self.trust_forwarded)
        client = scope.get("client")
        self._edge_client(scope, headers, method)
        if client and not headers.has("cf-connecting-ip"):
            headers.set("cf-connecting-ip", client[0])
        country = headers.get("cf-ipcountry")
        cf = JsObject(country=country) if country else JsObject()
        cf["httpProtocol"] = "HTTP/" + str(scope.get("http_version", "1.1"))
        return _js.Request(url, method, headers, body, None, cf)

    def _edge_client(self, scope, headers, method):
        """x-forkmesh-edge-client (deploy/core/edge_client.md): always
        stripped; when it verifies and the request came through Caddy
        (loopback peer), its visitor replaces cf-connecting-ip/country."""
        value = headers.get(request_guard.EDGE_HEADER)
        if value is None:
            return
        headers.delete(request_guard.EDGE_HEADER)
        verified = self._verified_edge(scope, value, method)
        if verified is None:
            return
        ip, country = verified
        headers.set("cf-connecting-ip", ip)
        if country:
            headers.set("cf-ipcountry", country)
        else:
            headers.delete("cf-ipcountry")

    def _verified_edge(self, scope, value, method):
        if EDGE_SCOPE_KEY in scope:
            return scope[EDGE_SCOPE_KEY]
        verified = None
        peer = (scope.get("client") or (None,))[0]
        if request_guard.is_loopback(peer):
            secret = getattr(self.runtime.env, "WWW_EDGE_SECRET", "") or ""
            raw_path = scope.get("raw_path")
            path = raw_path.decode("latin-1") if raw_path else \
                scope.get("path", "/")
            verified = self.edge_verifier.verify(
                value, str(secret) if isinstance(secret, str) else "",
                method, path.split("?", 1)[0] or "/")
        scope[EDGE_SCOPE_KEY] = verified
        return verified

    def _visitor(self, scope, method):
        agent = forwarded = edge = None
        for name, value in scope.get("headers") or []:
            if name == b"user-agent":
                agent = value.decode("latin-1")
            elif name == b"cf-connecting-ip" and forwarded is None:
                forwarded = value.decode("latin-1")
            elif name == EDGE_HEADER_BYTES and edge is None:
                edge = value.decode("latin-1")
        peer = (scope.get("client") or (None,))[0]
        verified = None if edge is None else \
            self._verified_edge(scope, edge, method)
        ip = verified[0] if verified else (forwarded or peer)
        return ip, agent or "", request_guard.is_loopback(peer)

    async def _policy_denial(self, scope, method, path):
        ip, agent, loopback = self._visitor(scope, method)
        verdict = await self.request_policy.check(
            self.runtime.env, path, ip, agent, loopback)
        if verdict is None:
            return None
        status, wait = verdict
        if status == 429:
            return self._plain_response(429, "Too Many Requests\n",
                                        {"retry-after": str(wait)})
        return self._plain_response(403, "Forbidden\n")

    async def _request_check(self, scope, send):
        headers = {}
        for name, value in scope.get("headers") or []:
            headers.setdefault(name, value.decode("latin-1"))
        uri = headers.get(b"x-forwarded-uri") or "/"
        method = (headers.get(b"x-forwarded-method") or "GET").upper()
        raw_path = uri.split("?", 1)[0] or "/"
        path = urllib.parse.unquote(raw_path)
        peer = (scope.get("client") or (None,))[0]
        verified = None
        edge = headers.get(EDGE_HEADER_BYTES)
        if edge is not None:
            secret = getattr(self.runtime.env, "WWW_EDGE_SECRET", "") or ""
            verified = self.edge_verifier.verify(
                edge, str(secret) if isinstance(secret, str) else "",
                method, raw_path, consume=False)
        ip = verified[0] if verified else (
            headers.get(b"cf-connecting-ip") or peer)
        policy = self.request_policy
        if not policy.exempt(path) and await policy.blocked(
                self.runtime.env, ip, headers.get(b"user-agent") or ""):
            policy.stats["blocked"] += 1
            response = self._plain_response(403, "Forbidden\n")
        else:
            response = self._plain_response(204, "")
        await self._send_response(send, response, "GET")

    @staticmethod
    def _body_stream(receive, limit=None, state=None):
        """The request body as a stream, cut off at ``limit`` bytes
        (request_guard.BodyTooLarge; ``state["too_large"]`` is set so the
        caller answers 413 whatever the Worker made of the error)."""
        async def chunks():
            seen = 0
            while True:
                message = await receive()
                if message["type"] == "http.disconnect":
                    if state is not None:
                        state["disconnected"] = True
                    raise request_guard.ClientDisconnected(
                        "client disconnected")
                chunk = message.get("body") or b""
                if chunk:
                    seen += len(chunk)
                    if limit is not None and seen > limit:
                        if state is not None:
                            state["too_large"] = True
                        raise request_guard.BodyTooLarge(
                            "request body over %d bytes" % limit)
                    yield chunk
                if not message.get("more_body"):
                    return
        return _js.ReadableStream(chunks())

    @staticmethod
    def _plain_response(status, text, extra=None):
        headers = {"content-type": "text/plain; charset=utf-8",
                   "cache-control": "no-store"}
        headers.update(extra or {})
        return _js.Response(text.encode(), status, "", _js.Headers(headers))

    # -- host endpoints ----------------------------------------------------
    async def _host_endpoint(self, path, scope, send, method):
        """``/readyz``; with FORKMESH_HOST_DEBUG=1 (tests only) also
        ``/_host/cluster`` (placement/membership snapshot) and
        ``/_host/scheduled`` (run one scheduled() event now)."""
        durable = self.runtime.durable
        if path == "/_host/scheduled":
            try:
                await self.runtime.scheduled()
                body, status = {"ok": True}, 200
            except Exception as error:
                body, status = {"ok": False, "error": repr(error)}, 500
        elif path == "/readyz":
            readiness = getattr(durable, "readiness", None)
            if readiness is None:
                ok, reasons = True, []
            else:
                ok, reasons = await readiness()
            if self.runtime.sealed:
                ok = False
                reasons = ["sealed"] + [r for r in reasons if r != "sealed"]
            body = {"ok": ok, "reasons": reasons}
            node = getattr(durable, "node_id", None)
            if node:
                body["node"] = node
            membership = getattr(durable, "membership", None)
            if membership is not None and getattr(membership, "replica",
                                                  False):
                body["replica"] = True
            if self._metrics_allowed(scope, durable):
                body["metrics"] = self.metrics.snapshot()
            status = 200 if ok else 503
        else:
            snapshot = getattr(durable, "snapshot", None)
            if snapshot is None:
                body, status = {"cluster": False}, 404
            else:
                query = dict(urllib.parse.parse_qsl(
                    (scope.get("query_string") or b"").decode("latin-1")))
                body = snapshot(query.get("ns"), query.get("name"))
                status = 200
        response = _js.Response(
            json.dumps(body, default=str).encode(), status, "",
            _js.Headers({"content-type": "application/json",
                         "cache-control": "no-store"}))
        await self._send_response(send, response, method)

    async def _fleet(self, scope, receive, send, method, path):
        from . import fleet_api
        if self._fleet_api is None:
            self._fleet_api = fleet_api.FleetApi(self.runtime)
        body = b""
        if method == "POST":
            chunks, size = [], 0
            while True:
                message = await receive()
                if message["type"] == "http.disconnect":
                    return
                chunk = message.get("body") or b""
                size += len(chunk)
                if size > fleet_api.MAX_BODY:
                    await self._send_response(send, self._plain_response(
                        413, "Payload Too Large\n", {"connection": "close"}),
                        method)
                    return
                chunks.append(chunk)
                if not message.get("more_body"):
                    break
            body = b"".join(chunks)
        headers = _headers_from_scope(scope)
        status, data, kind = await fleet_api.dispatch(
            self._fleet_api, method, path, headers, body,
            fleet_api.client_ip(scope, headers))
        await self._send_response(send, _js.Response(
            data, status, "", _js.Headers({"content-type": kind,
                                          "cache-control": "no-store"})),
            method)

    @staticmethod
    def _metrics_allowed(scope, durable):
        query = (scope.get("query_string") or b"").decode("latin-1")
        if query != "metrics=1":
            return False
        verifier = getattr(durable, "verifier", None)
        if verifier is None:
            return False
        header = None
        for name, value in scope.get("headers") or []:
            if name == AUTH_HEADER_BYTES:
                header = value.decode("latin-1")
        return bool(header) and verifier.verify(
            header, "GET", "/readyz?metrics=1", b"")

    @staticmethod
    def _sealed_response(path):
        """Sealed host (vault.py): ``/health`` says the process is alive
        but sealed; everything else is 503 until the operator unseals."""
        if path == "/health":
            body, status = {"ok": True, "sealed": True}, 200
        else:
            body, status = {"error": "sealed"}, 503
        headers = {"content-type": "application/json",
                   "cache-control": "no-store"}
        if status == 503:
            headers["retry-after"] = "30"
        return _js.Response(json.dumps(body).encode(), status, "",
                            _js.Headers(headers))

    # -- HTTP --------------------------------------------------------------
    async def _http(self, scope, receive, send):
        self.requests += 1
        method = scope.get("method", "GET").upper()
        path = scope.get("path", "/")
        if not self.host_allowlist.allows(scope, self.runtime.env):
            self.misdirected += 1
            await self._send_response(send, self._plain_response(
                421, "Misdirected Request\n", {"connection": "close"}),
                method)
            return
        if path == REQUEST_CHECK_PATH and request_guard.is_loopback(
                (scope.get("client") or (None,))[0]):
            await self._request_check(scope, send)
            return
        if path == "/readyz" or (self.debug_endpoints and path in (
                "/_host/cluster", "/_host/scheduled")):
            await self._host_endpoint(path, scope, send, method)
            return
        if path.startswith("/api/fleet/"):
            await self._fleet(scope, receive, send, method, path)
            return
        denial = await self._policy_denial(scope, method, path)
        if denial is not None:
            await self._send_response(send, denial, method)
            return
        if path == "/health/mobile" and self._world_health(scope, method):
            await self._send_world_health(send, method)
            return
        if self.runtime.sealed and (path == "/health"
                                    or await self._still_sealed()):
            await self._send_response(send, self._sealed_response(path),
                                      method)
            return
        try:
            if (self.repo_fastpath is not None
                    and path.startswith("/api/repo/")
                    and await self.repo_fastpath.handle(scope, send)):
                return
        except ValueError as error:
            if not request_guard.is_too_many_fields(error):
                raise
            await self._send_response(send, self._plain_response(
                400, "Bad Request\n"), method)
            return
        if (self.repo_fastpath is not None and method in ("GET", "HEAD")
                and read_surfaces.target(scope) is not None):
            origin = read_surfaces.origin_of(_url_from_scope(
                scope, _headers_from_scope(scope), self.trust_forwarded))
            try:
                if await read_surfaces.handle(self.repo_fastpath, self.runtime.fetch,
                                              scope, send, origin):
                    return
            except Exception as error:
                error_log.report("read surface failed on %s" % path, error)
        if (self.serve_assets and method in ("GET", "HEAD")
                and not self.rules.worker_first(path)):
            assets = getattr(self.runtime.env, "ASSETS", None)
            if assets is not None and assets.exists(path):
                if_none_match = None
                accept_encoding = ""
                for name, value in scope.get("headers") or []:
                    if name == b"if-none-match":
                        if_none_match = value.decode("latin-1")
                    elif name == b"accept-encoding":
                        accept_encoding = value.decode("latin-1")
                # A precompressed copy (tools/precompress_assets.py) when
                # Accept-Encoding allows one; the proxy in front leaves an
                # encoded response as it is.
                response = assets.serve(path, method, if_none_match,
                                        accept_encoding=accept_encoding)
                version = versioned_asset_query(scope)
                cache = str(response.headers.get("cache-control") or "")
                if (int(response.status) in (200, 304) and version
                        and "no-store" not in cache and "private" not in cache
                        and version == assets.content_version(path)):
                    response.headers.set("cache-control",
                                         VERSIONED_ASSET_CACHE)
                await self._send_response(send, response, method)
                return
        limit = request_guard.body_limit(method, path)
        declared = request_guard.declared_length(scope)
        if declared == -1:
            await self._send_response(send, self._plain_response(
                400, "Bad Content-Length\n", {"connection": "close"}), method)
            return
        if declared is not None and declared > limit:
            # Refused before a single body byte is read.
            await self._send_response(send, self._plain_response(
                413, "Payload Too Large\n", {"connection": "close"}), method)
            return
        body_state = {}
        body = None if method in ("GET", "HEAD") else \
            self._body_stream(receive, limit, body_state)
        request = self._request(scope, receive, method, body)
        started = time.perf_counter()
        try:
            ssr = repo_ssr.start(self.repo_fastpath, scope)
        except ValueError as error:
            if not request_guard.is_too_many_fields(error):
                raise
            ssr = None
        with d1_caller.scope():
            response = await self._fetch_response(
                ssr, request, method, path, body_state)
            db_count, db_ms = d1_caller.timing()
        if body_state.get("too_large"):
            response = self._plain_response(
                413, "Payload Too Large\n", {"connection": "close"})
        elapsed = (time.perf_counter() - started) * 1000
        if elapsed > 250 and path not in HELD_REQUEST_PATHS:
            print("slow request %.0fms %s %s" % (elapsed, method, path))
        if response is None or response.status == 101:
            response = _js.Response(b"worker returned no response", 500, "",
                                    _js.Headers())
        response = await repo_ssr.finish(ssr, response)
        if self.repo_fastpath is not None and path.startswith("/api/repo/"):
            stale = self.repo_fastpath.stale_answer(scope, response.status)
            if stale is not None:
                await self.repo_fastpath.send_stale(scope, send, stale)
                return
        extra = None
        if path.startswith("/api/"):
            extra = server_timing_headers(response, elapsed, db_count, db_ms)
        await self._send_response(send, response, method, extra)

    async def _fetch_response(self, ssr, request, method, path, body_state):
        try:
            return await repo_ssr.fetch(ssr, self.runtime.fetch, request)
        except Exception as error:
            if request_guard.is_too_many_fields(error):
                return self._plain_response(400, "Bad Request\n")
            if body_state.get("disconnected") or isinstance(
                    error, request_guard.ClientDisconnected):
                return self._plain_response(499, "Client Closed\n")
            if not body_state.get("too_large"):
                self.errors += 1
                error_log.report("worker fetch raised on %s %s -> 500"
                                 % (method, path), error)
            return _js.Response(
                b"Internal Server Error", 500, "",
                _js.Headers({"content-type": "text/plain; charset=utf-8"}))

    def _world_health(self, scope, method):
        """world.next's /health/mobile (world_health.py): static-file
        contracts only, no secrets, so it answers even while sealed."""
        del method
        from . import world_health
        host = None
        for name, value in scope.get("headers") or []:
            if name == b"host":
                host = value.decode("latin-1")
        return world_health.matches("/health/mobile", host, self.runtime.env)

    async def _send_world_health(self, send, method):
        from . import world_health
        status, headers, body = world_health.response(method,
                                                      self.runtime.env)
        await send({"type": "http.response.start", "status": status,
                    "headers": [(k.encode(), v.encode()) for k, v in headers]
                    + [(b"content-length", str(len(body)).encode())]})
        await send({"type": "http.response.body", "body": body})

    @staticmethod
    def _header_list(response, body_length=None, extra=None):
        out = []
        for name, value in response.headers.raw():
            if name in HOP_BY_HOP or name == "content-length":
                continue
            out.append((name.encode("latin-1"),
                        str(value).encode("latin-1", "replace")))
        if extra:
            out.extend(extra)
        if body_length is not None:
            out.append((b"content-length", str(body_length).encode()))
        return out

    async def _send_response(self, send, response, method, extra=None):
        body = response._body
        status = int(response.status)
        no_body = method == "HEAD" or status in (204, 304) or status < 200
        if body is None or isinstance(body, bytes):
            data = b"" if body is None else body
            if no_body:
                announced = response.headers.get("content-length")
                headers = self._header_list(response, extra=extra)
                if method == "HEAD" and announced is not None:
                    headers.append((b"content-length", announced.encode()))
                elif method == "HEAD" and body is not None:
                    headers.append((b"content-length",
                                    str(len(data)).encode()))
                data = b""
            else:
                headers = self._header_list(response, len(data), extra)
            await send({"type": "http.response.start", "status": status,
                        "headers": headers})
            await send({"type": "http.response.body", "body": data})
            return
        await send({"type": "http.response.start", "status": status,
                    "headers": self._header_list(response, extra=extra)})
        if no_body:
            await body.cancel()
            await send({"type": "http.response.body", "body": b""})
            return
        try:
            async for chunk in body:
                await send({"type": "http.response.body", "body": chunk,
                            "more_body": True})
        except (OSError, asyncio.CancelledError):
            await body.cancel()
            raise
        except Exception as error:
            print("response stream failed: %r" % (error,))
            await body.cancel()
        await send({"type": "http.response.body", "body": b""})

    # -- WebSocket ---------------------------------------------------------
    async def _websocket(self, scope, receive, send):
        self.requests += 1
        message = await receive()
        if message["type"] != "websocket.connect":
            return
        if not self.host_allowlist.allows(scope, self.runtime.env):
            self.misdirected += 1
            await self._deny(scope, send, self._plain_response(
                421, "Misdirected Request\n"))
            return
        denial = await self._policy_denial(
            scope, "GET", scope.get("path", "/"))
        if denial is not None:
            await self._deny(scope, send, denial)
            return
        if await self._still_sealed():
            await self._deny(scope, send, self._sealed_response(
                scope.get("path", "/")))
            return
        request = self._request(scope, receive, "GET", None)
        if not request.headers.has("upgrade"):
            request.headers.set("upgrade", "websocket")
        try:
            response = await self.runtime.fetch(request)
        except Exception as error:
            self.errors += 1
            error_log.report("worker websocket fetch raised on %s -> 500"
                             % (scope.get("path"),), error)
            response = _js.Response(b"Internal Server Error", 500, "",
                                    _js.Headers())
        client = getattr(response, "webSocket", None)
        if response.status != 101 or client is None:
            await self._deny(scope, send, response)
            return
        accept = {"type": "websocket.accept"}
        protocol = response.headers.get("sec-websocket-protocol")
        if protocol:
            accept["subprotocol"] = protocol.split(",")[0].strip()
        extra = [(n.encode(), str(v).encode()) for n, v in
                 response.headers.raw()
                 if n not in HOP_BY_HOP and not n.startswith("sec-websocket")]
        if extra:
            accept["headers"] = extra
        await send(accept)
        await self._bridge(client, receive, send)

    async def _deny(self, scope, send, response):
        extensions = scope.get("extensions") or {}
        if "websocket.http.response" in extensions:
            body = await response._read() if response._body is not None \
                else b""
            await send({"type": "websocket.http.response.start",
                        "status": int(response.status) or 500,
                        "headers": self._header_list(response, len(body))})
            await send({"type": "websocket.http.response.body",
                        "body": body})
            _denied_over_http.set(True)
            return
        await send({"type": "websocket.close", "code": 1008})

    @staticmethod
    async def _bridge(client, receive, send):
        async def outbound():
            while True:
                event = await client._inbox.get()
                if event[0] == "message":
                    data = event[1]
                    if isinstance(data, str):
                        await send({"type": "websocket.send", "text": data})
                    else:
                        await send({"type": "websocket.send",
                                    "bytes": bytes(data)})
                elif event[0] == "close":
                    code = int(event[1] or 1000)
                    if code in _RESERVED_CLOSE:
                        code = 1000
                    await send({"type": "websocket.close", "code": code,
                                "reason": str(event[2] or "")[:120]})
                    return

        async def inbound():
            while True:
                message = await receive()
                kind = message["type"]
                if kind == "websocket.receive":
                    text = message.get("text")
                    client._client_send(
                        text if text is not None else message.get("bytes"))
                elif kind == "websocket.disconnect":
                    code = int(message.get("code") or 1005)
                    client._client_closed(code, message.get("reason") or "",
                                          code != 1006)
                    return

        out_task = asyncio.ensure_future(outbound())
        in_task = asyncio.ensure_future(inbound())
        try:
            done, pending = await asyncio.wait(
                {out_task, in_task}, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in (out_task, in_task):
                if not task.done():
                    task.cancel()
        if client.readyState != _js.CLOSED:
            client._client_closed(1006, "", False)
        for task in done:
            error = task.exception()
            if error is not None and not isinstance(
                    error, (OSError, asyncio.CancelledError)):
                print("websocket bridge failed: %r" % (error,))
