import FMBase
import FMCrypto
import Foundation

public enum CoveStoreError: Error, Equatable, CustomStringConvertible {
    case needsNameAndPassword
    case needsNameAndAccount
    case notAccountScoped
    case accountScoped
    case locked
    case missingParameters
    case crypto(CoveCryptoError)
    case grantsFailed

    public var description: String {
        switch self {
        case .needsNameAndPassword: return "A cove needs a name and a password."
        case .needsNameAndAccount: return "A cove needs a name and a creator account."
        case .notAccountScoped: return "This cove is not account-scoped."
        case .accountScoped: return "Account-scoped coves are saved without a password."
        case .locked: return "Unlock the cove before saving it."
        case .missingParameters: return "The cove is missing its encryption parameters."
        case .crypto(let error): return error.description
        case .grantsFailed: return "Could not seal the cove's access grants."
        }
    }
}

public enum CoveStore {
    public static let covesDir = ".forkmesh/coves"
    public static let envelopeVersion = 2
    public static let contentKeyBytes = 32
    public static let grantSlotBlock = 4
    public static let maxAccessEntries = 500

    public static func normalizedAccount(_ account: String) -> String {
        account.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    public static func normalizedAccounts(_ accounts: [String]) -> [String] {
        var out: [String] = []
        for account in accounts {
            let normalized = normalizedAccount(account)
            if !normalized.isEmpty && !out.contains(normalized) { out.append(normalized) }
        }
        return out.sorted { $0.lowercased() < $1.lowercased() }
    }

    public static func cleanPath(_ path: String) -> String {
        var parts: [String] = []
        for part in path.split(separator: "/", omittingEmptySubsequences: true) {
            if part == "." { continue }
            if part == ".." {
                if !parts.isEmpty && parts.last != ".." { parts.removeLast() } else if !path.hasPrefix("/") { parts.append("..") }
                continue
            }
            parts.append(String(part))
        }
        let joined = parts.joined(separator: "/")
        return path.hasPrefix("/") ? "/" + joined : joined
    }

    public static func relPath(slug: String) -> String { "\(covesDir)/\(slug).cove" }

    public static func slug(fromPath path: String) -> String {
        let file = path.split(separator: "/").last.map(String.init) ?? path
        return file.hasSuffix(".cove") ? String(file.dropLast(5)) : file
    }

    public static func newId() -> String { UUID().uuidString.lowercased() }

    public static func obscureSlug() -> String { Hex.encode(CoveCrypto.randomBytes(16)) }

    public static func accountSecretV1(_ cove: Cove) -> String {
        if !cove.accountScoped { return "" }
        var invitees = normalizedAccounts(cove.invitedAccounts)
        let creator = normalizedAccount(cove.creatorAccount)
        if !creator.isEmpty && !invitees.contains(creator) { invitees.append(creator) }
        invitees.sort { $0.lowercased() < $1.lowercased() }
        let material = "forkmesh-account-cove-v1\n\(cove.id)\n\(cove.creator)\n\(creator)\n\(invitees.joined(separator: "\n"))"
        return B64.encodeUrlNoPadding(Sha256.hash(Array(material.utf8)))
    }

    public static func grantSecret(coveId: String, account: String) -> String {
        "forkmesh-cove-grant-v2\n\(coveId)\n\(normalizedAccount(account))"
    }

    public static func parseEnvelope(_ bytes: [UInt8]) -> Cove? {
        guard let object = try? JSON.parse(Data(bytes)).objectValue else { return nil }
        guard object["kind"]?.stringValue == "cove" else { return nil }
        var cove = Cove()
        cove.id = object["id"]?.stringValue ?? ""
        cove.version = object["v"].map { $0.int(or: 1) } ?? 1
        cove.name = object["name"]?.stringValue ?? ""
        cove.creator = object["creator"]?.stringValue ?? ""
        let access = object["access"]?.objectValue ?? JSONObject()
        cove.accessMode = access["mode"]?.stringValue ?? (cove.version >= 2 ? "" : "password")
        cove.creatorAccount = normalizedAccount(access["creatorAccount"]?.stringValue ?? "")
        var invited: [String] = []
        for value in access["invitedAccounts"]?.arrayValue ?? [] {
            let account = normalizedAccount(value.stringValue ?? "")
            if !account.isEmpty && !invited.contains(account) { invited.append(account) }
        }
        cove.invitedAccounts = invited
        cove.createdAtMs = object["createdAtMs"].map { $0.int64(or: 0) } ?? 0
        cove.notifyOnOpen = object["notifyOnOpen"]?.boolValue ?? false
        cove.grants = (object["grants"]?.arrayValue ?? []).compactMap { CoveCipher(json: $0.objectValue) }
        let kdf = object["kdf"]?.objectValue ?? JSONObject()
        cove.salt = CoveBase64.decode(kdf["salt"]?.stringValue ?? "")
        cove.rounds = kdf["rounds"].map { $0.int(or: 0) } ?? 0
        cove.cipher = CoveCipher(json: object["cipher"]?.objectValue)
        if cove.id.isEmpty || cove.salt.isEmpty || cove.rounds <= 0 { return nil }
        return cove
    }

    public static func parseEnvelope(_ bytes: [UInt8], path: String) -> Cove? {
        guard var cove = parseEnvelope(bytes) else { return nil }
        cove.relPath = path
        cove.slug = slug(fromPath: path)
        return cove
    }

    public static func envelope(_ cove: Cove) -> JSONObject {
        [
            "cipher": .object(cove.cipher?.json ?? JSONObject()),
            "grants": .array(cove.grants.map { .object($0.json) }),
            "id": .string(cove.id),
            "kdf": .object([
                "algo": .string("pbkdf2-sha256"), "rounds": .int(Int64(cove.rounds)),
                "salt": .string(CoveBase64.encode(cove.salt)),
            ]),
            "kind": .string("cove"),
            "v": .int(Int64(envelopeVersion)),
        ]
    }

    public static func envelopeText(_ cove: Cove) -> String {
        JSON.encodePretty(.object(envelope(cove)), indent: "    ") + "\n"
    }

    public static func payload(_ cove: Cove) -> [UInt8] {
        var object: JSONObject = [
            "accessLog": .array(cove.accessLog.map { .object($0.json) }),
            "createdAtMs": .int(cove.createdAtMs),
            "creator": .string(cove.creator),
        ]
        if cove.accountScoped {
            object["creatorAccount"] = .string(normalizedAccount(cove.creatorAccount))
        }
        object["documents"] = .array(cove.documents.map { .object($0.json) })
        if cove.accountScoped {
            object["invitedAccounts"] = .array(normalizedAccounts(cove.invitedAccounts).map { .string($0) })
        }
        object["mode"] = .string(cove.accessMode)
        object["name"] = .string(cove.name)
        object["notifyOnOpen"] = .bool(cove.notifyOnOpen)
        return Array(JSON.encode(object).utf8)
    }

    @discardableResult
    public static func applyPayload(_ cove: inout Cove, _ plain: [UInt8]) -> Bool {
        guard let payload = try? JSON.parse(Data(plain)).objectValue else { return false }
        if let name = payload["name"]?.stringValue { cove.name = name }
        if let mode = payload["mode"]?.stringValue {
            cove.accessMode = mode
        } else if cove.accessMode.isEmpty {
            cove.accessMode = "password"
        }
        if let creator = payload["creator"]?.stringValue { cove.creator = creator }
        cove.creatorAccount = normalizedAccount(payload["creatorAccount"]?.stringValue ?? cove.creatorAccount)
        if let invited = payload["invitedAccounts"] {
            var accounts: [String] = []
            for value in invited.arrayValue ?? [] {
                let account = normalizedAccount(value.stringValue ?? "")
                if !account.isEmpty && !accounts.contains(account) { accounts.append(account) }
            }
            cove.invitedAccounts = accounts
        }
        if let notify = payload["notifyOnOpen"] { cove.notifyOnOpen = notify.boolValue ?? false }
        if let created = payload["createdAtMs"] { cove.createdAtMs = created.int64(or: 0) }
        cove.documents = (payload["documents"]?.arrayValue ?? []).compactMap(\.objectValue).map(CoveDocument.init(json:))
        cove.accessLog = (payload["accessLog"]?.arrayValue ?? []).compactMap(\.objectValue).map(CoveAccessEntry.init(json:))
        cove.unlocked = true
        return true
    }

    public static func unlock(_ cove: inout Cove, password: String) -> Bool {
        guard let cipher = cove.cipher, let crypto = try? CoveCrypto(password: password, salt: cove.salt, rounds: cove.rounds),
            let plain = crypto.decrypt(cipher)
        else { return false }
        return applyPayload(&cove, plain)
    }

    public static func accountCanAccess(_ cove: Cove, account: String) -> Bool {
        if !cove.accountScoped { return false }
        let me = normalizedAccount(account)
        if me.isEmpty { return false }
        if normalizedAccount(cove.creatorAccount) == me { return true }
        return normalizedAccounts(cove.invitedAccounts).contains(me)
    }

    public static func unlockForAccount(_ cove: inout Cove, account: String) -> Bool {
        let me = normalizedAccount(account)
        if me.isEmpty || cove.salt.isEmpty || cove.rounds <= 0 { return false }
        guard let cipher = cove.cipher else { return false }
        if !cove.grants.isEmpty {
            guard let kek = try? CoveCrypto(password: grantSecret(coveId: cove.id, account: me), salt: cove.salt, rounds: cove.rounds)
            else { return false }
            for grant in cove.grants {
                guard let key = kek.decrypt(grant), key.count == contentKeyBytes else { continue }
                guard let body = try? CoveCrypto(key: key), let plain = body.decrypt(cipher),
                    applyPayload(&cove, plain)
                else { return false }
                cove.contentKey = key
                return true
            }
            return false
        }
        if !accountCanAccess(cove, account: me) { return false }
        guard let crypto = try? CoveCrypto(password: accountSecretV1(cove), salt: cove.salt, rounds: cove.rounds),
            let plain = crypto.decrypt(cipher)
        else { return false }
        return applyPayload(&cove, plain)
    }

    public static func appendAccess(_ cove: inout Cove, _ entry: CoveAccessEntry) {
        cove.accessLog.append(entry)
        if cove.accessLog.count > maxAccessEntries {
            cove.accessLog.removeFirst(cove.accessLog.count - maxAccessEntries)
        }
    }

    public static func decoyGrant() -> CoveCipher {
        CoveCipher(
            nonce: CoveBase64.encode(CoveCrypto.randomBytes(CoveCrypto.nonceBytes)),
            tag: CoveBase64.encode(CoveCrypto.randomBytes(CoveCrypto.tagBytes)),
            body: CoveBase64.encode(CoveCrypto.randomBytes(contentKeyBytes)))
    }

    public static func buildGrants(_ cove: Cove, contentKey: [UInt8]) throws -> [CoveCipher] {
        var slots: [CoveCipher] = []
        if cove.accountScoped {
            for account in cove.members {
                let kek: CoveCrypto
                do {
                    kek = try CoveCrypto(password: grantSecret(coveId: cove.id, account: account), salt: cove.salt, rounds: cove.rounds)
                    slots.append(try kek.encrypt(contentKey))
                } catch {
                    throw CoveStoreError.grantsFailed
                }
            }
        }
        var target = max(slots.count, 1)
        target = ((target + grantSlotBlock - 1) / grantSlotBlock) * grantSlotBlock
        while slots.count < target { slots.append(decoyGrant()) }
        var generator = SystemRandomNumberGenerator()
        slots.shuffle(using: &generator)
        return slots
    }

    public static func sealPassword(_ cove: Cove, password: String) throws -> Cove {
        if cove.accountScoped { throw CoveStoreError.accountScoped }
        if cove.relPath.isEmpty || cove.salt.isEmpty || cove.rounds <= 0 { throw CoveStoreError.missingParameters }
        let crypto: CoveCrypto
        do {
            crypto = try CoveCrypto(password: password, salt: cove.salt, rounds: cove.rounds)
        } catch let error as CoveCryptoError {
            throw CoveStoreError.crypto(error)
        }
        var sealed = cove
        if sealed.accessMode.isEmpty { sealed.accessMode = CoveAccessMode.password.rawValue }
        do {
            sealed.cipher = try crypto.encrypt(payload(sealed))
        } catch let error as CoveCryptoError {
            throw CoveStoreError.crypto(error)
        }
        sealed.grants = try buildGrants(sealed, contentKey: [])
        sealed.version = envelopeVersion
        return sealed
    }

    public static func sealAccount(_ cove: Cove) throws -> Cove {
        if !cove.accountScoped { throw CoveStoreError.notAccountScoped }
        if !cove.unlocked { throw CoveStoreError.locked }
        if cove.relPath.isEmpty || cove.salt.isEmpty || cove.rounds <= 0 { throw CoveStoreError.missingParameters }
        var sealed = cove
        sealed.invitedAccounts = normalizedAccounts(sealed.invitedAccounts)
        sealed.creatorAccount = normalizedAccount(sealed.creatorAccount)
        if sealed.contentKey.count != contentKeyBytes { sealed.contentKey = CoveCrypto.randomKey() }
        do {
            let crypto = try CoveCrypto(key: sealed.contentKey)
            sealed.cipher = try crypto.encrypt(payload(sealed))
        } catch let error as CoveCryptoError {
            throw CoveStoreError.crypto(error)
        }
        sealed.grants = try buildGrants(sealed, contentKey: sealed.contentKey)
        sealed.version = envelopeVersion
        return sealed
    }

    public static func newPasswordCove(
        name: String, password: String, creator: String, notifyOnOpen: Bool, documents: [CoveDocument] = [],
        nowMs: Int64, rounds: Int = CoveCrypto.defaultRounds
    ) throws -> Cove {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty || password.isEmpty { throw CoveStoreError.needsNameAndPassword }
        var cove = Cove()
        cove.id = newId()
        cove.name = trimmed
        cove.slug = obscureSlug()
        cove.relPath = relPath(slug: cove.slug)
        cove.creator = creator
        cove.createdAtMs = nowMs
        cove.notifyOnOpen = notifyOnOpen
        cove.salt = CoveCrypto.randomSalt()
        cove.rounds = rounds
        cove.documents = documents
        cove.unlocked = true
        return try sealPassword(cove, password: password)
    }

    public static func newAccountCove(
        name: String, creatorAccount: String, invitedAccounts: [String] = [], creator: String, notifyOnOpen: Bool,
        documents: [CoveDocument] = [], nowMs: Int64, rounds: Int = CoveCrypto.defaultRounds
    ) throws -> Cove {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let owner = normalizedAccount(creatorAccount)
        if trimmed.isEmpty || owner.isEmpty { throw CoveStoreError.needsNameAndAccount }
        var cove = Cove()
        cove.id = newId()
        cove.name = trimmed
        cove.slug = obscureSlug()
        cove.relPath = relPath(slug: cove.slug)
        cove.creator = creator
        cove.accessMode = CoveAccessMode.account.rawValue
        cove.creatorAccount = owner
        var invited = normalizedAccounts(invitedAccounts)
        if !invited.contains(owner) { invited.append(owner) }
        cove.invitedAccounts = invited.sorted { $0.lowercased() < $1.lowercased() }
        cove.createdAtMs = nowMs
        cove.notifyOnOpen = notifyOnOpen
        cove.salt = CoveCrypto.randomSalt()
        cove.rounds = rounds
        cove.contentKey = CoveCrypto.randomKey()
        cove.documents = documents
        cove.unlocked = true
        return try sealAccount(cove)
    }

    public static func openCanonical(coveId: String, creatorKey: String, openerKey: String, ts: Int64) -> [UInt8] {
        Array("forkmesh-cove-open-v1\n\(coveId)\n\(creatorKey)\n\(openerKey)\n\(ts)".utf8)
    }
}
