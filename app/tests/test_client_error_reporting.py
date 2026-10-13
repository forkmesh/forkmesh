"""Browser-error persistence and administrator World HUD contracts."""

import ast
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8") + "\n"
    + (ROOT / "src" / "admin_console.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
REPORTER = (ROOT.parent / "www" / "public" / "analytics.js").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
WORLD_CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(
    encoding="utf-8")


def _client_error_namespace():
    tree = ast.parse(ENTRY)
    names = {
        "CLIENT_ERROR_SURFACES",
        "CLIENT_ERROR_KINDS",
    }
    body = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in names
            for target in node.targets
        ):
            body.append(node)
        elif (
            isinstance(node, ast.FunctionDef)
            and node.name in {
                "_sanitize_client_error_text",
                "_client_error_is_injected_only",
                "_client_error_is_non_actionable",
                "_client_error_fields",
            }
        ):
            body.append(node)
    namespace = {"re": re}
    exec(compile(ast.Module(body=body, type_ignores=[]), "<entry>", "exec"),
         namespace)
    return namespace


def _client_error_fields():
    return _client_error_namespace()["_client_error_fields"]


def test_uncaught_errors_and_rejections_use_the_private_collector():
    assert 'window.addEventListener(\n    "error"' in REPORTER
    assert 'window.addEventListener("unhandledrejection"' in REPORTER
    assert 'const CLIENT_ERROR_ENDPOINT = "/api/client-errors"' in REPORTER
    assert 'credentials: "same-origin"' in REPORTER
    assert "keepalive: true" in REPORTER
    assert "recentClientErrors" in REPORTER
    assert "[redacted-secret]" in REPORTER
    assert "[redacted-email]" in REPORTER
    assert "[redacted-id]" in REPORTER
    assert "[redacted-url]" in REPORTER


def test_expected_abort_rejections_do_not_pollute_operational_errors():
    assert 'reason?.name === "AbortError"' in REPORTER
    assert "signal is aborted|operation was aborted" in REPORTER


def test_browser_runtime_noise_does_not_pollute_operational_errors():
    assert "function knownNonActionableBrowserError(" in REPORTER
    assert "knownNonActionableBrowserError(error, fallbackMessage)" in REPORTER
    assert "script error" in REPORTER.lower()
    assert "resizeobserver loop" in REPORTER.lower()
    assert "runtime\\.sendmessage" in REPORTER.lower()
    assert "iabjs:" in REPORTER

    namespace = _client_error_namespace()
    non_actionable = namespace["_client_error_is_non_actionable"]
    injected_only = namespace["_client_error_is_injected_only"]
    for message in (
        "Script error.",
        "ResizeObserver loop limit exceeded",
        "ResizeObserver loop completed with undelivered notifications.",
        "Invalid call to runtime.sendMessage(). Tab not found.",
    ):
        assert non_actionable({"message": message})
    assert not non_actionable({"message": "TypeError: render failed"})
    assert injected_only(
        "Error at sendDataToNative (iabjs://bridge-id:1:10025)"
    )


def test_client_collector_is_bounded_redacted_and_stored_in_error_log():
    assert "async def client_error_handler(env, request):" in ENTRY
    assert "_request_same_origin(request)" in ENTRY
    assert "CLIENT_ERROR_MAX_BODY" in ENTRY
    assert "CLIENT_ERROR_RATE_PER_CLIENT" in ENTRY
    assert "CLIENT_ERROR_RATE_GLOBAL" in ENTRY
    assert "await _write_error_log(" in ENTRY
    assert '"/client-error/" + surface' in ENTRY
    assert (
        'if url.path in ("/api/client-errors", "/api/client-errors/"):'
        in APP_RUNTIME
    )


def test_discarded_world_sessions_are_stored_without_a_new_group_ping():
    fields = _client_error_fields()({
        "kind": "discard",
        "surface": "world",
        "message": "World reloaded after the browser closed it in the background",
    })
    assert fields is not None
    assert fields[1].startswith("client discard [world] World reloaded")
    handler = ENTRY[ENTRY.index("async def client_error_handler(env, request):"):]
    handler = handler[:handler.index("\n\n\n")]
    assert (
        'notify=str(payload.get("kind") or "").strip().lower() != "discard",'
        in handler
    )


def test_client_errors_are_identified_as_javascript_in_admin():
    assert "def _admin_error_source(method, path):" in ENTRY
    assert 'str(method or "").strip().upper() == "JS"' in ENTRY
    assert 'str(path or "").startswith("/client-error/")' in ENTRY
    assert 'return "JavaScript" if is_javascript else "Worker"' in ENTRY
    assert 'class="src %s"' in ENTRY


