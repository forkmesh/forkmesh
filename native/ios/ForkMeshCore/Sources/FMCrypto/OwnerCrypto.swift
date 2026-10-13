// This device's owner-encryption identity (Kotlin: security/OwnerCrypto.kt), the counterpart of
// the Qt client's MirrorCrypto (desktop/src/MirrorCrypto.cpp), byte-compatible with it and with
// the relay's `validate_owner_envelope`.

import FMBase
import Foundation

/// Agent sessions, transcripts and steering prompts never cross the relay in the clear. They
/// travel as `forkmesh.owner-sealed` envelopes: one random AES-256-GCM content key encrypts the
/// payload, and that key is wrapped once per enrolled owner device under a hybrid
/// X25519 + ML-KEM-768 KEM.
///
/// The wrap key is HKDF-SHA256(ikm: ssX25519 || ssMlKem, salt: "forkmesh-mirror-kem-v1",
/// info: ephemeralX25519Pub || mlkemCiphertext), so mixing wrap fields between recipients cannot
/// yield a usable key.
///
/// Stored format (identical to the retired Flutter app): secure-store key `seedKey` holds
/// `{"v":1,"x25519Pub":b64,"x25519Priv":b64,"mlkemPub":b64,"mlkemPriv":b64}` with standard padded
/// base64; the X25519 private key is stored clamped.
///
/// Immutable and thread-safe. ML-KEM and storage are slow-ish (milliseconds); keep `loadOrCreate`,
/// `seal` and `open` off the main thread when they sit on a hot path.
public final class OwnerCrypto: OwnerKeys, @unchecked Sendable {
    public static let seedKey = "ownerCrypto/hybridV1"
    public static let envelopeKind = "forkmesh.owner-sealed"
    public static let envelopeAlg = "x25519+mlkem768/aes256gcm"

    /// Matches MAX_OWNER_ENVELOPE_RECIPIENTS in app/src/security_controls.py and the bound
    /// MirrorCrypto enforces: one wrap entry per enrolled owner device.
    public static let maxRecipients = 6

    static let hkdfSalt = "forkmesh-mirror-kem-v1"
    static let keyBytes = 32
    static let nonceBytes = 12
    static let tagBytes = 16
    static let x25519Bytes = 32
    static let mlkemPublicBytes = 1184
    static let mlkemPrivateBytes = 2400
    static let mlkemCiphertextBytes = 1088
    static let maxBodyBytes = 1024 * 1024

    /// The 32-byte X25519 public key.
    public let x25519Public: [UInt8]
    /// The 1184-byte ML-KEM-768 encapsulation key.
    public let mlkemPublic: [UInt8]
    private let x25519Private: [UInt8]
    private let mlkemPrivate: [UInt8]

    /// base64url(SHA-256(x25519Pub || mlkemPub)) without padding: the same id
    /// MirrorCrypto::keyId() and the Worker's `validate_owner_public_bundle` derive.
    public let keyId: String

    private init(x25519Public: [UInt8], x25519Private: [UInt8], mlkemPublic: [UInt8], mlkemPrivate: [UInt8]) {
        self.x25519Public = x25519Public
        self.x25519Private = x25519Private
        self.mlkemPublic = mlkemPublic
        self.mlkemPrivate = mlkemPrivate
        self.keyId = B64.encodeUrlNoPadding(Sha256.hash(x25519Public, mlkemPublic))
    }

    /// The public-only bundle registered at POST /api/security/owner-keys.
    public var publicBundle: JSONObject {
        [
            "v": 1,
            "x25519": .string(B64.encodeUrlNoPadding(x25519Public)),
            "mlkem768": .string(B64.encodeUrlNoPadding(mlkemPublic)),
        ]
    }

    // MARK: Storage

