import FMBase
import Foundation

struct ChatV4Entry {
    let kid: String
    let sid: String
    let spk: String
    let ct: [UInt8]
    let dh: String
    let dhBytes: [UInt8]
    let pn: Int64
    let n: Int64
    let nonce: String
    let tag: String
    let key: String

    static func parse(_ value: JSONValue) -> ChatV4Entry? {
        guard let entry = value.objectValue, let kid = entry["kid"]?.stringValue, ChatV3.isKeyId(kid),
            let sid = entry["sid"]?.stringValue, ChatV3.isKeyId(sid), let spk = entry["spk"]?.stringValue,
            ChatV3.isKeyId(spk), let ct = ChatV3.bytes64(entry["ct"], MlKem768.ciphertextBytes),
            let dh = entry["dh"]?.stringValue, dh.count == 43, let dhBytes = ChatV3.unb64u(dh, length: X25519.keyBytes),
            let pn = ChatV4.isCount(entry["pn"]), let n = ChatV4.isCount(entry["n"]),
            let nonce = entry["nonce"]?.stringValue, ChatV3.unb64(nonce, length: ChatV3.nonceBytes) != nil,
            let tag = entry["tag"]?.stringValue, ChatV3.unb64(tag, length: ChatV3.tagBytes) != nil,
            let key = entry["key"]?.stringValue, ChatV3.unb64(key, length: 32) != nil
        else { return nil }
        return ChatV4Entry(
            kid: kid, sid: sid, spk: spk, ct: ct, dh: dh, dhBytes: dhBytes, pn: pn, n: n, nonce: nonce, tag: tag, key: key)
    }

    var line: String {
        [kid, sid, spk, ChatV3.b64u(Sha256.hash(ct)), dh, String(pn), String(n), nonce, tag, key].joined(separator: "|")
    }
}

enum ChatRatchetFailure: Error {
    case noSession
    case replay
}

extension RoomSeal {
    func contentAadV4(_ sender: String, _ counter: Int64, _ ts: Int64) -> [UInt8] {
        ChatV3.lines(["forkmesh-chat-e2ee-content-v4", room, sender, String(counter), String(ts)])
    }

    static func wrapAadV4(
        _ room: String, sender: String, recipient: String, sid: String, spk: String, ct: [UInt8], dh: String, pn: Int64,
        n: Int64, bodyNonce: String, bodyTag: String
    ) -> [UInt8] {
        ChatV3.lines([
            "forkmesh-chat-e2ee-wrap-v4", room, sender, recipient, sid, spk, ChatV3.b64u(Sha256.hash(ct)), dh, String(pn),
            String(n), bodyNonce, bodyTag,
        ])
    }

    static func sealedMessageV4(_ room: String, _ frame: JSONObject, _ entries: [ChatV4Entry]) -> [UInt8]? {
        guard let body = ChatV3.bytes64(frame["body"]), let kid = frame["kid"]?.stringValue,
            let idk = frame["idk"]?.stringValue, let counter = ChatV3.int(frame["ctr"]), let ts = ChatV3.int(frame["ts"]),
            let nonce = frame["nonce"]?.stringValue, let tag = frame["tag"]?.stringValue
        else { return nil }
        var text = [
            "forkmesh-chat-e2ee-sealed-v4", room, kid, idk, String(counter), String(ts), nonce, tag,
            ChatV3.b64u(Sha256.hash(body)),
        ].joined(separator: "\n")
        for entry in entries { text += "\n" + entry.line }
        return Array(text.utf8)
    }

