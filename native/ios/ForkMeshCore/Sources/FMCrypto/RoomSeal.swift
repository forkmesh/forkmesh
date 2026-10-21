import FMBase
import Foundation

public enum RoomSealOpen: Equatable, Sendable {
    case opened(plain: JSONObject, senderKid: String, senderAcct: String, senderNode: String)
    case held(String)
    case own
    case notForMe
    case noSession(String)
    case replay
    case invalid

    public var name: String {
        switch self {
        case .opened: return "opened"
        case .held: return "held"
        case .own: return "own"
        case .notForMe: return "notForMe"
        case .noSession: return "noSession"
        case .replay: return "replay"
        case .invalid: return "invalid"
        }
    }

    public var plain: JSONObject? {
        if case .opened(let plain, _, _, _) = self { return plain }
        return nil
    }

    public var senderAcct: String? {
        if case .opened(_, _, let acct, _) = self { return acct }
        return nil
    }

    public var senderNode: String? {
        if case .opened(_, _, _, let node) = self { return node }
        return nil
    }

    public var authenticatedPlain: JSONObject? {
        guard case .opened(var plain, _, _, let node) = self else { return nil }
        if !node.isEmpty { plain["senderId"] = .string(node) }
        return plain
    }

    public var senderKid: String? {
        if case .opened(_, let kid, _, _) = self { return kid }
        return nil
    }
}

public final class RoomSeal: @unchecked Sendable {
    public static let sealedType = "sealed"
    public static let keyType = "e2ee-key"
    public static let privateKeyStoreKey = ChatKeyring.identityStoreKey
    static let durableAnnouncementMs: Int64 = 24 * 60 * 60 * 1000

    public let room: String
    public let scope: String
    public let policy: ChatTrustPolicy
    public let acct: String?
    public let node: String?
    public let participants: Set<String>?
    public let keyring: ChatKeyring
    public let trust: ChatTrustStore
    public let version: Int64

    private let store: SecureStore?
    private let lock = NSLock()
    private var advertisedPrekey = ""
    private let salt: [UInt8]

    public init(
        room roomName: String, scope trustScope: String? = nil, policy: ChatTrustPolicy, acct: String?, node: String?,
        participants: Set<String>? = nil, keyring: ChatKeyring, store: SecureStore? = nil, version: Int64? = nil
    ) throws {
        let trimmed = roomName.dartTrimmed
        if trimmed.isEmpty { throw CryptoArgumentError("A chat room is required.") }
        let scopeName = trustScope?.dartTrimmed ?? ""
        self.room = trimmed
        self.scope = scopeName.isEmpty ? trimmed : scopeName
        self.policy = policy
        self.version = version ?? (policy == .strict ? ChatV4.version : ChatV3.version)
        self.acct = acct.flatMap { ChatV3.isValidAcct($0) ? $0 : nil }
        self.node = node.flatMap { ChatV3.isValidAcct($0) ? $0 : nil }
        self.participants = participants
        self.keyring = keyring
        self.trust = keyring.trust(scopeName.isEmpty ? trimmed : scopeName)
        self.store = store
        self.salt = Sha256.hash(Array("ForkMesh chat e2ee salt v3:\(trimmed)".utf8))
    }

    public static func loadOrCreate(
        _ roomName: String, scope: String? = nil, policy: ChatTrustPolicy = .open, acct: String, node: String,
        participants: Set<String>? = nil, secureStore: SecureStore, now: Int64 = Date().epochMillis
    ) throws -> RoomSeal {
        let keyring = try ChatKeyring.shared(secureStore, now: now)
        let seal = try RoomSeal(
            room: roomName, scope: scope, policy: policy, acct: acct, node: node, participants: participants,
            keyring: keyring, store: secureStore)
        seal.trust.room = seal.room
        return seal
    }

    public var keyId: String { keyring.kid }

    public var approvedKeyIds: [String] {
        trust.records(in: .approved).map(\.kid).filter { $0 != keyring.kid }
    }

    public var peerKeyIds: [String] { approvedKeyIds }

    public var peerCount: Int { approvedKeyIds.count }

    public func approvedKeyIds(acct: String) -> [String] {
        trust.records(in: .approved).filter { $0.acct == acct && $0.kid != keyring.kid }.map(\.kid)
    }


