import FMBase
import FMCrypto
import Foundation

public struct VaultAutofillIdentity: Equatable, Hashable, Sendable {
    public let recordIdentifier: String
    public let host: String
    public let user: String

    public init(recordIdentifier: String, host: String, user: String) {
        self.recordIdentifier = recordIdentifier
        self.host = host
        self.user = user
    }
}

public protocol VaultAutofillKeyStore: AnyObject {
    func readKey() throws -> [UInt8]?
    func writeKey(_ key: [UInt8]) throws
    func deleteKey() throws
}

public protocol VaultAutofillBlobStore: AnyObject {
    func readBlob() throws -> Data?
    func writeBlob(_ data: Data) throws
    func deleteBlob() throws
}

public protocol VaultAutofillLinkStore: AnyObject {
    func readLinks() -> VaultAutofillLinks
    func writeLinks(_ links: VaultAutofillLinks)
}

public struct VaultAutofillLinks: Equatable, Sendable {
    public static let maxHosts = 500
    public static let maxPerHost = 10

    public private(set) var hosts: [String: [String]]

    public init(hosts: [String: [String]] = [:]) {
        self.hosts = hosts
    }

    public init(data: Data) {
        var hosts: [String: [String]] = [:]
        for (host, value) in JSON.parseObject(data) {
            let ids = (value.arrayValue ?? []).compactMap(\.stringValue).filter { !$0.isEmpty }
            if !host.isEmpty && !ids.isEmpty { hosts[host] = Array(ids.prefix(Self.maxPerHost)) }
        }
        self.hosts = hosts
    }

    public var data: Data {
        var object = JSONObject()
        for (host, ids) in hosts { object[host] = .array(ids.map { .string($0) }) }
        return Data(JSON.encode(.object(object)).utf8)
    }

    public var isEmpty: Bool { hosts.isEmpty }

    public func ids(for serviceIdentifiers: [String]) -> Set<String> {
        var out = Set<String>()
        for identifier in serviceIdentifiers {
            let host = VaultAutofillCache.bare(VaultAutofillCache.host(of: identifier))
            if !host.isEmpty { out.formUnion(hosts[host] ?? []) }
        }
        return out
    }

    @discardableResult
    public mutating func link(_ login: VaultAutofillLogin, serviceIdentifiers: [String]) -> [String] {
        var added: [String] = []
        let own = VaultAutofillCache.bare(login.host.lowercased())
        for identifier in serviceIdentifiers {
            let host = VaultAutofillCache.bare(VaultAutofillCache.host(of: identifier))
            if host.isEmpty || host == own { continue }
            if hosts[host] == nil && hosts.count >= Self.maxHosts { continue }
            var ids = hosts[host] ?? []
            if ids.first == login.id { continue }
            ids.removeAll { $0 == login.id }
            ids.insert(login.id, at: 0)
            hosts[host] = Array(ids.prefix(Self.maxPerHost))
            added.append(host)
        }
        return added
    }

    public mutating func keep(_ ids: Set<String>) {
        var kept: [String: [String]] = [:]
        for (host, linked) in hosts {
            let remaining = linked.filter(ids.contains)
            if !remaining.isEmpty { kept[host] = remaining }
        }
        hosts = kept
    }
}

public enum VaultAutofillCacheError: Error, Equatable {
    case noCache
    case noKey
    case unreadable
}

public enum VaultAutofillCache {
    public static let version = 1
    public static let maxLogins = 2000
    public static let aad = Array("forkmesh-vault-autofill-v1".utf8)

    public static func usable(_ logins: [VaultAutofillLogin]) -> [VaultAutofillLogin] {
        var seen = Set<String>()
        var out: [VaultAutofillLogin] = []
        for login in logins where !login.id.isEmpty && !login.password.isEmpty && !login.host.isEmpty {
            if seen.contains(login.id) { continue }
            seen.insert(login.id)
            out.append(login)
            if out.count >= maxLogins { break }
        }
        return out
    }

