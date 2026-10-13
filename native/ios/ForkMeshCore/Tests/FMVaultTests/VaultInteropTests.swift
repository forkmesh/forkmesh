// The fmv1 suite against the browser's, the desktop's and the retired Flutter app's. Opens every
// fixture in VaultFixtures (and any interop-from-*.json the repository holds now that differs),
// round-trips its own output, and refuses every tampering they refuse. VAULT_WRITE_INTEROP=1 writes
// interop-from-swift.json next to them for the browser's test (tests/interop_check.js) to open in
// turn.
import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMVault

/// The fixtures use exactly one compatibility character, the Angstrom sign (U+212B).
let fixturePasswordBytes = Array("correct horse battery staple ".utf8) + [0xe2, 0x84, 0xab]

func expectCode(_ code: String, file: StaticString = #filePath, line: UInt = #line, _ action: () throws -> Void) {
    do {
        try action()
        XCTFail("accepted; expected \(code)", file: file, line: line)
    } catch let error as VaultCryptoError {
        XCTAssertEqual(error.code, code, file: file, line: line)
    } catch {
        XCTFail("threw \(error); expected \(code)", file: file, line: line)
    }
}

final class VaultInteropTests: XCTestCase {
    static var fixturesDirectory: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("marketplace/packages/vault/tests/fixtures")
    }

    func testFromJs() throws { try opensFixture(JSON.parseObject(VaultFixtures.fromJs), "js") }

    func testFromCpp() throws { try opensFixture(JSON.parseObject(VaultFixtures.fromCpp), "cpp") }

    func testFromDart() throws { try opensFixture(JSON.parseObject(VaultFixtures.fromDart), "dart") }

    /// Fixtures regenerated since they were carried here, or new clients' ones.
    func testRepositoryFixtures() throws {
        let directory = Self.fixturesDirectory
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else {
            throw XCTSkip("the repository fixtures are not reachable from here")
        }
        let carried = [VaultFixtures.fromJs, VaultFixtures.fromCpp, VaultFixtures.fromDart].map(JSON.parseObject)
        let found = names.filter { $0.hasPrefix("interop-from-") && $0.hasSuffix(".json") }.sorted()
        XCTAssertTrue(Set(found).isSuperset(of: ["interop-from-js.json", "interop-from-cpp.json"]))
        for name in found {
            let fixture = JSON.parseObject(try Data(contentsOf: directory.appendingPathComponent(name)))
            if carried.contains(fixture) { continue }
            try opensFixture(fixture, name)
        }
    }

    func testOpensWhatItWrites() throws {
        let made = try makesFixture()
        try opensFixture(made, "swift")
        if ProcessInfo.processInfo.environment["VAULT_WRITE_INTEROP"] == "1" {
            let url = Self.fixturesDirectory.appendingPathComponent("interop-from-swift.json")
            try (JSON.encodePretty(.object(made)) + "\n").write(to: url, atomically: true, encoding: .utf8)
        }
    }

    func opensFixture(_ v: JSONObject, _ name: String) throws {
        XCTAssertEqual(v.str("format"), "forkmesh-vault-interop-v1", name)
        XCTAssertEqual(v.str("suite"), VaultCrypto.suite, name)
        XCTAssertEqual(Array(v.str("password").utf8), fixturePasswordBytes, "\(name): the password keeps U+212B")
        let kdf = try VaultKdf(json: v["kdf"])
        let masterKey = try VaultCrypto.stretch(v.str("password"), kdf)
        XCTAssertEqual(VaultCrypto.toHex(masterKey), v.str("masterKey"), "\(name): Argon2id master key")

        let accounts = v.obj("accounts")
        let alice = try VaultKeyset(json: accounts.obj("alice")["keyset"])
        let bob = try VaultKeyset(json: accounts.obj("bob")["keyset"])
        let aliceBundle = try VaultBundle.verify(accounts.obj("alice")["bundle"], account: "alice")
        let bobBundle = try VaultBundle.verify(accounts.obj("bob")["bundle"], account: "bob")

        let opened = try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "alice", blob: v["wrappedKeyset"])
        XCTAssertEqual(opened.toJSON(), alice.toJSON(), name)
        expectCode("wrong_password") {
            _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "master", account: "bob", blob: v["wrappedKeyset"])
        }
        expectCode("wrong_password") {
            _ = try VaultCrypto.unwrapKeyset(masterKey, purpose: "recovery", account: "alice", blob: v["wrappedKeyset"])
        }

        let recoveryCode = v.str("recoveryCode")
        let recovery = try VaultCrypto.recoverySecret(recoveryCode)
        XCTAssertEqual(VaultCrypto.toHex(recovery), v.str("recoverySecret"), name)
        let loose = recoveryCode.lowercased().replacingOccurrences(of: "-", with: " ").replacingOccurrences(of: "0", with: "o")
        XCTAssertEqual(VaultCrypto.toHex(try VaultCrypto.recoverySecret(loose)), v.str("recoverySecret"),
                       "\(name): a loosely typed code decodes the same")
        XCTAssertEqual(VaultCrypto.recoveryCode(recovery), recoveryCode, "\(name): the code re-encodes")
        let recovered = try VaultCrypto.unwrapKeyset(recovery, purpose: "recovery", account: "alice", blob: v["wrappedRecovery"])
        XCTAssertEqual(recovered.toJSON(), alice.toJSON(), name)

        for (who, keyset) in [("alice", alice), ("bob", bob)] {
            let derived = try VaultBundle.of(keyset, account: who)
            let published = accounts.obj(who).obj("bundle")
            XCTAssertEqual(VaultCrypto.toBase64(derived.x25519), published.str("x25519"), "\(name) \(who)")
            XCTAssertEqual(VaultCrypto.toBase64(derived.mlkem768), published.str("mlkem768"),
                           "\(name) \(who): ML-KEM-768 key expands from its seed as G(d||k)")
            XCTAssertEqual(VaultCrypto.toBase64(derived.ed25519), published.str("ed25519"), "\(name) \(who)")
            XCTAssertEqual(derived.fingerprint(), accounts.obj(who).str("fingerprint"), "\(name) \(who)")
            _ = try VaultBundle.verify(.object(derived.toJSON()), account: who)
        }

        let item = v.obj("item")
        let itemId = item.str("id")
        let itemKey = try VaultCrypto.unwrapItemKey(alice, itemId: itemId, blob: item["wrappedKey"])
        XCTAssertEqual(VaultCrypto.toHex(itemKey), item.str("key"), name)
        XCTAssertEqual(try VaultCrypto.openItem(itemKey, itemId: itemId, blob: item["body"]), item.obj("fields"), name)
        XCTAssertEqual(try VaultCrypto.fromBase64(item["body"]).count % 256, 12 + 16, name)
        XCTAssertEqual(try VaultCrypto.openPrivate(alice, label: "trust", blob: v.obj("trust")["blob"]),
                       v.obj("trust").obj("value"), name)

        let grants = v.obj("grants")
        func open(
            keyset: VaultKeyset? = nil, recipient: String = "bob", sender: VaultBundle? = nil,
            id: String? = nil, grant: JSONValue? = nil, permission: String = "read"
        ) throws -> [UInt8] {
            try VaultCrypto.openGrant(
                keyset: keyset ?? bob, recipient: recipient, sender: sender ?? aliceBundle,
                itemId: id ?? itemId, grant: grant ?? grants["read"], permission: permission)
        }
        for permission in VaultCrypto.permissions {
            XCTAssertEqual(VaultCrypto.toHex(try open(grant: grants[permission], permission: permission)), item.str("key"),
                           "\(name) \(permission)")
        }
        // Every binding a share carries is enforced.
        expectCode("invalid_share_signature") { _ = try open(permission: "write") }
        expectCode("invalid_share_signature") { _ = try open(id: VaultCrypto.newItemId()) }
        expectCode("invalid_share_signature") { _ = try open(keyset: alice, recipient: "alice") }
        let forged = try VaultBundle.of(VaultKeyset.generate(), account: "alice")
        expectCode("invalid_share_signature") { _ = try open(sender: forged) }
        for field in ["x25519", "mlkem768", "wrapped"] {
            var bytes = try VaultCrypto.fromBase64(grants.obj("read")[field])
            bytes[3] ^= 1
            var tampered = grants.obj("read")
            tampered[field] = .string(VaultCrypto.toBase64(bytes))
            expectCode("invalid_share_signature") { _ = try open(grant: .object(tampered)) }
        }
        var relabelled = accounts.obj("bob").obj("bundle")
        relabelled["account"] = "mallory"
        expectCode("invalid_public_keys") { _ = try VaultBundle.verify(.object(relabelled), account: "mallory") }
        var swapped = accounts.obj("bob").obj("bundle")
        swapped["mlkem768"] = accounts.obj("alice").obj("bundle")["mlkem768"]
        expectCode("invalid_public_keys") { _ = try VaultBundle.verify(.object(swapped), account: "bob") }
        expectCode("decryption_failed") { _ = try VaultCrypto.openItem(itemKey, itemId: VaultCrypto.newItemId(), blob: item["body"]) }
        XCTAssertEqual(bobBundle.account, "bob")
    }

    func makesFixture() throws -> JSONObject {
        let password = String(decoding: fixturePasswordBytes, as: UTF8.self)
        let kdf = VaultKdf.fresh(memoryKiB: VaultKdf.floorMemoryKiB, passes: VaultKdf.floorPasses)
        let masterKey = try VaultCrypto.stretch(password, kdf)
        let alice = VaultKeyset.generate(), bob = VaultKeyset.generate()
        let aliceBundle = try VaultBundle.of(alice, account: "alice")
        let bobBundle = try VaultBundle.of(bob, account: "bob")
        let recoveryCode = VaultCrypto.newRecoveryCode()
        let itemId = VaultCrypto.newItemId(), itemKey = VaultCrypto.randomBytes(32)
        let fields: JSONObject = [
            "title": "Registrar — prod",
            "username": "ops@acme.test",
            "password": "päss \"quoted\" \\ wörd",
            "url": "https://registrar.example/login",
            "notes": "line one\nline two",
        ]
        let trust: JSONObject = ["pins": ["bob": .string(bobBundle.fingerprint())]]
        var grants = JSONObject()
        for permission in VaultCrypto.permissions {
            grants[permission] = .object(try VaultCrypto.sealGrant(
                keyset: alice, sender: "alice", recipient: bobBundle, itemId: itemId, itemKey: itemKey, permission: permission))
        }
        let recovery = try VaultCrypto.recoverySecret(recoveryCode)
        return [
            "format": "forkmesh-vault-interop-v1",
            "suite": .string(VaultCrypto.suite),
            "madeBy": "swift",
            "warning": "Test keys only. Nothing here protects anything.",
            "password": .string(password),
            "kdf": .object(kdf.toJSON()),
            "masterKey": .string(VaultCrypto.toHex(masterKey)),
            "accounts": [
                "alice": [
                    "keyset": .object(alice.toJSON()),
                    "bundle": .object(aliceBundle.toJSON()),
                    "fingerprint": .string(aliceBundle.fingerprint()),
                ],
                "bob": [
                    "keyset": .object(bob.toJSON()),
                    "bundle": .object(bobBundle.toJSON()),
                    "fingerprint": .string(bobBundle.fingerprint()),
                ],
            ],
            "wrappedKeyset": .string(try VaultCrypto.wrapKeyset(masterKey, purpose: "master", account: "alice", keyset: alice)),
            "recoveryCode": .string(recoveryCode),
            "recoverySecret": .string(VaultCrypto.toHex(recovery)),
            "wrappedRecovery": .string(try VaultCrypto.wrapKeyset(recovery, purpose: "recovery", account: "alice", keyset: alice)),
            "item": [
                "id": .string(itemId),
                "key": .string(VaultCrypto.toHex(itemKey)),
                "fields": .object(fields),
                "body": .string(try VaultCrypto.sealItem(itemKey, itemId: itemId, fields: fields)),
                "wrappedKey": .string(try VaultCrypto.wrapItemKey(alice, itemId: itemId, itemKey: itemKey)),
            ],
            "trust": ["value": .object(trust), "blob": .string(try VaultCrypto.sealPrivate(alice, label: "trust", value: trust))],
            "grants": .object(grants),
        ]
    }
}
