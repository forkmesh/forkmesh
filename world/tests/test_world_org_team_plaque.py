#!/usr/bin/env python3
"""Organization team assignment from the back of a World avatar (adhoc #438).

An organization owner or administrator sees one extra plaque on the back of
every campfire avatar belonging to a member of an organization they manage,
whether that member is currently logged in or represented by their directory
figure. Opening it shows accessible checkboxes for that organization's teams;
saving writes the same /api/orgs/<org>/teams/<team>/members grants the settings
page makes, and team permission is what opens the organization's repository
floors and offices.

These tests pin:

  * GET /api/orgs/<org>/members carries each member's team list (one roster
    read instead of one request per team) and the anonymous public-redacted
    branch still exposes no names and no teams;
  * the scene renders the plaque on the avatar BACK (positive Z, single sided)
    for live, inactive, and directory-seated registered members, never for
    guests, bots, or node peers, and registers/cleans it up like the existing
    moderation plaques;
  * the plaque texture carries names and counts only — never a session token;
  * the app layer builds the plaque only for organizations whose viewerRole is
    owner or admin, and the dialog uses real checkboxes whose save diffs into
    POST/DELETE team-member writes, re-reads the roster, and refreshes the
    signed-in viewer's server-authoritative elevator grants once.

Run: python3 -m pytest cloudflare_worker/tests/test_world_org_team_plaque.py
"""

import ast
import asyncio
import importlib.util
from pathlib import Path
from world_scene_source import world_scene_source

ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
SCENE = world_scene_source()
APP = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")


def _module(name):
    spec = importlib.util.spec_from_file_location(
        name, ROOT / "src" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


catalog = _module("catalog")
org_rbac = _module("organization_rbac")


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


ORG_ROLES = _constant("ORG_ROLES")
ORG_WORLD_ACCESS_VALUES = _constant("ORG_WORLD_ACCESS_VALUES")


# --- Roster read -------------------------------------------------------------

class _Request:
    method = "GET"


def _members_handler(viewer, viewer_role):
    queries = []

    async def noop(env):
        return None

    async def org_row(env, org):
        return "org-bi", {"name": org, "data": "enc"}

    async def session_record(env, request, data=None):
        return None, ({"name": viewer} if viewer else None)

    async def org_role(env, org_bi, account):
        return viewer_role if account == viewer else "member"

    async def decrypt_row(env, blob):
        return {"worldAccess": {"lobby": "public", "floors": "restricted",
                                "offices": "restricted"}}

    async def d1_all(env, sql, *params):
        queries.append(sql)
        if "FROM org_team_members" in sql:
            return [
                {"name": "ada", "team": "core"},
                {"name": "ada", "team": "release"},
                {"name": "grace", "team": "core"},
            ]
        return [
            {"name": "ada", "role": "member", "created_at": 11},
            {"name": "grace", "role": "admin", "created_at": 12},
            {"name": "linus", "role": "member", "created_at": 13},
        ]

    async def d1_first(env, sql, *params):
        queries.append(sql)
        return {"n": 3}

    def response(data, status=200, **kwargs):
        return {"status": status, "data": data}

    async def name_case(env, name):
        return {"ada": "Ada"}.get(name, "")

    namespace = _load(
        "org_members_handler",
        "_org_world_access",
        "_org_world_access_allowed",
        extra_globals={
            "ORG_ROLES": ORG_ROLES,
            "ORG_WORLD_ACCESS_VALUES": ORG_WORLD_ACCESS_VALUES,
            "ensure_schema": noop,
            "method_name": lambda request: request.method,
            "_org_row": org_row,
            "_account_session_record": session_record,
            "_org_management_session": session_record,
            "_org_role": org_role,
            "decrypt_row": decrypt_row,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "json_response": response,
            "clean_string": catalog.clean_string,
            "_org_rbac": org_rbac,
            "_account_name_case": name_case,
        },
    )
    return namespace["org_members_handler"], queries


def test_member_roster_carries_each_members_teams():
    handler, queries = _members_handler("grace", "admin")
    result = _run(handler(None, _Request(), "acme"))
    assert result["status"] == 200
    assert result["data"]["members"] == [
        {"name": "ada", "nameCase": "Ada", "role": "member", "since": 11,
         "teams": ["core", "release"]},
        {"name": "grace", "nameCase": "grace", "role": "admin", "since": 12,
         "teams": ["core"]},
        {"name": "linus", "nameCase": "linus", "role": "member", "since": 13,
         "teams": []},
    ]
    # One roster read plus one team read: never one request per team.
    assert sum("org_team_members" in sql for sql in queries) == 1


def test_anonymous_roster_read_exposes_neither_names_nor_teams():
    handler, _ = _members_handler("", "")
    result = _run(handler(None, _Request(), "acme"))
    # Default office visibility is "restricted", so an anonymous read is
    # rejected outright; a public hallway still redacts every name.
    assert result["status"] == 403
    assert result["data"] == {"error": "forbidden"}
    assert '"visibility": "public-redacted"' in ENTRY_TEXT
    redacted = ENTRY_TEXT[
        ENTRY_TEXT.index('"visibility": "public-redacted"'):
        ENTRY_TEXT.index(
            "rows = await d1_all(",
            ENTRY_TEXT.index('"visibility": "public-redacted"'),
        )
    ]
    assert '"members": []' in redacted
    assert "teams" not in redacted


def test_team_member_writes_stay_owner_admin_only():
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def org_team_members_handler"):
        ENTRY_TEXT.index("async def org_repos_handler")
    ]
    assert '"organization.teams.manage"' in handler
    assert org_rbac.ORG_AUTHORIZATION_MATRIX["organization.teams.manage"][1] \
        == ("owner", "admin")
    assert '{"error": "forbidden"}' in handler
    # Ordinary membership still rejects outsiders unless the admin explicitly
    # selects the limited external-collaborator path.
    assert '"not_a_member"' in handler
    assert 'data.get("external") is True' in handler
    assert "org_team_collaborators" in handler
    assert '{"error": "active_user_required"}' in handler


