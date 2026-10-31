import ast
import asyncio
import hashlib
import hmac
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse


APP = Path(__file__).resolve().parents[1]
ENTRY = APP / "src/entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
sys.path.insert(0, str(APP / "src"))
import public_snapshots as snapshot_contract  # noqa: E402
import catalog as catalog_contract  # noqa: E402
import static_routes as routing_contract  # noqa: E402
from _maintenance_stubs import with_maintenance_off
FINGERPRINT = "a" * 64
SNAPSHOT_PATHS = (
    "/api/repositories",
    "/api/network/overview",
    "/api/repo/forkmesh/forkmesh/mirrors",
    "/api/status",
    "/api/status/world",
)


def _valid_snapshot_payload(path, now):
    if path == "/api/repositories":
        return {
            "ok": True,
            "repositories": [],
            "sshGatewayConfigured": False,
            "sshTransport": "not-configured",
            "payoutCustody": "external",
            "payoutNotice": "notice",
        }
    if path == "/api/network/overview":
        return {
            "ok": True,
            "stats": {"ok": True},
            "leaderboards": {"ok": True},
            "history": {"ok": True},
        }
    if path == "/api/repo/forkmesh/forkmesh/mirrors":
        return {
            "ok": True,
            "owner": "forkmesh",
            "repo": "forkmesh",
            "groupKey": "root",
            "generatedAt": now,
            "summary": {
                "mirrors": 0,
                "online": 0,
                "cloneAvailable": 0,
                "dataHostedBytes": 0,
                "longestHostedSince": None,
                "freshestSync": None,
            },
            "mirrors": [],
            "sourceOfTruth": {
                "owner": "forkmesh",
                "repo": "forkmesh",
                "kind": "working-copy",
                "isSourceOfTruth": True,
            },
        }
    if path in ("/api/status", "/api/status/world"):
        return {"ok": True, "now": now, "systems": [], "current": {}}
    raise AssertionError("unknown snapshot path: " + path)


class Headers:
    def __init__(self, values=None):
        self.values = {}
        if isinstance(values, Headers):
            values = values.items()
        if isinstance(values, dict):
            values = values.items()
        for name, value in values or ():
            self.set(name, value)

    def get(self, name, default=None):
        return self.values.get(str(name).lower(), default)

    def has(self, name):
        return str(name).lower() in self.values

    def set(self, name, value):
        self.values[str(name).lower()] = str(value)

    def __setitem__(self, name, value):
        self.set(name, value)

    def items(self):
        return self.values.items()


class Response:
    def __init__(self, body=None, *, status=200, status_text="", headers=None):
        if isinstance(body, Response):
            self.body = body.body
            self.status = body.status
            self.status_text = body.status_text
            self.headers = Headers(body.headers)
            return
        self.body = body
        self.status = status
        self.status_text = status_text
        self.headers = Headers(headers)


class Request:
    def __init__(self, url, method="GET", headers=None, body=None):
        if isinstance(url, Request):
            source = url
            self.url = source.url
            self.method = source.method
            self.headers = Headers(source.headers)
            self.body = source.body
            return
        self.url = url
        self.method = method
        self.headers = Headers(headers)
        self.body = body


class _JsJSON:
    @staticmethod
    def stringify(value):
        return json.dumps(value, separators=(",", ":"))


class _JsURL:
    @staticmethod
    def new(value):
        parsed = urlparse(str(value))
        if not parsed.scheme or not parsed.hostname:
            raise ValueError("absolute URL required")
        default_port = (
            parsed.scheme == "https" and parsed.port in (None, 443)
        ) or (
            parsed.scheme == "http" and parsed.port in (None, 80)
        )
        host = parsed.hostname.lower()
        if ":" in host and not host.startswith("["):
            host = "[" + host + "]"
        authority = host if default_port else host + ":" + str(parsed.port)
        return SimpleNamespace(
            protocol=parsed.scheme.lower() + ":",
            hostname=parsed.hostname.lower(),
            port="" if default_port else str(parsed.port),
            username=parsed.username or "",
            password=parsed.password or "",
            pathname=parsed.path or "/",
            search=("?" + parsed.query) if parsed.query else "",
            hash=("#" + parsed.fragment) if parsed.fragment else "",
            origin=parsed.scheme.lower() + "://" + authority,
        )


def _method_nodes(*names):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    # The retired-transport constants come from the module itself so the fast
    # path is exercised against the exact payload production returns.
    constants = {
        "EXPECTED_DEGRADED_HEADER",
        "PRODUCT_ANALYSIS_PATH",
        "REPOSITORY_SOCKET_RETIRED_PAYLOAD",
        "REPOSITORY_SOCKET_RETIRED_CACHE_CONTROL",
    }
    helpers = [
        node for node in tree.body
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in {
                "_exact_public_snapshot_integer",
                "_strict_public_snapshot_json",
                "_app_ingress_shard",
                "_app_ingress_request",
                "_app_ingress_authorization",
                "_app_ingress_fetch",
                "_git_clone_http_request",
                "_mirror_node_http_request",
                "_package_api_http_request",
                "_repository_projection_http_request",
                "_serialized_http_request",
                "_node_events_direct_fetch",
                "_node_events_do_name",
                "_public_websocket_path",
                "_response_is_expected_degraded",
                "_retired_repository_socket_path",
                "uncached_status_response",
            }
        ) or (
            isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Name) and target.id in constants
                for target in node.targets
            )
        )
    ]
    default = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
    )
    selected = [
        node for node in default.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    class_fields = {
        "_PUBLIC_SNAPSHOT_SCHEMA_VERSION",
        "_PUBLIC_SNAPSHOT_KIND",
        "_PUBLIC_SNAPSHOT_TOMBSTONE_KIND",
        "_PUBLIC_SNAPSHOT_MAX_AGE",
        "_PUBLIC_SNAPSHOT_MAX_BYTES",
    }
    assignments = [
        node for node in default.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id in class_fields
            for target in node.targets
        )
    ]
    assert {
        target.id
        for node in assignments
        for target in node.targets
        if isinstance(target, ast.Name)
    } == class_fields
    class_node = ast.ClassDef(
        name="Default",
        bases=[],
        keywords=[],
        body=[*assignments, *selected],
        decorator_list=[],
    )
    return ast.fix_missing_locations(ast.Module(
        body=[*helpers, class_node], type_ignores=[]
    ))


def _snapshot_default():
    now = 1_784_000_000_000

    class Date:
        @staticmethod
        def now():
            return now

    async def snapshot_row(env, _path):
        if isinstance(env.row, BaseException):
            raise env.row
        return env.row

    async def router_status(_env):
        return "A" * 43, True

    async def world_deploy_status(_env, request):
        default = holder["default"]
        default.deploy_status_calls.append(request)
        if default.deploy_status_error is not None:
            raise default.deploy_status_error
        return Response(
            json.dumps({
                "ok": True,
                "state": "idle",
                "revision": "release-rev",
                "startedAt": 0,
                "finishedAt": 0,
                "now": Date.now(),
            }),
            headers={
                "content-type": "application/json; charset=utf-8",
                "cache-control": "no-store, max-age=0",
            },
        )

    def json_response(payload, status=200, cache_control="", extra_headers=None):
        headers = {"content-type": "application/json; charset=utf-8"}
        if cache_control:
            headers["cache-control"] = cache_control
        headers.update(extra_headers or {})
        return Response(json.dumps(payload), status=status, headers=headers)

    holder = {}

    async def edge_status_cache_match(cache_key):
        default = holder["default"]
        default.status_cache_calls.append(cache_key)
        return default.status_cache.get(cache_key)

    async def websocket_direct_fetch(_env, request):
        default = holder["default"]
        default.websocket_calls.append(request)
        if str(request.headers.get("upgrade") or "").lower() != "websocket":
            return Response('{"error":"not_found"}', status=404)
        if not re.fullmatch(
                r"/api/(?:world(?:/.*)?|chat(?:/.*)?|notes(?:/.*)?|"
                r"repo(?:/.*)?|room(?:/.*)?)",
                urlparse(request.url).path):
            return Response('{"error":"not_found"}', status=404)
        return default.websocket_response

    async def durable_request(request):
        headers = {}
        for name in (
            "upgrade", "connection", "sec-websocket-version",
            "sec-websocket-key", "sec-websocket-protocol", "content-type",
            "content-encoding", "authorization", "accept", "user-agent",
        ):
            value = request.headers.get(name)
            if value:
                headers[name] = value
        return Request(request.url, method=request.method, headers=headers)

    async def log_abort(env, request, path, error):
        holder["default"].abort_logs.append((env, request, path, error))

    async def log_d1_unavailable(env, request, path, error):
        holder["default"].d1_unavailable_logs.append(
            (env, request, path, error)
        )

    namespace = {
        "APP_INGRESS_NAME_PREFIX": "app-ingress-v1:",
        "APP_INGRESS_HTTP_PURPOSE": "v1",
        "APP_GIT_INGRESS_NAME_PREFIX": "app-git-ingress-v1:",
        "APP_INGRESS_GIT_READ_PURPOSE": "git-read-v1",
        "APP_INGRESS_NODE_AUTH_PURPOSE": "node-events-auth-v1",
        "_AppIngressRouter": lambda env: SimpleNamespace(env=env),
        "JsJSON": _JsJSON,
        "JsRequest": _JsRequest,
        "JsURL": _JsURL,
        "Date": Date,
        "EXPECTED_DEGRADED_HEADERS": {"x-forkmesh-expected-degraded": "1"},
        "STATUS_EDGE_CACHE_KEYS": {
            "full": "https://forkmesh.internal/api/status/full-v1",
            "world": "https://forkmesh.internal/api/status/world-v1",
        },
        "CHAT_CHANNEL_WS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/ws/?$"),
        "CHAT_DIRECT_MESSAGE_WS_RE": re.compile(
            r"^/api/chat/direct-messages/([0-9a-f]{32})/ws/?$"),
        "Response": Response,
        "OFFICE_CHANNEL_WS_RE": re.compile(
            r"^/api/world/office/channels/([0-9a-f]{32})/ws/?$"),
        "REPO_ROOM_RE": re.compile(
            r"^/api/repo/([^/]+)/([^/]+)/rooms/([^/]+)/(?:ws|clients)$"),
        "ROOM_RE": re.compile(r"^/api/room/([^/]+)/(?:ws|clients)$"),
        "_CORS_CANONICAL_FRONTEND_ORIGINS": (
            "https://forkmesh.com", "https://www.forkmesh.com",
            "https://app.forkmesh.com", "https://world.forkmesh.com",
        ),
        "_CORS_EXPOSE_HEADERS": (
            "x-forkmesh-worker", "x-forkmesh-public-snapshot",
            "x-forkmesh-snapshot-generated-at",
            "x-forkmesh-snapshot-fingerprint",
        ),
        "_CORS_ORIGIN_ENV_NAMES": (
            "PUBLIC_BASE_URL", "API_ORIGIN", "APP_ORIGIN", "WWW_ORIGIN",
            "WORLD_ORIGIN",
        ),
        "_build_rev": lambda env: env.BUILD_REV,
        "durable_object_request": durable_request,
        "edge_cache_match": edge_status_cache_match,
        "_https_mirror_router_public_key": lambda env: (
            env.MIRROR_ROUTER_PUBLIC_KEY),
        "_https_mirror_router_identity_status": router_status,
        "_is_d1_platform_error": lambda error: (
            "d1_error" in str(error).lower()
        ),
        "_merge_vary": lambda *values: ", ".join(dict.fromkeys(
            token.strip()
            for value in values
            for token in str(value or "").split(",")
            if token.strip()
        )),
        "_normalized_http_origin": lambda value: str(value or "").rstrip("/"),
        "_public_snapshot_row": snapshot_row,
        "_request_target_origin": lambda request: (
            f"{urlparse(request.url).scheme}://{urlparse(request.url).netloc}"
        ),
        "_websocket_direct_fetch": websocket_direct_fetch,
        "json": json,
        "json_response": json_response,
        "log_durable_object_abort": log_abort,
        "log_d1_unavailable": log_d1_unavailable,
        "method_name": lambda request: request.method,
        "parse_qs": parse_qs,
        # The generated registry is the only place package API prefixes
        # come from; core source names no package, so neither does this
        # fixture.
        "plugin_api_prefixes": lambda: ("/api/demo-package",),
        "public_snapshots": snapshot_contract,
        "re": re,
        "safe_segment": lambda value: (
            str(value or "").strip().lower()
            if re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}",
                            str(value or "").strip().lower())
            else ""
        ),
        "to_js": lambda value: value,
        "urlparse": urlparse,
        "world_deploy_status_handler": world_deploy_status,
    }
    methods = (
        "_control_response",
        "_without_head_body",
        "_native_json_response",
        "_fast_normalized_http_origin",
        "_fast_cors_headers",
        "_with_fast_cors",
        "_mirror_identity_control_response",
        "_world_online_control_response",
        "_health_control_response",
        "_version_control_response",
        "_request_has_header",
        "_explicit_live_read",
        "_public_snapshot_path",
        "_stateless_fast_response",
        "_invalid_public_snapshot",
        "_public_snapshot_live_fallback",
        "_public_snapshot_response",
        "_stateless_status_snapshot_response",
        "fetch",
    )
    exec(compile(_method_nodes(*methods), str(ENTRY), "exec"), with_maintenance_off(namespace))

    async def direct_fetch_inner(_router, request):
        default = holder["default"]
        default.direct_calls.append(request)
        if default.direct_handler is not None:
            return await default.direct_handler(request)
        return default.ingress_response

    # The production method is deliberately not copied into this small AST
    # fixture; bind its routing seam so _app_ingress_fetch can exercise the
    # stateless dispatch without constructing the full application router.
    namespace["AppRuntime"] = namespace["Default"]
    namespace["AppRuntime"]._fetch_inner = direct_fetch_inner
    default = namespace["Default"]()
    holder["default"] = default
    default._inside_app_ingress = True
    default.env = SimpleNamespace(
        APP_ORIGIN="https://app.forkmesh.com",
        APP_VERSION="1.2.3",
        API_ORIGIN="https://app.forkmesh.com",
        BUILD_REV="release-rev",
        CORS_ALLOWED_ORIGINS="",
        DEPLOY_FINGERPRINT=FINGERPRINT,
        MIRROR_ROUTER_PUBLIC_KEY="A" * 43,
        NODE_NAME="forkmesh-mainnode",
        NODE_SOLANA_ADDRESS="",
        PUBLIC_BASE_URL="https://forkmesh.com",
        SINGLE_WORKER_SITE="false",
        WORKER_ROLE="app",
        WORLD_ORIGIN="https://world.forkmesh.com",
        WWW_ORIGIN="https://forkmesh.com",
        row=None,
    )
    default.route_calls = []
    default.status_cache = {}
    default.status_cache_calls = []
    default.abort_logs = []
    default.direct_calls = []
    default.direct_handler = None
    default.ingress_calls = []
    default.ingress_handler = None
    default.ingress_response = Response(
        '{"live":true}',
        status=209,
        headers={
            "content-type": "application/json",
            "set-cookie": "private=never-forward",
        },
    )
    default.ingress_auth_response = Response(
        None,
        status=204,
        headers={"x-forkmesh-authorized-owner": "mirror9"},
    )
    default.ingress_names = []
    default.node_names = []
    default.node_calls = []
    default.node_response = Response(
        None, status=101, headers={"upgrade": "websocket"})
    default.websocket_calls = []
    default.websocket_response = Response(
        None, status=101, headers={"upgrade": "websocket"})
    default.world_calls = []
    default.deploy_status_calls = []
    default.deploy_status_error = None
    default.d1_unavailable_logs = []

    class IngressBinding:
        def idFromName(self, name):
            default.ingress_names.append(name)
            return name

        def get(self, _object_id):
            class Stub:
                async def fetch(self, request):
                    default.ingress_calls.append(request)
                    purpose = request.headers.get(
                        "x-forkmesh-internal-ingress")
                    if purpose == "node-events-auth-v1":
                        return default.ingress_auth_response
                    assert purpose in ("v1", "git-read-v1")
                    if default.ingress_handler is not None:
                        return await default.ingress_handler(request)
                    return default.ingress_response

            return Stub()

    class NodesBinding:
        def idFromName(self, name):
            default.node_names.append(name)
            return name

        def get(self, _object_id):
            class Stub:
                async def fetch(self, request):
                    default.node_calls.append(request)
                    return default.node_response

            return Stub()

    class WorldBinding:
        def idFromName(self, name):
            assert name == "town-square-v1"
            return name

        def get(self, _object_id):
            class Stub:
                async def fetch(self, request):
                    default.world_calls.append(request)
                    return Response(
                        '{"ok":true,"online":3}',
                        headers={
                            "content-type": "application/json",
                            "cache-control": "public, max-age=10",
                        },
                    )

            return Stub()

    default.env.FORKMESH_CRON_RUNNER = IngressBinding()
    default.env.FORKMESH_NODES = NodesBinding()
    default.env.FORKMESH_WORLD = WorldBinding()

    async def route(request, _url):
        default.route_calls.append(request)
        return Response(
            '{"live":true}',
            status=209,
            headers={
                "content-type": "application/json",
                "set-cookie": "private=never-forward",
            },
        )

    default._route = route
    return default, now


