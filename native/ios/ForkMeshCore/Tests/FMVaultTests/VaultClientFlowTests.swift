// The vault client against the real Worker module, over the
// stdio bridge. Skipped when python3 is missing.
import FMBase
import Foundation
import XCTest
@testable import FMVault

final class VaultClientFlowTests: XCTestCase {
    @MainActor
    func testTheWholeVaultFlowAgainstTheRealWorkerModule() async throws {
        let bridge = try VaultBridge.start()
        defer { bridge.stop() }
        let alice = bridge.client(for: "alice"), bob = bridge.client(for: "bob")
        let carol = bridge.client(for: "carol")

        try await alice.status()
        XCTAssertFalse(alice.installed)
        let recoveryCode = try await alice.setUp("alice master password")
        XCTAssertNotNil(recoveryCode.range(of: #"^([0-9A-Z]{4}-){12}[0-9A-Z]{4}$"#, options: .regularExpression))
        _ = try await bob.setUp("bob master password")
        await refused("vault_exists") { _ = try await bridge.client(for: "alice").setUp(String(repeating: "x", count: 12)) }

        let loginId = try await alice.save([
            "title": "Registrar",
            "username": "ops@acme.test",
            "password": "s3cret!",
            "url": "https://registrar.example/login",
        ])
        let bankId = try await alice.save(["title": "Bank", "password": "only-mine", "url": "bank.example"])
        await refused("title_required") { try await alice.save(["title": "  "]) }
        _ = try await alice.load()
        XCTAssertEqual(alice.find(loginId)?.password, "s3cret!")
        XCTAssertEqual(alice.matching("registrar.example").map(\.id), [loginId])
        XCTAssertEqual(alice.matching("www.bank.example").map(\.id), [bankId])
        XCTAssertTrue(alice.matching("evil.example").isEmpty)
        XCTAssertTrue(alice.matching("sub.registrar.example").isEmpty)

        // Another device: wrong password, right password, and resuming from a keyset the app kept
        // behind the device's own lock.
        let elsewhere = bridge.client(for: "alice")
        await refused("wrong_password") { try await elsewhere.unlock("not the password") }
        try await elsewhere.unlock("alice master password")
        let elsewhereItems = try await elsewhere.load()
        XCTAssertEqual(elsewhereItems.count, 2)
        let resumed = bridge.client(for: "alice")
        try await resumed.resume(try elsewhere.exportKeyset())
        let resumedItems = try await resumed.load()
        XCTAssertEqual(resumedItems.count, 2)
        await refused("keyset_mismatch") { try await bridge.client(for: "bob").resume(try elsewhere.exportKeyset()) }

        let dump = JSON.encode(try bridge.control("dump")["rows"] ?? .null)
        for secret in ["s3cret!", "only-mine", "Registrar", "registrar.example", "master password"] {
            XCTAssertFalse(dump.contains(secret), "the database never sees \"\(secret)\"")
        }

        // Share with one user: first sight, then pinned.
        let preview = try await alice.previewShare(kind: "user", name: "bob")
        XCTAssertEqual(preview.map(\.state), ["new"])
        let bobFingerprint = try await bob.myFingerprint()
        XCTAssertEqual(preview.first?.fingerprint, bobFingerprint)
        await refused("recipient_has_no_vault") { _ = try await alice.share(loginId, kind: "user", name: "carol") }
        await refused("recipient_not_found") { _ = try await alice.share(loginId, kind: "user", name: "nobody") }
        let outcome = try await alice.share(loginId, kind: "user", name: "bob")
        XCTAssertEqual(outcome.sealedTo, ["bob"])
        _ = try await bob.load()
        XCTAssertEqual(bob.items.count, 1)
        XCTAssertEqual(bob.items.first?.password, "s3cret!")
        XCTAssertEqual(bob.items.first?.canEdit, false)
        await refused("read_only") {
            var fields = bob.find(loginId)!.fields
            fields["password"] = "x"
            try await bob.save(fields, id: loginId)
        }
        let pinned = try await alice.previewShare(kind: "user", name: "bob")
        XCTAssertEqual(pinned.map(\.state), ["pinned"])

        _ = try await alice.load()
        _ = try await alice.share(loginId, kind: "user", name: "bob", permission: "write")
        _ = try await bob.load()
        var edited = bob.find(loginId)!.fields
        edited["password"] = "rotated-by-bob"
        try await bob.save(edited, id: loginId)
        _ = try await alice.load()
        XCTAssertEqual(alice.find(loginId)?.password, "rotated-by-bob")
        XCTAssertEqual(alice.find(loginId)?.updatedBy, "bob")
        await refused("owner_required") { _ = try await bob.share(loginId, kind: "user", name: "alice") }

        // Organization share, a late joiner, leaving.
        let org = try await alice.share(bankId, kind: "organization", name: "acme")
        XCTAssertEqual(org.sealedTo, ["bob"])
        XCTAssertEqual(org.withoutVault, ["carol"])
        await refused("organization_not_found") { _ = try await alice.share(bankId, kind: "organization", name: "globex") }
        _ = try await carol.setUp("carol master password")
        let carolBefore = try await carol.load()
        XCTAssertTrue(carolBefore.isEmpty)
        _ = try await alice.load()
        let sealedFirst = try await alice.syncPending()
        XCTAssertEqual(sealedFirst, 1)
        let sealedAgain = try await alice.syncPending()
        XCTAssertEqual(sealedAgain, 0)
        let carolAfter = try await carol.load()
        XCTAssertEqual(carolAfter.first?.viaName, "acme")
        _ = try bridge.control("leave", ["organization": "acme", "member": "bob"])
        _ = try await bob.load()
        XCTAssertNil(bob.find(bankId))
        XCTAssertNotNil(bob.find(loginId))

        // Unsharing rotates and keeps the remaining readers.
        try await alice.unshare(loginId, kind: "user", name: "bob")
        _ = try await alice.load()
        XCTAssertEqual(alice.find(loginId)?.password, "rotated-by-bob")
        XCTAssertEqual(alice.find(loginId)?.shares, [])
        let bobAfter = try await bob.load()
        XCTAssertTrue(bobAfter.isEmpty)
        _ = try await alice.share(bankId, kind: "user", name: "bob")
        try await alice.unshare(bankId, kind: "user", name: "bob")
        let carolLast = try await carol.load()
        XCTAssertEqual(carolLast.first?.password, "only-mine")

        // A server that publishes other keys under bob's name is caught.
        let honest = try bridge.send(["actor": "alice", "method": "GET", "path": "/api/vault/users/bob", "body": .null])
            .obj("data")["bundle"] ?? .null
        let impostor = JSONValue.object(try VaultBundle.of(VaultKeyset.generate(), account: "bob").toJSON())
        _ = try bridge.control("set_bundle", ["member": "bob", "bundle": impostor])
        let changed = try await alice.previewShare(kind: "user", name: "bob")
        XCTAssertEqual(changed.map(\.state), ["changed"])
        await refused("recipient_keys_changed") { _ = try await alice.share(loginId, kind: "user", name: "bob") }
        _ = try bridge.control("set_bundle", ["member": "bob", "bundle": honest])
        _ = try await bob.load()
        let fromBob = try await bob.save(["title": "From bob", "password": "pw"])
        _ = try await bob.load()
        _ = try await bob.share(fromBob, kind: "user", name: "alice")
        let aliceWithShare = try await alice.load()
        XCTAssertEqual(aliceWithShare.first { $0.id == fromBob }?.title, "From bob")
        _ = try bridge.control("set_bundle", ["member": "bob", "bundle": impostor])
        let suspicious = try await alice.load().first { $0.id == fromBob }
        XCTAssertEqual(suspicious?.readable, false)
        XCTAssertEqual(suspicious?.error, "sender_keys_changed")
        _ = try bridge.control("set_bundle", ["member": "bob", "bundle": honest])

        // Master password change and recovery.
        try await alice.changePassword("alice second password")
        await refused("wrong_password") { try await bridge.client(for: "alice").unlock("alice master password") }
        let recovered = bridge.client(for: "alice")
        await refused("wrong_recovery_code") { _ = try await recovered.recover(VaultCrypto.newRecoveryCode(), newMasterPassword: String(repeating: "x", count: 12)) }
        await refused("invalid_recovery_code") { _ = try await recovered.recover("not a code", newMasterPassword: String(repeating: "x", count: 12)) }
        let renewed = try await recovered.recover(recoveryCode, newMasterPassword: "alice third password")
        XCTAssertNotEqual(renewed, recoveryCode)
        let recoveredItems = try await recovered.load()
        XCTAssertEqual(recoveredItems.count, 3)
        await refused("wrong_recovery_code") { _ = try await bridge.client(for: "alice").recover(recoveryCode, newMasterPassword: String(repeating: "x", count: 12)) }

        recovered.lock()
        XCTAssertFalse(recovered.unlocked)
        XCTAssertTrue(recovered.items.isEmpty)
        await refused("vault_locked") { try await recovered.save(["title": "x"]) }
        await refused("vault_locked") { _ = try recovered.exportKeyset() }
    }

    func testALoginOriginIsSchemeHostAndPort() {
        XCTAssertEqual(VaultClient.loginOrigin("192.168.1.1"), "http://192.168.1.1:80")
        XCTAssertEqual(VaultClient.loginOrigin("HTTPS://Router.LAN/cgi-bin/luci"), "https://router.lan:443")
        XCTAssertEqual(VaultClient.loginOrigin("http://[fd00::1]:8080"), "http://fd00::1:8080")
        XCTAssertEqual(VaultClient.loginOrigin("ftp://router.lan"), "")
        XCTAssertEqual(VaultClient.loginOrigin("router notes"), "")
        XCTAssertEqual(VaultClient.loginOrigin("  "), "")
    }

    @MainActor
    func testARouterLoginIsRememberedAndFoundByItsAddress() async throws {
        let bridge = try VaultBridge.start()
        defer { bridge.stop() }
        let alice = bridge.client(for: "alice")
        try await alice.status()
        _ = try await alice.setUp("alice master password")
        let router: JSONObject = ["title": "OpenWrt router (192.168.1.1)", "url": "http://192.168.1.1", "username": "root", "password": "router-pass"]
        let first = try await alice.rememberLogin(router)
        XCTAssertTrue(first.changed)
        XCTAssertEqual(alice.logins(for: "http://192.168.1.1:80/").map(\.id), [first.id])
        XCTAssertEqual(alice.logins(for: "http://192.168.1.1").first?.password, "router-pass")
        XCTAssertTrue(alice.logins(for: "https://192.168.1.1").isEmpty)
        XCTAssertTrue(alice.logins(for: "http://192.168.1.2").isEmpty)

        let again = try await alice.rememberLogin(router)
        XCTAssertEqual(again.id, first.id)
        XCTAssertFalse(again.changed)

        var rotated = router
        rotated["password"] = "router-pass-2"
        rotated["title"] = "ignored"
        let updated = try await alice.rememberLogin(rotated)
        XCTAssertEqual(updated.id, first.id)
        XCTAssertTrue(updated.changed)
        XCTAssertEqual(alice.items.count, 1)
        XCTAssertEqual(alice.find(first.id)?.password, "router-pass-2")
        XCTAssertEqual(alice.find(first.id)?.title, "OpenWrt router (192.168.1.1)")

        var admin = router
        admin["username"] = "admin"
        let other = try await alice.rememberLogin(admin)
        XCTAssertNotEqual(other.id, first.id)
        XCTAssertEqual(alice.logins(for: "http://192.168.1.1").count, 2)

        var nowhere = router
        nowhere["url"] = "router notes"
        await refused("invalid_url") { _ = try await alice.rememberLogin(nowhere) }
        let dump = JSON.encode(try bridge.control("dump")["rows"] ?? .null)
        XCTAssertFalse(dump.contains("router-pass"))

        alice.lock()
        XCTAssertTrue(alice.logins(for: "http://192.168.1.1").isEmpty)
        await refused("vault_locked") { _ = try await alice.rememberLogin(router) }
    }

    func testFieldsAreCleanedAndCut() throws {
        let clean = try VaultFields.clean([
            "title": "  Bank  ", "url": " bank.example ", "password": .string(String(repeating: "p", count: 5000)), "extra": "dropped",
        ])
        XCTAssertEqual(Array(clean.keys), ["title", "username", "password", "url", "notes"])
        XCTAssertEqual(clean.str("title"), "Bank")
        XCTAssertEqual(clean.str("url"), "bank.example")
        XCTAssertEqual(clean.str("username"), "")
        XCTAssertEqual(clean.str("password").count, 4096)
        XCTAssertThrowsError(try VaultFields.clean(["title": " "])) { XCTAssertEqual($0 as? VaultError, VaultError("title_required")) }
        XCTAssertEqual(encodeComponent("Ab-_.!~*'() é/"), "Ab-_.!~*'()%20%C3%A9%2F")
        XCTAssertEqual(dartString(nil), "null")
        XCTAssertEqual(dartString(.int(3)), "3")
    }
}
