import ast
import json
import re
import subprocess
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
ENTRY = ROOT / "src" / "entry.py"
CLIENT = REPOSITORY / "www" / "public" / "api-client.js"


FUNCTIONS = {
    "_normalized_http_origin",
    "_request_origin",
    "_request_target_origin",
    "_request_same_origin",
    "_cors_allowed_origins",
    "_request_origin_allowed",
    "_merge_vary",
    "_cors_headers",
    "_api_cors_preflight_response",
    "_api_cors_response",
    "world_websocket_origin_allowed",
}
CONSTANTS = {
    "_CORS_CANONICAL_FRONTEND_ORIGINS",
    "_CORS_ORIGIN_ENV_NAMES",
    "_CORS_METHODS",
    "_CORS_DEFAULT_HEADERS",
    "_CORS_EXPOSE_HEADERS",
    "_CORS_HEADER_TOKEN_RE",
}


class Headers(dict):
    def __init__(self, values=None):
        entries = values.items() if hasattr(values, "items") else (values or [])
        super().__init__({
            str(name).lower(): value
            for name, value in entries
        })

    def get(self, name, default=None):
        return super().get(str(name).lower(), default)


class Request:
    def __init__(self, url, method="GET", headers=None):
        self.url = url
        self.method = method
        self.headers = Headers(headers)


class Response:
    def __init__(self, body=None, status=200, status_text="", headers=None):
        self.body = body
        self.status = status
        self.status_text = status_text
        self.headers = Headers(headers)
        self.js_object = self


def _runtime():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in FUNCTIONS:
                selected.append(node)
            continue
        if isinstance(node, ast.Assign):
            names = {
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            }
            if names & CONSTANTS:
                selected.append(node)
    namespace = {
        "Response": Response,
        "json_response": lambda data, status=200, extra_headers=None, **kwargs: (
            Response(
                json.dumps(data), status=status,
                headers={
                    **({"cache-control": kwargs["cache_control"]}
                       if kwargs.get("cache_control") else {}),
                    **(extra_headers or {}),
                },
            )
        ),
        "method_name": lambda request: request.method.upper(),
        "re": re,
        "urlparse": urlparse,
    }
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    assert FUNCTIONS <= namespace.keys()
    return namespace


def _env(**values):
    defaults = {
        "PUBLIC_BASE_URL": "https://forkmesh.com",
        "API_ORIGIN": "https://app.forkmesh.com",
        "APP_ORIGIN": "https://app.forkmesh.com",
        "WWW_ORIGIN": "https://forkmesh.com",
        "WORLD_ORIGIN": "https://world.forkmesh.com",
        "CORS_ALLOWED_ORIGINS": "",
        "SINGLE_WORKER_SITE": "false",
    }
    defaults.update(values)
    return SimpleNamespace(**defaults)


def test_canonical_frontends_receive_credentialed_cors():
    runtime = _runtime()
    for origin in (
        "https://forkmesh.com",
        "https://www.forkmesh.com",
        "https://app.forkmesh.com",
        "https://world.forkmesh.com",
    ):
        request = Request(
            "https://app.forkmesh.com/api/version",
            headers={"origin": origin},
        )
        assert runtime["_request_origin_allowed"](_env(), request)
        headers = runtime["_cors_headers"](_env(), request)
        assert headers["access-control-allow-origin"] == origin
        assert headers["access-control-allow-credentials"] == "true"
        assert "Origin" in headers["vary"]