def test_client_error_fields_remove_identity_urls_and_credentials():
    fields = _client_error_fields()({
        "kind": "error",
        "surface": "world",
        "message": (
            "token=super-secret-value user@example.com "
            "https://forkmesh.com/alice/private-repo?token=abc"),
        "stack": (
            "at run (https://forkmesh.com/alice/private-repo/world.js:44:2)"),
        "source": "https://forkmesh.com/private/path/world.js?account=alice",
        "line": 44,
        "column": 2,
    })
    assert fields is not None
    surface, detail = fields
    assert surface == "world"
    assert "super-secret-value" not in detail
    assert "user@example.com" not in detail
    assert "alice/private-repo" not in detail
    assert "world.js:44:2" in detail


def test_extension_only_stacks_are_dropped_by_the_reporter():
    assert "function injectedOnlyOrigin(stack, source) {" in REPORTER
    assert "if (injectedOnlyOrigin(error?.stack, source)) return;" in REPORTER
    # The stack decides when it has frames, so an evaluated extension frame
    # attributed to the document does not smuggle the report through.
    assert (
        "const frames = String(stack || \"\").match(STACK_FRAME) || [];\n"
        "    if (frames.length) return frames.every((frame) => "
        "INJECTED_FRAME.test(frame));"
    ) in REPORTER


def test_extension_only_stacks_are_dropped_by_the_worker():
    injected_only = _client_error_namespace()["_client_error_is_injected_only"]
    # The reported group: an extension wrapping addEventListener twice and
    # recursing until the stack overflows. Every frame is injected code.
    assert injected_only(
        "RangeError: Maximum call stack size exceeded "
        "at addEL_hook (<anonymous>:670:24) "
        "at top.addEventListener (<anonymous>:695:9) "
        "at top.addEventListener (<anonymous>:696:20)")
    assert injected_only(
        "TypeError: x at s (chrome-extension://abcdef/content.js:1:220)")
    assert injected_only(
        "Error at h (safari-web-extension://ab/inject.js:9:1)")
    # Ours, redacted or not, and stacks with no frame evidence at all.
    assert not injected_only("TypeError: boom at render ([redacted-url])")
    assert not injected_only(
        "TypeError: boom at render (https://forkmesh.com/blog.js:12:4)")
    assert not injected_only("RangeError: Maximum call stack size exceeded")
    assert not injected_only("")
    assert not injected_only(None)
    # V8 names native frames "<anonymous>" too, and an extension frame can sit
    # above a real one; neither may silence a first-party defect.
    assert not injected_only(
        "TypeError: boom at foo ([redacted-url]) at Array.map (<anonymous>)")
    assert not injected_only(
        "TypeError: boom at foo ([redacted-url]) "
        "at s (chrome-extension://ab/c.js:1:1)")


def test_worker_accepts_but_does_not_store_injected_client_errors():
    # Reporters stay cached in browsers long after a deploy, so the Worker has
    # to hold the same line -- without answering an error report with an error.
    guard = ENTRY.split(
        "    if (_client_error_is_non_actionable(payload)\n"
        "            or _client_error_is_injected_only(payload.get(\"stack\"))):\n",
        1,
    )
    assert len(guard) == 2, "handler no longer screens injected client errors"
    assert "{\"ok\": True, \"stored\": False}" in guard[1][:300]
    assert "status=202" in guard[1][:300]


