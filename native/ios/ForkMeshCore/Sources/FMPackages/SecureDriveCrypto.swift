import FMBase
import FMCrypto
import Foundation

#if canImport(CryptoKit)
import CryptoKit
#endif

public struct SecureDriveError: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String

    public init(_ code: String) {
        self.code = code
    }

    public var description: String { code }
}

public struct SecureDriveKdf: Equatable, Sendable {
    public static let floorMemoryKiB = 19456
    public static let floorPasses = 2

    public var memoryKiB: Int
    public var passes: Int
    public var parallelism: Int
    public var salt: [UInt8]

    public init(memoryKiB: Int = 65536, passes: Int = 3, parallelism: Int = 1, salt: [UInt8]) {
        self.memoryKiB = memoryKiB
        self.passes = passes
        self.parallelism = parallelism
        self.salt = salt
    }

    public static func fresh(memoryKiB: Int = 65536, passes: Int = 3) -> SecureDriveKdf {
        SecureDriveKdf(memoryKiB: memoryKiB, passes: passes, salt: CryptoBytes.random(16))
    }

    public func toJSON() -> JSONObject {
        [
            "algorithm": "argon2id",
            "version": 19,
            "memoryKiB": JSONValue(memoryKiB),
            "passes": JSONValue(passes),
            "parallelism": JSONValue(parallelism),
            "salt": JSONValue(SecureDriveCrypto.base64(salt)),
        ]
    }

    public static func parse(_ json: JSONObject, floorMemoryKiB: Int = floorMemoryKiB, floorPasses: Int = floorPasses) throws -> SecureDriveKdf {
        let bad = SecureDriveError("unsafe_kdf_parameters")
        guard json["algorithm"]?.stringValue == "argon2id", SecureDriveJSON.int(json, "version") == 19,
              let memory = SecureDriveJSON.whole(json["memoryKiB"]),
              let passes = SecureDriveJSON.whole(json["passes"]),
              let lanes = SecureDriveJSON.whole(json["parallelism"]),
              let salt = SecureDriveCrypto.unbase64(json["salt"]?.stringValue ?? "_"),
              salt.count >= 16, salt.count <= 64,
              memory >= floorMemoryKiB, memory <= 1_048_576,
              passes >= floorPasses, passes <= 16,
              lanes >= 1, lanes <= 16 else { throw bad }
        return SecureDriveKdf(memoryKiB: memory, passes: passes, parallelism: lanes, salt: salt)
    }
}

public struct SecureDriveDeviceKeys: Equatable, Sendable {
    public var x25519: [UInt8]
    public var mlkemSeed: [UInt8]
    public var ed25519: [UInt8]

    public init(x25519: [UInt8], mlkemSeed: [UInt8], ed25519: [UInt8]) {
        self.x25519 = x25519
        self.mlkemSeed = mlkemSeed
        self.ed25519 = ed25519
    }

    public var isValid: Bool { x25519.count == 32 && mlkemSeed.count == 64 && ed25519.count == 32 }

    public static func generate() -> SecureDriveDeviceKeys {
        SecureDriveDeviceKeys(x25519: CryptoBytes.random(32), mlkemSeed: CryptoBytes.random(64), ed25519: CryptoBytes.random(32))
    }

    public func toJSON() -> JSONObject {
        [
            "x25519": JSONValue(SecureDriveCrypto.base64(x25519)),
            "mlkemSeed": JSONValue(SecureDriveCrypto.base64(mlkemSeed)),
            "ed25519": JSONValue(SecureDriveCrypto.base64(ed25519)),
        ]
    }

    public static func parse(_ json: JSONObject) throws -> SecureDriveDeviceKeys {
        guard let x = SecureDriveCrypto.unbase64(json["x25519"]?.stringValue ?? "_"),
              let m = SecureDriveCrypto.unbase64(json["mlkemSeed"]?.stringValue ?? "_"),
              let e = SecureDriveCrypto.unbase64(json["ed25519"]?.stringValue ?? "_") else {
            throw SecureDriveError("invalid_device_keys")
        }
        let keys = SecureDriveDeviceKeys(x25519: x, mlkemSeed: m, ed25519: e)
        guard keys.isValid else { throw SecureDriveError("invalid_device_keys") }
        return keys
    }

