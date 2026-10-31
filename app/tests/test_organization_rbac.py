#!/usr/bin/env python3
"""Organization authorization matrix (issue #487).

Admin, maintainer, member, auditor and guest roles pass an authorization
matrix with deny-by-default behaviour:

  * every (role, capability) pair is decided by one explicit table, and an
    unknown role, an empty role or an unknown capability is denied for
    everyone, owners included;
  * read-only roles (auditor, guest) never appear on a write capability, and
    a guest is never an insider;
  * role administration: owners assign anything, admins only the
    non-administrative roles, everyone else nothing;
  * repository permission baselines and ceilings per role;
  * every capability the Worker names exists in the matrix, no organization
    handler still hard-codes an owner/admin tuple, and the SQL role lists
    agree with the matrix;
  * the published documentation table is the matrix.

Run: python3 -m pytest app/tests/test_organization_rbac.py
"""

import ast
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
sys.path.insert(0, str(SRC))
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
WORLD_SRC = ROOT.parent / "world" / "src"
DOC = ROOT.parent / "www" / "docs" / "security" / "organization-roles.md"


def _module(name):
    spec = importlib.util.spec_from_file_location(name, SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rbac = _module("organization_rbac")
ROLES = rbac.ORG_ROLES
WRITE_ONLY_NEVER = ("auditor", "guest")


def _entry_function(name):
    for node in ast.parse(ENTRY_TEXT).body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name == name):
            return ast.get_source_segment(ENTRY_TEXT, node)
    raise AssertionError(name + " not found in entry.py")


# --- The matrix itself -------------------------------------------------------

def test_roles_are_the_issue_487_vocabulary_strongest_first():
    assert ROLES == (
        "owner", "admin", "maintainer", "member", "auditor", "guest")
    assert set(rbac.ORG_INSIDER_ROLES) == set(ROLES) - {"guest"}
    assert rbac.ORG_ADMINISTRATIVE_ROLES == ("owner", "admin")


def test_every_pair_is_decided_and_unknowns_are_denied():
    for capability in rbac.ORG_CAPABILITIES:
        kind, roles = rbac.ORG_AUTHORIZATION_MATRIX[capability]
        assert kind in ("read", "write"), capability
        assert set(roles) <= set(ROLES), capability
        assert roles, capability  # a capability nobody holds is a dead row
        for role in ROLES:
            decision = rbac.org_role_allows(role, capability)
            assert isinstance(decision, bool)
            assert decision is (role in roles), (role, capability)
        # deny-by-default on the role axis (whitespace/case are normalised,
        # anything else is a foreign role)
        for bogus in ("", None, "superuser", "owners", "own er", 0, object()):
            assert rbac.org_role_allows(bogus, capability) is False, (
                bogus, capability)
    # deny-by-default on the capability axis, owners included
    for bogus in ("", None, "organization.everything", "organization.READ"):
        for role in ROLES:
            assert rbac.org_role_allows(role, bogus) is False, (role, bogus)


def test_role_strings_are_normalised_but_never_widened():
    assert rbac.clean_org_role("  Admin ") == "admin"
    assert rbac.clean_org_role("guest") == "guest"
    assert rbac.clean_org_role("admins") == ""
    assert rbac.clean_org_role(None) == ""
    assert rbac.org_role_allows(" OWNER ", "organization.dissolve") is True


def test_read_only_roles_never_hold_a_write_capability():
    for capability, (kind, roles) in rbac.ORG_AUTHORIZATION_MATRIX.items():
        if kind == "write":
            for role in WRITE_ONLY_NEVER:
                assert role not in roles, (capability, role)
    # and a guest never crosses an insider boundary
    for capability in ("organization.roster.read", "organization.agents.read",
                       "organization.world.restricted",
                       "organization.chat.channels",
                       "organization.succession.approve"):
        assert rbac.org_role_allows("guest", capability) is False, capability
    assert rbac.org_role_is_insider("guest") is False
    assert rbac.org_role_is_insider("auditor") is True
    assert rbac.org_role_is_insider("nobody") is False


def test_owner_only_and_administrator_only_rows():
    for capability in ("organization.dissolve", "organization.roles.administer"):
        assert rbac.ORG_AUTHORIZATION_MATRIX[capability][1] == ("owner",)
    for capability in (
            "organization.settings.write", "organization.members.manage",
            "organization.teams.manage", "organization.repositories.link",
            "organization.repositories.import", "organization.issues.manage",
            "organization.fediverse.manage", "organization.bots.manage",
            "organization.tasks.administer", "organization.world.private"):
        assert set(rbac.ORG_AUTHORIZATION_MATRIX[capability][1]) == {
            "owner", "admin"}, capability
    # maintainers ship code but administer nothing
    for capability, (kind, roles) in rbac.ORG_AUTHORIZATION_MATRIX.items():
        if "maintainer" in roles and kind == "write":
            assert capability in (
                "organization.agents.queue", "organization.bots.link",
                "organization.chat.channels",
                "organization.succession.approve"), capability