def test_admin_error_board_opens_from_the_debug_tab():
    assert "async def world_admin_errors_handler(env, request):" in ENTRY
    assert '"platform_administrator"' in ENTRY
    assert '"SELECT COUNT(*) AS n FROM error_log WHERE id>?"' in ENTRY
    assert '"FROM error_log ORDER BY id DESC LIMIT 100"' in ENTRY
    assert 'query.get("include", ["0"])[0] == "1"' in ENTRY
    assert '"/api/world/admin/errors"' in APP_RUNTIME
    assert "ADMIN_ERROR_SEEN_KEY" not in WORLD
    assert "data-world-admin-error-count" not in WORLD
    assert "data-world-admin-errors-open hidden>Logged errors</button>" in WORLD
    assert "this.identity?.isAdmin !== true" in WORLD
    assert "world-admin-error-arrival" not in WORLD_CSS
    assert 'method not in ("GET", "POST", "DELETE")' in ENTRY
    assert '"DELETE FROM error_log WHERE id=?"' in ENTRY
    assert "_admin_error_create_bot_task(" in ENTRY
    assert 'payload["groups"]' in ENTRY
    assert 'payload["hourly"]' in ENTRY
    assert '"hours": [0] * 24' in ENTRY
    assert "world-error-group-table" in WORLD
    assert "world-error-chart" in WORLD
    assert "world-error-sparkline" in WORLD
    assert "world-error-table-header" in WORLD
    assert "data-world-admin-error-delete" in WORLD
    assert "data-world-admin-error-task" in WORLD
    assert "data-world-admin-error-group-delete" in WORLD
    assert "data-world-admin-error-group-task" in WORLD
    assert "data-world-admin-error-copy" in WORLD
    assert "async deleteAdminError(" in WORLD
    assert "async createTaskFromAdminError(" in WORLD
    assert "async deleteAdminErrorGroup(" in WORLD
    assert "async createTaskFromAdminErrorGroup(" in WORLD
    assert "relativeTime(group.firstSeen)" in WORLD
    assert 'title="${escapeHTML(exactTime(group.firstSeen))}"' in WORLD
    assert ".world-error-actor-stack" in WORLD_CSS


def test_the_world_no_longer_polls_for_new_errors():
    assert (
        '"SELECT id,ts,status,method,path FROM error_log ORDER BY id DESC "'
        in ENTRY)
    assert '"latestStatus": latest_status,' in ENTRY
    assert "refreshAdminErrors" not in WORLD
    assert "startAdminErrorPolling" not in WORLD
    assert "/api/world/admin/errors?after=${" not in WORLD
    assert 'data-world-admin-error-detail="' in WORLD
    assert "openAdminErrorDetail(errorId)" in WORLD
    assert 'query.get("id", ["0"])[0]' in ENTRY
    assert '"FROM error_log WHERE id=? LIMIT 1"' in ENTRY


def test_world_admin_error_chart_height_expression_keeps_ternary_balanced():
    assert (
        'style="height:${chartPeak ? Math.max(2, '
        'Math.round((132 * count) / chartPeak)) : 2}px"'
    ) in WORLD


_REJECTION_HARNESS = r"""
const vm = require("node:vm");
const source = require("node:fs").readFileSync(process.argv[1], "utf8");
const listeners = {};
const posted = [];
const window = {
  addEventListener(type, handler) { (listeners[type] ||= []).push(handler); },
  dispatchEvent() {},
};
const context = vm.createContext({
  window,
  location: new URL("https://forkmesh.com/"),
  navigator: {},
  document: { addEventListener() {}, head: { appendChild() {} }, createElement: () => ({}) },
  localStorage: { getItem: () => null, setItem() {} },
  fetch: (url, init) => { posted.push(JSON.parse(init.body)); return Promise.resolve({}); },
  Event,
  EventTarget,
  Response,
  CustomEvent: class extends Event {},
  Map, Set, Date, JSON, Promise, Error, TypeError, String, Number, Math, Object,
  setTimeout,
});
vm.runInContext(source, context);
class Script { get tagName() { return "SCRIPT"; } }
const loadFailure = new Event("error");
Object.defineProperty(loadFailure, "target", { value: new Script() });
const reasons = [
  undefined,
  { status: 401, error: "unauthorized" },
  loadFailure,
  new Response("", { status: 503 }),
  { message: "session expired", stack: "" },
];
for (const reason of reasons) {
  for (const handler of listeners.unhandledrejection || []) handler({ reason });
}
for (const handler of listeners.unhandledrejection || []) {
  handler({ reason: new Error("handled elsewhere"), defaultPrevented: true });
}
setTimeout(() => {
  process.stdout.write(JSON.stringify(posted.map((row) => row.message)));
}, 5);
"""


def test_unhandled_rejections_report_what_was_rejected():
    import json
    import subprocess

    assert "function describeRejectionReason(reason)" in REPORTER
    result = subprocess.run(
        ["node", "-e", _REJECTION_HARNESS,
         str(ROOT.parent / "www" / "public" / "analytics.js")],
        check=True, capture_output=True, text=True)
    assert result.stderr == ""
    messages = json.loads(result.stdout)
    assert messages == [
        "Unhandled promise rejection: undefined",
        "Unhandled promise rejection: Object with keys error, status; error unauthorized",
        'Unhandled promise rejection: Event "error" from script',
        "Unhandled promise rejection: Response status 503",
        "session expired",
    ]
