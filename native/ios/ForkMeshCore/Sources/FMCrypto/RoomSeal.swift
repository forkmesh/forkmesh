// Automatic device-to-device encryption inside the relay-derived room cipher (Kotlin:
// crypto/RoomSeal.kt; the Qt client's RoomSeal.cpp and app/public/chat-seal.js).

import FMBase
import Foundation

/// Each installation keeps a persistent X25519 private key in secure storage and exchanges only
/// its public half. Every protected application frame gets a fresh AES-256-GCM content key,
/// wrapped once for every known room device using an X25519/HKDF pairwise key. The relay can
/// retain and forward the envelope but cannot unwrap a content key.
///
/// Synchronous and thread-safe: every member takes this instance's lock. `loadOrCreate`,
/// `acceptKeyFrame`, `unseal` and `shouldPersistAdvertisement` may write to the secure store.
public final class RoomSeal: @unchecked Sendable {
    static let version: Int64 = 2
    static let sealedType = "sealed"
    static let keyType = "e2ee-key"
    static let nonceBytes = 12
    static let tagBytes = 16
    static let keyIdChars = 16
    static let maxRecipients = 64
    /// Secure-store key of this installation's X25519 private key (standard base64, clamped).
    public static let privateKeyStoreKey = "chat/e2ee/x25519PrivateV2"
    static let durableAnnouncementMs: Int64 = 24 * 60 * 60 * 1000

    private let roomName: String
    private let storageScope: String
    private let privateKey: [UInt8]
    private let publicKey: [UInt8]
    private let store: SecureStore?
    private let hkdfSalt: [UInt8]

    /// Public, non-secret identifier of this device's X25519 public key.
    public let keyId: String

    private let lock = NSLock()
    private var peers: [String: [UInt8]] = [:]
    private var peerOrder: [String] = []
    private var lastAnnouncement: Int64 = 0
    // Pairwise wrap keys by "sender\nrecipient" key id. Key ids are SHA-256 of the validated
    // public key, so an id pins its key; each entry saves one X25519 + HKDF per frame.
    private var pairwiseKeys: [String: [UInt8]] = [:]

    private init(roomName: String, storageScope: String, privateKey: [UInt8], publicKey: [UInt8], store: SecureStore?) {
        self.roomName = roomName
        self.storageScope = storageScope
        self.privateKey = privateKey
        self.publicKey = publicKey
        self.store = store
        self.keyId = RoomSeal.idFor(publicKey)
        self.hkdfSalt = Sha256.hash(Array("ForkMesh chat e2ee salt:\(roomName)".utf8))
    }

    public var peerCount: Int { locked { peers.count } }

    public var peerKeyIds: [String] { locked { peerOrder.filter { peers[$0] != nil } } }

    // MARK: Construction

    /// Loads (or creates and stores) this installation's device key and the room's known peers.
    /// Throws `CryptoArgumentError` for an empty room.
    public static func loadOrCreate(_ roomName: String, storageScope: String? = nil, secureStore: SecureStore) throws -> RoomSeal {
        let room = roomName.dartTrimmed
        if room.isEmpty { throw CryptoArgumentError("A chat room is required.") }
        var privateKey: [UInt8]?
        if let encoded = secureStore.read(privateKeyStoreKey), !encoded.isEmpty,
           let decoded = B64.decodeOrNil(encoded), decoded.count == 32 {
            privateKey = Array(decoded)
        }
        if privateKey == nil {
            let generated = X25519.generatePrivateKey()
            secureStore.write(privateKeyStoreKey, B64.encode(generated))
            privateKey = generated
        }
        let seal = try fromPrivateKey(room, privateKey ?? [], storageScope: storageScope, secureStore: secureStore)
        seal.locked { seal.loadPeers() }
        return seal
    }

    /// Fixed-key constructor used by the cross-client wire-vector tests.
    public static func fromPrivateKey(
        _ roomName: String, _ privateKey: [UInt8], storageScope: String? = nil, secureStore: SecureStore? = nil
    ) throws -> RoomSeal {
        let room = roomName.dartTrimmed
        if room.isEmpty || privateKey.count != 32 {
            throw CryptoArgumentError("A room and 32-byte X25519 private key are required.")
        }
        let clamped = try X25519.clampPrivateKey(privateKey)
        let publicKey = try X25519.publicKey(privateKey: clamped)
        let scope = storageScope?.dartTrimmed ?? ""
        return RoomSeal(
            roomName: room, storageScope: scope.isEmpty ? room : scope, privateKey: clamped, publicKey: publicKey,
            store: secureStore)
    }

