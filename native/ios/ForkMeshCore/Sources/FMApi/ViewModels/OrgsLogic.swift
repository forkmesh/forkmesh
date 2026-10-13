// The pure logic of the Organizations and organization detail screens: error
// text, role tones, the create/member/team/repo form rules, the row menus,
// the team picker and labels. Follows native/android core/account/OrgsLogic.kt.

import FMAccount
import FMBase
import Foundation

public enum OrgCopy {
    public static let genericError = "Something went wrong. Check your connection and try again."
    public static let signInTitle = "Sign in to manage organizations"
    public static let signInMessage =
        "Organizations are tied to your Worker account. Sign in, then create or manage orgs and the repos they serve."
    public static let emptyTitle = "No organizations yet"
    public static let emptyMessage =
        "Create one to serve your repos at /<org>/<repo> and share management with teams."
    public static let loadFailed = "Could not load organizations"
    public static let createHelper =
        "The name shares the /<name>/<repo> namespace with accounts, so it must be free. You become its first owner."
    public static let deleteBody =
        "This dissolves the organization, its members, teams, and repo links. The linked repos themselves are not "
        + "deleted. This cannot be undone."
    public static let memberPromptTitle = "Add or update member"
    public static let memberPromptHelper = "Enter an existing account name. New members are notified."
    public static let teamPromptTitle = "New or updated team"
    public static let teamPromptHelper = "A team grants one permission over the org's linked repos."
    public static let repoPromptTitle = "Link a repo"
    public static let repoPromptHelper = "Link one of your own node's published repos to serve it at /<org>/<repo>."
}

/// `_messageFor`: human-readable text for an error thrown by an org call.
public func orgErrorText(_ error: Error?) -> String {
    if let org = error as? OrgApiException { return org.message }
    return OrgCopy.genericError
}

/// `OrgRoleBadge`'s colour: owner accent, admin amber, everyone else tertiary.
public enum OrgRoleTone: Equatable, Sendable {
    case accent, warning, tertiary
}

public func orgRoleTone(_ role: String) -> OrgRoleTone {
    switch role {
    case "owner": return .accent
    case "admin": return .warning
    default: return .tertiary
    }
}

public func orgRoleLabel(_ role: String) -> String { role.isEmpty ? "member" : role }

/// Organizations belong to a Worker account: the list and the create action
/// need a real session.
public func canUseOrgs(_ session: AuthSession?) -> Bool { hasRealSession(session) }

/// The orgs list's swap key (`FutureBuilder` state).
public func orgsListStateKey(loading: Bool, failed: Bool, orgs: [OrgSummary]) -> String {
    if loading { return "loading" }
    if failed { return "error" }
    if orgs.isEmpty { return "empty" }
    return "list-\(orgs.count)"
}

/// `_CreateOrgSheetState._submit`: the lower-cased name, or the error to show.
public enum NewOrgName: Equatable, Sendable {
    case valid(String)
    case invalid(String)
}

public func newOrgName(_ raw: String) -> NewOrgName {
    let name = raw.dartTrimmed.lowercased()
    return name.isEmpty ? .invalid("Enter an organization name.") : .valid(name)
}

public struct OrgMemberInput: Equatable, Sendable {
    public let name: String
    public let role: String
    public init(_ name: String, _ role: String) {
        self.name = name
        self.role = role
    }
}

/// The member sheet's Save: nothing happens for a blank name.
public func orgMemberInput(_ name: String, _ role: String) -> OrgMemberInput? {
    let clean = name.dartTrimmed.lowercased()
    return clean.isEmpty ? nil : OrgMemberInput(clean, role)
}

public struct OrgTeamInput: Equatable, Sendable {
    public let name: String
    public let permission: String
    public init(_ name: String, _ permission: String) {
        self.name = name
        self.permission = permission
    }
}

public func orgTeamInput(_ name: String, _ permission: String) -> OrgTeamInput? {
    let clean = name.dartTrimmed.lowercased()
    return clean.isEmpty ? nil : OrgTeamInput(clean, permission)
}

