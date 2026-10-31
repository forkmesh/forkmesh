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
    public var timeZone: String = ""
    public var avatar: String = ""
    public var solana: String = ""
    public var links: [ProfileLink] = []
    public var mastodon: String = ""
    public var mastodonUrl: String = ""

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
            mirrorCount: max(0, map.int("mirrorCount")),
            timeZone: zone(map.str("profileTimezone")),
            avatar: map.str("avatarPng"),
            solana: map.str("solana").trimmingCharacters(in: .whitespaces),
            links: ProfileLink.parseAll(map.maps("profileLinks")),
            mastodon: String(map.str("mastodon").prefix(120)),
            mastodonUrl: ProfileLink.isWebUrl(map.str("mastodonUrl")) ? map.str("mastodonUrl") : "")
    }

    static func zone(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed.count <= 80, TimeZone(identifier: trimmed) != nil else { return "" }
        return trimmed
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

    public var tipAddress: String { SolanaAddress.valid(solana) }

    public var walletQRText: String { SolanaAddress.qrText(solana) }

    public var identityQRText: String { pubkey.isEmpty ? "" : "forkmesh:identity/\(name.lowercased())/\(pubkey)" }
}

public struct ProfileLink: Equatable, Sendable {
    public let label: String
    public let url: String
    public let verified: Bool

    public init(label: String, url: String, verified: Bool) {
        self.label = label
        self.url = url
        self.verified = verified
    }

    public static func isWebUrl(_ url: String) -> Bool {
        url.count <= 500 && (url.hasPrefix("https://") || url.hasPrefix("http://"))
    }

    public static func parseAll(_ items: [JSONObject]) -> [ProfileLink] {
        let links = items.compactMap { (item: JSONObject) -> ProfileLink? in
            let url = item.str("url").trimmingCharacters(in: .whitespaces)
            guard isWebUrl(url) else { return nil }
            var label = String(item.str("label").trimmingCharacters(in: .whitespaces).prefix(60))
            if label.isEmpty { label = item.str("domain").isEmpty ? url : item.str("domain") }
            return ProfileLink(label: label, url: url, verified: item.bool("verified"))
        }
        return Array(links.prefix(10))
    }
}

public enum SolanaAddress {
    private static let alphabet = Set("123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz")

    public static func isValid(_ address: String) -> Bool {
        (32...44).contains(address.count) && address.allSatisfy { alphabet.contains($0) }
    }

    public static func valid(_ address: String) -> String {
        let trimmed = address.trimmingCharacters(in: .whitespaces)
        return isValid(trimmed) ? trimmed : ""
    }

    public static func qrText(_ address: String) -> String {
        let trimmed = valid(address)
        return trimmed.isEmpty ? "" : "solana:\(trimmed)"
    }
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