    // MARK: Frames

    public static func isKeyFrame(_ frame: JSONObject) -> Bool {
        guard frame["type"] == .string(keyType), frame["v"]?.numEquals(version) == true,
              let kid = frame["kid"]?.stringValue, CryptoText.isBase64UrlText(kid, length: 16),
              let pub = frame["pub"]?.stringValue, CryptoText.isBase64UrlText(pub, length: 43) else { return false }
        return true
    }

    public static func isSealedFrame(_ frame: JSONObject) -> Bool {
        guard frame["type"] == .string(sealedType), frame["v"]?.numEquals(version) == true,
              let kid = frame["kid"]?.stringValue, CryptoText.isBase64UrlText(kid, length: 16),
              let pub = frame["pub"]?.stringValue, CryptoText.isBase64UrlText(pub, length: 43),
              let keys = frame["keys"]?.arrayValue, !keys.isEmpty, keys.count <= maxRecipients else { return false }
        return true
    }

    public static func frameKeyId(_ frame: JSONObject) -> String {
        isSealedFrame(frame) ? (frame["kid"]?.stringValue ?? "") : ""
    }

    /// This device's key advertisement.
    public func keyFrame() -> JSONObject {
        [
            "type": .string(RoomSeal.keyType),
            "v": .int(RoomSeal.version),
            "kid": .string(keyId),
            "pub": .string(B64.encodeUrlNoPadding(publicKey)),
        ]
    }

    /// Remembers a peer's advertised key; true when the key is new (and was persisted).
    @discardableResult
    public func acceptKeyFrame(_ frame: JSONObject) -> Bool {
        guard RoomSeal.isKeyFrame(frame), let id = frame["kid"]?.stringValue,
              let publicBytes = RoomSeal.publicBytes(frame["pub"]?.stringValue ?? "") else { return false }
        return locked {
            let isNew = rememberPeer(id, publicBytes)
            // Peers re-advertise on every connect; only a new key changes what storage holds.
            if isNew { persistPeers() }
            return isNew
        }
    }

    /// Whether this device should re-advertise its key durably (at most once a day per scope and
    /// room).
    public func shouldPersistAdvertisement(now: Int64 = Date().epochMillis) -> Bool {
        locked {
            var previous = lastAnnouncement
            if let store {
                previous = RoomSeal.dartIntTryParse(store.read(announcedStoreKey) ?? "") ?? 0
            }
            if now - previous < RoomSeal.durableAnnouncementMs { return false }
            lastAnnouncement = now
            store?.write(announcedStoreKey, String(now))
            return true
        }
    }

    /// Seals `plain` to every known room device and this device.
    public func seal(_ plain: JSONObject) throws -> JSONObject {
        try locked { try sealLocked(plain, for: peerOrder) }
    }

    public func seal(_ plain: JSONObject, for keyIds: [String]) throws -> JSONObject {
        try locked { try sealLocked(plain, for: keyIds) }
    }

    private func sealLocked(_ plain: JSONObject, for keyIds: [String]) throws -> JSONObject {
        var contentBytes = CryptoBytes.random(32)
        defer { CryptoBytes.wipe(&contentBytes) }
        let content = try AesGcm.encrypt(Array(Utf8.encode(JSON.encode(plain))), key: contentBytes, aad: contentAad(keyId))

        var recipients: [String: [UInt8]] = [:]
        for id in keyIds {
            if let peer = peers[id] { recipients[id] = peer }
        }
        recipients[keyId] = publicKey
        var wrappedKeys: [JSONValue] = []
        // Ids are sorted by UTF-16 code units; key ids are ASCII, where Swift's order agrees.
        for id in recipients.keys.sorted() {
            let kek = try pairwiseKey(recipients[id] ?? [], keyId, id)
            let wrapped = try AesGcm.encrypt(contentBytes, key: kek, aad: wrapAad(keyId, id, content.nonce))
            wrappedKeys.append(.object([
                "kid": .string(id),
                "nonce": .string(B64.encode(wrapped.nonce)),
                "tag": .string(B64.encode(wrapped.mac)),
                "key": .string(B64.encode(wrapped.cipherText)),
            ]))
        }
        return [
            "type": .string(RoomSeal.sealedType),
            "v": .int(RoomSeal.version),
            "kid": .string(keyId),
            "pub": .string(B64.encodeUrlNoPadding(publicKey)),
            "nonce": .string(B64.encode(content.nonce)),
            "tag": .string(B64.encode(content.mac)),
            "body": .string(B64.encode(content.cipherText)),
            "keys": .array(wrappedKeys),
        ]
    }

