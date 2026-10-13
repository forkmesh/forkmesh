// Port of native/android/.../crypto/MlKem768CryptoTest.kt. FIPS 203 ML-KEM-768 known answers: the
// C2SP/CCTV accumulated vector pinned by Go's crypto/mlkem (key generation, encapsulation,
// decapsulation and implicit rejection over SHAKE-128 driven inputs), and a seeded key plus
// encapsulation produced by OpenSSL 3.5.
import XCTest
@testable import FMCrypto

final class MlKem768Tests: XCTestCase {
    /// Go 1.24 crypto/mlkem TestAccumulated: 100 iterations (its -short value).
    func testAccumulatedKnownAnswer() throws {
        XCTAssertEqual("1114b1b6699ed191734fa339376afa7e285c9e6acf6ff0177d346696ce564415", try accumulated(100))
        if ProcessInfo.processInfo.environment["FORKMESH_CRYPTO_SLOW_KAT"] == "1" {
            XCTAssertEqual("8a518cc63da366322a8e7a818c7a0d63483cb3528d34a4cf42f35d5ad73f22fc", try accumulated(10000))
        }
    }

    func testOpenSslKnownAnswers() throws {
        let pair = try MlKem768.generateKeyPair(seed: unhex(MlKem768Kat.seed))
        assertHex(MlKem768Kat.ekSha256, Sha256.hash(pair.publicKey))
        assertHex(MlKem768Kat.dkSha256, Sha256.hash(pair.secretKey))
        assertHex(MlKem768Kat.sharedSecret, try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: unhex(MlKem768Kat.ciphertext)))
        assertHex(MlKem768Kat.rejectionSecret, try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: unhex(MlKem768Kat.tamperedCiphertext)))
    }

    func testRoundTripAndImplicitRejection() throws {
        let pair = MlKem768.generateKeyPair()
        XCTAssertEqual(MlKem768.publicKeyBytes, pair.publicKey.count)
        XCTAssertEqual(MlKem768.secretKeyBytes, pair.secretKey.count)
        for _ in 0..<5 {
            let encapsulation = try MlKem768.encapsulate(publicKey: pair.publicKey)
            XCTAssertEqual(MlKem768.ciphertextBytes, encapsulation.ciphertext.count)
            XCTAssertEqual(encapsulation.sharedSecret, try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: encapsulation.ciphertext))
            let z = Array(pair.secretKey[(MlKem768.secretKeyBytes - 32)...])
            for (index, flip) in [(1000, UInt8(0x04)), (0, 0x80), (1087, 0x80), (500, 0x01)] {
                var tampered = encapsulation.ciphertext
                tampered[index] ^= flip
                let rejected = try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: tampered)
                XCTAssertNotEqual(rejected, encapsulation.sharedSecret, "byte \(index) xor \(flip)")
                XCTAssertEqual(Keccak.shake256(32, z, tampered), rejected)
            }
        }
        let other = MlKem768.generateKeyPair()
        let encapsulation = try MlKem768.encapsulate(publicKey: pair.publicKey)
        XCTAssertNotEqual(try MlKem768.decapsulate(secretKey: other.secretKey, ciphertext: encapsulation.ciphertext), encapsulation.sharedSecret)
    }

    func testMalformedInputsThrowLikePqcrypto() throws {
        let seed = [UInt8](repeating: 3, count: 64)
        let pair = try MlKem768.generateKeyPair(seed: seed)
        var badKey = pair.publicKey // coefficient 0xfff >= q
        badKey[0] = 0xff
        badKey[1] |= 0x0f
        XCTAssertThrowsError(try MlKem768.encapsulate(publicKey: badKey)) { XCTAssertTrue($0 is CryptoArgumentError) }
        XCTAssertThrowsError(try MlKem768.encapsulate(publicKey: Array(pair.publicKey[0..<1183])))
        XCTAssertThrowsError(try MlKem768.encapsulate(publicKey: pair.publicKey, message: [1]))
        XCTAssertThrowsError(try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: [UInt8](repeating: 0, count: 1087)))
        var badHash = pair.secretKey
        badHash[1152 + 1184] ^= 1
        XCTAssertThrowsError(try MlKem768.decapsulate(secretKey: badHash, ciphertext: [UInt8](repeating: 0, count: MlKem768.ciphertextBytes)))
        var badSecret = pair.secretKey // an s coefficient of 0xfff
        badSecret[0] = 0xff
        badSecret[1] |= 0x0f
        XCTAssertThrowsError(try MlKem768.decapsulate(secretKey: badSecret, ciphertext: [UInt8](repeating: 0, count: MlKem768.ciphertextBytes)))
        XCTAssertThrowsError(try MlKem768.generateKeyPair(seed: [UInt8](repeating: 0, count: 32)))
        XCTAssertEqual(pair.publicKey, try MlKem768.generateKeyPair(seed: seed).publicKey)
    }

    /// The multiply-and-shift that replaces `% q` and `/ q` is exact over everything it is given.
    func testReductionIsExact() {
        let q: Int32 = 3329
        var x: Int32 = 0
        let limit: Int32 = 2 * q * q + q
        while x < limit {
            XCTAssertEqual(x % q, MlKem768.reduce(x))
            x += 7
        }
        var multiple: Int32 = 0
        while multiple < limit {
            for delta: Int32 in [-1, 0, 1] where multiple + delta >= 0 {
                XCTAssertEqual((multiple + delta) % q, MlKem768.reduce(multiple + delta))
            }
            multiple += q
        }
    }

    private func accumulated(_ iterations: Int) throws -> String {
        var source = Keccak.shake128()
        var output = Keccak.shake128()
        for _ in 0..<iterations {
            let pair = try MlKem768.generateKeyPair(seed: source.squeeze(MlKem768.seedBytes))
            output.update(pair.publicKey)
            let encapsulation = try MlKem768.encapsulate(publicKey: pair.publicKey, message: source.squeeze(32))
            output.update(encapsulation.ciphertext)
            output.update(encapsulation.sharedSecret)
            XCTAssertEqual(encapsulation.sharedSecret, try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: encapsulation.ciphertext))
            output.update(try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: source.squeeze(MlKem768.ciphertextBytes)))
        }
        return CryptoBytes.hex(output.digest(32))
    }
}
