"""Behavioral cover for the admin error view's related-user, copy and
ForkBot-task additions. Each helper is executed, not just grepped for."""

import ast
import asyncio
import hashlib
import importlib.util
from pathlib import Path
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
) + "\n"
    + ENTRY.with_name("admin_console.py").read_text(encoding="utf-8")
)


def _organization_rbac():
    spec = importlib.util.spec_from_file_location(
        "organization_rbac", ROOT / "src" / "organization_rbac.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load(names, extra=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names), sorted(
        set(names) - {node.name for node in selected})
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = {"quote": quote, "hashlib": hashlib,
                 "_org_rbac": _organization_rbac()}
    namespace.update(extra or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


RENDER_NAMES = {
    "_html_escape",
    "_admin_href",
    "_admin_error_related_users",
    "_admin_error_users_cell",
    "_admin_error_row_user_cell",
    "_admin_error_copy_button",
    "_admin_error_bot_task_fields",
    "_admin_error_bot_task_form",
    "_admin_error_row_bot_task_button",
}


def test_related_users_rank_accounts_and_keep_anonymous_hits_visible():
    ns = _load(RENDER_NAMES)
    summarize = ns["_admin_error_related_users"]

    assert summarize({}, 0) == "—"
    assert summarize({"alice": 2, "bob": 5}, 3) == "bob (5), alice (2), anonymous (3)"
    # Anonymous-only failures must not read as "nobody was affected".
    assert summarize({}, 4) == "anonymous (4)"

    cell = ns["_admin_error_users_cell"]({"alice": 1}, 0)
    assert cell.startswith('<td class="error-users"')
    assert "alice (1)" in cell

    row_cell = ns["_admin_error_row_user_cell"]("  Alice  ")
    assert ">alice<" in row_cell
    assert "anonymous" in ns["_admin_error_row_user_cell"]("")


def test_copy_button_carries_the_full_escaped_message():
    ns = _load(RENDER_NAMES)
    html = ns["_admin_error_copy_button"]('boom "<script>" & co')
    assert 'data-copy="boom &quot;&lt;script&gt;&quot; &amp; co"' in html
    assert "<script>" not in html
    # A row click must not navigate to the record detail when copying.
    assert "event.stopPropagation()" in html


def test_bot_task_controls_post_to_the_audited_action():
    ns = _load(RENDER_NAMES)
    form = ns["_admin_error_bot_task_form"](
        "500", "GET", "/api/x", "kaboom", "alice (2)",
        csrf_field='<input type="hidden" name="csrf" value="t">',
        admin_query="admin=root",
    )
    assert "action=create_bot_task" in form
    assert 'name="csrf"' in form
    for field in ("group_status", "group_method", "group_path",
                  "group_message", "group_users"):
        assert 'name="%s"' % field in form

    button = ns["_admin_error_row_bot_task_button"](17, "admin=root")
    assert 'name="error_id" value="17"' in button
    assert "action=create_bot_task" in button
    # Row buttons ride the bulk-delete form, so they must carry a formaction.
    assert "formaction=" in button


def _bot_task_namespace(row=None, count=3):
    calls = {}

    async def d1_first(_env, sql, *args):
        calls.setdefault("queries", []).append((sql, args))
        if "SELECT COUNT(*)" in sql:
            return {"n": count}
        return row

    async def d1_run(_env, sql, *args):
        calls["insert"] = (sql, args)

    async def org_row(_env, _org):
        return "org-bi", {"name": "forkmesh"}

    async def org_role(_env, _org_bi, _requester):
        return "admin"

    async def blind(_env, value):
        return "bi:" + value

    async def encrypt(_env, value):
        calls["payload"] = value
        return "sealed"

    class Date:
        @staticmethod
        def now():
            return 123456

    ns = _load(
        {"_admin_error_create_bot_task"},
        {
            "FLAGSHIP_MONITOR_ID": "forkmesh/forkmesh",
            "MAX_NODE_NAME": 64,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "d1_first": d1_first,
            "d1_run": d1_run,
            "_org_row": org_row,
            "_org_role": org_role,
            "blind_index": blind,
            "encrypt_row": encrypt,
            "_ap_uuid": lambda: "a" * 32,
            "Date": Date,
        },
    )
    return ns["_admin_error_create_bot_task"], calls


def test_bot_task_from_one_row_creates_an_org_task_assigned_to_bot():
    create, calls = _bot_task_namespace(
        row={
            "status": 500, "method": "get", "path": "/api/x",
            "message": "kaboom", "actor": "alice",
        }
    )
    banner, details = asyncio.run(
        create(object(), {"error_id": ["17"]}, "root"))

    payload = calls["payload"]
    assert "kaboom" in payload["title"]
    assert "Related user(s): alice" in payload["details"]
    assert "by @root" in payload["details"]
    assert payload["assignee"] == "agent"
    assert payload["repository"] == "forkmesh/forkmesh"
    assert "organization_tasks" in calls["insert"][0]
    assert "assigned to Bot" in banner
    assert details["taskId"] == "a" * 32
    # The audit trail keeps a digest, never the error text.
    assert "kaboom" not in str(details)


def test_bot_task_from_a_group_counts_occurrences():
    create, calls = _bot_task_namespace(count=9)
    banner, details = asyncio.run(create(
        object(),
        {
            "group_status": ["503"], "group_method": ["get"],
            "group_path": ["/api/y"], "group_message": ["nope"],
            "group_users": ["alice (2), anonymous (7)"],
        },
        "root",
    ))
    assert "Logged occurrences: 9" in calls["payload"]["details"]
    assert "alice (2), anonymous (7)" in calls["payload"]["details"]
    assert "assigned to Bot" in banner
    assert details["occurrences"] == 9


def test_bot_task_reports_a_vanished_row_instead_of_filing_an_empty_issue():
    create, calls = _bot_task_namespace(row=None)
    banner, details = asyncio.run(
        create(object(), {"error_id": ["17"]}, "root"))
    assert "failed" in banner.lower()
    assert "insert" not in calls and details == {}
