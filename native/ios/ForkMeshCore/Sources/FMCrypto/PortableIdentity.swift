// The provider-neutral encrypted identity export (Kotlin: security/PortableIdentity.kt) shared
// with the web client (app/public/portable-identity.js) and the Qt desktop, format documented in
// www/docs/portable-identities.md.

import FMBase
import Foundation

/// An offline recovery-kit container, never an API payload: only the person restoring it has the
/// passphrase. PBKDF2-HMAC-SHA-256 (600000 iterations, 16-byte salt) derives an AES-256-GCM key;
/// the 12-byte IV is `encryption.iv`, the ciphertext is followed by its 16-byte tag, and the
/// authenticated data is UTF-8 `forkmesh.portable-identity/v1`.
///
/// Blocking: each call runs 600000 PBKDF2 rounds (seconds on a phone); keep it off the main thread.
public enum PortableIdentity {
    public static let type = "forkmesh.portable-identity"
    public static let schemaVersion = 1
    private static let aad = "forkmesh.portable-identity/v1"
    private static let iterations = 600_000
    private static let maxPlaintextBytes = 128 * 1024

    /// An unsupported, malformed, tampered or wrong-passphrase container.
    public struct FormatError: Error, Equatable, CustomStringConvertible {
        public let message: String

        public init(_ message: String) {
            self.message = message
        }

        public var description: String { message }
    }

    private static func decodeB64url(_ value: JSONValue?) throws -> [UInt8] {
        guard let text = value?.stringValue, CryptoText.isBase64UrlText(text) else {
            throw FormatError("Invalid base64url.")
        }
        let decoded: [UInt8]
        do {
            decoded = Array(try B64.decodeUnpadded(text))
        } catch let error as EncodingError {
            throw FormatError(error.message)
        }
        if B64.encodeUrlNoPadding(decoded) != text { throw FormatError("Non-canonical base64url.") }
        return decoded
    }

    private static func checkPassphrase(_ passphrase: String) throws {
        // The minimum length counts UTF-16 code units.
        if passphrase.utf16.count < 12 {
            throw CryptoArgumentError("Use a recovery passphrase of at least 12 characters.")
        }
    }

    private static func derive(_ passphrase: String, _ salt: [UInt8]) -> [UInt8] {
        Pbkdf2.hmacSha256(password: Array(passphrase.utf8), salt: salt, iterations: iterations, length: 32)
    }

    /// Encrypts `identity` as a JSON-safe portable recovery-kit document. Throws
    /// `CryptoArgumentError` for a short passphrase or an empty/oversized payload.
    public static func export(_ identity: JSONObject, passphrase: String, createdAt: Int64? = nil) throws -> JSONObject {
        try checkPassphrase(passphrase)
        var plaintext = Array(JSON.encode(identity).utf8)
        if plaintext.isEmpty || plaintext.count > maxPlaintextBytes {
            throw CryptoArgumentError("Identity payload is empty or too large.")
        }
        let salt = CryptoBytes.random(16)
        let nonce = CryptoBytes.random(12)
        var key = derive(passphrase, salt)
        let box = try AesGcm.encrypt(plaintext, key: key, nonce: nonce, aad: Array(aad.utf8))
        CryptoBytes.wipe(&key)
        CryptoBytes.wipe(&plaintext)
        // WebCrypto returns the AES-GCM authentication tag appended to ciphertext.
        let ciphertext = box.cipherText + box.mac
        return [
            "type": .string(type),
            "schemaVersion": .int(Int64(schemaVersion)),
            "createdAt": .int(createdAt ?? SystemClock().nowMillis),
            "kdf": [
                "name": "PBKDF2-HMAC-SHA-256",
                "iterations": .int(Int64(iterations)),
                "salt": .string(B64.encodeUrlNoPadding(salt)),
            ],
            "encryption": [
                "name": "AES-256-GCM",
                "iv": .string(B64.encodeUrlNoPadding(nonce)),
                "ciphertext": .string(B64.encodeUrlNoPadding(ciphertext)),
            ],
        ]
    }

    /// Restores a portable recovery-kit document. Wrong passphrases fail closed with
    /// `FormatError`; a short passphrase throws `CryptoArgumentError`.
    public static func `import`(_ container: JSONObject, passphrase: String) throws -> JSONObject {
        try checkPassphrase(passphrase)
        guard container["type"] == .string(type),
              container["schemaVersion"]?.numEquals(Int64(schemaVersion)) == true,
              let kdf = container["kdf"]?.objectValue,
              let encryption = container["encryption"]?.objectValue,
              kdf["name"] == .string("PBKDF2-HMAC-SHA-256"),
              kdf["iterations"]?.numEquals(Int64(iterations)) == true,
              encryption["name"] == .string("AES-256-GCM")
        else {
            throw FormatError("Unsupported portable identity format.")
        }
        let salt = try decodeB64url(kdf["salt"])
        let nonce = try decodeB64url(encryption["iv"])
        let combined = try decodeB64url(encryption["ciphertext"])
        if salt.count != 16 || nonce.count != 12 || combined.count < 17 || combined.count > maxPlaintextBytes + 16 {
            throw FormatError("Invalid portable identity encryption parameters.")
        }
        var key = derive(passphrase, salt)
        defer { CryptoBytes.wipe(&key) }
        guard let clear = AesGcm.decryptOrNull(
            cipherText: Array(combined[0..<(combined.count - 16)]), nonce: nonce,
            mac: Array(combined[(combined.count - 16)...]), key: key, aad: Array(aad.utf8)),
            let result = CryptoText.jsonObject(clear)
        else {
            throw FormatError("Recovery passphrase is incorrect or the file was changed.")
        }
        return result
    }
}
