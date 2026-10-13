import FMBase
import Foundation

public struct CoveDocument: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var mime: String
    public var body: String
    public var updatedAtMs: Int64

    public init(id: String, name: String, mime: String = "text/markdown", body: String = "", updatedAtMs: Int64 = 0) {
        self.id = id
        self.name = name
        self.mime = mime
        self.body = body
        self.updatedAtMs = updatedAtMs
    }

    public init(json: JSONObject) {
        id = json["id"]?.stringValue ?? ""
        name = json["name"]?.stringValue ?? ""
        mime = json["mime"]?.stringValue ?? "text/markdown"
        body = json["body"]?.stringValue ?? ""
        updatedAtMs = json["updatedAtMs"].map { $0.int64(or: 0) } ?? 0
    }

    public var json: JSONObject {
        [
            "body": .string(body), "id": .string(id), "mime": .string(mime), "name": .string(name),
            "updatedAtMs": .int(updatedAtMs),
        ]
    }
}

public struct CoveAccessEntry: Equatable, Hashable, Sendable {
    public var who: String
    public var name: String
    public var node: String
    public var ts: Int64
    public var action: String

    public init(who: String = "", name: String = "", node: String = "", ts: Int64 = 0, action: String = "open") {
        self.who = who
        self.name = name
        self.node = node
        self.ts = ts
        self.action = action
    }

    public init(json: JSONObject) {
        who = json["who"]?.stringValue ?? ""
        name = json["name"]?.stringValue ?? ""
        node = json["node"]?.stringValue ?? ""
        ts = json["ts"].map { $0.int64(or: 0) } ?? 0
        action = json["action"]?.stringValue ?? "open"
    }

    public var json: JSONObject {
        ["action": .string(action), "name": .string(name), "node": .string(node), "ts": .int(ts), "who": .string(who)]
    }
}

public enum CoveAccessMode: String, Sendable {
    case password, account
}

public struct Cove: Equatable, Sendable, Identifiable {
    public var id: String = ""
    public var name: String = ""
    public var slug: String = ""
    public var relPath: String = ""
    public var version: Int = 1
    public var creator: String = ""
    public var accessMode: String = "password"
    public var creatorAccount: String = ""
    public var invitedAccounts: [String] = []
    public var createdAtMs: Int64 = 0
    public var notifyOnOpen = false
    public var salt: [UInt8] = []
    public var rounds: Int = 0
    public var cipher: CoveCipher?
    public var grants: [CoveCipher] = []

    public var unlocked = false
    public var contentKey: [UInt8] = []
    public var documents: [CoveDocument] = []
    public var accessLog: [CoveAccessEntry] = []

    public init() {}

    public var accountScoped: Bool { accessMode == CoveAccessMode.account.rawValue }

    public var members: [String] {
        var people = CoveStore.normalizedAccounts(invitedAccounts)
        let creator = CoveStore.normalizedAccount(creatorAccount)
        if !creator.isEmpty && !people.contains(creator) { people.append(creator) }
        return people.sorted { $0.lowercased() < $1.lowercased() }
    }

    public func createdBy(account: String) -> Bool {
        let me = CoveStore.normalizedAccount(account)
        return !me.isEmpty && CoveStore.normalizedAccount(creatorAccount) == me
    }

    public func document(named name: String) -> CoveDocument? {
        let wanted = CoveStore.cleanPath(name)
        return documents.first { CoveStore.cleanPath($0.name) == wanted }
    }

    public var displayName: String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty { return trimmed }
        return unlocked ? "Secure cove" : "Locked cove"
    }
}
