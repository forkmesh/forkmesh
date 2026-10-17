import FMBase
import Foundation

public enum ChatV4 {
    public static let version: Int64 = 4
    public static let maxSkip: Int64 = 256
    public static let skippedKept = 512
    public static let skippedLifeMs: Int64 = 8 * ChatV3.dayMs
    public static let sessionsKept = 4
    public static let resetType = "e2ee-reset"
    public static let maxCount: Int64 = 1 << 31

    public static func kdfInit(
        room: String, sender: String, recipient: String, spk: String, sid: String, ss1: [UInt8], ss2: [UInt8]
    ) -> (rk: [UInt8], ck: [UInt8]) {
        let salt = Sha256.hash(Array("ForkMesh chat e2ee salt v4:\(room)".utf8))
        let info = ChatV3.lines(["forkmesh-chat-e2ee-init-v4", room, sender, recipient, spk, sid])
        let out = Hkdf.sha256(ikm: ss1 + ss2, salt: salt, info: info, length: 64)
        return (Array(out[0..<32]), Array(out[32..<64]))
    }

    public static func kdfRk(_ rk: [UInt8], dhOut: [UInt8], ss: [UInt8]) -> (rk: [UInt8], ck: [UInt8]) {
        let out = Hkdf.sha256(ikm: dhOut + ss, salt: rk, info: Array("forkmesh-chat-e2ee-ratchet-v4".utf8), length: 64)
        return (Array(out[0..<32]), Array(out[32..<64]))
    }

    public static func kdfCk(_ ck: [UInt8]) -> (mk: [UInt8], next: [UInt8]) {
        (HmacSha256.mac(key: ck, [0x01]), HmacSha256.mac(key: ck, [0x02]))
    }

    public static func sessionId(dh: [UInt8], ct: [UInt8]) -> String { ChatV3.id16(dh + ct) }

    static func isCount(_ value: JSONValue?) -> Int64? {
        guard let number = ChatV3.int(value), number >= 0, number < maxCount else { return nil }
        return number
    }

    static func storeKey(_ room: String) -> String { "chat/e2ee/ratchetV4/\(ChatV3.storeSuffix(room))" }
}

public struct ChatRatchetSkipped: Equatable, Sendable {
    public internal(set) var dh: String
    public internal(set) var n: Int64
    var mk: [UInt8]
    public internal(set) var ts: Int64

    var json: JSONObject {
        ["dh": .string(dh), "n": .int(n), "mk": .string(ChatV3.b64(mk)), "ts": .int(ts)]
    }

    static func parse(_ value: JSONValue) -> ChatRatchetSkipped? {
        guard let object = value.objectValue, let dh = object["dh"]?.stringValue, let n = ChatV3.int(object["n"]),
            let mk = ChatV3.bytes64(object["mk"], 32), let ts = ChatV3.int(object["ts"])
        else { return nil }
        return ChatRatchetSkipped(dh: dh, n: n, mk: mk, ts: ts)
    }
}

public struct ChatRatchetSession: Equatable, Sendable {
    public internal(set) var sid: String
    var rk: [UInt8]
    var dhsPriv: [UInt8]?
    public internal(set) var dhsPub: String?
    public internal(set) var dhr: String?
    var cks: [UInt8]?
    public internal(set) var ns: Int64
    public internal(set) var pn: Int64
    public internal(set) var sqSpk: String?
    var sqCt: [UInt8]?
    var ckr: [UInt8]?
    public internal(set) var nr: Int64
    public internal(set) var skipped: [ChatRatchetSkipped]
    public internal(set) var created: Int64
    public internal(set) var used: Int64

    var json: JSONObject {
        var dhs: JSONValue = .null
        if let dhsPriv, let dhsPub { dhs = .object(["priv": .string(ChatV3.b64(dhsPriv)), "pub": .string(dhsPub)]) }
        var sq: JSONValue = .null
        if let sqSpk, let sqCt { sq = .object(["spk": .string(sqSpk), "ct": .string(ChatV3.b64(sqCt))]) }
        return [
            "sid": .string(sid), "rk": .string(ChatV3.b64(rk)), "dhs": dhs, "dhr": dhr.map { .string($0) } ?? .null,
            "cks": cks.map { .string(ChatV3.b64($0)) } ?? .null, "ns": .int(ns), "pn": .int(pn), "sq": sq,
            "ckr": ckr.map { .string(ChatV3.b64($0)) } ?? .null, "nr": .int(nr),
            "skipped": .array(skipped.map { .object($0.json) }), "created": .int(created), "used": .int(used),
        ]
    }

