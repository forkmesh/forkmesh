// View models of the Organizations screen, its create sheet, the organization
// detail screen and the team detail screen. They talk to the Worker's
// /api/orgs collection through `OrgClient`, which ApiService implements.

import FMAccount
import FMBase
import Foundation
import Observation

/// The `/api/orgs` calls of ApiService. Errors are
/// `OrgApiException` for a Worker refusal; anything else is a network failure.
@MainActor
public protocol OrgClient: AnyObject {
    func myOrgs() async throws -> [OrgSummary]
    func createOrg(name: String, displayName: String, description: String) async throws -> OrgSummary
    func orgProfile(_ org: String) async throws -> OrgProfile
    func deleteOrg(_ org: String) async throws
    func orgMembers(_ org: String) async throws -> [OrgMember]
    func setOrgMember(_ org: String, _ member: String, role: String) async throws
    func removeOrgMember(_ org: String, _ member: String) async throws
    func orgTeams(_ org: String) async throws -> [OrgTeam]
    func setOrgTeam(_ org: String, _ team: String, permission: String) async throws
    func deleteOrgTeam(_ org: String, _ team: String) async throws
    func orgTeamMembers(_ org: String, _ team: String) async throws -> [OrgMember]
    func addOrgTeamMember(_ org: String, _ team: String, _ member: String) async throws
    func removeOrgTeamMember(_ org: String, _ team: String, _ member: String) async throws
    func orgRepos(_ org: String) async throws -> [OrgRepo]
    func linkOrgRepo(_ org: String, _ repo: String, node: String) async throws
    func unlinkOrgRepo(_ org: String, _ repo: String) async throws
}

extension ApiService: OrgClient {}

/// `_OrgsScreenState`: the account's organizations.
@MainActor
@Observable
public final class OrgsListModel {
    public private(set) var orgs: [OrgSummary] = []
    public private(set) var loading = false
    /// The load error's text, or nil.
    public private(set) var failure: String?

    public let session: AuthSession?
    @ObservationIgnored private let client: OrgClient?
    @ObservationIgnored private var generation = 0

    public init(client: OrgClient?, session: AuthSession?) {
        self.client = client
        self.session = session
    }

    public var signedIn: Bool { canUseOrgs(session) && client != nil }
    public var stateKey: String { orgsListStateKey(loading: loading, failed: failure != nil, orgs: orgs) }

    /// `_reload`. The newest reload wins, as a fresh Future does.
    public func reload() async {
        guard signedIn, let client else { return }
        generation += 1
        let mine = generation
        loading = true
        failure = nil
        do {
            let fresh = try await client.myOrgs()
            guard mine == generation else { return }
            orgs = fresh
            loading = false
        } catch {
            guard mine == generation else { return }
            failure = orgErrorText(error)
            loading = false
        }
    }
}

/// `_CreateOrgSheetState`.
@MainActor
@Observable
public final class CreateOrgModel {
    public var name = ""
    public var displayName = ""
    public var description = ""
    public private(set) var busy = false
    public private(set) var error = ""

    @ObservationIgnored private let client: OrgClient

    public init(client: OrgClient) {
        self.client = client
    }

    /// The created org when the sheet should close with it.
    public func submit() async -> OrgSummary? {
        switch newOrgName(name) {
        case .invalid(let message):
            error = message
            return nil
        case .valid(let clean):
            busy = true
            error = ""
            do {
                let created = try await client.createOrg(name: clean, displayName: displayName, description: description)
                return created
            } catch {
                busy = false
                self.error = orgErrorText(error)
                return nil
            }
        }
    }
}

/// `_OrgDetailScreenState`: profile, members, teams and linked repos.
@MainActor
@Observable
public final class OrgDetailModel {
    public let orgName: String
    public private(set) var loading = true
    public private(set) var error = ""
    public private(set) var profile: OrgProfile?
    public private(set) var members: [OrgMember] = []
    public private(set) var teams: [OrgTeam] = []
    public private(set) var repos: [OrgRepo] = []
    /// A refused action's text, shown inline in red until the next action clears it.
    public var actionError: String?

    @ObservationIgnored private let client: OrgClient
    @ObservationIgnored private let session: AuthSession?

    public init(client: OrgClient, orgName: String, session: AuthSession?) {
        self.client = client
        self.orgName = orgName
        self.session = session
    }

