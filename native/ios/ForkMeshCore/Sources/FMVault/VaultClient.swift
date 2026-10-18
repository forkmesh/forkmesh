// Everything between /api/vault and a screen,
// the twin of vault-client.js and the desktop's VaultClient. It owns the unlocked state (keyset,
// item keys, decrypted items, pinned fingerprints) and has no HTTP of its own: the request
// function is handed in, so the same code runs under the app and under the test that drives it
// against the real Worker module (VaultClientFlowTests).
//
// Trust is pinned on first use. The first time a person's public keys are seen their fingerprint
// is remembered in an encrypted record only this account can open. If the server later presents
// different keys for the same name, nothing is sealed to them and nothing from them is opened
// until the owner confirms the new fingerprint out of band.
//
// Main-actor isolated. Argon2id and the opening of a
// listing run on detached tasks over copies of the keys, so `lock()` can wipe at any time.

import FMBase
import FMCrypto
import Foundation
import Observation

let vaultApi = "/api/vault"

/// Raised with the same codes the other clients use ("vault_locked", "recipient_keys_changed", a
/// server error code, ...).
public struct VaultError: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String

    public init(_ code: String) {
        self.code = code
    }

    public var description: String { code.replacingOccurrences(of: "_", with: " ") }
}

/// One call under /api/vault: `(method, path, body)` to the decoded JSON object of a 2xx answer.
/// Anything else must throw, `VaultError(<server error code>)` when the answer carried one
/// (VaultService maps the app's OrgApiException to it).
public typealias VaultRequest = @Sendable (_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject

public struct VaultShare: Equatable, Hashable, Sendable {
    /// "user" or "organization".
    public let kind: String
    public let name: String
    /// "read" or "write".
    public let permission: String

    public init(kind: String, name: String, permission: String) {
        self.kind = kind
        self.name = name
        self.permission = permission
    }
}

public struct VaultItem: Equatable, Sendable, Identifiable {
    public let id: String
    public let owner: String
    public let owned: Bool
    /// "owner", "read" or "write".
    public let permission: String
    public let revision: Int
    public let updatedAt: Int64
    public let updatedBy: String
    public let viaKind: String
    public let viaName: String
    public let shares: [VaultShare]
    /// Empty when the item could not be opened.
    public let fields: JSONObject
    /// The error code that kept it closed; "" when readable.
    public let error: String

    public init(
        id: String, owner: String, owned: Bool, permission: String, revision: Int, updatedAt: Int64,
        updatedBy: String, viaKind: String = "", viaName: String = "", shares: [VaultShare] = [],
        fields: JSONObject = JSONObject(), error: String = ""
    ) {
        self.id = id
        self.owner = owner
        self.owned = owned
        self.permission = permission
        self.revision = revision
        self.updatedAt = updatedAt
        self.updatedBy = updatedBy
        self.viaKind = viaKind
        self.viaName = viaName
        self.shares = shares
        self.fields = fields
        self.error = error
    }

    public var readable: Bool { error.isEmpty }
    public var canEdit: Bool { readable && permission != "read" }
    public func field(_ name: String) -> String { fields[name]?.string(or: "") ?? "" }
    public var title: String { field("title") }
    public var username: String { field("username") }
    public var password: String { field("password") }
    public var url: String { field("url") }
    public var notes: String { field("notes") }
    public var host: String { VaultCrypto.loginHost(url) }
}

/// One person a share would reach, with the state of their pinned fingerprint.
public struct VaultRecipient: Equatable, Sendable {
    public let name: String
    /// "new", "pinned", "changed", "no_vault" or "invalid_keys".
    public let state: String
    public let fingerprint: String
    public let bundle: VaultBundle?

    public init(name: String, state: String, fingerprint: String, bundle: VaultBundle?) {
        self.name = name
        self.state = state
        self.fingerprint = fingerprint
        self.bundle = bundle
    }
}

public struct VaultShareOutcome: Equatable, Sendable {
    public let sealedTo: [String]
    public let skipped: [String]
    public let withoutVault: [String]
}

public enum VaultFields {
    /// The field limits, in order, counted in UTF-16 units.
    public static let limits: [(name: String, limit: Int)] = [
        ("title", 200),
        ("username", 320),
        ("password", 4096),
        ("url", 2048),
        ("notes", 8000),
    ]

    public static func limit(_ name: String) -> Int { limits.first { $0.name == name }?.limit ?? 0 }

    /// The five fields, cut to their limits, title and url trimmed; `title_required` without a title.
    public static func clean(_ fields: JSONObject) throws -> JSONObject {
        var out = JSONObject()
        for (name, limit) in limits {
            let text = fields[name]?.string(or: "") ?? ""
            let units = Array(text.utf16)
            // A cut through a surrogate pair becomes U+FFFD.
            out[name] = .string(units.count > limit ? String(decoding: units[0..<limit], as: UTF16.self) : text)
        }
        out["title"] = .string(out.str("title").dartTrimmed)
        out["url"] = .string(out.str("url").dartTrimmed)
        if out.str("title").isEmpty { throw VaultError("title_required") }
        return out
    }
}

/// A JSON value that may be missing as text: missing and null print as "null".
func dartString(_ value: JSONValue?) -> String {
    guard let value, !value.isNull else { return "null" }
    return value.text
}

/// Percent-encodes the UTF-8 bytes of everything but ASCII letters, digits and `-_.!~*'()`.
func encodeComponent(_ text: String) -> String {
    var out = ""
    for byte in text.utf8 {
        let c = Character(Unicode.Scalar(byte))
        if byte < 0x80 && (c.isLetter || c.isNumber || "-_.!~*'()".contains(c)) {
            out.append(c)
        } else {
            out += String(format: "%%%02X", byte)
        }
    }
    return out
}

@Observable
@MainActor
public final class VaultClient {
    @ObservationIgnored private let request: VaultRequest
    @ObservationIgnored public let normalizer: VaultNormalizer?

    /// Tests use the smallest stretch the suite accepts to stay quick.
    @ObservationIgnored public var newMemoryKiB = 65536
    @ObservationIgnored public var newPasses = 3

    public private(set) var account = ""
    public private(set) var installed = false
    /// The account record.
    @ObservationIgnored private var server = JSONObject()
    private var keyset: VaultKeyset?
    @ObservationIgnored private var pins = JSONObject()
    public private(set) var items: [VaultItem] = []
    @ObservationIgnored private var itemKeys: [String: [UInt8]] = [:]

    public init(request: @escaping VaultRequest, normalizer: VaultNormalizer? = VaultCrypto.nfkc) {
        self.request = request
        self.normalizer = normalizer
    }

    public var unlocked: Bool { keyset != nil }

    public func find(_ id: String) -> VaultItem? { items.first { $0.id == id } }

    private func keys() throws -> VaultKeyset {
        guard let keyset else { throw VaultError("vault_locked") }
        return keyset
    }

    @discardableResult
    private func call(_ method: String, _ path: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await request(method, path, body)
    }

    /// The crypto layer and this one share their error codes; callers see one error type.
    private func guarded<T>(_ run: () async throws -> T) async throws -> T {
        do {
            return try await run()
        } catch let error as VaultCryptoError {
            throw VaultError(error.code)
        }
    }

    private func wipeItemKeys() {
        for id in Array(itemKeys.keys) { CryptoBytes.wipe(&itemKeys[id]!) }
        itemKeys.removeAll()
    }

    public func lock() {
        wipeItemKeys()
        keyset?.wipe()
        keyset = nil
        items = []
        pins = JSONObject()
    }

    public func status() async throws {
        let payload = try await call("GET", vaultApi)
        account = payload["actor"]?.string(or: "") ?? ""
        installed = payload["installed"] == .bool(true)
        server = installed ? (payload["account"]?.objectValue ?? JSONObject()) : JSONObject()
    }

    // MARK: - Trust

    private func saveTrust() async throws {
        let blob = try VaultCrypto.sealPrivate(try keys(), label: "trust", value: ["pins": .object(pins)])
        try await call("PUT", "\(vaultApi)/trust", ["trust": .string(blob)])
        server["trust"] = .string(blob)
    }

    /// "new" on first sight, "pinned" when it matches, "changed" when not.
    nonisolated static func trustState(_ pins: JSONObject, _ name: String, _ fingerprint: String) -> String {
        guard let pinned = pins[name] else { return "new" }
        return pinned == .string(fingerprint) ? "pinned" : "changed"
    }

    // MARK: - Unlocking

    private func adopt(_ opened: VaultKeyset) throws {
        // The keyset must be the one whose public half the server publishes: otherwise shares
        // sealed to this account could never be opened.
        let mine = try VaultBundle.of(opened, account: account)
        guard let published = server["bundle"]?.objectValue,
              published["x25519"] == .string(VaultCrypto.toBase64(mine.x25519)),
              published["mlkem768"] == .string(VaultCrypto.toBase64(mine.mlkem768)),
              published["ed25519"] == .string(VaultCrypto.toBase64(mine.ed25519)) else {
            opened.wipe()
            throw VaultError("keyset_mismatch")
        }
        keyset = opened
        pins = JSONObject()
        let trust = server["trust"]?.string(or: "") ?? ""
        if !trust.isEmpty {
            let stored = try VaultCrypto.openPrivate(opened, label: "trust", blob: .string(trust))
            if let saved = stored["pins"]?.objectValue {
                var next = JSONObject()
                for (name, fingerprint) in saved { next[name] = .string(dartString(fingerprint)) }
                pins = next
            }
        }
    }

    private func newKdf() -> VaultKdf { VaultKdf.fresh(memoryKiB: newMemoryKiB, passes: newPasses) }

    /// Argon2id on a detached task: seconds of work that must never hold the main actor.
    private func stretch(_ password: String, _ kdf: VaultKdf) async throws -> [UInt8] {
        let normalizer = self.normalizer
        return try await Task.detached(priority: .userInitiated) {
            try VaultCrypto.stretch(password, kdf, normalizer: normalizer)
        }.value
    }

    /// Creates the vault and returns the recovery code, shown exactly once.
    public func setUp(_ masterPassword: String) async throws -> String {
        try await guarded {
            try await status()
            if installed { throw VaultError("vault_exists") }
            let kdf = newKdf()
            var masterKey = try await stretch(masterPassword, kdf)
            let fresh = VaultKeyset.generate()
            let recoveryCode = VaultCrypto.newRecoveryCode()
            var recoveryKey = try VaultCrypto.recoverySecret(recoveryCode)
            defer {
                CryptoBytes.wipe(&masterKey)
                CryptoBytes.wipe(&recoveryKey)
            }
            let payload = try await call("POST", "\(vaultApi)/install", [
                "kdf": .object(kdf.toJSON()),
                "wrappedKeyset": .string(try VaultCrypto.wrapKeyset(masterKey, purpose: "master", account: account, keyset: fresh)),
                "wrappedRecovery": .string(try VaultCrypto.wrapKeyset(recoveryKey, purpose: "recovery", account: account, keyset: fresh)),
                "bundle": .object(try VaultBundle.of(fresh, account: account).toJSON()),
            ])
            installed = true
            server = payload.obj("account")
            keyset?.wipe()
            keyset = fresh
            pins = JSONObject()
            items = []
            return recoveryCode
        }
    }

    public func unlock(_ masterPassword: String) async throws {
        try await guarded {
            try await status()
            if !installed { throw VaultError("vault_not_set_up") }
            let kdf = try VaultKdf(json: server["kdf"])
            var masterKey = try await stretch(masterPassword, kdf)
            defer { CryptoBytes.wipe(&masterKey) }
            try adopt(try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: account, blob: server["wrappedKeyset"]))
        }
    }

    /// Re-enters an unlocked vault from a keyset the app kept behind the device's biometric or
    /// passcode prompt.
    public func resume(_ savedKeyset: JSONObject) async throws {
        try await guarded {
            try await status()
            if !installed { throw VaultError("vault_not_set_up") }
            try adopt(try VaultKeyset(json: .object(savedKeyset)))
        }
    }

    public func exportKeyset() throws -> JSONObject { try keys().toJSON() }

    private func rewrap(_ masterPassword: String?, newRecovery: Bool) async throws -> String {
        try await guarded {
            let keyset = try keys()
            let revision = server["revision"].flatMap { $0.isNull ? nil : $0 } ?? 1
            var body: JSONObject = ["revision": revision]
            var recoveryCode = ""
            if let masterPassword {
                let kdf = newKdf()
                var masterKey = try await stretch(masterPassword, kdf)
                defer { CryptoBytes.wipe(&masterKey) }
                body["kdf"] = .object(kdf.toJSON())
                body["wrappedKeyset"] = .string(try VaultCrypto.wrapKeyset(masterKey, purpose: "master", account: account, keyset: keyset))
            }
            if newRecovery {
                recoveryCode = VaultCrypto.newRecoveryCode()
                var recoveryKey = try VaultCrypto.recoverySecret(recoveryCode)
                defer { CryptoBytes.wipe(&recoveryKey) }
                body["wrappedRecovery"] = .string(try VaultCrypto.wrapKeyset(recoveryKey, purpose: "recovery", account: account, keyset: keyset))
            }
            let payload = try await call("PUT", "\(vaultApi)/keys", body)
            server = payload.obj("account")
            return recoveryCode
        }
    }

    public func changePassword(_ masterPassword: String) async throws {
        _ = try await rewrap(masterPassword, newRecovery: false)
    }

    public func renewRecoveryCode() async throws -> String {
        try await rewrap(nil, newRecovery: true)
    }

    /// A used recovery code is spent: recovering sets a new master password and returns a new
    /// code in one step.
    public func recover(_ recoveryCode: String, newMasterPassword: String) async throws -> String {
        try await guarded {
            try await status()
            if !installed { throw VaultError("vault_not_set_up") }
            var recoveryKey = try VaultCrypto.recoverySecret(recoveryCode)
            defer { CryptoBytes.wipe(&recoveryKey) }
            let blob = try await call("GET", "\(vaultApi)/recovery")["wrappedRecovery"]
            do {
                try adopt(try VaultCrypto.unwrapKeyset(recoveryKey, purpose: "recovery", account: account, blob: blob))
            } catch let error as VaultCryptoError {
                throw VaultError(error.code == "wrong_password" ? "wrong_recovery_code" : error.code)
            }
            return try await rewrap(newMasterPassword, newRecovery: true)
        }
    }

    // MARK: - Reading

    struct Listing: Sendable {
        var items: [VaultItem]
        var itemKeys: [String: [UInt8]]
        var pins: JSONObject
        var pinsChanged: Bool
    }

    nonisolated static func item(_ row: JSONObject, owned: Bool, fields: JSONObject = JSONObject(), error: String = "") -> VaultItem {
        let via = row["via"]?.objectValue
        return VaultItem(
            id: dartString(row["id"]),
            owner: row["owner"]?.string(or: "") ?? "",
            owned: owned,
            permission: row["permission"]?.string(or: "") ?? "",
            revision: row["revision"]?.intValue ?? 1,
            updatedAt: row["updatedAt"]?.int64Value ?? 0,
            updatedBy: row["updatedBy"]?.string(or: "") ?? "",
            viaKind: via.map { dartString($0["kind"]) } ?? "",
            viaName: via.map { dartString($0["name"]) } ?? "",
            shares: row.list("shares").compactMap(\.objectValue).map {
                VaultShare(kind: dartString($0["kind"]), name: dartString($0["name"]), permission: dartString($0["permission"]))
            },
            fields: fields,
            error: error)
    }

    /// Opens a GET /api/vault/items answer: the owner's own items under the vault key, shared ones
    /// through their grants after checking the sender's pinned keys. Pure, so it runs detached.
    nonisolated static func openListing(_ payload: JSONObject, keyset: VaultKeyset, account: String, pins start: JSONObject) -> Listing {
        var listing = Listing(items: [], itemKeys: [:], pins: start, pinsChanged: false)
        for row in payload.list("items").compactMap(\.objectValue) {
            let id = dartString(row["id"])
            do {
                let itemKey = try VaultCrypto.unwrapItemKey(keyset, itemId: id, blob: row["wrappedKey"])
                let fields = try VaultCrypto.openItem(itemKey, itemId: id, blob: row["body"])
                listing.itemKeys[id] = itemKey
                listing.items.append(item(row, owned: true, fields: fields))
            } catch {
                listing.items.append(item(row, owned: true, error: (error as? VaultCryptoError)?.code ?? "invalid_item"))
            }
        }
        let bundles = payload["bundles"]?.objectValue ?? JSONObject()
        for row in payload.list("shared").compactMap(\.objectValue) {
            let id = dartString(row["id"]), owner = dartString(row["owner"])
            do {
                guard bundles.contains(owner) else { throw VaultCryptoError("sender_keys_missing") }
                let sender = try VaultBundle.verify(bundles[owner], account: owner)
                let fingerprint = sender.fingerprint()
                let state = trustState(listing.pins, owner, fingerprint)
                if state == "changed" { throw VaultCryptoError("sender_keys_changed") }
                let itemKey = try VaultCrypto.openGrant(
                    keyset: keyset, recipient: account, sender: sender, itemId: id,
                    grant: row["grant"], permission: dartString(row["permission"]))
                let fields = try VaultCrypto.openItem(itemKey, itemId: id, blob: row["body"])
                // Pinned only after the share's signature held under these keys.
                if state == "new" {
                    listing.pins[owner] = .string(fingerprint)
                    listing.pinsChanged = true
                }
                listing.itemKeys[id] = itemKey
                listing.items.append(item(row, owned: false, fields: fields))
            } catch {
                listing.items.append(item(row, owned: false, error: (error as? VaultCryptoError)?.code ?? "invalid_item"))
            }
        }
        listing.items.sort { a, b in
            if a.readable != b.readable { return a.readable }
            return a.title.lowercased().utf16.lexicographicallyPrecedes(b.title.lowercased().utf16)
        }
        return listing
    }

    public func load() async throws -> [VaultItem] {
        try await guarded {
            let keyset = try keys()
            let payload = try await call("GET", "\(vaultApi)/items")
            guard self.keyset === keyset else { throw VaultError("vault_locked") }
            // The detached work gets its own copy of the keys: `lock()` wipes ours meanwhile.
            let copy = keyset.copy()
            let account = self.account, known = self.pins
            var listing = await Task.detached(priority: .userInitiated) {
                defer { copy.wipe() }
                return VaultClient.openListing(payload, keyset: copy, account: account, pins: known)
            }.value
            guard self.keyset === keyset else {
                for id in Array(listing.itemKeys.keys) { CryptoBytes.wipe(&listing.itemKeys[id]!) }
                throw VaultError("vault_locked")
            }
            wipeItemKeys()
            itemKeys = listing.itemKeys
            pins = listing.pins
            items = listing.items
            if listing.pinsChanged { try await saveTrust() }
            return items
        }
    }

    /// Readable logins saved for exactly this host (the autofill match rule).
    public func matching(_ host: String) -> [VaultItem] {
        items.filter { $0.readable && VaultCrypto.matchesHost($0.url, host) }
    }

    nonisolated public static func loginOrigin(_ url: String) -> String {
        let text = url.dartTrimmed
        if text.isEmpty { return "" }
        let full = text.range(of: "^[A-Za-z][A-Za-z0-9+.-]*://", options: .regularExpression) != nil ? text : "http://" + text
        guard let parts = URLComponents(string: full), let scheme = parts.scheme?.lowercased(),
              scheme == "http" || scheme == "https" else { return "" }
        var host = (parts.host ?? "").lowercased()
        if host.hasPrefix("[") && host.hasSuffix("]") { host = String(host.dropFirst().dropLast()) }
        if host.isEmpty { return "" }
        return "\(scheme)://\(host):\(parts.port ?? (scheme == "https" ? 443 : 80))"
    }

    public func logins(for url: String) -> [VaultItem] {
        let origin = Self.loginOrigin(url)
        if origin.isEmpty || !unlocked { return [] }
        return items.filter { $0.readable && !$0.password.isEmpty && Self.loginOrigin($0.url) == origin }
    }

    public func rememberLogin(_ fields: JSONObject) async throws -> (id: String, changed: Bool) {
        _ = try keys()
        let url = fields["url"]?.string(or: "") ?? ""
        if Self.loginOrigin(url).isEmpty { throw VaultError("invalid_url") }
        _ = try await load()
        let username = fields["username"]?.string(or: "") ?? ""
        let password = fields["password"]?.string(or: "") ?? ""
        let match = logins(for: url).first { $0.permission != "read" && $0.username == username }
        if let match, match.password == password { return (match.id, false) }
        var next = fields
        if let match {
            next = match.fields
            next["password"] = .string(password)
        }
        let id = try await save(next, id: match?.id ?? "")
        _ = try await load()
        return (id, true)
    }

    // MARK: - Writing

    @discardableResult
    public func save(_ fields: JSONObject, id: String = "") async throws -> String {
        try await guarded {
            let keyset = try keys()
            let clean = try VaultFields.clean(fields)
            if id.isEmpty {
                let itemId = VaultCrypto.newItemId()
                var itemKey = VaultCrypto.randomBytes(32)
                try await call("POST", "\(vaultApi)/items", [
                    "id": .string(itemId),
                    "body": .string(try VaultCrypto.sealItem(itemKey, itemId: itemId, fields: clean)),
                    "wrappedKey": .string(try VaultCrypto.wrapItemKey(keyset, itemId: itemId, itemKey: itemKey)),
                ])
                if self.keyset === keyset {
                    itemKeys[itemId] = itemKey
                } else {
                    CryptoBytes.wipe(&itemKey)
                }
                return itemId
            }
            guard let current = find(id), let itemKey = itemKeys[id] else { throw VaultError("item_not_found") }
            if current.permission == "read" { throw VaultError("read_only") }
            try await call("PUT", "\(vaultApi)/items/\(id)", [
                "revision": JSONValue(current.revision),
                "body": .string(try VaultCrypto.sealItem(itemKey, itemId: id, fields: clean)),
            ])
            return id
        }
    }

    public func remove(_ id: String) async throws {
        _ = try keys()
        try await call("DELETE", "\(vaultApi)/items/\(id)")
        if var key = itemKeys.removeValue(forKey: id) { CryptoBytes.wipe(&key) }
    }

    // MARK: - Sharing

    private func recipients(_ members: [JSONValue]) -> [VaultRecipient] {
        var out = [VaultRecipient]()
        for member in members.compactMap(\.objectValue) {
            let name = dartString(member["name"])
            guard member["bundle"]?.objectValue != nil else {
                out.append(VaultRecipient(name: name, state: "no_vault", fingerprint: "", bundle: nil))
                continue
            }
            do {
                let bundle = try VaultBundle.verify(member["bundle"], account: name)
                let fingerprint = bundle.fingerprint()
                out.append(VaultRecipient(name: name, state: Self.trustState(pins, name, fingerprint), fingerprint: fingerprint, bundle: bundle))
            } catch {
                out.append(VaultRecipient(name: name, state: "invalid_keys", fingerprint: "", bundle: nil))
            }
        }
        return out
    }

    /// Who a share would reach, with each person's fingerprint and whether it is new, pinned, or
    /// changed since it was pinned. Nothing is sent yet.
    public func previewShare(kind: String, name: String) async throws -> [VaultRecipient] {
        _ = try keys()
        let clean = encodeComponent(name.dartTrimmed.lowercased())
        if kind == "user" {
            let payload = try await call("GET", "\(vaultApi)/users/\(clean)")
            return recipients([["name": payload["name"] ?? .null, "bundle": payload["bundle"] ?? .null]])
        }
        let payload = try await call("GET", "\(vaultApi)/organizations/\(clean)/members")
        return recipients(payload.list("members"))
    }

    /// Grants for everyone in `members` who may be sealed to. `accepted` names people whose
    /// changed fingerprint the owner has just re-verified.
    private func grantsFor(
        _ id: String, _ itemKey: [UInt8], kind: String, name: String, permission: String,
        members: [VaultRecipient], accepted: [String] = []
    ) throws -> (grants: [JSONObject], skipped: [String]) {
        var grants = [JSONObject](), skipped = [String]()
        for member in members {
            if member.name == account || member.state == "no_vault" { continue }
            guard let bundle = member.bundle, member.state != "changed" || accepted.contains(member.name) else {
                skipped.append(member.name)
                continue
            }
            grants.append([
                "recipient": .string(member.name),
                "via": ["kind": .string(kind), "name": .string(name)],
                "grant": .object(try VaultCrypto.sealGrant(
                    keyset: try keys(), sender: account, recipient: bundle, itemId: id, itemKey: itemKey, permission: permission)),
            ])
            pins[member.name] = .string(member.fingerprint)
        }
        return (grants, skipped)
    }

    public func share(_ id: String, kind: String, name: String, permission: String = "read", accepted: [String] = []) async throws -> VaultShareOutcome {
        try await guarded {
            _ = try keys()
            guard let current = find(id), current.owned, let itemKey = itemKeys[id] else {
                throw VaultError("owner_required")
            }
            let members = try await previewShare(kind: kind, name: name)
            var target = name.dartTrimmed.lowercased()
            if kind == "user" {
                guard let only = members.first, only.state != "no_vault" else { throw VaultError("recipient_has_no_vault") }
                if only.state == "invalid_keys" { throw VaultError("invalid_public_keys") }
                if only.state == "changed" && !accepted.contains(only.name) { throw VaultError("recipient_keys_changed") }
                target = only.name
            }
            let (grants, skipped) = try grantsFor(
                id, itemKey, kind: kind, name: target, permission: permission, members: members, accepted: accepted)
            try await call("POST", "\(vaultApi)/items/\(id)/shares", [
                "target": ["kind": .string(kind), "name": .string(target)],
                "permission": .string(permission),
                "grants": .array(grants.map(JSONValue.object)),
            ])
            try await saveTrust()
            return VaultShareOutcome(
                sealedTo: grants.map { dartString($0["recipient"]) },
                skipped: skipped,
                withoutVault: members.filter { $0.state == "no_vault" }.map(\.name))
        }
    }

    /// Re-encrypt an item under a fresh key and seal that key to everyone its remaining shares
    /// cover. Whoever was removed keeps only a dead key.
    private func rotate(_ id: String) async throws {
        guard let current = find(id), current.owned, current.readable else { throw VaultError("owner_required") }
        let listing = try await call("GET", "\(vaultApi)/items")
        var fresh: JSONObject?
        for row in listing.list("items").compactMap(\.objectValue) where row["id"] == .string(id) { fresh = row }
        let groups = try await call("GET", "\(vaultApi)/items/\(id)/recipients").list("recipients")
        // Someone with write access may have saved since this list was loaded: what gets
        // re-encrypted is the server's current body, not a stale copy.
        let fields: JSONObject
        if let fresh, let oldKey = itemKeys[id] {
            fields = try VaultCrypto.openItem(oldKey, itemId: id, blob: fresh["body"])
        } else {
            fields = current.fields
        }
        let itemKey = VaultCrypto.randomBytes(32)
        var grants = [JSONObject]()
        for group in groups.compactMap(\.objectValue) {
            let sealed = try grantsFor(
                id, itemKey, kind: dartString(group["kind"]), name: dartString(group["name"]),
                permission: dartString(group["permission"]), members: recipients(group.list("members")))
            grants.append(contentsOf: sealed.grants)
        }
        try await call("PUT", "\(vaultApi)/items/\(id)", [
            "revision": JSONValue(fresh?["revision"]?.intValue ?? current.revision),
            "body": .string(try VaultCrypto.sealItem(itemKey, itemId: id, fields: fields)),
            "wrappedKey": .string(try VaultCrypto.wrapItemKey(try keys(), itemId: id, itemKey: itemKey)),
            "grants": .array(grants.map(JSONValue.object)),
        ])
        if itemKeys[id] != nil { CryptoBytes.wipe(&itemKeys[id]!) }
        itemKeys[id] = itemKey
    }

    public func unshare(_ id: String, kind: String, name: String) async throws {
        try await guarded {
            _ = try keys()
            try await call("POST", "\(vaultApi)/items/\(id)/unshare", ["kind": .string(kind), "name": .string(name)])
            try await rotate(id)
        }
    }

    /// Members who joined an organization, or made their vault, after an item was shared with it.
    /// Only a holder of the item key can seal to them, so the owner's client catches up each time
    /// it unlocks.
    public func syncPending() async throws -> Int {
        try await guarded {
            _ = try keys()
            let pending = try await call("GET", "\(vaultApi)/pending").list("pending")
            var sealedCount = 0
            for entry in pending.compactMap(\.objectValue) {
                let id = dartString(entry["itemId"])
                guard let itemKey = itemKeys[id] else { continue }
                let (grants, _) = try grantsFor(
                    id, itemKey, kind: "organization", name: dartString(entry["organization"]),
                    permission: dartString(entry["permission"]), members: recipients(entry.list("members")))
                if grants.isEmpty { continue }
                try await call("POST", "\(vaultApi)/items/\(id)/grants", ["grants": .array(grants.map(JSONValue.object))])
                sealedCount += grants.count
            }
            if sealedCount > 0 { try await saveTrust() }
            return sealedCount
        }
    }

    public func organizations() async throws -> [String] {
        try await call("GET", "\(vaultApi)/organizations").list("organizations").compactMap(\.objectValue).map { dartString($0["name"]) }
    }

    public func myFingerprint() async throws -> String {
        try await guarded { try VaultBundle.of(try keys(), account: account).fingerprint() }
    }

    public func exportData() async throws -> JSONObject {
        try await call("GET", "\(vaultApi)/export")
    }

    @discardableResult
    public func destroy() async throws -> JSONObject {
        let payload = try await call("POST", "\(vaultApi)/uninstall", JSONObject())
        lock()
        installed = false
        server = JSONObject()
        return payload
    }
}
