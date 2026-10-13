#!/usr/bin/env python3
"""Organizations + teams contract checks (issue #388).

Users create organizations, add accounts to them, and group members into
teams holding one repository permission (read / write / maintain / admin)
over every repo linked under the org. The org name fronts the linked repos'
URLs: /<org>/<repo> git endpoints and /api/repo/<org>/<repo>/... are
rewritten worker-internally to the hosting node's namespace before routing.

These tests load the real helpers out of src/entry.py (AST extraction, same
style as test_ap_repo_settings.py) and pin:

  * the /api/orgs route table and the org tables/migration exist;
  * the permission ladder ranks read < write < maintain < admin and unknown
    permissions never qualify;
  * _org_permission: owners/admins => admin, plain members => read, team
    membership raises read to the best team's permission;
  * _org_write_allowed requires write+ and fails CLOSED on any error;
  * org_alias_rewrite rewrites git + /api/repo paths (and only those), and
    _route runs it before any matching;
  * the push gate branches a non-owner Basic-auth username to the org-team
    token (forkmesh-org-push-v1, signed with the pusher's OWN key);
  * the org and account namespaces reject each other's names, so the alias
    rewrite can never shadow a real node's URL;
  * linking a repo under an org requires the caller's own account namespace or
    a node that account demonstrably owns.

Run: python3 -m pytest cloudflare_worker/tests/test_orgs_teams.py
"""

import ast
import asyncio
import importlib.util
import re
from pathlib import Path
from urllib.parse import urlparse

from worker_test_helpers import json_from_request_double

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "route_handlers.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + "".join(
        (ROOT.parent / "world" / "src" / name).read_text(encoding="utf-8")
        for name in ("world_routes.py", "world_office_routes.py")
    )
)
SCHEMA_TEXT = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")


def _module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "src" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


urls = _module("urls")
catalog = _module("catalog")
organization_rbac = _module("organization_rbac")


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {n.name for n in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    namespace.setdefault("bounded_json_request", json_from_request_double)
    # Every org role decision goes through the deny-by-default matrix.
    namespace.setdefault("_org_rbac", organization_rbac)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _constant(name):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    for node in tree.body:
        if (isinstance(node, ast.Assign) and
                any(getattr(t, "id", "") == name for t in node.targets)):
            return ast.literal_eval(node.value)
    raise AssertionError(name + " not found")


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


TEAM_PERMISSIONS = _constant("TEAM_PERMISSIONS")
ORG_ROLES = _constant("ORG_ROLES")
ORG_WORLD_ACCESS_VALUES = _constant("ORG_WORLD_ACCESS_VALUES")
OFFICE_FLOOR_TEAM_ALIASES = _constant("OFFICE_FLOOR_TEAM_ALIASES")


# --- Route table + schema ----------------------------------------------------

def test_routes_cover_the_org_api():
    assert urls.ORGS_RE.match("/api/orgs")
    assert urls.ORG_RE.match("/api/orgs/acme").group(1) == "acme"
    assert urls.ORG_MEMBERS_RE.match("/api/orgs/acme/members")
    assert urls.ORG_TEAMS_RE.match("/api/orgs/acme/teams")
    m = urls.ORG_TEAM_MEMBERS_RE.match("/api/orgs/acme/teams/core/members")
    assert m and m.group(2) == "core"
    assert urls.ORG_REPOS_RE.match("/api/orgs/acme/repos")
    # single-segment ORG_RE never swallows the sub-collections
    assert not urls.ORG_RE.match("/api/orgs/acme/members")


def test_repo_api_prefix_matches_repo_scoped_paths_only():
    m = urls.REPO_API_PREFIX_RE.match("/api/repo/acme/widget/issues")
    assert m and m.group(1) == "acme" and m.group(2) == "widget"
    assert urls.REPO_API_PREFIX_RE.match("/api/repo/acme/widget")
    assert not urls.REPO_API_PREFIX_RE.match("/api/orgs/acme")
    assert not urls.REPO_API_PREFIX_RE.match("/acme/widget/info/refs")


def test_schema_defines_org_tables_and_migration_exists():
    for table in ("orgs", "org_members", "org_teams", "org_team_members",
                  "org_repos"):
        assert "CREATE TABLE IF NOT EXISTS " + table in SCHEMA_TEXT, table
    assert "idx_org_repos_node" in SCHEMA_TEXT
    assert "idx_org_members_member" in SCHEMA_TEXT
    assert (ROOT / "migrations" / "0038_orgs_teams.sql").exists()
    assert "CREATE TABLE IF NOT EXISTS org_team_collaborators" in SCHEMA_TEXT
    universal = (
        ROOT / "migrations" / "0105_organization_tasks.sql"
    ).read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS org_team_collaborators" in universal
    assert "CREATE TABLE IF NOT EXISTS organization_tasks" in universal


# --- Permission ladder -------------------------------------------------------

def test_permission_ladder_order_and_roles():
    assert TEAM_PERMISSIONS == ("read", "write", "maintain", "admin")
    assert ORG_ROLES == (
        "owner", "admin", "maintainer", "member", "auditor", "guest")
    assert ORG_ROLES == organization_rbac.ORG_ROLES
    ns = _load("team_permission_rank",
               extra_globals={"TEAM_PERMISSIONS": TEAM_PERMISSIONS})
    rank = ns["team_permission_rank"]
    assert rank("read") < rank("write") < rank("maintain") < rank("admin")
    assert rank("WRITE") == rank("write")  # case-insensitive
    assert rank("") == -1 and rank("bogus") == -1 and rank(None) == -1


def _org_perm_env(role, team_permissions):
    # Fake d1 layer: one org, one member with `role`, teams granting
    # `team_permissions`.
    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *params):
        if "FROM org_members" in sql:
            return {"role": role} if role else None
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_all(env, sql, *params):
        assert "org_team_members" in sql
        return [{"permission": p} for p in team_permissions]

    ns = _load("_org_role", "_org_permission", "team_permission_rank",
               extra_globals={
                   "TEAM_PERMISSIONS": TEAM_PERMISSIONS,
                   "blind_index": blind_index,
                   "d1_first": d1_first,
                   "d1_all": d1_all,
               })
    return ns