    /// Load this device's owner identity, generating and persisting one on first use. A stored
    /// blob that cannot be parsed is treated as a rotated/corrupt key: a fresh identity replaces it.
    public static func loadOrCreate(secureStore: SecureStore) -> OwnerCrypto {
        if let stored = secureStore.read(seedKey), !stored.isEmpty, let restored = restore(stored) {
            return restored
        }
        let created = generate()
        secureStore.write(seedKey, created.serialize())
        return created
    }

    /// Discard this device's owner identity (sign-out / revocation). Sealed data already on the
    /// relay stays unreadable to this device afterwards.
    public static func forget(secureStore: SecureStore) {
        secureStore.delete(seedKey)
    }

    /// A fresh identity that is not persisted (tests, previews).
    public static func generate() -> OwnerCrypto {
        let xPriv = X25519.generatePrivateKey()
        // A clamped 32-byte key always has a public key.
        let xPub = (try? X25519.publicKey(privateKey: xPriv)) ?? []
        let mlkem = MlKem768.generateKeyPair()
        return OwnerCrypto(x25519Public: xPub, x25519Private: xPriv, mlkemPublic: mlkem.publicKey, mlkemPrivate: mlkem.secretKey)
    }

    /// The stored JSON (`_serialize`).
    func serialize() -> String {
        JSON.encode(.object([
            "v": 1,
            "x25519Pub": .string(B64.encode(x25519Public)),
            "x25519Priv": .string(B64.encode(x25519Private)),
            "mlkemPub": .string(B64.encode(mlkemPublic)),
            "mlkemPriv": .string(B64.encode(mlkemPrivate)),
        ]))
    }

    /// Parse a stored identity (`_restore`); nil for anything malformed.
    static func restore(_ stored: String) -> OwnerCrypto? {
        guard let data = (try? JSON.parse(stored))?.objectValue, data["v"]?.numEquals(1) == true,
              let xPubText = data["x25519Pub"]?.stringValue, let xPrivText = data["x25519Priv"]?.stringValue,
              let mPubText = data["mlkemPub"]?.stringValue, let mPrivText = data["mlkemPriv"]?.stringValue,
              let xPub = B64.decodeOrNil(xPubText), let xPriv = B64.decodeOrNil(xPrivText),
              let mPub = B64.decodeOrNil(mPubText), let mPriv = B64.decodeOrNil(mPrivText),
              xPub.count == x25519Bytes, xPriv.count == x25519Bytes,
              mPub.count == mlkemPublicBytes, mPriv.count == mlkemPrivateBytes else {
            return nil
        }
        return OwnerCrypto(x25519Public: Array(xPub), x25519Private: Array(xPriv), mlkemPublic: Array(mPub), mlkemPrivate: Array(mPriv))
    }

    // MARK: Envelopes

    /// Every recipient id in envelope order, or `[]` when the framing is invalid. The first entry
    /// is the device that sealed the envelope.
    public static func recipientKeyIds(_ envelope: JSONObject) -> [String] {
        guard envelope["kind"] == .string(envelopeKind), envelope["v"]?.numEquals(1) == true,
              envelope["alg"] == .string(envelopeAlg),
              let recipients = envelope["recipients"]?.arrayValue,
              !recipients.isEmpty, recipients.count <= maxRecipients else {
            return []
        }
        var ids: [String] = []
        for entry in recipients {
            guard let map = entry.objectValue, let kid = map["kid"]?.stringValue else { return [] }
            let length = kid.utf16.count
            if length < 16 || length > 128 || ids.contains(kid) || !CryptoText.isBase64UrlText(kid) { return [] }
            ids.append(kid)
        }
        return ids
    }

    /// Whether this device can open `envelope` without attempting to.
    public func isRecipientOf(_ envelope: JSONObject) -> Bool {
        OwnerCrypto.recipientKeyIds(envelope).contains(keyId)
    }