    public func publicKeys() throws -> SecureDrivePublicKeys {
        guard isValid else { throw SecureDriveError("invalid_device_keys") }
        do {
            let x = try X25519.publicKey(privateKey: x25519)
            let e = try Ed25519.publicKey(seed: ed25519)
            let m = try MlKem768.generateKeyPair(seed: mlkemSeed).publicKey
            return SecureDrivePublicKeys(x25519: x, mlkem768: m, ed25519: e)
        } catch {
            throw SecureDriveError("invalid_device_keys")
        }
    }

    mutating func wipe() {
        CryptoBytes.wipe(&x25519)
        CryptoBytes.wipe(&mlkemSeed)
        CryptoBytes.wipe(&ed25519)
        x25519 = []
        mlkemSeed = []
        ed25519 = []
    }
}

public struct SecureDrivePublicKeys: Equatable, Hashable, Sendable {
    public var x25519: [UInt8]
    public var mlkem768: [UInt8]
    public var ed25519: [UInt8]

    public init(x25519: [UInt8] = [], mlkem768: [UInt8] = [], ed25519: [UInt8] = []) {
        self.x25519 = x25519
        self.mlkem768 = mlkem768
        self.ed25519 = ed25519
    }

    public var isValid: Bool {
        x25519.count == 32 && mlkem768.count == SecureDriveCrypto.mlkemPublicBytes && ed25519.count == 32
    }

    public func toJSON() -> JSONObject {
        [
            "x25519": JSONValue(SecureDriveCrypto.base64(x25519)),
            "mlkem768": JSONValue(SecureDriveCrypto.base64(mlkem768)),
            "ed25519": JSONValue(SecureDriveCrypto.base64(ed25519)),
        ]
    }

    public static func parse(_ json: JSONObject?) -> SecureDrivePublicKeys? {
        guard let json,
              let x = SecureDriveCrypto.unbase64(json["x25519"]?.stringValue ?? "_"),
              let m = SecureDriveCrypto.unbase64(json["mlkem768"]?.stringValue ?? "_"),
              let e = SecureDriveCrypto.unbase64(json["ed25519"]?.stringValue ?? "_") else { return nil }
        let keys = SecureDrivePublicKeys(x25519: x, mlkem768: m, ed25519: e)
        guard keys.isValid, SecureDriveCrypto.mlkemPublicKeyIsValid(m) else { return nil }
        return keys
    }
}

public enum SecureDriveCrypto {
    public static let suite = "fmsd1"
    public static let keyBytes = 32
    public static let mlkemPublicBytes = 1184
    public static let mlkemCiphertextBytes = 1088
    static let prefix = Array("forkmesh-secure-drive/fmsd1/".utf8)
    static let xwingLabel: [UInt8] = [0x5c, 0x2e, 0x2f, 0x2f, 0x5e, 0x5c]
    static let base32Alphabet = Array("0123456789ABCDEFGHJKMNPQRSTVWXYZ".utf8)

    public static func label(_ name: String) -> [UInt8] { prefix + Array(name.utf8) }

    public static func base64(_ bytes: [UInt8]) -> String { B64.encodeUrlNoPadding(bytes) }

    public static func unbase64(_ text: String) -> [UInt8]? {
        for c in text.utf8 {
            let ok = (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) || c == 0x2d || c == 0x5f
            if !ok { return nil }
        }
        if text.isEmpty { return [] }
        guard let data = try? B64.decodeUnpadded(text) else { return nil }
        return Array(data)
    }

    public static func hex(_ bytes: [UInt8]) -> String { Hex.encode(bytes) }

    public static func randomBytes(_ count: Int) -> [UInt8] { CryptoBytes.random(count) }

    public static func randomId() -> String { hex(randomBytes(16)) }

    public static func sha256(_ parts: [UInt8]...) -> [UInt8] { Sha256.hash(parts: parts) }

    public static func hkdf(_ secret: [UInt8], _ info: String, length: Int = 32) -> [UInt8] {
        if secret.isEmpty { return [] }
        return Hkdf.sha256(ikm: secret, salt: [UInt8](repeating: 0, count: 32), info: label(info), length: length)
    }

    public static func hmac(_ key: [UInt8], _ message: [UInt8]) -> [UInt8] { HmacSha256.mac(key: key, message) }

