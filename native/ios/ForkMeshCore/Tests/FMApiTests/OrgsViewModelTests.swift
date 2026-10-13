// The orgs screen's logic and the org detail rules (native/android OrgsLogicTest.kt),
// and drives the org view models over a fake OrgClient.

import FMAccount
import FMBase
import XCTest
@testable import FMApi

private func orgSession(kind: String = "account") -> AuthSession {
    AuthSession.fromJSON([
        "nodeName": " Alice ", "email": "alice@example.com", "status": "active", "sessionKind": .string(kind),
    ])
}

private struct Offline: Error {}

/// Records calls and answers from canned values.
@MainActor
private final class FakeOrgClient: OrgClient {
    var calls: [String] = []
    var orgs: [OrgSummary] = []
    var profile = OrgProfile(name: "acme", viewerRole: "owner")
    var members: [OrgMember] = []
    var teams: [OrgTeam] = []
    var repos: [OrgRepo] = []
    var teamMembers: [OrgMember] = []
    var failure: Error?
    var failMutations: Error?

    private func check() throws {
        if let failure { throw failure }
    }

    private func mutate(_ call: String) throws {
        calls.append(call)
        if let failMutations { throw failMutations }
    }

    func myOrgs() async throws -> [OrgSummary] {
        calls.append("myOrgs")
        try check()
        return orgs
    }

    func createOrg(name: String, displayName: String, description: String) async throws -> OrgSummary {
        try mutate("create \(name) \(displayName)|\(description)")
        return OrgSummary(name: name, role: "owner")
    }

    func orgProfile(_ org: String) async throws -> OrgProfile {
        try check()
        return profile
    }

    func deleteOrg(_ org: String) async throws { try mutate("delete \(org)") }

    func orgMembers(_ org: String) async throws -> [OrgMember] {
        try check()
        return members
    }

    func setOrgMember(_ org: String, _ member: String, role: String) async throws {
        try mutate("member \(org) \(member) \(role)")
    }

    func removeOrgMember(_ org: String, _ member: String) async throws { try mutate("remove \(org) \(member)") }

    func orgTeams(_ org: String) async throws -> [OrgTeam] {
        try check()
        return teams
    }

    func setOrgTeam(_ org: String, _ team: String, permission: String) async throws {
        try mutate("team \(org) \(team) \(permission)")
    }

    func deleteOrgTeam(_ org: String, _ team: String) async throws { try mutate("deleteTeam \(org) \(team)") }

    func orgTeamMembers(_ org: String, _ team: String) async throws -> [OrgMember] {
        try check()
        return teamMembers
    }

    func addOrgTeamMember(_ org: String, _ team: String, _ member: String) async throws {
        try mutate("addToTeam \(team) \(member)")
    }

    func removeOrgTeamMember(_ org: String, _ team: String, _ member: String) async throws {
        try mutate("removeFromTeam \(team) \(member)")
    }

    func orgRepos(_ org: String) async throws -> [OrgRepo] {
        try check()
        return repos
    }

    func linkOrgRepo(_ org: String, _ repo: String, node: String) async throws {
        try mutate("link \(repo) \(node)")
    }

    func unlinkOrgRepo(_ org: String, _ repo: String) async throws { try mutate("unlink \(repo)") }
}

final class OrgsViewModelTests: XCTestCase {
    func testListsTheAccountOrgsWithRoleBadges() {
        let orgs = [OrgSummary(name: "acme", role: "owner"), OrgSummary(name: "globex", role: "member")]
        XCTAssertTrue(canUseOrgs(orgSession()))
        XCTAssertEqual(orgsListStateKey(loading: false, failed: false, orgs: orgs), "list-2")
        XCTAssertEqual(orgs.map { orgRoleLabel($0.role).uppercased() }, ["OWNER", "MEMBER"])
        XCTAssertEqual(orgRoleTone("owner"), .accent)
        XCTAssertEqual(orgRoleTone("admin"), .warning)
        XCTAssertEqual(orgRoleTone("member"), .tertiary)
        XCTAssertEqual(orgRoleLabel(""), "member")
    }

    func testPromptsSignInWhenThereIsNoAccountSession() {
        XCTAssertFalse(canUseOrgs(nil))
        XCTAssertFalse(canUseOrgs(orgSession(kind: "preview")))
    }

    func testListStates() {
        XCTAssertEqual(orgsListStateKey(loading: true, failed: false, orgs: []), "loading")
        XCTAssertEqual(orgsListStateKey(loading: false, failed: true, orgs: []), "error")
        XCTAssertEqual(orgsListStateKey(loading: false, failed: false, orgs: []), "empty")
    }