def _row(path, now, **overrides):
    body = overrides.pop(
        "payload",
        json.dumps(_valid_snapshot_payload(path, now), separators=(",", ":")),
    )
    try:
        json.loads(
            body,
            parse_constant=lambda value: (_ for _ in ()).throw(
                ValueError("invalid JSON constant: " + value)
            ),
        )
        payload_json_valid = 1
    except (TypeError, ValueError):
        payload_json_valid = 0
    payload_bytes = len(body.encode("utf-8")) if isinstance(body, str) else 0
    result = {
        "path": path,
        "schema_version": 1,
        "kind": "forkmesh-public-api-snapshot",
        "generated_at": now,
        "fresh_until": now + 60_000,
        "stale_until": now + 60_000,
        "deploy_fingerprint": FINGERPRINT,
        "content_length": payload_bytes,
        "payload_bytes": payload_bytes,
        "payload_json_valid": payload_json_valid,
        "payload": body,
    }
    result.update(overrides)
    return result


def _control_default():
    class Date:
        @staticmethod
        def now():
            return 1_784_000_000_000

    def json_response(payload):
        return Response(
            json.dumps(payload, separators=(",", ":")),
            headers={"content-type": "application/json; charset=utf-8"},
        )

    async def mirror_handler(_env, _request):
        return Response(json.dumps({"ok": True, "routerPublicKey": "A" * 43}))

    async def router_status(_env):
        return "A" * 43, True

    namespace = {
        "Date": Date,
        "JsJSON": _JsJSON,
        "Response": Response,
        "_build_rev": lambda env: env.BUILD_REV,
        "_https_mirror_router_identity_status": router_status,
        "_https_mirror_router_public_key": lambda env: (
            env.MIRROR_ROUTER_PUBLIC_KEY),
        "_normalized_http_origin": lambda value: str(value or "").rstrip("/"),
        "_request_target_origin": lambda request: (
            f"{urlparse(request.url).scheme}://{urlparse(request.url).netloc}"
        ),
        "external_site_route": routing_contract.external_site_route,
        "https_mirror_endpoint_handler": mirror_handler,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "to_js": lambda value: value,
    }
    methods = (
        "_control_response",
        "_without_head_body",
        "_native_json_response",
        "_mirror_identity_control_response",
        "_app_root_redirect",
        "_world_ingress_redirect",
        "_health_control_response",
        "_version_control_response",
    )
    exec(compile(_method_nodes(*methods), str(ENTRY), "exec"), with_maintenance_off(namespace))
    default = namespace["Default"]()
    default.env = SimpleNamespace(
        APP_ORIGIN="https://app.forkmesh.com",
        APP_VERSION="1.2.3",
        BUILD_REV="release-rev",
        DEPLOY_FINGERPRINT=FINGERPRINT,
        MIRROR_ROUTER_PUBLIC_KEY="A" * 43,
        NODE_NAME="forkmesh-mainnode",
        NODE_SOLANA_ADDRESS="",
        WORLD_ORIGIN="https://world.forkmesh.com",
        WORKER_ROLE="app",
        WWW_ORIGIN="https://forkmesh.com",
    )
    return default


def _reason(response):
    return json.loads(response.body)["reason"]


def test_direct_app_snapshot_fresh_fallback_tombstone_and_corrupt_matrix():
    default, now = _snapshot_default()
    for path in SNAPSHOT_PATHS:
        default.env.row = _row(path, now)
        request = Request("https://app.forkmesh.com" + path)
        response = asyncio.run(default._public_snapshot_response(
            request, urlparse(request.url), path
        ))
        assert response.status == 200
        assert response.headers.get("x-forkmesh-worker") == "app"
        assert response.headers.get("x-forkmesh-public-snapshot") == "fresh"
        assert response.headers.get("x-forkmesh-snapshot-fingerprint") == FINGERPRINT
        assert "must-revalidate" in response.headers.get("cache-control")
        if path in snapshot_contract.SNAPSHOT_STATUS_PATHS:
            assert "no-store" in response.headers.get("cache-control")
            assert response.headers.get("cdn-cache-control") == "no-store"
            assert response.headers.get(
                "cloudflare-cdn-cache-control") == "no-store"

    path = SNAPSHOT_PATHS[0]
    request = Request("https://app.forkmesh.com" + path)
    default.env.row = None
    missing = asyncio.run(default._public_snapshot_response(
        request, urlparse(request.url), path
    ))
    assert missing.status == 209
    assert missing.headers.get("x-forkmesh-public-snapshot") == "missing-fallback"
    assert missing.headers.get("set-cookie") is None
    assert "no-store" in missing.headers.get("cache-control")

    default.env.row = _row(path, now, deploy_fingerprint="b" * 64)
    mismatch = asyncio.run(default._public_snapshot_response(
        request, urlparse(request.url), path
    ))
    assert mismatch.status == 209
    assert mismatch.headers.get("x-forkmesh-public-snapshot") == "fingerprint-fallback"

    default.env.row = _row(
        path, now, generated_at=now - 120_000,
        fresh_until=now - 1, stale_until=now - 1,
    )
    expired = asyncio.run(default._public_snapshot_response(
        request, urlparse(request.url), path
    ))
    assert expired.status == 209
    assert expired.headers.get("x-forkmesh-public-snapshot") == "expired-fallback"

    failures = (
        (_row(path, now, schema_version="corrupt"), "invalid-or-expired"),
        (_row(path, now, schema_version="1"), "invalid-or-expired"),
        (_row(path, now, schema_version=True), "invalid-or-expired"),
        (_row(path, now, schema_version=1.9), "invalid-or-expired"),
        (_row(path, now, generated_at=float("nan")), "invalid-or-expired"),
        (_row(path, now, generated_at=float("inf")), "invalid-or-expired"),
        (_row(path, now, content_length=12.9), "invalid-or-expired"),
        (_row(path, now, content_length="11"), "invalid-or-expired"),
        (_row(path, now, fresh_until=now + 60_000.5), "invalid-or-expired"),
        (_row(path, now, stale_until=now + 60_000.5), "invalid-or-expired"),
        (_row(path, now, kind="forkmesh-public-api-snapshot-tombstone",
              payload="{}", content_length=2), "privacy-transition"),
        (_row(path, now, kind="forkmesh-public-api-snapshot-tombstone",
              payload='{"ok":true}'), "invalid-barrier"),
        (_row(path, now, deploy_fingerprint="not-a-fingerprint"), "invalid-fingerprint"),
        (_row(path, now, payload="not-json", content_length=8), "invalid-or-expired"),
        (_row(path, now, payload='{"ok":true,"poison":NaN}',
              content_length=24), "invalid-or-expired"),
        (_row(path, now, payload='{"ok":true}'), "invalid-payload"),
        (_row(path, now, payload_bytes=1), "invalid-or-expired"),
        (_row(path, now, payload_json_valid=False), "invalid-or-expired"),
        (_row(path, now, generated_at=now + 60_001), "invalid-or-expired"),
    )
    for row, reason in failures:
        default.env.row = row
        response = asyncio.run(default._public_snapshot_response(
            request, urlparse(request.url), path
        ))
        assert response.status == 503
        assert _reason(response) == reason
        assert "no-store" in response.headers.get("cache-control")
    default.env.row = RuntimeError("D1 unavailable")
    assert _reason(asyncio.run(default._public_snapshot_response(
        request, urlparse(request.url), path
    ))) == "database-error"


def test_status_snapshot_stays_available_when_sampling_or_deploy_handoff_stalls():
    default, now = _snapshot_default()
    path = "/api/status"
    request = Request("https://app.forkmesh.com" + path)
    generated_at = now - 3_600_000
    default.env.row = _row(
        path,
        generated_at,
        fresh_until=now - 3_000_000,
        stale_until=now - 3_000_000,
        deploy_fingerprint="b" * 64,
    )

    response = asyncio.run(default._public_snapshot_response(
        request, urlparse(request.url), path
    ))

    assert response.status == 200
    assert response.headers.get("x-forkmesh-public-snapshot") == (
        "status-emergency-stale")
    assert response.headers.get("x-forkmesh-snapshot-fingerprint") == "b" * 64
    assert "last known status" in response.headers.get("warning")
    assert default.route_calls == []


def test_status_snapshot_stays_available_when_all_ingress_objects_fail():
    default, now = _snapshot_default()
    path = "/api/status/world"
    payload = json.dumps(
        _valid_snapshot_payload(path, now), separators=(",", ":"))
    cache_key = "https://forkmesh.internal/api/status/world-v1"
    default.status_cache[cache_key] = Response(
        payload,
        headers={
            "cache-control": "public, max-age=600",
            "content-type": "application/json; charset=utf-8",
            "x-forkmesh-public-snapshot": "fresh",
            "x-forkmesh-snapshot-generated-at": str(now),
            "x-forkmesh-snapshot-fingerprint": FINGERPRINT,
            "x-forkmesh-worker": "app",
        },
    )

    async def abort(_request):
        raise RuntimeError(
            "internal error; reference = shared-durable-object-outage")

    default.ingress_handler = abort
    request = Request(
        "https://app.forkmesh.com/api/status?view=world&ts=123",
        headers={
            "authorization": "Bearer private",
            "cache-control": "no-cache",
            "cookie": "session=browser",
            "origin": "https://world.forkmesh.com",
        },
    )

    response = asyncio.run(default.fetch(request))

    assert response.status == 200
    assert response.body == payload
    assert response.headers.get("x-forkmesh-public-snapshot") == "fresh"
    assert response.headers.get("access-control-allow-origin") == (
        "https://world.forkmesh.com")
    assert default.ingress_calls == []
    assert default.direct_calls == []
    assert default.status_cache_calls == [cache_key]

    # A delayed sampler still yields the last cached view and labels it stale;
    # it must not retry the failed ingress layer and turn recovery into a 503.
    default.status_cache[cache_key] = Response(
        payload,
        headers={
            "cache-control": "public, max-age=600",
            "content-type": "application/json; charset=utf-8",
            "x-forkmesh-public-snapshot": "fresh",
            "x-forkmesh-snapshot-generated-at": str(now - 5 * 60_000),
            "x-forkmesh-snapshot-fingerprint": FINGERPRINT,
            "x-forkmesh-worker": "app",
        },
    )
    stale = asyncio.run(default.fetch(request))
    assert stale.status == 200
    assert stale.body == payload
    assert stale.headers.get("x-forkmesh-public-snapshot") == (
        "status-emergency-stale")
    assert "last known status" in stale.headers.get("warning")
    assert default.ingress_calls == []
    assert default.direct_calls == []

    # Cache API entries survive code deployments. A body validated and written
    # by the previous release remains available while this release's sampler
    # catches up, without depending on the failed ingress layer.
    default.status_cache[cache_key].headers.set(
        "x-forkmesh-snapshot-fingerprint", "b" * 64)
    old_release = asyncio.run(default.fetch(request))
    assert old_release.status == 200
    assert old_release.body == payload
    assert old_release.headers.get("x-forkmesh-public-snapshot") == (
        "status-emergency-stale")
    assert "last known status" in old_release.headers.get("warning")
    assert default.ingress_names == []
    assert default.direct_calls == []


