#!/usr/bin/env python3
"""The dashboard's Organizations settings tab.

The /api/orgs endpoints (create/list orgs, members, teams, team members,
linked repos) previously had no web client. The Settings page now carries an
"Organizations" sub-tab whose JS (in dashboard/js/04-account.js, bundled into
dashboard.js) drives a master/detail panel over those endpoints. These are
static contracts: the markup hooks the JS binds to must exist, and the JS must
call each endpoint with the right method/path.
"""

from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
SETTINGS_VIEW = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html"
).read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()


def test_settings_view_has_organizations_tab_and_root():
    # The sub-tab nav button and its panel, plus the container the JS renders
    # into. setSettingsSection toggles panels by data-settings-section.
    assert 'data-settings-section-link="organizations"' in SETTINGS_VIEW
    assert 'data-settings-section="organizations"' in SETTINGS_VIEW
    assert "data-orgs-root" in SETTINGS_VIEW


def test_bundle_registers_organizations_section():
    # The tab is a no-op unless it is a recognised settings section and the
    # lazy loader is wired from setSettingsSection.
    assert '"nodes", "organizations", "danger"' in DASHBOARD_JS
    assert 'if (activeSection === "organizations") initOrgsSection();' in DASHBOARD_JS


def test_bundle_defines_the_org_client_functions():
    for symbol in (
        "async function orgApiRequest(",
        "function initOrgsSection(",
        "async function showOrgsList(",
        "async function onCreateOrgSubmit(",
        "async function showOrgDetail(",
        "function wireOrgDetail(",
        "async function showOrgTeamDetail(",
    ):
        assert symbol in DASHBOARD_JS, symbol


def test_bundle_calls_every_org_endpoint_shape():
    # GET list + POST create.
    assert 'orgApiRequest("GET", "/api/orgs")' in DASHBOARD_JS
    assert 'orgApiRequest("POST", "/api/orgs", body)' in DASHBOARD_JS
    # Detail collections.
    assert '"/api/orgs/" + encodeURIComponent(name) + "/members"' in DASHBOARD_JS
    assert '"/api/orgs/" + encodeURIComponent(name) + "/teams"' in DASHBOARD_JS
    assert '"/api/orgs/" + encodeURIComponent(name) + "/repos"' in DASHBOARD_JS
    # Team-member sub-view and org deletion.
    assert '/teams/" + encodeURIComponent(team) + "/members"' in DASHBOARD_JS
    assert 'orgApiRequest("DELETE", "/api/orgs/" + encodeURIComponent(name), {})' in DASHBOARD_JS


def test_org_error_codes_map_to_human_text():
    # A representative slug from every org handler must have a message so the
    # UI never shows a raw error code.
    for code in (
        "org_name_taken",
        "last_owner",
        "not_your_node",
        "unknown_repo",
        "too_many_members",
        "forbidden",
    ):
        assert code + ":" in DASHBOARD_JS, code


def test_member_picker_autocompletes_active_accounts_and_excludes_members():
    for contract in (
        'list="org-member-suggestions"',
        'role="combobox"',
        'aria-autocomplete="list"',
        "function wireOrgMemberAutocomplete(",
        '"/api/chat/direct-messages/users?query="',
        "existing.has(account)",
        "document.createElement(\"option\")",
        "matching active account",
    ):
        assert contract in DASHBOARD_JS


def test_org_detail_explains_member_role_and_team_access_on_the_right():
    for copy in (
        "What organization access means",
        "Every organization member can",
        "Engineering team members can",
        "organization role alone does not grant access",
        "Members outside Engineering also cannot view or control Claude/Codex sessions",
        "Member role cannot",
        "Admin role adds",
        "Owner role adds",
        "Team permission",
        "lg:grid-cols-[minmax(0,1fr)_22rem]",
        "data-org-access-summary",
    ):
        assert copy in DASHBOARD_JS


def test_org_admin_lists_every_office_floor_group_for_each_user():
    for contract in (
        "const ORG_OFFICE_FLOOR_GROUPS",
        "function orgMemberFloorGroups(member, canManage)",
        "data-org-member-floor-groups",
        "Office floor groups",
        "activeCount",
        "orgMemberFloorGroups(member, canManage)",
        '"Marketing"',
        '"Engineering"',
        '"Infrastructure"',
        '"frontend"',
        '"sre"',
    ):
        assert contract in DASHBOARD_JS
    # Retired office storeys no longer appear as floor groups; their team
    # names remain ordinary organization teams.
    for retired in (
        '"Product & Design"',
        '"Partnerships"',
        '"Executive"',
        '"devrel"',
    ):
        assert retired not in DASHBOARD_JS


def test_org_admin_floor_group_chips_toggle_team_membership():
    for contract in (
        "data-org-member-floor-group",
        'aria-pressed="',
        "const matchingTeams = group.aliases.filter",
        'if (!knownTeams.has(team))',
        '{ team, permission: "read" }',
        '"/members",',
        '"DELETE"',
        "{ member }",
    ):
        assert contract in DASHBOARD_JS
