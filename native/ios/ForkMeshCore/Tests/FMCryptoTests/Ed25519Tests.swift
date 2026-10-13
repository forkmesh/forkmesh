// Port of native/android/.../crypto/Ed25519CryptoTest.kt: RFC 8032 vectors, tampering, strict
// decoding, and the limb arithmetic against big-integer results. Kotlin checks the arithmetic
// against java.math.BigInteger; there is none here, so the expected values were computed with
// Python integers over inputs drawn from SHAKE-256, and are pinned as digests.
import XCTest
@testable import FMCrypto

final class Ed25519Tests: XCTestCase {
    private let rfc8032: [[String]] = [
        [
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        ],
        [
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "72",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        ],
        [
            "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "af82",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
        ],
        // RFC 8032 "TEST SHA(abc)": the message is SHA-512("abc").
        [
            "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
            "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704",
        ],
    ]

    /// L = 2^252 + 27742317777372353535851937790883648493, little endian.
    private let order = unhex("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010")

    func testRfc8032Vectors() throws {
        for vector in rfc8032 {
            let seed = unhex(vector[0]), message = unhex(vector[1]), publicKey = unhex(vector[2]), signature = unhex(vector[3])
            assertHex(vector[2], try Ed25519.publicKey(seed: seed))
            assertHex(vector[3], try Ed25519.sign(seed: seed, message: message))
            assertHex(vector[3], try Ed25519.sign(seed: seed, message: message, publicKey: publicKey))
            XCTAssertTrue(Ed25519.verify(publicKey: publicKey, message: message, signature: signature))
            XCTAssertTrue(Ed25519.verifyStrict(publicKey: publicKey, message: message, signature: signature))
        }
        XCTAssertThrowsError(try Ed25519.publicKey(seed: [1, 2, 3]))
        XCTAssertThrowsError(try Ed25519.sign(seed: [1, 2, 3], message: []))
        XCTAssertThrowsError(try Ed25519.sign(seed: unhex(rfc8032[0][0]), message: [], publicKey: [1]))
        XCTAssertEqual(32, Ed25519.generateSeed().count)
    }

    func testSignaturesVerifyAndTamperingFails() throws {
        var random = JavaRandom(1234)
        for round in 0..<60 {
            let seed = random.nextBytes(32)
            let message = random.nextBytes(round * 7)
            let publicKey = try Ed25519.publicKey(seed: seed)
            let signature = try Ed25519.sign(seed: seed, message: message)
            XCTAssertTrue(Ed25519.verify(publicKey: publicKey, message: message, signature: signature))
            XCTAssertTrue(Ed25519.verifyStrict(publicKey: publicKey, message: message, signature: signature))
            var badSignature = signature
            let index = random.nextInt(64)
            badSignature[index] ^= UInt8(1 << random.nextInt(8))
            XCTAssertFalse(Ed25519.verify(publicKey: publicKey, message: message, signature: badSignature), "tampered signature \(round)")
            if !message.isEmpty {
                var badMessage = message
                badMessage[0] ^= 1
                XCTAssertFalse(Ed25519.verify(publicKey: publicKey, message: badMessage, signature: signature), "tampered message \(round)")
            }
            XCTAssertFalse(Ed25519.verify(publicKey: try Ed25519.publicKey(seed: random.nextBytes(32)), message: message, signature: signature))
            XCTAssertFalse(Ed25519.verify(publicKey: publicKey, message: message, signature: Array(signature[0..<63])))
        }
    }

    /// Signing is deterministic (RFC 8032): the same seed and message always give the same bytes.
    func testSigningIsDeterministic() throws {
        let seed = [UInt8](repeating: 7, count: 32)
        let message = ascii("pinned across clients")
        XCTAssertEqual(try Ed25519.sign(seed: seed, message: message), try Ed25519.sign(seed: seed, message: message))
    }

