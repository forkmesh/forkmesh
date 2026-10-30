import FMBase
import Foundation
import XCTest

@testable import FMCrypto

final class ChatKeyringReinstallTests: XCTestCase {
    let room = "chat-direct:0123456789abcdef0123456789abcdef:v1"
    let scope = "direct:0123456789abcdef0123456789abcdef"
    let installedAt: Int64 = 1_790_000_000_000
    let reinstalledAt: Int64 = 1_790_000_000_000 + 3_600_000

    func phone(_ keychain: SecureStore, now: Int64) throws -> RoomSeal {
        let node = try Identity.loadOrCreate(secureStore: keychain)
        return try RoomSeal.loadOrCreate(
            room, scope: scope, policy: .strict, acct: "alice", node: node.nodeId, secureStore: keychain, now: now)
    }

    func peer(now: Int64) throws -> RoomSeal {
        try RoomSeal.loadOrCreate(
            room, scope: scope, policy: .strict, acct: "bob", node: "bob-node", secureStore: InMemorySecureStore(),
            now: now)
    }

    func testAReinstallThatRestoresTheNodeKeyKeepsTheSafetyNumber() throws {
        let oldKeychain = InMemorySecureStore()
        let alice = try phone(oldKeychain, now: installedAt)
        let bob = try peer(now: installedAt)
        XCTAssertEqual(bob.acceptBundle(alice.keyFrame(now: installedAt), now: installedAt)?.state, .approved)
        alice.acceptBundle(bob.keyFrame(now: installedAt), now: installedAt)
        var lastCounter: Int64 = 0
        for index in 1...3 {
            let frame = try alice.seal(["type": "chat", "id": .string("a\(index)"), "senderId": .string(alice.node ?? "")], now: installedAt)
            XCTAssertEqual(bob.open(frame, now: installedAt).name, "opened")
            lastCounter = ChatV3.int(frame["ctr"]) ?? 0
        }
        let before = bob.safetyNumber()
        bob.trust.markVerified()

        let newKeychain = InMemorySecureStore()
        newKeychain.write(Identity.seedKey, try XCTUnwrap(oldKeychain.read(Identity.seedKey)))
        let reinstalled = try phone(newKeychain, now: reinstalledAt)
        XCTAssertEqual(reinstalled.keyId, alice.keyId)
        XCTAssertEqual(
            bob.acceptBundle(reinstalled.keyFrame(now: reinstalledAt), now: reinstalledAt)?.state, .approved,
            "the peer sees the same device, not a new one waiting for approval")
        reinstalled.acceptBundle(bob.keyFrame(now: reinstalledAt), now: reinstalledAt)

        let after = bob.safetyNumber()
        XCTAssertEqual(after.digits, before.digits)
        XCTAssertFalse(after.changed)
        XCTAssertTrue(after.verified)
        XCTAssertEqual(reinstalled.safetyNumber().digits, before.digits)

        let frame = try reinstalled.seal(
            ["type": "chat", "id": "r1", "senderId": .string(reinstalled.node ?? ""), "text": "back"], now: reinstalledAt)
        XCTAssertGreaterThan(ChatV3.int(frame["ctr"]) ?? 0, lastCounter, "the new install's counters pass the old ones")
        XCTAssertEqual(bob.open(frame, now: reinstalledAt).plain?["text"], "back")
    }

    func testANewNodeKeyIsANewDevice() throws {
        let one = try phone(InMemorySecureStore(), now: installedAt)
        let two = try phone(InMemorySecureStore(), now: installedAt)
        XCTAssertNotEqual(one.keyId, two.keyId)
    }

    func testAStoredChatKeyIsKeptAsItIs() throws {
        let keychain = InMemorySecureStore()
        let earlier = try XCTUnwrap(try ChatKeyring.load(keychain, create: true))
        _ = try Identity.loadOrCreate(secureStore: keychain)
        let later = try XCTUnwrap(try ChatKeyring.load(keychain, create: false))
        XCTAssertEqual(later.kid, earlier.kid, "an install from before keeps its key, so its safety numbers stay")
        XCTAssertNil(keychain.read(ChatKeyring.counterFloorStoreKey))
    }

    func testWithoutANodeKeyTheChatKeyIsRandomAndCountsFromOne() throws {
        let keychain = InMemorySecureStore()
        let seal = try RoomSeal.loadOrCreate(
            room, scope: scope, policy: .strict, acct: "alice", node: "alice-node", secureStore: keychain, now: installedAt)
        XCTAssertNil(keychain.read(Identity.seedKey))
        let frame = try seal.seal(["type": "chat", "id": "a1", "senderId": "alice-node"], now: installedAt)
        XCTAssertEqual(frame["ctr"], 1)
    }

    func testTheDerivedKeyIsSeparateFromTheNodeKey() throws {
        let nodeSeed = Ed25519.generateSeed()
        let derived = ChatKeyring.derivedSeed(nodeSeed: nodeSeed)
        XCTAssertEqual(derived.count, Ed25519.seedBytes)
        XCTAssertNotEqual(derived, nodeSeed)
        XCTAssertEqual(derived, ChatKeyring.derivedSeed(nodeSeed: nodeSeed))
    }
}