    public static func isKeyFrame(_ frame: JSONObject) -> Bool {
        guard frame["type"] == .string(keyType), frame["v"]?.numEquals(ChatV3.version) == true,
            let kid = frame["kid"]?.stringValue, ChatV3.isKeyId(kid), frame["spk"]?.objectValue != nil
        else { return false }
        return true
    }

    public static func isSealedFrame(_ frame: JSONObject) -> Bool {
        guard frame["type"] == .string(sealedType), let kid = frame["kid"]?.stringValue, ChatV3.isKeyId(kid),
            let keys = frame["keys"]?.arrayValue, keys.count <= ChatV3.maxRecipients
        else { return false }
        if frame["v"]?.numEquals(ChatV4.version) == true { return true }
        return frame["v"]?.numEquals(ChatV3.version) == true && !keys.isEmpty
    }

    public static func frameVersion(_ frame: JSONObject) -> Int64? {
        guard isSealedFrame(frame) else { return nil }
        return frame["v"]?.numEquals(ChatV4.version) == true ? ChatV4.version : ChatV3.version
    }

    public static func frameKeyId(_ frame: JSONObject) -> String {
        isSealedFrame(frame) ? (frame["kid"]?.stringValue ?? "") : ""
    }

    public static func wrappedKeyIds(_ frame: JSONObject) -> [String] {
        (frame["keys"]?.arrayValue ?? []).compactMap { $0.objectValue?["kid"]?.stringValue }
    }


    static func bundleMessage(
        _ room: String, _ kid: String, _ idk: String, _ acct: String, _ node: String, _ spk: JSONObject
    ) -> [UInt8] {
        ChatV3.lines([
            "forkmesh-chat-e2ee-bundle-v3", room, kid, idk, acct, node, spk["id"]?.stringValue ?? "",
            spk["x"]?.stringValue ?? "", spk["pq"]?.stringValue ?? "", String(ChatV3.int(spk["ts"]) ?? 0),
            String(ChatV3.int(spk["exp"]) ?? 0),
        ])
    }

    public static func bundle(
        room: String, acct: String, node: String, keyring: ChatKeyring, prekey: ChatPrekey
    ) throws -> JSONObject {
        let spk = prekey.json
        let signature = try keyring.sign(bundleMessage(room, keyring.kid, keyring.idk, acct, node, spk))
        return [
            "type": .string(keyType), "v": .int(ChatV3.version), "kid": .string(keyring.kid), "idk": .string(keyring.idk),
            "acct": .string(acct), "node": .string(node), "spk": .object(spk), "sig": .string(ChatV3.b64u(signature)),
        ]
    }

    public static func verifyBundle(_ frame: JSONObject, room: String, now: Int64) -> Bool {
        guard frame["type"] == .string(keyType), frame["v"]?.numEquals(ChatV3.version) == true,
            let kid = frame["kid"]?.stringValue, let idk = frame["idk"]?.stringValue,
            let acct = frame["acct"]?.stringValue, let node = frame["node"]?.stringValue,
            let spk = frame["spk"]?.objectValue, let idkBytes = ChatV3.unb64u(idk, length: Ed25519.publicKeyBytes),
            let x = ChatV3.bytes64u(spk["x"], X25519.keyBytes),
            let pq = ChatV3.bytes64u(spk["pq"], MlKem768.publicKeyBytes),
            let spkId = spk["id"]?.stringValue, let ts = ChatV3.int(spk["ts"]), let exp = ChatV3.int(spk["exp"]),
            let signature = ChatV3.bytes64u(frame["sig"], ChatV3.signatureBytes)
        else { return false }
        if kid != ChatV3.id16(idkBytes) || spkId != ChatV3.id16(x + pq) || !ChatV3.isValidAcct(acct)
            || !ChatV3.isValidAcct(node)
        {
            return false
        }
        if !(exp - ts > 0 && exp - ts <= ChatV3.maxBundleLifeMs) { return false }
        if ts > now + ChatV3.dayMs || exp <= now { return false }
        return Ed25519.verify(
            publicKey: idkBytes, message: bundleMessage(room, kid, idk, acct, node, spk), signature: signature)
    }