def test_org_permission_ladder():
    # owners and admins hold admin regardless of teams
    for role in ("owner", "admin"):
        ns = _org_perm_env(role, [])
        assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "admin"
    # a plain member holds read; team membership raises it to the best team
    ns = _org_perm_env("member", [])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "read"
    ns = _org_perm_env("member", ["write", "maintain"])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "maintain"
    # a maintainer holds maintain; a team grant can still raise it to admin
    ns = _org_perm_env("maintainer", [])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "maintain"
    ns = _org_perm_env("maintainer", ["write"])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "maintain"
    ns = _org_perm_env("maintainer", ["admin"])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "admin"
    # auditors are read-only whatever their teams grant
    ns = _org_perm_env("auditor", ["write", "admin"])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "read"
    # guests hold nothing until a team lists them, and then only read
    ns = _org_perm_env("guest", [])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == ""
    ns = _org_perm_env("guest", ["maintain"])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == "read"
    # a non-member holds nothing, and so does a role the matrix never heard of
    ns = _org_perm_env("", [])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == ""
    ns = _org_perm_env("superuser", ["admin"])
    assert _run(ns["_org_permission"](None, "org-bi", "alice")) == ""


def test_org_write_allowed_requires_write_rank_and_fails_closed():
    def build(permission, explode=False):
        async def ensure_schema(env):
            return None

        async def blind_index(env, value):
            return "bi:" + value

        async def d1_first(env, sql, *params):
            if "FROM org_members" in sql:
                return {"role": "member"}
            return None

        async def d1_all(env, sql, *params):
            if explode:
                raise RuntimeError("d1 down")
            if "FROM org_repos" in sql:
                return [{"org_bi": "org-bi"}]
            return [{"permission": permission}] if permission else []

        return _load(
            "_org_write_allowed", "_org_permission", "_org_role",
            "team_permission_rank",
            extra_globals={
                "TEAM_PERMISSIONS": TEAM_PERMISSIONS,
                "ensure_schema": ensure_schema,
                "blind_index": blind_index,
                "d1_first": d1_first,
                "d1_all": d1_all,
            })["_org_write_allowed"]

    assert _run(build("write")(None, "jett", "widget", "alice")) is True
    assert _run(build("admin")(None, "jett", "widget", "alice")) is True
    # plain membership (read) is NOT enough to push
    assert _run(build("")(None, "jett", "widget", "alice")) is False
    # a D1 error must never grant push access
    assert _run(build("write", explode=True)(None, "jett", "widget", "alice")) is False
    assert _run(build("write")(None, "", "widget", "alice")) is False