public struct OrgRepoInput: Equatable, Sendable {
    public let repo: String
    public let node: String
    public init(_ repo: String, _ node: String) {
        self.repo = repo
        self.node = node
    }
}

public func orgRepoInput(_ repo: String, _ node: String) -> OrgRepoInput? {
    let clean = repo.dartTrimmed.lowercased()
    return clean.isEmpty ? nil : OrgRepoInput(clean, node.dartTrimmed)
}

/// The link sheet's node field starts with the signed-in account.
public func defaultRepoNode(_ session: AuthSession?) -> String {
    (session?.nodeName ?? "").dartTrimmed.lowercased()
}

/// One entry of a member or team menu.
public struct OrgMenuItem: Equatable, Sendable, Identifiable {
    public let value: String
    public let label: String
    public let enabled: Bool
    public var id: String { value }

    public init(_ value: String, _ label: String, enabled: Bool = true) {
        self.value = value
        self.label = label
        self.enabled = enabled
    }
}

/// "Set role: …" for every role (the current one disabled); then, after a
/// divider, `memberMenuRemove`.
public func memberMenuRoles(_ member: OrgMember) -> [OrgMenuItem] {
    orgRoles.map { OrgMenuItem($0, "Set role: \($0)", enabled: $0 != member.role) }
}

public let memberMenuRemove = OrgMenuItem("remove", "Remove from org")

/// "Permission: …" for every permission (the current one disabled); then,
/// after a divider, `teamMenuDelete`.
public func teamMenuPermissions(_ team: OrgTeam) -> [OrgMenuItem] {
    orgTeamPermissions.map { OrgMenuItem("perm:\($0)", "Permission: \($0)", enabled: $0 != team.permission) }
}

public let teamMenuDelete = OrgMenuItem("delete", "Delete team")

public enum OrgMenuAction: Equatable, Sendable {
    case removeMember
    case setRole(String)
    case deleteTeam
    case setPermission(String)
}

public func memberMenuAction(_ value: String) -> OrgMenuAction {
    value == "remove" ? .removeMember : .setRole(value)
}

/// Nil for a value the team menu does not know.
public func teamMenuAction(_ value: String) -> OrgMenuAction? {
    if value == "delete" { return .deleteTeam }
    if value.hasPrefix("perm:") { return .setPermission(String(value.dropFirst(5))) }
    return nil
}

/// The team roster's add picker: org members not already on the team (the
/// Worker rejects non-members, so only they are offered).
public func teamAddCandidates(_ orgMembers: [OrgMember], _ teamMembers: [OrgMember]) -> [OrgMember] {
    let onTeam = Set(teamMembers.map(\.name))
    return orgMembers.filter { !onTeam.contains($0.name) }
}

public let teamAlreadyFull = "Every org member is already on this team."

public func teamMemberCountLabel(_ members: Int) -> String { "\(members) member\(members == 1 ? "" : "s")" }

public func teamSubtitle(_ team: OrgTeam) -> String { "\(team.permission) permission" }

public func teamEmptyMessage(_ error: String, _ team: OrgTeam) -> String {
    error.isEmpty ? "Add org members to grant them the \(team.permission) permission over linked repos." : error
}

public func linkedRepoTitle(_ orgName: String, _ repo: OrgRepo) -> String { "\(orgName)/\(repo.repo)" }

public func linkedRepoServes(_ repo: OrgRepo) -> String? {
    repo.node.isEmpty ? nil : "serves \(repo.node)/\(repo.repo)"
}

public struct OrgStat: Equatable, Sendable {
    public let label: String
    public let value: Int
}

/// The overview card's three numbers.
public func orgStats(_ profile: OrgProfile) -> [OrgStat] {
    [
        OrgStat(label: "Members", value: profile.members),
        OrgStat(label: "Teams", value: profile.teams),
        OrgStat(label: "Repos", value: profile.repos.count),
    ]
}

public func orgDeleteTitle(_ orgName: String) -> String { "Delete \(orgName)?" }