# --- The plaque on the avatar back -------------------------------------------

def test_team_plaque_rides_live_and_offline_member_avatar_backs():
    for contract in (
        "const WORLD_ORG_NAME_PATTERN = /^[a-z0-9][a-z0-9-]{0,39}$/",
        "function sanitizedOrgTeamAssignment(remote)",
        "/^(?:node|bot):/",
        'remote?.accountStatus === "Verified bot"',
        'String(remote?.accountStatus || "Guest") === "Guest"',
        "function createAvatarOrgTeamControl",
        '"ASSIGN TEAMS"',
        "control.position.set(0, 1.61, 0.321)",
        "control.userData.worldOrgTeamControl = true",
        "onOrgTeamAssign = () => {}",
        "orgTeamActions.set(control",
        "interactive.push(control)",
        "function removeRemoteOrgTeamControl",
        "orgTeamActions.delete(child)",
        "interactive.splice(interactiveIndex, 1)",
        "disposeObject3D(controls)",
        "syncRemoteOrgTeamControl(avatar, remote)",
        "removeRemoteOrgTeamControl(avatar, id)",
        "syncRemoteOrgTeamControl(figure, {",
        "orgTeam: member?.orgTeam",
    ):
        assert contract in SCENE, contract

    sanitizer = SCENE[
        SCENE.index("function sanitizedOrgTeamAssignment(remote)"):
        SCENE.index("function orgTeamControlTexture")
    ]
    # These are real registered accounts rendered from server-backed presence
    # or the public users directory, so being offline/local is not a reason to
    # remove an organization administrator's control.
    assert "persistedInactive" not in sanitizer
    assert "inactive|local" not in sanitizer

    lounge = SCENE[
        SCENE.index("  function updateMemberLounge("):
        SCENE.index("  function visitNeighborhoodHome(")
    ]
    assert lounge.count("removeRemoteOrgTeamControl(figure, id)") >= 1

    click = SCENE[
        SCENE.index("const orgTeamAction = hit?.object"):
        SCENE.index(
            "if (hit?.object?.userData?.campfireLog)",
            SCENE.index("const orgTeamAction = hit?.object"),
        )
    ]
    for field in ("org", "member", "peerId", "name"):
        assert f"{field}: orgTeamAction.{field}" in click
    assert "onOrgTeamAssign({" in click


def test_team_plaque_texture_carries_no_secret():
    texture = SCENE[
        SCENE.index("function orgTeamControlTexture"):
        SCENE.index("function createAvatarOrgTeamControl")
    ]
    for secret in ("token", "session", "handle", "email"):
        assert secret not in texture.lower(), secret


# --- The team checkboxes the plaque opens ------------------------------------

