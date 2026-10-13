// This node's Ed25519 identity (Kotlin: security/Identity.kt), matching ForkMeshIdentity in the
// Qt client (raw 32-byte key, base64url) and the Worker's ed25519_verify, so events this app
// signs are accepted on the relay.

import FMBase
import Foundation

/// The public key, base64url-encoded without padding, is the canonical node id: the chat sender
/// id, the repo owner namespace and the `author` on signed inbox events.
///
/// Stored format (identical to the retired Flutter app): secure-store key `seedKey` holds the
/// standard padded base64 of the raw 32-byte Ed25519 seed.
///
/// Calls are synchronous (Keychain I/O and a scalar multiplication); keep `loadOrCreate` and
/// `rotate` off the main thread. `sign` is thread-safe and sees a `rotate` atomically.
public final class Identity: DeviceIdentity, @unchecked Sendable {
    public static let seedKey = "identity/ed25519Seed"

    /// `rotate()` on an identity made with `fromSeed`, which has no store to persist to.
    public struct NoStoreError: Error, Equatable {
        public init() {}
    }

    private struct Keys {
        let seed: [UInt8]
        let publicKey: [UInt8]
    }

    private let lock = NSLock()
    private var keys: Keys
    private let store: SecureStore?

    private init(keys: Keys, store: SecureStore?) {
        self.keys = keys
        self.store = store
    }

    private var current: Keys {
        lock.lock()
        defer { lock.unlock() }
        return keys
    }

    public var publicKeyBytes: [UInt8] { current.publicKey }

    /// Loads the stored seed, or generates and stores one. A stored value that is not
    /// valid base64 (`EncodingError`) or not 32 bytes (`CryptoArgumentError`) throws instead of
    /// silently replacing the device key.
    ///
    /// Pass `prefs` to move a seed an older build left in plaintext preferences into
    /// `secureStore` first.
    public static func loadOrCreate(secureStore: SecureStore, prefs: KeyValueStore? = nil) throws -> Identity {
        if let prefs { secureStore.migrateFromPrefs(prefs, keys: [seedKey]) }
        let seed: [UInt8]
        if let stored = secureStore.read(seedKey), !stored.isEmpty {
            seed = Array(try B64.decode(stored))
        } else {
            seed = Ed25519.generateSeed()
            secureStore.write(seedKey, B64.encode(seed))
        }
        return Identity(keys: Keys(seed: seed, publicKey: try Ed25519.publicKey(seed: seed)), store: secureStore)
    }

    /// An identity over a known seed that is not tied to storage (worker threads, tests).
    public static func fromSeed(_ seed: [UInt8]) throws -> Identity {
        Identity(keys: Keys(seed: seed, publicKey: try Ed25519.publicKey(seed: seed)), store: nil)
    }

    /// Detached Ed25519 signature over `data`, base64url with no padding: the `sig` field on
    /// inbox events. Deterministic (RFC 8032).
    public func sign(_ data: [UInt8]) -> String {
        let keys = current
        // The seed and key were validated when this identity was built, so signing cannot throw.
        let signature = (try? Ed25519.sign(seed: keys.seed, message: data, publicKey: keys.publicKey)) ?? []
        return B64.encodeUrlNoPadding(signature)
    }

    public func exportSeed() -> [UInt8] { current.seed }

    /// Mints a brand-new Ed25519 key in place and persists its seed to the store this identity
    /// was loaded from, replacing the current one. Every holder of this `Identity` sees the new
    /// key immediately.
    ///
    /// Used when the Worker reports this device's key as revoked or bound to a different account
    /// (`device_revoked` / `device_key_conflict`): a revoked `account_devices` row is permanent,
    /// so re-binding needs a new key. Local repositories are user data and are never touched by a
    /// rotation.
    public func rotate() throws {
        guard let store else { throw NoStoreError() }
        try rotate(secureStore: store)
    }

    /// `rotate`, persisting to `secureStore`.
    public func rotate(secureStore: SecureStore) throws {
        let seed = Ed25519.generateSeed()
        let publicKey = try Ed25519.publicKey(seed: seed)
        secureStore.write(Identity.seedKey, B64.encode(seed))
        lock.lock()
        keys = Keys(seed: seed, publicKey: publicKey)
        lock.unlock()
    }

    /// Re-reads the seed from the store this identity was loaded from (creating and storing one
    /// when it holds none), replacing the current key in place. Every holder of this `Identity`
    /// signs with the reloaded key from then on.
    ///
    /// Used by a project switch: the app's secure store then addresses another project's
    /// namespace, and each project has its own node key (docs/native/PARITY.md, WS1).
    public func reload() throws {
        guard let store else { throw NoStoreError() }
        let seed: [UInt8]
        if let stored = store.read(Identity.seedKey), !stored.isEmpty {
            seed = Array(try B64.decode(stored))
        } else {
            seed = Ed25519.generateSeed()
            store.write(Identity.seedKey, B64.encode(seed))
        }
        let publicKey = try Ed25519.publicKey(seed: seed)
        lock.lock()
        keys = Keys(seed: seed, publicKey: publicKey)
        lock.unlock()
    }

    public static func clearStoredSeed(secureStore: SecureStore, prefs: KeyValueStore? = nil) {
        secureStore.delete(seedKey)
        prefs?.remove(seedKey)
    }
}
