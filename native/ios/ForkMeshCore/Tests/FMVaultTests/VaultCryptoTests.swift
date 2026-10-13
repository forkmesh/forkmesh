// Vault crypto (stretch floor, normalization, generator, recovery codes, site matching), the
// protocol checks of marketplace/packages/vault/tests/crypto_vectors.js, and its OpenSSL known
// answers (tests/fixtures/openssl-vectors.json) for the two primitives the suite leans on hardest:
// Argon2id and ML-KEM-768.
import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMVault

final class VaultCryptoTests: XCTestCase {
    // MARK: OpenSSL known answers

    func testArgon2idAgainstOpenSSL() throws {
        let vectors = JSON.parseObject(VaultFixtures.openssl)
        var sawDefault = false
        for v in vectors.maps("argon2id") {
            let key = try Argon2id.deriveKey(
                password: Array(v.str("password").utf8), salt: VaultCrypto.fromHex(v.str("salt"))!,
                parallelism: v.int("parallelism"), memoryKiB: v.int("memoryKiB"),
                iterations: v.int("passes"), hashLength: v.int("length"))
            XCTAssertEqual(VaultCrypto.toHex(key), v.str("key"), "m=\(v.int("memoryKiB")) t=\(v.int("passes")) p=\(v.int("parallelism"))")
            let fresh = VaultKdf.fresh()
            if v.int("memoryKiB") == fresh.memoryKiB && v.int("passes") == fresh.passes && v.int("parallelism") == fresh.parallelism {
                sawDefault = true
            }
        }
        XCTAssertTrue(sawDefault, "the default stretch is one of the vectors")
    }

