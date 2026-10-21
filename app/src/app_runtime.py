"""app runtime routes, loaded on demand.

Split out of entry.py so Cloudflare's startup-memory validation (error
10021) does not pay for this domain on every isolate; entry.py loads it
through a _LazyModule proxy on the first request that needs it.
"""


async def _org_office_resync(env, request, org, response):
    if method_name(request) in ("GET", "HEAD", "OPTIONS"):
        return
    if int(getattr(response, "status", 500) or 500) >= 300:
        return
    await org_office_chat.resync_org(env, org)


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# Docs and blog have an intentionally small same-origin relay path for visitors
# already using a node-hosted App.  This is document-sized, public content — it
# is never an authenticated API proxy and it must not turn a node request into
# an unbounded remote fetch.
REPO_HOST_READ_ACTIONS = frozenset({
    "tree", "blobs", "blob", "raw", "history", "commit",
    "compare", "merge-preview", "branches", "search", "stats", "sizes",
    "pulse",
})
SITE_DOCUMENT_PROXY_TIMEOUT_SECONDS = 8
SITE_DOCUMENT_PROXY_MAX_BYTES = 1024 * 1024
SITE_DOCUMENT_ORIGIN_HEADER = "x-forkmesh-site-document-proxy"
SITE_DOCUMENT_SECURITY_HEADERS = (
    "content-security-policy",
    "cross-origin-opener-policy",
    "permissions-policy",
    "referrer-policy",
    "x-content-type-options",
    "x-frame-options",
)
SITE_DOCUMENT_FALLBACK_SECURITY_HEADERS = {
    "content-security-policy": (
        "default-src 'self'; base-uri 'self'; object-src 'none'; "
        "frame-ancestors 'none'; form-action 'self'; script-src 'self' "
        "'unsafe-inline' https://cdn.jsdelivr.net "
        "https://static.cloudflareinsights.com; style-src 'self' "
        "'unsafe-inline'; img-src 'self' data: blob: https:; "
        "font-src 'self' data:; connect-src 'self' https: wss:; "
        "media-src 'self' blob: https:; frame-src 'self' https:; "
        "worker-src 'self' blob:; manifest-src 'self'; "
        "upgrade-insecure-requests"
    ),
    "cross-origin-opener-policy": "same-origin",
    "referrer-policy": "strict-origin-when-cross-origin",
    "x-content-type-options": "nosniff",
    "x-frame-options": "DENY",
}

WORLD_DOCUMENT_SECURITY_HEADERS = {
    "content-security-policy": (
        "default-src 'self'; base-uri 'self'; object-src 'none'; "
        "frame-ancestors 'none'; form-action 'self'; "
        "script-src 'self' 'unsafe-inline' "
        "https://static.cloudflareinsights.com; style-src 'self' "
        "'unsafe-inline'; img-src 'self' data: blob: https:; "
        "font-src 'self' data:; connect-src 'self' https: wss:; "
        "media-src 'self' blob: https:; frame-src 'self' https:; "
        "worker-src 'self' blob:; manifest-src 'self'; "
        "upgrade-insecure-requests"
    ),
    "cross-origin-opener-policy": "same-origin",
    "permissions-policy": (
        "camera=(), geolocation=(), payment=(), usb=(), serial=(), "
        "browsing-topics=(), microphone=(self), clipboard-write=(self)"
    ),
    "referrer-policy": "strict-origin-when-cross-origin",
    "x-content-type-options": "nosniff",
    "x-frame-options": "DENY",
}