    public static func identities(_ logins: [VaultAutofillLogin], links: VaultAutofillLinks = VaultAutofillLinks()) -> [VaultAutofillIdentity] {
        let usable = usable(logins)
        var out = usable.map {
            VaultAutofillIdentity(recordIdentifier: $0.id, host: $0.host, user: $0.username.isEmpty ? $0.title : $0.username)
        }
        let byId = Dictionary(usable.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        for host in links.hosts.keys.sorted() {
            for id in links.hosts[host] ?? [] {
                guard let login = byId[id], bare(login.host.lowercased()) != host else { continue }
                out.append(VaultAutofillIdentity(recordIdentifier: id, host: host, user: login.username.isEmpty ? login.title : login.username))
            }
        }
        return out
    }

    public static func seal(account: String, logins: [VaultAutofillLogin], key: [UInt8], nonce: [UInt8]? = nil) throws -> Data {
        let rows: [JSONValue] = usable(logins).map { login in
            .object([
                "host": .string(login.host), "id": .string(login.id), "password": .string(login.password),
                "title": .string(login.title), "username": .string(login.username),
            ])
        }
        let plain = Array(JSON.encode(.object(["account": .string(account), "logins": .array(rows)])).utf8)
        let box = try AesGcm.encrypt(plain, key: key, nonce: nonce, aad: aad)
        let envelope: JSONObject = [
            "body": .string(B64.encode(box.cipherText)), "nonce": .string(B64.encode(box.nonce)),
            "tag": .string(B64.encode(box.mac)), "v": .int(Int64(version)),
        ]
        return Data(JSON.encode(.object(envelope)).utf8)
    }

    public static func open(_ data: Data, key: [UInt8]) throws -> (account: String, logins: [VaultAutofillLogin]) {
        let envelope = JSON.parseObject(data)
        guard envelope["v"]?.intValue == version,
            let nonce = B64.decodeOrNil(envelope["nonce"]?.stringValue ?? ""),
            let tag = B64.decodeOrNil(envelope["tag"]?.stringValue ?? ""),
            let body = B64.decodeOrNil(envelope["body"]?.stringValue ?? ""),
            let plain = AesGcm.decryptOrNull(cipherText: [UInt8](body), nonce: [UInt8](nonce), mac: [UInt8](tag), key: key, aad: aad)
        else { throw VaultAutofillCacheError.unreadable }
        let object = JSON.parseObject(Data(plain))
        let logins = (object["logins"]?.arrayValue ?? []).compactMap(\.objectValue).map {
            VaultAutofillLogin(
                id: $0["id"]?.stringValue ?? "", title: $0["title"]?.stringValue ?? "",
                username: $0["username"]?.stringValue ?? "", password: $0["password"]?.stringValue ?? "",
                host: $0["host"]?.stringValue ?? "")
        }
        return (object["account"]?.stringValue ?? "", usable(logins))
    }

    public static let appPrefix = "app:"

    public static func host(of identifier: String) -> String {
        let raw = identifier.trimmingCharacters(in: .whitespacesAndNewlines)
        if raw.hasPrefix(appPrefix) { return raw.count > appPrefix.count ? raw : "" }
        let trimmed = raw.lowercased()
        if trimmed.isEmpty { return "" }
        if trimmed.contains("://") { return VaultCrypto.loginHost(trimmed) }
        return VaultCrypto.loginHost("https://" + trimmed)
    }

    public static func matches(_ login: VaultAutofillLogin, serviceIdentifiers: [String]) -> Bool {
        let hosts = serviceIdentifiers.map(host(of:)).filter { !$0.isEmpty }
        if hosts.isEmpty { return false }
        let loginHost = bare(login.host.lowercased())
        return hosts.contains { bare($0) == loginHost }
    }

    static func bare(_ host: String) -> String { host.hasPrefix("www.") ? String(host.dropFirst(4)) : host }

    public static func ranked(
        _ logins: [VaultAutofillLogin], serviceIdentifiers: [String], query: String = "", links: VaultAutofillLinks = VaultAutofillLinks()
    ) -> (suggested: [VaultAutofillLogin], others: [VaultAutofillLogin]) {
        let linked = links.ids(for: serviceIdentifiers)
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let filtered = logins.filter { login in
            needle.isEmpty || login.title.lowercased().contains(needle) || login.username.lowercased().contains(needle)
                || login.host.lowercased().contains(needle)
        }
        let order: (VaultAutofillLogin, VaultAutofillLogin) -> Bool = {
            ($0.title.lowercased(), $0.username.lowercased()) < ($1.title.lowercased(), $1.username.lowercased())
        }
        let suggested = filtered.filter { linked.contains($0.id) || matches($0, serviceIdentifiers: serviceIdentifiers) }.sorted(by: order)
        let suggestedIds = Set(suggested.map(\.id))
        return (suggested, filtered.filter { !suggestedIds.contains($0.id) }.sorted(by: order))
    }
}

public final class VaultAutofillVault {
    private let keys: VaultAutofillKeyStore
    private let blobs: VaultAutofillBlobStore

    public init(keys: VaultAutofillKeyStore, blobs: VaultAutofillBlobStore) {
        self.keys = keys
        self.blobs = blobs
    }

    public func store(account: String, logins: [VaultAutofillLogin]) throws {
        var key = CryptoBytes.random(AesGcm.keyBytes)
        defer { CryptoBytes.wipe(&key) }
        let sealed = try VaultAutofillCache.seal(account: account, logins: logins, key: key)
        try blobs.deleteBlob()
        try keys.writeKey(key)
        try blobs.writeBlob(sealed)
    }

    public func load() throws -> (account: String, logins: [VaultAutofillLogin]) {
        guard let blob = try blobs.readBlob() else { throw VaultAutofillCacheError.noCache }
        guard var key = try keys.readKey(), key.count == AesGcm.keyBytes else { throw VaultAutofillCacheError.noKey }
        defer { CryptoBytes.wipe(&key) }
        return try VaultAutofillCache.open(blob, key: key)
    }

    public func clear() {
        try? blobs.deleteBlob()
        try? keys.deleteKey()
    }
}
