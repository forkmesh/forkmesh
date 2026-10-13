// Port of native/android/.../crypto/KeccakCryptoTest.kt: SHA3 / SHAKE against CPython's hashlib
// (OpenSSL / XKCP) outputs.
import XCTest
@testable import FMCrypto

final class KeccakTests: XCTestCase {
    private struct Case {
        let message: [UInt8]
        let sha3_256: String
        let sha3_512: String
        let shake128Prefix: String
        let shake128Sha256: String
        let shake256Sha256: String
    }

    private let cases = [
        Case(
            message: [],
            sha3_256: "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
            sha3_512: "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26",
            shake128Prefix: "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef263cb1eea988004b93",
            shake128Sha256: "c4ada152a345f24d530ced20f8737b392fcc3195d6e4642b4ea0e3ec38f8185e",
            shake256Sha256: "8287717d2889d82c800c5b6a68235b00bf9979ea5674380804eaecf8da0f524d"),
        Case(
            message: Array("abc".utf8),
            sha3_256: "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
            sha3_512: "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0",
            shake128Prefix: "5881092dd818bf5cf8a3ddb793fbcba74097d5c526a6d35f97b83351940f2cc844c50af32acd3f2c",
            shake128Sha256: "ea2032989d2c99e6d541114aff90d36d06198e0cbffac0e5bd10030e93ec4ad4",
            shake256Sha256: "a38e374d8a710bb8d24e623119462b82bfee28f84374d7df7f70d48f1e7263b2"),
        Case(
            message: (0..<768).map { UInt8(truncatingIfNeeded: $0) },
            sha3_256: "c043b2b15d405c9f4cd92fdaef420eba6201d328fb34ec0e2c16e4981b9e4b39",
            sha3_512: "ad3a11a3430f0fac234a6c15bff0cb609b3d6fde0eac5873893e2775d15cf47d6791b4db907361c53719da248a7662f759b3f6c7ffb4fe69492a9449728cdad7",
            shake128Prefix: "92b62d6682dda8ef27e599c00ce6fcd070dafa726908c07bf6c361ab7be2149ff7b03259d2a42cd3",
            shake128Sha256: "ab0274c3d1877e2696110f2151bcbf97165f7073c3a62e123678843135bf1c83",
            shake256Sha256: "4b3a6334658ac7d338debe613ab0132909600367ec01543efc5fd8102f17733c"),
    ]

    func testKnownDigests() {
        for c in cases {
            assertHex(c.sha3_256, Keccak.sha3_256(c.message))
            assertHex(c.sha3_512, Keccak.sha3_512(c.message))
            let shake128 = Keccak.shake128(700, c.message)
            XCTAssertEqual(c.shake128Prefix, CryptoBytes.hex(Array(shake128[0..<40])))
            assertHex(c.shake128Sha256, Sha256.hash(shake128))
            assertHex(c.shake256Sha256, Sha256.hash(Keccak.shake256(300, c.message)))
        }
    }

    func testIncrementalAbsorbAndSqueeze() {
        let message = (0..<1000).map { UInt8(truncatingIfNeeded: $0 * 31) }
        let oneShot = Keccak.shake128(1200, message)
        var sponge = Keccak.shake128()
        var offset = 0
        for chunk in [1, 167, 168, 169, 3, 492] {
            sponge.update(message, from: offset, length: chunk)
            offset += chunk
        }
        var out = [UInt8]()
        for chunk in [3, 165, 168, 1, 504, 359] { out += sponge.squeeze(chunk) }
        XCTAssertEqual(oneShot, out)
        XCTAssertEqual(Keccak.sha3_256(message), Keccak.sha3_256(Array(message[0..<500]), Array(message[500...])))
    }
}