    static func shapeV4(_ frame: JSONObject) -> [ChatV4Entry]? {
        guard frame["type"] == .string(sealedType), frame["v"]?.numEquals(ChatV4.version) == true,
            let kid = frame["kid"]?.stringValue, let idk = ChatV3.bytes64u(frame["idk"], Ed25519.publicKeyBytes),
            kid == ChatV3.id16(idk), let keys = frame["keys"]?.arrayValue, keys.count <= ChatV3.maxRecipients,
            let counter = ChatV4.isCount(frame["ctr"]), counter >= 1, ChatV3.int(frame["ts"]) != nil,
            ChatV3.bytes64(frame["nonce"], ChatV3.nonceBytes) != nil, ChatV3.bytes64(frame["tag"], ChatV3.tagBytes) != nil,
            ChatV3.bytes64(frame["body"]) != nil, ChatV3.bytes64u(frame["sig"], ChatV3.signatureBytes) != nil
        else { return nil }
        var entries: [ChatV4Entry] = []
        for value in keys {
            guard let entry = ChatV4Entry.parse(value), entry.kid != kid else { return nil }
            if let previous = entries.last, !ChatV3.ascending(previous.kid, entry.kid) { return nil }
            entries.append(entry)
        }
        return entries
    }

    func sealV4(_ plain: JSONObject, only: Set<String>?, now: Int64) throws -> JSONObject {
        keyring.rotateIfNeeded(now: now)
        var targets: [String: ChatPrekey] = [:]
        for record in trust.records(in: .approved) where record.kid != keyring.kid {
            if let only, !only.contains(record.kid) { continue }
            if record.spk.exp > now { targets[record.kid] = record.spk }
        }
        if targets.count > ChatV3.maxRecipients { throw CryptoArgumentError("Too many chat recipients.") }
        let counter = keyring.nextCounter(room)
        var content = CryptoBytes.random(32)
        defer { CryptoBytes.wipe(&content) }
        let body = try AesGcm.encrypt(
            Array(JSON.encode(plain).utf8), key: content, aad: contentAadV4(keyring.kid, counter, now))
        let nonceText = ChatV3.b64(body.nonce)
        let tagText = ChatV3.b64(body.mac)
        return try keyring.ratchetLocked {
            var peers: [String: ChatRatchetPeer] = [:]
            var values: [JSONValue] = []
            var entries: [ChatV4Entry] = []
            for recipient in targets.keys.sorted(by: ChatV3.ascending) {
                guard let prekey = targets[recipient] else { continue }
                var peer = peers[recipient] ?? keyring.ratchetPeer(room, recipient)
                let step = try sendStep(&peer, recipient: recipient, prekey: prekey, now: now)
                peers[recipient] = peer
                var mk = step.mk
                defer { CryptoBytes.wipe(&mk) }
                let wrapped = try AesGcm.encrypt(
                    content, key: mk,
                    aad: RoomSeal.wrapAadV4(
                        room, sender: keyring.kid, recipient: recipient, sid: step.sid, spk: step.spk, ct: step.ct,
                        dh: step.dh, pn: step.pn, n: step.n, bodyNonce: nonceText, bodyTag: tagText))
                let entry = ChatV4Entry(
                    kid: recipient, sid: step.sid, spk: step.spk, ct: step.ct, dh: step.dh, dhBytes: [], pn: step.pn,
                    n: step.n, nonce: ChatV3.b64(wrapped.nonce), tag: ChatV3.b64(wrapped.mac),
                    key: ChatV3.b64(wrapped.cipherText))
                entries.append(entry)
                values.append(
                    .object([
                        "kid": .string(recipient), "sid": .string(step.sid), "spk": .string(step.spk),
                        "ct": .string(ChatV3.b64(step.ct)), "dh": .string(step.dh), "pn": .int(step.pn), "n": .int(step.n),
                        "nonce": .string(entry.nonce), "tag": .string(entry.tag), "key": .string(entry.key),
                    ]))
            }
            var frame: JSONObject = [
                "type": .string(RoomSeal.sealedType), "v": .int(ChatV4.version), "kid": .string(keyring.kid),
                "idk": .string(keyring.idk), "ctr": .int(counter), "ts": .int(now), "nonce": .string(nonceText),
                "tag": .string(tagText), "body": .string(ChatV3.b64(body.cipherText)), "keys": .array(values),
            ]
            guard let message = RoomSeal.sealedMessageV4(room, frame, entries) else {
                throw CryptoArgumentError("Unsignable frame.")
            }
            frame["sig"] = .string(ChatV3.b64u(try keyring.sign(message)))
            keyring.storeRatchetPeers(room, peers, now: now)
            return frame
        }
    }