    public static func constantTimeEqual(_ a: [UInt8], _ b: [UInt8]) -> Bool {
        !a.isEmpty && a.count == b.count && CryptoBytes.constantTimeEquals(a, b)
    }

    public static func aeadSeal(key: [UInt8], nonce: [UInt8], aad: [UInt8], plaintext: [UInt8]) -> [UInt8]? {
        guard key.count == keyBytes, nonce.count == 12,
              let box = try? AesGcm.encrypt(plaintext, key: key, nonce: nonce, aad: aad) else { return nil }
        return box.cipherText + box.mac
    }

    public static func aeadOpen(key: [UInt8], nonce: [UInt8], aad: [UInt8], sealed: [UInt8]) -> [UInt8]? {
        guard key.count == keyBytes, nonce.count == 12, sealed.count >= 16 else { return nil }
        let split = sealed.count - 16
        return AesGcm.decryptOrNull(cipherText: Array(sealed[0..<split]), nonce: nonce, mac: Array(sealed[split...]), key: key, aad: aad)
    }

    public static func stretch(_ passphrase: String, _ kdf: SecureDriveKdf) throws -> [UInt8] {
        var secret = Array(passphrase.precomposedStringWithCompatibilityMapping.utf8)
        defer { CryptoBytes.wipe(&secret) }
        do {
            return try Argon2id.deriveKey(
                password: secret, salt: kdf.salt, parallelism: kdf.parallelism,
                memoryKiB: kdf.memoryKiB, iterations: kdf.passes, hashLength: 32)
        } catch {
            throw SecureDriveError("kdf_failed")
        }
    }

    public static func sign(_ seed: [UInt8], _ message: [UInt8]) throws -> [UInt8] {
        do {
            return try Ed25519.sign(seed: seed, message: message)
        } catch {
            throw SecureDriveError("signing_failed")
        }
    }

    public static func verify(_ publicKey: [UInt8], _ message: [UInt8], _ signature: [UInt8]) -> Bool {
        publicKey.count == 32 && signature.count == 64 && Ed25519.verify(publicKey: publicKey, message: message, signature: signature)
    }

    static func mlkemPublicKeyIsValid(_ key: [UInt8]) -> Bool {
        guard key.count == mlkemPublicBytes else { return false }
        var i = 0
        while i < 1152 {
            let a = UInt16(key[i]) | (UInt16(key[i + 1] & 0x0f) << 8)
            let b = (UInt16(key[i + 1]) >> 4) | (UInt16(key[i + 2]) << 4)
            if a >= 3329 || b >= 3329 { return false }
            i += 3
        }
        return true
    }

    public static func sealToDevice(_ recipient: SecureDrivePublicKeys, label sealLabel: [UInt8], _ plaintext: [UInt8]) throws -> [UInt8] {
        guard recipient.isValid else { throw SecureDriveError("invalid_public_keys") }
        var ephemeral = randomBytes(32)
        defer { CryptoBytes.wipe(&ephemeral) }
        guard let ctX = try? X25519.publicKey(privateKey: ephemeral),
              var ssX = try? X25519.sharedSecret(privateKey: ephemeral, remotePublicKey: recipient.x25519),
              !CryptoBytes.isAllZero(ssX) else { throw SecureDriveError("invalid_public_keys") }
        defer { CryptoBytes.wipe(&ssX) }
        guard let encapsulation = try? MlKem768.encapsulate(publicKey: recipient.mlkem768),
              encapsulation.ciphertext.count == mlkemCiphertextBytes else { throw SecureDriveError("invalid_public_keys") }
        var ssM = encapsulation.sharedSecret
        defer { CryptoBytes.wipe(&ssM) }
        let ctM = encapsulation.ciphertext
        var shared = Keccak.sha3_256(ssM, ssX, ctX, recipient.x25519, xwingLabel)
        defer { CryptoBytes.wipe(&shared) }
        let labelText = String(decoding: sealLabel, as: UTF8.self)
        var key = hkdf(shared, "seal/" + labelText)
        defer { CryptoBytes.wipe(&key) }
        let nonce = randomBytes(12)
        let aad = label("seal/") + sealLabel + ctX + ctM
        guard let body = aeadSeal(key: key, nonce: nonce, aad: aad, plaintext: plaintext) else {
            throw SecureDriveError("encryption_failed")
        }
        return ctX + ctM + nonce + body
    }

