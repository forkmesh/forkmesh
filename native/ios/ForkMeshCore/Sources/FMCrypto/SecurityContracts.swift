// Port of native/android/.../security/SecurityContracts.kt: the members the rest of the app uses
// from `Identity` and `OwnerCrypto`, as protocols, so auth, API, inbox,
// agents and chat can be built and tested against fakes. (The Kotlin `SecretStore` is FMBase's
// `SecureStore`.)

import FMBase
import Foundation

/// This device's Ed25519 node identity (`Identity`).
public protocol DeviceIdentity: AnyObject, Sendable {
    /// The 32-byte Ed25519 public key.
    var publicKeyBytes: [UInt8] { get }

    /// Signs `data`; returns the signature as base64url without padding (`sign`).
    func sign(_ data: [UInt8]) -> String

    /// The 32-byte private seed, for the portable identity export and the Git transport's
    /// per-request device signature (`exportSeed`). Never persist, log, or send this anywhere.
    func exportSeed() -> [UInt8]

    /// Replaces the key pair with a fresh one and persists it (`rotate`).
    func rotate() throws
}

extension DeviceIdentity {
    /// base64url of the public key without padding (`publicKeyB64url`).
    public var publicKeyB64url: String { B64.encodeUrlNoPadding(publicKeyBytes) }

    /// The node id relays see; equal to `publicKeyB64url` (`nodeId`).
    public var nodeId: String { publicKeyB64url }

    /// The abbreviated key shown in the UI (`shortKey`).
    public var shortKey: String {
        let k = publicKeyB64url
        return k.count <= 16 ? k : "\(k.prefix(8))...\(k.suffix(8))"
    }

    public func sign(_ data: Data) -> String { sign(Array(data)) }

    /// Signs the UTF-8 of `text`, which is what every canonical signing string is.
    public func sign(utf8 text: String) -> String { sign(Array(text.utf8)) }
}

/// This device's owner-encryption identity (`OwnerCrypto`).
public protocol OwnerKeys: AnyObject, Sendable {
    /// base64url(SHA-256(x25519Public || mlkem768Public)) without padding (`keyId`).
    var keyId: String { get }

    /// The public-only bundle registered at POST /api/security/owner-keys (`publicBundle`).
    var publicBundle: JSONObject { get }

    /// Whether an owner-sealed envelope carries a wrap entry for this device (`isRecipientOf`).
    func isRecipientOf(_ envelope: JSONObject) -> Bool

    /// The plaintext of an envelope sealed to this device, or nil (`open`).
    func open(_ envelope: JSONObject) -> [UInt8]?

    /// The JSON object inside an envelope sealed to this device, or nil (`openJson`).
    func openJson(_ envelope: JSONObject) -> JSONObject?
}

/// The base64url alphabet check: `^[A-Za-z0-9_-]+$`.
enum CryptoText {
    static func isBase64UrlText(_ value: String, length: Int? = nil) -> Bool {
        var count = 0
        for c in value.utf8 {
            let ok = (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) || c == 0x2d || c == 0x5f
            if !ok { return false }
            count += 1
        }
        if let length { return count == length }
        return count > 0
    }

    /// The JSON object in `bytes`, as `jsonDecode(utf8.decode(bytes))` then `is Map<String, dynamic>`.
    static func jsonObject(_ bytes: [UInt8]) -> JSONObject? {
        guard let text = try? Utf8.decode(bytes), let value = try? JSON.parse(text) else { return nil }
        return value.objectValue
    }
}
