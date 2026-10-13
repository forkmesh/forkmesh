// Port of native/android/.../crypto/X25519CryptoTest.kt: RFC 7748 §5.2 and §6.1 vectors plus
// the stored-key clamping.
import XCTest
@testable import FMCrypto

final class X25519Tests: XCTestCase {
    func testRfc7748ScalarMultiplication() throws {
        assertHex(
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
            try X25519.scalarMult(
                scalar: unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                u: unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")))
        // The peer key of this vector has its top bit set, which RFC 7748 says to mask.
        assertHex(
            "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
            try X25519.scalarMult(
                scalar: unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
                u: unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")))
    }

    func testRfc7748Iterations() throws {
        var k = [UInt8](repeating: 0, count: 32)
        k[0] = 9
        var u = k
        for i in 1...1000 {
            let result = try X25519.scalarMultPortable(scalar: k, u: u)
            u = k
            k = result
            if i == 1 { assertHex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", k) }
        }
        assertHex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", k)
    }

    func testRfc7748DiffieHellman() throws {
        let alice = unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        let bob = unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
        let alicePublic = try X25519.publicKey(privateKey: alice)
        let bobPublic = try X25519.publicKey(privateKey: bob)
        assertHex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", alicePublic)
        assertHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bobPublic)
        let shared = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"
        assertHex(shared, try X25519.sharedSecret(privateKey: alice, remotePublicKey: bobPublic))
        assertHex(shared, try X25519.sharedSecret(privateKey: bob, remotePublicKey: alicePublic))
    }

    func testClampingMatchesDart() throws {
        let seed = [UInt8](repeating: 0xff, count: 32)
        let clamped = try X25519.clampPrivateKey(seed)
        assertHex("f8" + String(repeating: "ff", count: 30) + "7f", clamped)
        XCTAssertEqual(try X25519.publicKey(privateKey: seed), try X25519.publicKey(privateKey: clamped))
        for _ in 0..<20 {
            let key = X25519.generatePrivateKey()
            XCTAssertTrue(key[0] & 7 == 0 && key[31] & 0xc0 == 0x40)
        }
        XCTAssertThrowsError(try X25519.clampPrivateKey([1]))
        XCTAssertThrowsError(try X25519.sharedSecret(privateKey: seed, remotePublicKey: [1]))
        XCTAssertThrowsError(try X25519.sharedSecret(privateKey: [1], remotePublicKey: seed))
    }

    func testHighBitOfPeerKeyIsIgnoredAndSmallOrderYieldsZero() throws {
        let k = X25519.generatePrivateKey()
        let u = try X25519.publicKey(privateKey: X25519.generatePrivateKey())
        var high = u
        high[31] |= 0x80
        XCTAssertEqual(try X25519.sharedSecret(privateKey: k, remotePublicKey: u), try X25519.sharedSecret(privateKey: k, remotePublicKey: high))
        XCTAssertTrue(CryptoBytes.isAllZero(try X25519.sharedSecret(privateKey: k, remotePublicKey: [UInt8](repeating: 0, count: 32))))
        var one = [UInt8](repeating: 0, count: 32)
        one[0] = 1
        XCTAssertTrue(CryptoBytes.isAllZero(try X25519.sharedSecret(privateKey: k, remotePublicKey: one)))
    }

    func testCanonicalUCheck() {
        XCTAssertTrue(X25519.isCanonicalU(unhex("ec" + String(repeating: "ff", count: 30) + "7f")))
        XCTAssertFalse(X25519.isCanonicalU(unhex("ed" + String(repeating: "ff", count: 30) + "7f")))
        XCTAssertFalse(X25519.isCanonicalU(unhex("09" + String(repeating: "00", count: 30) + "80")))
        XCTAssertTrue(X25519.isCanonicalU(unhex("ff" + String(repeating: "fe", count: 30) + "7f")))
    }
}