# --- URL aliasing ------------------------------------------------------------

class _FakeJsRequest:
    def __init__(self, url, base):
        self.url = url
        self.base = base

    @classmethod
    def new(cls, url, base):
        return cls(url, base)


def _alias_ns(node):
    async def _org_repo_node(env, org, repo):
        return node if org == "acme" else ""

    return _load("org_alias_rewrite", extra_globals={
        "REPO_API_PREFIX_RE": urls.REPO_API_PREFIX_RE,
        "GIT_INFO_RE": urls.GIT_INFO_RE,
        "GIT_PACK_RE": urls.GIT_PACK_RE,
        "GIT_RECEIVE_RE": urls.GIT_RECEIVE_RE,
        "safe_segment": catalog.safe_segment,
        "_org_repo_node": _org_repo_node,
        "JsRequest": _FakeJsRequest,
        "quote": __import__("urllib.parse", fromlist=["quote"]).quote,
        "urlparse": urlparse,
    })["org_alias_rewrite"]


def test_alias_rewrite_rewrites_api_and_git_paths():
    rewrite = _alias_ns("jett")
    for path, expected in (
        ("/api/repo/acme/widget/tree", "/api/repo/jett/widget/tree"),
        ("/api/repo/acme/widget/branches",
         "/api/repo/jett/widget/branches"),
        ("/api/repo/acme/widget/issues", "/api/repo/jett/widget/issues"),
        ("/acme/widget.git/info/refs", "/jett/widget.git/info/refs"),
        ("/acme/widget.git/git-upload-pack", "/jett/widget.git/git-upload-pack"),
        ("/acme/widget/info/refs", "/jett/widget/info/refs"),
        ("/acme/widget/git-upload-pack", "/jett/widget/git-upload-pack"),
        ("/acme/widget/git-receive-pack", "/jett/widget/git-receive-pack"),
    ):
        url = urlparse("https://forkmesh.com" + path + "?service=git-upload-pack")
        result = _run(rewrite(None, "req", url))
        assert result is not None, path
        request, new_url = result
        assert new_url.path == expected
        assert new_url.query == "service=git-upload-pack"  # query survives
        assert request == "req"  # original body/headers/cf metadata survive


def test_alias_rewrite_does_not_reconstruct_the_worker_request():
    source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def org_alias_rewrite"):
        ENTRY_TEXT.index("\n\n\nclass _OrganizationSuccessionRuntime")
    ]
    assert "return request, urlparse(new_url)" in source
    assert "JsRequest.new" not in source


def test_alias_rewrite_leaves_other_paths_and_plain_nodes_alone():
    rewrite = _alias_ns("jett")
    for path in ("/dashboard", "/api/orgs/acme", "/acme/widget",
                 "/api/repo/othernode/widget/tree"):
        url = urlparse("https://forkmesh.com" + path)
        assert _run(rewrite(None, "req", url)) is None, path
    # no alias registered -> untouched even on a matching shape
    rewrite = _alias_ns("")
    url = urlparse("https://forkmesh.com/api/repo/acme/widget/tree")
    assert _run(rewrite(None, "req", url)) is None


def test_route_runs_the_alias_rewrite_before_any_matching():
    assert "aliased = await org_alias_rewrite(self.env, request, url)" in ENTRY_TEXT
    assert ENTRY_TEXT.index("aliased = await org_alias_rewrite") < \
        ENTRY_TEXT.index("git_info = GIT_INFO_RE.match(url.path)")


# --- Fediverse org-alias resolution (adhoc #187) -----------------------------

def _ap_alias_ns():
    async def _org_repo_node(env, org, repo):
        return "jett" if org == "acme" else ""

    return _load("_ap_org_alias_owner", extra_globals={
        "_org_repo_node": _org_repo_node,
    })["_ap_org_alias_owner"]


def test_ap_org_alias_owner_resolves_org_handles_to_the_backing_node():
    # A fediverse mention of @acme.widget@host must read its repo row, AP
    # settings and issue inbox under the linked node ("jett"), not the org.
    resolve = _ap_alias_ns()
    assert _run(resolve(None, "acme", "widget")) == "jett"
    # A plain node handle (no org alias) resolves to itself, so every
    # non-org AP path is a no-op.
    assert _run(resolve(None, "jett", "widget")) == "jett"


