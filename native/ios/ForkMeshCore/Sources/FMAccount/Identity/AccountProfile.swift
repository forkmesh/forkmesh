import FMBase
import Foundation
import Observation

public struct AccountProfile: Equatable, Sendable {
    public let name: String
    public let exists: Bool
    public let isPrivate: Bool
    public let kind: String
    public let status: String
    public let online: Bool
    public let owner: String
    public let nodes: [String]
    public let pubkey: String
    public let createdAt: Int64
    public let isAdmin: Bool
    public let bio: String
    public let location: String
    public let followers: Int
    public let following: Int
    public let mirrorCount: Int

    public static func parse(_ map: JSONObject, requested: String) -> AccountProfile {
        let nodes = map.strings("nodes").filter { NodeLink.isNodeName($0.lowercased()) }
        return AccountProfile(
            name: map.str("name").isEmpty ? requested : map.str("name"),
            exists: map["exists"] != .bool(false),
            isPrivate: map.bool("profilePrivate") && map["kind"] == nil,
            kind: map.str("kind"),
            status: map.str("status"),
            online: map.bool("online"),
            owner: map.str("owner"),
            nodes: Array(nodes.prefix(200)),
            pubkey: map.str("pubkey"),
            createdAt: map.int64("createdAt"),
            isAdmin: map.bool("isAdmin"),
            bio: String(map.str("profileBio").prefix(500)),
            location: String(map.str("profileLocation").prefix(120)),
            followers: max(0, map.int("followers")),
            following: max(0, map.int("following")),
            mirrorCount: max(0, map.int("mirrorCount")))
    }

    public var isNode: Bool { kind == "node" }

    public var kindLabel: String {
        if isNode { return online ? "Node · online" : "Node · offline" }
        return isAdmin ? "User · admin" : "User"
    }

    public var accountStatus: String {
        switch status {
        case "active": return "Active"
        case "pending": return "Pending email verification"
        case "": return "Unknown"
        default: return status.prefix(1).uppercased() + status.dropFirst()
        }
    }

    public var linkedNodesLine: String {
        if isNode { return owner.isEmpty ? "Not linked to a user" : "Linked to @\(owner)" }
        if nodes.isEmpty { return "No nodes linked" }
        return "\(nodes.count) node\(nodes.count == 1 ? "" : "s") linked"
    }

    public func canLinkNode(viewer: String) -> Bool {
        !isNode && !viewer.isEmpty && viewer.lowercased() == name.lowercased()
    }

    public var identityQRText: String { pubkey.isEmpty ? "" : "forkmesh:identity/\(name.lowercased())/\(pubkey)" }
}

@MainActor
@Observable
public final class AccountProfileModel {
    public let name: String
    public private(set) var profile: AccountProfile?
    public private(set) var loading = false
    public private(set) var error = ""

    @ObservationIgnored private let client: AccountEndpointCalling

    public init(name: String, client: AccountEndpointCalling) {
        self.name = name.trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "@"))
        self.client = client
    }

    public var validName: Bool { NodeLink.isNodeName(name.lowercased()) }

    public func load() async {
        guard validName else {
            error = "That is not a ForkMesh name."
            return
        }
        loading = true
        defer { loading = false }
        do {
            profile = AccountProfile.parse(
                try await client.accountCall("GET", "/api/accounts/\(name.lowercased())", nil), requested: name)
            error = ""
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not load @\(name).")
        }
    }
}