    func testNonCanonicalScalarIsRejected() throws {
        let seed = [UInt8](repeating: 5, count: 32)
        let message = ascii("malleable")
        let publicKey = try Ed25519.publicKey(seed: seed)
        let signature = try Ed25519.sign(seed: seed, message: message)
        // S + L: the same point equation, a non-canonical encoding.
        var forged = signature
        var carry = 0
        for i in 0..<32 {
            let sum = Int(signature[32 + i]) + Int(order[i]) + carry
            forged[32 + i] = UInt8(sum & 0xff)
            carry = sum >> 8
        }
        XCTAssertEqual(0, carry)
        XCTAssertFalse(Ed25519.verify(publicKey: publicKey, message: message, signature: forged))
        XCTAssertFalse(Ed25519.verifyStrict(publicKey: publicKey, message: message, signature: forged))
        XCTAssertFalse(Ed25519.isCanonicalScalar(order))
        var below = order
        below[0] -= 1
        XCTAssertTrue(Ed25519.isCanonicalScalar(below))
        XCTAssertFalse(Ed25519.isCanonicalScalar([1, 2, 3]))
    }

    func testStrictVerificationRejectsSmallOrderKeys() throws {
        let identity = unhex("01" + String(repeating: "00", count: 31))
        let trivial = identity + [UInt8](repeating: 0, count: 32) // R = identity, S = 0
        let message = ascii("anything")
        XCTAssertTrue(Ed25519.verify(publicKey: identity, message: message, signature: trivial), "RFC 8032 cofactorless verification accepts the identity key")
        XCTAssertFalse(Ed25519.verifyStrict(publicKey: identity, message: message, signature: trivial), "strict verification rejects it")
        let order2 = unhex("ec" + String(repeating: "ff", count: 30) + "7f")
        XCTAssertFalse(Ed25519.verifyStrict(publicKey: order2, message: message, signature: trivial))
        let seed = [UInt8](repeating: 9, count: 32)
        XCTAssertTrue(Ed25519.verifyStrict(publicKey: try Ed25519.publicKey(seed: seed), message: message, signature: try Ed25519.sign(seed: seed, message: message)))
    }

    func testNonCanonicalPointEncodingsAreRejected() {
        // Both decode to the identity under a lax decoder, where R = identity, S = 0 would verify.
        let message = ascii("lax")
        let identity = unhex("01" + String(repeating: "00", count: 31))
        let zeros = [UInt8](repeating: 0, count: 32)
        for encoding in ["ee" + String(repeating: "ff", count: 30) + "7f", "01" + String(repeating: "00", count: 30) + "80"] {
            let key = unhex(encoding)
            XCTAssertFalse(Ed25519.verify(publicKey: key, message: message, signature: identity + zeros), encoding)
            XCTAssertFalse(Ed25519.verify(publicKey: identity, message: message, signature: key + zeros), "non-canonical R \(encoding)")
        }
        // y with no square root: not a curve point.
        XCTAssertFalse(Ed25519.verify(publicKey: unhex("02" + String(repeating: "00", count: 31)), message: message, signature: [UInt8](repeating: 0, count: 64)))
    }

