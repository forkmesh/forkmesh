// Port of the AES-GCM case in native/android/.../crypto/PrimitivesCryptoTest.kt, plus the NIST GCM
// specification test cases 13-16 (AES-256) for the pure Swift path.
import XCTest
@testable import FMCrypto

final class AesGcmTests: XCTestCase {
    func testAesGcm() throws {
        let key = (0..<32).map { UInt8($0) }
        let nonce = (0..<12).map { UInt8(100 + $0) }
        let box = try AesGcm.encrypt(ascii("forkmesh aes-gcm vector"), key: key, nonce: nonce, aad: ascii("aad bytes"))
        assertHex("2e74ac0d148c25f61e033a9bf702099062b46369ff0381", box.cipherText)
        assertHex("9747de6ef4c6f54b48950fa3d372cad9", box.mac)
        XCTAssertEqual(nonce, box.nonce)
        XCTAssertEqual(
            ascii("forkmesh aes-gcm vector"),
            try AesGcm.decrypt(cipherText: box.cipherText, nonce: nonce, mac: box.mac, key: key, aad: ascii("aad bytes")))
        let empty = try AesGcm.encrypt([], key: key, nonce: nonce)
        XCTAssertEqual(0, empty.cipherText.count)
        assertHex("609bd50f6ad76df665f048ba8c5fd706", empty.mac)

        XCTAssertNil(AesGcm.decryptOrNull(cipherText: box.cipherText, nonce: nonce, mac: box.mac, key: key, aad: ascii("aad byteZ")))
        XCTAssertNil(AesGcm.decryptOrNull(cipherText: box.cipherText, nonce: nonce, mac: box.mac, key: key))
        var tamperedTag = box.mac
        tamperedTag[0] ^= 1
        XCTAssertThrowsError(try AesGcm.decrypt(cipherText: box.cipherText, nonce: nonce, mac: tamperedTag, key: key, aad: ascii("aad bytes"))) {
            XCTAssertTrue($0 is AesGcm.AuthenticationError)
        }
        XCTAssertThrowsError(try AesGcm.decrypt(cipherText: box.cipherText, nonce: nonce, mac: Array(box.mac[0..<12]), key: key, aad: ascii("aad bytes"))) {
            XCTAssertTrue($0 is AesGcm.AuthenticationError)
        }
        var tamperedText = box.cipherText
        tamperedText[3] ^= 0x40
        XCTAssertNil(AesGcm.decryptOrNull(cipherText: tamperedText, nonce: nonce, mac: box.mac, key: key, aad: ascii("aad bytes")))
        XCTAssertThrowsError(try AesGcm.encrypt([1], key: Array(key[0..<16]))) { XCTAssertTrue($0 is CryptoArgumentError) }
        XCTAssertThrowsError(try AesGcm.encrypt([1], key: key, nonce: [1, 2, 3])) { XCTAssertTrue($0 is CryptoArgumentError) }
        let random = try AesGcm.encrypt(ascii("x"), key: key)
        XCTAssertNotEqual(random.nonce, try AesGcm.encrypt(ascii("x"), key: key).nonce, "fresh nonces")
    }

    /// McGrew & Viega, "The Galois/Counter Mode of Operation", test cases 13 to 16.
    func testNistVectorsOnThePortablePath() throws {
        let zeroKey = [UInt8](repeating: 0, count: 32)
        let zeroNonce = [UInt8](repeating: 0, count: 12)
        let case13 = try AesGcm.encryptPortable([], key: zeroKey, nonce: zeroNonce)
        assertHex("530f8afbc74536b9a963b4f1c4cb738b", case13.mac)
        let case14 = try AesGcm.encryptPortable([UInt8](repeating: 0, count: 16), key: zeroKey, nonce: zeroNonce)
        assertHex("cea7403d4d606b6e074ec5d3baf39d18", case14.cipherText)
        assertHex("d0d1c8a799996bf0265b98b5d48ab919", case14.mac)

        let key = unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308")
        let nonce = unhex("cafebabefacedbaddecaf888")
        let plain = unhex(
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255")
        let case15 = try AesGcm.encryptPortable(plain, key: key, nonce: nonce)
        assertHex(
            "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
            case15.cipherText)
        assertHex("b094dac5d93471bdec1a502270e3cc6c", case15.mac)

        let aad = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2")
        let short = Array(plain[0..<60])
        let case16 = try AesGcm.encryptPortable(short, key: key, nonce: nonce, aad: aad)
        assertHex(
            "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
            case16.cipherText)
        assertHex("76fc6ece0f4e1768cddf8853bb2d551b", case16.mac)
        XCTAssertEqual(short, try AesGcm.decryptPortable(cipherText: case16.cipherText, nonce: nonce, mac: case16.mac, key: key, aad: aad))
    }
}
