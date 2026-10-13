import ast
import asyncio
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
REMOTE_MCP = (ROOT / "src" / "remote_mcp.py").read_text(encoding="utf-8")
CATALOG = (ROOT / "src" / "catalog.py").read_text(encoding="utf-8")
DASHBOARD = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
DOC = (
    ROOT.parent / "docs" / "app" / "organization-bot-scopes.md"
).read_text(encoding="utf-8")

RETIRED = {
    "world.presence",
    "world.chat.write",
    "world.interact",
    "repository.contents.read",
    "repository.issues.write",
    "repository.pulls.review",
    "agents.sessions.run",
}
LIVE = {"organization.tasks.read", "organization.tasks.write", "local.connect"}
FUNCS = {"_org_bot_scopes", "_org_bot_token_context"}
CONSTANTS = {
    "ORG_BOT_TOKEN_SCOPES",
    "ORG_BOT_RETIRED_SCOPES",
    "ORG_BOT_DEFAULT_SCOPES",
    "ORG_BOT_TOKEN_RE",
    "GENIE_CREDENTIAL_SCOPES",
}
TOKEN = "fmbot_" + "a" * 32 + "_" + "b" * 43


def _rbac():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "organization_rbac_for_bot_tokens",
        ROOT / "src" / "organization_rbac.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load(extra=None):
    selected = []
    for node in ast.parse(ENTRY).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) \
                and node.name in FUNCS:
            selected.append(node)
        elif isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in CONSTANTS
                for target in node.targets):
            selected.append(node)
    for node in ast.parse(CATALOG).body:
        if isinstance(node, ast.FunctionDef) and node.name == "clean_string":
            selected.append(node)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = {"re": re, "urlparse": urlparse, "_org_rbac": _rbac()}
    namespace.update(extra or {})
    exec(compile(module, "entry.py", "exec"), namespace)
    return namespace


def _function(source, name):
    for node in ast.parse(source).body:
        if getattr(node, "name", None) == name:
            return ast.get_source_segment(source, node)
    raise AssertionError(name)


def test_retired_scopes_are_no_longer_grantable_or_offered():
    ns = _load()
    assert set(ns["ORG_BOT_TOKEN_SCOPES"]) == LIVE
    assert set(ns["ORG_BOT_RETIRED_SCOPES"]) == RETIRED
    assert set(ns["ORG_BOT_DEFAULT_SCOPES"]) == LIVE
    handler = _function(ENTRY, "org_bot_tokens_handler")
    assert "for scope, description in ORG_BOT_TOKEN_SCOPES.items()" in handler
    for scope in RETIRED:
        assert '"%s"' % scope not in DASHBOARD


def test_a_grant_request_naming_retired_scopes_keeps_only_live_ones():
    scopes = _load()["_org_bot_scopes"]
    requested = ["world.presence", "organization.tasks.read",
                 "agents.sessions.run", "local.connect"]
    assert scopes(requested, default=True) == [
        "organization.tasks.read", "local.connect"]
    assert scopes(sorted(RETIRED), default=True) == []
    assert scopes(None, default=True) == [
        "organization.tasks.read", "organization.tasks.write",
        "local.connect"]
    assert scopes(["organization.admin"], default=True) is None
    handler = _function(ENTRY, "org_bot_tokens_handler")
    assert 'scopes = _org_bot_scopes(data.get("scopes"), default=True)' \
        in handler
    assert "if scopes is None or not scopes:" in handler
    assert '"invalid_bot_permissions"' in handler


def _context_namespace(stored_scopes, creator_role="owner"):
    runs = []

    async def d1_first(env, sql, *args):
        return {
            "token_id": "a" * 32, "org_bi": "org-bi", "provider": "codex",
            "data": "sealed", "created_at": 1, "last_used_at": 0,
            "expires_at": 0, "revoked_at": 0, "created_by_bi": "owner-bi",
            "org_name": "acme", "creator_role": creator_role,
        }

    async def d1_run(env, sql, *args):
        runs.append(sql)

    async def blind_index(env, value):
        return "bi:" + value

    async def decrypt_row(env, data):
        return {"label": "Old bot", "scopes": list(stored_scopes)}

    ns = _load({
        "d1_first": d1_first,
        "d1_run": d1_run,
        "blind_index": blind_index,
        "decrypt_row": decrypt_row,
        "Date": SimpleNamespace(now=lambda: 10_000),
        "method_name": lambda request: request.method,
    })
    return ns, runs


