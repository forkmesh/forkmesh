import FMBase
import FMCrypto
import XCTest

@testable import FMCoves

final class CoveCryptoTests: XCTestCase {
    private let salt: [UInt8] = Array(0...15)

    func testKeyDerivationMatchesDesktopOpenSSL() {
        let key = CoveCrypto.deriveKey(password: "correct horse battery staple", salt: salt, rounds: 2000)
        XCTAssertEqual(Hex.encode(key), "747aefd163ba4d3a6596ebc653329c342a4c9e7c1df2394d30ae4d05df8e000e")
        let unicode = CoveCrypto.deriveKey(password: "pässwörd \u{1F512}", salt: salt, rounds: 1000)
        XCTAssertEqual(Hex.encode(unicode), "c19d4443fbe043672ccf91e9076f9942ef6ea17637be65bdd27bda3dc961a047")
    }

    func testEncryptDecryptRoundTripAndTamper() throws {
        let crypto = try CoveCrypto(password: "pw", salt: salt, rounds: 10)
        let cipher = try crypto.encrypt(Array("hello cove".utf8))
        XCTAssertEqual(CoveBase64.decode(cipher.nonce).count, 12)
        XCTAssertEqual(CoveBase64.decode(cipher.tag).count, 16)
        XCTAssertEqual(crypto.decrypt(cipher).map { String(decoding: $0, as: UTF8.self) }, "hello cove")
        let wrong = try CoveCrypto(password: "pW", salt: salt, rounds: 10)
        XCTAssertNil(wrong.decrypt(cipher))
        var tampered = cipher
        var body = CoveBase64.decode(cipher.body)
        body[0] ^= 1
        tampered.body = CoveBase64.encode(body)
        XCTAssertNil(crypto.decrypt(tampered))
    }

    func testFixedNonceIsDeterministicAndPortableAgrees() throws {
        let key = [UInt8](repeating: 7, count: 32)
        let nonce = [UInt8](repeating: 1, count: 12)
        let crypto = try CoveCrypto(key: key)
        let cipher = try crypto.encrypt(Array("x".utf8), nonce: nonce)
        let portable = try AesGcm.encryptPortable(Array("x".utf8), key: key, nonce: nonce)
        XCTAssertEqual(CoveBase64.decode(cipher.body), portable.cipherText)
        XCTAssertEqual(CoveBase64.decode(cipher.tag), portable.mac)
    }

    func testRejectsBadInputsLikeDesktop() throws {
        XCTAssertThrowsError(try CoveCrypto(password: "", salt: salt, rounds: 1))
        XCTAssertThrowsError(try CoveCrypto(password: "x", salt: [], rounds: 1))
        XCTAssertThrowsError(try CoveCrypto(password: "x", salt: salt, rounds: 0))
        XCTAssertThrowsError(try CoveCrypto(key: [1, 2, 3]))
        let crypto = try CoveCrypto(key: [UInt8](repeating: 0, count: 32))
        XCTAssertThrowsError(try crypto.encrypt([UInt8](repeating: 0, count: CoveCrypto.maxPlainBytes + 1)))
        let empty = try crypto.encrypt([])
        XCTAssertNil(crypto.decrypt(empty))
        XCTAssertNil(crypto.decrypt(CoveCipher(nonce: "AAAA", tag: empty.tag, body: "AA==")))
    }
}