    func testFieldArithmeticMatchesBigIntegers() {
        let pMinus1 = unhex("ec" + String(repeating: "ff", count: 30) + "7f")
        var pMinus2 = pMinus1
        pMinus2[0] = 0xeb
        var p = pMinus1
        p[0] = 0xed
        var pPlus1 = pMinus1
        pPlus1[0] = 0xee
        var one = [UInt8](repeating: 0, count: 32)
        one[0] = 1
        var top = [UInt8](repeating: 0, count: 32)
        top[31] = 0x40
        var samples: [[UInt8]] = [
            [UInt8](repeating: 0, count: 32), one, pMinus1, pMinus2,
            [UInt8](repeating: 0xff, count: 31) + [0x7f], p, pPlus1, top,
        ]
        var stream = Keccak.shake256()
        stream.update(ascii("forkmesh fe test"))
        for _ in 0..<300 { samples.append(stream.squeeze(32)) }

        var outputs = [[UInt8]]()
        for (index, x) in samples.enumerated() {
            let a = Fe(bytes: x)
            let b = Fe(bytes: samples[(index * 7 + 3) % samples.count])
            outputs.append(Fe.mul(a, b).toBytes())
            outputs.append(Fe.add(a, b).toBytes())
            outputs.append(Fe.sub(a, b).toBytes())
            outputs.append(Fe.sq(a).toBytes())
            outputs.append(Fe.neg(a).toBytes())
            outputs.append(Fe.mulSmall(a, 121665).toBytes())
            outputs.append(Fe.invert(a).toBytes())
            outputs.append(Fe.pow22523(a).toBytes())
        }
        assertHex("d175e5ed1ac8d1cb93fb314ba28c078de07a86064a891842600f75effa0fcc05", Sha256.hash(parts: outputs))
        // A few readable ones, so a digest mismatch has somewhere to start.
        assertHex(String(repeating: "00", count: 32), Fe(bytes: p).toBytes())
        assertHex("01" + String(repeating: "00", count: 31), Fe(bytes: pPlus1).toBytes())
        assertHex("01" + String(repeating: "00", count: 31), Fe.mul(Fe(bytes: pMinus1), Fe(bytes: pMinus1)).toBytes())
        assertHex("01" + String(repeating: "00", count: 31), Fe.mul(Fe(bytes: samples[20]), Fe.invert(Fe(bytes: samples[20]))).toBytes())

        // Long chains keep limbs bounded.
        var a = Fe(bytes: samples[8])
        let b = Fe(bytes: samples[9])
        for _ in 0..<2000 {
            let product = Fe.mul(Fe.add(a, b), Fe.sub(a, b))
            a = Fe.add(product, product)
        }
        assertHex("70c2220f4dc50593a3dc37e581942b6460a2c4e70a7c6824ce48c4e27c967a52", a.toBytes())
    }

    func testScalarReductionMatchesBigIntegers() {
        var wides: [[UInt8]] = [
            [UInt8](repeating: 0, count: 64), [UInt8](repeating: 0xff, count: 64),
            order + [UInt8](repeating: 0, count: 32),
        ]
        var below = order
        below[0] -= 1
        wides.append(below + [UInt8](repeating: 0, count: 32))
        var doubled = [UInt8](repeating: 0, count: 64)
        var carry = 0
        for i in 0..<32 {
            let sum = 2 * Int(order[i]) + carry
            doubled[i] = UInt8(sum & 0xff)
            carry = sum >> 8
        }
        doubled[32] = UInt8(carry)
        wides.append(doubled)
        var stream = Keccak.shake256()
        stream.update(ascii("forkmesh scalar test"))
        for _ in 0..<500 { wides.append(stream.squeeze(64)) }
        assertHex("3df09ce84443bec5756aefaa5bc9e9ef71c0d040f390c7899bda00e66efe8033", Sha256.hash(parts: wides.map { Ed25519.reduce($0) }))
        assertHex(String(repeating: "00", count: 32), Ed25519.reduce(wides[2]))
        XCTAssertEqual(below, Ed25519.reduce(wides[3]))

        stream = Keccak.shake256()
        stream.update(ascii("forkmesh muladd test"))
        var results = [[UInt8]]()
        let zeros = [UInt8](repeating: 0, count: 32)
        for _ in 0..<500 {
            let k = Ed25519.reduce(stream.squeeze(32) + zeros)
            let r = Ed25519.reduce(stream.squeeze(32) + zeros)
            var a = stream.squeeze(32)
            a[0] &= 248
            a[31] = (a[31] & 127) | 64
            results.append(Ed25519.mulAdd(k, a, r))
        }
        assertHex("97514b3216a472dff46f9ae0c581eeb7da543408c0bf537be815fe91f5b0a128", Sha256.hash(parts: results))
    }
}