    private func sendStep(_ peer: inout ChatRatchetPeer, recipient: String, prekey: ChatPrekey, now: Int64) throws
        -> (sid: String, spk: String, ct: [UInt8], dh: String, pn: Int64, n: Int64, mk: [UInt8])
    {
        var index = peer.sessions.firstIndex { $0.sid == peer.current }
        if let found = index, peer.sessions[found].cks == nil, peer.sessions[found].dhr == nil { index = nil }
        if index == nil {
            var eph = X25519.generatePrivateKey()
            defer { CryptoBytes.wipe(&eph) }
            let ephPub = try X25519.publicKey(privateKey: eph)
            var ss1 = try X25519.sharedSecret(privateKey: eph, remotePublicKey: prekey.x)
            defer { CryptoBytes.wipe(&ss1) }
            if CryptoBytes.isAllZero(ss1) { throw CryptoArgumentError("Invalid X25519 shared secret.") }
            let encapsulated = try MlKem768.encapsulate(publicKey: prekey.pq)
            var ss2 = encapsulated.sharedSecret
            defer { CryptoBytes.wipe(&ss2) }
            let sid = ChatV4.sessionId(dh: ephPub, ct: encapsulated.ciphertext)
            let derived = ChatV4.kdfInit(
                room: room, sender: keyring.kid, recipient: recipient, spk: prekey.id, sid: sid, ss1: ss1, ss2: ss2)
            peer.sessions.append(
                ChatRatchetSession(
                    sid: sid, rk: derived.rk, dhsPriv: eph, dhsPub: ChatV3.b64u(ephPub), dhr: nil, cks: derived.ck, ns: 0,
                    pn: 0, sqSpk: prekey.id, sqCt: encapsulated.ciphertext, ckr: nil, nr: 0, skipped: [], created: now,
                    used: now))
            peer.current = sid
            index = peer.sessions.count - 1
        } else if let found = index, peer.sessions[found].cks == nil, let dhr = peer.sessions[found].dhr {
            guard let remote = ChatV3.unb64u(dhr, length: X25519.keyBytes) else {
                throw CryptoArgumentError("Invalid ratchet key.")
            }
            var dhs = X25519.generatePrivateKey()
            defer { CryptoBytes.wipe(&dhs) }
            let dhsPub = try X25519.publicKey(privateKey: dhs)
            let encapsulated = try MlKem768.encapsulate(publicKey: prekey.pq)
            var dhOut = try X25519.sharedSecret(privateKey: dhs, remotePublicKey: remote)
            defer { CryptoBytes.wipe(&dhOut) }
            if CryptoBytes.isAllZero(dhOut) { throw CryptoArgumentError("Invalid X25519 shared secret.") }
            let derived = ChatV4.kdfRk(peer.sessions[found].rk, dhOut: dhOut, ss: encapsulated.sharedSecret)
            peer.sessions[found].rk = derived.rk
            peer.sessions[found].cks = derived.ck
            peer.sessions[found].dhsPriv = dhs
            peer.sessions[found].dhsPub = ChatV3.b64u(dhsPub)
            peer.sessions[found].sqSpk = prekey.id
            peer.sessions[found].sqCt = encapsulated.ciphertext
        }
        guard let at = index, let cks = peer.sessions[at].cks, let spk = peer.sessions[at].sqSpk,
            let ct = peer.sessions[at].sqCt, let dh = peer.sessions[at].dhsPub
        else { throw CryptoArgumentError("No ratchet session.") }
        let chain = ChatV4.kdfCk(cks)
        let session = peer.sessions[at]
        peer.sessions[at].cks = chain.next
        peer.sessions[at].ns += 1
        peer.sessions[at].used = now
        return (session.sid, spk, ct, dh, session.pn, session.ns, chain.mk)
    }

