import ast
import asyncio
import json
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import repo_short_urls  # noqa: E402
import test_issue_submission_inbox as issue_inbox  # noqa: E402
import test_pull_submission_inbox as pull_inbox  # noqa: E402

ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
CANON = {"short": True, "owner": "acme-bolt", "canonical": True}
NOW = 1_800_000_000_000


def single_org_on():
    repo_short_urls.remember(CANON, NOW)


def function(source, name):
    return source.split("async def %s(" % name, 1)[1].split("\nasync def ", 1)[0]


def test_a_non_member_opens_an_issue_on_an_organization_repository():
    single_org_on()
    try:
        inserted, wakes = [], []
        ns = issue_inbox._issue_handler_namespace(inserted, wakes)
        response = asyncio.run(ns["issues_handler"](
            object(), issue_inbox._Request(issue_inbox._open_body()),
            "source", "forkmesh"))
        assert response == {"status": 201, "data": {"ok": True, "number": 622}}
        assert inserted and inserted[0][1]["event"]["type"] == "open"
    finally:
        repo_short_urls.forget()


def test_a_non_member_opens_a_pull_request_on_an_organization_repository():
    single_org_on()
    try:
        inserted, notifications, verified = [], [], []
        runtime = pull_inbox._runtime(inserted, notifications, verified)

        async def feature_on(*_args):
            return None
        runtime.setdefault("_repo_feature_off", feature_on)
        handler = pull_inbox._load_pulls_handler(runtime)
        pull = {"title": "Fix typo", "body": "From a visitor", "base": "main",
                "head": "fix", "patch": "diff --git a/a b/a\n+x\n",
                "author": "visitor-key", "authorName": "visitor", "ts": 1,
                "sig": "signed"}
        response = asyncio.run(handler(
            object(), pull_inbox._Request(pull), "alice", "project"))
        assert response == {"status": 201, "data": {"ok": True, "number": 95}}
        assert len(inserted) == 1
    finally:
        repo_short_urls.forget()


def test_opening_issues_pulls_and_discussions_never_asks_for_membership():
    for name in ("issues_handler", "_accept_issue_submission",
                 "pulls_handler", "discussions_handler"):
        body = function(ENTRY, name)
        assert "repo_short_urls" not in body and "_repo_canonical" not in body
        assert "_org_role(" not in body and "org_members" not in body
    accept = function(ENTRY, "_accept_issue_submission")
    gates = accept.split("owner_event_types = {", 1)[1].split("}", 1)[0]
    assert '"edit"' in gates and '"open"' not in gates


def test_sign_up_and_forks_do_not_depend_on_single_organization_mode():
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    signup = route.split('if url.path == "/signup/":', 1)[1].split(
        "\n        if ", 1)[0]
    assert "repo_short_urls" not in signup
    page = RUNTIME.split("    async def _serve_signup_page(", 1)[1].split(
        "\n    async def ", 1)[0]
    assert "repo_short_urls" not in page and "canonical" not in page
    for name in ("_account_signup_handler", "accounts_handler"):
        if ("async def %s(" % name) in ENTRY:
            assert "repo_short_urls" not in function(ENTRY, name)


def load(names, extra):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in names]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra)
    exec(compile(ast.Module(body=selected, type_ignores=[]),
                 str(ENTRY_PATH), "exec"), namespace)
    return namespace


def internal_namespace(setting):
    async def d1_first(_env, sql, *args):
        return {"doc": repo_short_urls.dump(setting)}

    accounts = {"acme-bolt-actions": {"kind": "node"},
                "forkmesh": {"kind": "node"},
                "drew": {"kind": "user", "pass_hash": "x"},
                "erin": {"kind": "user", "pass_hash": "x"}}

    async def account_row(_env, name):
        return "bi", accounts.get(name)

    async def org_row(_env, name):
        return "bi", ({"name": name} if name == "acme-bolt" else None)

    return load({"_org_internal_members", "_repo_short_setting_row",
                 "_repo_name_is_account"}, {
        "repo_short_urls": repo_short_urls,
        "Date": SimpleNamespace(now=lambda: NOW),
        "d1_first": d1_first, "_account_row": account_row,
        "_org_row": org_row,
        "_account_kind": lambda rec: rec.get("kind", "user"),
    })


def test_internal_accounts_are_hidden_from_the_roster_only_in_single_org_mode():
    names = ["drew", "erin", "acme-bolt-actions"]
    repo_short_urls.forget()
    ns = internal_namespace(CANON)
    assert asyncio.run(ns["_org_internal_members"](
        None, "acme-bolt", names)) == {"acme-bolt-actions"}
    assert asyncio.run(ns["_org_internal_members"](
        None, "other-org", names)) == set()
    repo_short_urls.forget()
    ns = internal_namespace({"short": True, "owner": "acme-bolt"})
    assert asyncio.run(ns["_org_internal_members"](
        None, "acme-bolt", names)) == set()
    repo_short_urls.forget()
    roster = function(ENTRY, "org_members_handler")
    assert 'globals().get("_org_internal_members")' in roster
    assert 'if str(r.get("name") or "") not in hidden' in roster


def test_internal_account_names_do_not_block_root_addresses_in_single_org_mode():
    repo_short_urls.forget()
    ns = internal_namespace(CANON)
    is_account = ns["_repo_name_is_account"]
    assert asyncio.run(is_account(None, "forkmesh", True)) is False
    assert asyncio.run(is_account(None, "forkmesh")) is True
    assert asyncio.run(is_account(None, "drew", True)) is True
    assert asyncio.run(is_account(None, "acme-bolt", True)) is True
    repo_short_urls.forget()
    helper = RUNTIME.split("    async def _repo_short_route(self, url):", 1)[1]
    helper = helper.split("\n    async def ", 1)[0]
    assert 'setting.get("canonical")' in helper
    assert '_account_kind(account) == "node"' in helper


def test_forkmesh_com_and_default_instances_are_unchanged():
    default = repo_short_urls.parse("")
    assert default["canonical"] is False
    assert repo_short_urls.canonical_redirect("/acme/tools", "", default) is None
    assert repo_short_urls.client_doc(default) == {
        "owner": "", "short": False, "canonical": False}
    assert repo_short_urls.short_urls(
        {"cloneUrl": "https://forkmesh.com/forkmesh/forkmesh"},
        "https://forkmesh.com", default, {}) == {
        "cloneUrl": "https://forkmesh.com/forkmesh/forkmesh"}
    short_only = repo_short_urls.parse('{"short":true,"owner":"acme"}')
    assert repo_short_urls.canonical_redirect(
        "/acme/tools", "", short_only) is None
    helpers = (ROOT / "public" / "dashboard" / "js" / "02-helpers.js").read_text(
        encoding="utf-8")
    assert 'const REPO_CORE_HOSTS = ["app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"];' in helpers
    assert json.loads(json.dumps(repo_short_urls.DEFAULT)) == {
        "short": False, "owner": "", "canonical": False}
