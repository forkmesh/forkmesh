import FMBase
import Foundation

public struct MeshLinkError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { message }
}

public enum MeshLinkCrypto {
    public static let helloTag = "fm-link-hello"
    public static let authTag = "fm-link-auth"
    public static let label = "forkmesh-mesh-link-v3"
    public static let nonceBytes = 16

    public static func transcript(initiator: JSONObject, responder: JSONObject) -> [UInt8] {
        let text = [
            label, initiator["idk"]?.stringValue ?? "", initiator["eph"]?.stringValue ?? "",
            initiator["nonce"]?.stringValue ?? "", responder["idk"]?.stringValue ?? "",
            responder["eph"]?.stringValue ?? "", responder["nonce"]?.stringValue ?? "",
        ].joined(separator: "\n")
        return Sha256.hash(Array(text.utf8))
    }

    public static func authMessage(role: String, transcript: [UInt8]) -> [UInt8] {
        Array("forkmesh-mesh-link-auth-v3\n\(role)\n\(ChatV3.b64u(transcript))".utf8)
    }

    public static func keys(ownEph: [UInt8], peerEph: [UInt8], transcript: [UInt8]) throws -> (i2r: [UInt8], r2i: [UInt8]) {
        var shared = try X25519.sharedSecret(privateKey: ownEph, remotePublicKey: peerEph)
        defer { CryptoBytes.wipe(&shared) }
        if CryptoBytes.isAllZero(shared) { throw MeshLinkError("all-zero link secret") }
        let i2r = Hkdf.sha256(ikm: shared, salt: transcript, info: Array("\(label) i2r".utf8), length: 32)
        let r2i = Hkdf.sha256(ikm: shared, salt: transcript, info: Array("\(label) r2i".utf8), length: 32)
        return (i2r, r2i)
    }

    public static func nonce(_ counter: UInt64) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: 12)
        var value = counter
        var index = 11
        while index >= 4 {
            out[index] = UInt8(value & 0xff)
            value >>= 8
            index -= 1
        }
        return out
    }

    public static func seal(key: [UInt8], counter: UInt64, plain: JSONObject) throws -> [UInt8] {
        let box = try AesGcm.encrypt(
            Array(JSON.encode(plain).utf8), key: key, nonce: nonce(counter), aad: Array(label.utf8))
        return box.cipherText + box.mac
    }

    public static func open(key: [UInt8], counter: UInt64, data: [UInt8]) -> JSONObject? {
        if data.count < ChatV3.tagBytes { return nil }
        let split = data.count - ChatV3.tagBytes
        guard
            let clear = AesGcm.decryptOrNull(
                cipherText: Array(data[0..<split]), nonce: nonce(counter), mac: Array(data[split...]), key: key,
                aad: Array(label.utf8))
        else { return nil }
        return CryptoText.jsonObject(clear)
    }

    public static func isHello(_ frame: JSONObject) -> Bool { frame["t"]?.stringValue == helloTag }
}

public final class MeshLinkHandshake {
    public let initiator: Bool
    public let hello: JSONObject
    public private(set) var peerIdentityKey: [UInt8]?
    public private(set) var peerKid = ""
    public private(set) var authenticated = false

    private let keyring: ChatKeyring
    private var ephPriv: [UInt8]
    private var transcript: [UInt8] = []
    private var sendKey: [UInt8] = []
    private var receiveKey: [UInt8] = []
    private var sendCounter: UInt64 = 0
    private var receiveCounter: UInt64 = 0
    private var gotHello = false

    public init(keyring: ChatKeyring, initiator: Bool, ephemeral: [UInt8]? = nil, nonce: [UInt8]? = nil) throws {
        self.keyring = keyring
        self.initiator = initiator
        let priv = ephemeral ?? X25519.generatePrivateKey()
        self.ephPriv = priv
        let pub = try X25519.publicKey(privateKey: priv)
        self.hello = [
            "t": .string(MeshLinkCrypto.helloTag), "v": .int(ChatV3.version), "idk": .string(keyring.idk),
            "eph": .string(ChatV3.b64u(pub)),
            "nonce": .string(ChatV3.b64u(nonce ?? CryptoBytes.random(MeshLinkCrypto.nonceBytes))),
        ]
    }

    public var canSeal: Bool { !sendKey.isEmpty }

    public func receiveHello(_ frame: JSONObject) throws -> JSONObject {
        guard !gotHello else { throw MeshLinkError("second link hello") }
        guard frame["t"]?.stringValue == MeshLinkCrypto.helloTag, frame["v"]?.numEquals(ChatV3.version) == true,
            let idk = ChatV3.bytes64u(frame["idk"], Ed25519.publicKeyBytes),
            let eph = ChatV3.bytes64u(frame["eph"], X25519.keyBytes),
            ChatV3.bytes64u(frame["nonce"], MeshLinkCrypto.nonceBytes) != nil
        else { throw MeshLinkError("bad link hello") }
        if idk == keyring.identityKey { throw MeshLinkError("own identity") }
        gotHello = true
        peerIdentityKey = idk
        peerKid = ChatV3.id16(idk)
        transcript =
            initiator
            ? MeshLinkCrypto.transcript(initiator: hello, responder: frame)
            : MeshLinkCrypto.transcript(initiator: frame, responder: hello)
        let keys = try MeshLinkCrypto.keys(ownEph: ephPriv, peerEph: eph, transcript: transcript)
        CryptoBytes.wipe(&ephPriv)
        sendKey = initiator ? keys.i2r : keys.r2i
        receiveKey = initiator ? keys.r2i : keys.i2r
        let signature = try keyring.sign(MeshLinkCrypto.authMessage(role: initiator ? "i" : "r", transcript: transcript))
        return ["t": .string(MeshLinkCrypto.authTag), "sig": .string(ChatV3.b64u(signature))]
    }

    @discardableResult
    public func receiveAuth(_ frame: JSONObject) throws -> String {
        guard gotHello, !authenticated, let peer = peerIdentityKey else { throw MeshLinkError("link auth out of order") }
        guard frame["t"]?.stringValue == MeshLinkCrypto.authTag,
            let signature = ChatV3.bytes64u(frame["sig"], ChatV3.signatureBytes),
            Ed25519.verify(
                publicKey: peer, message: MeshLinkCrypto.authMessage(role: initiator ? "r" : "i", transcript: transcript),
                signature: signature)
        else { throw MeshLinkError("bad link signature") }
        authenticated = true
        return peerKid
    }

    public func seal(_ plain: JSONObject) throws -> [UInt8] {
        guard !sendKey.isEmpty else { throw MeshLinkError("link not keyed") }
        let data = try MeshLinkCrypto.seal(key: sendKey, counter: sendCounter, plain: plain)
        sendCounter += 1
        return data
    }

    public func open(_ data: [UInt8]) throws -> JSONObject {
        guard authenticated, !receiveKey.isEmpty else { throw MeshLinkError("link not authenticated") }
        guard let plain = MeshLinkCrypto.open(key: receiveKey, counter: receiveCounter, data: data) else {
            throw MeshLinkError("link frame did not open")
        }
        receiveCounter += 1
        return plain
    }

    public func wipe() {
        CryptoBytes.wipe(&ephPriv)
        CryptoBytes.wipe(&sendKey)
        CryptoBytes.wipe(&receiveKey)
        sendKey = []
        receiveKey = []
    }
}
