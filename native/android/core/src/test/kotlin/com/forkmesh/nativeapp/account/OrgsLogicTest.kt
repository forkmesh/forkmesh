package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.model.OrgMember
import com.forkmesh.nativeapp.model.OrgProfile
import com.forkmesh.nativeapp.model.OrgRepo
import com.forkmesh.nativeapp.model.OrgSummary
import com.forkmesh.nativeapp.model.OrgTeam
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.model.orgErrorMessage
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class OrgsLogicTest {
    private fun session(kind: String = "account") = AuthSession.fromJson(
        mapOf("nodeName" to " Alice ", "email" to "alice@example.com", "status" to "active", "sessionKind" to kind),
    )

    fun testListsTheAccountOrgsWithRoleBadges() {
        val orgs = listOf(OrgSummary(name = "acme", role = "owner"), OrgSummary(name = "globex", role = "member"))
        expect(canUseOrgs(session()))
        assertEq("list-2", orgsListStateKey(loading = false, failed = false, orgs = orgs))
        assertEq(listOf("OWNER", "MEMBER"), orgs.map { orgRoleLabel(it.role).uppercase() })
        assertEq(OrgRoleTone.Accent, orgRoleTone("owner"))
        assertEq(OrgRoleTone.Warning, orgRoleTone("admin"))
        assertEq(OrgRoleTone.Tertiary, orgRoleTone("member"))
        assertEq("member", orgRoleLabel(""))
    }

    fun testPromptsSignInWhenThereIsNoAccountSession() {
        expect(!canUseOrgs(null))
        expect(!canUseOrgs(session(kind = "preview")))
    }

    fun testListStates() {
        assertEq("loading", orgsListStateKey(loading = true, failed = false, orgs = emptyList()))
        assertEq("error", orgsListStateKey(loading = false, failed = true, orgs = emptyList()))
        assertEq("empty", orgsListStateKey(loading = false, failed = false, orgs = emptyList()))
    }

    fun testErrorText() {
        assertEq("That name is already taken.", orgErrorText(OrgApiException("org_name_taken", orgErrorMessage("org_name_taken"))))
        assertEq("Something went wrong. Check your connection and try again.", orgErrorText(RuntimeException("boom")))
        assertEq("Something went wrong. Check your connection and try again.", orgErrorText(null))
    }

    fun testCreateAndPromptFormsNormalizeNames() {
        assertEq(NewOrgName.Invalid("Enter an organization name."), newOrgName("   "))
        assertEq(NewOrgName.Valid("acme-labs"), newOrgName(" Acme-Labs "))
        assertEq(null, orgMemberInput(" ", "admin"))
        assertEq(OrgMemberInput("bob", "admin"), orgMemberInput(" Bob ", "admin"))
        assertEq(OrgTeamInput("core", "write"), orgTeamInput("CORE", "write"))
        assertEq(null, orgTeamInput("", "read"))
        assertEq(OrgRepoInput("forkmesh", "Alice"), orgRepoInput(" ForkMesh ", " Alice "))
        assertEq(null, orgRepoInput(" ", "alice"))
        assertEq("alice", defaultRepoNode(session()))
        assertEq("", defaultRepoNode(null))
    }

    fun testMemberAndTeamMenus() {
        val admin = OrgMember(name = "bob", role = "admin")
        val roles = memberMenuRoles(admin)
        assertEq(listOf("owner", "admin", "maintainer", "member", "auditor", "guest"), roles.map { it.value })
        assertEq("Set role: owner", roles.first().label)
        assertEq(listOf("admin"), roles.filter { !it.enabled }.map { it.value })
        assertEq(OrgMenuAction.RemoveMember, memberMenuAction(MEMBER_MENU_REMOVE.value))
        assertEq("Remove from org", MEMBER_MENU_REMOVE.label)
        assertEq(OrgMenuAction.SetRole("owner"), memberMenuAction("owner"))

        val team = OrgTeam(team = "core", permission = "write", members = 1)
        val permissions = teamMenuPermissions(team)
        assertEq(listOf("perm:read", "perm:write", "perm:maintain", "perm:admin"), permissions.map { it.value })
        assertEq("Permission: read", permissions.first().label)
        assertEq(listOf("perm:write"), permissions.filter { !it.enabled }.map { it.value })
        assertEq(OrgMenuAction.SetPermission("maintain"), teamMenuAction("perm:maintain"))
        assertEq(OrgMenuAction.DeleteTeam, teamMenuAction(TEAM_MENU_DELETE.value))
        assertEq("Delete team", TEAM_MENU_DELETE.label)
        assertEq(null, teamMenuAction("other"))
    }

    fun testTeamRosterRules() {
        val org = listOf(OrgMember("alice", "owner"), OrgMember("bob", "member"), OrgMember("carol", "member"))
        val onTeam = listOf(OrgMember("bob", "member"))
        assertEq(listOf("alice", "carol"), teamAddCandidates(org, onTeam).map { it.name })
        assertEq(emptyList<OrgMember>(), teamAddCandidates(onTeam, onTeam))
        assertEq("Every org member is already on this team.", TEAM_ALREADY_FULL)
        assertEq("1 member", teamMemberCountLabel(1))
        assertEq("0 members", teamMemberCountLabel(0))
        val team = OrgTeam(team = "core", permission = "maintain")
        assertEq("maintain permission", teamSubtitle(team))
        assertEq("Add org members to grant them the maintain permission over linked repos.", teamEmptyMessage("", team))
        assertEq("Not found.", teamEmptyMessage("Not found.", team))
    }

    fun testOverviewAndRepoLabels() {
        val profile = OrgProfile(name = "acme", members = 3, teams = 1, repos = listOf(OrgRepo("app", "alice")))
        assertEq(listOf("Members" to 3, "Teams" to 1, "Repos" to 1), orgStats(profile))
        assertEq("acme/app", linkedRepoTitle("acme", OrgRepo("app", "alice")))
        assertEq("serves alice/app", linkedRepoServes(OrgRepo("app", "alice")))
        assertEq(null, linkedRepoServes(OrgRepo("app")))
        assertEq("Delete acme?", orgDeleteTitle("acme"))
    }
}