    func openV4(_ frame: JSONObject, now: Int64, commit: Bool) -> RoomSealOpen {
        guard let entries = RoomSeal.shapeV4(frame), let kid = frame["kid"]?.stringValue,
            let idk = frame["idk"]?.stringValue, let idkBytes = ChatV3.bytes64u(frame["idk"], Ed25519.publicKeyBytes),
            let counter = ChatV3.int(frame["ctr"]), let ts = ChatV3.int(frame["ts"]),
            let signature = ChatV3.bytes64u(frame["sig"], ChatV3.signatureBytes)
        else { return .invalid }
        if kid == keyring.kid { return .own }
        guard let record = trust.record(kid), record.state == .approved else {
            if commit && trust.record(kid)?.state != .blocked && entries.contains(where: { $0.kid == keyring.kid }) {
                trust.hold(room, frame)
            }
            return .held(kid)
        }
        if record.idk != idk { return .invalid }
        guard let message = RoomSeal.sealedMessageV4(room, frame, entries),
            Ed25519.verify(publicKey: idkBytes, message: message, signature: signature)
        else { return .invalid }
        return keyring.ratchetLocked {
            if keyring.isReplay(room, kid, counter) { return .replay }
            if ts < now - ChatV3.prekeyLifeMs || ts > now + ChatV3.dayMs { return .invalid }
            guard let entry = entries.first(where: { $0.kid == keyring.kid }) else { return .notForMe }
            guard let nonceText = frame["nonce"]?.stringValue, let tagText = frame["tag"]?.stringValue,
                let bodyNonce = ChatV3.bytes64(frame["nonce"], ChatV3.nonceBytes),
                let bodyTag = ChatV3.bytes64(frame["tag"], ChatV3.tagBytes), let body = ChatV3.bytes64(frame["body"]),
                let keyNonce = ChatV3.unb64(entry.nonce, length: ChatV3.nonceBytes),
                let keyTag = ChatV3.unb64(entry.tag, length: ChatV3.tagBytes),
                let wrapped = ChatV3.unb64(entry.key, length: 32)
            else { return .invalid }
            var peer = keyring.ratchetPeer(room, kid)
            var mk: [UInt8]
            do {
                mk = try receiveStep(&peer, sender: kid, entry: entry, now: now)
            } catch ChatRatchetFailure.replay {
                return .replay
            } catch {
                return .noSession(kid)
            }
            defer { CryptoBytes.wipe(&mk) }
            let aad = RoomSeal.wrapAadV4(
                room, sender: kid, recipient: keyring.kid, sid: entry.sid, spk: entry.spk, ct: entry.ct, dh: entry.dh,
                pn: entry.pn, n: entry.n, bodyNonce: nonceText, bodyTag: tagText)
            guard
                var content = AesGcm.decryptOrNull(
                    cipherText: wrapped, nonce: keyNonce, mac: keyTag, key: mk, aad: aad), content.count == 32
            else { return .noSession(kid) }
            defer { CryptoBytes.wipe(&content) }
            guard
                let clear = AesGcm.decryptOrNull(
                    cipherText: body, nonce: bodyNonce, mac: bodyTag, key: content, aad: contentAadV4(kid, counter, ts)),
                let plain = CryptoText.jsonObject(clear)
            else { return .invalid }
            if let claimed = plain["senderId"], claimed.stringValue != record.node { return .invalid }
            if commit {
                keyring.storeRatchetPeers(room, [kid: peer], now: now)
                _ = keyring.recordSeen(room, kid, counter)
                trust.noteSeen(kid, now)
            }
            return .opened(plain: plain, senderKid: kid, senderAcct: record.acct, senderNode: record.node)
        }
    }