    public static func openFromDevice(_ recipient: SecureDriveDeviceKeys, label sealLabel: [UInt8], _ sealed: [UInt8]) throws -> [UInt8] {
        guard recipient.isValid else { throw SecureDriveError("invalid_device_keys") }
        guard sealed.count >= 32 + mlkemCiphertextBytes + 12 + 16 else { throw SecureDriveError("invalid_ciphertext") }
        let ctX = Array(sealed[0..<32])
        let ctM = Array(sealed[32..<(32 + mlkemCiphertextBytes)])
        let nonce = Array(sealed[(32 + mlkemCiphertextBytes)..<(44 + mlkemCiphertextBytes)])
        let body = Array(sealed[(44 + mlkemCiphertextBytes)...])
        guard let myX = try? X25519.publicKey(privateKey: recipient.x25519),
              var ssX = try? X25519.sharedSecret(privateKey: recipient.x25519, remotePublicKey: ctX),
              !CryptoBytes.isAllZero(ssX) else { throw SecureDriveError("invalid_ciphertext") }
        defer { CryptoBytes.wipe(&ssX) }
        guard var pair = try? MlKem768.generateKeyPair(seed: recipient.mlkemSeed).secretKey,
              var ssM = try? MlKem768.decapsulate(secretKey: pair, ciphertext: ctM) else {
            throw SecureDriveError("invalid_ciphertext")
        }
        defer {
            CryptoBytes.wipe(&ssM)
            CryptoBytes.wipe(&pair)
        }
        var shared = Keccak.sha3_256(ssM, ssX, ctX, myX, xwingLabel)
        defer { CryptoBytes.wipe(&shared) }
        var key = hkdf(shared, "seal/" + String(decoding: sealLabel, as: UTF8.self))
        defer { CryptoBytes.wipe(&key) }
        let aad = label("seal/") + sealLabel + ctX + ctM
        guard let plain = aeadOpen(key: key, nonce: nonce, aad: aad, sealed: body) else {
            throw SecureDriveError("decryption_failed")
        }
        return plain
    }

    public static func safetyNumber(_ context: [UInt8], _ keys: SecureDrivePublicKeys) -> String {
        let digest = sha256(label("safety"), context, keys.ed25519, keys.x25519, keys.mlkem768)
        var groups = [String]()
        for i in 0..<4 {
            var value: UInt32 = 0
            for j in 0..<4 { value = (value << 8) | UInt32(digest[i * 4 + j]) }
            let text = String(value % 100_000)
            groups.append(String(repeating: "0", count: 5 - text.count) + text)
        }
        return groups.joined(separator: " ")
    }

    static func base32(_ bytes: [UInt8]) -> String {
        var out = [UInt8]()
        var buffer: UInt32 = 0
        var bits = 0
        for byte in bytes {
            buffer = (buffer << 8) | UInt32(byte)
            bits += 8
            while bits >= 5 {
                out.append(base32Alphabet[Int((buffer >> UInt32(bits - 5)) & 31)])
                bits -= 5
            }
        }
        if bits > 0 { out.append(base32Alphabet[Int((buffer << UInt32(5 - bits)) & 31)]) }
        return String(decoding: out, as: UTF8.self)
    }

    public static func newInviteCode() -> (code: String, secret: [UInt8]) {
        let secret = randomBytes(16)
        let check = sha256(label("invite-check"), secret)[0]
        let text = Array(base32(secret + [check]).utf8.prefix(27))
        var groups = [String]()
        var i = 0
        while i < text.count {
            groups.append(String(decoding: text[i..<min(i + 3, text.count)], as: UTF8.self))
            i += 3
        }
        return (groups.joined(separator: "-"), secret)
    }