# --- Role administration -----------------------------------------------------

def test_role_assignment_matrix():
    can = rbac.org_role_can_assign
    for target in ROLES:
        assert can("owner", target) is True, target
        assert can("owner", target, "owner") is True, target
    for target in ("maintainer", "member", "auditor", "guest"):
        assert can("admin", target) is True, target
        assert can("admin", target, "member") is True, target
        assert can("admin", target, "admin") is False, target  # no demotion
        assert can("admin", target, "owner") is False, target
    for target in ("owner", "admin"):
        assert can("admin", target) is False, target
    for caller in ("maintainer", "member", "auditor", "guest", "", None,
                   "superuser"):
        for target in ROLES:
            assert can(caller, target) is False, (caller, target)
    # an unknown target role is never assignable, even by an owner
    assert can("owner", "superuser") is False
    assert can("owner", "") is False


def test_role_removal_matrix():
    can = rbac.org_role_can_remove
    for existing in ROLES:
        assert can("owner", existing) is True, existing
    for existing in ("maintainer", "member", "auditor", "guest"):
        assert can("admin", existing) is True, existing
    for existing in ("owner", "admin"):
        assert can("admin", existing) is False, existing
    for caller in ("maintainer", "member", "auditor", "guest", "", "x"):
        for existing in ROLES:
            assert can(caller, existing) is False, (caller, existing)
    assert can("owner", "") is False
    assert can("owner", "superuser") is False


# --- Repository permissions --------------------------------------------------

def test_repository_permission_baselines_and_ceilings():
    baseline = rbac.org_role_repo_permission
    ceiling = rbac.org_role_repo_permission_ceiling
    assert (baseline("owner"), ceiling("owner")) == ("admin", "admin")
    assert (baseline("admin"), ceiling("admin")) == ("admin", "admin")
    assert (baseline("maintainer"), ceiling("maintainer")) == (
        "maintain", "admin")
    assert (baseline("member"), ceiling("member")) == ("read", "admin")
    assert (baseline("auditor"), ceiling("auditor")) == ("read", "read")
    assert (baseline("guest"), ceiling("guest")) == ("", "read")
    for bogus in ("", None, "superuser"):
        assert (baseline(bogus), ceiling(bogus)) == ("", "")


# --- Enforcement in the Worker ----------------------------------------------

_MATRIX_CALL_RE = re.compile(
    r'org_role_allows\(((?:[^()]|\([^()]*\))*)\)', re.DOTALL)
_CAPABILITY_RE = re.compile(r'"(organization\.[a-z_.]+)"')


def _matrix_call_capabilities(text):
    # Every string literal inside an org_role_allows(...) call, including the
    # branches of a conditional expression choosing between two capabilities.
    found = set()
    for match in _MATRIX_CALL_RE.finditer(text):
        found |= set(_CAPABILITY_RE.findall(match.group(1)))
    return found


def test_every_capability_the_worker_names_is_in_the_matrix():
    named = _matrix_call_capabilities(ENTRY_TEXT)
    assert named, "entry.py never consults the matrix"
    unknown = named - set(rbac.ORG_AUTHORIZATION_MATRIX)
    assert not unknown, sorted(unknown)
    # Every matrix row is enforced somewhere: a documented capability nobody
    # checks would be a promise the Worker does not keep. Rows that are
    # enumerated in SQL or on a client name the capability at that site.
    enforced_elsewhere = {
        "organization.read": None,  # the unauthenticated public profile
        "organization.chat.channels": ROOT / "public" / "chat.js",
        "organization.succession.approve": SRC / "organization_succession.py",
        # Consulted by org_role_can_assign / org_role_can_remove, which the
        # members handler calls (test_organization_handlers_decide_...).
        "organization.roles.administer": SRC / "organization_rbac.py",
        "organization.pull_rules.manage": SRC / "pull_rules.py",
    }
    for capability, path in enforced_elsewhere.items():
        if path is not None:
            assert capability in path.read_text(encoding="utf-8"), capability
    missing = set(rbac.ORG_AUTHORIZATION_MATRIX) - named - set(
        enforced_elsewhere)
    assert not missing, sorted(missing)


def test_organization_handlers_decide_through_the_matrix_only():
    handlers = (
        "org_handler", "org_members_handler", "org_teams_handler",
        "org_team_members_handler", "org_repos_handler",
        "org_fediverse_handler", "org_bot_tokens_handler",
        "_org_agent_member_context", "_repository_import_target_owner",
        "_repository_import_can_manage_owner", "_authorize_issue_manager",
        "_admin_error_create_bot_task", "_org_world_access_allowed",
        "_org_permission", "genie_credential_handler",
    )
    for name in handlers:
        source = _entry_function(name)
        assert "_org_rbac." in source, name
        assert '("owner", "admin")' not in source, name
        assert '("owner", "admin", "member")' not in source, name
        assert "in ORG_ROLES" not in source or name == "org_members_handler", name
    # Request validation still uses the shared vocabulary.
    members = _entry_function("org_members_handler")
    assert "if role not in ORG_ROLES:" in members
    assert "_org_rbac.org_role_can_assign(caller_role, role, existing_role)" in members
    assert "_org_rbac.org_role_can_remove(caller_role, existing_role)" in members
    assert 'org_role_allows(\n                viewer_role, "organization.roster.read")' in members
    org = _entry_function("org_handler")
    assert '"organization.dissolve"' in org
    assert '"organization.settings.write"' in org
    bots = _entry_function("org_bot_tokens_handler")
    assert '"organization.bots.manage" if method in ("GET", "DELETE")' in bots
    assert 'else "organization.bots.link"' in bots