    public var canManage: Bool { profile?.canManage == true }
    public var isOwner: Bool { profile?.isOwner == true }
    public var title: String { profile?.title ?? orgName }
    public var subtitle: String { "/\(orgName)" }
    public var defaultNode: String { defaultRepoNode(session) }

    /// `_load`: all four reads together; any failure fails the page.
    public func load() async {
        loading = true
        error = ""
        let api = client
        let org = orgName
        do {
            async let profileRead = api.orgProfile(org)
            async let membersRead = api.orgMembers(org)
            async let teamsRead = api.orgTeams(org)
            async let reposRead = api.orgRepos(org)
            let (p, m, t, r) = try await (profileRead, membersRead, teamsRead, reposRead)
            profile = p
            members = m
            teams = t
            repos = r
            loading = false
        } catch {
            loading = false
            self.error = orgErrorText(error)
        }
    }

    /// `_run`: an action, then a reload; a refusal becomes `actionError`.
    public func run(_ action: @escaping (OrgClient) async throws -> Void) async {
        actionError = nil
        do {
            try await action(client)
            await load()
        } catch {
            actionError = orgErrorText(error)
        }
    }

    public func memberMenu(_ member: OrgMember, choose value: String) async {
        let org = orgName
        switch memberMenuAction(value) {
        case .removeMember:
            await run { try await $0.removeOrgMember(org, member.name) }
        case .setRole(let role):
            await run { try await $0.setOrgMember(org, member.name, role: role) }
        default:
            return
        }
    }

    public func teamMenu(_ team: OrgTeam, choose value: String) async {
        let org = orgName
        switch teamMenuAction(value) {
        case .some(.deleteTeam):
            await run { try await $0.deleteOrgTeam(org, team.team) }
        case .some(.setPermission(let permission)):
            await run { try await $0.setOrgTeam(org, team.team, permission: permission) }
        default:
            return
        }
    }

    public func addMember(_ input: OrgMemberInput) async {
        let org = orgName
        await run { try await $0.setOrgMember(org, input.name, role: input.role) }
    }

    public func addTeam(_ input: OrgTeamInput) async {
        let org = orgName
        await run { try await $0.setOrgTeam(org, input.name, permission: input.permission) }
    }

    public func linkRepo(_ input: OrgRepoInput) async {
        let org = orgName
        await run { try await $0.linkOrgRepo(org, input.repo, node: input.node) }
    }

    public func unlink(_ repo: OrgRepo) async {
        let org = orgName
        await run { try await $0.unlinkOrgRepo(org, repo.repo) }
    }

    /// `_confirmDelete` after the confirmation: true when the screen should close.
    public func deleteOrg() async -> Bool {
        actionError = nil
        do {
            try await client.deleteOrg(orgName)
            return true
        } catch {
            actionError = orgErrorText(error)
            return false
        }
    }
}

/// `_TeamDetailScreenState`: one team's roster.
@MainActor
@Observable
public final class TeamDetailModel {
    public let orgName: String
    public let team: OrgTeam
    public let orgMembers: [OrgMember]
    public let canManage: Bool
    public private(set) var loading = true
    public private(set) var error = ""
    public private(set) var members: [OrgMember] = []
    public var actionError: String?

    @ObservationIgnored private let client: OrgClient

    public init(client: OrgClient, orgName: String, team: OrgTeam, orgMembers: [OrgMember], canManage: Bool) {
        self.client = client
        self.orgName = orgName
        self.team = team
        self.orgMembers = orgMembers
        self.canManage = canManage
    }

    public var subtitle: String { teamSubtitle(team) }
    public var emptyMessage: String { teamEmptyMessage(error, team) }
    public var candidates: [OrgMember] { teamAddCandidates(orgMembers, members) }

    public func load() async {
        loading = true
        error = ""
        do {
            members = try await client.orgTeamMembers(orgName, team.team)
            loading = false
        } catch {
            loading = false
            self.error = orgErrorText(error)
        }
    }

    private func run(_ action: () async throws -> Void) async {
        actionError = nil
        do {
            try await action()
            await load()
        } catch {
            actionError = orgErrorText(error)
        }
    }

    public func add(_ member: String) async {
        await run { try await client.addOrgTeamMember(orgName, team.team, member) }
    }

    public func remove(_ member: OrgMember) async {
        await run { try await client.removeOrgTeamMember(orgName, team.team, member.name) }
    }
}
