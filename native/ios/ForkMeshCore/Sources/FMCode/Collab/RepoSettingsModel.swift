import FMApi
import FMBase
import Foundation

public struct RepoAbout: Equatable, Sendable {
    public var description: String
    public var website: String
    public var logoUrl: String
    public var bannerUrl: String
    public var isPrivate: Bool
    public var federate: Bool
    public var broadcastEvents: Bool
    public var acceptComments: Bool
    public var fediverseHandle: String
    public var followers: Int

    public init(description: String = "", website: String = "", logoUrl: String = "", bannerUrl: String = "",
                isPrivate: Bool = false, federate: Bool = true, broadcastEvents: Bool = true, acceptComments: Bool = true,
                fediverseHandle: String = "", followers: Int = 0) {
        self.description = description
        self.website = website
        self.logoUrl = logoUrl
        self.bannerUrl = bannerUrl
        self.isPrivate = isPrivate
        self.federate = federate
        self.broadcastEvents = broadcastEvents
        self.acceptComments = acceptComments
        self.fediverseHandle = fediverseHandle
        self.followers = followers
    }

    public init(json j: JSONObject) {
        let fedi = j.obj("fediverse")
        let settings = fedi.obj("settings")
        self.init(
            description: j.str("description"), website: j.str("website"), logoUrl: j.str("logoUrl"),
            bannerUrl: j.str("bannerUrl"), isPrivate: j.bool("isPrivate"),
            federate: settings.optBool("federate") ?? true, broadcastEvents: settings.optBool("broadcastEvents") ?? true,
            acceptComments: settings.optBool("acceptComments") ?? true, fediverseHandle: fedi.str("handle"),
            followers: fedi.int("followers"))
    }
}

public struct RepoForkRow: Equatable, Sendable, Identifiable {
    public var id: String { "\(owner)/\(name)" }
    public let owner: String
    public let name: String

    public init?(json j: JSONObject) {
        let owner = j.firstStr("owner", "node")
        let name = j.firstStr("name", "repo")
        guard !owner.isEmpty, !name.isEmpty else { return nil }
        self.owner = owner
        self.name = name
    }
}

public struct RepoForkOutcome: Equatable, Sendable {
    public let created: Bool
    public let queued: Bool
    public let owner: String
    public let name: String

    public init(json j: JSONObject) {
        let fork = j.obj("fork")
        created = j.bool("created")
        queued = j.bool("queued")
        owner = fork.firstStr("owner", "node")
        name = fork.firstStr("name", "repo")
    }

    public var message: String {
        if created { return "Fork created\(owner.isEmpty ? "" : " as \(owner)/\(name)")." }
        if queued { return "Fork recorded. Your managed mirror creates it the next time it is online." }
        return "You already have a fork\(owner.isEmpty ? "" : ": \(owner)/\(name)")."
    }
}

public enum RepoSettingsSection: String, CaseIterable, Sendable {
    case general, agents, automation, mirror, coves, permissions, danger

    public var title: String {
        switch self {
        case .general: return "General"
        case .agents: return "Agents"
        case .automation: return "Automation"
        case .mirror: return "Mirror to GitHub or GitLab"
        case .coves: return "Coves"
        case .permissions: return "Permissions"
        case .danger: return "Danger zone"
        }
    }

    public var icon: String {
        switch self {
        case .general: return "gearshape"
        case .agents: return "cpu"
        case .automation: return "gearshape.2"
        case .mirror: return "arrow.left.arrow.right"
        case .coves: return "lock.shield"
        case .permissions: return "lock"
        case .danger: return "exclamationmark.triangle"
        }
    }

    public init(route: String?) { self = RepoSettingsSection(rawValue: route ?? "") ?? .general }
}

public enum RepoSettingsRules {
    public static let maxDescription = 240
    public static let maxWebsite = 240

