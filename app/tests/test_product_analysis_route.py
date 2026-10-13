#!/usr/bin/env python3
"""/product-analysis is wired as a lazily loaded, undiscoverable Worker route."""

import ast
import re
import tomllib
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
APP_RUNTIME = ROOT / "src" / "app_runtime.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
RUNTIME_TEXT = APP_RUNTIME.read_text(encoding="utf-8")


def _route_function():
    module = ast.parse(RUNTIME_TEXT)
    for node in ast.walk(module):
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "_route":
            return node
    raise AssertionError("Default._route was not found")


def test_entry_defines_the_path_and_lazy_exports_before_binding_app_runtime():
    assert 'PRODUCT_ANALYSIS_PATH = "/product-analysis"' in ENTRY_TEXT
    assert '_product_analysis = _LazyModule("product_analysis")' in ENTRY_TEXT
    assert ('product_analysis_handler = _product_analysis.export('
            '"product_analysis_handler")') in ENTRY_TEXT
    assert ('classify_signup_origin = _product_analysis.export('
            '"classify_signup_origin")') in ENTRY_TEXT
    # app_runtime copies entry globals when it is bound, so the names must
    # exist by then; a top-level import of the module must never appear.
    bound = ENTRY_TEXT.index("_app_runtime_module._bind_runtime(globals())")
    assert ENTRY_TEXT.index('PRODUCT_ANALYSIS_PATH = "/product-analysis"') < bound
    assert ENTRY_TEXT.index('_LazyModule("product_analysis")') < bound
    assert "import product_analysis" not in ENTRY_TEXT


def test_route_dispatches_the_path_and_shares_the_ordinary_404():
    route = _route_function()
    calls = {
        node.func.id
        for node in ast.walk(route)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    assert "product_analysis_handler" in calls
    names = {node.id for node in ast.walk(route) if isinstance(node, ast.Name)}
    assert "PRODUCT_ANALYSIS_PATH" in names
    block = RUNTIME_TEXT[RUNTIME_TEXT.index(
        "response = await product_analysis_handler(self.env, request)"):]
    block = block[:block.index("\n\n")]
    assert "if response is not None:" in block
    assert "return await self._serve_not_found_page(url)" in block
    # No 401/403 text response leaks that the route exists.
    assert "Admin login required" not in block
    assert "403" not in block


def test_wrangler_routes_the_path_to_the_worker_and_robots_disallows_it():
    manifest = tomllib.loads((ROOT / "wrangler.toml").read_text("utf-8"))
    first = manifest["assets"]["run_worker_first"]
    assert "/product-analysis" in first
    assert "/product-analysis/*" in first
    robots = (ROOT / "public" / "robots.txt").read_text("utf-8")
    assert "Disallow: /product-analysis" in robots
    # The disallow is the only rule, so nothing else on the app origin changes.
    assert robots.strip().splitlines() == [
        "User-agent: *", "Disallow: /product-analysis"]


def test_the_dashboard_module_is_registered_as_lazy_for_the_footprint():
    tool = (ROOT.parent / "world" / "tools" / "build_worker_footprint.py"
            ).read_text("utf-8")
    assert '"product_analysis.py",' in tool


def test_no_admin_bootstrap_or_credential_constant_was_added():
    for needle in ("first user becomes admin", "PRODUCT_ANALYSIS_PASS = ",
                   'PRODUCT_ANALYSIS_PASS="', "PRODUCT_ANALYSIS_USER = "):
        assert needle not in ENTRY_TEXT
        assert needle not in RUNTIME_TEXT
    manifest = (ROOT / "wrangler.toml").read_text("utf-8")
    assert "PRODUCT_ANALYSIS_PASS" not in manifest
    assert "PRODUCT_ANALYSIS_USER" not in manifest


class _Url:
    """The two attributes the dispatcher predicates read off a parsed URL."""

    def __init__(self, url):
        parsed = urlparse(str(url))
        self.pathname = parsed.path
        self.search = ("?" + parsed.query) if parsed.query else ""

    @staticmethod
    def new(url):
        return _Url(url)


def _request(path, method="GET"):
    return SimpleNamespace(
        url="https://app.forkmesh.com" + path,
        method=method,
        headers=SimpleNamespace(get=lambda _name: None),
    )


def _dispatcher(name):
    """The named entry.py routing predicate, with the helpers it calls."""
    module = ast.parse(ENTRY_TEXT)
    wanted = {name, "_git_clone_http_request",
              "_package_api_http_request",
              "_repository_projection_http_request"}
    selected = [
        node for node in module.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in wanted)
        or (isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name)
                    and target.id == "PRODUCT_ANALYSIS_PATH"
                    for target in node.targets))
    ]
    assert wanted <= {getattr(node, "name", "") for node in selected}
    namespace = {
        "re": re,
        "JsURL": _Url,
        "method_name": lambda request: request.method,
        # Package API prefixes only ever come from the generated registry;
        # core source, and so this fixture, names no package.
        "plugin_api_prefixes": lambda: ("/api/demo-package",),
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    assert namespace["PRODUCT_ANALYSIS_PATH"] == "/product-analysis"
    return namespace[name]


def test_every_dashboard_request_runs_in_its_own_ingress_object():
    # Building one report drains and decrypts the users and repositories
    # tables -- a full plaintext megabyte today. That exceeds the stateless
    # 10 ms Pyodide allowance (503/1102 at 462 ms CPU on GET
    # /product-analysis?view=users, 2026-09-10), so the route is serialized
    # into a Durable Object, where a request has the 30-second budget. The
    # shard is the route's own: no other traffic family may share an isolate
    # with a load that heavy, and a dashboard load must not be poisoned by
    # one either.
    serialized = _dispatcher("_serialized_http_request")
    shard = _dispatcher("_app_ingress_shard")
    for path, method in (
            ("/product-analysis", "GET"),
            ("/product-analysis/", "GET"),
            ("/product-analysis?view=users", "GET"),
            ("/product-analysis?view=repositories", "HEAD"),
            ("/product-analysis", "POST"),
    ):
        request = _request(path, method)
        assert serialized(request), path
        assert shard(request) == "1", path
    # A lookalike prefix is a different route and keeps ordinary dispatch.
    other = _request("/product-analysis-x")
    assert not serialized(other)
    assert shard(other) != "1"
    # No other family was moved onto this object.
    for path in ("/", "/api/status", "/signup", "/api/ledger",
                 "/api/network/stats", "/dashboard"):
        assert shard(_request(path)) != "1", path