def test_ap_data_reads_route_through_the_org_alias_resolver():
    # The repo-federates gate, per-repo settings key, actor doc and the
    # mention handler must all resolve the org alias before touching repo
    # data — otherwise an org-fronted repo is invisible to federation.
    fed = ENTRY_TEXT[ENTRY_TEXT.index("async def _ap_repo_federates"):]
    fed = fed[:fed.index("\n\n\n")]
    assert "_ap_org_alias_owner(env, owner, repo)" in fed
    settings_bi = ENTRY_TEXT[ENTRY_TEXT.index("async def _ap_repo_settings_bi"):]
    settings_bi = settings_bi[:settings_bi.index("\n\n\n")]
    assert "_ap_org_alias_owner(env, owner, repo)" in settings_bi
    mention = ENTRY_TEXT[ENTRY_TEXT.index("async def _ap_handle_repo_mention"):]
    mention = mention[:mention.index("\n\n\nasync def")]
    assert "data_owner = await _ap_org_alias_owner(env, owner, repo)" in mention
    assert "fediverse_mentions_api.record_verified(" in mention
    assert "data_owner=data_owner" in mention
    assert "signature_verified=True" in mention
    assert "_forkbot_enqueue_issue(" not in mention


# --- Push gate ---------------------------------------------------------------

def test_push_gate_branches_a_foreign_username_to_the_org_token():
    # The Basic-auth username selects the org-team path: the pusher signs with
    # their OWN key and must hold write+ on the repo through an org.
    assert ("return await verify_org_push_token(env, username, owner, repo, ts, sig)"
            in ENTRY_TEXT)
    assert '"forkmesh-org-push-v1\\n" + pusher + "\\n" + owner + "\\n"' in ENTRY_TEXT
    assert "return await _org_write_allowed(env, owner, repo, pusher)" in ENTRY_TEXT
    # the owner's own push path is unchanged
    assert '"forkmesh-push-v1\\n" + owner + "\\n" + repo + "\\n"' in ENTRY_TEXT


# --- Namespace exclusivity ---------------------------------------------------

def test_org_and_account_namespaces_reject_each_other():
    # org creation rejects names any account row holds...
    assert '"error": "org_name_taken"' in ENTRY_TEXT
    # ...and both account signup paths reject org names, so the alias rewrite
    # can never shadow (or be shadowed by) a real node's URL.
    assert ENTRY_TEXT.count(
        "_, org_holder = await _org_row(env, name)") == 2
    assert "if org_holder:" in ENTRY_TEXT


def test_linking_a_repo_requires_the_callers_own_account_or_fleet_node():
    assert '"error": "not_your_node"' in ENTRY_TEXT
    assert "if not await _account_owns_node(env, account, node):" in ENTRY_TEXT
    # ...and the repo must actually be published by that node.
    assert '"error": "unknown_repo"' in ENTRY_TEXT