    public static func validateWebsite(_ text: String) -> String? {
        let value = text.dartTrimmed
        if value.isEmpty { return nil }
        guard value.count <= maxWebsite, let url = URL(string: value), let scheme = url.scheme?.lowercased(),
              scheme == "https" || scheme == "http", url.host != nil else {
            return "Enter a full http(s) address."
        }
        return nil
    }

    public static func aboutPatch(from old: RepoAbout, to new: RepoAbout) -> JSONObject {
        var body: JSONObject = [:]
        if new.description != old.description { body["description"] = .string(String(new.description.prefix(maxDescription))) }
        if new.website.dartTrimmed != old.website.dartTrimmed { body["website"] = .string(new.website.dartTrimmed) }
        if new.federate != old.federate || new.broadcastEvents != old.broadcastEvents || new.acceptComments != old.acceptComments {
            body["fediverse"] = .object([
                "federate": .bool(new.federate), "broadcastEvents": .bool(new.broadcastEvents),
                "acceptComments": .bool(new.acceptComments),
            ])
        }
        return body
    }

    public static func canEdit(repoOwner: String, account: String?, organizationOwner: Bool) -> Bool {
        guard let account, !account.isEmpty else { return false }
        return organizationOwner || account.lowercased() == repoOwner.lowercased()
    }

    public static func confirmsName(_ typed: String, repo: String) -> Bool {
        !repo.isEmpty && typed.dartTrimmed == repo
    }
}

@MainActor
public final class RepoSettingsClient {
    let transport: CollabTransport
    let owner: String
    let name: String
    let sessionToken: () -> String

    public init(transport: CollabTransport, owner: String, name: String, sessionToken: @escaping () -> String) {
        self.transport = transport
        self.owner = owner
        self.name = name
        self.sessionToken = sessionToken
    }

    var base: String { "/api/repo/\(owner)/\(name)" }

    func authed(_ body: JSONObject) -> JSONObject {
        var out = body
        let token = sessionToken()
        if !token.isEmpty && token != "cookie" { out["sessionToken"] = .string(token) }
        return out
    }

    public func about() async throws -> RepoAbout {
        RepoAbout(json: try await CollabHTTP.json(transport, "GET", "\(base)/about"))
    }

    public func saveAbout(from old: RepoAbout, to new: RepoAbout) async throws -> Bool {
        let patch = RepoSettingsRules.aboutPatch(from: old, to: new)
        if patch.isEmpty { return false }
        _ = try await CollabHTTP.json(transport, "POST", "\(base)/about", body: authed(patch))
        return true
    }

    public func forks() async throws -> (count: Int, rows: [RepoForkRow]) {
        let reply = try await CollabHTTP.json(transport, "GET", "\(base)/forks")
        return (reply.int("count"), reply.maps("forks").prefix(200).compactMap(RepoForkRow.init(json:)))
    }

    public func fork(targetNode: String = "") async throws -> RepoForkOutcome {
        var body: JSONObject = [:]
        if !targetNode.dartTrimmed.isEmpty { body["targetNode"] = .string(targetNode.dartTrimmed.lowercased()) }
        return RepoForkOutcome(json: try await CollabHTTP.json(transport, "POST", "\(base)/forks", body: authed(body)))
    }

    public func sizes(ref: String = "", depth: Int = 0) async throws -> CodeMapNode {
        var query: [(String, String)] = []
        if !ref.isEmpty { query.append(("ref", ref)) }
        if depth > 0 { query.append(("depth", "\(depth)")) }
        let reply = try await CollabHTTP.json(transport, "GET", "\(base)/sizes", query: query)
        let root = reply["tree"]?.objectValue ?? reply["root"]?.objectValue ?? reply
        return CodeMapNode(json: root)
    }

    public func history(ref: String) async throws -> [JSONObject] {
        let reply = try await CollabHTTP.json(transport, "GET", "\(base)/history", query: ref.isEmpty ? [] : [("ref", ref)])
        return reply.maps("commits")
    }
}
