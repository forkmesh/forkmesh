// The OwnerCrypto contract (the Qt interop cases run only where desktop/build/forkmesh-tests
// exists) and the OwnerCrypto half of
// native/android/.../api/OwnerSealedAgentsTest.kt.

import FMBase
import Foundation
import XCTest
@testable import FMCrypto

final class OwnerCryptoTests: XCTestCase {
    func testAPromptThisDeviceSealsIsOpenedByTheRecipientItNames() throws {
        let phone = OwnerCrypto.loadOrCreate(secureStore: InMemorySecureStore())
        let desktop = OwnerCrypto.loadOrCreate(secureStore: InMemorySecureStore())

        let sealed = try XCTUnwrap(try phone.sealJson(
            ["kind": "forkmesh.agent-prompt", "text": "tighten the retry backoff"],
            to: [phone.publicBundle, desktop.publicBundle]))

        XCTAssertEqual(OwnerCrypto.recipientKeyIds(sealed), [phone.keyId, desktop.keyId])
        XCTAssertEqual(desktop.openJson(sealed)?["text"], "tighten the retry backoff")
        XCTAssertEqual(phone.openJson(sealed)?["kind"], "forkmesh.agent-prompt")
        XCTAssertFalse(JSON.encode(sealed).contains("tighten the retry backoff"))
    }

    func testEnvelopeFramingBoundsMatchTheRelayValidator() throws {
        let owner = OwnerCrypto.loadOrCreate(secureStore: InMemorySecureStore())
        let bundle = owner.publicBundle

        XCTAssertNil(try owner.seal([1, 2, 3], to: []))
        XCTAssertNil(try owner.seal([1, 2, 3], to: Array(repeating: bundle, count: OwnerCrypto.maxRecipients + 1)))
        // Duplicate recipient ids are refused by the relay, so never emit them.
        XCTAssertNil(try owner.seal([1, 2, 3], to: [bundle, bundle]))

        let sealed = try XCTUnwrap(try owner.seal([1, 2, 3], to: [bundle]))
        XCTAssertEqual(sealed["kind"], "forkmesh.owner-sealed")
        XCTAssertEqual(sealed["alg"], "x25519+mlkem768/aes256gcm")
        XCTAssertEqual(sealed["v"], 1)
        XCTAssertEqual(OwnerCrypto.recipientKeyIds(sealed), [owner.keyId])
        for field in ["nonce", "tag", "body"] {
            XCTAssertFalse(sealed[field]?.stringValue?.contains("=") ?? true, field)
        }
        XCTAssertEqual(owner.open(sealed), [1, 2, 3])

        // Malformed framing yields no recipient ids at all.
        var altered = sealed
        altered["alg"] = "aes"
        XCTAssertEqual(OwnerCrypto.recipientKeyIds(altered), [])
        altered = sealed
        altered["recipients"] = .array([])
        XCTAssertEqual(OwnerCrypto.recipientKeyIds(altered), [])
        let first = sealed["recipients"]?.arrayValue?.first ?? .null
        altered["recipients"] = .array([first, first])
        XCTAssertEqual(OwnerCrypto.recipientKeyIds(altered), [])
        altered = sealed
        altered["v"] = .double(1)
        XCTAssertEqual(OwnerCrypto.recipientKeyIds(altered), [owner.keyId], "Dart num equality accepts 1.0")

        // A bundle with the wrong version or a short key is refused.
        var badBundle = bundle
        badBundle["v"] = 2
        XCTAssertNil(try owner.seal([1], to: [badBundle]))
        badBundle = bundle
        badBundle["x25519"] = "AAAA"
        XCTAssertNil(try owner.seal([1], to: [badBundle]))
    }

    func testTheStoredIdentitySurvivesAReloadAndIsDeviceSpecific() throws {
        let store = InMemorySecureStore()
        let first = OwnerCrypto.loadOrCreate(secureStore: store)
        let reloaded = OwnerCrypto.loadOrCreate(secureStore: store)
        XCTAssertEqual(reloaded.keyId, first.keyId)

        let sealed = try XCTUnwrap(try first.sealJson(["a": 1], to: [first.publicBundle]))
        XCTAssertEqual(reloaded.openJson(sealed)?["a"], 1)

        // Forgetting the identity makes previously sealed data unreadable.
        OwnerCrypto.forget(secureStore: store)
        let fresh = OwnerCrypto.loadOrCreate(secureStore: store)
        XCTAssertNotEqual(fresh.keyId, first.keyId)
        XCTAssertNil(fresh.open(sealed))
    }