def test_entry_role_vocabulary_matches_the_module():
    for node in ast.parse(ENTRY_TEXT).body:
        if (isinstance(node, ast.Assign)
                and any(getattr(t, "id", "") == "ORG_ROLES"
                        for t in node.targets)):
            assert ast.literal_eval(node.value) == ROLES
            break
    else:
        raise AssertionError("ORG_ROLES not found")
    assert 'ORG_ROLE_OPTIONS = ["owner", "admin", "maintainer", "member", "auditor", "guest"]' in (
        ROOT / "public" / "dashboard" / "js" / "04-account.js"
    ).read_text(encoding="utf-8")
    ios = (ROOT.parent / "native" / "ios" / "ForkMeshCore" / "Sources" / "FMApi"
           / "Models" / "Orgs.swift").read_text(encoding="utf-8")
    assert 'public let orgRoles = ["owner", "admin", "maintainer", "member", "auditor", "guest"]' in ios
    android = (ROOT.parent / "native" / "android" / "core" / "src" / "main" / "kotlin"
               / "com" / "forkmesh" / "nativeapp" / "model" / "Orgs.kt").read_text(encoding="utf-8")
    assert 'listOf("owner", "admin", "maintainer", "member", "auditor", "guest")' in android


def test_sql_role_lists_agree_with_the_matrix():
    insiders = rbac.ORG_INSIDER_ROLE_SQL
    approvers = rbac.ORG_SUCCESSION_APPROVER_ROLE_SQL
    assert insiders == "('owner','admin','maintainer','member','auditor')"
    assert approvers == "('owner','admin','maintainer','member')"
    assert tuple(rbac.ORG_AUTHORIZATION_MATRIX[
        "organization.succession.approve"][1]) == (
            "owner", "admin", "maintainer", "member")
    # No roster query still enumerates the pre-#487 three-role list.
    stale = "IN ('owner','admin','member')"
    for path in [SRC / "entry.py", SRC / "schema.py",
                 SRC / "organization_succession_api.py"] + sorted(
                     WORLD_SRC.glob("*.py")):
        assert stale not in path.read_text(encoding="utf-8"), path.name
    assert "om.role IN " + insiders in _entry_function("_org_floor_team_rows")
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    assert schema.count("m.role IN " + approvers) == 3
    api = (SRC / "organization_succession_api.py").read_text(encoding="utf-8")
    assert "m.role IN " + approvers in api
    office = (WORLD_SRC / "world_office_routes.py").read_text(encoding="utf-8")
    assert office.count("om.role IN " + insiders) == 3
    migration = (ROOT / "migrations" / "0183_organization_rbac_roles.sql"
                 ).read_text(encoding="utf-8")
    assert migration.count("m.role IN " + approvers) == 2
    assert "DROP TRIGGER IF EXISTS trg_org_succession_approval_authorized" in migration
    assert "DROP TRIGGER IF EXISTS trg_org_succession_completion_guard" in migration
    down = (ROOT / "migrations" / "down" / "0183_organization_rbac_roles.sql"
            ).read_text(encoding="utf-8")
    assert down.count("m.role IN ('owner','admin','member')") == 2


def test_succession_and_discord_take_their_roles_from_the_matrix():
    succession = _module("organization_succession")
    assert succession.ORG_ROLES == frozenset(
        rbac.ORG_AUTHORIZATION_MATRIX["organization.succession.approve"][1])
    assert succession.eligible_approver("auditor", "cy", "ada", "bo") is False
    assert succession.eligible_approver("guest", "cy", "ada", "bo") is False
    assert succession.eligible_approver("maintainer", "cy", "ada", "bo") is True
    discord = _module("organization_discord")
    assert discord.ORG_ROLES == frozenset(rbac.ORG_INSIDER_ROLES)


# --- Documentation ----------------------------------------------------------

def test_published_documentation_table_is_the_matrix():
    doc = DOC.read_text(encoding="utf-8")
    assert rbac.org_authorization_matrix_markdown() in doc
    for role in ROLES:
        assert "| `" + role + "` |" in doc, role
    readme = (ROOT.parent / "www" / "docs" / "README.md").read_text(
        encoding="utf-8")
    assert "security/organization-roles.md" in readme