    public func keyFrame(now: Int64 = Date().epochMillis) -> JSONObject {
        keyring.rotateIfNeeded(now: now)
        guard let acct, let node, let prekey = keyring.newestPrekey(),
            let frame = try? RoomSeal.bundle(room: room, acct: acct, node: node, keyring: keyring, prekey: prekey)
        else { return [:] }
        _ = trust.accept(
            kid: keyring.kid, idk: keyring.idk, acct: acct, node: node, spk: prekey, policy: policy,
            ownKid: keyring.kid, now: now)
        locked { advertisedPrekey = prekey.id }
        return frame
    }

    public func needsAdvertisement(now: Int64 = Date().epochMillis) -> Bool {
        keyring.rotateIfNeeded(now: now)
        let newest = keyring.newestPrekey()?.id ?? ""
        return locked { !newest.isEmpty && newest != advertisedPrekey }
    }

    private var announcedStoreKey: String {
        "chat/e2ee/announcedV3/\(ChatV3.storeSuffix("\(scope)\n\(room)"))"
    }

    public func shouldPersistAdvertisement(now: Int64 = Date().epochMillis) -> Bool {
        keyring.rotateIfNeeded(now: now)
        let newest = keyring.newestPrekey()?.id ?? ""
        guard let store else { return true }
        let parts = (store.read(announcedStoreKey) ?? "").split(separator: " ", maxSplits: 1).map(String.init)
        let previous = Int64(parts.first ?? "") ?? 0
        let previousKey = parts.count > 1 ? parts[1] : ""
        if now - previous < RoomSeal.durableAnnouncementMs && previousKey == newest { return false }
        store.write(announcedStoreKey, "\(now) \(newest)")
        return true
    }

    @discardableResult
    public func acceptBundle(_ frame: JSONObject, now: Int64 = Date().epochMillis) -> ChatBundleResult? {
        guard RoomSeal.verifyBundle(frame, room: room, now: now), let spk = ChatPrekey.parse(frame["spk"]),
            let kid = frame["kid"]?.stringValue, let idk = frame["idk"]?.stringValue,
            let acct = frame["acct"]?.stringValue, let node = frame["node"]?.stringValue
        else { return nil }
        if let participants, !participants.contains(acct) { return nil }
        return trust.accept(
            kid: kid, idk: idk, acct: acct, node: node, spk: spk, policy: policy, ownKid: keyring.kid, now: now)
    }

    @discardableResult
    public func acceptKeyFrame(_ frame: JSONObject, now: Int64 = Date().epochMillis) -> Bool {
        guard let result = acceptBundle(frame, now: now) else { return false }
        return result.isNew || result.updated
    }

    @discardableResult
    public func approve(_ kid: String) -> Bool { trust.approve(kid) }

    @discardableResult
    public func block(_ kid: String) -> Bool { trust.block(kid) }


    private func contentAad(_ sender: String, _ counter: Int64, _ ts: Int64) -> [UInt8] {
        ChatV3.lines(["forkmesh-chat-e2ee-content-v3", room, sender, String(counter), String(ts)])
    }

    private func wrapAad(_ sender: String, _ recipient: String, _ nonce: String, _ tag: String) -> [UInt8] {
        ChatV3.lines(["forkmesh-chat-e2ee-wrap-v3", room, sender, recipient, nonce, tag])
    }

    func kek(
        sender: String, recipient: String, spkId: String, ephPub: [UInt8], ciphertext: [UInt8], ss1: [UInt8],
        ss2: [UInt8]
    ) -> [UInt8] {
        let info = ChatV3.lines([
            "forkmesh-chat-e2ee-kek-v3", room, sender, recipient, spkId, ChatV3.b64u(ephPub),
            ChatV3.b64u(Sha256.hash(ciphertext)),
        ])
        return Hkdf.sha256(ikm: ss1 + ss2, salt: salt, info: info, length: 32)
    }

    static func sealedMessage(_ room: String, _ frame: JSONObject) -> [UInt8]? {
        guard let body = ChatV3.bytes64(frame["body"]), let kid = frame["kid"]?.stringValue,
            let idk = frame["idk"]?.stringValue, let counter = ChatV3.int(frame["ctr"]), let ts = ChatV3.int(frame["ts"]),
            let nonce = frame["nonce"]?.stringValue, let tag = frame["tag"]?.stringValue
        else { return nil }
        var text = [
            "forkmesh-chat-e2ee-sealed-v3", room, kid, idk, String(counter), String(ts), nonce, tag,
            ChatV3.b64u(Sha256.hash(body)),
        ].joined(separator: "\n")
        for value in frame["keys"]?.arrayValue ?? [] {
            guard let entry = value.objectValue, let ct = ChatV3.bytes64(entry["ct"]) else { return nil }
            let fields = [
                entry["kid"]?.stringValue ?? "", entry["spk"]?.stringValue ?? "", entry["eph"]?.stringValue ?? "",
                ChatV3.b64u(Sha256.hash(ct)), entry["nonce"]?.stringValue ?? "", entry["tag"]?.stringValue ?? "",
                entry["key"]?.stringValue ?? "",
            ]
            text += "\n" + fields.joined(separator: "|")
        }
        return Array(text.utf8)
    }