    func testErrorText() {
        XCTAssertEqual(
            orgErrorText(OrgApiException("org_name_taken", orgErrorMessage("org_name_taken"))), "That name is already taken.")
        XCTAssertEqual(orgErrorText(Offline()), "Something went wrong. Check your connection and try again.")
        XCTAssertEqual(orgErrorText(nil), "Something went wrong. Check your connection and try again.")
    }

    func testCreateAndPromptFormsNormalizeNames() {
        XCTAssertEqual(newOrgName("   "), .invalid("Enter an organization name."))
        XCTAssertEqual(newOrgName(" Acme-Labs "), .valid("acme-labs"))
        XCTAssertNil(orgMemberInput(" ", "admin"))
        XCTAssertEqual(orgMemberInput(" Bob ", "admin"), OrgMemberInput("bob", "admin"))
        XCTAssertEqual(orgTeamInput("CORE", "write"), OrgTeamInput("core", "write"))
        XCTAssertNil(orgTeamInput("", "read"))
        XCTAssertEqual(orgRepoInput(" ForkMesh ", " Alice "), OrgRepoInput("forkmesh", "Alice"))
        XCTAssertNil(orgRepoInput(" ", "alice"))
        XCTAssertEqual(defaultRepoNode(orgSession()), "alice")
        XCTAssertEqual(defaultRepoNode(nil), "")
    }

    func testMemberAndTeamMenus() {
        let admin = OrgMember(name: "bob", role: "admin")
        let roles = memberMenuRoles(admin)
        XCTAssertEqual(roles.map(\.value), ["owner", "admin", "maintainer", "member", "auditor", "guest"])
        XCTAssertEqual(roles.first?.label, "Set role: owner")
        XCTAssertEqual(roles.filter { !$0.enabled }.map(\.value), ["admin"])
        XCTAssertEqual(memberMenuAction(memberMenuRemove.value), .removeMember)
        XCTAssertEqual(memberMenuRemove.label, "Remove from org")
        XCTAssertEqual(memberMenuAction("owner"), .setRole("owner"))

        let team = OrgTeam(team: "core", permission: "write", members: 1)
        let permissions = teamMenuPermissions(team)
        XCTAssertEqual(permissions.map(\.value), ["perm:read", "perm:write", "perm:maintain", "perm:admin"])
        XCTAssertEqual(permissions.first?.label, "Permission: read")
        XCTAssertEqual(permissions.filter { !$0.enabled }.map(\.value), ["perm:write"])
        XCTAssertEqual(teamMenuAction("perm:maintain"), .setPermission("maintain"))
        XCTAssertEqual(teamMenuAction(teamMenuDelete.value), .deleteTeam)
        XCTAssertEqual(teamMenuDelete.label, "Delete team")
        XCTAssertNil(teamMenuAction("other"))
    }

    func testTeamRosterRules() {
        let org = [OrgMember(name: "alice", role: "owner"), OrgMember(name: "bob"), OrgMember(name: "carol")]
        let onTeam = [OrgMember(name: "bob")]
        XCTAssertEqual(teamAddCandidates(org, onTeam).map(\.name), ["alice", "carol"])
        XCTAssertEqual(teamAddCandidates(onTeam, onTeam), [])
        XCTAssertEqual(teamAlreadyFull, "Every org member is already on this team.")
        XCTAssertEqual(teamMemberCountLabel(1), "1 member")
        XCTAssertEqual(teamMemberCountLabel(0), "0 members")
        let team = OrgTeam(team: "core", permission: "maintain")
        XCTAssertEqual(teamSubtitle(team), "maintain permission")
        XCTAssertEqual(
            teamEmptyMessage("", team), "Add org members to grant them the maintain permission over linked repos.")
        XCTAssertEqual(teamEmptyMessage("Not found.", team), "Not found.")
    }

    func testOverviewAndRepoLabels() {
        let profile = OrgProfile(name: "acme", members: 3, teams: 1, repos: [OrgRepo(repo: "app", node: "alice")])
        XCTAssertEqual(
            orgStats(profile),
            [OrgStat(label: "Members", value: 3), OrgStat(label: "Teams", value: 1), OrgStat(label: "Repos", value: 1)])
        XCTAssertEqual(linkedRepoTitle("acme", OrgRepo(repo: "app", node: "alice")), "acme/app")
        XCTAssertEqual(linkedRepoServes(OrgRepo(repo: "app", node: "alice")), "serves alice/app")
        XCTAssertNil(linkedRepoServes(OrgRepo(repo: "app")))
        XCTAssertEqual(orgDeleteTitle("acme"), "Delete acme?")
    }

    // MARK: Models