    /// Open an owner-sealed envelope, or nil when this device is not a recipient, the framing is
    /// malformed, or any tag fails to authenticate. A nil return is never distinguished by cause.
    public func open(_ envelope: JSONObject) -> [UInt8]? {
        let ids = OwnerCrypto.recipientKeyIds(envelope)
        guard let index = ids.firstIndex(of: keyId),
              let recipient = envelope["recipients"]?.arrayValue?[index].objectValue else { return nil }
        guard let payloadNonce = OwnerCrypto.fromB64UrlExact(envelope["nonce"], OwnerCrypto.nonceBytes),
              let payloadTag = OwnerCrypto.fromB64UrlExact(envelope["tag"], OwnerCrypto.tagBytes),
              let payloadBody = OwnerCrypto.fromB64UrlBounded(envelope["body"], OwnerCrypto.maxBodyBytes),
              let ephemeral = OwnerCrypto.fromB64UrlExact(recipient["x25519"], OwnerCrypto.x25519Bytes),
              let kemCiphertext = OwnerCrypto.fromB64UrlExact(recipient["mlkem768"], OwnerCrypto.mlkemCiphertextBytes),
              let wrapNonce = OwnerCrypto.fromB64UrlExact(recipient["nonce"], OwnerCrypto.nonceBytes),
              let wrapTag = OwnerCrypto.fromB64UrlExact(recipient["tag"], OwnerCrypto.tagBytes),
              let wrappedKey = OwnerCrypto.fromB64UrlExact(recipient["key"], OwnerCrypto.keyBytes) else {
            return nil
        }
        do {
            guard var contentKey = try unwrapContentKey(ephemeral, kemCiphertext, wrapNonce, wrapTag, wrappedKey) else {
                return nil
            }
            defer { CryptoBytes.wipe(&contentKey) }
            return AesGcm.decryptOrNull(cipherText: payloadBody, nonce: payloadNonce, mac: payloadTag, key: contentKey)
        } catch {
            return nil // bad KEM material, corrupt ciphertext
        }
    }

    /// Open an owner-sealed envelope carrying UTF-8 JSON, or nil.
    public func openJson(_ envelope: JSONObject) -> JSONObject? {
        open(envelope).flatMap(CryptoText.jsonObject)
    }

    private func unwrapContentKey(
        _ ephemeral: [UInt8], _ kemCiphertext: [UInt8], _ wrapNonce: [UInt8], _ wrapTag: [UInt8], _ wrappedKey: [UInt8]
    ) throws -> [UInt8]? {
        var ssX = try X25519.sharedSecret(privateKey: x25519Private, remotePublicKey: ephemeral)
        var ssM = try MlKem768.decapsulate(secretKey: mlkemPrivate, ciphertext: kemCiphertext)
        var kek = OwnerCrypto.deriveKek(ssX, ssM, CryptoBytes.concat(ephemeral, kemCiphertext))
        CryptoBytes.wipe(&ssX)
        CryptoBytes.wipe(&ssM)
        defer { CryptoBytes.wipe(&kek) }
        guard let unwrapped = AesGcm.decryptOrNull(cipherText: wrappedKey, nonce: wrapNonce, mac: wrapTag, key: kek) else {
            return nil
        }
        return unwrapped.count == OwnerCrypto.keyBytes ? unwrapped : nil
    }

    /// Seal `plaintext` to every bundle in `recipientBundles`, or nil for an empty or oversized
    /// recipient list, a malformed bundle, or a duplicate key id. This device's own bundle must be
    /// included by the caller when it needs to read the result back; a prompt bound for the
    /// desktop must include the desktop's policy key. An ML-KEM key that fails the FIPS 203
    /// modulus check throws `CryptoArgumentError`.
    public func seal(_ plaintext: [UInt8], to recipientBundles: [JSONObject]) throws -> JSONObject? {
        try OwnerCrypto.seal(plaintext, to: recipientBundles)
    }