def _org_repo_link_harness(owns_node):
    writes = []
    audits = []
    ownership_checks = []

    class Request:
        method = "POST"

        async def json(self):
            return {
                "sessionToken": "valid",
                "repo": "forkmesh",
                "node": "mirror2",
            }

    class Clock:
        @staticmethod
        def now():
            return 123456

    async def noop(*args, **kwargs):
        return None

    async def org_row(env, org):
        assert org == "forkmesh"
        return "org-bi", {"name": "forkmesh"}

    async def session_record(env, request, data):
        return "jett-bi", {"name": "jett"}

    async def org_role(env, org_bi, account):
        assert (org_bi, account) == ("org-bi", "jett")
        return "owner"

    async def account_owns_node(env, account, node):
        ownership_checks.append((account, node))
        return owns_node

    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *params):
        if "FROM repositories" in sql:
            return {"one": 1}
        if "FROM org_repos" in sql:
            return {"one": 1}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(env, sql, *params):
        writes.append((sql, params))

    async def audit(env, actor, action, target_type, target, outcome,
                    details=None):
        audits.append({
            "actor": actor,
            "action": action,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    def response(data, status=200, **kwargs):
        return {"status": status, "data": data}

    namespace = _load(
        "org_repos_handler",
        extra_globals={
            "MAX_NODE_NAME": 63,
            "MAX_REPO_SEGMENT": 80,
            "MAX_ORG_REPOS": 200,
            "Date": Clock,
            "_ORG_ALIAS_MEMO": {},
            "purge_catalog_related_caches": noop,
            "schedule_public_catalog_republish": noop,
            "ensure_schema": noop,
            "method_name": lambda request: request.method,
            "_org_row": org_row,
            "_org_management_session": session_record,
            "_org_role": org_role,
            "_account_owns_node": account_owns_node,
            "_audit_sensitive_action": audit,
            "clean_string": catalog.clean_string,
            "safe_segment": catalog.safe_segment,
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "json_response": response,
        },
    )
    return namespace["org_repos_handler"], Request(), writes, audits, ownership_checks


def test_org_owner_can_link_a_published_node_in_their_fleet():
    handler, request, writes, audits, checks = _org_repo_link_harness(True)
    response = _run(handler(None, request, "forkmesh"))
    assert response["status"] == 200
    assert response["data"] == {
        "ok": True,
        "repo": "forkmesh",
        "node": "mirror2",
        "linked": True,
    }
    assert checks == [("jett", "mirror2")]
    assert len(writes) == 1
    assert "INSERT INTO org_repos" in writes[0][0]
    assert writes[0][1][2] == "mirror2"
    assert audits[-1]["outcome"] == "success"


def test_org_owner_cannot_link_an_unowned_node_namespace():
    handler, request, writes, audits, checks = _org_repo_link_harness(False)
    response = _run(handler(None, request, "forkmesh"))
    assert response == {
        "status": 403,
        "data": {"error": "not_your_node"},
    }
    assert checks == [("jett", "mirror2")]
    assert writes == []
    assert audits == [{
        "actor": "jett",
        "action": "organization.repo_link",
        "target": "forkmesh/forkmesh",
        "outcome": "denied",
        "details": {"reason": "not_your_node"},
    }]


def test_org_admin_guards_and_caps_are_present():
    # last-owner protection + per-account/org caps bounding D1 growth
    assert '"error": "last_owner"' in ENTRY_TEXT
    for cap in ("MAX_ORGS_PER_ACCOUNT", "MAX_ORG_MEMBERS", "MAX_ORG_TEAMS",
                "MAX_ORG_REPOS"):
        assert cap in ENTRY_TEXT, cap
    # team membership can only raise an existing member's permission
    assert '"error": "not_a_member"' in ENTRY_TEXT


def test_world_logo_and_floor_office_access_are_server_enforced():
    ns = _load(
        "_org_logo_url",
        "_org_world_access",
        "_org_world_access_allowed",
        extra_globals={
            "clean_string": lambda value, size: str(value or "")[:size],
            "urlparse": urlparse,
            "ORG_WORLD_ACCESS_VALUES": ORG_WORLD_ACCESS_VALUES,
            "ORG_ROLES": ORG_ROLES,
        },
    )
    logo = ns["_org_logo_url"]
    assert logo("https://cdn.example/acme.svg") == \
        "https://cdn.example/acme.svg"
    assert logo("http://cdn.example/acme.svg") == ""
    assert logo("https://user:secret@cdn.example/acme.svg") == ""
    assert logo("javascript:alert(1)") == ""

    access = ns["_org_world_access"]({
        "lobby": "public",
        "floors": "restricted",
        "offices": "private",
    })
    assert access == {
        "lobby": "public",
        "floors": "restricted",
        "offices": "private",
    }
    allowed = ns["_org_world_access_allowed"]
    assert allowed("public", "") is True
    assert allowed("restricted", "member") is True
    assert allowed("restricted", "") is False
    assert allowed("private", "admin") is True
    assert allowed("private", "member") is False
    # insiders reach restricted floors; guests and unknown roles do not
    assert allowed("restricted", "auditor") is True
    assert allowed("restricted", "guest") is False
    assert allowed("restricted", "superuser") is False
    assert allowed("private", "maintainer") is False

    handler = next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "org_handler"
    )
    source = ast.unparse(handler)
    assert "PATCH" in source
    assert "organization_world_settings_update" in source
    assert "worldCapabilities" in source
    assert "floors_visible" in source


def _floor_team_rows_namespace(rows):
    """_org_floor_team_rows over a fake org_team_members/org_members join."""
    calls = []

    async def d1_all(_env, sql, *params):
        calls.append((sql, params))
        assert "INNER JOIN org_members om" in sql
        assert "om.role IN ('owner','admin','maintainer','member','auditor')" in sql
        member = params[1] if len(params) > 1 else ""
        return [
            dict(row) for row in rows
            if not member or row["member_bi"] == member
        ]

    namespace = _load(
        "_org_floor_team_rows",
        "_office_team_slug",
        extra_globals={
            "d1_all": d1_all,
            "re": re,
            "OFFICE_FLOOR_TEAM_ALIASES": OFFICE_FLOOR_TEAM_ALIASES,
        },
    )
    namespace["calls"] = calls
    return namespace


def test_engineering_floor_teams_are_resolved_by_alias_not_one_slug():
    # An org that calls its Engineering team "Platform" or "developers" already
    # holds the Engineering floor, so the work behind that floor must admit the
    # same members (adhoc #1668). The board itself stays org-member-scoped.
    rows = [
        {"name": "ada", "team": "engineering", "member_bi": "bi:ada"},
        {"name": "bo", "team": "Platform", "member_bi": "bi:bo"},
        {"name": "cy", "team": "developers", "member_bi": "bi:cy"},
        {"name": "di", "team": "marketing", "member_bi": "bi:di"},
        {"name": "eli", "team": "legal", "member_bi": "bi:eli"},
    ]
    ns = _floor_team_rows_namespace(rows)
    floor_team_rows = ns["_org_floor_team_rows"]

    roster = _run(floor_team_rows(None, "org:acme", "engineering"))
    assert [row["name"] for row in roster] == ["ada", "bo", "cy"]

    # A per-account question narrows in SQL and still answers by alias.
    for member, expected in (
            ("bi:ada", True), ("bi:bo", True), ("bi:cy", True),
            ("bi:di", False), ("bi:eli", False)):
        answer = _run(floor_team_rows(
            None, "org:acme", "engineering", member_bi=member))
        assert bool(answer) is expected, member
        assert "AND tm.member_bi=?" in ns["calls"][-1][0]

    # Other floors keep their own rosters, and an unknown floor grants nobody.
    assert [row["name"] for row in _run(
        floor_team_rows(None, "org:acme", "marketing"))] == ["di"]
    assert _run(floor_team_rows(None, "org:acme", "legal")) == []


def _function_source(name):
    """Unparsed source of one function, top-level or inside a class body."""
    for node in ast.walk(ast.parse(ENTRY_TEXT)):
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name == name):
            return ast.unparse(node)
    raise AssertionError(name + " not found")