def test_status_snapshot_uses_database_json_validation_without_python_parse():
    default, now = _snapshot_default()
    path = "/api/status"
    request = Request("https://app.forkmesh.com" + path)
    default.env.row = _row(path, now, payload='{"ok":true}')

    response = asyncio.run(default._public_snapshot_response(
        request, urlparse(request.url), path
    ))

    assert response.status == 200
    assert response.body == '{"ok":true}'


def test_direct_app_snapshot_admission_bypass_and_cors_contract():
    default, _ = _snapshot_default()
    path = SNAPSHOT_PATHS[0]
    valid = Request("https://app.forkmesh.com" + path)
    assert default._public_snapshot_path(valid, urlparse(valid.url)) == path
    public_catalog_headers = (
        {"cookie": "session=value"},
        {"cache-control": "no-cache"},
        {"cache-control": "no-store"},
        {"cache-control": "max-age=0"},
        {"cache-control": "s-maxage=0"},
        {"pragma": "no-cache"},
        {"cookie": "session=value", "cache-control": "no-cache"},
    )
    assert all(
        default._public_snapshot_path(
            Request(valid.url, headers=headers), urlparse(valid.url)
        ) == path
        for headers in public_catalog_headers
    )
    bypasses = (
        Request(valid.url, headers={"authorization": "Bearer private"}),
        Request(valid.url + "?fresh=1"),
        Request(valid.url + "?"),
        Request(valid.url, method="POST"),
        Request(valid.url, method="HEAD"),
        Request("https://other.invalid" + path),
        Request(valid.url + "/"),
    )
    assert all(
        default._public_snapshot_path(request, urlparse(request.url)) == ""
        for request in bypasses
    )

    expose_block = ENTRY_TEXT.split("_CORS_EXPOSE_HEADERS = (", 1)[1].split(")", 1)[0]
    for header in (
        "x-forkmesh-public-snapshot",
        "x-forkmesh-snapshot-generated-at",
        "x-forkmesh-snapshot-fingerprint",
    ):
        assert header in expose_block
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    default_class = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
    )
    fetch_method = next(
        node for node in default_class.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "_fetch_inner"
    )
    fetch_block = ast.get_source_segment(ENTRY_TEXT, fetch_method)
    assert fetch_block.index("_api_cors_preflight_response") < fetch_block.index(
        "_public_snapshot_path"
    )
    assert fetch_block.index("_public_snapshot_path") < fetch_block.index("self._route")
    assert "_api_cors_response" in fetch_block


def test_status_snapshot_cannot_be_bypassed_into_live_aggregation():
    default, _ = _snapshot_default()
    cases = (
        ("/api/status?view=world", "/api/status/world"),
        ("/api/status?view=world&ts=123", "/api/status/world"),
        ("/api/status?fresh=browser-reload", "/api/status"),
        ("/api/status?unknown=value", "/api/status"),
        ("/api/status/world?ts=123", "/api/status/world"),
    )
    for suffix, expected in cases:
        request = Request(
            "https://app.forkmesh.com" + suffix,
            headers={
                "authorization": "Bearer private",
                "cache-control": "no-cache",
                "cookie": "session=value",
                "pragma": "no-cache",
            },
        )
        assert default._public_snapshot_path(
            request, urlparse(request.url)) == expected


def test_fast_controls_snapshot_ingress_and_direct_websocket_dispatch():
    default, now = _snapshot_default()

    version_request = Request(
        "https://app.forkmesh.com/api/version",
        headers={"origin": "https://forkmesh.com"},
    )
    version = asyncio.run(default.fetch(version_request))
    assert version.status == 200
    assert json.loads(version.body)["rev"] == "release-rev"
    assert version.headers.get("access-control-allow-origin") == (
        "https://forkmesh.com"
    )
    assert version.headers.get("access-control-allow-credentials") == "true"
    assert "x-forkmesh-snapshot-fingerprint" in version.headers.get(
        "access-control-expose-headers"
    )
    assert version.headers.get("vary") == "Origin"
    assert default.ingress_calls == []

    deploy_status_request = Request(
        "https://app.forkmesh.com/api/world/deploy-status",
        headers={"origin": "https://world.forkmesh.com"},
    )
    deploy_status = asyncio.run(default.fetch(deploy_status_request))
    assert deploy_status.status == 200
    assert json.loads(deploy_status.body)["state"] == "idle"
    assert deploy_status.headers.get("cache-control") == "no-store, max-age=0"
    assert deploy_status.headers.get("access-control-allow-origin") == (
        "https://world.forkmesh.com"
    )
    assert default.deploy_status_calls == [deploy_status_request]
    assert default.ingress_calls == []

    default.deploy_status_error = RuntimeError("D1_ERROR: overload")
    deploy_status_degraded = asyncio.run(default.fetch(deploy_status_request))
    assert deploy_status_degraded.status == 503
    assert json.loads(deploy_status_degraded.body) == {
        "error": "database_unavailable"
    }
    assert deploy_status_degraded.headers.get(
        "x-forkmesh-expected-degraded"
    ) == "1"
    assert deploy_status_degraded.headers.get("retry-after") == "2"
    assert deploy_status_degraded.headers.get("access-control-allow-origin") == (
        "https://world.forkmesh.com"
    )
    assert default.d1_unavailable_logs == [(
        default.env,
        deploy_status_request,
        "/api/world/deploy-status",
        default.deploy_status_error,
    )]
    assert default.ingress_calls == []
    default.deploy_status_error = None

    path = SNAPSHOT_PATHS[0]
    default.env.row = _row(path, now)

    async def serve_snapshot(request):
        parsed = urlparse(request.url)
        response = await default._public_snapshot_response(
            request, parsed, parsed.path
        )
        return default._with_fast_cors(request, response)

    default.direct_handler = serve_snapshot
    snapshot_request = Request(
        "https://app.forkmesh.com" + path,
        headers={"origin": "https://world.forkmesh.com"},
    )
    snapshot = asyncio.run(default.fetch(snapshot_request))
    assert snapshot.status == 200
    assert set(
        value.strip() for value in snapshot.headers.get("vary").split(",")
    ) == {"Cookie", "Authorization", "Origin"}
    assert snapshot.headers.get("access-control-allow-origin") == (
        "https://world.forkmesh.com"
    )
    assert len(default.direct_calls) == 1
    assert default.direct_calls[0].url == snapshot_request.url
    assert default.direct_calls[0].headers.get(
        "x-forkmesh-internal-ingress") is None

    default.env.row = _row(
        path,
        now,
        kind="forkmesh-public-api-snapshot-tombstone",
        payload="{}",
        content_length=2,
    )
    tombstone = asyncio.run(default.fetch(snapshot_request))
    assert tombstone.status == 503
    assert _reason(tombstone) == "privacy-transition"
    assert tombstone.headers.get("access-control-allow-origin") == (
        "https://world.forkmesh.com"
    )
    assert len(default.direct_calls) == 2
    assert all(
        request.url == snapshot_request.url
        for request in default.direct_calls
    )

    socket = object()
    upgrade = Response(None, status=101, headers={"upgrade": "websocket"})
    upgrade.webSocket = socket
    default.node_response = upgrade
    event_request = Request(
        "https://app.forkmesh.com/api/nodes/events?owner=mirror9",
        headers={
            "authorization": "ForkMesh-Signature signed",
            "cookie": "session=browser",
            "origin": "https://app.forkmesh.com",
            "upgrade": "websocket",
        },
    )
    delegated = asyncio.run(default.fetch(event_request))
    assert delegated is upgrade
    assert delegated.status == 101 and delegated.webSocket is socket
    assert len(default.ingress_calls) == 1
    auth_request = default.ingress_calls[-1]
    assert auth_request.url == event_request.url
    assert auth_request.headers.get("upgrade") == "websocket"
    assert auth_request.headers.get(
        "x-forkmesh-internal-ingress") == "node-events-auth-v1"
    assert default.ingress_names[-1] == "app-ingress-v1:0"
    assert default.node_names == ["nodes:mirror9"]
    assert len(default.node_calls) == 1
    assert default.node_calls[0].url == event_request.url
    assert default.node_calls[0].headers.get("upgrade") == "websocket"
    assert default.node_calls[0].headers.get(
        "x-forkmesh-internal-ingress") is None

    direct = default.websocket_response
    direct.webSocket = object()
    legacy_inner_calls = []

    async def direct_fetch_inner(request):
        legacy_inner_calls.append(request)
        return Response('{"error":"legacy_inner_called"}', status=599)

    default._fetch_inner = direct_fetch_inner
    generic_request = Request(
        "https://app.forkmesh.com/api/world/ws?ticket=short-lived",
        headers={
            "cookie": "session=browser",
            "origin": "https://world.forkmesh.com",
            "upgrade": "websocket",
        },
    )
    ingress_count = len(default.ingress_calls)
    generic = asyncio.run(default.fetch(generic_request))
    assert generic is direct
    assert default.websocket_calls == [generic_request]
    assert legacy_inner_calls == []
    assert len(default.ingress_calls) == ingress_count

    for rejected_request in (
        Request(
            "https://app.forkmesh.com/api/world/ws",
            headers={"upgrade": "h2c"},
        ),
        Request(
            "https://app.forkmesh.com/api/repositories",
            headers={"upgrade": "websocket"},
        ),
    ):
        rejected = asyncio.run(default.fetch(rejected_request))
        assert rejected.status == 426
    assert default.websocket_calls == [generic_request]
    assert legacy_inner_calls == []


def test_retired_repository_socket_answers_cacheably_without_waking_ingress():
    default, _ = _snapshot_default()
    retired_url = "https://app.forkmesh.com/api/repo/jett/forkmesh/host"

    retired = asyncio.run(default.fetch(Request(
        retired_url, headers={"origin": "https://forkmesh.com"})))
    assert retired.status == 410
    assert json.loads(retired.body) == {
        "error": "repository_socket_transport_retired",
        "httpsMirrorRequired": True,
        "socketFallback": False,
    }
    # The route is gone for good and its body depends on nothing, so a stale
    # client polling it must be able to answer itself instead of billing the
    # Worker plus the ingress object on every retry.
    assert retired.headers.get("cache-control") == "public, max-age=86400"
    assert retired.headers.get("access-control-allow-origin") == (
        "https://forkmesh.com")
    assert retired.headers.get("vary") == "Origin"
    assert default.direct_calls == []

    head = asyncio.run(default.fetch(Request(retired_url, method="HEAD")))
    assert head.status == 410
    assert not head.body
    # A cached copy earned no CORS headers here, so it must not be reused for
    # an origin that would have earned them.
    assert head.headers.get("vary") == "Origin"
    assert head.headers.get("access-control-allow-origin") is None
    assert default.direct_calls == []

    # Retirement must never look like an available socket.
    upgrade = asyncio.run(default.fetch(Request(
        retired_url, headers={"upgrade": "websocket"})))
    assert upgrade.status == 426
    assert default.websocket_calls == []
    assert default.direct_calls == []

    # Only the exact retired shape short-circuits: live HTTPS mirror actions,
    # rooms, and near-miss paths still reach the router.
    for forwarded in (
        "https://app.forkmesh.com/api/repo/jett/forkmesh/tree?path=",
        "https://app.forkmesh.com/api/repo/jett/forkmesh/host/",
        "https://app.forkmesh.com/api/repo/jett/forkmesh/rooms/general/host",
        "https://app.forkmesh.com/api/repo/jett/host",
        "https://app.forkmesh.com/api/repo//forkmesh/host",
    ):
        assert asyncio.run(default.fetch(Request(forwarded))).status == 209
    assert len(default.direct_calls) == 5

    # A session-bearing read skips the fast path and reaches the router, which
    # must answer from the same single definition rather than a second copy
    # that could drift back to an uncacheable denial.
    assert "return _repository_socket_retired()" in ENTRY_TEXT
    assert ENTRY_TEXT.count("repository_socket_transport_retired") == 1