    public func seal(_ plain: JSONObject, now: Int64 = Date().epochMillis) throws -> JSONObject {
        try sealTo(plain, only: nil, now: now)
    }

    public func seal(_ plain: JSONObject, for keyIds: [String], now: Int64 = Date().epochMillis) throws -> JSONObject {
        try sealTo(plain, only: Set(keyIds), now: now)
    }

    public func seal(_ plain: JSONObject, forNodes nodes: Set<String>, now: Int64 = Date().epochMillis) throws
        -> JSONObject
    {
        var kids = Set(trust.records(in: .approved).filter { nodes.contains($0.node) }.map(\.kid))
        if let acct { kids.formUnion(approvedKeyIds(acct: acct)) }
        return try sealTo(plain, only: kids, now: now)
    }

    private func sealTo(_ plain: JSONObject, only: Set<String>?, now: Int64) throws -> JSONObject {
        if version == ChatV4.version { return try sealV4(plain, only: only, now: now) }
        keyring.rotateIfNeeded(now: now)
        guard let own = keyring.newestPrekey() else { throw CryptoArgumentError("No chat prekey is available.") }
        var candidates: [ChatTrustRecord] = []
        for record in trust.records(in: .approved) where record.kid != keyring.kid {
            if let only, !only.contains(record.kid) { continue }
            if record.spk.exp > now { candidates.append(record) }
        }
        if candidates.count > ChatV3.maxRecipients - 1 {
            if policy == .strict { throw CryptoArgumentError("Too many chat recipients.") }
            candidates = Array(
                candidates.sorted { ($0.lastSeen, $0.firstSeen) > ($1.lastSeen, $1.firstSeen) }
                    .prefix(ChatV3.maxRecipients - 1))
        }
        var targets: [String: ChatPrekey] = [keyring.kid: own]
        for record in candidates { targets[record.kid] = record.spk }
        let counter = keyring.nextCounter(room)
        var content = CryptoBytes.random(32)
        defer { CryptoBytes.wipe(&content) }
        let body = try AesGcm.encrypt(
            Array(JSON.encode(plain).utf8), key: content, aad: contentAad(keyring.kid, counter, now))
        let nonceText = ChatV3.b64(body.nonce)
        let tagText = ChatV3.b64(body.mac)
        var entries: [JSONValue] = []
        for recipient in targets.keys.sorted(by: ChatV3.ascending) {
            guard let prekey = targets[recipient] else { continue }
            var ephPriv = X25519.generatePrivateKey()
            defer { CryptoBytes.wipe(&ephPriv) }
            let ephPub = try X25519.publicKey(privateKey: ephPriv)
            var ss1 = try X25519.sharedSecret(privateKey: ephPriv, remotePublicKey: prekey.x)
            defer { CryptoBytes.wipe(&ss1) }
            if CryptoBytes.isAllZero(ss1) { throw CryptoArgumentError("Invalid X25519 shared secret.") }
            let encapsulated = try MlKem768.encapsulate(publicKey: prekey.pq)
            var ss2 = encapsulated.sharedSecret
            defer { CryptoBytes.wipe(&ss2) }
            var wrapKey = kek(
                sender: keyring.kid, recipient: recipient, spkId: prekey.id, ephPub: ephPub,
                ciphertext: encapsulated.ciphertext, ss1: ss1, ss2: ss2)
            defer { CryptoBytes.wipe(&wrapKey) }
            let wrapped = try AesGcm.encrypt(content, key: wrapKey, aad: wrapAad(keyring.kid, recipient, nonceText, tagText))
            entries.append(
                .object([
                    "kid": .string(recipient), "spk": .string(prekey.id), "eph": .string(ChatV3.b64u(ephPub)),
                    "ct": .string(ChatV3.b64(encapsulated.ciphertext)), "nonce": .string(ChatV3.b64(wrapped.nonce)),
                    "tag": .string(ChatV3.b64(wrapped.mac)), "key": .string(ChatV3.b64(wrapped.cipherText)),
                ]))
        }
        var frame: JSONObject = [
            "type": .string(RoomSeal.sealedType), "v": .int(ChatV3.version), "kid": .string(keyring.kid),
            "idk": .string(keyring.idk), "ctr": .int(counter), "ts": .int(now), "nonce": .string(nonceText),
            "tag": .string(tagText), "body": .string(ChatV3.b64(body.cipherText)), "keys": .array(entries),
        ]
        guard let message = RoomSeal.sealedMessage(room, frame) else { throw CryptoArgumentError("Unsignable frame.") }
        frame["sig"] = .string(ChatV3.b64u(try keyring.sign(message)))
        return frame
    }


