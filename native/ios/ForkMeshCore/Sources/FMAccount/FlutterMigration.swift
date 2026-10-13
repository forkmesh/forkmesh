import FMBase
import Foundation

public enum FlutterMigrationOutcome: Equatable, Sendable {
    case alreadyDone
    case nothingToMigrate
    case migrated(secrets: Int, preferences: Int)
    case deferred(reason: String)
}

public enum PreferenceValue: Equatable, Sendable {
    case bool(Bool)
    case int(Int)
    case double(Double)
    case string(String)
    case stringList([String])

    public static func read(_ store: KeyValueStore, _ key: String) -> PreferenceValue? {
        if let value = store.bool(forKey: key) { return .bool(value) }
        if let value = store.int(forKey: key) { return .int(value) }
        if let value = store.double(forKey: key) { return .double(value) }
        if let value = store.string(forKey: key) { return .string(value) }
        if let value = store.stringList(forKey: key) { return .stringList(value) }
        return nil
    }

    public func write(_ store: KeyValueStore, _ key: String) {
        switch self {
        case .bool(let value): store.set(value, forKey: key)
        case .int(let value): store.set(value, forKey: key)
        case .double(let value): store.set(value, forKey: key)
        case .string(let value): store.set(value, forKey: key)
        case .stringList(let value): store.set(value, forKey: key)
        }
    }
}

public final class FlutterMigration {
    public static let flutterSecureService = "flutter_secure_storage_service"
    public static let flutterPreferencePrefix = "flutter."

    public static let doneKey = "migration/flutter/done"
    public static let doneAtKey = "migration/flutter/doneAt"
    public static let secretKeysKey = "migration/flutter/secretKeys"
    public static let preferenceKeysKey = "migration/flutter/preferenceKeys"
    public static let retiredKey = "migration/flutter/retired"
    public static let attemptsKey = "migration/flutter/attempts"

    public static let upgradeProjectId = AppEnv.prodHost

    private let flutterSecrets: ThrowingSecureBackend
    private let flutterPrefs: KeyValueStore
    private let secrets: ThrowingSecureBackend
    private let prefs: KeyValueStore
    private let clock: Clock

    public init(
        flutterSecrets: ThrowingSecureBackend,
        flutterPrefs: KeyValueStore,
        secrets: ThrowingSecureBackend,
        prefs: KeyValueStore,
        clock: Clock = SystemClock()
    ) {
        self.flutterSecrets = flutterSecrets
        self.flutterPrefs = flutterPrefs
        self.secrets = secrets
        self.prefs = prefs
        self.clock = clock
    }

    public var isDone: Bool { prefs.bool(forKey: FlutterMigration.doneKey) == true }
    public var isRetired: Bool { prefs.bool(forKey: FlutterMigration.retiredKey) == true }

    public var migratedSecretKeys: [String] { prefs.stringList(forKey: FlutterMigration.secretKeysKey) ?? [] }
    public var migratedPreferenceKeys: [String] { prefs.stringList(forKey: FlutterMigration.preferenceKeysKey) ?? [] }

    public var migratedSessionProjects: [String] {
        let prefix = "auth/session/"
        return migratedSecretKeys.filter { $0.hasPrefix(prefix) }.map { String($0.dropFirst(prefix.count)) }.sorted()
    }

    public var migratedAnySession: Bool {
        migratedSecretKeys.contains { $0 == "auth/session" || $0.hasPrefix("auth/session/") }
    }

    private static func isBookkeeping(_ key: String) -> Bool { key.hasPrefix("migration/flutter/") }

    @discardableResult
    public func run() -> FlutterMigrationOutcome {
        if isDone { return .alreadyDone }
        prefs.set((prefs.int(forKey: FlutterMigration.attemptsKey) ?? 0) + 1, forKey: FlutterMigration.attemptsKey)

        let sourceSecrets: [String: String]
        do {
            sourceSecrets = try flutterSecrets.readAll()
        } catch {
            return .deferred(reason: "Flutter keychain unreadable: \(error)")
        }
        var sourcePrefs: [String: PreferenceValue] = [:]
        for key in flutterPrefs.keys where !FlutterMigration.isBookkeeping(key) {
            if let value = PreferenceValue.read(flutterPrefs, key) { sourcePrefs[key] = value }
        }

        if sourceSecrets.isEmpty && sourcePrefs.isEmpty {
            markDone(secretKeys: [], preferenceKeys: [])
            return .nothingToMigrate
        }

        for key in sourceSecrets.keys.sorted() {
            guard let value = sourceSecrets[key] else { continue }
            do {
                if try secrets.read(key) != value { try secrets.write(key, value) }
            } catch {
                return .deferred(reason: "native keychain write failed for \(key): \(error)")
            }
        }
        for key in sourcePrefs.keys.sorted() {
            guard let value = sourcePrefs[key] else { continue }
            if PreferenceValue.read(prefs, key) != value { value.write(prefs, key) }
        }

        let active = (sourcePrefs[RelayDirectory.activeKey]).flatMap { value -> String? in
            if case .string(let id) = value, !id.isEmpty { return id }
            return nil
        }
        if active == nil {
            prefs.set(FlutterMigration.upgradeProjectId, forKey: RelayDirectory.activeKey)
        }

        for (key, value) in sourceSecrets {
            let stored: String?
            do {
                stored = try secrets.read(key)
            } catch {
                return .deferred(reason: "native keychain read-back failed for \(key): \(error)")
            }
            if stored != value { return .deferred(reason: "native keychain read-back mismatch for \(key)") }
        }
        for (key, value) in sourcePrefs where PreferenceValue.read(prefs, key) != value {
            return .deferred(reason: "preference read-back mismatch for \(key)")
        }

        markDone(secretKeys: sourceSecrets.keys.sorted(), preferenceKeys: sourcePrefs.keys.sorted())
        Log.info("migration", "Flutter data migrated: \(sourceSecrets.count) secrets, \(sourcePrefs.count) preferences")
        return .migrated(secrets: sourceSecrets.count, preferences: sourcePrefs.count)
    }

    private func markDone(secretKeys: [String], preferenceKeys: [String]) {
        prefs.set(secretKeys, forKey: FlutterMigration.secretKeysKey)
        prefs.set(preferenceKeys, forKey: FlutterMigration.preferenceKeysKey)
        prefs.set(Int(clock.nowMillis), forKey: FlutterMigration.doneAtKey)
        prefs.set(true, forKey: FlutterMigration.doneKey)
    }

    public func canRetire(sessionAccepted: Bool) -> Bool {
        guard isDone, !isRetired else { return false }
        return sessionAccepted || !migratedAnySession
    }

    @discardableResult
    public func retireFlutterData(sessionAccepted: Bool) -> Bool {
        guard canRetire(sessionAccepted: sessionAccepted) else { return false }
        for key in migratedSecretKeys {
            do {
                try flutterSecrets.delete(key)
            } catch {
                Log.warning("migration", "Flutter keychain item not removed yet (\(key)): \(error)")
                return false
            }
        }
        for key in migratedPreferenceKeys {
            flutterPrefs.remove(key)
        }
        prefs.set(true, forKey: FlutterMigration.retiredKey)
        Log.info("migration", "Flutter copies removed after the native session was verified")
        return true
    }
}