def test_ordinary_http_does_not_depend_on_duration_exhausted_ingress_objects(
        capsys):
    default, _ = _snapshot_default()

    async def abort(_request):
        raise RuntimeError(
            "internal error; reference = 9na2i4b7prd9u609rru3ig06")

    default.ingress_handler = abort

    # An exhausted account-wide Durable Object allowance must not take down
    # ordinary HTTP. The stateless route answers without touching the failing
    # ingress binding.
    read = asyncio.run(default.fetch(Request(
        "https://app.forkmesh.com/api/repo/jett/forkmesh/tree?path=")))
    assert read.status == 209
    assert len(default.direct_calls) == 1
    assert default.ingress_calls == []
    assert default.abort_logs == []

    # Writes take the same direct path before their body crosses any second
    # execution boundary, so there is no ambiguous replay.
    write = asyncio.run(default.fetch(Request(
        "https://app.forkmesh.com/api/repositories",
        method="POST",
        headers={"content-type": "application/json"},
        body='{"name":"forkmesh"}',
    )))
    assert write.status == 209
    assert len(default.direct_calls) == 2
    assert default.ingress_calls == []

    # Bursty mirror synchronization is serialized through the existing
    # ingress shards. A node can retry; it must not consume the stateless
    # Python CPU budget and take public status down with it.
    default.ingress_handler = None
    mirror = asyncio.run(default.fetch(Request(
        "https://app.forkmesh.com/api/repositories",
        method="POST",
        headers={
            "content-type": "application/json",
            "user-agent": "ForkMesh-Mirror-Node/0.7.99",
        },
        body='{"name":"forkmesh"}',
    )))
    assert mirror.status == 209
    assert len(default.ingress_calls) == 1
    assert len(default.direct_calls) == 2
    replica = asyncio.run(default.fetch(Request(
        "https://app.forkmesh.com/api/mirror-replica",
        headers={"user-agent": "ForkMesh-Mirror-Replica/1"},
    )))
    assert replica.status == 209
    assert len(default.ingress_calls) == 2
    assert len(default.direct_calls) == 2

    # A status cache miss reads its one publisher-validated D1 row directly;
    # the endpoint that reports a Durable Object outage cannot depend on the
    # exhausted ingress object. In this fixture _fetch_inner is that direct
    # routing seam.
    for path in ("/api/status", "/api/status/world"):
        routed = asyncio.run(default.fetch(Request(
            "https://app.forkmesh.com" + path)))
        assert routed.status == 209
    assert len(default.ingress_calls) == 2
    assert len(default.direct_calls) == 4

    # Other heavy live reads share stable serialized objects. Concurrent
    # account, repository, ledger, or signed-sync work must not re-enter the
    # stateless Pyodide request loop.
    serialized_routes = (
        ("/api/ledger", "a", "GET"),
        ("/api/mainnode", "b", "GET"),
        ("/api/sync?owner=jett", "c", "GET"),
        ("/api/chat/channels/" + "1" * 32 + "/room-access", "d", "GET"),
        ("/api/chat/direct-messages/" + "2" * 32 + "/room-access", "d", "GET"),
        ("/api/repository-forks?mirror=1&owner=mirror36", "5", "GET"),
        ("/api/repo/jett/demo/issues?owner=jett&ts=123&sig=signed", "5", "GET"),
        ("/api/repo/jett/demo/pulls?owner=jett&ts=123&sig=signed", "5", "GET"),
        ("/api/repo/jett/demo/discussions?owner=jett&ts=123&sig=signed", "5", "GET"),
        ("/api/repo/jett/demo/pending?owner=jett&ts=123&sig=signed", "5", "GET"),
        ("/api/repo/jett/demo/logo", "5", "GET"),
        ("/api/repo/jett/demo/raw?path=logo.png&ref=main", "5", "GET"),
        ("/api/repo/jett/demo/star", "5", "GET"),
        ("/api/repo/jett/demo/forks", "5", "GET"),
        ("/api/repo/jett/demo/about", "5", "GET"),
            ("/api/repositories?viewer=jett&ts=123&sig=signed", "f", "GET"),
            ("/api/tasks", "3", "GET"),
            ("/api/tasks/" + "3" * 32, "3", "PATCH"),
        ("/signup", "6", "GET"),
        ("/api/accounts/login", "6", "POST"),
        ("/api/accounts/sessions", "6", "GET"),
        ("/api/accounts/users?node=jett&ts=123&sig=signed", "6", "GET"),
        ("/api/orgs/forkmesh/discord", "4", "GET"),
        ("/api/orgs/forkmesh/discord/messages?channelId=123", "4", "GET"),
        ("/api/orgs/forkmesh/discord/messages", "4", "POST"),
        ("/api/orgs/forkmesh/discord/oauth/start", "4", "GET"),
        ("/api/chat/activity", "7", "GET"),
        ("/api/chat/channels?node=jett&ts=123&sig=signed", "7", "GET"),
        ("/api/desktop-errors", "8", "GET"),
        ("/api/network/stats", "9", "GET"),
        # The founder dashboard decrypts the whole users table per report
        # view. Its own object keeps that megabyte of AES-GCM work off the
        # stateless allowance and away from every other family's isolate.
        ("/product-analysis", "1", "GET"),
        ("/product-analysis?view=users", "1", "GET"),
        ("/product-analysis", "1", "POST"),
        # Package installs, uninstall exports, and every package-owned API
        # read run on the one ingress object the core does not own.
        ("/api/plugins", "0", "GET"),
        ("/api/plugins/forkmesh/demo-package/install", "0", "POST"),
        ("/api/demo-package", "0", "GET"),
        ("/api/demo-package/forkmesh/contacts/" + "d" * 32
         + "?node=jett&ts=123&sig=signed", "0", "GET"),
    )
    for path, shard, method in serialized_routes:
        routed = asyncio.run(default.fetch(Request(
            "https://app.forkmesh.com" + path, method=method)))
        assert routed.status == 209
        assert default.ingress_names[-1] == "app-ingress-v1:" + shard
    # A clone's idempotent advertisement and upload-pack stream use one
    # repository-stable, explicitly gated Git object. They never enter the
    # stateless application router and unrelated repositories retain sixteen
    # fixed shards of throughput.
    git_requests = (
        Request(
            "https://app.forkmesh.com/forkmesh/forkmesh/info/refs"
            "?service=git-upload-pack"),
        Request(
            "https://app.forkmesh.com/forkmesh/forkmesh/git-upload-pack",
            method="POST",
            headers={"content-type": "application/x-git-upload-pack-request"},
            body="want exact-object\n",
        ),
    )
    git_names = []
    for git_request in git_requests:
        routed = asyncio.run(default.fetch(git_request))
        assert routed.status == 209
        git_names.append(default.ingress_names[-1])
        assert default.ingress_calls[-1].headers.get(
            "x-forkmesh-internal-ingress") == "git-read-v1"
    assert git_names[0] == git_names[1]
    assert re.fullmatch(r"app-git-ingress-v1:[0-9a-f]", git_names[0])

    # A receive-pack advertisement is a write flow and is never moved across
    # the clone-read boundary by a query-string lookalike.
    ingress_before_receive = len(default.ingress_calls)
    direct_before_receive = len(default.direct_calls)
    receive_advertisement = asyncio.run(default.fetch(Request(
        "https://app.forkmesh.com/forkmesh/forkmesh/info/refs"
        "?service=git-receive-pack")))
    assert receive_advertisement.status == 209
    assert len(default.ingress_calls) == ingress_before_receive
    assert len(default.direct_calls) == direct_before_receive + 1

    serialized_ingress_count = 2 + len(serialized_routes) + len(git_requests)
    assert len(default.ingress_calls) == serialized_ingress_count
    assert len(default.direct_calls) == 5

    # Repository mutations stay direct so a streamed body is never replayed
    # after an ambiguous ingress failure.
    mutation = asyncio.run(default.fetch(Request(
        "https://app.forkmesh.com/api/repo/jett/demo/issues",
        method="POST",
        headers={"content-type": "application/json"},
        body='{"title":"one"}',
    )))
    assert mutation.status == 209
    assert len(default.ingress_calls) == serialized_ingress_count
    assert len(default.direct_calls) == 6

    # Missing snapshots also use the direct live route.
    default._inside_app_ingress = False
    default.env.row = None
    snapshot_path = SNAPSHOT_PATHS[0]
    snapshot_request = Request("https://app.forkmesh.com" + snapshot_path)
    live = asyncio.run(default._public_snapshot_response(
        snapshot_request, urlparse(snapshot_request.url), snapshot_path
    ))
    assert live.status == 209
    assert live.headers.get("x-forkmesh-public-snapshot") == (
        "missing-fallback")
    assert len(default.ingress_calls) == serialized_ingress_count

    # A failed serialized ingress request is intentional backpressure. Its
    # response header keeps the outer Worker logger quiet, while the compact
    # console marker lets a Cloudflare tail viewer make the same distinction
    # even though tail records omit response headers.
    default.ingress_handler = abort
    unavailable = asyncio.run(default.fetch(Request(
        "https://app.forkmesh.com/api/mirror-replica",
        headers={"user-agent": "ForkMesh-Mirror-Replica/1"},
    )))
    assert unavailable.status == 503
    assert json.loads(unavailable.body) == {
        "error": "mirror_sync_temporarily_unavailable"}
    assert unavailable.headers.get("x-forkmesh-expected-degraded") == "1"
    assert unavailable.headers.get("retry-after") == "30"
    assert "forkmesh_expected_degraded_response" in capsys.readouterr().out
    assert len(default.ingress_calls) == serialized_ingress_count + 1