    static func validShape(_ frame: JSONObject) -> Bool {
        guard frame["type"] == .string(sealedType), frame["v"]?.numEquals(ChatV3.version) == true,
            let kid = frame["kid"]?.stringValue, let idk = ChatV3.bytes64u(frame["idk"], Ed25519.publicKeyBytes),
            kid == ChatV3.id16(idk), let keys = frame["keys"]?.arrayValue, !keys.isEmpty,
            keys.count <= ChatV3.maxRecipients, let counter = ChatV3.int(frame["ctr"]), counter >= 1,
            ChatV3.int(frame["ts"]) != nil, ChatV3.bytes64(frame["nonce"], ChatV3.nonceBytes) != nil,
            ChatV3.bytes64(frame["tag"], ChatV3.tagBytes) != nil, ChatV3.bytes64(frame["body"]) != nil,
            ChatV3.bytes64u(frame["sig"], ChatV3.signatureBytes) != nil
        else { return false }
        var previous: String?
        for value in keys {
            guard let entry = value.objectValue, let recipient = entry["kid"]?.stringValue, ChatV3.isKeyId(recipient),
                let spk = entry["spk"]?.stringValue, ChatV3.isKeyId(spk),
                ChatV3.bytes64u(entry["eph"], X25519.keyBytes) != nil,
                ChatV3.bytes64(entry["ct"], MlKem768.ciphertextBytes) != nil,
                ChatV3.bytes64(entry["nonce"], ChatV3.nonceBytes) != nil,
                ChatV3.bytes64(entry["tag"], ChatV3.tagBytes) != nil, ChatV3.bytes64(entry["key"], 32) != nil
            else { return false }
            if let previous, !ChatV3.ascending(previous, recipient) { return false }
            previous = recipient
        }
        return true
    }

    public func open(
        _ frame: JSONObject, now: Int64 = Date().epochMillis, history: Bool = false, commit: Bool = true
    ) -> RoomSealOpen {
        if version == ChatV4.version { return openV4(frame, now: now, commit: commit) }
        return open(frame, now: now, hold: commit, record: commit, history: history || !commit)
    }

    public func peek(_ frame: JSONObject, now: Int64 = Date().epochMillis) -> RoomSealOpen {
        open(frame, now: now, commit: false)
    }

    public func highestCounter(from kid: String) -> Int64 { keyring.highestSeen(room, kid) }

    public func resetSessions(with kid: String) {
        keyring.resetRatchet(room, kid)
    }