class Default:
    """Request implementation for per-event global HTTP and cron handlers."""
    _PUBLIC_SNAPSHOT_SCHEMA_VERSION = 1
    _PUBLIC_SNAPSHOT_KIND = "forkmesh-public-api-snapshot"
    _PUBLIC_SNAPSHOT_TOMBSTONE_KIND = (
        "forkmesh-public-api-snapshot-tombstone")
    # Browser max-age per served snapshot. Repository pages keep a last-good
    # routing catalog and refresh it in the background; five minutes prevents
    # every navigation from paying a D1 snapshot read while direct mirror
    # failover keeps repository data current if a cached node goes away.
    # Status paths are additionally forced to 0 by _public_snapshot_response:
    # their clients ride the /api/status/ws doorbell and must re-read now.
    _PUBLIC_SNAPSHOT_MAX_AGE = {
        "/api/repositories": 300,
        "/api/network/overview": 20,
        "/api/repo/forkmesh/forkmesh/mirrors": 3,
        "/api/status": 10,
        "/api/status/world": 10,
    }
    _PUBLIC_SNAPSHOT_MAX_BYTES = {
        "/api/repositories": 1536 * 1024,
        "/api/network/overview": 512 * 1024,
        "/api/repo/forkmesh/forkmesh/mirrors": 1024 * 1024,
        "/api/status": 1536 * 1024,
        "/api/status/world": 512 * 1024,
    }

    async def queue(self, batch, env, ctx):
        """Deliver instant notification email for one queued batch.

        Never raises: a batch that throws is retried whole, and the cron
        backstop already covers anything that did not send (see
        process_notification_queue).
        """
        del ctx
        # Same compatibility dance as scheduled() below — Cloudflare's Python
        # adapter may leave the explicit env argument as None.
        if env is None or js_nullish(env):
            env = self.env
        if _read_only(env):
            try:
                batch.ackAll()
            except BaseException:
                pass
            return
        try:
            await process_notification_queue(env, batch)
        except Exception:
            pass

    async def scheduled(self, controller, env, ctx):
        """Doorbell the serialized alarm-backed maintenance coordinator.

        Cloudflare may start a new Python scheduled event while the preceding
        Pyodide task is still unwinding. Running the 10--30 second maintenance
        dispatch in this top-level handler therefore made otherwise healthy
        minutes fail with ``Cannot enter into task ... while another task is
        being executed``. Keep this entrypoint deliberately tiny: one Durable
        Object request arms an alarm, and that singleton owns the long work.
        Durable Object alarm delivery serializes/retries the work and the
        existing per-slot state keeps it idempotent.
        """
        del ctx
        # WorkerEntrypoint exposes bindings on ``self.env``.  Cloudflare's
        # Python scheduled-event adapter currently leaves the compatibility
        # ``env`` argument as None, so prefer the explicit value in tests and
        # older runtimes but fall back to the bound entrypoint environment in
        # production.
        if env is None or js_nullish(env):
            env = self.env
        if _read_only(env):
            return False
        coordinator_id = env.FORKMESH_CRON_RUNNER.idFromName(CRON_RUNNER_NAME)
        coordinator = env.FORKMESH_CRON_RUNNER.get(coordinator_id)
        kick_request = JsRequest.new(
            "https://forkmesh.internal/cron-runner/kick",
            to_js({
                "method": "POST",
                "signal": JsAbortSignal.timeout(CRON_TRIGGER_KICK_TIMEOUT_MS),
            }),
        )
        try:
            response = await coordinator.fetch(kick_request)
        except BaseException:
            # A deploy reset or a free-tier duration abort can reject this
            # platform hop before the coordinator receives it. The next
            # minute retries, while the independent watchdog detects a real
            # run gap. Do not turn that already-recoverable missed doorbell
            # into an unhandled scheduled-event exception.
            return False
        return int(getattr(response, "status", 0) or 0) == 200

    @staticmethod
    def _control_response(response):
        """Mark a response as owned by the consolidated App Worker."""
        if not hasattr(response, "js_object"):
            response = Response(response)
        headers = [
            (str(name).lower(), value)
            for name, value in response.headers.items()
            if str(name).lower() != "x-forkmesh-worker"
        ]
        headers.append(("x-forkmesh-worker", "app"))
        return Response(
            response.body,
            status=int(getattr(response, "status", 200) or 200),
            status_text=str(getattr(response, "status_text", "") or ""),
            headers=headers,
        )

    @staticmethod
    def _without_head_body(request, response):
        if method_name(request) != "HEAD":
            return response
        return Response(
            None,
            status=int(getattr(response, "status", 200) or 200),
            status_text=str(getattr(response, "status_text", "") or ""),
            headers=list(response.headers.items()),
        )

    @staticmethod
    def _native_json_response(payload, status=200, headers=None):
        """Serialize a small control payload in V8, not lazy Python json."""
        values = {
            "content-type": "application/json; charset=utf-8",
            **dict(headers or {}),
        }
        return Response(
            str(JsJSON.stringify(to_js(payload))),
            status=int(status),
            headers=values,
        )

    @staticmethod
    def _fast_normalized_http_origin(value):
        """Normalize an Origin through V8 without importing urllib.parse."""
        try:
            parsed = JsURL.new(str(value or "").strip())
            protocol = str(parsed.protocol or "").lower()
            hostname = str(parsed.hostname or "").lower().rstrip(".")
            if hostname.startswith("[") and hostname.endswith("]"):
                hostname = hostname[1:-1]
            if (
                protocol not in ("http:", "https:")
                or not hostname
                or str(parsed.username or "")
                or str(parsed.password or "")
                or str(parsed.pathname or "/") not in ("", "/")
                or str(parsed.search or "")
                or str(parsed.hash or "")
                or (
                    protocol == "http:"
                    and hostname not in ("localhost", "127.0.0.1", "::1")
                )
            ):
                return ""
            host = "[" + hostname + "]" if ":" in hostname else hostname
            port = str(parsed.port or "")
            return (
                protocol[:-1] + "://" + host
                + ((":" + port) if port else "")
            )
        except BaseException:
            return ""

    def _fast_cors_headers(self, request):
        """CORS for stateless control/snapshot responses without urlparse."""
        origin = self._fast_normalized_http_origin(
            request.headers.get("origin") or "")
        if not origin:
            return {}
        allowed = set(_CORS_CANONICAL_FRONTEND_ORIGINS)
        for name in _CORS_ORIGIN_ENV_NAMES:
            value = self._fast_normalized_http_origin(
                getattr(self.env, name, "") or "")
            if value:
                allowed.add(value)
        for value in str(
                getattr(self.env, "CORS_ALLOWED_ORIGINS", "") or ""
        ).replace("\n", ",").split(","):
            value = self._fast_normalized_http_origin(value)
            if value:
                allowed.add(value)
        if origin not in allowed:
            return {}
        return {
            "access-control-allow-origin": origin,
            "access-control-allow-credentials": "true",
            "access-control-expose-headers": ", ".join(
                _CORS_EXPOSE_HEADERS),
            "vary": "Origin",
        }

    @staticmethod
    def _with_headers(response, values):
        values = dict(values)
        if "vary" in values:
            values["vary"] = _merge_vary(", ".join(
                str(value) for name, value in response.headers.items()
                if str(name).lower() == "vary"), values["vary"])
        current = [
            (str(name).lower(), value)
            for name, value in response.headers.items()
            if str(name).lower() not in values
            and str(name).lower() != "access-control-allow-credentials"
        ]
        current.extend(values.items())
        return Response(
            response.body,
            status=int(getattr(response, "status", 200) or 200),
            status_text=str(getattr(response, "status_text", "") or ""),
            headers=current,
        )

    def _with_fast_cors(self, request, response):
        values = self._fast_cors_headers(request)
        if not values:
            return response
        current = [
            (str(name).lower(), value)
            for name, value in response.headers.items()
        ]
        existing_vary = ", ".join(
            str(value) for name, value in current if name == "vary")
        values["vary"] = _merge_vary(existing_vary, "Origin")
        replaced = set(values)
        current = [
            (name, value) for name, value in current if name not in replaced
        ]
        current.extend(values.items())
        return Response(
            response.body,
            status=int(getattr(response, "status", 200) or 200),
            status_text=str(getattr(response, "status_text", "") or ""),
            headers=current,
        )

    async def _mirror_identity_control_response(self, request):
        if method_name(request) == "GET":
            router_key = _https_mirror_router_public_key(self.env)
            response = self._native_json_response(
                {
                    "ok": bool(router_key),
                    "protocol": "forkmesh-masked-proxy-v1",
                    "registration": "forkmesh-https-endpoint-v1",
                    "routerPublicKey": router_key,
                    "nonCustodial": True,
                    "repositoryBytesInD1": False,
                },
                status=200 if router_key else 503,
                headers={
                    "cache-control": "no-store",
                    "x-forkmesh-worker": "app",
                },
            )
            return self._without_head_body(request, response)
        response = await https_mirror_endpoint_handler(self.env, request)
        return response

    async def _world_online_control_response(self, request):
        world_id = self.env.FORKMESH_WORLD.idFromName("town-square-v1")
        try:
            response = await self.env.FORKMESH_WORLD.get(world_id).fetch(
                JsRequest.new(
                    str(request.url),
                    to_js({"method": "GET"}),
                )
            )
            response = self._control_response(response)
        except BaseException:
            response = self._native_json_response(
                {"ok": False, "online": 0, "error": "unavailable"},
                status=503,
                headers={
                    "cache-control": "no-store",
                    "x-forkmesh-worker": "app",
                    **EXPECTED_DEGRADED_HEADERS,
                },
            )
        return self._without_head_body(request, response)

    def _app_root_redirect(self, request, url):
        app_origin = _normalized_http_origin(
            getattr(self.env, "APP_ORIGIN", "") or "")
        if not app_origin or _request_target_origin(request) != app_origin:
            return None
        location = "/dashboard"
        if url.query:
            location += "?" + url.query
        return self._control_response(Response(
            None,
            status=308,
            headers={
                "cache-control": "no-store",
                "location": location,
            },
        ))

    def _world_ingress_redirect(self, request, url):
        """Route non-World paths delegated by the World asset worker."""
        world_origin = _normalized_http_origin(
            getattr(self.env, "WORLD_ORIGIN", "") or "")
        if (
            not world_origin
            or _request_target_origin(request) != world_origin
        ):
            return None

        external_owner = external_site_route(url.path)
        if external_owner == "world":
            return None
        origin_name = (
            "WWW_ORIGIN" if external_owner == "www" else "APP_ORIGIN"
        )
        origin = _normalized_http_origin(
            getattr(self.env, origin_name, "") or "")
        if not origin or origin == world_origin:
            return None
        destination = origin + url.path
        if url.query:
            destination += "?" + url.query
        return Response("", status=308, headers={"location": destination})

    async def _health_control_response(self, request, url):
        payload = {
            "ok": True,
            "service": "forkmesh-mainnode",
            "rev": _build_rev(self.env),
            "worker": str(
                getattr(self.env, "WORKER_ROLE", "") or "app"),
            "deployFingerprint": str(
                getattr(self.env, "DEPLOY_FINGERPRINT", "") or ""),
            "node": getattr(self.env, "NODE_NAME", "forkmesh"),
            "nodeSolanaAddress": getattr(
                self.env, "NODE_SOLANA_ADDRESS", ""),
            "websocket": "/api/repo/{owner}/{repo}/rooms/{room}/ws",
            "compatWebsocket": "/api/room/{room}/ws",
            "capabilities": [
                "encrypted-relay-rooms",
                "repo-scoped-rooms",
                "ephemeral-ciphertext-broadcast",
            ],
            "runtime": "python-workers",
        }
        if str(
            getattr(url, "path", None)
            or getattr(url, "pathname", "")
            or ""
        ) == "/api/mainnode":
            router_key, router_ready = (
                await _https_mirror_router_identity_status(self.env))
            payload["routerPublicKey"] = router_key
            payload["routerIdentityReady"] = router_ready
        response = self._native_json_response(
            payload,
            headers={"x-forkmesh-worker": "app"},
        )
        return self._without_head_body(request, response)

    # Optional client-facing features this Worker serves, named exactly as
    # the mobile apps' RelayFeature enum
    # (native/ios/ForkMeshCore/Sources/FMAccount/RelayFeatures.swift).
    #
    # Declared rather than reflected from the route table: /api/openapi.json
    # and /api/metrics/endpoints already reflect routes, but a route existing
    # does not mean the feature behaves, and a relay run on a different
    # codebase cannot reproduce route reflection. A short, deliberate list is
    # something any relay operator can answer honestly.
    #
    # A relay that omits this field is treated by clients as "did not say",
    # and they assume everything until a 404 proves otherwise. A relay that
    # sends it is taken at its word, so only list what actually works.
    RELAY_FEATURES = (
        # Core — a client refuses a relay without these.
        "accounts",
        "repositories",
        "repoRead",
        # Optional.
        "chat",
        "notifications",
        "push",
        "accountEvents",
        "nodeSync",
        "issues",
        "pulls",
        "agents",
        "actions",
        "orgs",
        "devices",
        "imports",
        "bugReports",
        "ads",
        "networkStats",
        "plugins",
        "bounties",
        "ownerKeys",
        "userStatus",
        "iosUdid",
    )

    def _version_control_response(self, request):
        response = self._native_json_response(
            {
                "ok": True,
                "rev": _build_rev(self.env),
                "features": list(self.RELAY_FEATURES),
                "worker": str(
                    getattr(self.env, "WORKER_ROLE", "") or "app"),
                "version": str(
                    getattr(self.env, "APP_VERSION", "") or ""),
                "deployFingerprint": str(
                    getattr(self.env, "DEPLOY_FINGERPRINT", "") or ""),
                "now": Date.now(),
            },
            headers={"x-forkmesh-worker": "app"},
        )
        return self._without_head_body(request, response)

    @staticmethod
    def _request_has_header(request, name):
        has_header = getattr(request.headers, "has", None)
        if callable(has_header):
            return bool(has_header(name))
        return request.headers.get(name) is not None

    @staticmethod
    def _explicit_live_read(request):
        directives = {}
        for item in str(request.headers.get("cache-control") or "").split(","):
            name, separator, value = item.partition("=")
            name = name.strip().lower()
            if name:
                directives[name] = (
                    value.strip().strip('"') if separator else "")
        pragma = {
            item.strip().lower()
            for item in str(request.headers.get("pragma") or "").split(",")
            if item.strip()
        }
        return bool(
            "no-cache" in directives
            or "no-store" in directives
            or directives.get("max-age") == "0"
            or directives.get("s-maxage") == "0"
            or "no-cache" in pragma
        )

    def _public_snapshot_path(self, request, url):
        app_origin = _normalized_http_origin(
            getattr(self.env, "APP_ORIGIN", "") or "")
        target_is_app = bool(
            app_origin and _request_target_origin(request) == app_origin)
        # Status is an anonymous, cron-built projection. Always serve its D1
        # snapshot to public GET callers, including the legacy query spelling
        # and clients that attach generic no-cache/session headers. Allowing a
        # query or logged-in browser to trigger the live 30-day aggregation
        # made a single cold request capable of exhausting stateless CPU.
        if (
            method_name(request) == "GET"
            and target_is_app
            and url.path in ("/api/status", "/api/status/world")
        ):
            if url.path == "/api/status/world":
                return "/api/status/world"
            status_view = parse_qs(url.query).get("view", ["full"])[0]
            return (
                "/api/status/world"
                if status_view == "world" else "/api/status"
            )
        # The catalog's private view is selected only by its signed query
        # parameters, never by a browser session cookie. Admit an exact public
        # catalog URL even when the browser attaches a cookie or reload cache
        # directives. Otherwise an ordinary signed-in page load bypasses the
        # materialized snapshot and decrypts the entire repository table in a
        # Free-plan request, which can exceed the Worker CPU limit.
        if (
            method_name(request) == "GET"
            and target_is_app
            and url.path == "/api/repositories"
            and not self._request_has_header(request, "authorization")
            and "?" not in str(request.url)
            and "#" not in str(request.url)
            and not url.query
            and not url.fragment
        ):
            return url.path
        if (
            method_name(request) != "GET"
            or self._request_has_header(request, "authorization")
            or self._request_has_header(request, "cookie")
            or self._explicit_live_read(request)
            or "?" in str(request.url)
            or "#" in str(request.url)
            or url.query
            or url.fragment
            or url.path not in self._PUBLIC_SNAPSHOT_MAX_AGE
        ):
            return ""
        return url.path if target_is_app else ""

    async def _stateless_fast_response(self, request):
        """Answer tiny D1-free routes within the Free 10 ms CPU budget."""
        try:
            url = JsURL.new(str(request.url))
        except BaseException:
            return None
        app_origin = self._fast_normalized_http_origin(
            getattr(self.env, "APP_ORIGIN", "") or "")
        if not app_origin or str(url.origin or "") != app_origin:
            return None
        path = str(url.pathname or "")
        method = method_name(request)
        response = None
        if path in ("/api/version", "/api/version/") and method in (
                "GET", "HEAD"):
            response = self._version_control_response(request)
        elif path in (
                "/api/mirrors/https", "/api/mirrors/https/") and method in (
                "GET", "HEAD"):
            response = await self._mirror_identity_control_response(request)
        elif path in (
                "/api/world/deploy-status",
                "/api/world/deploy-status/") and method == "GET":
            # The World update watcher and the optional status obelisk each
            # read this public, single-row semaphore once a minute while idle.
            # Sending that tiny read through generic ingress kept a
            # CronRunner object active for 120 invocations/hour (236 seconds
            # of wall time in the 2026-08-21 reliability window). It needs no
            # signature verification or request-body work, so answer it on
            # the same bounded stateless path as the other public controls.
            try:
                response = await world_deploy_status_handler(self.env, request)
            except BaseException as error:
                if not _is_d1_platform_error(error):
                    raise
                try:
                    await log_d1_unavailable(self.env, request, path, error)
                except BaseException:
                    pass
                response = json_response(
                    {"error": "database_unavailable"},
                    status=503,
                    cache_control="no-store",
                    extra_headers={
                        **EXPECTED_DEGRADED_HEADERS,
                        "Retry-After": "2",
                    },
                )
        elif path in ("/api/world/online", "/api/world/online/") and method in (
                "GET", "HEAD"):
            response = await self._world_online_control_response(request)
        elif path == "/health" and method in ("GET", "HEAD"):
            response = await self._health_control_response(request, url)
        elif (
            method in ("GET", "HEAD")
            and _retired_repository_socket_path(path)
        ):
            # A retired route needs no state, so answering here keeps a stale
            # client that ignores the cached 410 from waking the ingress
            # object. Upgrade requests never reach this far: the Upgrade guard
            # above rejects every non-public WebSocket path with a 426.
            response = self._without_head_body(
                request,
                self._native_json_response(
                    REPOSITORY_SOCKET_RETIRED_PAYLOAD,
                    status=410,
                    headers={
                        "cache-control": (
                            REPOSITORY_SOCKET_RETIRED_CACHE_CONTROL),
                        # CORS headers are added only for an allowed Origin,
                        # so a cached copy must never be reused across
                        # origins even when this one earned none.
                        "vary": "Origin",
                        "x-forkmesh-worker": "app",
                    },
                ),
            )
        elif (
            path == "/"
            and method in ("GET", "HEAD")
            and str(getattr(self.env, "SINGLE_WORKER_SITE", "") or "")
                .strip().lower() not in ("1", "true", "yes", "on")
        ):
            location = "/dashboard" + str(url.search or "")
            response = Response(
                None,
                status=308,
                headers={
                    "cache-control": "no-store",
                    "location": location,
                    "x-forkmesh-worker": "app",
                },
            )
        return (
            self._with_fast_cors(request, response)
            if response is not None else None
        )

    def _invalid_public_snapshot(self, reason):
        return self._native_json_response(
            {
                "error": "public_snapshot_unavailable",
                "reason": reason,
            },
            status=503,
            headers={
                "cache-control": "no-store, max-age=0, must-revalidate",
                "retry-after": "5",
                "x-forkmesh-worker": "app",
                "x-forkmesh-public-snapshot": reason,
                "cdn-cache-control": "no-store",
                "cloudflare-cdn-cache-control": "no-store",
                **EXPECTED_DEGRADED_HEADERS,
            },
        )

    async def _public_snapshot_live_fallback(
            self, request, url, reason):
        try:
            live = (
                await self._route(request, url)
                if getattr(self, "_inside_app_ingress", False)
                else await _app_ingress_fetch(self.env, request)
            )
            status = int(getattr(live, "status", 200) or 200)
            # An aborted ingress object now answers with its own marked 503
            # rather than raising, so keep reporting it as this path's own
            # failure reason instead of passing that generic body through as
            # though the live read had produced it.
            if status == 503 and _response_is_expected_degraded(live):
                return self._invalid_public_snapshot(reason + "-error")
            headers = [
                (str(name).lower(), value)
                for name, value in live.headers.items()
                if str(name).lower() not in {
                    "set-cookie", "cache-control", "x-forkmesh-worker",
                    "x-forkmesh-public-snapshot",
                }
            ]
            headers.extend((
                ("cache-control", "no-store, max-age=0, must-revalidate"),
                ("x-forkmesh-worker", "app"),
                ("x-forkmesh-public-snapshot", reason),
            ))
            if url.path in public_snapshots.SNAPSHOT_STATUS_PATHS:
                headers.extend((
                    ("cdn-cache-control", "no-store"),
                    ("cloudflare-cdn-cache-control", "no-store"),
                ))
            return Response(
                live.body,
                status=status,
                status_text=str(getattr(live, "status_text", "") or ""),
                headers=headers,
            )
        except BaseException:
            return self._invalid_public_snapshot(reason + "-error")

    async def _public_snapshot_response(self, request, url, path):
        try:
            row = await _public_snapshot_row(self.env, path)
        except BaseException:
            return self._invalid_public_snapshot("database-error")
        if not row:
            return await self._public_snapshot_live_fallback(
                request, url, "missing-fallback")

        now = int(Date.now())
        try:
            schema_version = _exact_public_snapshot_integer(
                row.get("schema_version"))
            generated_at = _exact_public_snapshot_integer(
                row.get("generated_at"))
            fresh_until = _exact_public_snapshot_integer(
                row.get("fresh_until"))
            stale_until = _exact_public_snapshot_integer(
                row.get("stale_until"))
            content_length = _exact_public_snapshot_integer(
                row.get("content_length"))
            payload_bytes = _exact_public_snapshot_integer(
                row.get("payload_bytes"))
            payload_json_valid = _exact_public_snapshot_integer(
                row.get("payload_json_valid"))
        except (TypeError, ValueError, OverflowError):
            return self._invalid_public_snapshot("invalid-or-expired")
        body = row.get("payload")
        if not isinstance(body, str):
            body = ""
        expected_fingerprint = str(
            getattr(self.env, "DEPLOY_FINGERPRINT", "") or "")
        fingerprint_pattern = r"[0-9a-f]{64}"
        if expected_fingerprint and not re.fullmatch(
                fingerprint_pattern, expected_fingerprint):
            return self._invalid_public_snapshot("invalid-app-fingerprint")
        safe_integer = lambda value: 0 <= value <= 9_007_199_254_740_991
        if (
            row.get("path") != path
            or schema_version != self._PUBLIC_SNAPSHOT_SCHEMA_VERSION
            or not safe_integer(generated_at)
            or generated_at <= 0
            or generated_at > now + 30_000
            or not safe_integer(fresh_until)
            or not safe_integer(stale_until)
            or fresh_until < generated_at
            or stale_until != fresh_until
            or not safe_integer(content_length)
            or content_length <= 1
            or content_length > self._PUBLIC_SNAPSHOT_MAX_BYTES[path]
            or payload_bytes != content_length
            or payload_json_valid != 1
        ):
            return self._invalid_public_snapshot("invalid-or-expired")

        kind = str(row.get("kind") or "")
        if kind == self._PUBLIC_SNAPSHOT_TOMBSTONE_KIND:
            if now >= fresh_until or content_length != 2 or body != "{}":
                return self._invalid_public_snapshot("invalid-barrier")
            return self._invalid_public_snapshot("privacy-transition")
        if kind != self._PUBLIC_SNAPSHOT_KIND:
            return self._invalid_public_snapshot("invalid-kind")

        status_path = path in public_snapshots.SNAPSHOT_STATUS_PATHS
        row_fingerprint = str(row.get("deploy_fingerprint") or "")
        fingerprint_mismatch = row_fingerprint != expected_fingerprint
        if fingerprint_mismatch:
            if not (
                re.fullmatch(fingerprint_pattern, row_fingerprint)
                and re.fullmatch(fingerprint_pattern, expected_fingerprint)
            ):
                return self._invalid_public_snapshot("invalid-fingerprint")
            if not status_path:
                return await self._public_snapshot_live_fallback(
                    request, url, "fingerprint-fallback")
        # The SELECT computed json_valid(payload) and its exact byte length in
        # SQLite above. The publisher also validates the projection schema
        # before its atomic upsert. Re-parsing the large status history in
        # Python on every page poll can itself exceed the stateless CPU limit,
        # so retain the extra reader-side shape check for the catalog documents
        # while status trusts the same validated D1 publication boundary.
        if not status_path:
            try:
                payload = _strict_public_snapshot_json(body)
            except Exception:
                return self._invalid_public_snapshot("invalid-json")
            if not public_snapshots.validate_snapshot_payload(
                    path, payload, generated_at):
                return self._invalid_public_snapshot("invalid-payload")
        expired = now >= fresh_until
        if expired and not status_path:
            return await self._public_snapshot_live_fallback(
                request, url, "expired-fallback")

        remaining_seconds = max(0, (fresh_until - now) // 1000)
        browser_max_age = (
            0
            if path in public_snapshots.SNAPSHOT_STATUS_PATHS
            else min(
                self._PUBLIC_SNAPSHOT_MAX_AGE[path], remaining_seconds)
        )
        snapshot_state = (
            "status-emergency-stale"
            if status_path and (expired or fingerprint_mismatch)
            else "fresh"
        )
        headers = {
            "vary": "Cookie, Authorization",
            "cache-control": (
                "no-store, max-age=0, must-revalidate"
                if status_path else
                "public, max-age=%d, must-revalidate" % browser_max_age
            ),
            "content-type": "application/json; charset=utf-8",
            "content-length": str(content_length),
            "x-forkmesh-worker": "app",
            "x-forkmesh-public-snapshot": snapshot_state,
            "x-forkmesh-snapshot-generated-at": str(generated_at),
            "x-forkmesh-snapshot-fingerprint": row_fingerprint,
        }
        if status_path:
            headers["cdn-cache-control"] = "no-store"
            headers["cloudflare-cdn-cache-control"] = "no-store"
            if snapshot_state != "fresh":
                headers["warning"] = (
                    '110 - "Serving last known status while live sampling '
                    'recovers"')
        return Response(body, status=200, headers=headers)

    async def _stateless_status_snapshot_response(self, request):
        """Serve the published status from the colo cache without Python D1.

        Cache API entries survive code deployments. The publisher validated
        the body before writing it, so a prior release's well-formed snapshot
        is safer than making the status page depend on an unavailable ingress
        object during rollout. Label that response emergency-stale until this
        deployment publishes its own generation.
        """
        if method_name(request) not in ("GET", "HEAD"):
            return None
        try:
            target = JsURL.new(str(request.url))
            app_origin = self._fast_normalized_http_origin(
                getattr(self.env, "APP_ORIGIN", "") or "")
            if not app_origin or str(target.origin or "") != app_origin:
                return None
            path = str(target.pathname or "")
            if path == "/api/status/world":
                view = "world"
            elif path == "/api/status":
                query = parse_qs(str(target.search or "")[1:])
                view = (
                    "world"
                    if query.get("view", ["full"])[0] == "world"
                    else "full"
                )
            else:
                return None
            cached = await edge_cache_match(STATUS_EDGE_CACHE_KEYS[view])
            if cached is None:
                return None
            fingerprint = str(
                cached.headers.get("x-forkmesh-snapshot-fingerprint") or "")
            expected_fingerprint = str(
                getattr(self.env, "DEPLOY_FINGERPRINT", "") or "")
            # These are public release identifiers, not secret material. The
            # exact shape check rejects an unrelated/corrupt cache response
            # without lazily importing hmac on the 10 ms path.
            if not re.fullmatch(r"[0-9a-f]{64}", fingerprint) or not (
                    re.fullmatch(r"[0-9a-f]{64}", expected_fingerprint)):
                return None
            deployment_mismatch = fingerprint != expected_fingerprint
            generated_at = int(
                cached.headers.get("x-forkmesh-snapshot-generated-at") or 0)
            now = int(Date.now())
            if generated_at <= 0 or generated_at > now + 30_000:
                return None
        except BaseException:
            return None
        snapshot_state = None
        if deployment_mismatch or now >= (
                generated_at + public_snapshots.SNAPSHOT_PATH_FRESH_MS[path]):
            snapshot_state = "status-emergency-stale"
        response = uncached_status_response(
            cached, snapshot_state=snapshot_state)
        response = self._without_head_body(request, response)
        return self._with_fast_cors(request, response)

    async def fetch(self, request):
        maintenance = _maintenance_gate_response(self.env, request)
        if maintenance is not None:
            return self._with_fast_cors(request, maintenance)
        if instance_features.branded(self.env):
            instance_features.remember_instance(self.env)
            await self._instance_plugins_load()
            plugin_path = urlparse(str(request.url)).path
            switched_off = instance_features.blocked(self.env, plugin_path)
            if switched_off:
                return self._instance_feature_off(plugin_path, switched_off)
        upgrade = str(request.headers.get("upgrade") or "").lower()
        if upgrade:
            # Never proxy a 101 through the generic ingress object: an
            # outbound upgraded fetch keeps its caller non-hibernateable and
            # would consume the Free Durable Object duration allowance for
            # the socket's lifetime. The signature-heavy node channel does a
            # short authorization call and then hands the upgrade directly to
            # its final hibernating ForkMeshNodes object. Every other active
            # socket uses the same short authorization/capture call and then
            # connects directly to its final hibernating Durable Object.
            try:
                socket_url = JsURL.new(str(request.url))
                app_origin = self._fast_normalized_http_origin(
                    getattr(self.env, "APP_ORIGIN", "") or "")
            except BaseException:
                socket_url = None
                app_origin = ""
            if (
                upgrade == "websocket"
                and socket_url is not None
                and str(socket_url.origin or "") == app_origin
                and str(socket_url.pathname or "") in (
                    "/api/nodes/events", "/api/nodes/events/")
            ):
                return await _node_events_direct_fetch(self.env, request)
            if (
                upgrade == "websocket"
                and socket_url is not None
                and str(socket_url.origin or "") == app_origin
                and _public_websocket_path(str(socket_url.pathname or ""))
            ):
                return await _websocket_direct_fetch(self.env, request)
            # Never let an arbitrary Upgrade header bypass ingress or enter
            # the ordinary Python router. Only the exact public WebSocket
            # families above can produce a 101.
            return self._with_fast_cors(
                request,
                self._native_json_response(
                    {"error": "unsupported_upgrade"}, status=426,
                    headers={
                        "cache-control": "no-store",
                        "x-forkmesh-worker": "app",
                    },
                ),
            )
        # Status is materialized into the Cache API after its validated D1 row
        # commits. A cache hit costs no D1 query and survives a brief shared
        # Durable Object outage. This intentionally precedes the auth/cookie
        # guard: status is public, and same-origin browsers commonly attach a
        # session cookie automatically. Missing or old-deployment cache entries
        # fall back through serialized ingress, where validation and any live
        # aggregation have the larger Durable Object CPU envelope.
        status_snapshot = await self._stateless_status_snapshot_response(
            request)
        if status_snapshot is not None:
            return status_snapshot
        # A cache miss must not make the status page depend on the shared
        # Durable Object allowance it is meant to observe. Read the single
        # materialized D1 row directly; _public_snapshot_response performs no
        # Python JSON parse for this already publisher-validated projection.
        # Only a genuinely missing row falls onward to the serialized live
        # aggregation fallback.
        if method_name(request) in ("GET", "HEAD"):
            try:
                status_url = JsURL.new(str(request.url))
                app_origin = self._fast_normalized_http_origin(
                    getattr(self.env, "APP_ORIGIN", "") or "")
                status_path = str(status_url.pathname or "")
            except BaseException:
                status_url = None
                app_origin = ""
                status_path = ""
            if (
                status_url is not None
                and app_origin
                and str(status_url.origin or "") == app_origin
                and status_path in ("/api/status", "/api/status/world")
            ):
                return await self._fetch_inner(request)
        # The marketing Worker's page-view relay is a tiny cookie-free POST
        # (one D1 read, at most two writes, no signatures). Answer it here
        # rather than spend a serialized ingress invocation per page view.
        if method_name(request) == "POST":
            try:
                relay_path = str(JsURL.new(str(request.url)).pathname or "")
            except BaseException:
                relay_path = ""
            if relay_path in (
                    "/api/attribution/capture", "/api/attribution/capture/"):
                return await self._fetch_inner(request)
        # Mutating, authenticated, and Upgrade requests can never use the
        # tiny public fast paths. Forward them before URL/CORS work so the
        # exact signature-heavy POST path that hit Free's 10 ms ceiling spends
        # almost no stateless Python CPU.
        if (
            method_name(request) not in ("GET", "HEAD")
            or self._request_has_header(request, "authorization")
            or self._request_has_header(request, "cookie")
        ):
            return await _app_ingress_fetch(self.env, request)
        fast = await self._stateless_fast_response(request)
        if fast is not None:
            return fast
        return await _app_ingress_fetch(self.env, request)

    async def _fetch_inner(self, request):
        maintenance = _maintenance_gate_response(self.env, request)
        if maintenance is not None:
            return maintenance
        read_only = _read_only(self.env)
        url = None
        try:
            url = urlparse(request.url)
            if _probe_ban_local:
                denied = _probe_ban.denied(self.env, request, url.path)
                if denied is not None:
                    return denied

            cross_origin = _request_origin(request)
            fed_cors = (
                await world_federation_cors(self.env, request, url.path)
                if cross_origin
                and cross_origin != _request_target_origin(request)
                and url.path.startswith("/api/")
                and world_federated_path(url.path) else {})
            response = (
                Response("", status=204, headers=fed_cors)
                if fed_cors and method_name(request) == "OPTIONS"
                else _api_cors_preflight_response(
                    self.env, request, url.path))
            if response is not None:
                response = self._control_response(response)

            elif (snapshot_path := self._public_snapshot_path(request, url)):
                response = await self._public_snapshot_response(
                    request, url, snapshot_path)

            elif _admin_path_rules.legacy_hidden(self.env, url.path):
                response = await self._serve_not_found_page(url)
            elif (admin_subpage := _admin_path_rules.subpage(
                    self.env, url.path)):
                response = await self._admin_subpage(
                    request, url, admin_subpage)
            # Admin error dashboard at a secret, env-configured path. Its 401/403
            # responses are not logged, but unexpected exceptions are captured.
            elif (
                (admin_path := _admin_path(self.env))
                and url.path.strip("/") == admin_path
            ):
                response = await self._admin(request)
            else:
                response = await self._route(request, url)
            if url.path.rstrip("/") in (
                    "/api/repositories", "/api/repositories/search"):
                response = await self._repo_canonical_json(
                    request, url, response)
            if fed_cors and method_name(request) != "OPTIONS":
                response = self._with_headers(response, fed_cors)
        except RequestBodyTooLarge:
            response = json_response(
                {"error": "request_too_large"},
                status=413,
                cache_control="no-store, max-age=0, must-revalidate",
            )
        except ReadOnlyModeError:
            response = _read_only_response()
        except Exception as error:
            if str(error) == "legacy_custody_migration_required":
                response = _legacy_custody_not_ready_response()
            # An unverified account reaching a write API is an expected refusal
            # (_account_session_record), not a worker fault: answer the 403 the
            # clients read instead of logging it as a 5xx.
            elif str(error) == UNVERIFIED_EMAIL_ERROR:
                response = _email_not_verified_response()
            elif _is_client_disconnect(error):
                response = json_response(
                    {"error": "client_disconnected"}, status=499,
                    cache_control="no-store")
            elif _is_too_many_fields(error):
                response = json_response(
                    {"error": "too_many_fields"}, status=400,
                    cache_control="no-store")
            # D1 being down or overloaded is a dependency outage, not a worker
            # bug: _d1_read already replayed the one-off "internal error;
            # reference = …" fault, so anything still escaping means the
            # database cannot serve this request. Answer 503 (marked degraded,
            # so the 5xx logger below does not double-log it) instead of
            # re-raising into a Cloudflare 1101 that clients read as a 500.
            elif _is_d1_platform_error(error):
                try:
                    await log_d1_unavailable(
                        self.env, request, getattr(url, "path", ""), error)
                except BaseException:
                    pass
                response = json_response(
                    {"error": "database_unavailable"}, status=503,
                    cache_control="no-store",
                    extra_headers={
                        **EXPECTED_DEGRADED_HEADERS,
                        "Retry-After": "2",
                    })
            else:
                try:
                    await capture_worker_exception(self.env, request, url, error)
                except BaseException:
                    pass
                raise
        response = _api_cors_response(
            self.env, request, response,
            str(getattr(url, "path", "") or ""),
        )
        response = _apply_browser_security_headers(
            response, str(getattr(url, "path", "") or "/")
        )
        private_console = url is not None and _admin_path_rules.is_console(
            self.env, url.path)
        if private_console:
            response = _private_console_headers(response)
        try:
            status = int(getattr(response, "status", 200) or 200)
        except Exception:
            status = 200
        # Acquisition attribution rides the same served-page gate as the
        # referral counter below — a top-level text/html GET and nothing
        # else — so git, raw-blob and API requests never see it. It can only
        # add a Set-Cookie to a response that already exists.
        if not read_only and not private_console:
            response = await _attribution.capture_page_view(
                self.env, request, url, response, status)
        # A failed forkmesh/forkmesh content read is /status evidence that
        # the repository is not loading (api_metrics.record_api_request counts
        # only those). Per-route traffic statistics are not stored: the
        # desktop builds them from the Worker log tail. Gated on 5xx so a
        # healthy request never even loads the lazy module.
        if not read_only and status >= 500 and url is not None and str(
                getattr(url, "path", "")).startswith("/api/repo/"):
            await _api_metrics.record_api_request(self.env, url.path, status)
        # Credit the website that sent this visitor. Gated to served page
        # navigations carrying an external Referer, so the extra D1 write only
        # happens on the rare request that is actually an inbound referral.
        if not read_only and not private_console:
            await record_site_referral(self.env, request, url, status)
        # Blog reach is recorded from the same successfully served top-level
        # navigation. It stores totals, a fixed aggregate HLL projection and
        # per-post referring-host counters only.
        if not read_only:
            await record_blog_visit(self.env, request, url, status)
        if not read_only and status == 404 and url is not None:
            await _not_found_log.record_response(self.env, request, url)
        if status >= 500:
            # An offline node's content being re-requested (502/503/504 on a
            # tunnel path) is expected in a P2P network — host_presence and
            # the /status git_hosting signal already reflect it. Logging every
            # hit cost a blocking Sentry call + a D1 write per response (a
            # single offline node's release blob generated hundreds of noise
            # rows a day and drowned out real failures). A 500 on the same
            # path is a real worker bug and still logs.
            if status in (502, 503, 504) and _is_tunnel_content_path(url.path):
                return response
            # Deliberate degraded answers (DO aborts already recorded with
            # their real reason, fail-closed 501s, unavailable upstreams) are
            # marked at the source; re-logging them as anonymous "response
            # status N" events only buried real bugs.
            if _response_is_expected_degraded(response):
                return response
            await log_error(
                self.env, status, method_name(request), url.path,
                "response status %d" % status, request.headers.get("cf-ray") or "",
                request=request,
            )
        return response

    async def _admin_subpage(self, request, url, name):
        if name == "theme":
            return await admin_theme_handler(self.env, request)
        if name == "acquisition":
            return await _attribution.admin_dashboard_handler(
                self.env, request)
        if instance_features.blocked(self.env, "/admin/hackathon"):
            return await self._serve_not_found_page(url)
        return await hackathon_admin_page_handler(self.env, request)

    async def _admin(self, request):
        await ensure_schema(self.env)
        if not await _check_admin_page_auth(self.env, request):
            if method_name(request) == "GET":
                url = urlparse(request.url)
                next_path = url.path + (("?" + url.query) if url.query else "")
                return Response(
                    "Admin login required.",
                    status=302,
                    headers={
                        "location": "/login?next=" + quote(next_path),
                        "cache-control": "no-store, max-age=0, must-revalidate",
                        "content-type": "text/plain; charset=utf-8",
                    },
                )
            return Response(
                "Admin account required.",
                status=403,
                headers={
                    "cache-control": "no-store, max-age=0, must-revalidate",
                    "content-type": "text/plain; charset=utf-8",
                },
            )
        params = parse_qs(urlparse(request.url).query)
        # Nav links carry ?admin= for continuity; fall back to the cookie's
        # name when the page was opened without the param (adhoc #168).
        account_cookie_name = await _account_cookie_name(self.env, request)
        admin_query = _admin_query(
            params.get("admin", [""])[0] or account_cookie_name)

        # POST action ?action=disburse reports migration-only custody status;
        # ?action=set_password resets a user account's login password. Every
        # state-changing POST must carry a CSRF token rendered into the admin
        # page, so a cross-site form cannot trigger these actions.
        banner = ""
        action = params.get("action", [""])[0]
        audit_details = {}
        csrf_field = ('<input type="hidden" name="csrf" value="%s">'
                      % _html_escape(_admin_csrf_token(self.env)))
        # The Support sections (mobile bug reports, docs and World feedback)
        # own their page and their one write, the triage status of a report.
        if params.get("view", [""])[0] in ("bugs", "feedback", "agent-notes"):
            return await admin_support_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if params.get("view", [""])[0] == "marketing-email":
            return await marketing_email_admin_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if params.get("view", [""])[0] == _not_found_log.VIEW:
            return await _not_found_log.admin_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if params.get("view", [""])[0] == _instance_directory_admin.VIEW:
            return await _instance_directory_admin.admin_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if params.get("view", [""])[0] in _instance_console.VIEWS:
            return await _instance_console.admin_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if params.get("view", [""])[0] in _server_update_admin.VIEWS:
            return await _server_update_admin.admin_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if params.get("view", [""])[0] == _ids_console.VIEW:
            return await _ids_console.admin_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if params.get("view", [""])[0] == "ecosystem":
            return await admin_ecosystem_handler(
                self.env, request, params, csrf_field, admin_query,
                account_cookie_name)
        if method_name(request) == "POST":
            try:
                form = parse_qs(await request.text(), keep_blank_values=True)
            except Exception:
                form = {}
            if not _admin_csrf_ok(self.env, form):
                banner = ("Action blocked: invalid or missing CSRF token. "
                          "Reload the admin page and try again.")
            elif action == "disburse":
                try:
                    banner = await _admin_disburse(self.env)
                except Exception as error:
                    banner = "Disburse failed: " + repr(error)
            elif action == "set_password":
                try:
                    banner = await _admin_set_password(
                        self.env,
                        form.get("name", [""])[0],
                        form.get("password", [""])[0],
                    )
                except Exception as error:
                    banner = "Set password failed: " + repr(error)
            elif action == "update_user":
                try:
                    banner, audit_details = await _admin_update_user(
                        self.env, request, form, account_cookie_name)
                except Exception as error:
                    banner = "Update user failed: " + repr(error)
            elif action == "resend_verify":
                try:
                    banner = await _admin_resend_verification(
                        self.env,
                        request,
                        form.get("name", [""])[0],
                    )
                except Exception as error:
                    banner = "Resend verify email failed: " + repr(error)
            elif action == "request_ownership":
                try:
                    banner = await _admin_console_request_ownership(
                        self.env,
                        form.get("target", [""])[0],
                        form.get("owner", [""])[0],
                    )
                except Exception as error:
                    banner = "Ownership request failed: " + repr(error)
            elif action == "set_signup_referral_code":
                try:
                    enabled = form.get("enabled", [""])[0] == "1"
                    await _set_admin_signup_referral_code_mode(
                        self.env, enabled)
                    banner = (
                        "Signup referral code requirement enabled."
                        if enabled else
                        "Signup referral code requirement disabled."
                    )
                    audit_details = {"enabled": enabled}
                except Exception as error:
                    banner = (
                        "Signup referral code update failed: " + repr(error))
            elif action == "set_repo_terms_flag":
                try:
                    owner = safe_segment(
                        form.get("owner", [""])[0]).lower()
                    repo = safe_segment(
                        form.get("repo", [""])[0]).lower()
                    category = clean_string(
                        form.get("category", [""])[0], 20).lower()
                    note = clean_string(form.get("note", [""])[0], 500)
                    clear = form.get("clear", [""])[0] == "1"
                    if (
                        not owner
                        or not repo
                        or category not in (
                            "spam", "malware", "harassment",
                            "illegal", "other",
                        )
                        or (not clear and not note)
                    ):
                        banner = "Repository Terms flag failed: invalid fields."
                    else:
                        repo_bi = await blind_index(
                            self.env, owner + "/" + repo)
                        exists = await d1_first(
                            self.env,
                            "SELECT 1 AS found FROM repositories "
                            "WHERE key_bi=? LIMIT 1",
                            repo_bi,
                        )
                        if not exists:
                            banner = (
                                "Repository Terms flag failed: repository "
                                "was not found."
                            )
                        else:
                            await d1_run(
                                self.env,
                                "INSERT INTO repo_terms_flags "
                                "(repo_bi,active,category,data,updated_by_bi,"
                                "updated_at) VALUES (?,?,?,?,?,?) "
                                "ON CONFLICT(repo_bi) DO UPDATE SET "
                                "active=excluded.active,"
                                "category=excluded.category,"
                                "data=excluded.data,"
                                "updated_by_bi=excluded.updated_by_bi,"
                                "updated_at=excluded.updated_at",
                                repo_bi,
                                0 if clear else 1,
                                category,
                                await encrypt_row(self.env, {
                                    "note": note,
                                    "actor": account_cookie_name,
                                }),
                                await blind_index(
                                    self.env, account_cookie_name),
                                int(Date.now()),
                            )
                            await purge_catalog_related_caches()
                            await schedule_public_catalog_republish(self.env)
                            banner = (
                                "Repository Terms flag cleared."
                                if clear
                                else "Repository Terms flag applied."
                            )
                            audit_details = {
                                "active": not clear,
                                "category": category,
                            }
                except Exception as error:
                    banner = "Repository Terms flag failed: " + repr(error)
            elif action == "create_bot_task":
                try:
                    banner, audit_details = (
                        await _admin_error_create_bot_task(
                            self.env, form, account_cookie_name))
                except Exception as error:
                    banner = "Create bot task failed: " + repr(error)
            elif action == "delete_error_row":
                try:
                    raw_id = form.get("error_id", [""])[0]
                    rowid = int(raw_id) if str(raw_id).isdigit() else 0
                    if rowid <= 0:
                        banner = "Delete error failed: invalid row."
                    else:
                        existing = await d1_first(
                            self.env,
                            "SELECT 1 AS found FROM error_log WHERE rowid=?",
                            rowid,
                        )
                        audit_details = {
                            "rowCount": 1 if existing else 0,
                            "rowDigestBefore": (
                                await _admin_selected_rows_digest(
                                    self.env, "error_log", [rowid])
                            ),
                        }
                        await d1_run(
                            self.env,
                            "DELETE FROM error_log WHERE rowid=?",
                            rowid,
                        )
                        banner = (
                            "Deleted error row."
                            if existing
                            else "Delete error: row was already gone."
                        )
                except Exception as error:
                    banner = "Delete error failed: " + repr(error)
            elif action == "delete_error_group":
                try:
                    group_status = str(
                        form.get("group_status", [""])[0])[:12]
                    group_method = str(
                        form.get("group_method", [""])[0])[:12]
                    group_path = str(
                        form.get("group_path", [""])[0])[:2000]
                    group_message = str(
                        form.get("group_message", [""])[0])[:1000]
                    if not group_status or not group_method:
                        banner = "Delete error group failed: invalid group."
                    else:
                        count_row = await d1_first(
                            self.env,
                            "SELECT COUNT(*) AS n FROM error_log "
                            "WHERE CAST(status AS TEXT)=? AND UPPER(method)=? "
                            "AND path=? AND message=?",
                            group_status, group_method.upper(),
                            group_path, group_message,
                        )
                        count = int((count_row or {}).get("n") or 0)
                        audit_details = {
                            "rowCount": count,
                            # Content stays out of the audit trail. This
                            # bounded digest still distinguishes group purges.
                            "groupDigest": hashlib.sha256(
                                (
                                    group_status + "\n"
                                    + group_method.upper() + "\n"
                                    + group_path + "\n" + group_message
                                ).encode("utf-8")
                            ).hexdigest(),
                        }
                        await d1_run(
                            self.env,
                            "DELETE FROM error_log "
                            "WHERE CAST(status AS TEXT)=? AND UPPER(method)=? "
                            "AND path=? AND message=?",
                            group_status, group_method.upper(),
                            group_path, group_message,
                        )
                        banner = "Deleted %d error(s) in the group." % count
                except Exception as error:
                    banner = "Delete error group failed: " + repr(error)
            elif action == "delete_rows":
                try:
                    tables = await _admin_list_tables(self.env)
                    table = params.get("table", [""])[0]
                    ids = [int(x) for x in form.get("ids", []) if str(x).isdigit()]
                    if table not in tables:
                        banner = "Delete failed: unknown table."
                    elif not _admin_purge_allowed(table):
                        banner = (
                            "Delete blocked: this table is read-only in the "
                            "generic administration browser."
                        )
                    elif not ids:
                        banner = "Delete: no rows were selected."
                    elif await _admin_selected_rows_have_wallet_keys(
                            self.env, table, ids):
                        banner = (
                            "Delete blocked: a selected row contains frozen "
                            "legacy wallet custody material. Use the explicit "
                            "offline migration."
                        )
                    else:
                        audit_details = {
                            "rowCount": len(ids),
                            "rowDigestBefore": (
                                await _admin_selected_rows_digest(
                                    self.env, table, ids
                                )
                            ),
                        }
                        if table == "repositories":
                            purged = await _admin_purge_repository_rows(
                                self.env, ids, account_cookie_name)
                            banner = "Purged %d repositor%s." % (
                                purged, "y" if purged == 1 else "ies")
                        else:
                            # D1 caps bound parameters per query (~100), so
                            # delete in chunks rather than one giant IN list.
                            chunk = 90
                            for start in range(0, len(ids), chunk):
                                batch = ids[start:start + chunk]
                                placeholders = ",".join(["?"] * len(batch))
                                await d1_run(
                                    self.env,
                                    "DELETE FROM " + table
                                    + " WHERE rowid IN (" + placeholders + ")",
                                    *batch,
                                )
                            banner = "Deleted %d row(s) from %s." % (
                                len(ids), table)
                except Exception as error:
                    banner = "Delete failed: " + repr(error)
            elif action in ("update_row", "insert_row"):
                try:
                    tables = await _admin_list_tables(self.env)
                    table = params.get("table", [""])[0]
                    if table not in tables:
                        banner = "Save failed: unknown table."
                    else:
                        banner = (
                            "Save blocked: generic database mutation is "
                            "disabled; use a purpose-built audited action."
                        )
                except Exception as error:
                    banner = "Save failed: " + repr(error)

            if action in (
                    "disburse", "set_password", "resend_verify",
                    "update_user",
                    "request_ownership", "delete_rows", "update_row",
                    "insert_row",
                    "set_signup_referral_code",
                    "set_repo_terms_flag", "delete_error_row",
                    "delete_error_group", "create_bot_task"):
                audit_actor = (
                    account_cookie_name
                    or params.get("admin", [""])[0])
                target_type = (
                    "account" if action in (
                        "set_password", "resend_verify", "update_user",
                        "request_ownership")
                    else "signup_policy"
                    if action == "set_signup_referral_code"
                    else "repository"
                    if action == "set_repo_terms_flag"
                    else "error_log"
                    if action in (
                        "delete_error_row", "delete_error_group",
                        "create_bot_task")
                    else "database_table" if action in (
                        "delete_rows", "update_row", "insert_row")
                    else "legacy_custody")
                target = (
                    form.get("name", [""])[0] if action in (
                        "set_password", "resend_verify", "update_user")
                    else form.get("target", [""])[0]
                    if action == "request_ownership"
                    else "referral-code-requirement"
                    if action == "set_signup_referral_code"
                    else (
                        form.get("owner", [""])[0] + "/"
                        + form.get("repo", [""])[0]
                    )
                    if action == "set_repo_terms_flag"
                    else "error_log"
                    if action in (
                        "delete_error_row", "delete_error_group",
                        "create_bot_task")
                    else params.get("table", [""])[0])
                lowered_banner = str(banner or "").lower()
                outcome = (
                    "denied" if "blocked" in lowered_banner
                    else "failed" if "failed" in lowered_banner
                    else "success")
                await _audit_sensitive_action(
                    self.env, audit_actor, "admin.console_" + action,
                    target_type, target, outcome,
                    audit_details)
            # Console POSTs are the writes that move nav row counts.
            await _admin_table_counts_forget()

        # Each console section is its own page inside the shared admin shell.
        # A section reads only what it shows: the overview pays for the stats
        # snapshot, the Database section for the table inventory and row
        # counts, and the rest for neither.
        if params.get("view", [""])[0] == "alerts":
            # Alert delivery moved to the Pings settings page (#347); outage
            # mail sent before the move still links here.
            return Response(
                "Moved to Pings settings.",
                status=302,
                headers={
                    "location": STATUS_ALERT_MANAGE_URL,
                    "cache-control": "no-store, max-age=0, must-revalidate",
                    "content-type": "text/plain; charset=utf-8",
                },
            )
        view = _admin_console_view(params)
        request_url = urlparse(request.url)
        next_path = request_url.path + (
            ("?" + request_url.query) if request_url.query else "")
        if view == "controls":
            content = _render_admin_controls(
                await _admin_signup_referral_code_state(self.env),
                csrf_field, admin_query)
        elif view == "database":
            # Table browser: pick the requested table (validated against the
            # live list), defaulting to the error log.
            tables = await _admin_list_tables(self.env)
            requested = params.get("table", [""])[0]
            if requested in tables:
                active = requested
            elif "error_log" in tables:
                active = "error_log"
            else:
                active = tables[0] if tables else ""

            # Row counts for the table list, batched (see _admin_table_counts)
            # and shared for a few minutes; Recount asks for fresh ones.
            counts = await _admin_table_counts_cached(
                self.env, tables,
                refresh=bool(params.get("refresh_counts", [""])[0]))

            # Every visible record has a read-only, vertically listed detail
            # page. Legacy edit/new bookmarks remain fail-closed below.
            if action == "detail" and active:
                table_html = await _render_record_detail(
                    self.env,
                    active,
                    params.get("rowid", [""])[0],
                    admin_query,
                )
            elif action == "edit" and active:
                rowid = params.get("rowid", [""])[0]
                table_html = await _render_row_form(
                    self.env, active, rowid, csrf_field, admin_query)
            elif action == "new" and active:
                table_html = await _render_row_form(
                    self.env, active, None, csrf_field, admin_query)
            else:
                table_html = (await _render_table_view(
                                  self.env, active, csrf_field, admin_query,
                                  params.get("user", [""])[0],
                                  known_total=counts.get(active),
                                  params=params)
                              if active
                              else '<p class="empty">No tables found.</p>')
            # Default the table list to most-records-first; ?sort=name opts
            # back into the A–Z ordering.
            sort_records = params.get("sort", [""])[0] != "name"
            content = _render_admin_database(
                tables, active, counts, table_html, admin_query, sort_records)
        elif view in ADMIN_VIEW_TABLES:
            # Users, Errors and Install diagnostics are purpose-built views of
            # one table each.
            content = await _render_table_view(
                self.env, ADMIN_VIEW_TABLES[view], csrf_field, admin_query,
                params.get("user", [""])[0], params=params)
        else:
            content = await _render_admin_overview(
                self.env, await admin_stats(self.env), admin_query)
        return Response(
            render_admin_html(
                view, content, banner, admin_query=admin_query,
                console=_admin_shell.console_base(self.env),
                theme=_admin_shell.theme_from_request(request),
                badges=await _admin_shell.nav_badges(self.env),
                account=account_cookie_name, next_path=next_path,
                subpages=_admin_shell.subpage_base(self.env)),
            status=200,
            headers={"content-type": "text/html; charset=utf-8",
                     "cache-control": "no-store",
                     "x-robots-tag": "noindex, nofollow"},
        )

    def _instance_feature_off(self, path, feature):
        if path.startswith("/api/"):
            return json_response(
                {"error": "plugin_disabled", "plugin": feature}, status=404,
                cache_control="no-store")
        name = _html_escape(instance_name(self.env))
        return Response(
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,"
            "initial-scale=1\"><title>Not available - " + name + "</title>"
            "</head><body style=\"font-family:system-ui,sans-serif;"
            "max-width:36rem;margin:4rem auto;padding:0 1rem\"><h1>Not "
            "available here</h1><p>The <code>" + _html_escape(feature) +
            "</code> plugin is turned off on " + name + ". An administrator "
            "can turn it on in the instance settings.</p><p><a href=\"/\">"
            "Back to " + name + "</a></p>" + (
                "<p style=\"color:#71717a;font-size:.85rem\">Powered by "
                "<a href=\"https://forkmesh.com/\">ForkMesh</a></p>"
                if instance_features.powered_by(self.env) else "")
            + "</body></html>",
            status=404,
            headers={"content-type": "text/html; charset=utf-8",
                     "cache-control": "no-store"},
        )

    def _instance_page(self, request, url):
        if method_name(request) not in ("GET", "HEAD"):
            return None
        if url.path.startswith("/docs/") and url.path != "/docs/":
            return Response("", status=308, headers={"location": "/docs"})
        origin = _normalized_http_origin(
            getattr(self.env, "APP_ORIGIN", "") or "") or (
                url.scheme + "://" + url.netloc)
        body = instance_pages.page_for(
            url.path, instance_name(self.env), origin,
            instance_features.powered_by(self.env))
        if not body:
            return None
        return self._without_head_body(request, Response(
            body, status=200,
            headers={"content-type": "text/html; charset=utf-8",
                     "cache-control": "public, max-age=300"}))

    async def _instance_plugins_load(self):
        now = int(Date.now())
        if instance_features.fresh(now):
            return
        try:
            row = await d1_first(
                self.env, "SELECT doc FROM instance_settings WHERE k=?",
                instance_features.SETTING_KEY)
        except Exception:
            instance_features.remember(instance_features.current(), now)
            return
        instance_features.remember(
            instance_features.parse((row or {}).get("doc") or ""), now)

    async def _instance_directory_setting(self):
        try:
            row = await d1_first(
                self.env, "SELECT doc FROM instance_settings WHERE k=?",
                instance_registry.SETTING_KEY)
        except Exception:
            row = None
        if row is None:
            return None
        return instance_registry.parse_setting(row.get("doc") or "")

    async def _instance_directory_document(self, request):
        setting = await self._instance_directory_setting()
        domain = instance_registry.domain_of(
            getattr(self.env, "PUBLIC_BASE_URL", "") or "")
        public_key = _https_mirror_router_public_key(self.env)
        if setting is None or not domain or not public_key:
            return json_response({"error": "not_found"}, status=404,
                                 cache_control="no-store")
        return self._without_head_body(request, json_response(
            instance_registry.document(
                domain, instance_name(self.env),
                getattr(self.env, "INSTANCE_LOGO", "") or "", setting,
                "world" in instance_features.enabled_ids(self.env),
                public_key), cache_control="no-store"))

    async def _instance_directory_links(self):
        if not instance_branded(self.env):
            return None
        setting = await self._instance_directory_setting()
        if setting is None:
            return None
        return {
            "listed": setting["listed"],
            "world": setting["world"],
            "portal": instance_registry.world_portal(
                setting, "world" in instance_features.enabled_ids(self.env)),
        }

    async def _instance_legal_setting(self):
        if not instance_branded(self.env):
            return instance_legal.default()
        now = int(Date.now())
        setting = instance_legal.cached(now)
        if setting is not None:
            return setting
        try:
            row = await d1_first(
                self.env, "SELECT doc FROM instance_settings WHERE k=?",
                instance_legal.SETTING_KEY)
        except Exception:
            row = None
        return instance_legal.remember(
            instance_legal.parse((row or {}).get("doc") or ""), now)

    async def _instance_legal_route(self, request, url):
        if method_name(request) not in ("GET", "HEAD"):
            return None
        security_txt = url.path == instance_legal.SECURITY_TXT_PATH
        page = "" if security_txt else instance_legal.route_page(url.path)
        if not security_txt and not page:
            return None
        setting = await self._instance_legal_setting()
        if security_txt:
            origin = _normalized_http_origin(
                getattr(self.env, "APP_ORIGIN", "") or "") or (
                    url.scheme + "://" + url.netloc)
            body = instance_legal.security_txt(setting, origin,
                                               int(Date.now()))
            if not body:
                return None
            return self._without_head_body(request, Response(
                body, status=200,
                headers={"content-type": "text/plain; charset=utf-8",
                         "cache-control": "public, max-age=300"}))
        if not instance_legal.enabled(setting, page):
            if url.path.rstrip("/") == instance_legal.PATHS["security"]:
                return None
            return await self._serve_not_found_page(url)
        entry = setting[page]
        if entry["mode"] == "url":
            return Response("", status=302, headers={
                "location": entry["url"], "cache-control": "no-store"})
        if url.path != instance_legal.PATHS[page]:
            return Response("", status=302, headers={
                "location": instance_legal.PATHS[page],
                "cache-control": "no-store"})
        return self._without_head_body(request, Response(
            instance_legal.page_html(instance_name(self.env), page,
                                     entry["markdown"]),
            status=200,
            headers={"content-type": "text/html; charset=utf-8",
                     "cache-control": "public, max-age=60"}))

    async def _instance_theme(self):
        if not instance_branded(self.env):
            return {}
        try:
            row = await d1_first(
                self.env,
                "SELECT doc FROM instance_settings WHERE k='theme'")
        except Exception:
            return {}
        return instance_theme.parse((row or {}).get("doc") or "")

    async def _instance_theme_write(self, request):
        try:
            data = await bounded_json_request(request, max_bytes=16 * 1024)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400,
                                 cache_control="no-store")
        if not isinstance(data, dict):
            data = {}
        token, cookie_auth = _request_account_session_token(request, data)
        if cookie_auth and not _request_origin_allowed(self.env, request):
            token = ""
        actor = await _account_session_token_name(self.env, token)
        if not actor or not await _is_admin(self.env, actor):
            return json_response({"error": "not_admin"}, status=403,
                                 cache_control="no-store")
        try:
            theme = instance_theme.clean(data.get("theme"))
            doc = instance_theme.dump(theme)
        except instance_theme.ThemeError as error:
            return json_response(
                {"error": "invalid_theme", "detail": str(error)},
                status=400, cache_control="no-store")
        await d1_run(
            self.env,
            "INSERT INTO instance_settings (k, doc, updated_at) "
            "VALUES ('theme', ?, ?) ON CONFLICT(k) DO UPDATE SET "
            "doc=excluded.doc, updated_at=excluded.updated_at",
            doc, int(Date.now()))
        return json_response({"ok": True, "theme": theme},
                             cache_control="no-store")

    async def _route(self, request, url):
        switched_off = instance_features.blocked(self.env, url.path)
        if switched_off:
            return self._instance_feature_off(url.path, switched_off)
        if url.path in ("/api/instance", "/api/instance/"):
            try:
                login_deadline = int(getattr(
                    self.env, "INSTANCE_LOGIN_DEADLINE", "") or 0)
            except (TypeError, ValueError):
                login_deadline = 0
            return json_response({
                "ok": True,
                "name": instance_name(self.env),
                "branded": instance_branded(self.env),
                "logo": instance_logo_url(self.env),
                "loginDeadline": login_deadline,
                "gitNode": str(getattr(self.env, "INSTANCE_FLAGSHIP_NODE", "")
                               or "").strip().lower()[:63],
                "poweredBy": ({"name": "ForkMesh",
                               "url": "https://forkmesh.com/"}
                              if str(getattr(self.env, "INSTANCE_POWERED_BY", "")
                                     or "1").strip() != "0" else None),
                "plugins": instance_features.describe(self.env),
                "offsiteHosts": list(instance_features.OFFSITE_HOSTS),
                "theme": await self._instance_theme(),
                "legal": instance_legal.links(
                    await self._instance_legal_setting(),
                    instance_branded(self.env)),
                "repoAddresses": await self._repo_addresses_doc(),
                "directory": await self._instance_directory_links(),
            }, cache_control="public, max-age=60")
        if url.path == "/api/instance/theme" and instance_branded(self.env) \
                and method_name(request) in ("PUT", "POST"):
            return await self._instance_theme_write(request)
        if url.path == "/instance-theme.css" and instance_branded(self.env) \
                and method_name(request) in ("GET", "HEAD"):
            return self._without_head_body(request, Response(
                instance_theme.css(await self._instance_theme()), status=200,
                headers={"content-type": "text/css; charset=utf-8",
                         "cache-control": "public, max-age=60"}))
        if instance_branded(self.env) and \
                url.path == instance_registry.DOC_PATH and \
                method_name(request) in ("GET", "HEAD"):
            return await self._instance_directory_document(request)
        if instance_branded(self.env):
            legal_page = await self._instance_legal_route(request, url)
            if legal_page is not None:
                return legal_page
            instance_page = self._instance_page(request, url)
            if instance_page is not None:
                return instance_page
        delegated = self._world_ingress_redirect(request, url)
        if delegated is not None:
            return delegated

        if str(request.headers.get("authorization") or "")[7:].strip() \
                .startswith("fmat_"):
            denied = await apps_rest_gate(self.env, request, url.path)
            if denied is not None:
                return denied

        if url.path.startswith("/api/plugins/"):
            plugin_response = await plugin_http_dispatch(
                request, self.env, url.path, method_name(request))
            if plugin_response is not None:
                return plugin_response

        if url.path in (
                HTTPS_MIRROR_ENDPOINT_PATH,
                HTTPS_MIRROR_ENDPOINT_PATH + "/"):
            return await self._mirror_identity_control_response(request)
        if url.path.rstrip("/") == HTTPS_MIRROR_PRIVATE_ROUTE_PATH:
            return await https_mirror_private_route_handler(self.env, request)
        if url.path.rstrip("/") == "/api/ssh/authorize":
            return await ssh_gateway_authorize_handler(self.env, request)

        private_access_match = PRIVATE_REPLICA_ACCESS_RE.match(url.path)
        if private_access_match:
            return await _https_mirror_private_access_handler(
                self.env, request, url, private_access_match.group(1))

        # Organization repo aliases (issue #388): rewrite /<org>/<repo> git and
        # /api/repo/<org>/<repo>/... requests to the linked node's namespace
        # before any route matching — see org_alias_rewrite.
        aliased = await org_alias_rewrite(self.env, request, url)
        if aliased is not None:
            request, url = aliased

        repo_api = REPO_API_PREFIX_RE.match(url.path)
        if repo_api:
            owner = safe_segment(repo_api.group(1))
            repo = safe_segment(repo_api.group(2))
            refusal = await read_only_mirror_refusal(
                self.env, request, owner, repo, url.path)
            if refusal is not None:
                return refusal
            pieces = [piece for piece in url.path.split("/") if piece]
            action = safe_segment(pieces[4] if len(pieces) > 4 else "")
            if action == "viewer-permission" and len(pieces) == 5:
                return await repo_viewer_permission_handler(
                    self.env, request, owner, repo)
            if action == "security":
                import security_advisories
                security_advisories._bind_runtime(globals())
                return await security_advisories.repo_route(
                    self.env, request, owner, repo)
            if action == "file-edit":
                import repository_file_edits
                repository_file_edits._bind_runtime(globals())
                return await repository_file_edits.route(
                    self.env, request, owner, repo, action)
            if action in ("dir-about", "dir-logo"):
                import directory_about
                directory_about._bind_runtime(globals())
                return await directory_about.route(
                    self.env, request, owner, repo, action,
                    projects=self._repo_projects_setting,
                    short=self._repo_short_setting)

        # Public clone traffic uses signed, masked HTTPS. Private/missing paths
        # fail closed in _git_host. Private clients use only the identity-free
        # opaque endpoint above; named Git paths never advertise or proxy it.
        git_info = GIT_INFO_RE.match(url.path)
        if git_info:
            service = parse_qs(url.query).get("service", [""])[0]
            if service == "git-upload-pack":
                owner = safe_segment(git_info.group(1))
                repo = safe_segment(git_info.group(2))
                if (
                    owner and repo
                    and not await _repo_is_private(self.env, owner, repo)
                ):
                    return await _git_error_as_text(await _https_mirror_proxy(
                        self.env, request, owner, repo, "git-info-refs"),
                        self.env)
                return await _git_error_as_text(
                    await self._git_host(request, owner, repo), self.env)
            if service == "git-receive-pack":
                return await _git_error_as_text(await self._git_push(
                    request, git_info.group(1), git_info.group(2)), self.env)
        git_pack = GIT_PACK_RE.match(url.path)
        if git_pack and method_name(request) == "POST":
            owner = safe_segment(git_pack.group(1))
            repo = safe_segment(git_pack.group(2))
            if (
                owner and repo
                and not await _repo_is_private(self.env, owner, repo)
            ):
                return await _https_mirror_proxy(
                    self.env, request, owner, repo, "git-upload-pack")
            return await self._git_host(request, owner, repo)
        git_recv = GIT_RECEIVE_RE.match(url.path)
        if git_recv:
            if method_name(request) != "POST":
                return json_response(
                    {"error": "method_not_allowed"}, status=405)
            return await self._git_push(
                request, git_recv.group(1), git_recv.group(2))

        if url.path == "/" and method_name(request) in ("GET", "HEAD"):
            single_site = str(
                getattr(self.env, "SINGLE_WORKER_SITE", "") or ""
            ).strip().lower() in ("1", "true", "yes", "on")
            if single_site:
                return await self._serve_homepage(url)
            redirect = self._app_root_redirect(request, url)
            if redirect is not None:
                return redirect
            return await self._serve_dashboard_asset(
                url, "dashboard/index.html")

        # Unlike other static account pages, /signup is Worker-owned so its
        # GET response can mint the one-time HttpOnly claim required by the
        # account-creation endpoint. The .html implementation path remains
        # blocked below and the trailing-slash spelling redirects to this one
        # canonical URL without minting a second claim.
        if url.path == "/signup/":
            signup_target = _signup_page_target(url.query)
            return Response(
                "", status=308, headers={"location": signup_target})
        if url.path == "/signup":
            if method_name(request) == "GET":
                return await self._serve_signup_page(request, url)
            if method_name(request) == "HEAD":
                return Response("", status=200, headers={
                    "content-type": "text/html; charset=utf-8",
                    "cache-control": "no-store, max-age=0, must-revalidate",
                })
            # A visitor can land here with a POST they never authored. The bot
            # challenge that fronts this page replays the blocked navigation by
            # submitting a form to /signup?__cf_chl_f_tk=..., and once that
            # token is spent or expired the edge stops absorbing the replay and
            # passes the POST through. Answering a navigation with a JSON 405
            # paints {"error":"method_not_allowed"} over the signup form, so
            # bounce every other method back to the canonical page instead:
            # 303 makes the browser drop the body and re-issue a GET, which
            # mints a fresh signup claim exactly like a first visit.
            return Response("", status=303, headers={
                "location": _signup_page_target(url.query),
                "cache-control": "no-store, max-age=0, must-revalidate",
            })

        # Founder-only product dashboard. The handler answers None whenever
        # the route is unconfigured, the path carries extra segments, or the
        # view does not exist, so those cases share the ordinary 404 page and
        # the route stays undiscoverable.
        if (url.path == PRODUCT_ANALYSIS_PATH
                or url.path.startswith(PRODUCT_ANALYSIS_PATH + "/")):
            response = await product_analysis_handler(self.env, request)
            if response is not None:
                return response
            return await self._serve_not_found_page(url)

        # Worker-owned acquisition pages, matched before the repository
        # router so /admin and /transparency are never read as owners. The
        # dashboard is account-session + is_admin gated; the public page is
        # a 404 until PUBLIC_ACQUISITION_PAGE is switched on.
        # The hackathon administrator page: growth dashboard, campaign
        # configuration, judges, submission review, ambassadors and results.
        # Gated exactly like /admin/acquisition, live on every request.
        if url.path in ("/admin/hackathon", "/admin/hackathon/"):
            return await hackathon_admin_page_handler(self.env, request)
        # The admin theme switch shared by every admin page: a form post that
        # sets one cookie and sends the viewer back where they were.
        if url.path == "/admin/theme":
            return await admin_theme_handler(self.env, request)
        if url.path in ("/admin/acquisition", "/admin/acquisition/"):
            return await _attribution.admin_dashboard_handler(
                self.env, request)
        if url.path in (
                "/transparency/acquisition", "/transparency/acquisition/"):
            public_page = await _attribution.transparency_page_handler(
                self.env, request)
            if public_page is not None:
                return public_page
            return await self._serve_not_found_page(url)

        if url.path in ("/api", "/api/", "/developers", "/developers/"):
            return _api_metrics.landing_page_response(self.env)

        if url.path in (
                blog_feed.FEED_PATH, "/blog/feed.xml",
                "/rss.xml", "/feed.xml"):
            return await blog_rss_handler(self.env, request)

        external_owner = external_site_route(url.path)
        if external_owner and method_name(request) in ("GET", "HEAD"):
            origin_name = (
                "WORLD_ORIGIN" if external_owner == "world" else "WWW_ORIGIN"
            )
            origin = str(getattr(self.env, origin_name, "") or "").rstrip("/")
            current_origin = url.scheme + "://" + url.netloc
            mode, destination_origin = external_site_delivery(
                url.path, current_origin, origin,
                self._published_site_origins())
            if mode == "redirect":
                destination = destination_origin + url.path
                if url.query:
                    destination += "?" + url.query
                return Response("", status=308, headers={"location": destination})
            if mode == "proxy":
                return await self._serve_site_document(
                    request, url, destination_origin)
            if external_owner == "world":
                return await self._serve_world_asset(request, url)
            return await self._serve_site_document(request, url, "")

        if url.path in BLOCKED_STATIC_HTML_PATHS:
            return await self._serve_not_found_page(url)

        # Retired page URLs (the plural /referrals) 308 to their current path.
        legacy_page = legacy_page_redirect(url.path, url.query)
        if legacy_page:
            return Response("", status=308, headers={"location": legacy_page})

        profile_page = profile_page_redirect(url.path, url.query)
        if profile_page:
            return Response("", status=308, headers={"location": profile_page})

        # Familiar forge URLs (/pull/12, /-/merge_requests/12) 308 to the
        # canonical repository route (issue #508).
        repo_alias = repo_alias_redirect(url.path, url.query)
        if repo_alias:
            return Response("", status=308, headers={"location": repo_alias})

        if url.path in ("/health", "/api/mainnode"):
            return await self._health_control_response(request, url)

        # Build/version marker for deploy verification. deploy.sh stamps BUILD_REV
        # (the git rev being shipped) as a Worker var on every production deploy and
        # then polls this endpoint: if the live rev never flips to the one it just
        # built, the upload didn't actually take effect on this origin (e.g. it hit
        # the wrong Cloudflare account — the account drift that let stale code keep
        # serving while the deploy reported success), and the script aborts loudly
        # instead of reporting a phantom success.
        if url.path in ("/api/version", "/api/version/"):
            return self._version_control_response(request)

        if url.path in ("/api/metrics/endpoints", "/api/metrics/endpoints/"):
            return await _api_metrics.endpoint_catalog_handler(
                self.env, request)

        if url.path in ("/api/openapi.json", "/api/openapi.json/"):
            return await _api_metrics.openapi_handler(self.env, request)

        # Public Turnstile site key for the static /forgot-password page (the
        # Worker-rendered /signup page carries it inline). Never the secret.
        if url.path in ("/api/turnstile", "/api/turnstile/"):
            settings = _turnstile.config(self.env)
            return json_response(
                {"siteKey": settings["siteKey"],
                 "enabled": settings["mode"] != "unavailable"},
                cache_control="no-store, max-age=0, must-revalidate")

        if url.path in (
            "/api/bootstrap/readiness", "/api/bootstrap/readiness/"
        ):
            return await bootstrap_readiness_handler(self.env, request)

        if url.path in ("/api/world/context", "/api/world/context/"):
            return world_context_handler(request)

        if url.path in ("/api/world/ticket", "/api/world/ticket/"):
            return await world_ticket_handler(self.env, request)

        if url.path in ("/api/world/mesh", "/api/world/mesh/"):
            return await world_mesh_handler(self.env, request)

        if url.path in (
            "/api/world/neighbour-scene", "/api/world/neighbour-scene/"
        ):
            return world_neighbour_scene_handler(self.env, request)

        if url.path in (
            "/api/world/neighbour-icon", "/api/world/neighbour-icon/"
        ):
            return await world_neighbour_icon_handler(self.env, request)

        if url.path in ("/api/world/visa", "/api/world/visa/"):
            return await world_visa_handler(self.env, request)

        if url.path in ("/api/world/visa/session",
                        "/api/world/visa/session/"):
            return await world_visa_session_handler(self.env, request)

        if url.path in ("/api/world/visa/verify", "/api/world/visa/verify/"):
            return await world_visa_verify_handler(self.env, request)

        if url.path in ("/api/world/client", "/api/world/client/"):
            return await world_client_profile_handler(self.env, request)

        if url.path in ("/api/world/inactive", "/api/world/inactive/"):
            return await world_inactive_handler(self.env, request)

        if url.path in ("/api/world/wallet", "/api/world/wallet/"):
            return await world_wallet_swr_handler(self.env, request)

        if url.path in ("/api/world/visitors", "/api/world/visitors/"):
            return await world_visitors_handler(self.env, request)

        if url.path in (
                "/api/world/social-posts", "/api/world/social-posts/"):
            return await world_social_posts_handler(self.env, request)

        if url.path in (
                "/api/world/profile-social", "/api/world/profile-social/"):
            return await world_profile_social_handler(self.env, request)

        if url.path in (
                "/api/world/profile-feed", "/api/world/profile-feed/"):
            return await world_profile_activity_feed_handler(self.env, request)

        if url.path in (
                "/api/world/satellites", "/api/world/satellites/"):
            return await world_satellites_handler(self.env, request)

        if url.path in (
                "/api/world/admin/errors",
                "/api/world/admin/errors/"):
            return await world_admin_errors_handler(self.env, request)

        if url.path in (
                "/api/world/organizations",
                "/api/world/organizations/"):
            return await world_organizations_handler(self.env, request)
        if url.path in (
                "/api/world/instances",
                "/api/world/instances/"):
            return await world_relay_instances_handler(self.env, request)
        if url.path in (
                "/api/world/installs",
                "/api/world/installs/"):
            return await world_recent_installs_handler(self.env, request)

        if (url.path == "/api/world/fediverse"
                or url.path == "/api/world/fediverse/"
                or url.path.startswith("/api/world/fediverse/")):
            return await world_fediverse_directory_handler(
                self.env, request, url.path)

        if (url.path == "/api/world/media"
                or url.path == "/api/world/media/"
                or url.path.startswith("/api/world/media/")):
            return await world_media_handler(
                self.env, request, url.path)

        if (url.path == "/api/world/events"
                or url.path == "/api/world/events/"
                or url.path.startswith("/api/world/events/")):
            return await world_events_handler(
                self.env, request, url.path)

        if (url.path == "/api/world/community-ads"
                or url.path == "/api/world/community-ads/"
                or url.path.startswith("/api/world/community-ads/")):
            return await world_community_ads_handler(
                self.env, request, url.path)

        if (url.path == "/api/world/fediverse-mentions"
                or url.path == "/api/world/fediverse-mentions/"
                or url.path.startswith(
                    "/api/world/fediverse-mentions/")):
            return await world_fediverse_mentions_handler(
                self.env, request, url.path)

        if url.path in (
                "/api/world/moderation", "/api/world/moderation/"):
            return await world_moderation_handler(self.env, request)

        if url.path in (
                "/api/world/admin/nodes/delete",
                "/api/world/admin/nodes/delete/"):
            return await world_admin_delete_node_handler(self.env, request)

        if url.path in ("/api/genie/credential", "/api/genie/credential/"):
            return await genie_credential_handler(self.env, request)

        if (
            url.path == "/api/tasks"
            or url.path == "/api/tasks/"
            or url.path.startswith("/api/tasks/")
        ):
            return await organization_tasks_handler(
                self.env, request, url.path)

        if url.path in ("/mcp", "/mcp/"):
            return await remote_mcp_handler(self.env, request)

        # Unauthenticated by design: iOS posts the device attributes with no
        # cookie or credential, and nothing here is stored (ios_udid.py).
        if url.path == "/api/ios-udid" or url.path.startswith("/api/ios-udid/"):
            return await ios_udid_handler(self.env, request, url.path)

        if (
            url.path == "/api/world/office/marketing-tasks"
            or url.path == "/api/world/office/marketing-tasks/"
            or url.path.startswith("/api/world/office/marketing-tasks/")
        ):
            return await world_office_marketing_tasks_handler(
                self.env, request, url.path)

        if url.path in (
                "/api/world/build-board", "/api/world/build-board/"):
            return await world_build_board_handler(
                self.env, request, url.path)

        if url.path in (
                "/api/world/link-kiosk", "/api/world/link-kiosk/"):
            return await world_link_kiosk_handler(
                self.env, request, url.path)

        if url.path in (
                "/api/world/deploy-status", "/api/world/deploy-status/"):
            return await world_deploy_status_handler(self.env, request)

        if url.path in ("/api/world/qa", "/api/world/qa/"):
            return await world_qa_handler(self.env, request)

        if url.path in (
                "/api/world/floor-notes", "/api/world/floor-notes/"):
            return await world_floor_notes_handler(self.env, request)

        if url.path in (
                "/api/world/preferences", "/api/world/preferences/"):
            return await world_preferences_handler(self.env, request)

        if (
            url.path == "/api/world/store"
            or url.path == "/api/world/store/"
            or url.path.startswith("/api/world/store/")
        ):
            return await world_element_store_handler(
                self.env, request, url.path)

        if url.path in (
                "/api/world/office/attendance",
                "/api/world/office/attendance/"):
            return await office_attendance_handler(self.env, request)

        if url.path in (
                "/api/world/office/general/status",
                "/api/world/office/general/status/"):
            return await office_general_status_handler(self.env, request)

        if url.path in (
                "/api/world/office/general/entry",
                "/api/world/office/general/entry/"):
            return await office_general_entry_handler(self.env, request)

        if url.path in (
                "/api/world/office/floors",
                "/api/world/office/floors/"):
            return await office_floor_access_handler(self.env, request)

        if url.path in (
                "/api/world/office/general/access",
                "/api/world/office/general/access/"):
            return await office_general_access_handler(self.env, request)

        if url.path in (
                "/api/world/office/general/ws",
                "/api/world/office/general/ws/"):
            return await _office_general_socket_handler(self.env, request)

        if url.path == "/api/office/session":
            return await org_office_session_handler(self.env, request)

        if url.path == "/api/office/entry":
            return await org_office_entry_handler(self.env, request)

        if url.path == "/api/office/ws":
            return await org_office_socket_handler(self.env, request)

        office_channel_socket = OFFICE_CHANNEL_WS_RE.fullmatch(url.path)
        if office_channel_socket:
            return await _office_channel_socket_handler(
                self.env, request, office_channel_socket.group(1))

        if url.path in ("/api/world/online", "/api/world/online/"):
            if method_name(request) != "GET":
                return json_response(
                    {"error": "method_not_allowed"},
                    status=405,
                    extra_headers={"allow": "GET"},
                )
            world_id = self.env.FORKMESH_WORLD.idFromName("town-square-v1")
            try:
                return await self.env.FORKMESH_WORLD.get(world_id).fetch(
                    JsRequest.new(
                        str(request.url),
                        to_js({"method": "GET"}),
                    )
                )
            except Exception:
                return json_response(
                    {"ok": False, "online": 0, "error": "unavailable"},
                    status=503,
                    extra_headers=EXPECTED_DEGRADED_HEADERS,
                )

        if url.path in ("/api/status/ws", "/api/status/ws/"):
            if method_name(request) != "GET":
                return json_response(
                    {"error": "method_not_allowed"},
                    status=405,
                    extra_headers={"allow": "GET"},
                )
            if not world_websocket_origin_allowed(request, self.env):
                return json_response(
                    {"error": "origin_not_allowed"}, status=403)
            if url.query:
                return json_response({"error": "invalid_request"}, status=400)
            world_id = self.env.FORKMESH_WORLD.idFromName("town-square-v1")
            try:
                return await self.env.FORKMESH_WORLD.get(world_id).fetch(
                    await durable_object_request(request))
            except Exception:
                return json_response(
                    {"error": "unavailable"}, status=503,
                    extra_headers=EXPECTED_DEGRADED_HEADERS)

        if url.path in ("/api/world/ws", "/api/world/ws/"):
            if method_name(request) != "GET":
                return json_response(
                    {"error": "method_not_allowed"},
                    status=405,
                    extra_headers={"allow": "GET"},
                )
            query = parse_qs(url.query, keep_blank_values=False)
            mesh_visitor = None
            arrival = False
            if world_websocket_origin_allowed(request, self.env):
                visa_values = query.get("visa") or []
                if len(visa_values) == 1 and not query.get("ticket"):
                    arrival = True
                    mesh_visitor = await world_mesh_visitor(
                        self.env, request, visa_values[0], arrival=True)
                    if not mesh_visitor or mesh_visitor.get("error") \
                            or not mesh_visitor.get("claim"):
                        return json_response(
                            {"error": "invalid_world_session"},
                            status=401,
                            cache_control=(
                                "no-store, max-age=0, must-revalidate"),
                        )
            else:
                visa_values = query.get("visa") or []
                visitor_values = query.get("visitor") or []
                mesh_visitor = await world_mesh_visitor(
                    self.env, request,
                    "" if visitor_values
                    else visa_values[0] if len(visa_values) == 1 else "")
                if mesh_visitor and len(visitor_values) == 1:
                    claim = world_visitor_claim(
                        self.env, visitor_values[0], mesh_visitor["home"])
                    mesh_visitor = ({**mesh_visitor, "claim": claim}
                                    if claim else {"error": "invalid_visitor"})
                if not mesh_visitor:
                    return json_response(
                        {"error": "origin_not_allowed"}, status=403)
                if mesh_visitor.get("error"):
                    return json_response(
                        {"error": "invalid_world_session"},
                        status=401,
                        cache_control="no-store, max-age=0, must-revalidate",
                    )
            ticket_values = (
                [] if mesh_visitor else (query.get("ticket") or []))
            trusted_claim = (
                _world_ticket_decode(self.env, ticket_values[0])
                if len(ticket_values) == 1 else None
            )
            if mesh_visitor:
                trusted_claim = mesh_visitor.get("claim") or None
            if ticket_values and not trusted_claim:
                return json_response(
                    {"error": "invalid_world_session"},
                    status=401,
                    cache_control="no-store, max-age=0, must-revalidate",
                )
            if trusted_claim and not mesh_visitor:
                try:
                    account_bi, session_record = (
                        await _account_session_record(
                            self.env, request))
                    session_name = world_protocol.clean_display_name(
                        (session_record or {}).get("name"), "")
                    ticket_name = world_protocol.clean_display_name(
                        trusted_claim.get("name"), "")
                except Exception:
                    session_record = None
                    session_name = ""
                    ticket_name = ""
                if (
                    not session_record
                    or not session_name
                    or not hmac.compare_digest(
                        session_name.lower(), ticket_name.lower())
                ):
                    return json_response(
                        {"error": "invalid_world_session"},
                        status=401,
                        cache_control=(
                            "no-store, max-age=0, must-revalidate"),
                    )
                trusted_claim["accountBi"] = (
                    str(account_bi)
                    if re.fullmatch(r"[a-f0-9]{64}", str(account_bi or ""))
                    else "")
            moderation_tokens = await _world_moderation_tokens(
                self.env, request)
            active_block = await _world_active_manual_block(
                self.env, moderation_tokens)
            if active_block:
                return json_response(
                    {
                        "error": "world_temporarily_blocked",
                        "targetType": active_block["targetType"],
                        "expiresAt": active_block["expiresAt"],
                        "notice": (
                            "This is a temporary manual administrator action; "
                            "it is not an automated abuse or quarantine result."
                        ),
                    },
                    status=403,
                    cache_control="no-store, max-age=0, must-revalidate",
                )
            world_id = self.env.FORKMESH_WORLD.idFromName("town-square-v1")
            last_error = None
            for _attempt in range(2):
                world_object = self.env.FORKMESH_WORLD.get(world_id)
                try:
                    return await world_object.fetch(
                        await world_durable_object_request(
                            request, trusted_claim, moderation_tokens,
                            mesh_home=(mesh_visitor or {}).get("home", ""),
                            watch=bool(mesh_visitor) and not arrival and (
                                query.get("watch") or [""])[0] == "1"))
                except Exception as error:
                    last_error = error
            await log_durable_object_abort(
                self.env, request, url.path, last_error)
            return json_response(
                {"error": "unavailable"}, status=503,
                extra_headers=EXPECTED_DEGRADED_HEADERS)

        if url.path in ("/simulate-sentry-error", "/simulate-sentry-error/"):
            division_by_zero = 1 / 0
            return json_response({"ok": False, "value": division_by_zero})

        # Cached aggregate stats for the homepage/network page. Served before the
        # per-repo handlers so a burst of visitors collapses to one computation
        # per colo per TTL instead of a Durable Object fan-out per page view.
        if url.path in ("/api/network/overview", "/api/network/overview/"):
            return await network_overview(self.env)

        if url.path in ("/api/network/stats", "/api/network/stats/"):
            include_payouts = parse_qs(url.query).get("payouts", [""])[0] == "1"
            return await network_stats(
                self.env, include_payouts=include_payouts,
                refresh=_wants_balance_refresh(
                    request, scope="network-stats"))

        # 24-hour online-activity series for the /network/ graph.
        if url.path in ("/api/network/online-history", "/api/network/online-history/"):
            return await online_history(self.env)

        # Public ranking boards (node uptime + repos per owner) for /network/.
        if url.path in ("/api/network/leaderboards", "/api/network/leaderboards/"):
            return await network_leaderboards(self.env)
        # Members who published a public payout address, by on-chain balance.
        if url.path in ("/api/leaderboards/wallets",
                        "/api/leaderboards/wallets/"):
            return await wallet_leaderboard(
                self.env,
                refresh=_wants_balance_refresh(
                    request, scope="wallet-leaderboard"))
        if url.path in ("/api/leaderboards", "/api/leaderboards/"):
            return await leaderboards_overview(self.env)

        # Referral-program board (clicks + signups per share link) for the
        # /referrals page and the World's referral leaderboard sign.
        if url.path in ("/api/referrals/leaderboard", "/api/referrals/leaderboard/"):
            return await referral_leaderboard(self.env)

        # Inbound website board (visits per referring hostname) for the
        # /referrals page.
        if url.path in ("/api/referrals/sites", "/api/referrals/sites/"):
            return await site_referrer_leaderboard(self.env)

        if url.path in ("/api/ecosystem", "/api/ecosystem/"):
            return await ecosystem_api_handler(self.env, request)

        if url.path in ("/api/ecosystem/import", "/api/ecosystem/import/"):
            return await ecosystem_import_handler(self.env, request)

        # Announcement permalinks for one post: public to read, is_admin to
        # write, so a post can be marked as announced without a deploy.
        blog_links_prefix = "/api/blog/links/"
        if url.path.startswith(blog_links_prefix):
            return await blog_post_links_handler(
                self.env, request,
                unquote(url.path[len(blog_links_prefix):]).strip("/"))

        blog_metrics_prefix = "/api/blog/metrics/"
        if url.path.startswith(blog_metrics_prefix):
            slug = unquote(url.path[len(blog_metrics_prefix):]).strip("/")
            method = method_name(request)
            # POST is the static post page reporting its own view; GET reads
            # the aggregates back.
            if method == "POST":
                return await blog_post_view_beacon(self.env, request, slug)
            if method != "GET":
                return json_response(
                    {"error": "method_not_allowed"}, status=405,
                    extra_headers={"allow": "GET, POST"})
            return await blog_post_metrics(self.env, slug)

        # Rendered share-link card (og:image of the /r/<name> preview page).
        referral_card_match = REFERRAL_CARD_RE.match(url.path)
        if referral_card_match:
            return await referral_card_handler(
                self.env, request, unquote(referral_card_match.group(1)))

        # Query-free compact projection for the World's minute board. The edge
        # normally serves its sibling snapshot derived from the same full
        # build; this live route is the authoritative no-store fallback.
        if url.path in ("/api/status/world", "/api/status/world/"):
            return uncached_status_response(
                await cached_status_history(self.env, "world"))

        # 30-day per-system uptime history for the public /status page. Keep
        # the old ?view=world compatibility behavior, but first-party World
        # clients use the exact route above so edge snapshot admission applies.
        if url.path in ("/api/status", "/api/status/"):
            status_query = parse_qs(url.query)
            status_view = status_query.get("view", ["full"])[0]
            force_refresh = (
                status_query.get("fresh", [""])[0] == "browser-reload")
            return uncached_status_response(
                await cached_status_history(
                    self.env,
                    "world" if status_view == "world" else "full",
                    force_refresh=force_refresh))

        if url.path in (
                MIRROR_REPLICA_SOURCE_PATH,
                MIRROR_REPLICA_SOURCE_PATH + "/"):
            return await mirror_replica_source_handler(self.env, request)

        # Installer clone source: pick the currently-online forkmesh host with
        # the most retained uptime instead of baking one node id into install.sh.
        if url.path in ("/api/install-source", "/api/install-source/"):
            return await install_source(self.env)

        # Anonymous per-step diagnostics posted by install.sh (no auth, no IP).
        if url.path in ("/api/install-diag", "/api/install-diag/"):
            return await install_diag_handler(self.env, request)

        # Anonymous marketplace install counts: the Market surfaces read these
        # with GET, desktop clients report their own state with POST.
        if url.path in ("/api/market/installs", "/api/market/installs/"):
            return await package_installs_handler(self.env, request)

        if url.path in ("/api/market/packages", "/api/market/packages/"):
            import market_packages
            return await market_packages.handle(
                method_name(request), json_response)

        # Legacy anonymous Market rating aggregates (writes retired: 410).
        if url.path in ("/api/market/ratings", "/api/market/ratings/"):
            return await package_ratings_handler(self.env, request)

        # Account-based Market star ratings with required improvement
        # reviews, owner replies and moderation (docs/MARKETPLACE_RATINGS.md).
        if (url.path == "/api/market/reviews"
                or url.path.startswith("/api/market/reviews/")):
            return await marketplace_reviews.handle(
                _MarketplaceReviewsRuntime(self.env, request),
                url.path.rstrip("/") or url.path)

        # Community marketplace catalogs and their two-stage publication
        # workflow. Exact paths are parsed again by the isolated module.
        if (
            url.path in (
                "/api/market/catalog.json",
                "/api/market/catalog.json/",
                "/api/market/submissions",
                "/api/market/submissions/",
            )
            or url.path.startswith("/api/market/submissions/")
        ):
            return await marketplace_governance.handle(
                _MarketplaceGovernanceRuntime(self.env, request), url.path)

        if url.path.startswith("/api/start/"):
            import runner_leases
            import instance_launch
            runner_leases._bind_runtime(globals())
            instance_launch._bind_runtime(globals())
            return await instance_launch.route(self.env, request, url.path)

        # Anonymous docs/page feedback: like/dislike clicks with optional text.
        if url.path in ("/api/feedback", "/api/feedback/"):
            return await feedback_handler(self.env, request)

        if url.path in ("/api/client-errors", "/api/client-errors/"):
            return await client_error_handler(self.env, request)

        # Acquisition attribution (docs/attribution.md): the www Worker's
        # page-view relay, the copy-clone-URL beacon, self-serve erasure,
        # and the admin-only report/list/discovery-call endpoints.
        if url.path in (
                "/api/attribution/capture", "/api/attribution/capture/"):
            return await _attribution.relay_capture_handler(self.env, request)
        if url.path in ("/api/not-found", "/api/not-found/"):
            return await _not_found_log.relay_handler(self.env, request)
        if url.path in ("/api/attribution/events", "/api/attribution/events/"):
            return await _attribution.browser_event_handler(self.env, request)
        if url.path in (
                "/api/attribution/session", "/api/attribution/session/"):
            return await _attribution.delete_session_handler(
                self.env, request, url)
        if url.path in (
                "/api/admin/acquisition/report",
                "/api/admin/acquisition/report/"):
            return await _attribution.admin_report_handler(self.env, request)
        if url.path in (
                "/api/admin/acquisition/sessions",
                "/api/admin/acquisition/sessions/"):
            return await _attribution.admin_list_handler(
                self.env, request, "sessions")
        if url.path in (
                "/api/admin/acquisition/events",
                "/api/admin/acquisition/events/"):
            return await _attribution.admin_list_handler(
                self.env, request, "events")
        if url.path in (
                "/api/admin/acquisition/discovery-call",
                "/api/admin/acquisition/discovery-call/"):
            return await _attribution.admin_discovery_call_handler(
                self.env, request)

        # Desktop-app failures (warning dialogs, error notices) from a signed
        # node, so an error nobody was watching still pings the administrators.
        if url.path in ("/api/desktop-errors", "/api/desktop-errors/"):
            return await desktop_error_handler(self.env, request)

        if url.path in ("/advisories", "/advisories/", "/advisories.json",
                        "/security-researchers", "/security-researchers/") \
                or url.path.startswith(("/advisories/",
                                        "/api/security-advisories/")):
            import security_advisories
            security_advisories._bind_runtime(globals())
            return await security_advisories.public_route(
                self.env, request, url.path)

        # Private vulnerability reports — stored encrypted, emailed to security@.
        if url.path in ("/api/security/report", "/api/security/report/"):
            return await security_report_handler(self.env, request)

        if url.path in ("/api/security/roles", "/api/security/roles/"):
            return await security_roles_handler(self.env, request)

        if url.path in ("/api/security/audit", "/api/security/audit/"):
            return await security_audit_handler(self.env, request)

        if url.path in (
                "/api/security-scans/queue",
                "/api/security-scans/queue/"):
            return await repository_security_scan_queue_handler(
                self.env, request)

        if url.path in (
                "/api/security/owner-keys",
                "/api/security/owner-keys/"):
            return await owner_encryption_keys_handler(self.env, request)

        if url.path in (
                "/api/rewards/signing-jobs",
                "/api/rewards/signing-jobs/"):
            return await reward_signing_jobs_handler(self.env, request)

        if url.path in (
                "/api/rewards/estimates",
                "/api/rewards/estimates/"):
            return await contributor_reward_estimates_handler(
                self.env, request)

        # A reward whose recipient published no payout address cannot be paid
        # by anyone. The owner's one move is to ask them for one.
        if url.path in (
                "/api/rewards/wallet-reminder",
                "/api/rewards/wallet-reminder/"):
            return await contributor_reward_wallet_reminder_handler(
                self.env, request)

        if url.path in ("/api/ledger", "/api/ledger/"):
            return await ledger_handler(self.env, request)

        if url.path == "/api/ledger/expenses":
            return await operating_expense_record_handler(self.env, request)

        # Paid microjobs: the public job catalog, the caller's own claims, and
        # the verified claim submission. Payouts leave here only as the same
        # public unsigned plan the owner's desktop already signs locally.
        if MICROJOBS_RE.match(url.path):
            return await microjobs_handler(self.env, request, url.path)
        if AFFILIATES_RE.match(url.path):
            return await affiliates_handler(self.env, request, url.path)
        if PLUGIN_RACE_RE.match(url.path):
            return await plugin_race_handler(self.env, request, url.path)

        # The hackathon growth layer: campaign, join, the participant's own
        # standing, share presses, public boards, squad pages and unfurl
        # cards. Every public read is edge-cached and bounded; nothing here
        # decides a referral, which stays with referral_attributions.
        pitch_deck_events = PITCH_DECK_EVENTS_RE.match(url.path)
        if pitch_deck_events:
            return await pitch_deck_events_handler(
                self.env, request, pitch_deck_events.group(1))
        if HACKATHON_API_RE.match(url.path):
            return await hackathon_api_handler(
                self.env, request, url.path)
        if CHALLENGES_API_RE.match(url.path):
            return await challenges_api_handler(
                self.env, request, url.path)
        if WORKFLOWS_API_RE.match(url.path):
            return await workflows_api_handler(
                self.env, request, url.path)
        if url.path in ("/api/admin/hackathon", "/api/admin/hackathon/"):
            return await hackathon_admin_handler(self.env, request)
        if url.path in (
                "/api/admin/phone/database", "/api/admin/phone/database/"):
            return await _admin_database_phone_handler(self.env, request)

        if url.path in (
                "/api/rewards/pending",
                "/api/rewards/pending/"):
            return await pending_rewards_handler(self.env, request)

        if url.path in (
                "/api/rewards/pending-awards",
                "/api/rewards/pending-awards/"):
            return await pending_reward_award_handler(self.env, request)

        if url.path in (
                "/api/rewards/contributions",
                "/api/rewards/contributions/"):
            return await reward_contributions_handler(self.env, request)

        if url.path in ("/api/rewards/pool", "/api/rewards/pool/"):
            if method_name(request) != "GET":
                return json_response(
                    {"error": "method_not_allowed"}, status=405)
            return await _account_central_fund(self.env, request)

        # Searchable public index (issue #478): ranked, paginated reads over
        # the validated anonymous catalog snapshot. Never per-viewer.
        if url.path in (
                "/api/repositories/search", "/api/repositories/search/"):
            return await repository_index_search_handler(self.env, request)

        # Persistent data lives in D1, not Durable Objects.
        if url.path in ("/api/repositories", "/api/repositories/"):
            return await catalog_handler(self.env, request)

        # Empty repositories created from the browser, plus the Basic
        # credential a stock git pushes them with (issue #676). Separate from
        # /api/repositories because that route accepts only a node-signed
        # record, which a web-only account has no key to produce.
        if url.path in ("/api/git/tag-moves", "/api/git/tag-moves/"):
            return await core_push_tag_moves_handler(self.env, request)
        if url.path in ("/api/git/branch-deletes", "/api/git/branch-deletes/"):
            return await core_push_branch_deletes_handler(self.env, request)
        if url.path in ("/api/git/branch-trash", "/api/git/branch-trash/"):
            return await core_push_branch_trash_handler(self.env, request)

        if (url.path == "/api/hosted-repositories"
                or url.path.startswith("/api/hosted-repositories/")):
            return await hosted_repository_handler(
                self.env, request, url.path)

        # A managed mirror's signed drain/ack for website-created fork jobs.
        # Browser creation itself lives on POST /api/repo/<owner>/<repo>/forks.
        if url.path in ("/api/repository-forks", "/api/repository-forks/"):
            return await repository_fork_jobs_handler(self.env, request)

        # GitHub/GitLab metadata imports and non-mirrored stubs are deliberately
        # separate from /api/repositories: only the latter represents a mirror
        # that ForkMesh can actually route or clone.
        if (url.path == "/api/repository-imports"
                or url.path == "/api/repository-imports/"
                or url.path.startswith("/api/repository-imports/")):
            return await repository_imports_handler(
                self.env, request, url.path)

        # Preservation-first crisis plans and their scenario catalog
        # (www/docs/crisis-preservation.md).
        if url.path in ("/api/preservation/crisis-plan",
                        "/api/preservation/crisis-plan/"):
            return await crisis_preservation_plan_handler(self.env, request)

        if url.path in ("/api/notifications", "/api/notifications/"):
            return await notifications_handler(self.env, request)

        if url.path in ("/api/me/recent-pushes", "/api/me/recent-pushes/"):
            return await recent_pushes_handler(self.env, request)

        # A countdown or alarm ran out on the desktop's clock; the ping it
        # raises is what reaches the phone over APNs.
        if url.path in ("/api/notifications/timer",
                        "/api/notifications/timer/"):
            return await timer_ping_handler(self.env, request)

        if url.path in ("/api/notifications/desktop",
                        "/api/notifications/desktop/"):
            return await desktop_ping_handler(self.env, request)

        if (url.path == "/api/calendar" or url.path == "/api/calendar/"
                or url.path.startswith("/api/calendar/")):
            return await calendar_handler(self.env, request, url.path)

        if (url.path == "/api/plugins" or url.path == "/api/plugins/"
                or url.path.startswith("/api/plugins/")):
            return await plugin_lifecycle_handler(self.env, request, url.path)

        plugin_api = plugin_data.api_plugin_for(url.path)
        if plugin_api:
            return await plugin_api_handler(
                self.env, request, url.path, plugin_api)

        if url.path.startswith("/api/actions/job/"):
            import actions_job_api
            actions_job_api._bind_runtime(globals())
            return await actions_job_api.route(self.env, request, url.path)

        # --- Phase 5 runner tier: /api/runners/* (runner_leases.py) ---
        # (/api/runners itself lists runners for the web sign-in page.)
        if url.path.startswith("/api/runners/") or url.path == "/api/runners":
            import runner_leases
            runner_leases._bind_runtime(globals())
            return await runner_leases.route(self.env, request, url.path)
        # --- end Phase 5 runner tier ---

        if url.path == "/api/docs-book" or url.path.startswith(
                "/api/docs-book/"):
            import docs_book
            docs_book._bind_runtime(globals())
            return await docs_book.route(self.env, request, url.path)

        if (url.path == "/api/notes" or url.path == "/api/notes/"
                or url.path.startswith("/api/notes/")):
            return await notes_handler(self.env, request, url.path)

        # Peer mirror requests (issue #385): create/accept/reject a request
        # asking another node to mirror a repo. Session-gated like the inbox.
        if url.path in ("/api/mirror-requests", "/api/mirror-requests/"):
            return await mirror_requests_handler(self.env, request)

        # Consolidated lightweight status digest (version + profile/notification
        # change tokens) so the client polls once instead of re-fetching the
        # full profile and notification list every tick.
        if url.path in ("/api/poll", "/api/poll/"):
            return await poll_handler(self.env, request)

        # Shared room-chat key, derived server-side from DATA_KEY and handed only
        # to authenticated clients — replaces the old public app-wide constant.
        if url.path in ("/api/chat/room-key", "/api/chat/room-key/"):
            return await chat_room_key_handler(self.env, request)

        # Public chat-activity counters (message + user counts) that drive the
        # unread badge on the shared site header's chat icon.
        if url.path in ("/api/chat/activity", "/api/chat/activity/"):
            return await chat_activity_handler(self.env, request)

        # Public AI-agent guestbook. The self-describing GET advertises one
        # narrow query-string GET capability; accepted notes become ordinary
        # encrypted messages in public World #general, never arbitrary room
        # writes.
        if url.path in ("/api/chat/agent-guestbook",
                        "/api/chat/agent-guestbook/"):
            return await agent_guestbook_handler(self.env, request)
        if url.path in ("/api/agent-notes", "/api/agent-notes/"):
            return await agent_notes_handler(self.env, request)

        # Private channel API and socket admission are checked before the
        # generic repository-room router. Authorization and current key-version
        # validation therefore happen before any Durable Object is selected.
        chat_channel_socket = CHAT_CHANNEL_WS_RE.match(url.path)
        if chat_channel_socket:
            return await _chat_channel_socket_handler(
                self.env, request, chat_channel_socket.group(1))
        if (
            CHAT_CHANNELS_RE.match(url.path)
            or CHAT_CHANNEL_RE.match(url.path)
            or CHAT_CHANNEL_MEMBERS_RE.match(url.path)
            or CHAT_CHANNEL_ROOM_ACCESS_RE.match(url.path)
            or CHAT_CHANNEL_HISTORY_RE.match(url.path)
            or CHAT_CHANNEL_WEBHOOKS_RE.match(url.path)
            or CHAT_CHANNEL_WEBHOOK_RE.match(url.path)
            or CHAT_WEBHOOK_DELIVERY_RE.match(url.path)
            or CHAT_CHANNEL_INVITE_RE.match(url.path)
            or CHAT_INVITE_RE.match(url.path)
            or CHAT_CHANNEL_PENDING_RE.match(url.path)
        ):
            return await chat_channels_api.handle(
                _ChatChannelsRuntime(self.env, request), url.path)

        # Direct messages never inherit platform-administrator channel access.
        # Both HTTP room access and WebSocket admission check participant rows.
        chat_direct_socket = CHAT_DIRECT_MESSAGE_WS_RE.match(url.path)
        if chat_direct_socket:
            return await _chat_direct_socket_handler(
                self.env, request, chat_direct_socket.group(1))
        if chat_direct_media_api.matches(url.path):
            return await chat_direct_media_api.handle(
                _ChatDirectMessagesRuntime(self.env, request), url.path)
        if (
            CHAT_DIRECT_MESSAGES_RE.match(url.path)
            or CHAT_DIRECT_MESSAGE_USERS_RE.match(url.path)
            or CHAT_DIRECT_MESSAGE_ROOM_ACCESS_RE.match(url.path)
            or CHAT_DIRECT_MESSAGE_READ_RE.match(url.path)
            or CHAT_DIRECT_MESSAGE_PENDING_RE.match(url.path)
            or CHAT_DIRECT_MESSAGE_DELIVERED_RE.match(url.path)
        ):
            return await chat_direct_messages_api.handle(
                _ChatDirectMessagesRuntime(self.env, request), url.path)
        if CHAT_READS_RE.match(url.path):
            return await chat_reads_api.handle(
                _ChatDirectMessagesRuntime(self.env, request), url.path)

        # Chat-triggered Cloudflare AI interface. Clients forward explicit
        # "forkbot ..." mentions here; the Worker queues compatible issue-inbox
        # records and returns a bot reply for the encrypted room.
        if url.path in ("/api/sync", "/api/sync/"):
            return await sync_handler(self.env, request)

        # Live push channel that makes /api/sync event-driven: an owner node
        # holds one WebSocket here and each web submission triggers an instant
        # payload-free event frame (see notify_repo_host / ForkMeshNodes).
        if url.path in ("/api/nodes/events", "/api/nodes/events/"):
            return await node_events_handler(self.env, request)

        # The browser's key to that same channel: a session traded for a
        # 60s account-scoped ticket, because a WebSocket upgrade carries no
        # Authorization header. Matched ahead of accounts_handler.
        if url.path in ("/api/accounts/event-ticket",
                        "/api/accounts/event-ticket/"):
            return await account_event_ticket_handler(self.env, request)

        if url.path in ("/api/accounts/register-device",
                        "/api/accounts/register-device/"):
            return await register_device_handler(self.env, request)

        # The phone's proof that the registration above goes anywhere: one
        # generic alert to the caller's own devices, with Apple's answer per
        # device (see push_test_handler). Session-bound like registration.
        if url.path in ("/api/accounts/push-test",
                        "/api/accounts/push-test/"):
            return await push_test_handler(self.env, request)

        # What the phone reads back after one of those alerts lands: the
        # newest unread notifications rendered for a lock screen, so the
        # notification service extension can add the actor's portrait and the
        # unread badge before the banner is drawn (see push_alert_handler).
        # Same gate as reading the inbox.
        if url.path in ("/api/push/alert", "/api/push/alert/"):
            return await push_alert_handler(self.env, request)

        if url.path in ("/api/push/mesh-wake", "/api/push/mesh-wake/"):
            return await mesh_wake_mint_handler(self.env, request)

        if url.path in ("/api/push/mesh", "/api/push/mesh/"):
            return await mesh_wake_handler(self.env, request)

        # In-app bug reports from the mobile client's report control. Writes
        # are deliberately reachable without a session: a crash in the
        # login flow is exactly the report an authenticated-only endpoint
        # cannot receive. Matched before the generic /api/accounts/ prefix so
        # an anonymous submission never enters the account router. The inbox
        # lists them back to platform administrators only.
        if url.path in ("/api/bug-reports", "/api/bug-reports/"):
            return await bug_reports_handler(self.env, request)
        if url.path in ("/api/bug-reports/inbox", "/api/bug-reports/inbox/",
                        "/api/bug-reports/inbox/attachment",
                        "/api/bug-reports/inbox/attachment/"):
            return await bug_reports_inbox_handler(self.env, request)

        if url.path in ("/api/forkbot/chat", "/api/forkbot/chat/"):
            return await forkbot_chat_handler(self.env, request)

        # Workers AI models a chat client may aim its ForkBot prompt at.
        if url.path in ("/api/forkbot/models", "/api/forkbot/models/"):
            return await forkbot_models_handler(self.env, request)

        # One prompt answered by a picked Workers AI model (desktop composer).
        if url.path in ("/api/ai/ask", "/api/ai/ask/"):
            return await ai_ask_handler(self.env, request)

        # Optional semantic branch/worktree naming for desktop agent prompts.
        if url.path in ("/api/ai/workspace-name",
                        "/api/ai/workspace-name/"):
            return await ai_workspace_name_handler(self.env, request)

        # One tool-use turn for the desktop's Workers AI coding agent.
        if url.path in ("/api/ai/agent", "/api/ai/agent/"):
            return await ai_agent_handler(self.env, request)

        # Self-serve plan surface (issue #488): usage meters + trial register.
        plans_match = PLAN_SERVICES_RE.match(url.path)
        if plans_match:
            return await plan_services_handler(
                self.env, request, plans_match.group(1))

        if url.path == "/api/apps" or url.path.startswith("/api/apps/"):
            return await apps_handler(self.env, request, url.path)
        if url.path in ("/oauth/token", "/oauth/token/"):
            return await apps_oauth_token_handler(self.env, request)
        if url.path in ("/oauth/revoke", "/oauth/revoke/"):
            return await apps_oauth_revoke_handler(self.env, request)
        if url.path in ("/oauth/authorize", "/oauth/authorize/"):
            return await self._serve_dashboard_asset(
                url, "oauth-authorize.html")

        # ForkMesh Ads: member-written, human-reviewed contextual text ads.
        if AD_NETWORK_RE.match(url.path):
            return await ad_network_handler(self.env, request, url.path)

        if PATREON_OAUTH_CALLBACK_RE.match(url.path):
            return await patreon_oauth_callback_handler(self.env, request)
        if PATREON_WEBHOOK_RE.match(url.path):
            return await patreon_webhook_handler(self.env, request)

        # All /api/accounts/* paths (reserve, profile, follow, login, lookup)
        # are dispatched by accounts_handler.
        # Solana payout relay mesh (canonical /api/relay-mesh/*, legacy
        # /api/federation/* alias) — unrelated to ActivityPub, which is /ap/*.
        if (url.path.startswith("/api/relay-mesh/")
                or url.path.startswith("/api/federation/")):
            return await federation_handler(self.env, request)

        if url.path.startswith("/api/accounts/"):
            return await accounts_handler(self.env, request)

        # Signed source-of-truth admission override. Dispatched here, not from
        # accounts_handler: that handler is only reached for /api/accounts/*,
        # so a route registered inside it can never run.
        if (url.path == "/api/mirrors/admission-override"
                and method_name(request) == "POST"):
            return await _admin_set_admission_override(self.env, request)

        # --- Organizations + teams (issue #388) --------------------------
        if url.path == "/api/marketing-email/unsubscribe":
            return await marketing_email_unsubscribe_handler(
                self.env, request)
        mail_tracking_match = MAIL_TRACKING_RE.match(url.path)
        if mail_tracking_match:
            return await mail_tracking_handler(
                self.env, request, mail_tracking_match.group(1),
                mail_tracking_match.group(2))
        mail_footer_match = MAIL_FOOTER_IMAGE_RE.match(url.path)
        if mail_footer_match:
            return await mail_footer_image_handler(
                self.env, request, mail_footer_match.group(1))
        if MAIL_FOOTER_RE.match(url.path):
            return await mail_footer_handler(self.env, request)
        if MAILTRAP_WEBHOOK_RE.match(url.path):
            return await mailtrap_webhook_handler(self.env, request)
        if BOT_SESSION_RE.match(url.path):
            return await bot_session_handler(self.env, request)
        if ORGS_RE.match(url.path):
            return await orgs_handler(self.env, request)
        if DISCORD_OAUTH_CALLBACK_RE.match(url.path):
            return await organization_discord_oauth_callback_handler(
                self.env, request)
        org_action_secrets_match = ORG_ACTION_SECRETS_RE.match(url.path)
        if org_action_secrets_match:
            import actions_secrets
            actions_secrets._bind_runtime(globals())
            return await actions_secrets.org_handler(
                self.env, request, org_action_secrets_match.group(1),
                org_action_secrets_match.group(2) or "")

        org_bot_tokens_match = ORG_BOT_TOKENS_RE.match(url.path)
        if org_bot_tokens_match:
            org = safe_segment(org_bot_tokens_match.group(1))
            if not org:
                return json_response({"error": "not_found"}, status=404)
            return await org_bot_tokens_handler(
                self.env, request, org)
        org_discord_match = ORG_DISCORD_RE.match(url.path)
        if org_discord_match:
            org = safe_segment(org_discord_match.group(1))
            action = str(org_discord_match.group(2) or "")
            if not org or action not in {"", "messages", "oauth/start"}:
                return json_response({"error": "not_found"}, status=404)
            return await organization_discord_handler(
                self.env, request, org, action)
        org_members_match = ORG_MEMBERS_RE.match(url.path)
        if org_members_match:
            org = safe_segment(org_members_match.group(1))
            if not org:
                return json_response({"error": "not_found"}, status=404)
            response = await org_members_handler(self.env, request, org)
            await _org_office_resync(self.env, request, org, response)
            return response
        org_team_members_match = ORG_TEAM_MEMBERS_RE.match(url.path)
        if org_team_members_match:
            org = safe_segment(org_team_members_match.group(1))
            team = safe_segment(org_team_members_match.group(2))
            if not org or not team:
                return json_response({"error": "not_found"}, status=404)
            response = await org_team_members_handler(
                self.env, request, org, team)
            await _org_office_resync(self.env, request, org, response)
            return response
        org_teams_match = ORG_TEAMS_RE.match(url.path)
        if org_teams_match:
            org = safe_segment(org_teams_match.group(1))
            if not org:
                return json_response({"error": "not_found"}, status=404)
            response = await org_teams_handler(self.env, request, org)
            await _org_office_resync(self.env, request, org, response)
            return response
        org_repos_match = ORG_REPOS_RE.match(url.path)
        if org_repos_match:
            org = safe_segment(org_repos_match.group(1))
            if not org:
                return json_response({"error": "not_found"}, status=404)
            return await org_repos_handler(self.env, request, org)
        org_succession_match = ORG_SUCCESSION_RE.match(url.path)
        if org_succession_match:
            org = safe_segment(org_succession_match.group(1))
            action = safe_segment(
                org_succession_match.group(2) or "", 40)
            if not org or (
                org_succession_match.group(2) and not action
            ):
                return json_response({"error": "not_found"}, status=404)
            return await organization_succession_handler(
                self.env, request, org, action)
        org_pull_rules_match = ORG_PULL_RULES_RE.match(url.path)
        if org_pull_rules_match:
            org = safe_segment(org_pull_rules_match.group(1))
            if not org:
                return json_response({"error": "not_found"}, status=404)
            return await org_pull_rules_handler(self.env, request, org)
        org_fediverse_match = ORG_FEDIVERSE_RE.match(url.path)
        if org_fediverse_match:
            org = safe_segment(org_fediverse_match.group(1))
            if not org:
                return json_response({"error": "not_found"}, status=404)
            return await org_fediverse_handler(
                self.env, request, org)
        org_match = ORG_RE.match(url.path)
        if org_match:
            org = safe_segment(org_match.group(1))
            if not org:
                return json_response({"error": "not_found"}, status=404)
            return await org_handler(self.env, request, org)

        # --- Achievement badges (adhoc #370) ------------------------------
        if BADGES_RE.match(url.path):
            return await badges_catalog_handler(self.env, request)
        badge_account_match = BADGE_ACCOUNT_RE.match(url.path)
        if badge_account_match:
            account = safe_segment(badge_account_match.group(1))
            if not account:
                return json_response({"error": "not_found"}, status=404)
            return await account_badges_handler(self.env, request, account)

        # --- ActivityPub federation (fediverse) --------------------------
        # WebFinger/NodeInfo discovery plus the /ap/* actor, inbox, object
        # and collection endpoints. Kept clear of /api/federation/*, which is
        # the (unrelated) Solana payout relay mesh.
        if url.path == "/.well-known/webfinger":
            return await ap_webfinger_handler(self.env, request)
        if url.path == "/.well-known/host-meta":
            return await ap_hostmeta_handler(self.env, request)
        if url.path == "/.well-known/nodeinfo":
            return await ap_nodeinfo_handler(self.env, request, True)
        if url.path == "/nodeinfo/2.1":
            return await ap_nodeinfo_handler(self.env, request, False)
        if url.path in ("/ap/actor", "/ap/actor/"):
            return await _ap_actor_doc_response(
                self.env, request, AP_ACTOR_INSTANCE, AP_INSTANCE_HANDLE)
        if url.path in ("/ap/inbox", "/ap/actor/inbox"):
            return await ap_inbox_handler(self.env, request)
        ap_user_match = AP_USER_RE.match(url.path)
        if ap_user_match:
            return await _ap_actor_doc_response(
                self.env, request, AP_ACTOR_USER,
                ap_user_match.group(1).lower())
        ap_user_sub = AP_USER_SUB_RE.match(url.path)
        if ap_user_sub:
            if ap_user_sub.group(2) == "inbox":
                return await ap_inbox_handler(self.env, request)
            return await ap_collection_handler(
                self.env, request, AP_ACTOR_USER,
                ap_user_sub.group(1).lower(), ap_user_sub.group(2))
        ap_repo_match = AP_REPO_RE.match(url.path)
        if ap_repo_match:
            ap_owner = safe_segment(ap_repo_match.group(1))
            ap_repo_name = safe_segment(ap_repo_match.group(2))
            if not ap_owner or not ap_repo_name:
                return json_response({"error": "not_found"}, status=404)
            return await _ap_actor_doc_response(
                self.env, request, AP_ACTOR_REPO,
                ap.repo_handle(ap_owner.lower(), ap_repo_name.lower()))
        ap_repo_sub = AP_REPO_SUB_RE.match(url.path)
        if ap_repo_sub:
            ap_owner = safe_segment(ap_repo_sub.group(1))
            ap_repo_name = safe_segment(ap_repo_sub.group(2))
            if not ap_owner or not ap_repo_name:
                return json_response({"error": "not_found"}, status=404)
            if ap_repo_sub.group(3) == "inbox":
                return await ap_inbox_handler(self.env, request)
            return await ap_collection_handler(
                self.env, request, AP_ACTOR_REPO,
                ap.repo_handle(ap_owner.lower(), ap_repo_name.lower()),
                ap_repo_sub.group(3))
        ap_object_media_match = AP_OBJECT_MEDIA_RE.match(url.path)
        if ap_object_media_match:
            return await ap_object_media_handler(
                self.env, request, ap_object_media_match.group(1),
                ap_object_media_match.group(2))
        ap_object_match = AP_OBJECT_RE.match(url.path)
        if ap_object_match:
            return await ap_object_handler(
                self.env, request, ap_object_match.group(1))

        # Match the profile route on the percent-decoded, case-folded path:
        # links pasted from address bars / other apps often arrive as /%40name
        # (an encoded @) or /@Name, and both used to fall through to the 404
        # page instead of the profile.
        public_profile = re.match(
            r"^/@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)(/repositories)?/?$",
            unquote(url.path).lower())
        if public_profile:
            if (str(url.hostname or "").lower() == "app.forkmesh.com"
                    and request.headers.get(
                        "x-forkmesh-public-profile-proxy")
                    != "forkmesh.com"):
                return Response("", status=308, headers={
                    "location": (
                        "https://forkmesh.com/@" + quote(
                            public_profile.group(1))
                        + ("/repositories"
                           if public_profile.group(2) else "")
                        + (("?" + url.query) if url.query else "")),
                    "cache-control": "public, max-age=3600",
                })
            # Fediverse clients resolve profile URLs with an ActivityPub
            # Accept header; hand them the actor document instead of HTML.
            if _wants_activity_json(request.headers.get("accept") or ""):
                return await _ap_actor_doc_response(
                    self.env, request, AP_ACTOR_USER,
                    public_profile.group(1))
            return await self._serve_profile_page(
                url, public_profile.group(1),
                repositories=bool(public_profile.group(2)))

        # /@owner.repo — a repo actor's fediverse profile page (a feed of its
        # federated posts). A user handle never contains a dot, so a dotted
        # /@handle is unambiguously a repo actor (see ap.split_handle). This is
        # the repo actor's `url`, so fediverse clients that resolve it with an
        # ActivityPub Accept header get the actor document instead of HTML.
        fedi_repo = re.match(r"^/@([^/]+)/?$", unquote(url.path))
        if fedi_repo:
            parsed = ap.split_handle(fedi_repo.group(1).lower())
            if parsed and parsed[0] == "repo":
                if (str(url.hostname or "").lower() == "app.forkmesh.com"
                        and request.headers.get(
                            "x-forkmesh-public-profile-proxy")
                        != "forkmesh.com"):
                    return Response("", status=308, headers={
                        "location": (
                            "https://forkmesh.com/@" + quote(
                                fedi_repo.group(1).lower())
                            + (("?" + url.query) if url.query else "")),
                        "cache-control": "public, max-age=3600",
                    })
                _, ph_owner, ph_repo = parsed
                if _wants_activity_json(request.headers.get("accept") or ""):
                    return await _ap_actor_doc_response(
                        self.env, request, AP_ACTOR_REPO,
                        ap.repo_handle(ph_owner, ph_repo))
                return await self._serve_repo_profile_page(
                    url, ph_owner, ph_repo)

        issues_match = REPO_ISSUES_RE.match(url.path)
        if issues_match:
            owner = safe_segment(issues_match.group(1))
            repo = safe_segment(issues_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            if (await _repo_is_private(self.env, owner, repo)
                    and not (method_name(request) in ("GET", "DELETE")
                             and await _authorize_owner(
                                 self.env, request, owner))):
                return json_response({"error": "not_found"}, status=404)
            return await issues_handler(self.env, request, owner, repo)

        pull_rules_match = REPO_PULL_RULES_RE.match(url.path)
        if pull_rules_match:
            owner = safe_segment(pull_rules_match.group(1))
            repo = safe_segment(pull_rules_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_pull_rules_handler(
                self.env, request, owner, repo,
                int(pull_rules_match.group(3)))

        for pull_mutation, pull_mutation_pattern in (
            ("merge", REPO_PULL_MERGE_RE),
            ("update", REPO_PULL_UPDATE_RE),
        ):
            pull_merge_match = pull_mutation_pattern.match(url.path)
            if not pull_merge_match:
                continue
            owner = safe_segment(pull_merge_match.group(1))
            repo = safe_segment(pull_merge_match.group(2))
            try:
                pull_number = int(pull_merge_match.group(3))
            except (TypeError, ValueError, OverflowError):
                pull_number = 0
            if not owner or not repo or pull_number <= 0:
                return json_response({"error": "not_found"}, status=404)
            return await repository_pull_merge_handler(
                self.env, request, owner, repo, pull_number, pull_mutation)

        branch_ops_match = REPO_BRANCH_OPS_RE.match(url.path)
        if branch_ops_match:
            owner = safe_segment(branch_ops_match.group(1))
            repo = safe_segment(branch_ops_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repository_branch_ops_handler(
                self.env, request, owner, repo)

        branch_delete_match = REPO_BRANCH_DELETE_RE.match(url.path)
        if branch_delete_match:
            owner = safe_segment(branch_delete_match.group(1))
            repo = safe_segment(branch_delete_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repository_branch_delete_handler(
                self.env, request, owner, repo)

        branch_op_results_match = REPO_BRANCH_OP_RESULTS_RE.match(url.path)
        if branch_op_results_match:
            owner = safe_segment(branch_op_results_match.group(1))
            repo = safe_segment(branch_op_results_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repository_branch_op_results_handler(
                self.env, request, owner, repo)

        action_log_match = REPO_ACTION_RUN_LOG_RE.match(url.path)
        if action_log_match:
            owner = safe_segment(action_log_match.group(1))
            repo = safe_segment(action_log_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repository_actions_run_log_handler(
                self.env, request, owner, repo,
                int(action_log_match.group(3)))

        action_secrets_match = REPO_ACTION_SECRETS_RE.match(url.path)
        if action_secrets_match:
            owner = safe_segment(action_secrets_match.group(1))
            repo = safe_segment(action_secrets_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            import actions_secrets
            actions_secrets._bind_runtime(globals())
            return await actions_secrets.repo_handler(
                self.env, request, owner, repo,
                action_secrets_match.group(3) or "")

        action_runs_match = REPO_ACTION_RUNS_RE.match(url.path)
        if action_runs_match:
            owner = safe_segment(action_runs_match.group(1))
            repo = safe_segment(action_runs_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repository_actions_status_handler(
                self.env, request, owner, repo)

        action_control_match = REPO_ACTION_CONTROL_RE.match(url.path)
        if action_control_match:
            owner = safe_segment(action_control_match.group(1))
            repo = safe_segment(action_control_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repository_actions_control_handler(
                self.env, request, owner, repo)

        pulls_match = REPO_PULLS_RE.match(url.path)
        if pulls_match:
            owner = safe_segment(pulls_match.group(1))
            repo = safe_segment(pulls_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            if (await _repo_is_private(self.env, owner, repo)
                    and not (method_name(request) in ("GET", "DELETE")
                             and await _authorize_owner(
                                 self.env, request, owner))):
                return json_response({"error": "not_found"}, status=404)
            return await pulls_handler(self.env, request, owner, repo)

        discussions_match = REPO_DISCUSSIONS_RE.match(url.path)
        if discussions_match:
            owner = safe_segment(discussions_match.group(1))
            repo = safe_segment(discussions_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            if (await _repo_is_private(self.env, owner, repo)
                    and not (method_name(request) in ("GET", "DELETE")
                             and await _authorize_owner(
                                 self.env, request, owner))):
                return json_response({"error": "not_found"}, status=404)
            return await discussions_handler(self.env, request, owner, repo)

        pending_match = REPO_PENDING_RE.match(url.path)
        if pending_match:
            owner = safe_segment(pending_match.group(1))
            repo = safe_segment(pending_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_pending_counts_handler(self.env, request, owner, repo)

        # Remote fediverse replies for a repo thread (public read), and the
        # owner-node push of release/merge announcements into the fediverse.
        fedi_comments_match = REPO_FEDI_COMMENTS_RE.match(url.path)
        if fedi_comments_match:
            owner = safe_segment(fedi_comments_match.group(1))
            repo = safe_segment(fedi_comments_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await fedi_comments_handler(self.env, request, owner, repo)

        ap_publish_match = REPO_AP_PUBLISH_RE.match(url.path)
        if ap_publish_match:
            owner = safe_segment(ap_publish_match.group(1))
            repo = safe_segment(ap_publish_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await ap_publish_handler(self.env, request, owner, repo)

        ap_digest_match = REPO_AP_DIGEST_RE.match(url.path)
        if ap_digest_match:
            owner = safe_segment(ap_digest_match.group(1))
            repo = safe_segment(ap_digest_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await ap_digest_handler(
                self.env, request, owner, repo)

        ap_posts_match = REPO_AP_POSTS_RE.match(url.path)
        if ap_posts_match:
            owner = safe_segment(ap_posts_match.group(1))
            repo = safe_segment(ap_posts_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await ap_posts_handler(self.env, request, owner, repo)

        webhooks_match = REPO_WEBHOOKS_RE.match(url.path)
        if webhooks_match:
            owner = safe_segment(webhooks_match.group(1))
            repo = safe_segment(webhooks_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_webhooks_handler(self.env, request, owner, repo)

        provider_mirror_match = REPO_PROVIDER_MIRROR_RE.match(url.path)
        if provider_mirror_match:
            owner = safe_segment(provider_mirror_match.group(1))
            repo = safe_segment(provider_mirror_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_provider_mirror_handler(
                self.env, request, owner, repo)

        repo_media_match = REPO_MEDIA_RE.match(url.path)
        if repo_media_match:
            owner = safe_segment(repo_media_match.group(1))
            repo = safe_segment(repo_media_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_media_handler(
                self.env, request, owner, repo, repo_media_match.group(3))

        repo_card_match = REPO_CARD_RE.match(url.path)
        if repo_card_match:
            owner = safe_segment(repo_card_match.group(1))
            repo = safe_segment(repo_card_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_card_handler(self.env, request, owner, repo)

        view_match = REPO_VIEW_RE.match(url.path)
        if view_match:
            owner = safe_segment(view_match.group(1))
            repo = safe_segment(view_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_view_handler(self.env, request, owner, repo)

        if REPO_VIEWS_RE.match(url.path):
            return await repo_views_lookup_handler(self.env, request)

        star_match = REPO_STAR_RE.match(url.path)
        if star_match:
            owner = safe_segment(star_match.group(1))
            repo = safe_segment(star_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_star_handler(self.env, request, owner, repo)

        reactions_match = REPO_REACTIONS_RE.match(url.path)
        if reactions_match:
            owner = safe_segment(reactions_match.group(1))
            repo = safe_segment(reactions_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await comment_reactions_handler(
                self.env, request, owner, repo)

        forks_match = REPO_FORKS_RE.match(url.path)
        if forks_match:
            owner = safe_segment(forks_match.group(1))
            repo = safe_segment(forks_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_forks_handler(self.env, request, owner, repo)

        remote_forks_match = REPO_REMOTE_FORKS_RE.match(url.path)
        if remote_forks_match:
            owner = safe_segment(remote_forks_match.group(1))
            repo = safe_segment(remote_forks_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            import remote_forks
            remote_forks._bind_runtime(globals())
            return await remote_forks.handler(
                self.env, request, owner, repo,
                await _public_repo_owner(
                    self.env, request, owner, repo, REPO_REMOTE_FORKS_RE))

        pull_batch_match = REPO_PULL_BATCH_RE.match(url.path)
        if pull_batch_match:
            owner = safe_segment(pull_batch_match.group(1))
            repo = safe_segment(pull_batch_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            import pull_batch
            import remote_forks
            remote_forks._bind_runtime(globals())
            pull_batch._bind_runtime(globals())
            return await pull_batch.handler(
                self.env, request, owner, repo,
                await _public_repo_owner(
                    self.env, request, owner, repo, REPO_PULL_BATCH_RE))

        subscribe_match = REPO_SUBSCRIBE_RE.match(url.path)
        if subscribe_match:
            owner = safe_segment(subscribe_match.group(1))
            repo = safe_segment(subscribe_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await subscribe_handler(self.env, request, owner, repo)

        bounty_match = REPO_BOUNTY_RE.match(url.path)
        if bounty_match:
            owner = safe_segment(bounty_match.group(1))
            repo = safe_segment(bounty_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await bounties_handler(self.env, request, owner, repo)

        shares_match = REPO_SHARES_RE.match(url.path)
        if shares_match:
            owner = safe_segment(shares_match.group(1))
            repo = safe_segment(shares_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await shares_handler(self.env, request, owner, repo)

        security_scans_match = REPO_SECURITY_SCANS_RE.match(url.path)
        if security_scans_match:
            owner = safe_segment(security_scans_match.group(1))
            repo = safe_segment(security_scans_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repository_security_scans_handler(
                self.env,
                request,
                owner,
                repo,
                security_scans_match.group(3),
            )

        mirror_reachability_match = REPO_MIRROR_REACHABILITY_RE.match(
            url.path)
        if mirror_reachability_match:
            owner = safe_segment(mirror_reachability_match.group(1))
            repo = safe_segment(mirror_reachability_match.group(2))
            node = safe_segment(mirror_reachability_match.group(3))
            if not owner or not repo or not node:
                return json_response({"error": "not_found"}, status=404)
            return await repo_mirror_reachability_handler(
                self.env, request, owner, repo, node)

        mirrors_match = REPO_MIRRORS_RE.match(url.path)
        if mirrors_match:
            owner = safe_segment(mirrors_match.group(1))
            repo = safe_segment(mirrors_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_mirrors_handler(self.env, request, owner, repo)

        traffic_match = REPO_TRAFFIC_RE.match(url.path)
        if traffic_match:
            owner = safe_segment(traffic_match.group(1))
            repo = safe_segment(traffic_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_traffic_handler(self.env, request, owner, repo)

        logo_suggestions_match = REPO_LOGO_SUGGESTIONS_RE.match(url.path)
        if logo_suggestions_match:
            owner = safe_segment(logo_suggestions_match.group(1))
            repo = safe_segment(logo_suggestions_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await native_repository_logo_handler(
                self.env, request, owner, repo, suggestions=True)

        logo_match = REPO_LOGO_RE.match(url.path)
        if logo_match:
            owner = safe_segment(logo_match.group(1))
            repo = safe_segment(logo_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await native_repository_logo_handler(
                self.env, request, owner, repo)

        about_match = REPO_ABOUT_RE.match(url.path)
        if about_match:
            owner = safe_segment(about_match.group(1))
            repo = safe_segment(about_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_about_handler(self.env, request, owner, repo)

        privacy_match = REPO_PRIVACY_RE.match(url.path)
        if privacy_match:
            owner = safe_segment(privacy_match.group(1))
            repo = safe_segment(privacy_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_privacy_handler(
                self.env, request, owner, repo)

        # The account's whole cloud queue in one read. A desktop shows the
        # runs its mirrors are working whichever repository is open, and a
        # phone reads every repository at once, without either client asking
        # repository by repository.
        if ACCOUNT_AGENT_SESSIONS_RE.match(url.path):
            return await account_agent_sessions_handler(self.env, request)

        if ACCOUNT_DESKTOP_AGENTS_RE.match(url.path):
            return await account_desktop_agents_handler(self.env, request)

        owner_agent_claim_match = REPO_OWNER_AGENT_SESSION_CLAIM_RE.match(
            url.path)
        if owner_agent_claim_match:
            owner = safe_segment(owner_agent_claim_match.group(1))
            repo = safe_segment(owner_agent_claim_match.group(2))
            session_id = safe_segment(owner_agent_claim_match.group(3))
            if not owner or not repo or not session_id:
                return json_response({"error": "not_found"}, status=404)
            return await repo_owner_agent_session_claim_handler(
                self.env, request, owner, repo, session_id)

        agent_stream_match = (
            REPO_OWNER_AGENT_SESSION_STREAM_RE.match(url.path)
            or ORG_AGENT_BOT_STREAM_RE.match(url.path))
        if agent_stream_match:
            owner = safe_segment(agent_stream_match.group(1))
            repo = safe_segment(agent_stream_match.group(2))
            session_id = safe_segment(agent_stream_match.group(3))
            if not owner or not repo or not session_id:
                return json_response({"error": "not_found"}, status=404)
            return await org_agent_bot_stream_handler(
                self.env, request, owner, repo, session_id,
                owner_scope=url.path.startswith("/api/repo/"))

        agent_diff_match = (
            REPO_OWNER_AGENT_SESSION_DIFF_RE.match(url.path)
            or ORG_AGENT_BOT_DIFF_RE.match(url.path))
        if agent_diff_match:
            owner = safe_segment(agent_diff_match.group(1))
            repo = safe_segment(agent_diff_match.group(2))
            session_id = safe_segment(agent_diff_match.group(3))
            if not owner or not repo or not session_id:
                return json_response({"error": "not_found"}, status=404)
            import agent_branches
            agent_branches._bind_runtime(globals())
            return await agent_branches.diff_handler(
                self.env, request, owner, repo, session_id,
                owner_scope=url.path.startswith("/api/repo/"))

        owner_agent_attachment_match = (
            REPO_OWNER_AGENT_SESSION_ATTACHMENT_RE.match(url.path))
        if owner_agent_attachment_match:
            owner = safe_segment(owner_agent_attachment_match.group(1))
            repo = safe_segment(owner_agent_attachment_match.group(2))
            session_id = safe_segment(owner_agent_attachment_match.group(3))
            if not owner or not repo or not session_id:
                return json_response({"error": "not_found"}, status=404)
            return await org_agent_session_attachment_handler(
                self.env, request, owner, repo, session_id,
                owner_agent_attachment_match.group(4), owner_scope=True)

        org_agent_attachment_match = ORG_AGENT_BOT_ATTACHMENT_RE.match(
            url.path)
        if org_agent_attachment_match:
            org = safe_segment(org_agent_attachment_match.group(1))
            repo = safe_segment(org_agent_attachment_match.group(2))
            session_id = safe_segment(org_agent_attachment_match.group(3))
            if not org or not repo or not session_id:
                return json_response({"error": "not_found"}, status=404)
            return await org_agent_session_attachment_handler(
                self.env, request, org, repo, session_id,
                org_agent_attachment_match.group(4))

        owner_agent_session_match = REPO_OWNER_AGENT_SESSION_RE.match(
            url.path)
        if owner_agent_session_match:
            owner = safe_segment(owner_agent_session_match.group(1))
            repo = safe_segment(owner_agent_session_match.group(2))
            session_id = safe_segment(owner_agent_session_match.group(3))
            if not owner or not repo or not session_id:
                return json_response({"error": "not_found"}, status=404)
            return await org_agent_bot_handler(
                self.env, request, owner, repo, session_id,
                owner_scope=True)

        owner_agent_sessions_match = REPO_OWNER_AGENT_SESSIONS_RE.match(
            url.path)
        if owner_agent_sessions_match:
            owner = safe_segment(owner_agent_sessions_match.group(1))
            repo = safe_segment(owner_agent_sessions_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await org_agent_bots_handler(
                self.env, request, owner, repo, owner_scope=True)

        org_agent_bot_match = ORG_AGENT_BOT_RE.match(url.path)
        if org_agent_bot_match:
            org = safe_segment(org_agent_bot_match.group(1))
            repo = safe_segment(org_agent_bot_match.group(2))
            session_id = safe_segment(org_agent_bot_match.group(3))
            if not org or not repo or not session_id:
                return json_response({"error": "not_found"}, status=404)
            return await org_agent_bot_handler(
                self.env, request, org, repo, session_id)

        org_agent_bots_match = ORG_AGENT_BOTS_RE.match(url.path)
        if org_agent_bots_match:
            org = safe_segment(org_agent_bots_match.group(1))
            repo = safe_segment(org_agent_bots_match.group(2))
            if not org or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await org_agent_bots_handler(
                self.env, request, org, repo)

        org_agent_job_result_match = REPO_ORG_AGENT_JOB_RESULT_RE.match(
            url.path)
        if org_agent_job_result_match:
            owner = safe_segment(org_agent_job_result_match.group(1))
            repo = safe_segment(org_agent_job_result_match.group(2))
            job_id = org_agent_job_result_match.group(3)
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_org_agent_job_result_handler(
                self.env, request, owner, repo, job_id)

        org_agent_job_stream_match = REPO_ORG_AGENT_JOB_STREAM_RE.match(
            url.path)
        if org_agent_job_stream_match:
            owner = safe_segment(org_agent_job_stream_match.group(1))
            repo = safe_segment(org_agent_job_stream_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_org_agent_job_stream_handler(
                self.env, request, owner, repo,
                org_agent_job_stream_match.group(3))

        org_agent_job_attachment_match = (
            REPO_ORG_AGENT_JOB_ATTACHMENT_RE.match(url.path))
        if org_agent_job_attachment_match:
            owner = safe_segment(org_agent_job_attachment_match.group(1))
            repo = safe_segment(org_agent_job_attachment_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_org_agent_job_attachment_handler(
                self.env, request, owner, repo,
                org_agent_job_attachment_match.group(3),
                org_agent_job_attachment_match.group(4))

        org_agent_jobs_match = REPO_ORG_AGENT_JOBS_RE.match(url.path)
        if org_agent_jobs_match:
            owner = safe_segment(org_agent_jobs_match.group(1))
            repo = safe_segment(org_agent_jobs_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await repo_org_agent_jobs_handler(
                self.env, request, owner, repo)

        agents_list_match = REPO_AGENTS_LIST_RE.match(url.path)
        if agents_list_match:
            owner = safe_segment(agents_list_match.group(1))
            repo = safe_segment(agents_list_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await agents_list_handler(self.env, request, owner, repo)

        desktop_commands_match = (
            REPO_DESKTOP_COMMANDS_RE.match(url.path)
            or REPO_DESKTOP_COMMANDS_ACK_RE.match(url.path))
        if desktop_commands_match:
            owner = safe_segment(desktop_commands_match.group(1))
            repo = safe_segment(desktop_commands_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            if url.path.endswith("/ack"):
                return await desktop_commands_ack_handler(
                    self.env, request, owner, repo)
            return await desktop_commands_handler(
                self.env, request, owner, repo)

        agents_ack_match = REPO_AGENTS_ACK_RE.match(url.path)
        if agents_ack_match:
            owner = safe_segment(agents_ack_match.group(1))
            repo = safe_segment(agents_ack_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await agents_ack_handler(
                self.env, request, owner, repo)

        agents_transcript_match = REPO_AGENTS_TRANSCRIPT_RE.match(url.path)
        if agents_transcript_match:
            owner = safe_segment(agents_transcript_match.group(1))
            repo = safe_segment(agents_transcript_match.group(2))
            agent_id = safe_segment(agents_transcript_match.group(3))
            if not owner or not repo or not agent_id:
                return json_response({"error": "not_found"}, status=404)
            return await agents_transcript_handler(self.env, request, owner, repo, agent_id)

        agent_handoff_match = REPO_AGENT_HANDOFF_RE.match(url.path)
        if agent_handoff_match:
            owner = safe_segment(agent_handoff_match.group(1))
            repo = safe_segment(agent_handoff_match.group(2))
            agent_id = safe_segment(agent_handoff_match.group(3))
            if not owner or not repo or not agent_id:
                return json_response({"error": "not_found"}, status=404)
            return await agent_handoff_handler(
                self.env, request, owner, repo, agent_id)

        agents_prompt_match = REPO_AGENTS_PROMPT_RE.match(url.path)
        if agents_prompt_match:
            owner = safe_segment(agents_prompt_match.group(1))
            repo = safe_segment(agents_prompt_match.group(2))
            agent_id = safe_segment(agents_prompt_match.group(3))
            if not owner or not repo or not agent_id:
                return json_response({"error": "not_found"}, status=404)
            return await agents_prompt_handler(self.env, request, owner, repo, agent_id)

        agents_match = REPO_AGENTS_RE.match(url.path)
        if agents_match:
            owner = safe_segment(agents_match.group(1))
            repo = safe_segment(agents_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await agents_handler(self.env, request, owner, repo)

        release_downloads_match = REPO_RELEASE_DOWNLOADS_RE.match(url.path)
        if release_downloads_match:
            owner = safe_segment(release_downloads_match.group(1))
            repo = safe_segment(release_downloads_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            return await release_downloads_handler(self.env, request, owner, repo)

        release_blob_match = RELEASE_BLOB_RE.match(url.path)
        if release_blob_match:
            owner = safe_segment(release_blob_match.group(1))
            repo = safe_segment(release_blob_match.group(2))
            sha256 = release_blob_match.group(3)
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            if await _repo_is_private(self.env, owner, repo):
                return json_response({"error": "not_found"}, status=404)
            response = await _https_mirror_proxy(
                self.env, request, owner, repo, "release-blob",
                release_sha=sha256)
            if (
                method_name(request) == "GET"
                and int(getattr(response, "status", 0) or 0) == 200
            ):
                await record_release_download(
                    self.env, await blind_index(self.env, owner + "/" + repo),
                    sha256,
                    clean_string(request.headers.get("user-agent") or "", 512))
            return response

        host_match = REPO_HOST_RE.match(url.path)
        if host_match:
            owner = safe_segment(host_match.group(1))
            repo = safe_segment(host_match.group(2))
            if not owner or not repo:
                return json_response({"error": "not_found"}, status=404)
            action = host_match.group(3)
            upgrade = (request.headers.get("upgrade") or "").lower()
            if (
                upgrade != "websocket"
                and action in REPO_HOST_READ_ACTIONS
            ):
                if await _repo_is_private(self.env, owner, repo):
                    # Named private content routes are intentionally inert. The
                    # only private byte transport is the identity-free opaque
                    # endpoint, with authorization outside the URL -- except a
                    # private repository the core nodes host, which its
                    # readers' sessions reach here (hosted_git.py).
                    hosted_read = globals().get("hosted_private_read")
                    hosted = (
                        await hosted_read(
                            self.env, request, owner, repo, action)
                        if callable(hosted_read) else None)
                    if hosted is not None:
                        return hosted
                    return _private_replica_not_found()
                return await _https_mirror_proxy(
                    self.env, request, owner, repo, action)
            # The repository tunnel is permanently retired. Repository bytes
            # use ordinary HTTPS mirror routes; desktop clients use bounded
            # polling for control-plane changes.
            return _repository_socket_retired()

        room = room_key_from_path(url.path)
        if room:
            public_world_general = (
                room.get("key") == PUBLIC_WORLD_GENERAL_ROOM_KEY
            )
            if (
                not room.get("compat")
                and not public_world_general
                and await _repository_access_context(
                    self.env,
                    request,
                    room.get("owner", ""),
                    room.get("repo", ""),
                ) is None
            ):
                # Apply this before choosing or calling the Durable Object. A
                # guessed private/missing repository therefore cannot reveal
                # whether a room or retained history exists.
                return _private_replica_not_found()
            if url.path.endswith("/clients"):
                # The per-room observer was retired in favor of the bounded
                # network stats snapshot. Older desktop builds still poll this
                # path; answer before touching a Durable Object so those
                # clients cannot repeatedly wake or exhaust a room isolate.
                return json_response(
                    {"error": "observers_removed"},
                    status=410,
                    cache_control="no-store",
                )
            room_id = self.env.FORKMESH_MAINNODE_ROOM.idFromName(room["key"])
            # The platform can abort a room DO mid-request — the free-tier
            # duration cap kills long-lived requests, and any co-located DO
            # blowing the isolate's limits resets every in-flight request with
            # a generic "internal error". Both are transient: a fresh stub
            # lands on a replacement isolate, so retry once before failing.
            # Room requests carry no body (GET snapshot / WebSocket upgrade),
            # so re-driving durable_object_request is safe. Without this
            # guard the abort would escape Default.fetch as a Worker Error
            # 1101; with it, a persistent failure is recorded (the /status
            # page counts these) and answered with a retryable 503 that
            # leaks no exception detail to the client.
            last_error = None
            for _attempt in range(2):
                room_object = self.env.FORKMESH_MAINNODE_ROOM.get(room_id)
                try:
                    return await room_object.fetch(
                        await durable_object_request(request))
                except Exception as error:
                    last_error = error
            await log_durable_object_abort(
                self.env, request, url.path, last_error)
            return json_response(
                {"error": "unavailable"}, status=503,
                extra_headers=EXPECTED_DEGRADED_HEADERS)

        # Personalised hackathon pages. Matched before the repo shortcut for
        # the same reason /r/<name> is: /hackathon/join/<name> must never be
        # read as owner/repo. Both return None when no campaign is public,
        # which falls through to the ordinary 404 -- that is what keeps the
        # whole feature invisible while its campaign is still a draft.
        hackathon_invite = HACKATHON_INVITE_RE.match(url.path)
        if hackathon_invite:
            invite_page = await hackathon_invite_page_handler(
                self.env, request, unquote(hackathon_invite.group(1)))
            if invite_page is not None:
                return invite_page
            return await self._serve_not_found_page(url)

        hackathon_project = HACKATHON_PROJECT_RE.match(url.path)
        if hackathon_project:
            project_page = await hackathon_project_page_handler(
                self.env, request, hackathon_project.group(1))
            if project_page is not None:
                return project_page
            return await self._serve_not_found_page(url)

        # The Worker owns the whole /pitch-deck prefix (wrangler.toml
        # run_worker_first), so the deck document is only ever reached
        # through a live token: any other spelling is a 404 here.
        if url.path == "/pitch-deck" or url.path.startswith("/pitch-deck/"):
            pitch_deck = PITCH_DECK_PAGE_RE.match(url.path)
            deck_page = None
            if pitch_deck:
                deck_page = await pitch_deck_page_handler(
                    self.env, request, pitch_deck.group(1))
            if deck_page is not None:
                return deck_page
            return await self._serve_not_found_page(url)

        hackathon_challenge = HACKATHON_CHALLENGE_RE.match(url.path)
        if hackathon_challenge:
            challenge_page = await hackathon_challenge_page_handler(
                self.env, request, unquote(hackathon_challenge.group(1)))
            if challenge_page is not None:
                return challenge_page
            return await self._serve_not_found_page(url)

        hackathon_squad = HACKATHON_SQUAD_RE.match(url.path)
        if hackathon_squad:
            squad_page = await hackathon_squad_page_handler(
                self.env, request, unquote(hackathon_squad.group(1)))
            if squad_page is not None:
                return squad_page
            return await self._serve_not_found_page(url)

        # Referral share links: count the click, bounce to signup. Matched
        # before the repo shortcut so /r/<name> never reads as owner/repo.
        referral_match = REFERRAL_LINK_RE.match(url.path)
        if referral_match:
            return await referral_click(
                self.env, unquote(referral_match.group(1)), request)

        # Human-readable published notes. Match before the generic two-segment
        # repository shortcut: /notes/<id> otherwise looks like owner/repo.
        if re.fullmatch(r"/notes/[0-9a-f]{32}/?", url.path):
            base = url.scheme + "://" + url.netloc + "/"
            try:
                resp = await self.env.ASSETS.fetch(base + "notes/view.html")
                body = await resp.text()
            except Exception:
                body = "<!doctype html><title>ForkMesh note</title>"
            return Response(body, status=200, headers={
                "content-type": "text/html; charset=utf-8",
                "cache-control": "public, max-age=300",
                "content-security-policy": (
                    "default-src 'self'; img-src 'self' data: https:; "
                    "style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'"),
            })

        # Repo shortcut URLs (/owner/repo and tab/tree/blob deep links) all
        # serve the prebuilt repo-detail page; its JS resolves the path. The
        # per-repo rel="me" head tags are injected at serve time so Mastodon
        # can verify the repo actor's profile link (green check) against this
        # very page — a static shared asset can't carry a per-repo link.
        project_target = await self._repo_projects_redirect(url)
        if project_target:
            return Response("", status=302, headers={
                "location": project_target, "cache-control": "no-store"})

        short_route = await self._repo_short_route(url)
        if short_route is not None:
            short_url, short_name = short_route
            short_alias = await self._repo_short_alias_redirect(url)
            if short_alias:
                return Response("", status=308, headers={
                    "location": short_alias})
            if short_name and looks_like_repo_route(short_url.path):
                return await self._serve_repo_page(
                    request, short_url, short_name)
            return await self._route(request, short_url)

        if looks_like_repo_route(url.path):
            canonical_target = await self._repo_canonical_redirect(
                request, url)
            if canonical_target:
                return Response("", status=308, headers={
                    "location": canonical_target,
                    "cache-control": "no-store"})
            return await self._serve_repo_page(request, url)

        if url.path == "/dashboard" or url.path.startswith("/dashboard/"):
            # Legacy /dashboard?section=X links 308 to the per-page paths.
            target = dashboard_section_redirect(url.path, url.query)
            if target:
                return Response("", status=308, headers={"location": target})
            # /dashboard/repos/ -> /dashboard/repos (canonical, no trailing /).
            normalized = url.path.rstrip("/") or "/dashboard"
            if normalized != url.path and normalized in DASHBOARD_PAGE_ASSETS:
                return Response("", status=308, headers={"location": normalized})
            asset = DASHBOARD_PAGE_ASSETS.get(url.path)
            if asset:
                return await self._serve_dashboard_asset(url, asset)
            # Settings sub-tabs (/dashboard/settings/<tab>) share the settings
            # document; the client restores the tab from the path.
            if url.path.startswith("/dashboard/settings/"):
                return await self._serve_dashboard_asset(
                    url, DASHBOARD_PAGE_ASSETS["/dashboard/settings"])
            # Old bounced deep links (/dashboard/owner/repo[...]) move to the
            # clean repo path.
            rest = dashboard_repo_redirect(url.path)
            if rest:
                return Response("", status=308, headers={"location": rest})
            return await self._serve_not_found_page(url)

        account_target = await self._account_page_redirect(url)
        if account_target:
            return Response("", status=302, headers={
                "location": account_target, "cache-control": "no-store"})
        if (not url.path.startswith("/api/")
                and _is_page_navigation(request)):
            return await self._serve_not_found_page(url)
        return json_response({"error": "not_found"}, status=404)

    async def _account_page_redirect(self, url):
        name = account_page_name(url.path)
        if not name:
            return None
        try:
            if not (await _org_row(self.env, name))[1]:
                account = (await _account_row(self.env, name))[1]
                if (not account or account.get("status") != "active"
                        or _account_kind(account) == "node"):
                    return None
        except Exception:
            return None
        return "/@" + quote(name) + ("?" + url.query if url.query else "")

    async def _repo_short_alias_redirect(self, url):
        setting = await self._repo_short_setting()
        if not setting["short"]:
            return None
        return repo_short_urls.alias_redirect(
            url.path, url.query, setting["owner"])

    async def _serve_not_found_page(self, url):
        base = url.scheme + "://" + url.netloc + "/"
        try:
            resp = await self.env.ASSETS.fetch(base + "404.html")
            body = await resp.text()
        except Exception:
            body = "not found"
        return Response(
            body,
            status=404,
            headers={"content-type": "text/html; charset=utf-8"},
        )

    def _published_site_origins(self):
        """Every origin this deployment publishes as one of its own.

        A request that arrives on some other host — a node subdomain, a
        tunnel host, a preview host — is a relay origin holding its own
        account session, so it keeps its documents instead of being sent to
        the origins listed here.
        """
        return [
            str(getattr(self.env, name, "") or "")
            for name in (
                "PUBLIC_BASE_URL", "API_ORIGIN", "APP_ORIGIN",
                "WWW_ORIGIN", "WORLD_ORIGIN",
            )
        ]

    @staticmethod
    def _site_document_headers(source, content_type, proxied=False):
        """Keep the public-document security policy when relaying it."""
        headers = {
            "content-type": content_type,
            "cache-control": "public, max-age=300",
        }
        for name in SITE_DOCUMENT_SECURITY_HEADERS:
            try:
                value = source.headers.get(name)
            except Exception:
                value = ""
            if value:
                headers[name] = str(value)
        for name, value in SITE_DOCUMENT_FALLBACK_SECURITY_HEADERS.items():
            headers.setdefault(name, value)
        if proxied:
            # The canonical public origin remains the one indexable copy.
            headers["x-robots-tag"] = "noindex"
        return headers

    async def _serve_site_document(self, request, url, upstream):
        """Serve a www-owned document under the origin that asked for it.

        Docs and Blog are reached from the dashboard's own sidebar, so the
        visitor following them is signed in. Both halves of that session are
        origin-scoped — the account cookie is host-only and the
        ``forkmesh.session`` marker lives in localStorage — so a 308 to the
        marketing origin renders the shared product header signed out and no
        amount of signing in on the destination fixes it (issue #625).

        A node-hosted request always reads the canonical document: the App
        bundle carries a compatibility ``blog.html`` shell but intentionally
        does not carry the official Blog's artwork or document tree. A
        cutover/self-host deployment instead has no upstream and answers from
        its own bundle. In either case the node's credentials never leave its
        origin and the relayed copy cannot compete with the canonical URL.
        """
        if upstream:
            if self._request_has_header(request, SITE_DOCUMENT_ORIGIN_HEADER):
                return await self._serve_not_found_page(url)
            return await self._proxy_site_document(request, url, upstream)
        base = url.scheme + "://" + url.netloc + "/"
        try:
            local = await self.env.ASSETS.fetch(
                base + site_document_asset_path(url.path).lstrip("/")
                + (("?" + url.query) if url.query else ""))
            if int(getattr(local, "status", 0) or 0) == 200:
                return self._without_head_body(request, JsResponse.new(
                    local.body, to_js({
                        "status": 200,
                        "headers": self._site_document_headers(
                            local,
                            str(local.headers.get("content-type")
                                or "text/html; charset=utf-8")),
                    })))
        except Exception:
            pass
        return await self._serve_not_found_page(url)

    async def _serve_world_asset(self, request, url):
        path = world_asset_path(url.path)
        if not path:
            return await self._serve_not_found_page(url)
        base = url.scheme + "://" + url.netloc
        try:
            local = await self.env.ASSETS.fetch(base + path)
            if int(getattr(local, "status", 0) or 0) != 200:
                return await self._serve_not_found_page(url)
            headers = dict(WORLD_DOCUMENT_SECURITY_HEADERS)
            headers["content-type"] = str(
                local.headers.get("content-type")
                or "application/octet-stream")
            headers["cache-control"] = world_asset_cache_control(
                url.path, url.query)
            return self._without_head_body(request, JsResponse.new(
                local.body, to_js({"status": 200, "headers": headers})))
        except Exception:
            return await self._serve_not_found_page(url)

    async def _proxy_site_document(self, request, url, upstream):
        destination = upstream + url.path + (
            ("?" + url.query) if url.query else "")
        try:
            response = await js_fetch_with_timeout(
                destination,
                {
                    "method": "GET",
                    "redirect": "follow",
                    # Deliberately not the caller's headers: the owning origin
                    # is a different origin and must never receive this
                    # visitor's cookies. The marker is the loop guard — a
                    # relay asked to proxy a relay serves its own 404 instead.
                    "headers": {
                        # The documents are language-neutral static content,
                        # so do not make a publicly cacheable node response
                        # vary with any browser header.
                        "accept": "text/html,application/json;q=0.9,*/*;q=0.1",
                        SITE_DOCUMENT_ORIGIN_HEADER: url.netloc,
                    },
                },
                SITE_DOCUMENT_PROXY_TIMEOUT_SECONDS,
            )
            status = int(getattr(response, "status", 0) or 0)
            if status != 200:
                return await self._serve_not_found_page(url)
            content_type = str(
                response.headers.get("content-type")
                or "text/html; charset=utf-8")
            if not (
                "text/html" in content_type.lower()
                or "json" in content_type.lower()
            ):
                return await self._serve_not_found_page(url)
            body = str(await response.text())
        except Exception:
            return await self._serve_not_found_page(url)
        if len(body.encode("utf-8")) > SITE_DOCUMENT_PROXY_MAX_BYTES:
            return await self._serve_not_found_page(url)
        if "text/html" in content_type.lower():
            # Post artwork is the one subresource the split App bundle does
            # not carry. Absolute upstream URLs keep those images loading
            # (the site CSP allows https: images) while the document itself,
            # its scripts and its styles stay same-origin.
            body = body.replace('"/assets/blog/', '"' + upstream + "/assets/blog/")
        return self._without_head_body(request, Response(
            body,
            status=200,
            headers=self._site_document_headers(
                response, content_type, proxied=True),
        ))

    async def _serve_profile_page(self, url, name, repositories=False):
        # /@name — the FULL public profile: the same prebuilt dashboard
        # profile document (sidebar, contribution heatmap, activity feed,
        # repositories tab) with per-user head tags injected at serve time.
        # dashboard.js detects the /@name path and renders the named account's
        # public data instead of the logged-in session (public-profile mode).
        # Eligibility mirrors the fediverse actor: active + not private —
        # node-owner accounts get pages too, matching _ap_user_federates.
        # Served identically to every viewer (personalization happens in
        # dashboard.js), so the built page lives in the colo edge cache: it is
        # the rel=me verification target every follower server GETs after an
        # Update(Person), and those bursts must not each rebuild it at origin.
        # _ap_broadcast_actor_update purges the key on profile changes.
        origin = _ap_origin(self.env)
        await ensure_schema(self.env)
        _, rec = await _account_row(self.env, name)
        is_org = False
        if (not rec or rec.get("status") != "active"
                or rec.get("profile_private")):
            # Organizations share the /@name address space: the same profile
            # document renders them from the public /api/orgs/<name> lookup.
            # An existing (inactive or private) account still 404s.
            if rec or not (await _org_row(self.env, name))[1]:
                if not rec and name == "desktop" and not repositories:
                    return Response("", status=302, headers={
                        "location": "/desktop",
                        "cache-control": "no-store"})
                return await self._serve_not_found_page(url)
            is_org = True
        cache_key = "%s/@%s%s" % (
            origin, quote(name), "/repositories" if repositories else "")
        cached = await edge_cache_match(cache_key)
        if cached is not None:
            return cached
        asset = DASHBOARD_PAGE_ASSETS[
            "/dashboard/profile/repositories" if repositories
            else "/dashboard/profile"]
        base = url.scheme + "://" + url.netloc + "/"
        try:
            resp = await self.env.ASSETS.fetch(base + asset)
            body = await resp.text()
        except Exception:
            body = "<!doctype html><title>ForkMesh</title>"
        canonical = "%s/@%s" % (origin, quote(name))
        # rel="me" is the reciprocal half of the fediverse Person actor's
        # verified profile link (the actor's `url` is this page); the
        # alternate link lets fediverse software discover the actor from the
        # page URL.
        tags = (
            "<link rel=\"canonical\" href=\"%s\">"
            "<link rel=\"me\" href=\"%s\">"
            "<link rel=\"alternate\" type=\"application/activity+json\" "
            "href=\"%s/ap/users/%s\">"
            % (canonical, canonical, origin, quote(name)))
        if is_org:
            # Organizations have no Person actor to verify or discover.
            tags = "<link rel=\"canonical\" href=\"%s\">" % canonical
        if "</head>" in body:
            body = body.replace("</head>", tags + "</head>", 1)
        shown = name if is_org else account_names.display_name(rec, name)
        body = re.sub(r"<title>[^<]*</title>",
                      "<title>@%s · %s</title>" % (
                          _html_escape(shown), _html_escape(
                              instance_features.display_name(self.env))),
                      body, count=1)
        page = Response(body, status=200, headers={
            "content-type": "text/html; charset=utf-8",
            "cache-control": "public, max-age=300",
        })
        await edge_cache_put(cache_key, page)
        return page

    async def _repo_short_setting(self):
        return await _repo_short_setting_row(self.env)

    async def _repo_short_keep(self, owner):
        return await _repo_short_links(self.env, owner)

    async def _repo_addresses_doc(self):
        setting = await self._repo_short_setting()
        owner = repo_short_urls.canonical_owner(setting)
        links = await self._repo_short_keep(owner) if owner else {}
        if links is None:
            setting = dict(setting, canonical=False)
        return repo_short_urls.client_doc(setting, links)

    async def _repo_canonical_redirect(self, request, url):
        if method_name(request) not in ("GET", "HEAD"):
            return None
        return await _repo_canonical_path(self.env, url.path, url.query)

    async def _repo_canonical_json(self, request, url, response):
        if method_name(request) != "GET" or int(
                getattr(response, "status", 0) or 0) != 200:
            return response
        setting = await self._repo_short_setting()
        owner = repo_short_urls.canonical_owner(setting)
        if not owner:
            return response
        links = await self._repo_short_keep(owner)
        if links is None:
            return response
        text = str(await response.text())
        headers = [(k, v) for k, v in response.headers.items()
                   if str(k).lower() not in ("content-length", "etag")]
        try:
            data = json.loads(text)
            text = json.dumps(repo_short_urls.short_urls(
                data, url.scheme + "://" + url.netloc, setting, links),
                separators=(",", ":"))
        except Exception:
            pass
        return Response(text, status=200, headers=headers)

    async def _repo_projects_setting(self):
        now = int(Date.now())
        setting = repo_projects.cached(now)
        if setting is not None:
            return setting
        try:
            row = await d1_first(
                self.env,
                "SELECT doc FROM instance_settings WHERE k=?",
                repo_projects.SETTING_KEY)
        except Exception:
            row = None
        return repo_projects.remember(
            repo_projects.parse((row or {}).get("doc") or ""), now)

    async def _repo_projects_redirect(self, url):
        setting = await self._repo_projects_setting()
        if not setting["repo"]:
            return None
        short = await self._repo_short_setting()
        if not short["short"]:
            return None
        return repo_projects.redirect(
            url.path, url.query, setting, short["owner"])

    async def _repo_projects_setting(self):
        now = int(Date.now())
        setting = repo_projects.cached(now)
        if setting is not None:
            return setting
        try:
            row = await d1_first(
                self.env,
                "SELECT doc FROM instance_settings WHERE k=?",
                repo_projects.SETTING_KEY)
        except Exception:
            row = None
        return repo_projects.remember(
            repo_projects.parse((row or {}).get("doc") or ""), now)

    async def _repo_projects_redirect(self, url):
        setting = await self._repo_projects_setting()
        if not setting["repo"]:
            return None
        short = await self._repo_short_setting()
        if not short["short"]:
            return None
        return repo_projects.redirect(
            url.path, url.query, setting, short["owner"])

    async def _repo_short_route(self, url):
        setting = await self._repo_short_setting()
        if not setting["short"]:
            return None
        hit = repo_short_urls.rewrite(url.path, setting["owner"])
        if hit is None:
            return None
        name = repo_short_urls.first_name(url.path)
        if repo_short_urls.OWNER_RE.fullmatch(name):
            try:
                account = (await _account_row(self.env, name))[1]
                if (await _org_row(self.env, name))[1] or (account and not (
                        setting.get("canonical")
                        and _account_kind(account) == "node")):
                    return None
            except Exception:
                return None
        return url._replace(path=hit[0]), hit[1]

    async def _serve_repo_page(self, request, url, short_name=""):
        # The shared repo-detail document, plus per-repo head tags:
        #  - <link rel="me" href="<repo actor url>"> — the reciprocal half of
        #    the repo actor's verified "Repository" row: that row links this git
        #    page, so Mastodon fetches it and looks for a rel=me back to the
        #    actor's `url` (the /@owner.repo fediverse profile) to mark it green;
        #  - <link rel="alternate" type="application/activity+json"> — lets
        #    fediverse software discover the actor straight from the page URL.
        # Identical for every viewer, and the rel=me verification target of
        # the repo actor: edge-cached so follower-server verification bursts
        # after an Update(Group) cost one origin build per colo per TTL.
        origin = url.scheme + "://" + url.netloc
        fediverse_origin = _ap_origin(self.env)
        parts = [p for p in url.path.split("/") if p]
        owner = safe_segment(unquote(parts[0])) if len(parts) >= 2 else ""
        repo = safe_segment(unquote(parts[1])) if len(parts) >= 2 else ""
        cache_key = ""
        explicit_public = False
        data_owner = owner
        repo_record = {}
        social_version = ""
        if owner and repo:
            # Only an explicit public catalog row may contribute names,
            # description, actor links, or a shared cache entry. Private and
            # missing paths intentionally receive the same generic app shell;
            # authenticated client APIs decide whether the viewer may render
            # repository data. This keeps URL probing non-disclosing.
            #
            # An organization repo URL is a public alias, not the encrypted
            # catalog key. Resolve its linked node only for privacy/catalog
            # reads; canonical, actor, card and displayed identity below must
            # remain the organization URL the visitor actually requested.
            data_owner = await _org_repo_node(
                self.env, owner, repo) or owner
            explicit_public = not await _repo_is_private(
                self.env, data_owner, repo)
            if explicit_public:
                try:
                    await ensure_schema(self.env)
                    repo_bi = await blind_index(
                        self.env, data_owner + "/" + repo)
                    repo_row = await d1_first(
                        self.env,
                        "SELECT data FROM repositories WHERE key_bi=?",
                        repo_bi)
                    repo_record = (
                        await decrypt_row(self.env, repo_row.get("data"))
                        if repo_row else {})
                    explicit_public = bool(
                        repo_record
                        and repo_record.get("visibility") == "public"
                        and _catalog_record_matches_identity(
                            repo_record, data_owner, repo))
                except Exception:
                    # A crawler must never receive public metadata or a shared
                    # cache entry when the signed record cannot be re-read.
                    explicit_public = False
                    repo_record = {}
                if explicit_public:
                    social_version = _repository_social_version(repo_record)
                    # A state-less legacy record may still render its generic
                    # shell, but it cannot safely create a reusable social
                    # cache entry until it republishes a commit/state pin.
                    if social_version and not short_name:
                        cache_key = "%s/%s/%s?repo-state=%s" % (
                            origin, quote(owner), quote(repo),
                            quote(social_version, safe=""))
                        cached = await edge_cache_match(cache_key)
                        if cached is not None:
                            return cached
        base = url.scheme + "://" + url.netloc + "/"
        try:
            resp = await self.env.ASSETS.fetch(base + DASHBOARD_REPO_ASSET)
            body = await resp.text()
        except Exception:
            body = "<!doctype html><title>ForkMesh Dashboard</title>"
        canonical_short = bool(short_name) and (
            repo_short_urls.canonical_owner(
                await self._repo_short_setting()) == owner.lower())
        if owner and repo and explicit_public and "</head>" in body:
            canonical = "%s/%s/%s" % (origin, quote(owner), quote(repo))
            if canonical_short:
                canonical = "%s/%s" % (origin, quote(short_name))
            # The repo actor's `url` — the /@owner.repo fediverse profile — is
            # the rel=me target that verifies this page's "Repository" row.
            # Lowercased to match the actor id (handles are case-folded).
            fedi_profile = "%s/@%s.%s" % (
                fediverse_origin, owner.lower(), repo.lower())
            # OpenGraph/Twitter card so social + fediverse shares of the repo
            # URL (the preview under a federated "new pull request" post, a
            # (adhoc #46, see repo_card_handler / og_card.py) — instead of a
            # full-bleed logo. og:description carries the catalog description
            # so the unfurl text matches the repo, not the generic shell copy.
            og_image = "%s/api/repo/%s/%s/card.png?v=%s" % (
                origin, quote(owner), quote(repo), quote(
                    "%s.%s" % (social_version, og_card.CARD_REVISION)
                    if social_version else og_card.CARD_REVISION, safe=""))
            shown = short_name if canonical_short else "%s/%s" % (
                owner, repo)
            og_description = str(
                repo_record.get("description", "") or "")
            if not og_description:
                og_description = ("%s on ForkMesh — distributed Git "
                                  "hosting on a mesh of desktop nodes."
                                  % shown
                                  if not instance_features.branded(self.env)
                                  else "%s on %s" % (
                                      shown,
                                      instance_features.display_name(
                                          self.env)))
            site_name = _html_escape(
                instance_features.display_name(self.env))
            title = _html_escape("%s - " % shown) + site_name
            tags = (
                "<link rel=\"canonical\" href=\"%s\">"
                "<link rel=\"me\" href=\"%s\">"
                "<link rel=\"alternate\" type=\"application/activity+json\" "
                "href=\"%s/ap/repos/%s/%s\">"
                "<meta property=\"og:type\" content=\"website\">"
                "<meta property=\"og:site_name\" content=\"%s\">"
                "<meta property=\"og:url\" content=\"%s\">"
                "<meta property=\"og:title\" content=\"%s\">"
                "<meta property=\"og:description\" content=\"%s\">"
                "<meta property=\"og:image\" content=\"%s\">"
                "<meta property=\"og:image:width\" content=\"%d\">"
                "<meta property=\"og:image:height\" content=\"%d\">"
                "<meta name=\"twitter:card\" content=\"summary_large_image\">"
                "<meta name=\"twitter:title\" content=\"%s\">"
                "<meta name=\"twitter:description\" content=\"%s\">"
                "<meta name=\"twitter:image\" content=\"%s\">"
                % (canonical, fedi_profile,
                   fediverse_origin, quote(owner), quote(repo),
                   site_name, canonical, title, _html_escape(og_description), og_image,
                   og_card.CARD_W, og_card.CARD_H,
                   title, _html_escape(og_description), og_image))
            body = body.replace("</head>", tags + "</head>", 1)
        if short_name and owner and "</head>" in body:
            short_metas = (
                '<meta name="forkmesh-repo-short" content="%s">'
                '<meta name="forkmesh-repo-owner" content="%s">'
                "%s" % (
                    _html_escape(short_name), _html_escape(owner),
                    '<meta name="forkmesh-repo-canonical" content="%s">'
                    % _html_escape(owner) if canonical_short else ""))
            if "<head>" in body:
                body = body.replace("<head>", "<head>" + short_metas, 1)
            else:
                body = body.replace("</head>", short_metas + "</head>", 1)
        status = 200
        if short_name and owner and repo and not explicit_public:
            try:
                visible = await _repository_access_context(
                    self.env, request, data_owner, repo)
            except Exception:
                visible = True
            if visible is None:
                status = 404
        page = Response(body, status=status, headers={
            "content-type": "text/html; charset=utf-8",
            "cache-control": (
                "public, max-age=300" if cache_key
                else "no-store, max-age=0, must-revalidate"),
        })
        if cache_key:
            await edge_cache_put(cache_key, page)
        return page

    async def _serve_repo_profile_page(self, url, owner, repo):
        # The repo actor's `url`: a lightweight, edge-cached fediverse profile —
        # banner, avatar, bio, follower count and a feed of the repo's federated
        # posts — so a Mastodon user who clicks the repo handle (in a mention or
        # via the profile's external-link) lands on a social profile instead of
        # the git-forge page (adhoc #50). The code is one click away. Fediverse
        # clients that send an ActivityPub Accept header are handed the actor
        # document by the caller; this is the HTML view for browsers.
        origin = _ap_origin(self.env)
        handle = ap.repo_handle(owner.lower(), repo.lower())
        await ensure_schema(self.env)
        if (not await _ap_enabled(self.env)
                or not await _ap_repo_federates(self.env, owner, repo)):
            return await self._serve_not_found_page(url)
        data_owner = await _ap_org_alias_owner(
            self.env, owner, repo)
        cache_key = "%s/@%s" % (origin, handle)
        cached = await edge_cache_match(cache_key)
        if cached is not None:
            return cached
        key_bi = await blind_index(self.env, data_owner + "/" + repo)
        repo_row = await d1_first(
            self.env,
            "SELECT is_private, data FROM repositories WHERE key_bi=?", key_bi)
        if not repo_row:
            return await self._serve_not_found_page(url)
        rec = await decrypt_row(self.env, repo_row.get("data"))
        if (
            not rec
            or rec.get("visibility") != "public"
            or not _catalog_record_matches_identity(
                rec, data_owner, repo)
        ):
            return await self._serve_not_found_page(url)
        description = clean_string(
            (rec or {}).get("description", "") or "", 240).strip()
        # Branding: owner-uploaded logo/banner (repo About tab), else the
        # repo's own committed artwork. The avatar keeps a brand default; the
        # header does not, because a ForkMesh-branded banner on an unrelated
        # project reads as the project being ours.
        icon_url = AP_AVATAR_PATH
        image_url = ""
        media_rows = await d1_all(
            self.env, "SELECT kind, updated_at FROM repo_media WHERE repo_bi=?",
            key_bi)
        for media in media_rows or []:
            media_url = "/api/repo/%s/%s/media/%s.png?v=%d" % (
                quote(owner), quote(repo), media.get("kind", ""),
                int(media.get("updated_at") or 0))
            if media.get("kind") == "logo":
                icon_url = media_url
            elif media.get("kind") == "banner":
                image_url = media_url
        if not image_url:
            banner_resolver = globals().get(
                "_committed_repository_banner_url")
            image_url = (
                banner_resolver(rec, owner, repo)
                if callable(banner_resolver) else ""
            )
        if icon_url == AP_AVATAR_PATH:
            logo_version = _repository_social_version(rec)
            icon_url = "/api/repo/%s/%s/logo?image=1" % (
                quote(owner), quote(repo))
            if logo_version:
                icon_url += "&v=" + quote(logo_version, safe="")
        actor_bi = await _ap_actor_bi(self.env, AP_ACTOR_REPO, handle)
        followers_row = await d1_first(
            self.env, "SELECT COUNT(*) AS c FROM ap_followers WHERE actor_bi=?",
            actor_bi)
        followers = int((followers_row or {}).get("c") or 0)
        post_rows = await d1_all(
            self.env,
            "SELECT data, published FROM ap_objects WHERE actor_bi=?"
            " ORDER BY published DESC LIMIT 20", actor_bi)
        posts = []
        for row in post_rows or []:
            note_rec = await decrypt_row(self.env, row.get("data"))
            note = (note_rec or {}).get("note") or {}
            content = note.get("content") or ""  # our own generated, safe HTML
            if not content:
                continue
            when = ap.iso_utc(int(row.get("published") or 0))[:10]
            link = str(note.get("url") or "")
            link_html = ("<a class=\"lnk\" href=\"%s\">View →</a>"
                         % _html_escape(link)) if link else ""
            posts.append(
                "<article class=\"post\"><div class=\"body\">%s</div>"
                "<div class=\"meta\"><time>%s</time>%s</div></article>"
                % (content, _html_escape(when), link_html))
        if not posts:
            posts.append(
                "<p class=\"empty\">No federated posts yet — new issues, "
                "pull requests, discussions and releases will appear here.</p>")
        title = _html_escape("%s/%s" % (owner, repo))
        handle_full = _html_escape("@%s@%s" % (handle, _ap_domain_of(origin)))
        code_url = _html_escape(origin + repo_web_href(owner, repo))
        actor_json = _html_escape(
            "%s/ap/repos/%s/%s" % (origin, quote(owner), quote(repo)))
        bio = (_html_escape(description) if description else
               instance_features.rename_html(
                   self.env, "ForkMesh repository — follow for new issues, "
                   "pull requests, discussions and releases."))
        body = _repo_fedi_profile_html(
            title=title, bio=bio, handle_full=handle_full, code_url=code_url,
            actor_json=actor_json, icon_url=_html_escape(icon_url),
            image_url=_html_escape(image_url), followers=followers,
            post_count=len(post_rows or []), posts_html="\n".join(posts))
        body = body.replace(
            " on the fediverse · ForkMesh</title>",
            " on the fediverse · %s</title>" % _html_escape(
                instance_features.display_name(self.env)), 1)
        page = Response(body, status=200, headers={
            "content-type": "text/html; charset=utf-8",
            "cache-control": "public, max-age=300",
        })
        await edge_cache_put(cache_key, page)
        return page

    async def _serve_dashboard_asset(self, url, asset_rel):
        # The per-page dashboard documents are generated by
        # tools/build_dashboard_assets.py. env.ASSETS.fetch serves the static
        # file directly from the assets binding, avoiding Pyodide CPU/GIL
        # pressure on hot dashboard navigation.
        base = url.scheme + "://" + url.netloc + "/"
        try:
            resp = await self.env.ASSETS.fetch(base + asset_rel)
            body = await resp.text()
        except Exception:
            body = "<!doctype html><title>ForkMesh Dashboard</title>"
        return Response(body, status=200, headers={
            "content-type": "text/html; charset=utf-8",
            "cache-control": "public, max-age=300",
        })

    async def _serve_homepage(self, url):
        # Root remains the regular product/source-code website for guests and
        # signed-in visitors alike. It embeds the World in a bounded iframe and
        # links to /world/ for full-screen play. no-cache permits ETag
        # revalidation without pinning an old shell after deploy.
        base = url.scheme + "://" + url.netloc + "/"
        # Stream the asset body rather than buffering and rebuilding the
        # highest-traffic document in the Python isolate.
        try:
            resp = await self.env.ASSETS.fetch(base + "index.html")
            if int(getattr(resp, "status", 0) or 0) != 200:
                raise RuntimeError("homepage asset unavailable")
            return JsResponse.new(resp.body, to_js({
                "status": 200,
                "headers": {
                    "content-type": "text/html; charset=utf-8",
                    "cache-control": "no-cache",
                },
            }))
        except Exception:
            return Response(
                instance_features.rename_html(
                    self.env, "<!doctype html><title>ForkMesh unavailable</title>"),
                status=503,
                headers={
                    "content-type": "text/html; charset=utf-8",
                    "cache-control": "no-store, max-age=0, must-revalidate",
                },
            )

    async def _serve_signup_page(self, request, url):
        # A page view is the capability boundary for password-user creation.
        # Keep the claim server-side and the signed handle in an HttpOnly
        # host-only cookie scoped to the one endpoint that consumes it.
        await ensure_schema(self.env)
        referral_code_required = await _admin_signup_referral_code_mode(
            self.env)
        claim_id, issued_at, expires_at, email_field_name = (
            await _issue_account_signup_intent(self.env))
        base = url.scheme + "://" + url.netloc + "/"
        try:
            resp = await self.env.ASSETS.fetch(base + "signup.html")
            if int(getattr(resp, "status", 0) or 0) != 200:
                raise RuntimeError("signup asset unavailable")
            body = str(await resp.text())
            marker = "__FORKMESH_SIGNUP_EMAIL_FIELD__"
            if body.count(marker) != 1:
                raise RuntimeError("signup email marker unavailable")
            body = body.replace(marker, email_field_name, 1)
            code_marker = "__FORKMESH_SIGNUP_REFERRAL_CODE_VISIBILITY__"
            if body.count(code_marker) != 1:
                raise RuntimeError("signup referral code marker unavailable")
            body = body.replace(
                code_marker, "" if referral_code_required else "hidden", 1)
            # Turnstile widget (turnstile.py): the public site key, and cdata
            # that ties the token to this page view's signup claim.
            for widget_marker, widget_value in (
                    ("__FORKMESH_TURNSTILE_SITE_KEY__",
                     _turnstile.site_key(self.env)),
                    ("__FORKMESH_TURNSTILE_CDATA__",
                     _turnstile.signup_cdata(claim_id))):
                if body.count(widget_marker) != 1:
                    raise RuntimeError("signup turnstile marker unavailable")
                body = body.replace(
                    widget_marker, _html_escape(widget_value), 1)
            return Response(body, status=200, headers={
                "content-type": "text/html; charset=utf-8",
                "cache-control": "no-store, max-age=0, must-revalidate",
                "referrer-policy": "strict-origin-when-cross-origin",
                "x-content-type-options": "nosniff",
                "set-cookie": _account_signup_intent_cookie(
                    self.env, claim_id, issued_at, expires_at,
                    secure=(url.scheme == "https")),
            })
        except Exception:
            return Response(
                instance_features.rename_html(
                    self.env,
                    "<!doctype html><title>ForkMesh signup unavailable</title>"),
                status=503,
                headers={
                    "content-type": "text/html; charset=utf-8",
                    "cache-control": "no-store, max-age=0, must-revalidate",
                },
            )

    async def _git_host(self, request, owner_raw, repo_raw):
        # Legacy compatibility boundary. Public clone traffic never reaches
        # this method; authorized private Git clients are directed to the
        # owner-sealed archive transport without consuming their request body.
        owner = safe_segment(owner_raw)
        repo = safe_segment(repo_raw)
        if not owner or not repo:
            return Response("not found", status=404)
        url = urlparse(request.url)
        is_info = url.path.endswith("/info/refs")
        # Named Git URLs never advertise or proxy private content. Authorized
        # private clients discover an opaque locator in their ACL-gated catalog
        # and use the dedicated identity-free HTTPS endpoint.
        repo_private = await _repo_is_private(self.env, owner, repo)
        operation = "git-info-refs" if is_info else "git-upload-pack"
        if repo_private:
            # A private repository the core nodes host is cloned over this
            # named URL with the owner's hosted credential (hosted_git.py).
            hosted_read = globals().get("hosted_private_read")
            hosted = (
                await hosted_read(self.env, request, owner, repo, operation)
                if callable(hosted_read) else None)
            if hosted is not None:
                return hosted
            return _private_replica_not_found()
        return await _https_mirror_proxy(
            self.env, request, owner, repo, operation)

    async def _git_push(self, request, owner_raw, repo_raw):
        # Direct HTTPS receive-pack (mobile-node-contract.md section 7): the
        # signed device headers are the only authorization, there is no
        # anonymous path, and repository bytes stream through the masked
        # mirror-gateway proxy exactly like clone traffic.
        owner = safe_segment(owner_raw)
        repo = safe_segment(repo_raw)
        if not owner or not repo:
            return Response("not found", status=404)
        refusal = await read_only_mirror_push_refusal(self.env, owner, repo)
        if refusal is not None:
            return refusal
        url = urlparse(request.url)
        is_info = url.path.endswith("/info/refs")
        return await _https_mirror_push_proxy(
            self.env, request, owner, repo, is_info)
