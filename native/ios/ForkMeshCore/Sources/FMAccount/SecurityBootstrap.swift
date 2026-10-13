// Port of native/android/app/.../feature/account/SecurityBootstrap.kt: the identity and
// owner-crypto startup (`Identity.loadOrCreate`, `OwnerCrypto.loadOrCreate`).

import FMBase
import FMCrypto
import Foundation

/// This device's signing identity and owner-encryption keys, loaded (or created and persisted) at
/// startup, right after the settings.
///
/// A stored key that cannot be read (a corrupt or foreign value) is left nil instead of crashing
/// every launch: sign-in then reports that the signing key is not ready (`identity_unavailable`),
/// and owner-sealed agent sessions count as unreadable. Both loads touch the Keychain and ML-KEM
/// key generation takes milliseconds, so call `load` off the main thread.
public struct SecurityBootstrap: Sendable {
    public let identity: DeviceIdentity?
    public let ownerKeys: OwnerKeys?

    public init(identity: DeviceIdentity?, ownerKeys: OwnerKeys?) {
        self.identity = identity
        self.ownerKeys = ownerKeys
    }

    /// `prefs` lets an identity seed an older build left in plaintext preferences move into the
    /// secure store first.
    public static func load(secureStore: SecureStore, prefs: KeyValueStore? = nil) -> SecurityBootstrap {
        var identity: DeviceIdentity?
        do {
            identity = try Identity.loadOrCreate(secureStore: secureStore, prefs: prefs)
        } catch {
            Log.error("security", "device identity unavailable: \(error)")
        }
        let ownerKeys = OwnerCrypto.loadOrCreate(secureStore: secureStore)
        return SecurityBootstrap(identity: identity, ownerKeys: ownerKeys)
    }

    /// Registers both under their protocol types in `Services`, which is how the Kotlin port's
    /// services find them at use time. Prefer passing them to constructors.
    public func register() {
        if let identity { Services.put(identity, as: DeviceIdentity.self) }
        if let ownerKeys { Services.put(ownerKeys, as: OwnerKeys.self) }
    }
}