    /// The plaintext of a sealed frame that carries a wrap entry for this device, or nil. A
    /// successfully opened frame from an unknown sender adds it as a peer.
    public func unseal(_ frame: JSONObject) -> JSONObject? {
        guard RoomSeal.isSealedFrame(frame), let senderId = frame["kid"]?.stringValue,
              let senderBytes = RoomSeal.publicBytes(frame["pub"]?.stringValue ?? ""),
              RoomSeal.idFor(senderBytes) == senderId else { return nil }
        let entries = frame["keys"]?.arrayValue ?? []
        guard let wrapped = entries.lazy.compactMap(\.objectValue).first(where: { $0["kid"] == .string(keyId) }) else {
            return nil
        }
        guard let bodyNonce = RoomSeal.decode(frame["nonce"]), let bodyTag = RoomSeal.decode(frame["tag"]),
              let body = RoomSeal.decode(frame["body"]), let keyNonce = RoomSeal.decode(wrapped["nonce"]),
              let keyTag = RoomSeal.decode(wrapped["tag"]), let wrappedKey = RoomSeal.decode(wrapped["key"]),
              bodyNonce.count == RoomSeal.nonceBytes, bodyTag.count == RoomSeal.tagBytes, !body.isEmpty,
              keyNonce.count == RoomSeal.nonceBytes, keyTag.count == RoomSeal.tagBytes, wrappedKey.count == 32 else {
            return nil
        }
        return locked {
            guard let kek = try? pairwiseKey(senderBytes, senderId, keyId),
                  var contentBytes = AesGcm.decryptOrNull(
                      cipherText: wrappedKey, nonce: keyNonce, mac: keyTag, key: kek, aad: wrapAad(senderId, keyId, bodyNonce)),
                  contentBytes.count == 32 else { return nil }
            defer { CryptoBytes.wipe(&contentBytes) }
            guard let clear = AesGcm.decryptOrNull(
                cipherText: body, nonce: bodyNonce, mac: bodyTag, key: contentBytes, aad: contentAad(senderId)),
                let decoded = CryptoText.jsonObject(clear) else { return nil }
            if senderId != keyId && rememberPeer(senderId, senderBytes) { persistPeers() }
            return decoded
        }
    }

    // MARK: Peers (call with the lock held)

    private func rememberPeer(_ id: String, _ publicBytes: [UInt8]) -> Bool {
        if id == keyId || publicBytes.count != 32 || RoomSeal.idFor(publicBytes) != id { return false }
        let isNew = peers[id] == nil
        peerOrder.removeAll { $0 == id }
        peerOrder.append(id)
        peers[id] = publicBytes
        while peerOrder.count >= RoomSeal.maxRecipients {
            peers.removeValue(forKey: peerOrder.removeFirst())
        }
        return isNew
    }

    private var scopeSuffix: String { B64.encodeUrlNoPadding(Array("\(storageScope)\n\(roomName)".utf8)) }
    private var peersStoreKey: String { "chat/e2ee/peersV2/\(scopeSuffix)" }
    private var announcedStoreKey: String { "chat/e2ee/announcedV2/\(scopeSuffix)" }

    private func loadPeers() {
        guard let store, let decoded = (try? JSON.parse(store.read(peersStoreKey) ?? "[]"))?.arrayValue else { return }
        let skip = decoded.count > RoomSeal.maxRecipients - 1 ? decoded.count - (RoomSeal.maxRecipients - 1) : 0
        for value in decoded.dropFirst(skip) {
            guard let map = value.objectValue else { continue }
            let id = map["kid"]?.string(or: "") ?? ""
            if let publicBytes = RoomSeal.publicBytes(map["pub"]?.string(or: "") ?? "") {
                _ = rememberPeer(id, publicBytes)
            }
        }
    }

