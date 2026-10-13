import FMBase
import Foundation

public struct RecentAccount: Equatable, Sendable, Identifiable {
    public let name: String
    public let email: String
    public let lastUsedAt: Int64

    public init(name: String, email: String, lastUsedAt: Int64) {
        self.name = name
        self.email = email
        self.lastUsedAt = lastUsedAt
    }

    public var id: String { name.lowercased() }

    public var subtitle: String { email.isEmpty ? "@\(name)" : email }

    func json() -> JSONValue {
        ["name": .string(name), "email": .string(email), "lastUsedAt": .int(lastUsedAt)]
    }

    static func parse(_ map: JSONObject) -> RecentAccount? {
        let name = map.str("name").trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty, name.count <= 63 else { return nil }
        return RecentAccount(name: name, email: String(map.str("email").prefix(254)), lastUsedAt: map.int64("lastUsedAt"))
    }
}

public final class RecentAccountsStore: @unchecked Sendable {
    public static let key = "account/recentAccounts"
    public static let limit = 8

    private let prefs: KeyValueStore

    public init(prefs: KeyValueStore) {
        self.prefs = prefs
    }

    public var accounts: [RecentAccount] {
        let raw = prefs.string(forKey: RecentAccountsStore.key) ?? ""
        guard !raw.isEmpty, let list = (try? JSON.parse(raw))?.arrayValue else { return [] }
        var seen = Set<String>()
        return list.compactMap { $0.objectValue.flatMap(RecentAccount.parse) }
            .filter { seen.insert($0.id).inserted }
            .sorted { $0.lastUsedAt > $1.lastUsedAt }
            .prefix(RecentAccountsStore.limit)
            .map { $0 }
    }

    public func others(than current: String) -> [RecentAccount] {
        accounts.filter { $0.id != current.lowercased() }
    }

    public func record(name: String, email: String, at millis: Int64) {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed != "preview-node" else { return }
        var list = accounts.filter { $0.id != trimmed.lowercased() }
        list.insert(RecentAccount(name: trimmed, email: email, lastUsedAt: millis), at: 0)
        save(Array(list.prefix(RecentAccountsStore.limit)))
    }

    public func forget(_ name: String) {
        save(accounts.filter { $0.id != name.lowercased() })
    }

    private func save(_ list: [RecentAccount]) {
        if list.isEmpty {
            prefs.remove(RecentAccountsStore.key)
        } else {
            prefs.set(JSON.encode(.array(list.map { $0.json() })), forKey: RecentAccountsStore.key)
        }
    }
}

public final class DesktopNicknames: @unchecked Sendable {
    public static let key = "account/desktopNicknames"
    public static let maxLength = 40
    public static let limit = 32

    private let prefs: KeyValueStore

    public init(prefs: KeyValueStore) {
        self.prefs = prefs
    }

    public var all: [String: String] {
        let map = JSON.parseObject(prefs.string(forKey: DesktopNicknames.key) ?? "")
        var out: [String: String] = [:]
        for (key, value) in map {
            let text = value.text
            if !key.isEmpty, !text.isEmpty { out[key] = text }
        }
        return out
    }

    public func name(for key: String, fallback: String) -> String {
        if let nickname = all[key], !nickname.isEmpty { return nickname }
        if !fallback.isEmpty { return fallback }
        return key.count <= 12 ? key : "Desktop \(key.prefix(6))"
    }

    public func rename(_ key: String, to nickname: String) {
        guard !key.isEmpty else { return }
        var map = all
        let cleaned = String(nickname.trimmingCharacters(in: .whitespacesAndNewlines).filter { !$0.isNewline }
            .prefix(DesktopNicknames.maxLength))
        if cleaned.isEmpty {
            map.removeValue(forKey: key)
        } else {
            map[key] = cleaned
        }
        save(map)
    }

    public func forget(_ key: String) {
        var map = all
        map.removeValue(forKey: key)
        save(map)
    }

    public func prune(keeping keys: Set<String>) {
        let map = all.filter { keys.contains($0.key) }
        if map.count != all.count { save(map) }
    }

    private func save(_ map: [String: String]) {
        let trimmed = map.sorted { $0.key < $1.key }.prefix(DesktopNicknames.limit)
        var object = JSONObject()
        for (key, value) in trimmed { object[key] = .string(value) }
        if object.isEmpty {
            prefs.remove(DesktopNicknames.key)
        } else {
            prefs.set(JSON.encode(object), forKey: DesktopNicknames.key)
        }
    }
}

public struct TrustListSummary: Equatable, Sendable {
    public let kind: String
    public let issuer: String
    public let name: String
    public let communities: [String]
    public let roles: [String]
    public let version: Int
    public let entries: Int
    public let issuedAt: Int64
    public let expiresAt: Int64
    public let documentId: String

    public static func summarize(_ raw: String) -> Result<TrustListSummary, TrustListError> {
        guard raw.utf8.count <= 256 * 1024 else { return .failure(TrustListError("document_too_large")) }
        let document: JSONObject
        do {
            document = try TrustLists.verifyDocument(raw)
        } catch let error as TrustListError {
            return .failure(error)
        } catch {
            return .failure(TrustListError("invalid_document"))
        }
        let schema = document.str("schema")
        let kind: String
        switch schema {
        case TrustLists.listSchema: kind = "Trust list"
        case TrustLists.revocationSchema: kind = "Revocation"
        case TrustLists.appealSchema: kind = "Appeal"
        default: kind = schema
        }
        return .success(TrustListSummary(
            kind: kind, issuer: document.str("issuer"), name: document.str("name"),
            communities: document.strings("communities"), roles: document.strings("roles"),
            version: document.int("version", document.int("fromVersion")), entries: document.list("entries").count,
            issuedAt: document.int64("issuedAt"), expiresAt: document.int64("expiresAt"),
            documentId: TrustLists.documentId(raw)))
    }
}

public struct DesktopStatusSummary: Equatable, Sendable {
    public let name: String
    public let user: String
    public let version: String
    public let running: Int
    public let transport: String
    public let providers: [String]
    public let defaultProvider: String

    public static func parse(status: JSONObject, agents: JSONObject = [:]) -> DesktopStatusSummary {
        let providers = agents.maps("providers").compactMap { (map: JSONObject) -> String? in
            let label = map.str("label").isEmpty ? map.str("id") : map.str("label")
            return label.isEmpty ? nil : String(label.prefix(40))
        }
        let defaultId = agents.str("defaultProvider")
        let defaultLabel = agents.maps("providers").first { $0.str("id") == defaultId }?.str("label") ?? defaultId
        return DesktopStatusSummary(
            name: String(status.str("name").prefix(63)), user: String(status.str("user").prefix(63)),
            version: String(status.str("version").prefix(32)), running: max(0, status.int("running")),
            transport: status.str("transport"), providers: Array(providers.prefix(16)),
            defaultProvider: String(defaultLabel.prefix(40)))
    }

    public var line: String {
        var parts: [String] = []
        if !version.isEmpty { parts.append("ForkMesh \(version)") }
        parts.append(running == 1 ? "1 agent running" : "\(running) agents running")
        if transport == "relay" { parts.append("through the relay") } else if transport == "lan" { parts.append("on this network") }
        return parts.joined(separator: " · ")
    }

    public var agentsLine: String {
        if providers.isEmpty { return "No agent providers set up on this desktop." }
        let list = providers.joined(separator: ", ")
        return defaultProvider.isEmpty ? list : "\(list). New agents use \(defaultProvider)."
    }
}