    func testStorageMatchesFlutter() throws {
        let store = InMemorySecureStore()
        let owner = OwnerCrypto.loadOrCreate(secureStore: store)
        let stored = try XCTUnwrap(store.read(OwnerCrypto.seedKey))
        let json = JSON.parseObject(stored)
        XCTAssertEqual(json.keys, ["v", "x25519Pub", "x25519Priv", "mlkemPub", "mlkemPriv"])
        XCTAssertEqual(json["v"], 1)
        let xPriv = Array(try B64.decode(json.str("x25519Priv")))
        XCTAssertTrue(xPriv[0] & 7 == 0 && xPriv[31] & 0xC0 == 0x40, "stored clamped, like Dart")
        XCTAssertEqual(Array(try B64.decode(json.str("x25519Pub"))), owner.x25519Public)
        XCTAssertEqual(try B64.decode(json.str("mlkemPub")).count, 1184)
        XCTAssertEqual(try B64.decode(json.str("mlkemPriv")).count, 2400)
        XCTAssertEqual(owner.keyId, B64.encodeUrlNoPadding(Sha256.hash(owner.x25519Public, owner.mlkemPublic)))
        XCTAssertEqual(owner.publicBundle.keys, ["v", "x25519", "mlkem768"])

        // A corrupt blob is replaced by a fresh identity rather than failing.
        let corrupt = InMemorySecureStore([OwnerCrypto.seedKey: "{\"v\":1,\"x25519Pub\":\"AA==\"}"])
        let replaced = OwnerCrypto.loadOrCreate(secureStore: corrupt)
        XCTAssertNotEqual(corrupt.read(OwnerCrypto.seedKey), "{\"v\":1,\"x25519Pub\":\"AA==\"}")
        XCTAssertEqual(OwnerCrypto.loadOrCreate(secureStore: corrupt).keyId, replaced.keyId)
    }

    func testTamperedEnvelopesFailAuthenticationInsteadOfReturningData() throws {
        let owner = OwnerCrypto.loadOrCreate(secureStore: InMemorySecureStore())
        let stranger = OwnerCrypto.loadOrCreate(secureStore: InMemorySecureStore())
        let sealed = try XCTUnwrap(try OwnerCrypto.seal(Array("secret".utf8), to: [owner.publicBundle]))
        XCTAssertTrue(owner.isRecipientOf(sealed))
        XCTAssertFalse(stranger.isRecipientOf(sealed))
        XCTAssertNil(stranger.open(sealed))

        var body = Array(try B64.decodeUnpadded(sealed.str("body")))
        body[0] ^= 1
        var tampered = sealed
        tampered["body"] = .string(B64.encodeUrlNoPadding(body))
        XCTAssertNil(owner.open(tampered))

        tampered = sealed
        tampered["nonce"] = .string(sealed.str("nonce") + "=")
        XCTAssertNil(owner.open(tampered), "padded base64 is not the wire form")

        var recipient = sealed["recipients"]?.arrayValue?.first?.objectValue ?? [:]
        var key = Array(try B64.decodeUnpadded(recipient.str("key")))
        key[0] ^= 1
        recipient["key"] = .string(B64.encodeUrlNoPadding(key))
        tampered = sealed
        tampered["recipients"] = .array([.object(recipient)])
        XCTAssertNil(owner.open(tampered))
    }

    // MARK: Qt interop (skipped without the desktop test binary)

    private func qtSeal(to owner: OwnerCrypto) throws -> JSONObject {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { root.deleteLastPathComponent() }
        let binary = root.appendingPathComponent("desktop/build/forkmesh-tests")
        guard FileManager.default.isExecutableFile(atPath: binary.path) else {
            throw XCTSkip("desktop/build/forkmesh-tests is not built; run ./desktop/run.sh test")
        }
        let process = Process()
        process.executableURL = binary
        process.arguments = ["--seal-owner-envelope-to", owner.publicBundle.str("x25519"), owner.publicBundle.str("mlkem768")]
        let out = Pipe()
        process.standardOutput = out
        try process.run()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        XCTAssertEqual(process.terminationStatus, 0)
        return JSON.parseObject(data)
    }

    func testThisDeviceOpensASessionSnapshotTheQtNodeSealedToIt() throws {
        let owner = OwnerCrypto.loadOrCreate(secureStore: InMemorySecureStore())
        let payload = try qtSeal(to: owner)
        let envelope = payload.obj("envelope")
        XCTAssertEqual(OwnerCrypto.recipientKeyIds(envelope), [payload.str("desktopKeyId"), payload.str("companionKeyId")])
        XCTAssertEqual(payload.str("companionKeyId"), owner.keyId)
        XCTAssertTrue(owner.isRecipientOf(envelope))
        XCTAssertEqual(owner.open(envelope), Array(try B64.decode(payload.str("plaintext"))))
        let json = owner.openJson(envelope)
        XCTAssertEqual(json?["kind"], "forkmesh.agent-session")
        XCTAssertTrue(json?.str("transcript").contains("only owner devices read") ?? false)
        XCTAssertNil(OwnerCrypto.loadOrCreate(secureStore: InMemorySecureStore()).open(envelope))
    }
}
