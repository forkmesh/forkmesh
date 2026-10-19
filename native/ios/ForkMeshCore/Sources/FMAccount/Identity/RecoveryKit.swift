import FMBase
import FMCrypto
import Foundation

public enum RecoveryKitError: Error, Equatable, CustomStringConvertible {
    case shortPassphrase
    case mismatchedPassphrase
    case noIdentity
    case unreadable(String)
    case notASeed
    case differentKey(String)

    public var description: String {
        switch self {
        case .shortPassphrase: return "Use a recovery passphrase of at least \(RecoveryKit.minPassphrase) characters."
        case .mismatchedPassphrase: return "The two passphrases do not match."
        case .noIdentity: return "This device has no identity key yet."
        case .unreadable(let message): return message
        case .notASeed: return "That kit does not hold an Ed25519 identity."
        case .differentKey(let key): return "That kit holds a different key (\(RecoveryKit.shortKey(key)))."
        }
    }
}

public struct RecoveryKitCheck: Equatable, Sendable {
    public let publicKey: String
    public let createdAt: Int64
    public let matchesThisDevice: Bool
}

public enum RecoveryKit {
    public static let seedType = "forkmesh.ed25519-seed"
    public static let minPassphrase = 12
    public static let fileExtension = "json"

    public static func validatePassphrase(_ passphrase: String, confirm: String) -> RecoveryKitError? {
        if passphrase.utf16.count < minPassphrase { return .shortPassphrase }
        if passphrase != confirm { return .mismatchedPassphrase }
        return nil
    }

    public static func strength(_ passphrase: String) -> Int {
        let count = passphrase.utf16.count
        if count < minPassphrase { return 0 }
        var classes = 0
        if passphrase.contains(where: { $0.isLowercase }) { classes += 1 }
        if passphrase.contains(where: { $0.isUppercase }) { classes += 1 }
        if passphrase.contains(where: { $0.isNumber }) { classes += 1 }
        if passphrase.contains(where: { !$0.isLetter && !$0.isNumber }) { classes += 1 }
        if count >= 20 && classes >= 2 { return 3 }
        if count >= 16 || classes >= 3 { return 2 }
        return 1
    }

    public static func payload(seed: [UInt8], publicKey: [UInt8]) -> JSONObject {
        [
            "type": .string(seedType),
            "schemaVersion": .int(1),
            "publicKey": .string(B64.encodeUrlNoPadding(publicKey)),
            "seed": .string(B64.encodeUrlNoPadding(seed)),
        ]
    }

    public static func export(identity: DeviceIdentity, passphrase: String, confirm: String, createdAt: Int64) throws -> String {
        if let problem = validatePassphrase(passphrase, confirm: confirm) { throw problem }
        var seed = identity.exportSeed()
        defer { CryptoBytes.wipe(&seed) }
        guard seed.count == 32 else { throw RecoveryKitError.noIdentity }
        let container = try PortableIdentity.export(
            payload(seed: seed, publicKey: identity.publicKeyBytes), passphrase: passphrase, createdAt: createdAt)
        return JSON.encodePretty(.object(container))
    }

    public static func check(kit text: String, passphrase: String, currentPublicKey: String) throws -> RecoveryKitCheck {
        let (seedBytes, createdAt) = try open(kit: text, passphrase: passphrase)
        var seed = seedBytes
        defer { CryptoBytes.wipe(&seed) }
        guard let derived = try? Ed25519.publicKey(seed: seed) else { throw RecoveryKitError.notASeed }
        let publicKey = B64.encodeUrlNoPadding(derived)
        return RecoveryKitCheck(
            publicKey: publicKey, createdAt: createdAt,
            matchesThisDevice: publicKey == currentPublicKey)
    }

    public static func openSeed(kit text: String, passphrase: String) throws -> [UInt8] {
        try open(kit: text, passphrase: passphrase).0
    }

