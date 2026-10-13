// The part of secure storage that FMBase's `SecureStore` protocol does not cover: a platform
// backend that moves to an in-memory store for the rest of the session when the platform store
// fails, instead of losing every write.

import FMBase
import Foundation

/// A platform secret store that reports failures (the Keychain's `OSStatus`) instead of hiding
/// them. `read` returns nil for a missing item and throws only when the store itself failed.
public protocol ThrowingSecureBackend: AnyObject, Sendable {
    func read(_ key: String) throws -> String?
    func write(_ key: String, _ value: String) throws
    func delete(_ key: String) throws
    func readAll() throws -> [String: String]
}

/// `SecureStore` over a `ThrowingSecureBackend` with a memory fallback: the first
/// failure moves reads and writes to memory for the rest of the session (a secret written then is
/// still readable until the app quits), while deletes keep reaching the platform store, so a
/// session written there before the failure cannot survive Sign out and come back on next launch.
public final class FallbackSecureStore: SecureStore, @unchecked Sendable {
    private let backend: ThrowingSecureBackend
    private let lock = NSLock()
    private var fallback: [String: String] = [:]
    private var useFallback = false

    public init(backend: ThrowingSecureBackend) {
        self.backend = backend
    }

    /// True once the platform store has failed and memory took over.
    public var isUsingFallback: Bool {
        lock.lock()
        defer { lock.unlock() }
        return useFallback
    }

    public func read(_ key: String) -> String? {
        if isUsingFallback { return locked { fallback[key] } }
        do {
            return try backend.read(key)
        } catch {
            return switchToFallback { fallback[key] }
        }
    }

    public func write(_ key: String, _ value: String) {
        if isUsingFallback {
            locked { fallback[key] = value }
            return
        }
        do {
            try backend.write(key, value)
        } catch {
            switchToFallback { fallback[key] = value }
        }
    }

    public func delete(_ key: String) {
        locked { _ = fallback.removeValue(forKey: key) }
        do {
            try backend.delete(key)
        } catch {
            switchToFallback {}
        }
    }

    public func readAll() -> [String: String] {
        if isUsingFallback { return locked { fallback } }
        do {
            return try backend.readAll()
        } catch {
            return switchToFallback { fallback }
        }
    }

    @discardableResult
    private func switchToFallback<R>(_ body: () -> R) -> R {
        locked {
            useFallback = true
            return body()
        }
    }

    private func locked<R>(_ body: () -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }
}