    func testMlKemAgainstOpenSSL() throws {
        let vectors = JSON.parseObject(VaultFixtures.openssl)
        XCTAssertFalse(vectors.maps("mlkem768").isEmpty)
        for v in vectors.maps("mlkem768") {
            let pair = try MlKem768.generateKeyPair(seed: VaultCrypto.fromHex(v.str("seed"))!)
            XCTAssertEqual(VaultCrypto.toHex(pair.publicKey), v.str("publicKey"), "keygen public")
            XCTAssertEqual(Sha256.hex(pair.secretKey), v.str("secretKeySha256"), "keygen secret")
            XCTAssertEqual(VaultCrypto.toHex(try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: VaultCrypto.fromHex(v.str("ciphertext"))!)),
                           v.str("sharedSecret"), "decapsulate")
            XCTAssertEqual(VaultCrypto.toHex(try MlKem768.decapsulate(secretKey: pair.secretKey, ciphertext: VaultCrypto.fromHex(v.str("tamperedCiphertext"))!)),
                           v.str("rejectionSecret"), "implicit rejection")
            // A key with an unreduced coefficient is refused by the bundle check.
            var unreduced = pair.publicKey
            unreduced[0] = 0xff
            unreduced[1] |= 0x0f
            XCTAssertTrue(VaultBundle.mlkemKeyReduced(pair.publicKey))
            XCTAssertFalse(VaultBundle.mlkemKeyReduced(unreduced))
        }
    }

    // MARK: Stretch settings

    func testWeakOrMalformedStretchSettingsAreRefused() {
        let good = VaultKdf.fresh().toJSON()
        XCTAssertEqual(good["memoryKiB"], 65536)
        XCTAssertEqual(good["passes"], 3)
        XCTAssertEqual(JSON.encode(good).hasPrefix(#"{"algorithm":"argon2id","version":19,"memoryKiB":65536,"passes":3,"parallelism":1,"salt":""#), true)
        XCTAssertNoThrow(try VaultKdf(json: .object(good)))
        let weak: [JSONObject] = [
            ["memoryKiB": 1024],
            ["passes": 1],
            ["algorithm": "pbkdf2"],
            ["version": 16],
            ["salt": "AAAA"],
            ["memoryKiB": 65536.5],
            ["memoryKiB": 65536.0],
            ["parallelism": 0],
            ["passes": 17],
            ["salt": "not base64!"],
        ]
        for change in weak {
            var kdf = good
            kdf.merge(change)
            expectCode("unsafe_kdf_parameters") { _ = try VaultKdf(json: .object(kdf)) }
        }
        var asDouble = good
        asDouble["version"] = 19.0
        XCTAssertNoThrow(try VaultKdf(json: .object(asDouble)), "Dart's 19.0 == 19")
    }

    func testANonAsciiPasswordIsNeverStretchedUnNormalized() {
        expectCode("normalization_unavailable") { _ = try VaultCrypto.stretch("päss", VaultKdf.fresh(), normalizer: nil) }
    }

    func testStretchingNormalizesUnicode() throws {
        let kdf = VaultKdf.fresh(memoryKiB: VaultKdf.floorMemoryKiB, passes: VaultKdf.floorPasses)
        let composed = try VaultCrypto.stretch("pa\u{DF}word \u{3A9}", kdf)
        XCTAssertEqual(try VaultCrypto.stretch("pa\u{DF}word \u{2126}", kdf), composed, "Ohm sign folds to Omega")
        XCTAssertEqual(try VaultCrypto.nfkc("\u{212B}").unicodeScalars.map(\.value), [0xC5])
    }

    // MARK: Protocol bindings (crypto_vectors.js)

    func testEveryBindingABlobCarriesIsEnforced() throws {
        let alice = VaultKeyset.generate(), bob = VaultKeyset.generate()
        let aliceBundle = try VaultBundle.of(alice, account: "alice")
        let bobBundle = try VaultBundle.of(bob, account: "bob")
        let id = VaultCrypto.newItemId(), otherId = VaultCrypto.newItemId(), itemKey = VaultCrypto.randomBytes(32)

        let body = try VaultCrypto.sealItem(itemKey, itemId: id, fields: ["title": "t", "password": "p"])
        XCTAssertEqual(try VaultCrypto.openItem(itemKey, itemId: id, blob: .string(body)).str("password"), "p")
        XCTAssertEqual(try VaultCrypto.sealItem(itemKey, itemId: id, fields: ["title": "t", "password": .string(String(repeating: "p", count: 60))]).count,
                       body.count, "ciphertext length hides the password length")
        expectCode("decryption_failed") { _ = try VaultCrypto.openItem(itemKey, itemId: otherId, blob: .string(body)) }
        var flipped = try VaultCrypto.fromBase64(body)
        flipped[flipped.count - 1] ^= 1
        expectCode("decryption_failed") { _ = try VaultCrypto.openItem(itemKey, itemId: id, blob: .string(VaultCrypto.toBase64(flipped))) }
        expectCode("invalid_item_id") { _ = try VaultCrypto.openItem(itemKey, itemId: "ABC", blob: .string(body)) }
        expectCode("invalid_encoding") { _ = try VaultCrypto.openItem(itemKey, itemId: id, blob: "a+b/") }
        expectCode("invalid_ciphertext") { _ = try VaultCrypto.openItem(itemKey, itemId: id, blob: "AAAA") }
        expectCode("invalid_key") { _ = try VaultCrypto.openItem([1, 2, 3], itemId: id, blob: .string(body)) }

        let wrapped = try VaultCrypto.wrapItemKey(alice, itemId: id, itemKey: itemKey)
        expectCode("decryption_failed") { _ = try VaultCrypto.unwrapItemKey(alice, itemId: otherId, blob: .string(wrapped)) }
        expectCode("decryption_failed") { _ = try VaultCrypto.unwrapItemKey(bob, itemId: id, blob: .string(wrapped)) }

        let grant = try VaultCrypto.sealGrant(keyset: alice, sender: "alice", recipient: bobBundle, itemId: id, itemKey: itemKey, permission: "read")
        XCTAssertEqual(Array(grant.keys), ["v", "suite", "x25519", "mlkem768", "wrapped", "signature"])
        XCTAssertEqual(try VaultCrypto.openGrant(keyset: bob, recipient: "bob", sender: aliceBundle, itemId: id, grant: .object(grant), permission: "read"), itemKey)
        expectCode("invalid_permission") {
            _ = try VaultCrypto.sealGrant(keyset: alice, sender: "alice", recipient: bobBundle, itemId: id, itemKey: itemKey, permission: "admin")
        }
        expectCode("invalid_share") {
            _ = try VaultCrypto.sealGrant(keyset: alice, sender: "alice", recipient: bobBundle, itemId: id, itemKey: [1], permission: "read")
        }
        expectCode("invalid_share") {
            _ = try VaultCrypto.openGrant(keyset: bob, recipient: "bob", sender: aliceBundle, itemId: id, grant: ["v": 2], permission: "read")
        }
        XCTAssertNotEqual(aliceBundle.fingerprint(), bobBundle.fingerprint(), "fingerprints differ per account")
        XCTAssertTrue(aliceBundle.fingerprint().range(of: #"^([0-9a-f]{4} ){15}[0-9a-f]{4}$"#, options: .regularExpression) != nil)

        // Master password wrapping.
        let kdf = VaultKdf.fresh(memoryKiB: VaultKdf.floorMemoryKiB, passes: VaultKdf.floorPasses)
        let masterKey = try VaultCrypto.stretch("paßword Ω", kdf)
        let blob = try VaultCrypto.wrapKeyset(masterKey, purpose: "master", account: "Alice ", keyset: alice)
        XCTAssertEqual(try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "alice", blob: .string(blob)).vaultKey, alice.vaultKey)
        expectCode("wrong_password") { _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "bob", blob: .string(blob)) }
        expectCode("wrong_password") { _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "recovery", account: "alice", blob: .string(blob)) }
        expectCode("wrong_password") { _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "alice", blob: "!!") }
        expectCode("invalid_account") { _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "-x", blob: .string(blob)) }
        // A keyset of another version sealed under the right key is reported as such.
        let kek = VaultCrypto.hkdf(masterKey, "keyset-kek|master")
        let future = try VaultCrypto.seal(kek, "keyset|master|alice", Array(#"{"v":2}"#.utf8))
        expectCode("unsupported_keyset") { _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "alice", blob: .string(future)) }
        let short = try VaultCrypto.seal(kek, "keyset|master|alice", Array(#"{"v":1,"vaultKey":"AA","x25519":"AA","mlkemSeed":"AA","ed25519":"AA"}"#.utf8))
        expectCode("invalid_keyset") { _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "alice", blob: .string(short)) }
    }

    func testKeysetJsonRoundTripsAndWipes() throws {
        let keyset = VaultKeyset.generate()
        let json = keyset.toJSON()
        XCTAssertEqual(Array(json.keys), ["v", "vaultKey", "x25519", "mlkemSeed", "ed25519"])
        XCTAssertEqual(try VaultKeyset(json: .object(json)).toJSON(), json)
        expectCode("unsupported_keyset") { _ = try VaultKeyset(json: nil) }
        _ = keyset.mlkem
        keyset.wipe()
        XCTAssertTrue(CryptoBytes.isAllZero(keyset.vaultKey + keyset.x25519 + keyset.mlkemSeed + keyset.ed25519))
    }

    // MARK: Generator, recovery codes, sites

    func testGenerator() throws {
        var seen = Set<String>()
        for _ in 0..<200 {
            let password = try VaultCrypto.generatePassword(length: 16)
            XCTAssertEqual(password.count, 16)
            XCTAssertTrue(password.contains { $0.isASCII && $0.isLowercase })
            XCTAssertTrue(password.contains { $0.isASCII && $0.isUppercase })
            XCTAssertTrue(password.contains { $0.isASCII && $0.isNumber })
            XCTAssertTrue(password.contains { !($0.isASCII && ($0.isLetter || $0.isNumber)) })
            XCTAssertFalse(password.contains { "lIO01".contains($0) })
            seen.insert(password)
        }
        XCTAssertEqual(seen.count, 200)
        XCTAssertEqual(try VaultCrypto.generatePassword().count, 24)
        XCTAssertEqual(try VaultCrypto.generatePassword(length: 2).count, 8)
        XCTAssertEqual(try VaultCrypto.generatePassword(length: 500).count, 128)
        let digits = try VaultCrypto.generatePassword(length: 8, lower: false, upper: false, symbols: false)
        XCTAssertTrue(digits.allSatisfy { "23456789".contains($0) }, "digits only when asked")
        expectCode("no_character_classes") { _ = try VaultCrypto.generatePassword(lower: false, upper: false, digits: false, symbols: false) }
    }

    func testRecoveryCodes() throws {
        let code = VaultCrypto.newRecoveryCode()
        XCTAssertTrue(code.range(of: #"^([0-9A-Z]{4}-){12}[0-9A-Z]{4}$"#, options: .regularExpression) != nil, code)
        XCTAssertEqual(try VaultCrypto.recoverySecret(code).count, 32)
        let loose = code.lowercased().replacingOccurrences(of: "-", with: " ")
            .replacingOccurrences(of: "0", with: "o").replacingOccurrences(of: "1", with: "l")
        XCTAssertEqual(try VaultCrypto.recoverySecret(loose), try VaultCrypto.recoverySecret(code))
        // All ones: 256 set bits then four zero padding bits.
        let ones = VaultCrypto.recoveryCode([UInt8](repeating: 0xff, count: 32))
        XCTAssertEqual(ones, String(repeating: "ZZZZ-", count: 12) + "ZZZG")
        XCTAssertEqual(VaultCrypto.recoveryCode([UInt8](repeating: 0, count: 32)), String(repeating: "0000-", count: 12) + "0000")
        expectCode("invalid_recovery_code") { _ = try VaultCrypto.recoverySecret("not a code") }
        expectCode("invalid_recovery_code") { _ = try VaultCrypto.recoverySecret(String(code.dropLast())) }
        expectCode("invalid_recovery_code") { _ = try VaultCrypto.recoverySecret(String(code.dropLast()) + "U") }
    }

    func testSiteMatching() {
        XCTAssertEqual(VaultCrypto.loginHost("https://www.Bank.Example/x"), "bank.example")
        XCTAssertEqual(VaultCrypto.loginHost("bank.example:8443/x"), "bank.example")
        XCTAssertEqual(VaultCrypto.loginHost("http://bank.example"), "")
        XCTAssertEqual(VaultCrypto.loginHost("http://localhost:3000/a"), "localhost")
        XCTAssertEqual(VaultCrypto.loginHost("http://[::1]:3000/a"), "::1")
        XCTAssertEqual(VaultCrypto.loginHost("HTTPS://Registrar.Example"), "registrar.example")
        for bad in ["javascript:alert(1)", "data:text/html,x", "ftp://x.y", "", "   ", "file:///etc/passwd"] {
            XCTAssertEqual(VaultCrypto.loginHref(bad), "", bad)
        }
        XCTAssertEqual(VaultCrypto.loginHref("registrar.example"), "https://registrar.example")
        XCTAssertEqual(VaultCrypto.loginHref(" https://Registrar.Example/Login "), "https://registrar.example/Login")
        let saved = "https://registrar.example/login"
        XCTAssertTrue(VaultCrypto.matchesHost(saved, "registrar.example"))
        XCTAssertTrue(VaultCrypto.matchesHost(saved, "www.registrar.example"))
        for other in ["evil.example", "registrar.example.evil.test", "sub.registrar.example", "notregistrar.example", ""] {
            XCTAssertFalse(VaultCrypto.matchesHost(saved, other), other)
        }
        XCTAssertFalse(VaultCrypto.matchesHost("", ""))
    }

    func testEncodings() throws {
        XCTAssertEqual(VaultCrypto.toBase64([0xfb, 0xff]), "-_8")
        XCTAssertEqual(try VaultCrypto.fromBase64("-_8"), [0xfb, 0xff])
        XCTAssertEqual(try VaultCrypto.fromBase64(""), [])
        expectCode("invalid_encoding") { _ = try VaultCrypto.fromBase64("+/8") }
        expectCode("invalid_encoding") { _ = try VaultCrypto.fromBase64("AAAAA") }
        expectCode("invalid_encoding") { _ = try VaultCrypto.fromBase64("AA==") }
        XCTAssertEqual(try VaultCrypto.fromBase64(.int(1)), [], "a non-string reads as \"\", as in Dart")
        XCTAssertTrue(VaultCrypto.sameBytes([1, 2], [1, 2]))
        XCTAssertFalse(VaultCrypto.sameBytes([1, 2], [1, 3]))
        XCTAssertEqual(VaultCryptoError("wrong_password").description, "wrong password")
    }
}
