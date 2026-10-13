import Foundation
import FMBase

/// The org roles the Worker recognises, most privileged first. Mirrors
/// `ORG_ROLES` in the Worker (organization_rbac.py): owner > admin >
/// maintainer > member > auditor > guest. The Worker's authorization matrix
/// decides what each role may do; the app only offers the vocabulary.
public let orgRoles = ["owner", "admin", "maintainer", "member", "auditor", "guest"]

/// The repository permissions a team can hold, least privileged first. Mirrors
/// `TEAM_PERMISSIONS` in the Worker.
public let orgTeamPermissions = ["read", "write", "maintain", "admin"]

public struct OrgSummary: Equatable, Hashable, Sendable {
    public var name: String
    public var role: String

    public init(name: String, role: String = "member") {
        self.name = name
        self.role = role
    }

    public init(json: JSONObject) {
        self.init(
            name: ModelJSON.firstString(json, ["name", "org"]),
            role: ModelJSON.string(json["role"]).dartTrimmed.lowercased()
        )
    }

    public var canManage: Bool { role == "owner" || role == "admin" }
}

public struct OrgProfile: Equatable, Hashable, Sendable {
    public var name: String
    public var displayName: String
    public var description: String
    public var createdMs: Int
    public var members: Int
    public var teams: Int
    public var repos: [OrgRepo]
    public var viewerRole: String

    public init(
        name: String, displayName: String = "", description: String = "", createdMs: Int = 0, members: Int = 0,
        teams: Int = 0, repos: [OrgRepo] = [], viewerRole: String = ""
    ) {
        self.name = name
        self.displayName = displayName
        self.description = description
        self.createdMs = createdMs
        self.members = members
        self.teams = teams
        self.repos = repos
        self.viewerRole = viewerRole
    }

    public init(json: JSONObject) {
        self.init(
            name: ModelJSON.firstString(json, ["org", "name"]),
            displayName: ModelJSON.string(json["displayName"]),
            description: ModelJSON.string(json["description"]),
            createdMs: ModelJSON.int(json["createdAt"]),
            members: ModelJSON.int(json["members"]),
            teams: ModelJSON.int(json["teams"]),
            repos: json.maps("repos").map(OrgRepo.init(json:)).filter { !$0.repo.isEmpty },
            viewerRole: ModelJSON.string(json["viewerRole"]).dartTrimmed.lowercased()
        )
    }

    public var title: String { displayName.isEmpty ? name : displayName }
    public var canManage: Bool { viewerRole == "owner" || viewerRole == "admin" }
    public var isOwner: Bool { viewerRole == "owner" }
}

public struct OrgMember: Equatable, Hashable, Sendable {
    public var name: String
    public var role: String
    public var sinceMs: Int

    public init(name: String, role: String = "member", sinceMs: Int = 0) {
        self.name = name
        self.role = role
        self.sinceMs = sinceMs
    }

    public init(json: JSONObject) {
        self.init(
            name: ModelJSON.string(json["name"]).dartTrimmed.lowercased(),
            role: ModelJSON.string(json["role"]).dartTrimmed.lowercased(),
            sinceMs: ModelJSON.int(json["since"])
        )
    }
}

public struct OrgTeam: Equatable, Hashable, Sendable {
    public var team: String
    public var permission: String
    public var members: Int

    public init(team: String, permission: String = "read", members: Int = 0) {
        self.team = team
        self.permission = permission
        self.members = members
    }

    public init(json: JSONObject) {
        self.init(
            team: ModelJSON.string(json["team"]).dartTrimmed.lowercased(),
            permission: ModelJSON.string(json["permission"]).dartTrimmed.lowercased(),
            members: ModelJSON.int(json["members"])
        )
    }
}

public struct OrgRepo: Equatable, Hashable, Sendable {
    public var repo: String
    public var node: String

    public init(repo: String, node: String = "") {
        self.repo = repo
        self.node = node
    }

    /// `node_owner` is the wire name used inside the org profile repo list.
    public init(json: JSONObject) {
        self.init(
            repo: ModelJSON.string(json["repo"]).dartTrimmed.lowercased(),
            node: ModelJSON.firstString(json, ["node", "node_owner"])
        )
    }
}

/// An org endpoint's refusal: the Worker's error code and `orgErrorMessage(code)`.
public struct OrgApiException: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String
    public let message: String

    public init(_ code: String, _ message: String) {
        self.code = code
        self.message = message
    }

    public var description: String { message }
}

public func orgErrorMessage(_ code: String) -> String {
    switch code {
    case "invalid_org_name": return "Invalid name. Use lowercase letters, numbers and dashes."
    case "invalid_team_name": return "Invalid team name. Use lowercase letters, numbers and dashes."
    case "org_name_taken": return "That name is already taken."
    case "too_many_orgs": return "You have reached the organization limit for this account."
    case "too_many_members": return "This organization has reached its member limit."
    case "too_many_teams": return "This organization has reached its team limit."
    case "too_many_repos": return "This organization has reached its linked-repo limit."
    case "invalid_session": return "Sign in to manage organizations."
    case "forbidden": return "You do not have permission to do that."
    case "not_found": return "Not found."
    case "last_owner": return "An organization must keep at least one owner."
    case "unknown_account": return "No account exists with that name."
    case "not_a_member": return "That account is not a member of this organization."
    case "bad_role": return "Invalid role."
    case "bad_permission": return "Invalid permission."
    case "member_required": return "Enter a member account name."
    case "repo_required": return "Enter a repository name."
    case "not_your_node": return "You can only link repositories from your own node."
    case "unknown_repo": return "That repository is not published on your node."
    case "": return "Request failed."
    default: return "Request failed (\(code))."
    }
}
