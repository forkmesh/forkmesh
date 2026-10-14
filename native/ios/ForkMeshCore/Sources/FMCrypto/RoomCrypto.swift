// The relay-derived room cipher (Kotlin: crypto/RoomCrypto.kt; the Qt client's RoomCrypto.cpp
// and app/public/chat-crypto.js), byte-compatible with every node on the same relay room.

import FMBase
import Foundation

/// AES-256-GCM with a 12-byte nonce and 16-byte tag. The key is
/// PBKDF2-HMAC-SHA256(passphrase-or-baked-app-key, salt, 210000, 32 bytes), where
/// salt = SHA256("ForkMesh room:" + room).first(16).
///
/// Wire envelope: `{"kind":"cipher","v":1,"nonce":b64,"tag":b64,"body":b64}`.
///
/// Blocking: the first `shared` / `withPassphrase` call for a (room, secret) pair runs PBKDF2,
/// which costs seconds on a phone, so call it off the main thread. Every later call returns the
/// same cached instance, and concurrent callers for one pair wait for a single derivation.
public final class RoomCrypto: @unchecked Sendable {
    private static let appRoomKey = "forkmesh-shared-room-key-v1"
    private static let rounds = 210_000
    private static let nonceBytes = 12
    private static let tagBytes = 16

    private let key: [UInt8]

    private init(key: [UInt8]) {
        self.key = key
    }

    /// Passphrase-free shared room (uses the baked-in app key).
    public static func shared(_ roomName: String) -> RoomCrypto {
        derive(appRoomKey, roomName)
    }

    /// Passphrase-protected room.
    public static func withPassphrase(_ roomName: String, _ passphrase: String) -> RoomCrypto {
        derive(passphrase, roomName)
    }

    /// One slot per (room, secret): the slot's own lock makes later callers wait for the first
    /// derivation instead of starting another.
    private final class Slot {
        let lock = NSLock()
        var value: RoomCrypto?
    }

    // Derived keys by SHA-256(room, secret). 210000 PBKDF2 rounds cost seconds on a phone and the
    // relay re-opens on every reconnect and app resume, so each pair is derived once per process.
    private static let cacheLock = NSLock()
    private static var derived = [String: Slot]()

    private static func derive(_ secret: String, _ roomName: String) -> RoomCrypto {
        let room = roomName.dartTrimmed
        let cacheKey = Sha256.hex(Array("\(room)\n\(secret)".utf8))
        cacheLock.lock()
        let slot: Slot
        if let existing = derived[cacheKey] {
            slot = existing
        } else {
            slot = Slot()
            derived[cacheKey] = slot
        }
        cacheLock.unlock()
        slot.lock.lock()
        defer { slot.lock.unlock() }
        if let value = slot.value { return value }
        let value = RoomCrypto(key: keyBytes(secret, room))
        slot.value = value
        return value
    }

    private static func keyBytes(_ secret: String, _ room: String) -> [UInt8] {
        let salt = Array(Sha256.hash(Array("ForkMesh room:\(room)".utf8))[0..<16])
        return Pbkdf2.hmacSha256(password: Array(secret.utf8), salt: salt, iterations: rounds, length: 32)
    }

    public static func derivedKey(_ roomName: String, passphrase: String) -> [UInt8] {
        keyBytes(passphrase, roomName.dartTrimmed)
    }

    public static func withKey(_ key: [UInt8]) -> RoomCrypto? {
        key.count == 32 ? RoomCrypto(key: key) : nil
    }

    /// Encrypt a JSON object into the wire envelope.
    public func encrypt(_ plain: JSONObject) throws -> JSONObject {
        let box = try AesGcm.encrypt(Array(JSON.encode(plain).utf8), key: key)
        return [
            "kind": "cipher",
            "v": 1,
            "nonce": .string(B64.encode(box.nonce)),
            "tag": .string(B64.encode(box.mac)),
            "body": .string(B64.encode(box.cipherText)),
        ]
    }

    /// Decrypt a wire envelope back into a JSON object, or nil on failure.
    public func decrypt(_ envelope: JSONObject) -> JSONObject? {
        guard envelope["kind"] == .string("cipher"),
              let nonceText = envelope["nonce"]?.stringValue, let nonce = B64.decodeOrNil(nonceText),
              let tagText = envelope["tag"]?.stringValue, let tag = B64.decodeOrNil(tagText),
              let bodyText = envelope["body"]?.stringValue, let body = B64.decodeOrNil(bodyText),
              nonce.count == RoomCrypto.nonceBytes, tag.count == RoomCrypto.tagBytes,
              let clear = AesGcm.decryptOrNull(cipherText: Array(body), nonce: Array(nonce), mac: Array(tag), key: key)
        else { return nil }
        return CryptoText.jsonObject(clear)
    }
}