    @MainActor
    func testTheListLoadsForARealSessionOnly() async {
        let client = FakeOrgClient()
        client.orgs = [OrgSummary(name: "acme", role: "owner")]
        let model = OrgsListModel(client: client, session: orgSession())
        XCTAssertTrue(model.signedIn)
        await model.reload()
        XCTAssertEqual(model.orgs.map(\.name), ["acme"])
        XCTAssertEqual(model.stateKey, "list-1")

        let preview = OrgsListModel(client: client, session: orgSession(kind: "preview"))
        await preview.reload()
        XCTAssertFalse(preview.signedIn)
        XCTAssertEqual(client.calls, ["myOrgs"])

        client.failure = OrgApiException("forbidden", "No access.")
        await model.reload()
        XCTAssertEqual(model.stateKey, "error")
        XCTAssertEqual(model.failure, "No access.")
    }

    @MainActor
    func testCreateValidatesThenCreates() async {
        let client = FakeOrgClient()
        let model = CreateOrgModel(client: client)
        let none = await model.submit()
        XCTAssertNil(none)
        XCTAssertEqual(model.error, "Enter an organization name.")
        model.name = " Acme "
        model.displayName = "Acme Inc"
        let created = await model.submit()
        XCTAssertEqual(created?.name, "acme")
        XCTAssertEqual(client.calls, ["create acme Acme Inc|"])

        client.failMutations = OrgApiException("org_name_taken", "That name is already taken.")
        let refused = await model.submit()
        XCTAssertNil(refused)
        XCTAssertEqual(model.error, "That name is already taken.")
        XCTAssertFalse(model.busy)
    }

    @MainActor
    func testDetailLoadsEverythingAndRunsActions() async {
        let client = FakeOrgClient()
        client.profile = OrgProfile(name: "acme", displayName: "Acme", viewerRole: "owner")
        client.members = [OrgMember(name: "bob", role: "member")]
        client.teams = [OrgTeam(team: "core", permission: "read")]
        client.repos = [OrgRepo(repo: "app", node: "alice")]
        let model = OrgDetailModel(client: client, orgName: "acme", session: orgSession())
        await model.load()
        XCTAssertFalse(model.loading)
        XCTAssertEqual(model.title, "Acme")
        XCTAssertEqual(model.subtitle, "/acme")
        XCTAssertTrue(model.canManage)
        XCTAssertTrue(model.isOwner)
        XCTAssertEqual(model.defaultNode, "alice")

        await model.memberMenu(client.members[0], choose: "admin")
        await model.memberMenu(client.members[0], choose: "remove")
        await model.teamMenu(client.teams[0], choose: "perm:write")
        await model.teamMenu(client.teams[0], choose: "delete")
        await model.addMember(OrgMemberInput("carol", "member"))
        await model.addTeam(OrgTeamInput("ops", "admin"))
        await model.linkRepo(OrgRepoInput("web", "alice"))
        await model.unlink(client.repos[0])
        XCTAssertEqual(client.calls, [
            "member acme bob admin", "remove acme bob", "team acme core write", "deleteTeam acme core",
            "member acme carol member", "team acme ops admin", "link web alice", "unlink app",
        ])

        client.failMutations = OrgApiException("forbidden", "Only owners can do that.")
        await model.unlink(client.repos[0])
        XCTAssertEqual(model.actionError, "Only owners can do that.")
        let deleted = await model.deleteOrg()
        XCTAssertFalse(deleted)
        client.failMutations = nil
        let gone = await model.deleteOrg()
        XCTAssertTrue(gone)
    }

    @MainActor
    func testDetailFailureShowsTheError() async {
        let client = FakeOrgClient()
        client.failure = Offline()
        let model = OrgDetailModel(client: client, orgName: "acme", session: nil)
        await model.load()
        XCTAssertNil(model.profile)
        XCTAssertEqual(model.error, OrgCopy.genericError)
        XCTAssertEqual(model.title, "acme")
    }

    @MainActor
    func testTeamRosterOffersOnlyOrgMembersNotOnTheTeam() async {
        let client = FakeOrgClient()
        client.teamMembers = [OrgMember(name: "bob")]
        let model = TeamDetailModel(
            client: client, orgName: "acme", team: OrgTeam(team: "core", permission: "write"),
            orgMembers: [OrgMember(name: "alice", role: "owner"), OrgMember(name: "bob")], canManage: true)
        await model.load()
        XCTAssertEqual(model.candidates.map(\.name), ["alice"])
        XCTAssertEqual(model.subtitle, "write permission")
        await model.add("alice")
        await model.remove(OrgMember(name: "bob"))
        XCTAssertEqual(client.calls, ["addToTeam core alice", "removeFromTeam core bob"])
    }
}