    public static func parseInviteCode(_ code: String) -> [UInt8]? {
        var cleaned = [UInt8]()
        for scalar in code.uppercased().unicodeScalars {
            if scalar == "-" || CharacterSet.whitespacesAndNewlines.contains(scalar) { continue }
            switch scalar {
            case "O": cleaned.append(UInt8(ascii: "0"))
            case "I", "L": cleaned.append(UInt8(ascii: "1"))
            default:
                guard scalar.isASCII else { return nil }
                cleaned.append(UInt8(scalar.value))
            }
        }
        guard cleaned.count == 27 else { return nil }
        var buffer: UInt64 = 0
        var bits = 0
        var bytes = [UInt8]()
        for c in cleaned {
            guard let index = base32Alphabet.firstIndex(of: c) else { return nil }
            buffer = (buffer << 5) | UInt64(index)
            bits += 5
            if bits >= 8 {
                bytes.append(UInt8((buffer >> UInt64(bits - 8)) & 0xff))
                bits -= 8
            }
        }
        guard bytes.count == 16 else { return nil }
        let check = sha256(label("invite-check"), bytes)[0]
        let expected = check >> UInt8(8 - bits)
        guard UInt8(buffer & ((1 << UInt64(bits)) - 1)) == expected else { return nil }
        return bytes
    }

    public static func paddedSize(_ length: Int64) -> Int64 {
        if length <= 4096 { return 4096 }
        var exponent = 0
        while (Int64(1) << Int64(exponent + 1)) <= length { exponent += 1 }
        var bitsOfExponent = 0
        while (1 << bitsOfExponent) <= exponent { bitsOfExponent += 1 }
        let mask = (Int64(1) << Int64(exponent - bitsOfExponent)) - 1
        return (length + mask) & ~mask
    }

    static func sealUnit(_ key: [UInt8], _ aad: [UInt8], _ plain: [UInt8], size: Int) -> [UInt8]? {
        guard plain.count <= size - SecureDriveLayout.unitOverhead else { return nil }
        var padded = plain
        padded.append(contentsOf: [UInt8](repeating: 0, count: size - SecureDriveLayout.unitOverhead - plain.count))
        defer { CryptoBytes.wipe(&padded) }
        let nonce = randomBytes(12)
        guard let sealed = aeadSeal(key: key, nonce: nonce, aad: aad, plaintext: padded) else { return nil }
        return nonce + sealed
    }

    static func openUnit(_ key: [UInt8], _ aad: [UInt8], _ bytes: [UInt8]?) -> [UInt8]? {
        guard let bytes, bytes.count >= SecureDriveLayout.unitOverhead, key.count == keyBytes else { return nil }
        return aeadOpen(key: key, nonce: Array(bytes[0..<12]), aad: aad, sealed: Array(bytes[12...]))
    }

    static func stripPadding(_ plain: [UInt8]) -> [UInt8] {
        guard let end = plain.firstIndex(of: 0) else { return plain }
        return Array(plain[0..<end])
    }
}

enum SecureDriveJSON {
    static func encode(_ object: JSONObject) -> [UInt8] { Array(JSON.encode(object).utf8) }

    static func parse(_ bytes: [UInt8]) -> JSONObject? {
        guard let value = try? JSON.parse(Data(bytes)), case .object(let object) = value else { return nil }
        return object
    }

    static func str(_ object: JSONObject, _ key: String) -> String { object[key]?.stringValue ?? "" }

    static func obj(_ object: JSONObject, _ key: String) -> JSONObject { object[key]?.objectValue ?? JSONObject() }

    static func arr(_ object: JSONObject, _ key: String) -> [JSONValue] { object[key]?.arrayValue ?? [] }

    static func num(_ value: JSONValue?, _ fallback: Int64) -> Int64 {
        switch value {
        case .int(let n): return n
        case .double(let d):
            if d.isNaN { return fallback }
            if d >= 0x1p63 { return Int64.max }
            if d <= -0x1p63 { return Int64.min }
            return Int64(d)
        default: return fallback
        }
    }

    static func num(_ object: JSONObject, _ key: String, _ fallback: Int64 = 0) -> Int64 { num(object[key], fallback) }

    static func whole(_ value: JSONValue?) -> Int? {
        switch value {
        case .int(let n): return n >= Int64(Int32.min) && n <= Int64(Int32.max) ? Int(n) : nil
        case .double(let d): return d == d.rounded() && abs(d) < 2_147_483_648 ? Int(d) : nil
        default: return nil
        }
    }

    static func int(_ object: JSONObject, _ key: String, _ fallback: Int = 0) -> Int { whole(object[key]) ?? fallback }

    static func envelope(_ body: [UInt8], _ signature: [UInt8]) -> JSONObject {
        ["b": JSONValue(SecureDriveCrypto.base64(body)), "s": JSONValue(SecureDriveCrypto.base64(signature))]
    }