def test_untrusted_origins_are_not_reflected_and_configured_instances_are():
    runtime = _runtime()
    foreign = Request(
        "https://app.forkmesh.com/api/version",
        headers={"origin": "https://evil.example"},
    )
    assert not runtime["_request_origin_allowed"](_env(), foreign)
    assert runtime["_cors_headers"](_env(), foreign) == {}

    configured = _env(
        CORS_ALLOWED_ORIGINS=(
            "https://console.mesh.example,https://world.mesh.example"
        ),
    )
    remote = Request(
        "https://app.forkmesh.com/api/version",
        headers={"origin": "https://console.mesh.example"},
    )
    assert runtime["_request_origin_allowed"](configured, remote)
    assert "http://console.mesh.example" not in runtime[
        "_cors_allowed_origins"](configured, remote)

    independent_instance = _env(
        PUBLIC_BASE_URL="https://mesh.example",
        API_ORIGIN="https://mesh.example",
        APP_ORIGIN="https://mesh.example",
        WWW_ORIGIN="https://mesh.example",
        WORLD_ORIGIN="https://mesh.example",
        CORS_ALLOWED_ORIGINS="https://mesh.example",
        SINGLE_WORKER_SITE="true",
    )
    central_app = Request(
        "https://mesh.example/api/accounts/login",
        headers={"origin": "https://app.forkmesh.com"},
    )
    assert not runtime["_request_origin_allowed"](
        independent_instance, central_app)

    custom_world = _env(
        PUBLIC_BASE_URL="https://forkmesh.example",
        API_ORIGIN="https://forkmesh.example",
        APP_ORIGIN="https://forkmesh.example",
        WWW_ORIGIN="https://forkmesh.example",
        WORLD_ORIGIN="https://forkmesh-world.example",
        CORS_ALLOWED_ORIGINS=(
            "https://forkmesh.example,https://forkmesh-world.example"
        ),
        SINGLE_WORKER_SITE="true",
    )
    world_request = Request(
        "https://forkmesh.example/api/world/context",
        headers={"origin": "https://forkmesh-world.example"},
    )
    assert runtime["_request_origin_allowed"](custom_world, world_request)


def test_preflight_allows_requested_headers_and_private_network_opt_in():
    runtime = _runtime()
    request = Request(
        "https://app.forkmesh.com/api/repo/alice/project/issues",
        method="OPTIONS",
        headers={
            "origin": "https://app.forkmesh.com",
            "access-control-request-method": "POST",
            "access-control-request-headers": (
                "content-type, authorization, x-forkmesh-signature"
            ),
            "access-control-request-private-network": "true",
        },
    )
    response = runtime["_api_cors_preflight_response"](
        _env(), request, "/api/repo/alice/project/issues")
    assert response.status == 204
    assert "POST" in response.headers["access-control-allow-methods"]
    assert "x-forkmesh-signature" in response.headers[
        "access-control-allow-headers"]
    assert response.headers["access-control-allow-private-network"] == "true"
    assert response.headers["access-control-max-age"] == "86400"
    assert "Access-Control-Request-Headers" in response.headers["vary"]


def test_cors_wrapper_preserves_response_headers_and_varies_by_origin():
    runtime = _runtime()
    request = Request(
        "https://app.forkmesh.com/api/accounts/login",
        method="POST",
        headers={"origin": "https://app.forkmesh.com"},
    )
    original = Response(
        "{}", status=201,
        headers={
            "cache-control": "no-store",
            "set-cookie": "forkmesh_account=token; Secure; HttpOnly",
            "vary": "Accept-Encoding",
        },
    )
    response = runtime["_api_cors_response"](
        _env(), request, original, "/api/accounts/login")
    assert response.status == 201
    assert response.headers["set-cookie"].startswith("forkmesh_account=")
    assert response.headers["access-control-allow-origin"] == (
        "https://app.forkmesh.com")
    assert response.headers["access-control-allow-credentials"] == "true"
    assert response.headers["vary"] == "Accept-Encoding, Origin"


def test_websocket_accepts_trusted_frontends_only_when_environment_is_bound():
    runtime = _runtime()
    trusted = Request(
        "https://app.forkmesh.com/api/world/ws",
        headers={"origin": "https://world.forkmesh.com"},
    )
    foreign = Request(
        "https://app.forkmesh.com/api/world/ws",
        headers={"origin": "https://evil.example"},
    )
    assert runtime["world_websocket_origin_allowed"](trusted, _env())
    assert not runtime["world_websocket_origin_allowed"](foreign, _env())


def test_every_standalone_page_installs_api_routing_before_other_scripts():
    pages = []
    for unit in ("app", "www", "world"):
        pages.extend((REPOSITORY / unit / "public").rglob("*.html"))
    for path in sorted(pages):
        if "/dashboard/partials/" in path.as_posix():
            continue
        html = path.read_text(encoding="utf-8")
        assert 'src="/api-client.js' in html, path
        first_script = html.lower().find("<script")
        api_script = html.find('src="/api-client.js')
        assert first_script <= api_script, path
        prefix = html[first_script:api_script]
        assert "src=" not in prefix, path