    private func persistPeers() {
        guard let store else { return }
        // Peer persistence is an availability optimization: a failed write keeps the validated
        // key in memory and never weakens encryption.
        let values: [JSONValue] = peerOrder.compactMap { id in
            peers[id].map { .object(["kid": .string(id), "pub": .string(B64.encodeUrlNoPadding($0))]) }
        }
        store.write(peersStoreKey, JSON.encode(.array(values)))
    }

    // MARK: Keys

    private func contentAad(_ senderKeyId: String) -> [UInt8] {
        Array("forkmesh-chat-e2ee-content-v\(RoomSeal.version)\n\(roomName)\n\(senderKeyId)".utf8)
    }

    private func wrapAad(_ senderKeyId: String, _ recipientKeyId: String, _ bodyNonce: [UInt8]) -> [UInt8] {
        Array(("forkmesh-chat-e2ee-wrap-v\(RoomSeal.version)\n\(roomName)\n\(senderKeyId)\n"
            + "\(recipientKeyId)\n\(B64.encodeUrlNoPadding(bodyNonce))").utf8)
    }

    private func pairwiseKey(_ peer: [UInt8], _ senderKeyId: String, _ recipientKeyId: String) throws -> [UInt8] {
        let cacheKey = "\(senderKeyId)\n\(recipientKeyId)"
        if let cached = pairwiseKeys[cacheKey] { return cached }
        if pairwiseKeys.count >= RoomSeal.maxRecipients * 4 { pairwiseKeys.removeAll() }
        let derived = try derivePairwiseKey(peer, senderKeyId, recipientKeyId)
        pairwiseKeys[cacheKey] = derived
        return derived
    }

    private func derivePairwiseKey(_ peer: [UInt8], _ senderKeyId: String, _ recipientKeyId: String) throws -> [UInt8] {
        var shared = try X25519.sharedSecret(privateKey: privateKey, remotePublicKey: peer)
        defer { CryptoBytes.wipe(&shared) }
        if CryptoBytes.isAllZero(shared) { throw CryptoArgumentError("Invalid X25519 shared key.") }
        return Hkdf.sha256(
            ikm: shared, salt: hkdfSalt,
            info: Array("forkmesh-chat-e2ee-kek-v\(RoomSeal.version)\n\(roomName)\n\(senderKeyId)\n\(recipientKeyId)".utf8),
            length: 32)
    }

    // MARK: Helpers

    static func idFor(_ publicKey: [UInt8]) -> String {
        String(B64.encodeUrlNoPadding(Sha256.hash(publicKey)).prefix(keyIdChars))
    }

    static func publicBytes(_ value: String) -> [UInt8]? {
        guard CryptoText.isBase64UrlText(value, length: 43), let bytes = B64.decodeOrNil(value + "="), bytes.count == 32 else {
            return nil
        }
        return Array(bytes)
    }

    /// `base64.decode(x as String)`: nil when not a string or not base64.
    private static func decode(_ value: JSONValue?) -> [UInt8]? {
        value?.stringValue.flatMap { B64.decodeOrNil($0) }.map { Array($0) }
    }

    /// Parses the decimal timestamps this class writes, also accepting a leading sign, a `0x` hex
    /// prefix and surrounding whitespace.
    static func dartIntTryParse(_ source: String) -> Int64? {
        var text = Substring(source.dartTrimmed)
        if text.isEmpty { return nil }
        var negative = false
        if text.first == "+" || text.first == "-" {
            negative = text.first == "-"
            text = text.dropFirst()
        }
        var radix = 10
        if text.count > 2, text.hasPrefix("0x") || text.hasPrefix("0X") {
            radix = 16
            text = text.dropFirst(2)
        }
        if text.isEmpty || !text.allSatisfy({ $0.isASCII && $0.isHexDigit && (radix == 16 || $0.isNumber) }) { return nil }
        return Int64((negative ? "-" : "") + text, radix: radix)
    }

    private func locked<R>(_ body: () throws -> R) rethrows -> R {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}