def test_node_event_auth_and_final_handoff_clone_native_request_independently():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in {
            "_app_ingress_shard",
            "_app_ingress_request",
            "_app_ingress_authorization",
            "_git_clone_http_request",
            "_package_api_http_request",
            "_repository_projection_http_request",
            "_node_events_direct_fetch",
            "_node_events_do_name",
        }
    ]
    assert {node.name for node in selected} == {
        "_app_ingress_shard",
        "_app_ingress_request",
        "_app_ingress_authorization",
        "_git_clone_http_request",
        "_package_api_http_request",
        "_repository_projection_http_request",
        "_node_events_direct_fetch",
        "_node_events_do_name",
    }
    logged_aborts = []
    durable_calls = []

    async def log_abort(env, request, path, error):
        logged_aborts.append((env, request, path, error))

    async def sanitized_durable_request(request):
        allowed = {}
        for name in (
            "upgrade", "connection", "sec-websocket-version",
            "sec-websocket-key", "sec-websocket-protocol", "content-type",
            "content-encoding", "authorization", "accept", "user-agent",
        ):
            value = request.headers.get(name)
            if value:
                allowed[name] = value
        forwarded = Request(
            request.url, method=request.method, headers=allowed)
        durable_calls.append(forwarded)
        return forwarded

    namespace = {
        "APP_INGRESS_NAME_PREFIX": "app-ingress-v1:",
        "APP_GIT_INGRESS_NAME_PREFIX": "app-git-ingress-v1:",
        "PRODUCT_ANALYSIS_PATH": "/product-analysis",
        "plugin_api_prefixes": lambda: ("/api/demo-package",),
        "APP_INGRESS_GIT_READ_PURPOSE": "git-read-v1",
        "APP_INGRESS_NODE_AUTH_PURPOSE": "node-events-auth-v1",
        "EXPECTED_DEGRADED_HEADERS": {"x-forkmesh-expected-degraded": "1"},
        "NODE_EVENTS_OBJECT_RE": re.compile(
            r"nodes:[a-z0-9._:-]{1,120}"
            r"|watch:[a-z0-9][a-z0-9-]{0,62}/[a-z0-9._-]{1,100}"),
        "JsRequest": _JsRequest,
        "JsURL": _JsURL,
        "re": re,
        "json_response": lambda payload, status=200, extra_headers=None: Response(
            json.dumps(payload), status=status, headers=extra_headers),
        "durable_object_request": sanitized_durable_request,
        "log_durable_object_abort": log_abort,
        "method_name": lambda request: request.method,
        "safe_segment": lambda value: (
            str(value or "").strip().lower()
            if re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}",
                            str(value or "").strip().lower())
            else ""
        ),
    }
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=selected, type_ignores=[]
    )), str(ENTRY), "exec"), with_maintenance_off(namespace))

    socket_response = SimpleNamespace(status=101, webSocket=object())

    authorized = Response(
        None,
        status=204,
        headers={"x-forkmesh-authorized-owner": "mirror9"},
    )

    class IngressBinding:
        def __init__(self, outcomes=None):
            self.names = []
            self.forwarded = []
            self.outcomes = list(outcomes or [authorized])

        def idFromName(self, name):
            self.names.append(name)
            return name

        def get(self, _object_id):
            binding = self

            class Stub:
                async def fetch(self, request):
                    binding.forwarded.append(request)
                    outcome = binding.outcomes.pop(0)
                    if isinstance(outcome, BaseException):
                        raise outcome
                    return outcome

            return Stub()

    class NodesBinding:
        def __init__(self, outcomes=None):
            self.names = []
            self.forwarded = []
            self.outcomes = list(outcomes or [socket_response])

        def idFromName(self, name):
            self.names.append(name)
            return name

        def get(self, _object_id):
            binding = self

            class Stub:
                async def fetch(self, request):
                    binding.forwarded.append(request)
                    outcome = binding.outcomes.pop(0)
                    if isinstance(outcome, BaseException):
                        raise outcome
                    return outcome

            return Stub()

    ingress = IngressBinding()
    nodes = NodesBinding()
    native_request = Request(
        "https://app.forkmesh.com/api/nodes/events?owner=mirror9",
        headers={
            "authorization": "ForkMesh-Signature signed",
            "cf-ray": "f1234567890-example",
            "connection": "Upgrade",
            "cookie": "session=browser",
            "if-none-match": '"rev"',
            "origin": "https://app.forkmesh.com",
            "range": "bytes=0-1023",
            "sec-websocket-protocol": "forkmesh-v1",
            "x-client-arbitrary": "must-not-cross",
            "upgrade": "websocket",
        },
    )
    original = SimpleNamespace(
        url=native_request.url,
        method=native_request.method,
        headers=native_request.headers,
        body=native_request.body,
        js_object=native_request,
    )
    _JsRequest.inputs.clear()
    response = asyncio.run(namespace["_node_events_direct_fetch"](
        SimpleNamespace(
            FORKMESH_CRON_RUNNER=ingress,
            FORKMESH_NODES=nodes,
        ),
        original,
    ))
    assert response is socket_response
    assert _JsRequest.inputs == [native_request]
    assert all(value is not original for value in _JsRequest.inputs)
    assert ingress.names == ["app-ingress-v1:f"]
    assert nodes.names == ["nodes:mirror9"]
    assert len(ingress.forwarded) == 1
    assert len(nodes.forwarded) == 1
    auth_forwarded = ingress.forwarded[0]
    final_forwarded = nodes.forwarded[0]
    assert auth_forwarded is not original
    assert final_forwarded is not original
    assert auth_forwarded is not final_forwarded
    assert auth_forwarded.url == native_request.url
    assert auth_forwarded.method == "GET"
    for name in (
        "authorization", "cf-ray", "connection", "cookie",
        "if-none-match", "origin", "range", "sec-websocket-protocol",
        "upgrade", "x-client-arbitrary",
    ):
        assert auth_forwarded.headers.get(name) == native_request.headers.get(name)
    assert final_forwarded.url == native_request.url
    assert final_forwarded.method == "GET"
    for name in (
        "authorization", "connection", "sec-websocket-protocol", "upgrade",
    ):
        assert final_forwarded.headers.get(name) == native_request.headers.get(name)
    for name in (
        "cf-ray", "cookie", "if-none-match", "origin", "range",
        "x-client-arbitrary", "x-forkmesh-internal-ingress",
    ):
        assert final_forwarded.headers.get(name) is None
    assert auth_forwarded.headers.get(
        "x-forkmesh-internal-ingress") == "node-events-auth-v1"
    assert final_forwarded.headers.get("x-forkmesh-internal-ingress") is None
    assert native_request.headers.get("x-forkmesh-internal-ingress") is None
    assert namespace["_app_ingress_shard"](
        Request("https://app.forkmesh.com/", headers={"cf-ray": "z-invalid"})
    ) == "0"
    assert namespace["_app_ingress_shard"](
        Request("https://app.forkmesh.com/")
    ) == "0"

    transient = RuntimeError("transient final object abort")
    retry_ingress = IngressBinding()
    retry_nodes = NodesBinding([transient, socket_response])
    _JsRequest.inputs.clear()
    retried = asyncio.run(namespace["_node_events_direct_fetch"](
        SimpleNamespace(
            FORKMESH_CRON_RUNNER=retry_ingress,
            FORKMESH_NODES=retry_nodes,
        ),
        original,
    ))
    assert retried is socket_response
    assert _JsRequest.inputs == [native_request]
    assert len(retry_ingress.forwarded) == 1
    assert len(retry_nodes.forwarded) == 2
    assert len(durable_calls) == 3
    assert retry_nodes.forwarded[0] is not retry_nodes.forwarded[1]
    assert all(
        forwarded.headers.get("upgrade") == "websocket"
        for forwarded in retry_nodes.forwarded
    )
    assert logged_aborts == []

    final_errors = [
        RuntimeError("first final object abort"),
        RuntimeError("second final object abort"),
    ]
    failed_ingress = IngressBinding()
    failed_nodes = NodesBinding(final_errors)
    failed_env = SimpleNamespace(
        FORKMESH_CRON_RUNNER=failed_ingress,
        FORKMESH_NODES=failed_nodes,
    )
    _JsRequest.inputs.clear()
    unavailable = asyncio.run(namespace["_node_events_direct_fetch"](
        failed_env, original,
    ))
    assert unavailable.status == 503
    assert json.loads(unavailable.body) == {"error": "unavailable"}
    assert unavailable.headers.get("x-forkmesh-expected-degraded") == "1"
    assert _JsRequest.inputs == [native_request]
    assert len(failed_ingress.forwarded) == 1
    assert len(failed_nodes.forwarded) == 2
    assert len(durable_calls) == 5
    assert len(logged_aborts) == 1
    logged_env, logged_request, logged_path, logged_error = logged_aborts[0]
    assert logged_env is failed_env
    assert logged_request is original
    assert logged_path == "/api/nodes/events"
    assert logged_error is final_errors[-1]

    auth_errors = [
        RuntimeError("first authorization object abort"),
        RuntimeError("second authorization object abort"),
    ]
    aborted_ingress = IngressBinding(auth_errors)
    untouched_nodes = NodesBinding()
    auth_env = SimpleNamespace(
        FORKMESH_CRON_RUNNER=aborted_ingress,
        FORKMESH_NODES=untouched_nodes,
    )
    authorization_unavailable = asyncio.run(
        namespace["_node_events_direct_fetch"](auth_env, original))
    assert authorization_unavailable.status == 503
    assert json.loads(authorization_unavailable.body) == {
        "error": "authorization_unavailable"}
    assert authorization_unavailable.headers.get(
        "x-forkmesh-expected-degraded") == "1"
    assert len(aborted_ingress.forwarded) == 2
    assert untouched_nodes.names == [] and untouched_nodes.forwarded == []
    assert len(logged_aborts) == 2
    assert logged_aborts[-1][2] == "/api/nodes/events/authorization"
    assert logged_aborts[-1][3] is auth_errors[-1]


def test_ingress_receiver_accepts_only_exact_shards_and_auth_is_preflight_only():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    runner = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "ForkMeshCronRunner"
    )
    selected = [
        node for node in runner.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in {"_object_name", "fetch"}
    ]
    assert {node.name for node in selected} == {"_object_name", "fetch"}
    runner_node = ast.ClassDef(
        name="ForkMeshCronRunner",
        bases=[],
        keywords=[],
        body=selected,
        decorator_list=[],
    )
    calls = {"auth": [], "websocket_auth": [], "inner": [], "gates": 0}
    inner_response = Response('{"inner":true}', status=209)

    class Default:
        @staticmethod
        async def _fetch_inner(router, request):
            calls["inner"].append((router, request))
            return inner_response

    async def authorize(_env, request):
        calls["auth"].append(request)
        if "reject=1" in request.url:
            return "", Response('{"error":"unauthorized"}', status=401)
        return "mirror9", None

    async def node_events_object(env, request):
        owner, rejection = await authorize(env, request)
        return ("nodes:" + owner if owner else ""), rejection

    async def websocket_authorize(_env, request):
        calls["websocket_auth"].append(request)
        return Response(
            None,
            status=204,
            headers={"x-forkmesh-websocket-handoff": "v1"},
        )

    def json_response(payload, status=200):
        return Response(json.dumps(payload), status=status)

    namespace = {
        "APP_INGRESS_NAME_PREFIX": "app-ingress-v1:",
        "APP_INGRESS_HTTP_PURPOSE": "v1",
        "APP_GIT_INGRESS_NAME_PREFIX": "app-git-ingress-v1:",
        "APP_INGRESS_GIT_READ_PURPOSE": "git-read-v1",
        "APP_INGRESS_NODE_AUTH_PURPOSE": "node-events-auth-v1",
        "APP_INGRESS_WEBSOCKET_AUTH_PURPOSE": "websocket-auth-v1",
            "CRON_RUNNER_NAME": "scheduled-runner-v1",
            "SNAPSHOT_REPUBLISH_NAME": "snapshot-republish-v1",
            "MIRROR_WAKE_NAME": "mirror-wake-v1",
        "AppRuntime": Default,
        "Response": Response,
        "_AppIngressRouter": lambda env: ("router", env),
        "_node_events_object": node_events_object,
        "_websocket_handoff_authorization": websocket_authorize,
        "json_response": json_response,
        "urlparse": urlparse,
    }
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[runner_node], type_ignores=[]
    )), str(ENTRY), "exec"), with_maintenance_off(namespace))

    class Context:
        def __init__(self, name):
            self.id = SimpleNamespace(name=name)

        async def blockConcurrencyWhile(self, callback):
            calls["gates"] += 1
            return await callback()

    def receiver(name):
        value = namespace["ForkMeshCronRunner"]()
        value.ctx = Context(name)
        # Deliberately expose no final socket binding: the auth receiver must
        # only decide the owner and return a bounded, non-upgraded preflight.
        value.env = SimpleNamespace(marker="env")
        return value

    ordinary = Request(
        "https://app.forkmesh.com/api/repositories",
        headers={"x-forkmesh-internal-ingress": "v1"},
    )
    accepted = asyncio.run(receiver("app-ingress-v1:a").fetch(ordinary))
    assert accepted is inner_response
    assert len(calls["inner"]) == 1
    assert calls["gates"] == 0

    git_read = Request(
        "https://app.forkmesh.com/forkmesh/forkmesh/info/refs"
        "?service=git-upload-pack",
        headers={"x-forkmesh-internal-ingress": "git-read-v1"},
    )
    accepted_git = asyncio.run(
        receiver("app-git-ingress-v1:c").fetch(git_read))
    assert accepted_git is inner_response
    assert len(calls["inner"]) == 2
    assert calls["inner"][-1][1] is git_read
    assert calls["gates"] == 1

    for invalid_name in (
        "app-ingress-v1:",
        "app-ingress-v1:00",
        "app-ingress-v1:g",
        "prefix-app-ingress-v1:0",
        "app-git-ingress-v1:",
        "app-git-ingress-v1:00",
        "app-git-ingress-v1:g",
        "prefix-app-git-ingress-v1:0",
    ):
        response = asyncio.run(receiver(invalid_name).fetch(ordinary))
        assert response.status == 404
    assert len(calls["inner"]) == 2

    wrong_git_marker = Request(
        git_read.url,
        headers={"x-forkmesh-internal-ingress": "v1"},
    )
    assert asyncio.run(
        receiver("app-git-ingress-v1:c").fetch(wrong_git_marker)
    ).status == 404
    assert calls["gates"] == 1

    wrong_marker = Request(
        "https://app.forkmesh.com/api/repositories",
        headers={"x-forkmesh-internal-ingress": "wrong"},
    )
    assert asyncio.run(
        receiver("app-ingress-v1:f").fetch(wrong_marker)
    ).status == 404
    assert len(calls["inner"]) == 2

    auth_request = Request(
        "https://app.forkmesh.com/api/nodes/events?owner=mirror9",
        headers={
            "upgrade": "websocket",
            "x-forkmesh-internal-ingress": "node-events-auth-v1",
        },
    )
    authorized = asyncio.run(
        receiver("app-ingress-v1:0").fetch(auth_request)
    )
    assert authorized.status == 204
    assert authorized.headers.get("x-forkmesh-authorized-owner") == "mirror9"
    assert calls["auth"] == [auth_request]
    assert len(calls["inner"]) == 2

    rejected_request = Request(
        "https://app.forkmesh.com/api/nodes/events?reject=1",
        headers={
            "upgrade": "websocket",
            "x-forkmesh-internal-ingress": "node-events-auth-v1",
        },
    )
    rejected = asyncio.run(
        receiver("app-ingress-v1:0").fetch(rejected_request)
    )
    assert rejected.status == 401
    assert calls["auth"][-1] is rejected_request
    assert len(calls["inner"]) == 2

    websocket_request = Request(
        "https://app.forkmesh.com/api/world/ws?ticket=short-lived",
        headers={
            "upgrade": "websocket",
            "x-forkmesh-internal-ingress": "websocket-auth-v1",
        },
    )
    websocket_authorized = asyncio.run(
        receiver("app-ingress-v1:b").fetch(websocket_request)
    )
    assert websocket_authorized.status == 204
    assert websocket_authorized.status != 101
    assert calls["websocket_auth"] == [websocket_request]
    assert len(calls["inner"]) == 2


