import FMBase
import FMCrypto
import Foundation

public struct CoveCipher: Equatable, Sendable {
    public var nonce: String
    public var tag: String
    public var body: String

    public init(nonce: String, tag: String, body: String) {
        self.nonce = nonce
        self.tag = tag
        self.body = body
    }

    public init?(json: JSONObject?) {
        guard let json else { return nil }
        let nonce = json["nonce"]?.stringValue ?? ""
        let tag = json["tag"]?.stringValue ?? ""
        let body = json["body"]?.stringValue ?? ""
        if nonce.isEmpty && tag.isEmpty && body.isEmpty { return nil }
        self.init(nonce: nonce, tag: tag, body: body)
    }

    public var json: JSONObject {
        ["body": .string(body), "nonce": .string(nonce), "tag": .string(tag)]
    }
}

public enum CoveCryptoError: Error, Equatable, CustomStringConvertible {
    case passwordRequired
    case missingParameters
    case badKeyLength
    case tooLarge
    case encryptFailed

    public var description: String {
        switch self {
        case .passwordRequired: return "A password is required."
        case .missingParameters: return "The cove is missing its key-derivation parameters."
        case .badKeyLength: return "A cove content key must be exactly 32 bytes."
        case .tooLarge: return "The cove is larger than 4 MB."
        case .encryptFailed: return "Could not encrypt the cove."
        }
    }
}

public struct CoveCrypto: Sendable {
    public static let defaultRounds = 600_000
    public static let keyBytes = 32
    public static let nonceBytes = 12
    public static let tagBytes = 16
    public static let saltBytes = 16
    public static let maxPlainBytes = 4 * 1024 * 1024

    private let key: [UInt8]

    public init(password: String, salt: [UInt8], rounds: Int) throws {
        if password.isEmpty { throw CoveCryptoError.passwordRequired }
        if salt.isEmpty || rounds <= 0 { throw CoveCryptoError.missingParameters }
        key = CoveCrypto.deriveKey(password: password, salt: salt, rounds: rounds)
    }

    public init(key: [UInt8]) throws {
        guard key.count == CoveCrypto.keyBytes else { throw CoveCryptoError.badKeyLength }
        self.key = key
    }

    public static func deriveKey(password: String, salt: [UInt8], rounds: Int) -> [UInt8] {
        Pbkdf2.hmacSha256(password: Array(password.utf8), salt: salt, iterations: rounds, length: keyBytes)
    }

    public static func randomBytes(_ count: Int) -> [UInt8] { CryptoBytes.random(count) }
    public static func randomSalt() -> [UInt8] { randomBytes(saltBytes) }
    public static func randomKey() -> [UInt8] { randomBytes(keyBytes) }

    public func encrypt(_ plaintext: [UInt8], nonce: [UInt8]? = nil) throws -> CoveCipher {
        if plaintext.count > CoveCrypto.maxPlainBytes { throw CoveCryptoError.tooLarge }
        let box: AesGcm.SecretBox
        do {
            box = try AesGcm.encrypt(plaintext, key: key, nonce: nonce ?? CryptoBytes.random(CoveCrypto.nonceBytes))
        } catch {
            throw CoveCryptoError.encryptFailed
        }
        return CoveCipher(
            nonce: CoveBase64.encode(box.nonce), tag: CoveBase64.encode(box.mac), body: CoveBase64.encode(box.cipherText))
    }

    public func decrypt(_ cipher: CoveCipher) -> [UInt8]? {
        let nonce = CoveBase64.decode(cipher.nonce)
        let tag = CoveBase64.decode(cipher.tag)
        let body = CoveBase64.decode(cipher.body)
        if nonce.count != CoveCrypto.nonceBytes || tag.count != CoveCrypto.tagBytes || body.isEmpty
            || body.count > CoveCrypto.maxPlainBytes
        {
            return nil
        }
        return AesGcm.decryptOrNull(cipherText: body, nonce: nonce, mac: tag, key: key)
    }
}

public enum CoveBase64 {
    public static func encode(_ bytes: [UInt8]) -> String { Data(bytes).base64EncodedString() }

    public static func decode(_ text: String) -> [UInt8] {
        var cleaned = text.filter { !$0.isWhitespace }
        let remainder = cleaned.count % 4
        if remainder != 0 { cleaned += String(repeating: "=", count: 4 - remainder) }
        guard let data = Data(base64Encoded: cleaned, options: .ignoreUnknownCharacters) else { return [] }
        return [UInt8](data)
    }
}