    static func optionalKey(_ value: JSONValue?, _ ok: inout Bool) -> [UInt8]? {
        guard let value, value != .null else { return nil }
        guard let bytes = ChatV3.bytes64(value, 32) else {
            ok = false
            return nil
        }
        return bytes
    }

    static func parse(_ value: JSONValue) -> ChatRatchetSession? {
        guard let object = value.objectValue, let sid = object["sid"]?.stringValue,
            let rk = ChatV3.bytes64(object["rk"], 32), let ns = ChatV3.int(object["ns"]), let pn = ChatV3.int(object["pn"]),
            let nr = ChatV3.int(object["nr"])
        else { return nil }
        var ok = true
        var dhsPriv: [UInt8]?
        var dhsPub: String?
        if let dhs = object["dhs"], dhs != .null {
            guard let priv = ChatV3.bytes64(dhs["priv"], X25519.keyBytes), let pub = dhs["pub"]?.stringValue else {
                return nil
            }
            dhsPriv = priv
            dhsPub = pub
        }
        var sqSpk: String?
        var sqCt: [UInt8]?
        if let sq = object["sq"], sq != .null {
            guard let spk = sq["spk"]?.stringValue, let ct = ChatV3.bytes64(sq["ct"], MlKem768.ciphertextBytes) else {
                return nil
            }
            sqSpk = spk
            sqCt = ct
        }
        var dhr: String?
        if let value = object["dhr"], value != .null {
            guard let text = value.stringValue else { return nil }
            dhr = text
        }
        let cks = optionalKey(object["cks"], &ok)
        let ckr = optionalKey(object["ckr"], &ok)
        if !ok { return nil }
        return ChatRatchetSession(
            sid: sid, rk: rk, dhsPriv: dhsPriv, dhsPub: dhsPub, dhr: dhr, cks: cks, ns: ns, pn: pn, sqSpk: sqSpk,
            sqCt: sqCt, ckr: ckr, nr: nr,
            skipped: (object["skipped"]?.arrayValue ?? []).compactMap(ChatRatchetSkipped.parse),
            created: ChatV3.int(object["created"]) ?? 0, used: ChatV3.int(object["used"]) ?? 0)
    }
}

public struct ChatRatchetPeer: Equatable, Sendable {
    public internal(set) var current: String?
    public internal(set) var sessions: [ChatRatchetSession]

    public init(current: String? = nil, sessions: [ChatRatchetSession] = []) {
        self.current = current
        self.sessions = sessions
    }

    var json: JSONObject {
        ["current": current.map { .string($0) } ?? .null, "sessions": .array(sessions.map { .object($0.json) })]
    }

    static func parse(_ value: JSONValue) -> ChatRatchetPeer? {
        guard let object = value.objectValue else { return nil }
        return ChatRatchetPeer(
            current: object["current"]?.stringValue,
            sessions: (object["sessions"]?.arrayValue ?? []).compactMap(ChatRatchetSession.parse))
    }

    mutating func trim(now: Int64) {
        let ordered = sessions.enumerated().sorted { a, b in
            a.element.used != b.element.used ? a.element.used > b.element.used : a.offset < b.offset
        }.map(\.element)
        let current = ordered.first { $0.sid == self.current }
        var kept: [ChatRatchetSession] = current.map { [$0] } ?? []
        var others = ordered
        if let current, let index = others.firstIndex(where: { $0.sid == current.sid }) { others.remove(at: index) }
        kept += others.prefix(ChatV4.sessionsKept - kept.count)
        for index in kept.indices {
            let fresh = kept[index].skipped.filter { $0.ts >= now - ChatV4.skippedLifeMs }
            kept[index].skipped = Array(fresh.suffix(ChatV4.skippedKept))
        }
        sessions = kept
    }
}

enum ChatRatchetBook {
    static func encode(_ peers: [String: ChatRatchetPeer]) -> JSONObject {
        var out = JSONObject()
        for kid in peers.keys.sorted(by: ChatV3.ascending) {
            if let peer = peers[kid] { out[kid] = .object(peer.json) }
        }
        return ["v": .int(ChatV4.version), "peers": .object(out)]
    }

    static func decode(_ root: JSONObject) -> [String: ChatRatchetPeer] {
        var peers: [String: ChatRatchetPeer] = [:]
        for (kid, value) in root["peers"]?.objectValue ?? JSONObject() {
            if let peer = ChatRatchetPeer.parse(value) { peers[kid] = peer }
        }
        return peers
    }
}