def _websocket_namespace():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    function_names = {
        "_app_ingress_authorization",
        "_public_websocket_path",
        "_websocket_handoff_response",
        "_websocket_handoff_authorization",
        "_validated_websocket_handoff",
        "_websocket_direct_fetch",
        "_chat_socket_query_ok",
        "room_key_from_path",
    }
    class_names = {
        "_WebSocketHandoffStub",
        "_WebSocketHandoffNamespace",
        "_WebSocketHandoffEnv",
    }
    constant_names = {
        "_WEBSOCKET_HANDOFF_BINDINGS",
        "_WEBSOCKET_HANDOFF_HEADERS",
        "_WEBSOCKET_HANDOFF_HEADERS_BY_BINDING",
        "_WEBSOCKET_HANDOFF_PREFIX",
    }
    functions = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in function_names
    ]
    classes = [
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name in class_names
    ]
    assignments = [
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id in constant_names
            for target in node.targets
        )
    ]
    assert {node.name for node in functions} == function_names
    assert {node.name for node in classes} == class_names
    assert {
        target.id
        for node in assignments
        for target in node.targets
        if isinstance(target, ast.Name)
    } == constant_names

    def json_response(payload, status=200, extra_headers=None, **_kwargs):
        return Response(
            json.dumps(payload), status=status, headers=extra_headers)

    async def unused_ingress(*_args):
        raise AssertionError("test must install its ingress response")

    async def unused_log(*_args):
        raise AssertionError("unexpected Durable Object abort")

    namespace = {
        "APP_INGRESS_WEBSOCKET_AUTH_PURPOSE": "websocket-auth-v1",
        "CHAT_CHANNEL_DO_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/v([1-9][0-9]*)/(ws|revoke)$"),
        "CHAT_CHANNEL_WS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/ws/?$"),
        "CHAT_DIRECT_MESSAGE_DO_RE": re.compile(
            r"^/api/chat/direct-messages/([0-9a-f]{32})/"
            r"v([1-9][0-9]*)/(ws|revoke)$"),
        "CHAT_DIRECT_MESSAGE_WS_RE": re.compile(
            r"^/api/chat/direct-messages/([0-9a-f]{32})/ws/?$"),
        "EXPECTED_DEGRADED_HEADERS": {"x-forkmesh-expected-degraded": "1"},
        "JsRequest": _JsRequest,
        "MAX_ROOM_NAME": 80,
        "MAX_NODE_NAME": 80,
        "OFFICE_CHANNEL_WS_RE": re.compile(
            r"^/api/world/office/channels/([0-9a-f]{32})/ws/?$"),
        "REPO_ROOM_RE": re.compile(
            r"^/api/repo/([^/]+)/([^/]+)/rooms/([^/]+)/(?:ws|clients)$"),
        "ROOM_RE": re.compile(r"^/api/room/([^/]+)/(?:ws|clients)$"),
        "Response": Response,
        "_AppIngressRouter": lambda env: SimpleNamespace(env=env),
        "_app_ingress_request": unused_ingress,
        "json_response": json_response,
        "clean_string": catalog_contract.clean_string,
        "log_durable_object_abort": unused_log,
        "method_name": lambda request: str(request.method).upper(),
        "parse_qs": parse_qs,
        "quote": quote,
        "re": re,
        "safe_segment": catalog_contract.safe_segment,
        "to_js": lambda value: value,
        "urlparse": urlparse,
    }
    module = ast.fix_missing_locations(ast.Module(
        body=[*assignments, *functions, *classes], type_ignores=[]
    ))
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


def _websocket_handoff_cases():
    channel = "1" * 32
    direct = "2" * 32
    note = "3" * 32
    account = "a" * 64
    handshake = {
        "upgrade": "websocket",
        "connection": "Upgrade",
        "sec-websocket-version": "13",
        "sec-websocket-key": "test-key",
        "sec-websocket-protocol": "forkmesh-v1",
        # These are deliberately not in the handoff allowlist.
        "authorization": "Bearer private",
        "cookie": "session=private",
        "origin": "https://world.forkmesh.com",
        "x-forkmesh-internal-ingress": "client-spoof",
    }
    return (
        (
            Request(
                "https://app.forkmesh.com/api/world/ws?ticket=valid",
                headers=handshake),
            "FORKMESH_WORLD",
            "town-square-v1",
            "https://app.forkmesh.com/api/world/ws",
            {
                **handshake,
                "x-forkmesh-country": "US",
                "x-forkmesh-world-claim": "trusted-world-claim",
                "x-forkmesh-world-ip-token": "b" * 64,
                "x-forkmesh-world-agent-token": "c" * 64,
                "x-forkmesh-world-private-ip": "192.0.2.1",
                "x-forkmesh-world-private-agent": "Browser 1",
            },
        ),
        (
            Request(
                "https://app.forkmesh.com/api/world/office/general/ws"
                "?ticket=valid", headers=handshake),
            "FORKMESH_OFFICE_ROOM",
            "office:world-general:v1",
            "https://app.forkmesh.com/api/world/office/world-general/v1/ws",
            {**handshake, "x-forkmesh-office-claim": "general-claim"},
        ),
        (
            Request(
                "https://app.forkmesh.com/api/world/office/channels/"
                + channel + "/ws?ticket=valid", headers=handshake),
            "FORKMESH_OFFICE_ROOM",
            "office:chat-channel:" + channel + ":v7",
            "https://app.forkmesh.com/api/world/office/"
            + channel + "/v7/ws",
            {**handshake, "x-forkmesh-office-claim": "channel-claim"},
        ),
        (
            Request(
                "https://app.forkmesh.com/api/chat/channels/"
                + channel + "/ws?ticket=valid", headers=handshake),
            "FORKMESH_MAINNODE_ROOM",
            "chat-channel:" + channel + ":v3",
            "https://app.forkmesh.com/api/chat/channels/"
            + channel + "/v3/ws?account=" + account,
            handshake,
        ),
        (
            Request(
                "https://app.forkmesh.com/api/chat/direct-messages/"
                + direct + "/ws?ticket=valid", headers=handshake),
            "FORKMESH_MAINNODE_ROOM",
            "chat-direct:" + direct + ":v2",
            "https://app.forkmesh.com/api/chat/direct-messages/"
            + direct + "/v2/ws?account=" + account,
            handshake,
        ),
        (
            Request(
                "https://app.forkmesh.com/api/notes/" + note + "/ws",
                headers=handshake,
            ),
            "FORKMESH_NOTE_ROOM",
            "note:" + note,
            "https://forkmesh.internal/api/notes/" + note
            + "/ws?role=editor&actor=alice",
            handshake,
        ),
        (
            Request(
                "https://app.forkmesh.com/api/repo/alice/demo/rooms/general/ws",
                headers=handshake,
            ),
            "FORKMESH_MAINNODE_ROOM",
            "repo:alice/demo:room:general",
            "https://app.forkmesh.com/api/repo/alice/demo/rooms/general/ws",
            handshake,
        ),
        (
            Request(
                "https://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/"
                "general/ws?node=alice&ts=1&sig=x",
                headers=handshake,
            ),
            "FORKMESH_MAINNODE_ROOM",
            "repo:mainnode/forkmesh:room:general",
            "https://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/"
            "general/ws?account=" + account + "&addr=" + "d" * 32,
            handshake,
        ),
        (
            Request(
                "https://app.forkmesh.com/api/status/ws",
                headers=handshake),
            "FORKMESH_WORLD",
            "town-square-v1",
            "https://app.forkmesh.com/api/status/ws",
            handshake,
        ),
    )


def test_websocket_auth_capture_never_opens_or_returns_final_socket():
    namespace = _websocket_namespace()
    route_calls = []

    class PublicEnv:
        def __getattr__(self, name):
            if name in namespace["_WEBSOCKET_HANDOFF_BINDINGS"]:
                raise AssertionError("authorization touched final namespace " + name)
            raise AttributeError(name)

    class Default:
        @staticmethod
        async def _route(router, request, parsed):
            route_calls.append((request, parsed.path))
            binding = router.env.FORKMESH_WORLD
            object_id = binding.idFromName("town-square-v1")
            forwarded = Request(
                "https://app.forkmesh.com/api/world/ws",
                headers={
                    "upgrade": "websocket",
                    "x-forkmesh-world-claim": "trusted",
                },
            )
            return await binding.get(object_id).fetch(forwarded)

    namespace["AppRuntime"] = Default
    request = Request(
        "https://app.forkmesh.com/api/world/ws?ticket=valid",
        headers={"upgrade": "websocket"},
    )
    authorized = asyncio.run(namespace["_websocket_handoff_authorization"](
        PublicEnv(), request,
    ))
    assert authorized.status == 204
    assert authorized.status != 101
    assert authorized.headers.get("x-forkmesh-websocket-handoff") == "v1"
    assert authorized.headers.get("x-forkmesh-handoff-binding") == (
        "FORKMESH_WORLD"
    )
    assert route_calls == [(request, "/api/world/ws")]

    for rejected in (
        Request(
            "https://app.forkmesh.com/api/world/ws",
            method="POST",
            headers={"upgrade": "websocket"},
        ),
        Request(
            "https://app.forkmesh.com/api/world/ws",
            headers={"upgrade": "h2c"},
        ),
        Request(
            "https://app.forkmesh.com/api/repositories",
            headers={"upgrade": "websocket"},
        ),
    ):
        response = asyncio.run(
            namespace["_websocket_handoff_authorization"](
                PublicEnv(), rejected)
        )
        assert response.status == 404
    assert len(route_calls) == 1


def test_websocket_descriptors_cover_all_final_bindings_and_strip_secrets():
    namespace = _websocket_namespace()
    observed_bindings = set()
    for source, binding, object_name, target_url, forwarded_headers in (
            _websocket_handoff_cases()):
        forwarded = Request(target_url, headers=forwarded_headers)
        response = namespace["_websocket_handoff_response"](
            binding, object_name, forwarded)
        assert response.status == 204
        assert response.status != 101
        descriptor = namespace["_validated_websocket_handoff"](
            SimpleNamespace(), source, response)
        assert descriptor is not None
        final_binding, final_object, final_url, final_headers = descriptor
        assert (final_binding, final_object, final_url) == (
            binding, object_name, target_url)
        assert final_headers["upgrade"] == "websocket"
        for secret in (
            "authorization", "cookie", "origin",
            "x-forkmesh-internal-ingress",
        ):
            assert secret not in final_headers
        assert set(final_headers).issubset(
            set(namespace["_WEBSOCKET_HANDOFF_HEADERS"]))
        observed_bindings.add(binding)
    assert observed_bindings == set(namespace["_WEBSOCKET_HANDOFF_BINDINGS"])


def test_organization_alias_room_handoff_canonicalizes_and_stays_correlated():
    namespace = _websocket_namespace()
    source = Request(
        "https://app.forkmesh.com/api/repo/acme/demo/rooms/general/ws",
        headers={"upgrade": "websocket", "cookie": "session=private"},
    )
    # The authorized repository router selected mirror9's room object while
    # its established forwarded Request still carried the public org alias.
    captured = namespace["_websocket_handoff_response"](
        "FORKMESH_MAINNODE_ROOM",
        "repo:mirror9/demo:room:general",
        Request(
            source.url,
            headers={
                "upgrade": "websocket",
                "connection": "Upgrade",
                "cookie": "session=must-not-cross",
            },
        ),
    )
    canonical_url = (
        "https://app.forkmesh.com/api/repo/"
        "mirror9/demo/rooms/general/ws"
    )
    assert captured.status == 204
    assert captured.headers.get("x-forkmesh-handoff-object") == (
        "repo:mirror9/demo:room:general"
    )
    assert captured.headers.get("x-forkmesh-handoff-url") == canonical_url
    descriptor = namespace["_validated_websocket_handoff"](
        SimpleNamespace(), source, captured)
    assert descriptor is not None
    assert descriptor[:3] == (
        "FORKMESH_MAINNODE_ROOM",
        "repo:mirror9/demo:room:general",
        canonical_url,
    )
    assert descriptor[3]["upgrade"] == "websocket"
    assert "cookie" not in descriptor[3]

    def changed(name, value):
        response = Response(captured)
        response.headers.set(name, value)
        return response

    mismatches = (
        changed(
            "x-forkmesh-handoff-url",
            "https://app.forkmesh.com/api/repo/"
            "mirror9/other/rooms/general/ws",
        ),
        changed(
            "x-forkmesh-handoff-url",
            "https://app.forkmesh.com/api/repo/"
            "mirror9/demo/rooms/other/ws",
        ),
        changed(
            "x-forkmesh-handoff-object",
            "repo:mirror14/demo:room:general",
        ),
    )
    for mismatch in mismatches:
        assert namespace["_validated_websocket_handoff"](
            SimpleNamespace(), source, mismatch) is None


def test_node_retirement_rehomes_durable_organization_aliases_first():
    # The World route bodies live in world/src/world_routes.py (kept out of
    # the Worker's startup source); this contract follows the handler there.
    world_text = (
        APP.parent / "world" / "src" / "world_routes.py"
    ).read_text(encoding="utf-8")
    tree = ast.parse(world_text)
    handler = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "world_admin_delete_node_handler"
    )
    source = ast.get_source_segment(world_text, handler)
    handoff = (APP / "src/catalog_routes.py").read_text(encoding="utf-8")
    assert "SELECT org_bi,repo FROM org_repos WHERE node_owner=?" in handoff
    assert "organization_alias_requires_replacement" in source
    assert "repo_mirror_same_group" in handoff
    assert "UPDATE org_repos SET node_owner=?" in handoff
    assert source.index(
        "rehome_organization_aliases_before_node_delete") < source.index(
            "await _delete_account_namespace")
    assert '"organizationAliasHandoffs": alias_handoffs' in source


def test_websocket_descriptor_corruption_and_cross_route_swaps_fail_closed():
    namespace = _websocket_namespace()
    cases = _websocket_handoff_cases()
    world_source, world_binding, world_object, world_url, world_headers = cases[0]

    def response_for(binding=world_binding, object_name=world_object,
                     target_url=world_url, headers=None):
        return namespace["_websocket_handoff_response"](
            binding,
            object_name,
            Request(target_url, headers=headers or world_headers),
        )

    corrupt = []
    unknown_binding = response_for()
    unknown_binding.headers.set("x-forkmesh-handoff-binding", "EVIL_BINDING")
    corrupt.append(unknown_binding)

    wrong_object = response_for()
    wrong_object.headers.set("x-forkmesh-handoff-object", "town-square-v2")
    corrupt.append(wrong_object)

    off_origin = response_for()
    off_origin.headers.set(
        "x-forkmesh-handoff-url", "https://evil.invalid/api/world/ws")
    corrupt.append(off_origin)

    missing_upgrade = response_for(headers={"connection": "Upgrade"})
    corrupt.append(missing_upgrade)

    extra_query = response_for()
    extra_query.headers.set(
        "x-forkmesh-handoff-url",
        "https://app.forkmesh.com/api/world/ws?account=" + "a" * 64,
    )
    corrupt.append(extra_query)

    for response in corrupt:
        assert namespace["_validated_websocket_handoff"](
            SimpleNamespace(), world_source, response) is None

    # Even a descriptor that is internally valid for another family is not a
    # valid authorization result for this source route.
    note_source, note_binding, note_object, note_url, note_headers = cases[5]
    del note_source
    cross_route = response_for(
        note_binding, note_object, note_url, note_headers)
    assert namespace["_validated_websocket_handoff"](
        SimpleNamespace(), world_source, cross_route) is None

    assert namespace["_websocket_handoff_response"](
        "EVIL_BINDING", "object", Request(
            world_url, headers=world_headers)
    ).status == 503


