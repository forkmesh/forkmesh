"""Contracts for the authenticated ForkMesh Streamable HTTP MCP endpoint."""

import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
# The endpoint itself moved into remote_mcp.py (loaded through entry.py's
# remote_mcp_handler proxy) so startup validation never parses it.
REMOTE_MCP = (ROOT / "src" / "remote_mcp.py").read_text(encoding="utf-8")
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + REMOTE_MCP
    + WORLD_ROUTE_SOURCE
)
WRANGLER = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
DASHBOARD = (
    ROOT / "public" / "dashboard" / "js" / "04-account.js"
).read_text(encoding="utf-8")


def _load_task_bridge(overrides):
    tree = ast.parse(ENTRY)
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in {"_public_base_url", "_remote_mcp_task_request"}
    ]
    namespace = {
        "json": json,
        "urlparse": urlparse,
        **overrides,
    }
    exec(compile(ast.Module(body=selected, type_ignores=[]),
                 "entry.py", "exec"), namespace)
    return namespace["_remote_mcp_task_request"]


def test_remote_mcp_is_routed_and_requires_org_bot_authentication():
    assert '"/mcp"' in WRANGLER
    assert '"/mcp/*"' in WRANGLER
    assert 'url.path in ("/mcp", "/mcp/")' in APP_RUNTIME
    assert "return await remote_mcp_handler(self.env, request)" in APP_RUNTIME
    assert '_remote_mcp = _LazyModule("remote_mcp")' in ENTRY
    assert 'remote_mcp_handler = _remote_mcp.export("remote_mcp_handler")' \
        in ENTRY
    handler = REMOTE_MCP[REMOTE_MCP.index("async def remote_mcp_handler("):]
    assert "await _org_bot_token_context(env, request, touch=True)" in handler
    assert '"invalid_bot_token"' in handler
    assert "bounded_json_request(request, max_bytes=64 * 1024)" in handler
    assert '"cache-control": "no-store"' in handler


def test_remote_mcp_implements_lifecycle_and_task_tools():
    for method in (
        '"initialize"',
        '"ping"',
        '"tools/list"',
        '"tools/call"',
    ):
        assert method in ENTRY
    for tool in (
        '"list_org_tasks"',
        '"get_org_task"',
        '"complete_org_task"',
    ):
        assert tool in ENTRY
    assert '"organization.tasks.read"' in ENTRY
    assert '"organization.tasks.write"' in ENTRY
    assert "world_office_tasks.handle(" in ENTRY
    task_request = ENTRY[
        ENTRY.index("async def _remote_mcp_task_request("):
        ENTRY.index("async def _remote_mcp_call_tool(")
    ]
    assert "_public_base_url(env, request)" in task_request


def test_remote_mcp_task_bridge_uses_request_origin_and_keeps_bot_context():
    captured = {}

    class FakeJsRequest:
        @staticmethod
        def new(url, init):
            captured["url"] = url
            captured["init"] = init
            return SimpleNamespace(url=url, headers=init["headers"])

    class FakeRuntime:
        def __init__(self, env, request, context):
            self.env = env
            self.request = request
            self.context = context
            captured["runtime"] = (env, request, context)

    class FakeResponse:
        status = 200

        async def json(self):
            return {"tasks": []}

    class FakeTasks:
        @staticmethod
        async def handle(runtime, path):
            captured["handler"] = (runtime, path)
            return FakeResponse()

    bridge = _load_task_bridge({
        "JsRequest": FakeJsRequest,
        "to_js": lambda value: value,
        "_OfficeMarketingTasksRuntime": FakeRuntime,
        "world_office_tasks": FakeTasks,
    })
    env = SimpleNamespace(PUBLIC_BASE_URL="")
    request = SimpleNamespace(
        url="https://self-host.example/mcp",
        headers={"authorization": "Bearer test-token"},
    )
    context = {
        "org": "forkmesh",
        "scopes": ["organization.tasks.read"],
    }

    status, data = asyncio.run(bridge(
        env, request, context, "GET", "/api/tasks?team=agents"))

    assert status == 200
    assert data == {"tasks": []}
    assert captured["url"] == (
        "https://self-host.example/api/tasks?team=agents")
    assert captured["init"]["headers"]["authorization"] == "Bearer test-token"
    runtime, path = captured["handler"]
    assert path == "/api/tasks"
    assert captured["runtime"] == (env, runtime.request, context)


def test_org_admin_generates_one_filled_remote_setup_block():
    assert "function orgRemoteMcpPrompt(name, mode, token)" in DASHBOARD
    assert "function orgRemoteMcpSetup(name)" in DASHBOARD
    # Spelled on the apex, not on app.forkmesh.com: www proxies /mcp to the
    # App worker, so the pasted endpoint works and names the public site.
    assert 'publicSiteOrigin() + "/mcp"' in DASHBOARD
    assert 'type: "http"' in DASHBOARD
    assert 'Authorization: "Bearer " + token' in DASHBOARD
    assert "Generate complete setup" in DASHBOARD
    assert "Copy complete prompt" in DASHBOARD
    assert "PASTE_" not in DASHBOARD[
        DASHBOARD.index("function orgRemoteMcpPrompt("):
        DASHBOARD.index("function renderOrgDetail(")
    ]


def test_generated_remote_credential_is_task_scoped():
    wiring = DASHBOARD[
        DASHBOARD.index(
            'root.querySelectorAll("[data-org-remote-mcp-generate]"'
        ):
        DASHBOARD.index("function wireOrgMemberAutocomplete(")
    ]
    assert '"organization.tasks.read"' in wiring
    assert '"organization.tasks.write"' in wiring
    assert '"repository.issues.write"' not in wiring
    assert '"agents.sessions.run"' not in wiring