def test_all_org_members_get_team_badges_but_only_admins_get_controls():
    managed = APP[
        APP.index("  managedOrganizations() {"):
        APP.index("  managedOrganization(name) {")
    ]
    assert '["owner", "admin"].includes(' in managed
    index = APP[
        APP.index("  rebuildOrgTeamIndex() {"):
        APP.index("  orgTeamAssignmentFor(name) {")
    ]
    assert "this.organizations.forEach" in index
    assert "const canManage" in index
    assert "canManage," in index
    assert "WORLD_ACCOUNT_NAME_RE.test(account)" in index
    # A guest may type any display name; only server-stamped accounts resolve.
    assert 'String(player?.accountStatus || "Guest") === "Guest"' in APP
    assert "orgTeam ? { orgTeam } : {}" in APP
    assert "onOrgTeamAssign: (target) => this.openOrgTeamAssignment(target)" \
        in APP

    sync = SCENE[
        SCENE.index("  function syncRemoteOrgTeamControl"):
        SCENE.index("  function setLocalOrgTeam")
    ]
    assert "if (assignment.canManage)" in sync
    assert "if (assignment.teams.length && avatar.userData.leftArm)" in sync
    badges = SCENE[
        SCENE.index("function createAvatarTeamBadges"):
        SCENE.index("function orgTeamControlTexture")
    ]
    assert "forkmesh-avatar-left-arm-team-badges" in badges
    # Small patches on the arm's outward (-X) face, not slabs overhanging the
    # sleeve on the front of the avatar.
    assert "new THREE.PlaneGeometry(0.26, 0.09)" in badges
    assert "badge.rotation.y = -Math.PI / 2" in badges
    assert "badge.position.set(-0.148, 0.36 - index * 0.105, 0)" in badges
    assert "forkmesh-avatar-organization-level-stars" in badges
    assert '"★".repeat(organizationLevel)' in badges
    assert "setLocalOrgTeam," in SCENE


def test_campfire_directory_hands_offline_members_the_same_assignment():
    lounge = APP[
        APP.index("  syncMemberLounge() {"):
        APP.index("  destroy() {", APP.index("  syncMemberLounge() {"))
    ]
    assert "this.memberDirectory.map((member) => ({" in lounge
    assert "orgTeam: this.orgTeamAssignmentFor(member.name)" in lounge
    assert "away: present.has(member.name.toLowerCase())" in lounge


def test_assignment_dialog_uses_checkboxes_and_diffs_into_team_writes():
    dialog = APP[
        APP.index("  openOrgTeamAssignment(target = {}) {"):
        APP.index("  bindUI() {")
    ]
    assert "this.managedOrganization(org)" in dialog
    assert "Organization administrator access is required." in dialog
    assert "<fieldset" in dialog
    assert "<legend" in dialog and ">Teams</legend>" in dialog
    assert 'type="checkbox" name="teams"' in dialog
    assert "form.querySelectorAll('input[name=\"teams\"]:checked')" in dialog
    assert '<select name="teams" multiple' not in dialog
    assert "selectedOptions" not in dialog
    assert "const added = [...selected].filter((team) => !current.has(team))" \
        in dialog
    assert "const removed = [...current].filter((team) => !selected.has(team))" \
        in dialog
    assert '`${root}/${encodeURIComponent(team)}/members`' in dialog
    assert 'grant ? {} : { method: "DELETE" }' in dialog
    assert "await this.refreshOrganizationTeams(org)" in dialog
    # The plaque names the one website setting behind both views and explains
    # the Office elevator effect without making the client authoritative.
    assert "same organization teams" in dialog
    assert "website organization settings" in dialog
    assert "officeFloorsForTeam(team?.team)" in dialog
    assert "Unlocks Office elevator" in dialog
    assert "No additional Office elevator floor" in dialog

    # Saving a different user's grants does not touch this browser's elevator.
    # Saving the signed-in viewer performs exactly one deliberate refresh;
    # the controller re-reads the authoritative allowlist without polling.
    assert "let savedChanges = 0" in dialog
    assert "savedChanges += 1" in dialog
    assert "savedChanges > 0" in dialog
    assert 'member === String(validWorldSession()?.nodeName || "").toLowerCase()' \
        in dialog
    assert "await this.officeController?.refreshAuthorization?.()" in dialog

    refresh = APP[
        APP.index("  async refreshOrganizationTeams(name) {"):
        APP.index("  openOrgTeamAssignment(target = {}) {")
    ]
    assert "/members" in refresh and "/teams" in refresh
    assert "this.rebuildOrgTeamIndex()" in refresh
    assert "this.renderPeers()" in refresh