def test_websocket_direct_handoff_uses_sanitized_headers_and_retries_final_do():
    namespace = _websocket_namespace()
    logged = []

    async def log_abort(env, request, path, error):
        logged.append((env, request, path, error))

    namespace["log_durable_object_abort"] = log_abort

    class Binding:
        def __init__(self, outcomes):
            self.outcomes = list(outcomes)
            self.names = []
            self.forwarded = []

        def idFromName(self, name):
            self.names.append(name)
            return name

        def get(self, _object_id):
            binding = self

            class Stub:
                async def fetch(self, request):
                    binding.forwarded.append(request)
                    outcome = binding.outcomes.pop(0)
                    if isinstance(outcome, BaseException):
                        raise outcome
                    return outcome

            return Stub()

    socket_response = SimpleNamespace(status=101, webSocket=object())
    for source, binding_name, object_name, target_url, forwarded_headers in (
            _websocket_handoff_cases()):
        authorized = namespace["_websocket_handoff_response"](
            binding_name, object_name,
            Request(target_url, headers=forwarded_headers))
        ingress_calls = []

        async def ingress(_env, request, purpose, response=authorized):
            ingress_calls.append((request, purpose))
            return response

        namespace["_app_ingress_request"] = ingress
        final_binding = Binding([socket_response])
        env = SimpleNamespace(**{binding_name: final_binding})
        _JsRequest.inputs.clear()
        _JsRequest.inits.clear()
        result = asyncio.run(namespace["_websocket_direct_fetch"](env, source))
        assert result is socket_response
        assert ingress_calls == [(source, "websocket-auth-v1")]
        assert final_binding.names == [object_name]
        assert len(final_binding.forwarded) == 1
        final_request = final_binding.forwarded[0]
        assert final_request.url == target_url
        assert final_request.method == "GET"
        descriptor = namespace["_validated_websocket_handoff"](
            env, source, authorized)
        assert descriptor is not None
        assert dict(final_request.headers.items()) == descriptor[3]
        for secret in (
            "authorization", "cookie", "origin",
            "x-forkmesh-internal-ingress",
        ):
            assert final_request.headers.get(secret) is None

    source, binding_name, object_name, target_url, forwarded_headers = (
        _websocket_handoff_cases()[0])
    authorized = namespace["_websocket_handoff_response"](
        binding_name, object_name,
        Request(target_url, headers=forwarded_headers))

    async def ingress(_env, _request, _purpose):
        return authorized

    namespace["_app_ingress_request"] = ingress
    transient = RuntimeError("transient final abort")
    retry_binding = Binding([transient, socket_response])
    retry_env = SimpleNamespace(**{binding_name: retry_binding})
    retried = asyncio.run(namespace["_websocket_direct_fetch"](
        retry_env, source))
    assert retried is socket_response
    assert len(retry_binding.forwarded) == 2
    assert logged == []

    final_errors = [RuntimeError("first abort"), RuntimeError("second abort")]
    failed_binding = Binding(final_errors)
    failed_env = SimpleNamespace(**{binding_name: failed_binding})
    unavailable = asyncio.run(namespace["_websocket_direct_fetch"](
        failed_env, source))
    assert unavailable.status == 503
    assert unavailable.headers.get("x-forkmesh-expected-degraded") == "1"
    assert len(failed_binding.forwarded) == 2
    assert len(logged) == 1
    assert logged[0][0] is failed_env
    assert logged[0][1] is source
    assert logged[0][2] == "/api/world/ws"
    assert logged[0][3] is final_errors[-1]

    invalid = Response(
        None,
        status=204,
        headers={
            "x-forkmesh-websocket-handoff": "v1",
            "x-forkmesh-handoff-binding": binding_name,
            "x-forkmesh-handoff-object": "wrong-object",
            "x-forkmesh-handoff-url": target_url,
            "x-forkmesh-handoff-header-upgrade": "websocket",
        },
    )

    async def invalid_ingress(_env, _request, _purpose):
        return invalid

    namespace["_app_ingress_request"] = invalid_ingress
    untouched_binding = Binding([socket_response])
    invalid_env = SimpleNamespace(**{binding_name: untouched_binding})
    rejected = asyncio.run(namespace["_websocket_direct_fetch"](
        invalid_env, source))
    assert rejected.status == 503
    assert rejected.headers.get("x-forkmesh-expected-degraded") == "1"
    assert untouched_binding.names == []
    assert untouched_binding.forwarded == []

    denied = Response('{"error":"not_found"}', status=404)

    async def denied_ingress(_env, _request, _purpose):
        return denied

    namespace["_app_ingress_request"] = denied_ingress
    assert asyncio.run(namespace["_websocket_direct_fetch"](
        invalid_env, source)) is denied

    auth_errors = [
        RuntimeError("first websocket authorization abort"),
        RuntimeError("second websocket authorization abort"),
    ]
    auth_attempts = []

    async def aborted_ingress(_env, _request, _purpose):
        error = auth_errors[len(auth_attempts)]
        auth_attempts.append(error)
        raise error

    namespace["_app_ingress_request"] = aborted_ingress
    authorization_unavailable = asyncio.run(
        namespace["_websocket_direct_fetch"](invalid_env, source))
    assert authorization_unavailable.status == 503
    assert json.loads(authorization_unavailable.body) == {
        "error": "authorization_unavailable"}
    assert authorization_unavailable.headers.get(
        "x-forkmesh-expected-degraded") == "1"
    assert auth_attempts == auth_errors
    assert len(logged) == 2
    assert logged[-1][2] == "/websocket-authorization"
    assert logged[-1][3] is auth_errors[-1]


def test_direct_app_root_health_version_and_mirror_identity_behavior():
    default = _control_default()

    root_request = Request("https://app.forkmesh.com/?section=repositories")
    root = default._app_root_redirect(root_request, urlparse(root_request.url))
    assert root.status == 308
    assert root.headers.get("location") == "/dashboard?section=repositories"
    assert root.headers.get("cache-control") == "no-store"
    assert root.headers.get("x-forkmesh-worker") == "app"
    foreign = Request("https://other.invalid/")
    assert default._app_root_redirect(foreign, urlparse(foreign.url)) is None

    world_login_request = Request(
        "https://world.forkmesh.com/login?next=%2Fdashboard"
    )
    world_login = default._world_ingress_redirect(
        world_login_request, urlparse(world_login_request.url)
    )
    assert world_login.status == 308
    assert world_login.headers.get("location") == (
        "https://app.forkmesh.com/login?next=%2Fdashboard"
    )
    world_docs_request = Request(
        "https://world.forkmesh.com/docs/protocol/?part=routing"
    )
    world_docs = default._world_ingress_redirect(
        world_docs_request, urlparse(world_docs_request.url)
    )
    assert world_docs.status == 308
    assert world_docs.headers.get("location") == (
        "https://forkmesh.com/docs/protocol/?part=routing"
    )
    world_shell_request = Request("https://world.forkmesh.com/world")
    assert default._world_ingress_redirect(
        world_shell_request, urlparse(world_shell_request.url)
    ) is None
    assert default._world_ingress_redirect(
        root_request, urlparse(root_request.url)
    ) is None

    health_request = Request("https://app.forkmesh.com/health")
    health = asyncio.run(default._health_control_response(
        health_request, urlparse(health_request.url)
    ))
    health_payload = json.loads(health.body)
    assert health.status == 200
    assert health_payload["runtime"] == "python-workers"
    assert health_payload["worker"] == "app"
    assert health_payload["rev"] == "release-rev"
    assert health_payload["deployFingerprint"] == FINGERPRINT
    assert health.headers.get("x-forkmesh-worker") == "app"

    head_request = Request("https://app.forkmesh.com/health", method="HEAD")
    head = asyncio.run(default._health_control_response(
        head_request, urlparse(head_request.url)
    ))
    assert head.status == 200 and head.body is None
    assert head.headers.get("x-forkmesh-worker") == "app"

    main_request = Request("https://app.forkmesh.com/api/mainnode")
    mainnode = asyncio.run(default._health_control_response(
        main_request, urlparse(main_request.url)
    ))
    main_payload = json.loads(mainnode.body)
    assert main_payload["routerIdentityReady"] is True
    assert main_payload["routerPublicKey"] == "A" * 43

    version_request = Request("https://app.forkmesh.com/api/version")
    version = default._version_control_response(version_request)
    version_payload = json.loads(version.body)
    assert version_payload == {
        "ok": True,
        "rev": "release-rev",
        "worker": "app",
        "version": "1.2.3",
        "deployFingerprint": FINGERPRINT,
        "now": 1_784_000_000_000,
    }
    assert version.headers.get("x-forkmesh-worker") == "app"

    identity_request = Request("https://app.forkmesh.com/api/mirrors/https")
    identity = asyncio.run(default._mirror_identity_control_response(
        identity_request
    ))
    assert json.loads(identity.body)["routerPublicKey"] == "A" * 43
    assert identity.headers.get("x-forkmesh-worker") == "app"
    registration = asyncio.run(default._mirror_identity_control_response(
        Request(identity_request.url, method="POST")
    ))
    assert registration.headers.get("x-forkmesh-worker") is None


def test_snapshot_payload_integer_contract_rejects_boolean_and_oversized_values():
    status = {"ok": True, "now": 1000, "systems": [], "current": {
        "lastCronSampleTs": True,
    }}
    assert not snapshot_contract.validate_snapshot_payload(
        "/api/status", status, 1000
    )

    mirror = {
        "ok": True,
        "owner": "forkmesh",
        "repo": "forkmesh",
        "groupKey": "root",
        "generatedAt": 1000,
        "summary": {
            "mirrors": 1, "online": 1, "cloneAvailable": 1,
            "dataHostedBytes": 0, "longestHostedSince": None,
            "freshestSync": None,
        },
        "mirrors": [{
            "endpointFresh": True, "endpointHealthy": True,
            "status": "online", "checkedAt": False,
        }],
    }
    assert not snapshot_contract.validate_snapshot_payload(
        "/api/repo/forkmesh/forkmesh/mirrors", mirror, 1000
    )

    def catalog(repository):
        return {
            "ok": True,
            "repositories": [repository],
            "sshGatewayConfigured": False,
            "sshTransport": "not-configured",
            "payoutCustody": "external",
            "payoutNotice": "notice",
        }

    base = {"owner": "alice", "name": "demo", "visibility": "public"}
    assert not snapshot_contract.validate_snapshot_payload(
        "/api/repositories", catalog({**base, "keyEpoch": False}), 1000
    )
    assert not snapshot_contract.validate_snapshot_payload(
        "/api/repositories",
        catalog({
            **base,
            "logoMetadata": {
                "languages": {
                    "Python": snapshot_contract.MAX_SAFE_INTEGER + 1,
                },
            },
        }),
        1000,
    )


class _AbortSignal:
    @staticmethod
    def timeout(value):
        return value


class _JsRequest:
    inputs = []
    inits = []

    @staticmethod
    def new(value, init=None):
        _JsRequest.inputs.append(value)
        _JsRequest.inits.append(init)
        if isinstance(value, Request):
            return Request(value)
        options = dict(init or {})
        if "headers" in options or "method" in options or "body" in options:
            built = Request(
                value,
                method=str(options.get("method") or "GET"),
                headers=options.get("headers"),
                body=options.get("body"),
            )
            # Workers exposes the abort deadline on the request itself, and a
            # bodied internal request carries one just like a bodyless one.
            built.signal = options.get("signal")
            return built
        return SimpleNamespace(url=value, signal=options.get("signal"))


class _JsPromise:
    @staticmethod
    def allSettled(promises):
        async def settle():
            values = await asyncio.gather(*promises, return_exceptions=True)
            return [
                SimpleNamespace(status="rejected", reason=value)
                if isinstance(value, BaseException)
                else SimpleNamespace(status="fulfilled", value=value)
                for value in values
            ]

        return asyncio.create_task(settle())


