// Port of native/android/.../crypto/PrimitivesCryptoTest.kt (the hash and KDF cases): RFC 4231,
// RFC 5869, RFC 7914 and pyca-generated vectors.
import XCTest
@testable import FMCrypto

final class HashesTests: XCTestCase {
    func testSha2() {
        assertHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Sha256.hash(ascii("abc")))
        XCTAssertEqual("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Sha256.hex(ascii("a"), ascii("bc")))
        assertHex(
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            Sha512.hash(ascii("abc")))
        assertHex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", Sha256.hash())
        assertHex(
            "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
            Sha512.hash())
        // FIPS 180-4 two-block messages.
        assertHex(
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            Sha256.hash(ascii("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))
        assertHex(
            "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
            Sha512.hash(ascii("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")))
    }

    /// Every split of a message across `update` calls hashes like the one-shot, over the padding edges.
    func testSha2Chunking() {
        var random = JavaRandom(3)
        for size in [0, 1, 55, 56, 63, 64, 65, 111, 112, 119, 120, 127, 128, 129, 255, 256, 1000] {
            let message = random.nextBytes(size)
            let whole256 = Sha256.hashPortable(parts: [message])
            let whole512 = Sha512.hashPortable(parts: [message])
            for cut in [0, 1, size / 3, size / 2, max(size - 1, 0)] where cut <= size {
                let parts = [Array(message[0..<cut]), [], Array(message[cut...])]
                XCTAssertEqual(whole256, Sha256.hashPortable(parts: parts))
                XCTAssertEqual(whole512, Sha512.hashPortable(parts: parts))
            }
        }
        // One million 'a' (FIPS 180-4 long message).
        let million = [UInt8](repeating: 0x61, count: 1_000_000)
        assertHex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", Sha256.hashPortable(parts: [million]))
        assertHex(
            "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
            Sha512.hashPortable(parts: [million]))
    }

    func testHmacSha256() {
        assertHex(
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            HmacSha256.mac(key: ascii("Jefe"), ascii("what do ya want for nothing?")))
        assertHex("e528c4d99e6177f5841f712a143b90843299a4aa181a06501422d9ca862bd2a5", HmacSha256.mac(key: [], ascii("data")))
        // RFC 4231 case 6: a key longer than the block is hashed first.
        assertHex(
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
            HmacSha256.mac(key: [UInt8](repeating: 0xaa, count: 131), ascii("Test Using Larger Than Block-Size Key - Hash Key First")))
        assertHex(
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            HmacSha256.macPortable(key: ascii("Jefe"), parts: [ascii("what do ya "), ascii("want for nothing?")]))
    }

    func testHkdfSha256() {
        assertHex(
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
            Hkdf.sha256(
                ikm: [UInt8](repeating: 0x0b, count: 22), salt: (0..<13).map { UInt8($0) },
                info: (0..<10).map { UInt8(0xf0 + $0) }, length: 42))
        assertHex(
            "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d",
            Hkdf.sha256(ikm: [UInt8](repeating: 0x0b, count: 22), salt: [], info: [], length: 32))
    }

    func testPbkdf2HmacSha256() {
        let salt16 = ascii("saltsaltsaltsalt")
        assertHex(
            "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
            Pbkdf2.hmacSha256(password: ascii("passwd"), salt: ascii("salt"), iterations: 1, length: 64))
        assertHex(
            "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d",
            Pbkdf2.hmacSha256(password: ascii("Password"), salt: ascii("NaCl"), iterations: 80000, length: 64))
        assertHex(
            "c75fbc54c506766c5456c7738010488ab4cf91311ff441fb0d994dfb78a7d830",
            Pbkdf2.hmacSha256(password: Array("pässwörd-😀".utf8), salt: salt16, iterations: 1000, length: 32))
        assertHex(
            "b88f67b6a828cb87ec03d16d8243f678544a017f2526f0e5e5b60a83369dde09",
            Pbkdf2.hmacSha256(password: [], salt: salt16, iterations: 1000, length: 32))
        let password = ascii("correct horse battery staple")
        XCTAssertEqual(
            Pbkdf2.hmacSha256Portable(password: password, salt: salt16, iterations: 2000, length: 32),
            Pbkdf2.hmacSha256(password: password, salt: salt16, iterations: 2000, length: 32))
        // A length that is not a multiple of the block, and a password longer than the HMAC block.
        assertHex(
            "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf1",
            Pbkdf2.hmacSha256Portable(password: ascii("passwd"), salt: ascii("salt"), iterations: 1, length: 37))
    }

    func testConstantTimeHelpers() {
        XCTAssertTrue(CryptoBytes.constantTimeEquals([1, 2, 3], [1, 2, 3]))
        XCTAssertFalse(CryptoBytes.constantTimeEquals([1, 2, 3], [1, 2, 4]))
        XCTAssertFalse(CryptoBytes.constantTimeEquals([1, 2, 3], [1, 2]))
        XCTAssertTrue(CryptoBytes.isAllZero([UInt8](repeating: 0, count: 32)))
        var high = [UInt8](repeating: 0, count: 32)
        high[31] = 0x80
        XCTAssertFalse(CryptoBytes.isAllZero(high))
        XCTAssertEqual("00ff7f80", CryptoBytes.hex([0, 0xff, 0x7f, 0x80]))
        XCTAssertEqual([0, 0xff, 0x7f, 0x80], CryptoBytes.fromHex("00FF7f80"))
        XCTAssertNil(CryptoBytes.fromHex("0g"))
        XCTAssertNil(CryptoBytes.fromHex("abc"))
        CryptoBytes.wipe(&high)
        XCTAssertTrue(CryptoBytes.isAllZero(high))
        XCTAssertEqual(CryptoBytes.random(33).count, 33)
        XCTAssertNotEqual(CryptoBytes.random(32), CryptoBytes.random(32))
    }
}