    private static func open(kit text: String, passphrase: String) throws -> ([UInt8], Int64) {
        if passphrase.utf16.count < minPassphrase { throw RecoveryKitError.shortPassphrase }
        guard let data = text.data(using: .utf8), data.count <= 256 * 1024,
              let container = (try? JSON.parse(data))?.objectValue
        else {
            throw RecoveryKitError.unreadable("That file is not a ForkMesh recovery kit.")
        }
        let clear: JSONObject
        do {
            clear = try PortableIdentity.import(container, passphrase: passphrase)
        } catch let error as PortableIdentity.FormatError {
            throw RecoveryKitError.unreadable(error.message)
        }
        guard clear.str("type") == seedType,
              let seedData = try? B64.decodeUnpadded(clear.str("seed")), seedData.count == 32
        else {
            throw RecoveryKitError.notASeed
        }
        var seed = Array(seedData)
        guard let derived = try? Ed25519.publicKey(seed: seed) else {
            CryptoBytes.wipe(&seed)
            throw RecoveryKitError.notASeed
        }
        let recorded = clear.str("publicKey")
        if !recorded.isEmpty && recorded != B64.encodeUrlNoPadding(derived) {
            CryptoBytes.wipe(&seed)
            throw RecoveryKitError.notASeed
        }
        return (seed, container.int64("createdAt"))
    }

    public static func fileName(account: String, createdAt: Int64) -> String {
        let cleaned = account.lowercased().filter { $0.isLetter || $0.isNumber || $0 == "-" }
        let date = Date(timeIntervalSince1970: TimeInterval(createdAt) / 1_000)
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyy-MM-dd"
        let stem = cleaned.isEmpty ? "forkmesh" : "forkmesh-\(cleaned)"
        return "\(stem)-recovery-kit-\(formatter.string(from: date)).\(fileExtension)"
    }

    public static func shortKey(_ key: String) -> String {
        key.count <= 16 ? key : "\(key.prefix(8))...\(key.suffix(8))"
    }
}

public struct IdentityBackupState: Equatable, Sendable {
    public let backedUpKey: String
    public let backedUpAt: Int64
    public let snoozedUntil: Int64

    public init(backedUpKey: String, backedUpAt: Int64, snoozedUntil: Int64) {
        self.backedUpKey = backedUpKey
        self.backedUpAt = backedUpAt
        self.snoozedUntil = snoozedUntil
    }

    public func needsBackup(currentKey: String) -> Bool {
        !currentKey.isEmpty && backedUpKey != currentKey
    }

    public func showsNag(currentKey: String, nowMillis: Int64) -> Bool {
        needsBackup(currentKey: currentKey) && nowMillis >= snoozedUntil
    }
}

public final class IdentityBackupStore: @unchecked Sendable {
    public static let keyKey = "identity/backedUpKey"
    public static let atKey = "identity/backedUpAt"
    public static let snoozeKey = "identity/backupSnoozedUntil"
    public static let snoozeMillis: Int64 = 7 * 86_400_000

    private let prefs: KeyValueStore

    public init(prefs: KeyValueStore) {
        self.prefs = prefs
    }

    public var state: IdentityBackupState {
        IdentityBackupState(
            backedUpKey: prefs.string(forKey: IdentityBackupStore.keyKey) ?? "",
            backedUpAt: Int64(prefs.double(forKey: IdentityBackupStore.atKey) ?? 0),
            snoozedUntil: Int64(prefs.double(forKey: IdentityBackupStore.snoozeKey) ?? 0))
    }

    public func markBackedUp(publicKey: String, at millis: Int64) {
        prefs.set(publicKey, forKey: IdentityBackupStore.keyKey)
        prefs.set(Double(millis), forKey: IdentityBackupStore.atKey)
        prefs.remove(IdentityBackupStore.snoozeKey)
    }

    public func snooze(nowMillis: Int64) {
        prefs.set(Double(nowMillis + IdentityBackupStore.snoozeMillis), forKey: IdentityBackupStore.snoozeKey)
    }
}