def _scheduler_namespace():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in {
                "_cron_due_phases",
                "_cron_tick_phase_order",
                "_cron_tick_detail",
                "_cron_tick_record",
            }
        )
    ]
    default = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
    )
    scheduled = next(
        node for node in default.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "scheduled"
    )
    class_node = ast.ClassDef(
        name="Default", bases=[], keywords=[], body=[scheduled], decorator_list=[]
    )
    module = ast.fix_missing_locations(ast.Module(
        body=[*selected, class_node], type_ignores=[]
    ))
    namespace = {
        "CRON_TRIGGER_KICK_TIMEOUT_MS": 3_000,
        "CRON_DISPATCH_TIMEOUT_MS": 35_000,
        "CRON_BATCH_MAX_OBJECTS": 13,
        "CRON_BATCH_MAX_PHASES": 24,
        "CRON_PHASE_REPLAY_SAFE": frozenset({
            "status-compact", "probe-flagship"}),
        "CRON_STATUS_CHAIN": (
            "status-sample", "status-alert-pings", "status-alert-email",
            "snapshot-status"),
        "CRON_WATCHDOG_PHASE": "watchdog-completion",
        "CRON_TICK_DETAIL_MAX_CHARS": 8 * 1024,
        "CRON_TICK_RETAIN_MS": 6 * 60 * 60 * 1000,
        "CRON_RUNNER_INTERVAL_MS": 60_000,
        "CRON_RUNNER_NAME": "scheduled-runner-v1",
        "clean_string": lambda value, limit: str(value)[:limit],
        "d1_run": None,
        "js_scheduler": SimpleNamespace(),
        "json": json,
        "Date": SimpleNamespace(now=lambda: 0),
        "JsAbortSignal": _AbortSignal,
        "JsPromise": _JsPromise,
        "JsRequest": _JsRequest,
        "public_snapshots": snapshot_contract,
        "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES": 3,
        "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES": 5,
        "WORLD_SATELLITE_REFRESH_CRON_MINUTES": 180,
        "WORLD_SATELLITE_REFRESH_CRON_OFFSET": 11,
        "FLAGSHIP_CHECK_INTERVAL_MINUTES": 6 * 60,
        "_safe_error_text": str,
        "js_nullish": lambda value: value is None,
        "quote": quote,
        "to_js": lambda value: value,
    }
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


def test_app_scheduler_only_doorbells_the_coordinator():
    """The Cron Trigger rings one bell; every phase runs in that object's alarm.

    The stateless scheduled handler must stay a tiny doorbell: no phase, no
    batch object, no status pre-sample, and never a Worker exception for a
    doorbell the platform rejected (the coordinator's own alarm chain and the
    independent watchdog already cover a missed minute).
    """
    namespace = _scheduler_namespace()

    class Binding:
        def __init__(self, rejection=None):
            self.rejection = rejection
            self.names = []
            self.timeouts = []

        def idFromName(self, name):
            self.names.append(name)
            return name

        def get(self, object_id):
            binding = self

            class Stub:
                async def fetch(self, request):
                    binding.timeouts.append(request.signal)
                    if binding.rejection is not None:
                        raise binding.rejection
                    assert object_id == "scheduled-runner-v1"
                    assert request.url.endswith("/cron-runner/kick")
                    return SimpleNamespace(status=200)

            return Stub()

    async def doorbell(binding):
        worker = namespace["Default"]()
        worker.env = SimpleNamespace(FORKMESH_CRON_RUNNER=binding)
        return await worker.scheduled(SimpleNamespace(), None, object())

    kicked = Binding()
    assert asyncio.run(doorbell(kicked)) is True
    assert kicked.names == ["scheduled-runner-v1"]
    assert kicked.timeouts == [3_000]

    rejected = Binding(RuntimeError(
        "Exceeded allowed duration in Durable Objects free tier"))
    assert asyncio.run(doorbell(rejected)) is False

    # The doorbell is all the stateless handler knows how to do; the phase
    # schedule it used to dispatch is still declared here for the tick.
    ordered, chain_length = namespace["_cron_tick_phase_order"](50)
    due = namespace["_cron_due_phases"](50)
    assert ordered[-1] == "watchdog-completion"
    assert sorted(ordered[:-1]) == sorted(due)
    assert ordered[:chain_length] == tuple(
        phase for phase in (
            "status-sample", "status-alert-pings", "status-alert-email",
            "snapshot-status") if phase in due)


def test_consolidated_app_manifest_routes_and_control_contracts():
    config = tomllib.loads((APP / "wrangler.toml").read_text(encoding="utf-8"))
    assert config["name"] == "forkmesh-next-app"
    assert config["routes"] == [{
        "pattern": "app.next.forkmesh.com", "custom_domain": True,
    }]
    assert config["triggers"]["crons"] == ["* * * * *"]
    assert [item["binding"] for item in config["d1_databases"]] == ["DB"]
    assert not (APP / "edge-control/worker.js").exists()
    assert not (APP / "edge-control/wrangler.toml").exists()

    route = ENTRY_TEXT.split("    async def _route(self, request, url):", 1)[1]
    assert "return await self._mirror_identity_control_response(request)" in route
    assert "redirect = self._app_root_redirect(request, url)" in route
    assert "return await self._health_control_response(request, url)" in route
    assert "return self._version_control_response(request)" in route


def test_deploy_has_exact_worker_override_and_two_stage_cutover_without_edge_deploy():
    deploy = (APP / "deploy.sh").read_text(encoding="utf-8")
    assert 'APP_WORKER_NAME="${FORKMESH_APP_WORKER_NAME:-forkmesh-next-app}"' in deploy
    assert 'SPLIT_SECRET_WORKER="$APP_WORKER_NAME"' in deploy
    invariant = "set -euo pipefail\nENV_FILE=" + deploy.split(
        "ENV_FILE=", 1
    )[1].split("trim() {", 1)[0]
    assert '${FORKMESH_APP_WORKER_NAME:-}" != "forkmesh-relay"' in invariant
    rejected = subprocess.run(
        ["bash", "-c", invariant],
        env={"PATH": "/usr/bin:/bin", "FORKMESH_APP_CONSOLIDATION_PREPARE": "1"},
        text=True,
        capture_output=True,
        check=False,
    )
    assert rejected.returncode == 2
    assert "explicitly target the existing forkmesh-relay" in rejected.stderr
    accepted = subprocess.run(
        ["bash", "-c", invariant],
        env={
            "PATH": "/usr/bin:/bin",
            "FORKMESH_APP_CONSOLIDATION_PREPARE": "1",
            "FORKMESH_APP_WORKER_NAME": "forkmesh-relay",
            "FORKMESH_LEGACY_EDGE_QUIESCED": "1",
        },
        text=True,
        capture_output=True,
        check=False,
    )
    assert accepted.returncode == 0, accepted.stderr
    deploy_version = deploy.split("deploy_app_version() {", 1)[1].split(
        "require_edge_router_public_key()", 1
    )[0]
    assert 'local worker_args=(--name "$APP_WORKER_NAME")' in deploy_version
    assert "deploy_edge_control_version" not in deploy
    assert "cd edge-control" not in deploy
    app_block = deploy.split("deploy_app_target() {", 1)[1].split(
        "initial_official_cutover_needed()", 1
    )[0]
    assert '[ "$APP_CONSOLIDATION_PREPARE" != "1" ]' in app_block
    assert app_block.index("deploy_app_version") < app_block.index(
        "wait_for_public_snapshot_rows"
    )
    assert app_block.index("deploy_app_version") < app_block.index(
        "expire_legacy_public_snapshot_rows"
    ) < app_block.index("wait_for_public_snapshot_rows")
    legacy_expiry = deploy.split("expire_legacy_public_snapshot_rows() {", 1)[1].split(
        "verify_public_snapshot_routes()", 1
    )[0]
    assert "fresh_until=generated_at" in legacy_expiry
    assert "stale_until=generated_at" in legacy_expiry
    assert "WHERE kind='forkmesh-public-api-snapshot'" in legacy_expiry
    assert "FORKMESH_LEGACY_SNAPSHOT_EXPIRE_ATTEMPTS:-3" in legacy_expiry
    assert (
        'pywrangler d1 execute "$database" --remote --command "$sql" &&'
        in legacy_expiry
    )
    assert "require_legacy_public_snapshot_rows_expired" in app_block
    prepare = app_block.split('if [ "$APP_CONSOLIDATION_PREPARE" = "1" ]', 1)[1]
    assert "legacy edge routes were not changed" in prepare
    assert "verify_app_control" in prepare
    assert "verify_public_snapshot_routes" in prepare
    changed = deploy.split("deploy_target_is_changed() {", 1)[1].split(
        "mark_deploy_target()", 1
    )[0]
    assert "APP_CONSOLIDATION_PREPARE" not in changed
    assert "x-forkmesh-worker" in deploy
    assert "x-forkmesh-edge-control" in deploy
    assert '[ -z "$legacy_marker" ]' in deploy
    snapshot_wait = deploy.split("wait_for_public_snapshot_rows() {", 1)[1].split(
        "require_legacy_public_snapshot_rows_expired()", 1
    )[0]
    assert "required_ready" in snapshot_wait
    assert "mirror_ready" in snapshot_wait
    assert "mirror_active" in snapshot_wait
    assert '"$required_ready" = "4"' in snapshot_wait
    assert '"$mirror_active" = "0"' in snapshot_wait
    assert "four required public snapshots" in snapshot_wait
    route_verify = deploy.split("verify_public_snapshot_routes() {", 1)[1].split(
        "verify_app_control()", 1
    )[0]
    assert '[ "$bypass_snapshot" = "fresh" ]' in route_verify
    assert '[ "$auth_snapshot" = "fresh" ]' in route_verify
    assert '[ "$bypass_fingerprint" = "$DEPLOY_TARGET_FINGERPRINT" ]' in route_verify
    assert '[ "$auth_fingerprint" = "$DEPLOY_TARGET_FINGERPRINT" ]' in route_verify
    routes = deploy.split("public_snapshot_routes_current() {", 1)[1].split(
        "all_public_snapshot_routes_current()", 1
    )[0]
    assert '"/api/repo/forkmesh/forkmesh/mirrors"' in routes
    assert '"missing-fallback"' in routes


_FAKE_CURL = '''#!/usr/bin/env python3
import os
import sys

url = sys.argv[-1]
with open(os.environ["FAKE_CURL_LOG"], "a", encoding="utf-8") as handle:
    handle.write(url + "\\n")


def take(name):
    counter = os.environ["FAKE_CURL_LOG"] + "." + name
    sequence = os.environ[name].split(",")
    try:
        with open(counter, encoding="utf-8") as handle:
            index = int(handle.read())
    except (OSError, ValueError):
        index = 0
    with open(counter, "w", encoding="utf-8") as handle:
        handle.write(str(index + 1))
    return sequence[min(index, len(sequence) - 1)]


if url.endswith("/api/version"):
    status = take("FAKE_VERSION_STATUS")
    body = ""
    if status == "200":
        body = ('{"worker":"app","deployFingerprint":"'
                + os.environ["FAKE_FINGERPRINT"] + '"}')
    sys.stdout.write(body + "\\n" + status)
else:
    sys.stdout.write(take("FAKE_LANDING_STATUS"))
'''


def _run_live_app_surface(tmp_path, *, landing, version="200", attempts="4",
                          fingerprint=FINGERPRINT):
    """Run deploy.sh's final smoke check against a scripted origin."""
    deploy = (APP / "deploy.sh").read_text(encoding="utf-8")
    function = "verify_live_app_surface() {" + deploy.split(
        "verify_live_app_surface() {", 1
    )[1].split("\ndeploy_app_target() {", 1)[0]
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    curl = bin_dir / "curl"
    curl.write_text(_FAKE_CURL, encoding="utf-8")
    curl.chmod(0o755)
    script = tmp_path / "surface.sh"
    script.write_text(
        "set -u\n" + function
        + '\nverify_live_app_surface "https://app.example"\n',
        encoding="utf-8",
    )
    log = tmp_path / "curl.log"
    result = subprocess.run(
        ["bash", str(script)],
        env={
            "PATH": str(bin_dir) + ":/usr/bin:/bin",
            "DEPLOY_TARGET_FINGERPRINT": fingerprint,
            "FORKMESH_DEPLOY_VERIFY_ATTEMPTS": attempts,
            "FORKMESH_DEPLOY_VERIFY_BACKOFF": "0",
            "FAKE_CURL_LOG": str(log),
            "FAKE_CURL_STATE": str(log),
            "FAKE_FINGERPRINT": FINGERPRINT,
            "FAKE_VERSION_STATUS": version,
            "FAKE_LANDING_STATUS": landing,
        },
        text=True,
        capture_output=True,
        check=False,
    )
    calls = log.read_text(encoding="utf-8").splitlines() if log.exists() else []
    return result, calls


def test_final_live_surface_smoke_retries_transient_ingress_failures(tmp_path):
    # An ingress Durable Object abort answers a deliberately marked, retryable
    # 503 (log_durable_object_abort). The deploy that already verified its own
    # build must not fail on one of those, the way deploy 8ccadb1c8d68 did.
    result, calls = _run_live_app_surface(tmp_path, landing="503,503,200")

    assert result.returncode == 0, result.stderr
    assert len([call for call in calls if call.endswith("/api")]) == 3
    assert "/api returned HTTP 503; retrying" in result.stderr
    assert "Verified the live App surface" in result.stdout


def test_final_live_surface_smoke_retries_an_unreachable_version_route(tmp_path):
    result, calls = _run_live_app_surface(
        tmp_path, landing="200", version="503,200"
    )

    assert result.returncode == 0, result.stderr
    assert "/api/version returned HTTP 503; retrying" in result.stderr
    assert len([call for call in calls if call.endswith("/api/version")]) == 2


def test_final_live_surface_smoke_still_fails_on_a_sustained_outage(tmp_path):
    result, calls = _run_live_app_surface(
        tmp_path, landing="503", attempts="3"
    )

    assert result.returncode != 0
    assert len([call for call in calls if call.endswith("/api")]) == 3
    assert "ERROR: https://app.example/api returned HTTP 503 after 3 attempts." \
        in result.stderr


def test_final_live_surface_smoke_still_fails_on_a_stale_fingerprint(tmp_path):
    result, _calls = _run_live_app_surface(
        tmp_path, landing="200", attempts="2", fingerprint="b" * 64
    )

    assert result.returncode != 0
    assert "is not reporting the deployed App fingerprint after 2 attempts" \
        in result.stderr