def _request(path="/api/tasks"):
    return SimpleNamespace(
        url="https://app.forkmesh.com" + path,
        method="GET",
        headers={"authorization": "Bearer " + TOKEN},
    )


def test_stored_tokens_carrying_retired_scopes_keep_their_live_scopes():
    ns, runs = _context_namespace([
        "world.presence", "organization.tasks.read", "repository.issues.write",
        "agents.sessions.run", "local.connect"])
    context = asyncio.run(
        ns["_org_bot_token_context"](None, _request(), touch=True))
    assert context is not None
    assert context["scopes"] == ["organization.tasks.read", "local.connect"]
    assert any("org_bot_token_usage" in sql for sql in runs)


def test_a_stored_token_with_only_retired_scopes_authenticates_with_none():
    ns, _ = _context_namespace(sorted(RETIRED))
    context = asyncio.run(ns["_org_bot_token_context"](None, _request()))
    assert context is not None
    assert context["scopes"] == []


def test_a_token_dies_with_its_creators_membership():
    for role in (None, "", "guest", "auditor"):
        ns, runs = _context_namespace(["organization.tasks.read"], role)
        assert asyncio.run(ns["_org_bot_token_context"](None, _request())) \
            is None
        assert runs == []
    ns, _ = _context_namespace(["organization.tasks.read"], "member")
    assert asyncio.run(ns["_org_bot_token_context"](None, _request()))


def test_a_demoted_creator_keeps_only_member_scopes():
    ns, _ = _context_namespace(
        ["organization.tasks.read", "local.connect"], "member")
    assert asyncio.run(ns["_org_bot_token_context"](None, _request())) is None
    ns, _ = _context_namespace(
        ["organization.tasks.read", "local.connect"], "admin")
    assert asyncio.run(ns["_org_bot_token_context"](None, _request()))


def test_the_token_lookup_joins_the_creators_membership():
    helper = _function(ENTRY, "_org_bot_token_context")
    assert "LEFT JOIN org_members m ON m.org_bi=t.org_bi " in helper
    assert "AND m.member_bi=t.created_by_bi" in helper


def test_every_grantable_scope_is_checked_by_a_bot_token_route():
    tasks = _function(ENTRY, "organization_tasks_handler")
    session = _function(ENTRY, "bot_session_handler")
    mcp = _function(REMOTE_MCP, "_remote_mcp_call_tool")
    assert '"organization.tasks.read"' in tasks
    assert '"organization.tasks.write"' in tasks
    assert '"organization.tasks.read"' in mcp
    assert '"organization.tasks.write"' in mcp
    assert '"local.connect" not in context["scopes"]' in session
    checked = tasks + session + mcp
    for scope in _load()["ORG_BOT_TOKEN_SCOPES"]:
        assert '"%s"' % scope in checked


def test_bot_tokens_are_accepted_only_by_the_routes_that_check_scopes():
    callers = set()
    for path, source in (("entry.py", ENTRY), ("remote_mcp.py", REMOTE_MCP)):
        for node in ast.walk(ast.parse(source)):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            if node.name == "_org_bot_token_context":
                continue
            segment = ast.get_source_segment(source, node)
            if "_org_bot_token_context(" in segment:
                callers.add(node.name)
    assert callers == {
        "organization_tasks_handler",
        "bot_session_handler",
        "remote_mcp_handler",
    }


def test_scope_doc_lists_live_and_retired_scopes():
    for scope in LIVE | RETIRED:
        assert "`%s`" % scope in DOC
