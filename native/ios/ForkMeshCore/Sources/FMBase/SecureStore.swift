// The Keychain surface behind a protocol (the app target implements it in
// Platform/KeychainStore), with an in-memory backend for tests.

import Foundation

/// Secrets at rest. A value that cannot be read (a locked or reset keychain)
/// reads as absent instead of throwing.
/// Calls are synchronous, as the Keychain API is, and thread-safe.
public protocol SecureStore: AnyObject, Sendable {
    func read(_ key: String) -> String?
    func write(_ key: String, _ value: String)
    func delete(_ key: String)
    func readAll() -> [String: String]
}

extension SecureStore {
    /// Copy plaintext preference secrets into secure storage once, then
    /// remove the insecure copies.
    public func migrateFromPrefs<S: Sequence>(_ prefs: KeyValueStore, keys: S) where S.Element == String {
        for key in keys {
            if let existing = read(key), !existing.isEmpty {
                prefs.remove(key)
                continue
            }
            guard let legacy = prefs.string(forKey: key), !legacy.isEmpty else { continue }
            write(key, legacy)
            prefs.remove(key)
        }
    }
}

/// An in-memory store, for tests and for platforms without a keychain.
public final class InMemorySecureStore: SecureStore, @unchecked Sendable {
    private let lock = NSLock()
    private var values: [String: String]

    public init(_ seed: [String: String] = [:]) { values = seed }

    public func read(_ key: String) -> String? {
        lock.lock()
        defer { lock.unlock() }
        return values[key]
    }

    public func write(_ key: String, _ value: String) {
        lock.lock()
        values[key] = value
        lock.unlock()
    }

    public func delete(_ key: String) {
        lock.lock()
        values.removeValue(forKey: key)
        lock.unlock()
    }

    public func readAll() -> [String: String] {
        lock.lock()
        defer { lock.unlock() }
        return values
    }
}