    static func openEnvelope(_ json: JSONObject?) -> (body: [UInt8], signature: [UInt8], parsed: JSONObject)? {
        guard let json, let b = json["b"]?.stringValue, let s = json["s"]?.stringValue,
              let body = SecureDriveCrypto.unbase64(b), let signature = SecureDriveCrypto.unbase64(s),
              signature.count == 64, let parsed = parse(body) else { return nil }
        return (body, signature, parsed)
    }

    static func prefix(_ text: String, _ count: Int) -> String {
        if text.utf16.count <= count { return text }
        var out = ""
        var used = 0
        for character in text {
            let width = character.utf16.count
            if used + width > count { break }
            out.append(character)
            used += width
        }
        return out
    }
}

struct Sha256Stream {
    #if canImport(CryptoKit)
    private var hasher = CryptoKit.SHA256()

    mutating func update(_ bytes: [UInt8]) { hasher.update(data: bytes) }

    mutating func finish() -> [UInt8] { Array(hasher.finalize()) }
    #else
    private var state: [UInt32] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
    private var buffer: [UInt8] = []
    private var length: UInt64 = 0

    private static let k: [UInt32] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ]

    mutating func update(_ bytes: [UInt8]) {
        length &+= UInt64(bytes.count)
        var index = 0
        if !buffer.isEmpty {
            let need = 64 - buffer.count
            let take = min(need, bytes.count)
            buffer.append(contentsOf: bytes[0..<take])
            index = take
            if buffer.count == 64 {
                let block = buffer
                block.withUnsafeBufferPointer { compress($0.baseAddress!) }
                buffer = []
            }
        }
        bytes.withUnsafeBufferPointer { pointer in
            while index + 64 <= bytes.count {
                compress(pointer.baseAddress! + index)
                index += 64
            }
        }
        if index < bytes.count { buffer.append(contentsOf: bytes[index...]) }
    }

    private mutating func compress(_ block: UnsafePointer<UInt8>) {
        var w = [UInt32](repeating: 0, count: 64)
        for i in 0..<16 {
            w[i] = UInt32(block[i * 4]) << 24 | UInt32(block[i * 4 + 1]) << 16 | UInt32(block[i * 4 + 2]) << 8 | UInt32(block[i * 4 + 3])
        }
        for i in 16..<64 {
            let s0 = (w[i - 15] >> 7 | w[i - 15] << 25) ^ (w[i - 15] >> 18 | w[i - 15] << 14) ^ (w[i - 15] >> 3)
            let s1 = (w[i - 2] >> 17 | w[i - 2] << 15) ^ (w[i - 2] >> 19 | w[i - 2] << 13) ^ (w[i - 2] >> 10)
            w[i] = w[i - 16] &+ s0 &+ w[i - 7] &+ s1
        }
        var a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7]
        for i in 0..<64 {
            let s1 = (e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7)
            let ch = (e & f) ^ (~e & g)
            let t1 = h &+ s1 &+ ch &+ Self.k[i] &+ w[i]
            let s0 = (a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10)
            let maj = (a & b) ^ (a & c) ^ (b & c)
            let t2 = s0 &+ maj
            h = g
            g = f
            f = e
            e = d &+ t1
            d = c
            c = b
            b = a
            a = t1 &+ t2
        }
        state[0] &+= a
        state[1] &+= b
        state[2] &+= c
        state[3] &+= d
        state[4] &+= e
        state[5] &+= f
        state[6] &+= g
        state[7] &+= h
    }

    mutating func finish() -> [UInt8] {
        let bits = length &* 8
        var tail = buffer
        tail.append(0x80)
        while tail.count % 64 != 56 { tail.append(0) }
        for i in (0..<8).reversed() { tail.append(UInt8(truncatingIfNeeded: bits >> (UInt64(i) * 8))) }
        buffer = []
        tail.withUnsafeBufferPointer { pointer in
            var offset = 0
            while offset < tail.count {
                compress(pointer.baseAddress! + offset)
                offset += 64
            }
        }
        var out = [UInt8]()
        for word in state {
            out.append(UInt8(word >> 24))
            out.append(UInt8((word >> 16) & 0xff))
            out.append(UInt8((word >> 8) & 0xff))
            out.append(UInt8(word & 0xff))
        }
        return out
    }
    #endif
}