    private func open(_ frame: JSONObject, now: Int64, hold: Bool, record: Bool, history: Bool) -> RoomSealOpen {
        guard RoomSeal.validShape(frame), let kid = frame["kid"]?.stringValue, let idk = frame["idk"]?.stringValue,
            let idkBytes = ChatV3.bytes64u(frame["idk"], Ed25519.publicKeyBytes),
            let counter = ChatV3.int(frame["ctr"]), let ts = ChatV3.int(frame["ts"]),
            let signature = ChatV3.bytes64u(frame["sig"], ChatV3.signatureBytes)
        else { return .invalid }
        let senderIdk: String
        let senderAcct: String?
        let senderNode: String?
        if kid == keyring.kid {
            senderIdk = keyring.idk
            senderAcct = acct
            senderNode = node
        } else {
            guard let record = trust.record(kid), record.state == .approved else {
                if hold && trust.record(kid)?.state != .blocked
                    && RoomSeal.wrappedKeyIds(frame).contains(keyring.kid)
                {
                    trust.hold(room, frame)
                }
                return .held(kid)
            }
            senderIdk = record.idk
            senderAcct = record.acct
            senderNode = record.node
        }
        if senderIdk != idk { return .invalid }
        guard let message = RoomSeal.sealedMessage(room, frame),
            Ed25519.verify(publicKey: idkBytes, message: message, signature: signature)
        else { return .invalid }
        if record && !history && keyring.isReplay(room, kid, counter) { return .replay }
        if ts < now - ChatV3.prekeyLifeMs || ts > now + ChatV3.dayMs { return .invalid }
        guard let entry = (frame["keys"]?.arrayValue ?? []).compactMap(\.objectValue)
            .first(where: { $0["kid"]?.stringValue == keyring.kid })
        else { return .notForMe }
        guard let spkId = entry["spk"]?.stringValue, let secret = keyring.prekeySecret(spkId) else { return .notForMe }
        guard let ct = ChatV3.bytes64(entry["ct"], MlKem768.ciphertextBytes),
            let ephPub = ChatV3.bytes64u(entry["eph"], X25519.keyBytes),
            let keyNonce = ChatV3.bytes64(entry["nonce"], ChatV3.nonceBytes),
            let keyTag = ChatV3.bytes64(entry["tag"], ChatV3.tagBytes), let wrapped = ChatV3.bytes64(entry["key"], 32),
            let nonceText = frame["nonce"]?.stringValue, let tagText = frame["tag"]?.stringValue,
            let bodyNonce = ChatV3.bytes64(frame["nonce"], ChatV3.nonceBytes),
            let bodyTag = ChatV3.bytes64(frame["tag"], ChatV3.tagBytes), let body = ChatV3.bytes64(frame["body"])
        else { return .invalid }
        guard var ss1 = try? X25519.sharedSecret(privateKey: secret.xPriv, remotePublicKey: ephPub) else { return .invalid }
        defer { CryptoBytes.wipe(&ss1) }
        if CryptoBytes.isAllZero(ss1) { return .invalid }
        guard var ss2 = try? MlKem768.decapsulate(secretKey: secret.kem, ciphertext: ct) else { return .invalid }
        defer { CryptoBytes.wipe(&ss2) }
        var wrapKey = kek(sender: kid, recipient: keyring.kid, spkId: spkId, ephPub: ephPub, ciphertext: ct, ss1: ss1, ss2: ss2)
        defer { CryptoBytes.wipe(&wrapKey) }
        guard
            var content = AesGcm.decryptOrNull(
                cipherText: wrapped, nonce: keyNonce, mac: keyTag, key: wrapKey,
                aad: wrapAad(kid, keyring.kid, nonceText, tagText)), content.count == 32
        else { return .invalid }
        defer { CryptoBytes.wipe(&content) }
        guard
            let clear = AesGcm.decryptOrNull(
                cipherText: body, nonce: bodyNonce, mac: bodyTag, key: content, aad: contentAad(kid, counter, ts)),
            let plain = CryptoText.jsonObject(clear)
        else { return .invalid }
        if let senderNode, let claimed = plain["senderId"], claimed.stringValue != senderNode { return .invalid }
        if record {
            let fresh = keyring.recordSeen(room, kid, counter)
            if !fresh && !history { return .replay }
            trust.noteSeen(kid, now)
        }
        let resolvedNode = senderNode ?? plain["senderId"]?.stringValue ?? ""
        return .opened(plain: plain, senderKid: kid, senderAcct: senderAcct ?? "", senderNode: resolvedNode)
    }

    public func unseal(_ frame: JSONObject, now: Int64 = Date().epochMillis) -> JSONObject? {
        peek(frame, now: now).plain
    }

    public func reopenHeld(now: Int64 = Date().epochMillis) -> [RoomSealOpen] {
        var results: [RoomSealOpen] = []
        for frame in trust.takeHeld(room) {
            let result =
                version == ChatV4.version
                ? openV4(frame, now: now, commit: true) : open(frame, now: now, hold: true, record: true, history: true)
            if case .held = result { continue }
            results.append(result)
        }
        return results
    }

    public func safetyNumber() -> ChatSafetyNumber { trust.safetyNumber() }

    private func locked<R>(_ body: () throws -> R) rethrows -> R {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}