def test_engineering_task_surfaces_share_the_alias_resolver():
    # Both the agent workspace gate and the private task pings used to match the
    # single slug "engineering", so a member on an equivalent team saw neither.
    for name in ("_org_agent_member_context",):
        source = _function_source(name)
        assert "_org_floor_team_rows" in source, name
        assert "team='engineering'" not in source, name
    assert "team='engineering'" not in ENTRY_TEXT


def test_dashboard_exposes_world_building_identity_and_access_controls():
    source = (
        ROOT / "public" / "dashboard" / "js" / "04-account.js"
    ).read_text(encoding="utf-8")
    assert "data-org-world-settings" in source
    assert "data-org-world-logo" in source
    assert "data-org-world-lobby" in source
    assert "data-org-world-floors" in source
    assert "data-org-world-offices" in source
    assert 'orgApiRequest("PATCH", "/api/orgs/"' in source


def test_world_organization_directory_only_returns_enterable_lobbies():
    handler = next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "world_organizations_handler"
    )
    source = ast.unparse(handler)
    assert "GET" in source
    assert "ORDER BY created_at DESC LIMIT 64" in source
    assert "_org_world_access_allowed(access['lobby'], viewer_role)" in source
    assert "continue" in source
    assert "enterable-lobbies-only" in source
    assert "organizations" in source
    assert "org_repos" not in source


def test_public_office_access_never_publishes_member_names_for_guests():
    handler = next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "org_members_handler"
    )
    source = ast.unparse(handler)
    # The named roster is an insider read: anonymous visitors AND guests get
    # the aggregate (organization.roster.read in the deny-by-default matrix).
    assert "org_role_allows(viewer_role, 'organization.roster.read')" in source
    assert "public-redacted" in source
    assert "'members': []" in source
    assert "memberCount" in source