    /// `seal`, which needs no private key: any holder of the recipients' public bundles can seal.
    public static func seal(_ plaintext: [UInt8], to recipientBundles: [JSONObject]) throws -> JSONObject? {
        if recipientBundles.isEmpty || recipientBundles.count > maxRecipients { return nil }
        var contentKey = CryptoBytes.random(keyBytes)
        defer { CryptoBytes.wipe(&contentKey) }
        let sealed = try AesGcm.encrypt(plaintext, key: contentKey)

        var recipients: [JSONValue] = []
        var seen = Set<String>()
        for bundle in recipientBundles {
            guard bundle["v"]?.numEquals(1) == true,
                  let xPub = fromB64UrlExact(bundle["x25519"], x25519Bytes),
                  let mPub = fromB64UrlExact(bundle["mlkem768"], mlkemPublicBytes) else { return nil }
            let kid = B64.encodeUrlNoPadding(Sha256.hash(xPub, mPub))
            if !seen.insert(kid).inserted { return nil } // the relay refuses duplicate ids

            var ephemeralPrivate = X25519.generatePrivateKey()
            let ephemeralPublic = try X25519.publicKey(privateKey: ephemeralPrivate)
            var ssX = try X25519.sharedSecret(privateKey: ephemeralPrivate, remotePublicKey: xPub)
            let encapsulation = try MlKem768.encapsulate(publicKey: mPub)
            var ssM = encapsulation.sharedSecret
            var kek = deriveKek(ssX, ssM, CryptoBytes.concat(ephemeralPublic, encapsulation.ciphertext))
            let wrapped = try AesGcm.encrypt(contentKey, key: kek)
            CryptoBytes.wipe(&ephemeralPrivate)
            CryptoBytes.wipe(&ssX)
            CryptoBytes.wipe(&ssM)
            CryptoBytes.wipe(&kek)
            recipients.append(.object([
                "kid": .string(kid),
                "x25519": .string(B64.encodeUrlNoPadding(ephemeralPublic)),
                "mlkem768": .string(B64.encodeUrlNoPadding(encapsulation.ciphertext)),
                "nonce": .string(B64.encodeUrlNoPadding(wrapped.nonce)),
                "tag": .string(B64.encodeUrlNoPadding(wrapped.mac)),
                "key": .string(B64.encodeUrlNoPadding(wrapped.cipherText)),
            ]))
        }

        return [
            "kind": .string(envelopeKind),
            "v": 1,
            "alg": .string(envelopeAlg),
            "nonce": .string(B64.encodeUrlNoPadding(sealed.nonce)),
            "tag": .string(B64.encodeUrlNoPadding(sealed.mac)),
            "body": .string(B64.encodeUrlNoPadding(sealed.cipherText)),
            "recipients": .array(recipients),
        ]
    }

    /// Seal a JSON object to `recipientBundles`.
    public func sealJson(_ payload: JSONObject, to recipientBundles: [JSONObject]) throws -> JSONObject? {
        try seal(Array(Utf8.encode(JSON.encode(payload))), to: recipientBundles)
    }

    // MARK: Helpers

    private static func deriveKek(_ ssX: [UInt8], _ ssM: [UInt8], _ info: [UInt8]) -> [UInt8] {
        Hkdf.sha256(ikm: CryptoBytes.concat(ssX, ssM), salt: Array(hkdfSalt.utf8), info: info, length: keyBytes)
    }

    static func fromB64UrlExact(_ value: JSONValue?, _ exactLength: Int) -> [UInt8]? {
        guard let decoded = fromB64Url(value), decoded.count == exactLength else { return nil }
        return decoded
    }

    static func fromB64UrlBounded(_ value: JSONValue?, _ maxLength: Int) -> [UInt8]? {
        guard let decoded = fromB64Url(value), !decoded.isEmpty, decoded.count <= maxLength else { return nil }
        return decoded
    }

    /// Unpadded base64url text only (no '=', only the url alphabet), decoded; nil otherwise.
    static func fromB64Url(_ value: JSONValue?) -> [UInt8]? {
        guard let text = value?.stringValue, !text.isEmpty, CryptoText.isBase64UrlText(text) else { return nil }
        let padded = text + String(repeating: "=", count: (4 - text.utf8.count % 4) % 4)
        return B64.decodeOrNil(padded).map { Array($0) }
    }
}