    private func receiveStep(_ peer: inout ChatRatchetPeer, sender: String, entry: ChatV4Entry, now: Int64) throws
        -> [UInt8]
    {
        var index = peer.sessions.firstIndex { $0.sid == entry.sid }
        if index == nil {
            if entry.sid != ChatV4.sessionId(dh: entry.dhBytes, ct: entry.ct) || entry.pn != 0 {
                throw ChatRatchetFailure.noSession
            }
            guard let secret = keyring.prekeySecret(entry.spk) else { throw ChatRatchetFailure.noSession }
            var ss1 = try X25519.sharedSecret(privateKey: secret.xPriv, remotePublicKey: entry.dhBytes)
            defer { CryptoBytes.wipe(&ss1) }
            if CryptoBytes.isAllZero(ss1) { throw ChatRatchetFailure.noSession }
            var ss2 = try MlKem768.decapsulate(secretKey: secret.kem, ciphertext: entry.ct)
            defer { CryptoBytes.wipe(&ss2) }
            let derived = ChatV4.kdfInit(
                room: room, sender: sender, recipient: keyring.kid, spk: entry.spk, sid: entry.sid, ss1: ss1, ss2: ss2)
            peer.sessions.append(
                ChatRatchetSession(
                    sid: entry.sid, rk: derived.rk, dhsPriv: nil, dhsPub: nil, dhr: entry.dh, cks: nil, ns: 0, pn: 0,
                    sqSpk: nil, sqCt: nil, ckr: derived.ck, nr: 0, skipped: [], created: now, used: now))
            index = peer.sessions.count - 1
        }
        guard let at = index else { throw ChatRatchetFailure.noSession }
        var session = peer.sessions[at]
        let mk = try ratchetReceive(&session, entry: entry, now: now)
        session.used = now
        peer.sessions[at] = session
        peer.current = session.sid
        return mk
    }

    private func ratchetReceive(_ session: inout ChatRatchetSession, entry: ChatV4Entry, now: Int64) throws -> [UInt8] {
        if let found = session.skipped.firstIndex(where: { $0.dh == entry.dh && $0.n == entry.n }) {
            return session.skipped.remove(at: found).mk
        }
        if entry.dh != session.dhr {
            guard let dhs = session.dhsPriv else { throw ChatRatchetFailure.noSession }
            if session.ckr != nil { try skip(&session, until: entry.pn, now: now) }
            guard let secret = keyring.prekeySecret(entry.spk) else { throw ChatRatchetFailure.noSession }
            var ss = try MlKem768.decapsulate(secretKey: secret.kem, ciphertext: entry.ct)
            defer { CryptoBytes.wipe(&ss) }
            var dhOut = try X25519.sharedSecret(privateKey: dhs, remotePublicKey: entry.dhBytes)
            defer { CryptoBytes.wipe(&dhOut) }
            if CryptoBytes.isAllZero(dhOut) { throw ChatRatchetFailure.noSession }
            let derived = ChatV4.kdfRk(session.rk, dhOut: dhOut, ss: ss)
            session.rk = derived.rk
            session.ckr = derived.ck
            session.nr = 0
            session.dhr = entry.dh
            session.pn = session.ns
            session.ns = 0
            session.cks = nil
            session.sqSpk = nil
            session.sqCt = nil
        }
        if session.ckr == nil { throw ChatRatchetFailure.noSession }
        try skip(&session, until: entry.n, now: now)
        guard let ckr = session.ckr else { throw ChatRatchetFailure.noSession }
        let chain = ChatV4.kdfCk(ckr)
        session.ckr = chain.next
        session.nr += 1
        return chain.mk
    }

    private func skip(_ session: inout ChatRatchetSession, until: Int64, now: Int64) throws {
        if until < session.nr { throw ChatRatchetFailure.replay }
        if until - session.nr > ChatV4.maxSkip { throw ChatRatchetFailure.noSession }
        guard var ck = session.ckr, let dhr = session.dhr else { throw ChatRatchetFailure.noSession }
        while session.nr < until {
            let chain = ChatV4.kdfCk(ck)
            session.skipped.append(ChatRatchetSkipped(dh: dhr, n: session.nr, mk: chain.mk, ts: now))
            ck = chain.next
            session.nr += 1
        }
        session.ckr = ck
    }
}
