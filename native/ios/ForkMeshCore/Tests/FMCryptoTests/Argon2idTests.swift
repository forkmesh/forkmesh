// RFC 7693 and RFC 9106 known answers, then the Argon2id master keys pinned by the vault's
// cross-client fixtures
// (marketplace/packages/vault/tests/fixtures/interop-from-{dart,js,cpp}.json).
import XCTest
@testable import FMCrypto

final class Argon2idTests: XCTestCase {
    func testBlake2b() {
        assertHex(
            "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923",
            Blake2b.hash(outputLength: 64, ascii("abc")))
        assertHex(
            "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce",
            Blake2b.hash(outputLength: 64))
        // Python hashlib.blake2b(bytes(range(256)) * 2, digest_size=32): block-boundary handling.
        let message = (0..<512).map { UInt8(truncatingIfNeeded: $0) }
        var split = Blake2b(outputLength: 32)
        split.update(Array(message[0..<128]))
        split.update(Array(message[128..<129]))
        split.update(Array(message[129...]))
        XCTAssertEqual(Blake2b.hash(outputLength: 32, message), split.finalize())
        assertHex("540b20132d8aeae54057cb69c24f95d26a1c472cc700dd450defe9bb796d4f14", Blake2b.hash(outputLength: 32, message))
    }

    /// RFC 9106 §5.3.
    func testRfc9106Argon2id() throws {
        let tag = try Argon2id.deriveKey(
            password: [UInt8](repeating: 1, count: 32), salt: [UInt8](repeating: 2, count: 16),
            parallelism: 4, memoryKiB: 32, iterations: 3, hashLength: 32,
            secret: [UInt8](repeating: 3, count: 8), associatedData: [UInt8](repeating: 4, count: 12))
        assertHex("0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", tag)
    }

    func testArgumentsAreChecked() {
        let salt = [UInt8](repeating: 2, count: 16)
        XCTAssertThrowsError(try Argon2id.deriveKey(password: [], salt: salt, parallelism: 0, memoryKiB: 32, iterations: 1, hashLength: 32))
        XCTAssertThrowsError(try Argon2id.deriveKey(password: [], salt: salt, parallelism: 1, memoryKiB: 7, iterations: 1, hashLength: 32))
        XCTAssertThrowsError(try Argon2id.deriveKey(password: [], salt: salt, parallelism: 1, memoryKiB: 32, iterations: 0, hashLength: 32))
        XCTAssertThrowsError(try Argon2id.deriveKey(password: [], salt: salt, parallelism: 1, memoryKiB: 32, iterations: 1, hashLength: 3))
        XCTAssertThrowsError(try Argon2id.deriveKey(password: [], salt: [1], parallelism: 1, memoryKiB: 32, iterations: 1, hashLength: 32))
    }

    /// The vault fixtures: 19456 KiB, 2 passes, 1 lane, over the NFKC form of the password
    /// ("correct horse battery staple " then U+212B, which NFKC folds to U+00C5).
    func testVaultInteropMasterKeys() throws {
        let password = unhex("636f727265637420686f727365206261747465727920737461706c6520c385")
        let fixtures = [
            ("aaea9c7d58df3d224ca1b5c28006a8e6", "72f7a1d807d76085317a5ad99bd41e19edd06056daf492bfdc442281693748d9"), // dart
            ("c965899994a76f932cb4ed2f723d9fc3", "ff494b1c99c6936b0fed6c847cd4e0254d3c018d7510ce38862dee172b5c0b2c"), // js
            ("817380015de47f779f638bd6a17fc101", "7b05a2485c6bef463b933a3f389e2658311c88911adb0004ea5ad45bb0342b9f"), // cpp
        ]
        for (salt, masterKey) in fixtures {
            assertHex(masterKey, try Argon2id.deriveKey(
                password: password, salt: unhex(salt), parallelism: 1, memoryKiB: 19456, iterations: 2, hashLength: 32))
        }
    }

    /// Output lengths either side of H' switching from one BLAKE2b call to a chain.
    func testLongHashLengths() {
        for length in [4, 32, 64, 65, 96, 97, 128, 1024] {
            XCTAssertEqual(length, Argon2id.longHash(length, ascii("x")).count)
        }
        XCTAssertNotEqual(Argon2id.longHash(64, ascii("x")), Array(Argon2id.longHash(65, ascii("x"))[0..<64]))
    }
}