def test_browser_runtime_routes_http_and_websocket_api_calls():
    harness = r'''
const fs = require("fs");
const assert = require("assert");
const calls = [];
const sockets = [];
const values = new Map();
global.location = new URL("https://app.forkmesh.com/dashboard");
global.document = { querySelector() { return null; } };
global.localStorage = {
  getItem(key) { return values.has(key) ? values.get(key) : null; },
  setItem(key, value) { values.set(key, String(value)); },
  removeItem(key) { values.delete(key); },
};
global.CustomEvent = class { constructor(type, init) { this.type = type; this.detail = init.detail; } };
class FakeWebSocket {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
  constructor(url, protocols) { this.url = url; this.protocols = protocols; sockets.push(this); }
}
const nativeFetch = async (input, init) => {
  calls.push({ url: input instanceof Request ? input.url : String(input), init: init || {} });
  return { ok: true };
};
global.window = {
  fetch: nativeFetch,
  WebSocket: FakeWebSocket,
  dispatchEvent() {},
};
eval(fs.readFileSync(process.argv[1], "utf8"));
(async () => {
  await window.fetch("/api/version", { credentials: "same-origin" });
  assert.equal(calls[0].url, "/api/version");
  assert.equal(calls[0].init.credentials, "include");
  await window.fetch("/assets/logo.png");
  assert.equal(calls[1].url, "/assets/logo.png");
  const socket = new window.WebSocket("wss://app.forkmesh.com/api/world/ws");
  assert.equal(socket.url, "wss://app.forkmesh.com/api/world/ws");
  assert.equal(window.ForkMeshAPI.origin, "https://app.forkmesh.com");
  assert.equal(window.ForkMeshAPI.isSameSite, true);
  window.ForkMeshAPI.configure("https://api.mesh.example");
  assert.equal(window.ForkMeshAPI.origin, "https://api.mesh.example");
  assert.equal(window.ForkMeshAPI.isSameSite, false);
  await window.fetch("/api/accounts/sessions");
  assert.equal(calls[2].url, "https://api.mesh.example/api/accounts/sessions");
  assert.equal(calls[2].init.credentials, "include");
})().catch((error) => { console.error(error); process.exit(1); });
'''
    completed = subprocess.run(
        ["node", "-e", harness, str(CLIENT)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_custom_world_origin_routes_fetch_and_websocket_to_self_host_app():
    harness = r'''
const fs = require("fs");
const assert = require("assert");
const calls = [];
global.location = new URL("https://forkmesh-world.theirdomain.com/");
global.document = {
  querySelector(selector) {
    assert.equal(selector, 'meta[name="forkmesh-api-origin"]');
    return { content: "https://forkmesh.theirdomain.com" };
  },
};
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
global.CustomEvent = class { constructor(type, init) { this.type = type; this.detail = init.detail; } };
class FakeWebSocket {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
  constructor(url) { this.url = url; }
}
global.window = {
  fetch: async (input, init) => {
    calls.push({ url: input instanceof Request ? input.url : String(input), init: init || {} });
    return { ok: true };
  },
  WebSocket: FakeWebSocket,
  dispatchEvent() {},
};
eval(fs.readFileSync(process.argv[1], "utf8"));
(async () => {
  assert.equal(window.ForkMeshAPI.origin, "https://forkmesh.theirdomain.com");
  await window.fetch("/api/version");
  assert.equal(calls[0].url, "https://forkmesh.theirdomain.com/api/version");
  assert.equal(calls[0].init.credentials, "include");
  const socket = new window.WebSocket("wss://forkmesh-world.theirdomain.com/api/world/ws");
  assert.equal(socket.url, "wss://forkmesh.theirdomain.com/api/world/ws");
  await window.fetch("/api");
  assert.equal(calls[1].url, "/api");
})().catch((error) => { console.error(error); process.exit(1); });
'''
    completed = subprocess.run(
        ["node", "-e", harness, str(CLIENT)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_legacy_official_api_origin_migrates_without_touching_remote_instances():
    harness = r'''
const fs = require("fs");
const assert = require("assert");
const values = new Map([["forkmesh.apiOrigin", "https://app.forkmesh.com"]]);
global.location = new URL("https://forkmesh.com/");
global.document = { querySelector() { return null; } };
global.localStorage = {
  getItem(key) { return values.has(key) ? values.get(key) : null; },
  setItem(key, value) { values.set(key, String(value)); },
  removeItem(key) { values.delete(key); },
};
global.CustomEvent = class { constructor(type, init) { this.type = type; this.detail = init.detail; } };
class FakeWebSocket {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
  constructor(url) { this.url = url; }
}
global.window = {
  fetch: async () => ({ ok: true }),
  WebSocket: FakeWebSocket,
  dispatchEvent() {},
};
eval(fs.readFileSync(process.argv[1], "utf8"));
assert.equal(window.ForkMeshAPI.origin, "https://app.forkmesh.com");
assert.equal(values.get("forkmesh.apiOrigin"), "https://app.forkmesh.com");
window.ForkMeshAPI.configure("https://remote.mesh.example");
assert.equal(window.ForkMeshAPI.origin, "https://remote.mesh.example");
assert.equal(values.get("forkmesh.apiOrigin"), "https://remote.mesh.example");
window.ForkMeshAPI.configure("https://app.forkmesh.com");
assert.equal(window.ForkMeshAPI.origin, "https://app.forkmesh.com");
assert.equal(values.get("forkmesh.apiOrigin"), "https://app.forkmesh.com");
'''
    completed = subprocess.run(
        ["node", "-e", harness, str(CLIENT)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def _websocket_urls(page, inputs):
    harness = r'''
const fs = require("fs");
global.location = new URL(process.argv[2]);
global.document = { querySelector() { return null; } };
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
global.CustomEvent = class { constructor(type, init) { this.type = type; this.detail = init.detail; } };
class FakeWebSocket {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
  constructor(url) {
    const parsed = new URL(url);
    if (!["ws:", "wss:"].includes(parsed.protocol) || parsed.hash) {
      throw new SyntaxError("Failed to construct 'WebSocket': The URL '" + url + "' is invalid.");
    }
    this.url = url;
  }
}
global.window = { fetch: async () => ({ ok: true }), WebSocket: FakeWebSocket, dispatchEvent() {} };
eval(fs.readFileSync(process.argv[1], "utf8"));
const out = JSON.parse(process.argv[3]).map((value) => {
  try { return new window.WebSocket(value).url; } catch (error) { return "THROW " + error.message; }
});
process.stdout.write(JSON.stringify(out));
'''
    completed = subprocess.run(
        ["node", "-e", harness, str(CLIENT), page, json.dumps(inputs)],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout)


def test_websocket_routing_always_hands_the_browser_a_ws_scheme():
    assert _websocket_urls("https://app.forkmesh.com/status", [
        "wss://app.forkmesh.com/api/status/ws",
        "https://app.forkmesh.com/api/status/ws",
        "/api/status/ws",
        "wss://app.forkmesh.com/api/status/ws#live",
        "https://relay.example/socket?x=1",
    ]) == [
        "wss://app.forkmesh.com/api/status/ws",
        "wss://app.forkmesh.com/api/status/ws",
        "wss://app.forkmesh.com/api/status/ws",
        "wss://app.forkmesh.com/api/status/ws",
        "wss://relay.example/socket?x=1",
    ]
    assert _websocket_urls("https://forkmesh.com/status", [
        "wss://forkmesh.com/api/status/ws",
        "/api/status/ws",
    ]) == [
        "wss://app.forkmesh.com/api/status/ws",
        "wss://app.forkmesh.com/api/status/ws",
    ]
    assert _websocket_urls("http://localhost:8787/status", [
        "/api/status/ws",
        "http://localhost:8787/api/status/ws",
    ]) == [
        "ws://localhost:8787/api/status/ws",
        "ws://localhost:8787/api/status/ws",
    ]


def test_status_stream_survives_a_websocket_constructor_failure():
    html = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    start = html.index("        function scheduleReconnect() {")
    end = html.index('        document.addEventListener("visibilitychange"')
    harness = r'''
let disposed = false, statusSocket = null, reconnectTimer = 0, reconnectDelay = 1000;
const scheduled = [];
const document = { hidden: false };
const location = { protocol: "https:", host: "app.forkmesh.com" };
const window = { setTimeout(fn, ms) { scheduled.push(ms); return scheduled.length; } };
let attempts = 0;
function WebSocket() {
  attempts += 1;
  throw new SyntaxError("Failed to construct 'WebSocket': Failed to create a WebSocket: the provided URL is invalid.");
}
function render() {}
''' + html[start:end] + r'''
connectStatusStream();
if (attempts !== 1) throw new Error("expected one attempt, got " + attempts);
if (statusSocket !== null) throw new Error("socket should stay unset");
if (scheduled.length !== 1 || scheduled[0] !== 1000) throw new Error("reconnect not scheduled: " + scheduled);
'''
    completed = subprocess.run(
        ["node", "-e", harness], cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert completed.returncode == 0, completed.stderr
