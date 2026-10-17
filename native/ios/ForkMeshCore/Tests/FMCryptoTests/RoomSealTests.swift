import FMBase
import Foundation
import XCTest

@testable import FMCrypto

func chatV3Vectors() throws -> JSONObject {
    var root = URL(fileURLWithPath: #filePath)
    for _ in 0..<6 { root.deleteLastPathComponent() }
    let data = try Data(contentsOf: root.appendingPathComponent("conformance/chat/v3/vectors.json"))
    return try XCTUnwrap(try JSON.parse(data).objectValue)
}

final class RoomSealVectorTests: XCTestCase {
    var vectors: JSONObject = [:]
    var room: String { vectors.str("room") }
    var scope: String { vectors.str("scope") }
    var now: Int64 { vectors.int64("now") }

    override func setUpWithError() throws {
        vectors = try chatV3Vectors()
    }

    func b64(_ text: String) -> [UInt8] { ChatV3.unb64(text) ?? [] }

    func b64u(_ text: String) -> [UInt8] { ChatV3.unb64u(text) ?? [] }

    func device(_ name: String, store: SecureStore? = InMemorySecureStore()) throws -> ChatKeyring {
        let secrets = vectors.obj("devices").obj(name)
        var prekeys: [ChatPrekeySecret] = []
        for spk in secrets.maps("prekeys") {
            let made = try ChatPrekeySecret(xPriv: b64(spk.str("xPriv")), pqSeed: b64(spk.str("pqSeed")), ts: spk.int64("ts"))
            XCTAssertEqual(made.prekey.id, spk.str("id"))
            XCTAssertEqual(ChatV3.b64u(made.prekey.x), spk.str("x"))
            XCTAssertEqual(ChatV3.b64u(made.prekey.pq), spk.str("pq"), "ML-KEM key generation from d || z matches OpenSSL")
            XCTAssertEqual(made.prekey.exp, spk.int64("exp"))
            prekeys.append(made)
        }
        let keyring = try ChatKeyring.fromSecrets(
            identitySeed: b64(secrets.str("identitySeed")), prekeys: prekeys, store: store)
        XCTAssertEqual(keyring.idk, secrets.str("idk"))
        XCTAssertEqual(keyring.kid, secrets.str("kid"))
        return keyring
    }

    func bundle(_ name: String) -> JSONObject { vectors.obj("bundles").obj(name) }

    func frame(_ name: String) -> JSONObject { vectors.obj("sealed").obj(name).obj("frame") }

    func kid(_ name: String) -> String { vectors.obj("devices").obj(name).str("kid") }

    var participants: Set<String> { Set(vectors.strings("participants")) }

    func node(_ name: String) -> String { vectors.obj("nodes").str(name) }

    func bobWithTrust(roomName: String? = nil) throws -> RoomSeal {
        let bob = try device("bob")
        let seal = try RoomSeal(
            room: roomName ?? room, scope: scope, policy: .strict, acct: "bob", node: node("bob"),
            participants: participants, keyring: bob, version: ChatV3.version)
        let setup = try RoomSeal(
            room: room, scope: scope, policy: .strict, acct: "bob", node: node("bob"), participants: participants,
            keyring: bob, version: ChatV3.version)
        XCTAssertEqual(setup.acceptBundle(bundle("alice"), now: now)?.state, .approved)
        XCTAssertEqual(setup.acceptBundle(bundle("bob"), now: now)?.state, .approved)
        XCTAssertEqual(setup.acceptBundle(bundle("alice2"), now: now)?.state, .pending)
        return seal
    }

    func testBundlesVerify() {
        for name in ["alice", "alice2", "bob", "bobOld", "injectedOutsider", "injectedAsBob"] {
            XCTAssertTrue(RoomSeal.verifyBundle(bundle(name), room: room, now: now), name)
        }
        for (name, value) in vectors.obj("bundles").obj("invalid") {
            XCTAssertFalse(RoomSeal.verifyBundle(value.objectValue ?? [:], room: room, now: now), name)
        }
        XCTAssertFalse(
            RoomSeal.verifyBundle(bundle("alice"), room: room, now: vectors.obj("bundleChecks").int64("expiredAt")))
        XCTAssertEqual(vectors.obj("bundleChecks").str("pendingStateForAlice2AtBob"), ChatTrustState.pending.rawValue)
    }

    func testABundleForAKnownKidCannotChangeTheAccount() throws {
        let bob = try bobWithTrust()
        var changed = bundle("alice")
        changed["acct"] = "mallory"
        XCTAssertNil(bob.acceptBundle(changed, now: now))
        XCTAssertEqual(bob.trust.record(kid("alice"))?.acct, "alice")
        var moved = bundle("alice")
        moved["node"] = "node-mallory"
        XCTAssertNil(bob.acceptBundle(moved, now: now))
        XCTAssertEqual(bob.trust.record(kid("alice"))?.node, node("alice"))
    }

    func testServerInjectedDevicesAreRefusedOrHeld() throws {
        let bob = try bobWithTrust()
        XCTAssertNil(bob.acceptBundle(bundle("injectedOutsider"), now: now), "a DM bundle must name a participant")
        XCTAssertEqual(bob.acceptBundle(bundle("injectedAsBob"), now: now)?.state, .pending)
        let open = try RoomSeal(room: room, scope: "open-scope", policy: .open, acct: "bob", node: node("bob"), keyring: try device("bob"))
        XCTAssertEqual(open.acceptBundle(bundle("injectedOutsider"), now: now)?.state, .approved)
    }

    func testTheRelaySequenceOpensAsTheReferenceDoes() throws {
        let bob = try bobWithTrust()
        let sealed = vectors.obj("sealed")
        let first = bob.open(frame("first"), now: now)
        guard case .opened(let plain, let senderKid, let senderAcct, let senderNode) = first else {
            return XCTFail("first gave \(first.name)")
        }
        XCTAssertEqual(plain, sealed.obj("first").obj("plain"))
        XCTAssertEqual(senderKid, sealed.obj("first").str("senderKid"))
        XCTAssertEqual(senderAcct, "alice")
        XCTAssertEqual(senderNode, node("alice"))
        XCTAssertEqual(plain["senderId"]?.stringValue, node("alice"))
        XCTAssertEqual(bob.open(frame("replay"), now: now), .replay)
        let second = bob.open(frame("second"), now: now + 1000)
        XCTAssertEqual(second.plain, sealed.obj("second").obj("plain"))
        XCTAssertEqual(bob.open(frame("tampered"), now: now), .invalid)
        XCTAssertEqual(bob.open(frame("forgedSenderId"), now: now + 2000), .invalid)
        let pending = sealed.obj("fromPendingDevice")
        XCTAssertEqual(RoomSeal.wrappedKeyIds(pending.obj("frame")), pending.strings("keys"))
        XCTAssertEqual(bob.open(pending.obj("frame"), now: now + 3000), .held(kid("alice2")))
        XCTAssertEqual(bob.trust.heldCount(room), 1)
        XCTAssertTrue(bob.approve(kid("alice2")))
        XCTAssertEqual(bob.open(pending.obj("frame"), now: now + 3000).name, "opened")
        XCTAssertEqual(
            [first, RoomSealOpen.held(""), .notForMe, .replay, .invalid].map(\.name),
            ["opened", "held", "notForMe", "replay", "invalid"])
    }

    func testHeldFramesOpenAgainOnApproval() throws {
        let bob = try bobWithTrust()
        let pending = frame("fromPendingDevice")
        XCTAssertEqual(bob.open(pending, now: now + 3000), .held(kid("alice2")))
        XCTAssertTrue(bob.reopenHeld(now: now + 3000).isEmpty, "still pending, still held")
        XCTAssertEqual(bob.trust.heldCount(room), 1)
        bob.approve(kid("alice2"))
        let reopened = bob.reopenHeld(now: now + 3000)
        XCTAssertEqual(reopened.count, 1)
        XCTAssertEqual(reopened.first?.plain?["text"], "from the second phone")
        XCTAssertEqual(bob.trust.heldCount(room), 0)
    }

    func testAForgedFrameDoesNotBurnACounter() throws {
        let bob = try bobWithTrust()
        XCTAssertEqual(bob.open(frame("tampered"), now: now), .invalid)
        XCTAssertEqual(bob.open(frame("first"), now: now).name, "opened")
    }

    func testABlockedSenderIsHeldNotOpenedAndNotKept() throws {
        let bob = try bobWithTrust()
        bob.block(kid("alice"))
        XCTAssertEqual(bob.open(frame("first"), now: now), .held(kid("alice")))
        XCTAssertEqual(bob.trust.heldCount(room), 0)
    }

    func testAWipedPrekeyCannotOpen() throws {
        let bob = try bobWithTrust()
        bob.keyring.wipeExpired(now: vectors.obj("afterBobWipesOldPrekey").int64("now"))
        XCTAssertEqual(bob.keyring.prekeyIds.count, 1)
        XCTAssertEqual(bob.open(frame("first"), now: now), .notForMe)
    }

    func testAnotherRoomCannotOpen() throws {
        let bob = try bobWithTrust(roomName: "another-room")
        XCTAssertEqual(bob.open(frame("first"), now: now), .invalid)
    }

    func testTheFrameTimeMustBeRecent() throws {
        let bob = try bobWithTrust()
        XCTAssertEqual(bob.open(frame("first"), now: now + ChatV3.prekeyLifeMs + 1), .invalid)
        XCTAssertEqual(bob.open(frame("first"), now: now - ChatV3.dayMs - 1), .invalid)
        XCTAssertEqual(bob.open(frame("first"), now: now).name, "opened")
    }

    func testKekAndMlKem() throws {
        let k = vectors.obj("kek")
        let ct = b64(k.str("ct"))
        let bob = try device("bob")
        let secret = try XCTUnwrap(bob.prekeySecret(k.str("spkId")))
        XCTAssertEqual(try MlKem768.decapsulate(secretKey: secret.kem, ciphertext: ct), b64(k.str("ss2")))
        XCTAssertEqual(
            try X25519.sharedSecret(privateKey: b64(k.str("ephPriv")), remotePublicKey: try X25519.publicKey(privateKey: secret.xPriv)),
            b64(k.str("ss1")))
        XCTAssertEqual(try X25519.sharedSecret(privateKey: secret.xPriv, remotePublicKey: b64u(k.str("ephPub"))), b64(k.str("ss1")))
        XCTAssertEqual(ChatV3.b64u(try X25519.publicKey(privateKey: b64(k.str("ephPriv")))), k.str("ephPub"))
        let seal = try RoomSeal(room: k.str("room"), policy: .strict, acct: "bob", node: node("bob"), keyring: bob)
        let derived = seal.kek(
            sender: k.str("sender"), recipient: k.str("recipient"), spkId: k.str("spkId"), ephPub: b64u(k.str("ephPub")),
            ciphertext: ct, ss1: b64(k.str("ss1")), ss2: b64(k.str("ss2")))
        XCTAssertEqual(ChatV3.b64(derived), k.str("kek"))
    }

    func testSafetyNumbers() {
        for name in ["twoDevices", "threeDevices"] {
            let s = vectors.obj("safety").obj(name)
            let keys = s.strings("idks").map(b64u)
            let computed = ChatSafetyNumber.compute(scope: s.str("scope"), identityKeys: keys)
            XCTAssertEqual(computed.digits, s.str("digits"), name)
            XCTAssertEqual(computed.digits.count, 60)
            XCTAssertEqual(computed.qr, s.str("qr"), name)
            XCTAssertEqual(ChatSafetyNumber.compute(scope: s.str("scope"), identityKeys: keys.reversed()).digits, computed.digits)
        }
        XCTAssertNotEqual(
            vectors.obj("safety").obj("twoDevices").str("digits"), vectors.obj("safety").obj("threeDevices").str("digits"))
    }

    func testTheTrustStoreSafetyNumberMatchesTheVectors() throws {
        let bob = try bobWithTrust()
        XCTAssertEqual(bob.safetyNumber().digits, vectors.obj("safety").obj("twoDevices").str("digits"))
        XCTAssertFalse(bob.safetyNumber().changed, "the first two accounts are not a change")
        XCTAssertEqual(bob.safetyNumber().groups.count, 12)
        bob.trust.markVerified()
        XCTAssertTrue(bob.safetyNumber().verified)
        bob.approve(kid("alice2"))
        let after = bob.safetyNumber()
        XCTAssertEqual(after.digits, vectors.obj("safety").obj("threeDevices").str("digits"))
        XCTAssertTrue(after.changed)
        XCTAssertFalse(after.verified, "a new approved device clears verified")
        XCTAssertFalse(bob.trust.verify(scanned: "forkmesh-safety:3:wrong"))
        XCTAssertTrue(bob.trust.verify(scanned: after.qr))
        XCTAssertTrue(bob.safetyNumber().verified)
        XCTAssertFalse(bob.safetyNumber().changed)
    }

    func testTheLinkHandshakeMatchesTheVectors() throws {
        let link = vectors.obj("link")
        let alice = try ChatKeyring.fromSecrets(identitySeed: b64(link.str("initiatorIdentitySeed")))
        let bob = try ChatKeyring.fromSecrets(identitySeed: b64(link.str("responderIdentitySeed")))
        let iHello = link.obj("initiatorHello")
        let rHello = link.obj("responderHello")
        let transcript = MeshLinkCrypto.transcript(initiator: iHello, responder: rHello)
        XCTAssertEqual(ChatV3.b64u(transcript), link.str("transcript"))
        XCTAssertTrue(
            Ed25519.verify(
                publicKey: b64u(iHello.str("idk")), message: MeshLinkCrypto.authMessage(role: "i", transcript: transcript),
                signature: b64u(link.str("initiatorAuthSig"))))
        XCTAssertTrue(
            Ed25519.verify(
                publicKey: b64u(rHello.str("idk")), message: MeshLinkCrypto.authMessage(role: "r", transcript: transcript),
                signature: b64u(link.str("responderAuthSig"))))
        XCTAssertFalse(
            Ed25519.verify(
                publicKey: b64u(iHello.str("idk")), message: MeshLinkCrypto.authMessage(role: "r", transcript: transcript),
                signature: b64u(link.str("initiatorAuthSig"))))
        let iEph = b64(link.str("initiatorEphPriv"))
        let rEph = b64(link.str("responderEphPriv"))
        let keys = try MeshLinkCrypto.keys(ownEph: iEph, peerEph: try X25519.publicKey(privateKey: rEph), transcript: transcript)
        let mirrored = try MeshLinkCrypto.keys(ownEph: rEph, peerEph: try X25519.publicKey(privateKey: iEph), transcript: transcript)
        XCTAssertEqual(keys.i2r, mirrored.i2r)
        XCTAssertEqual(keys.r2i, mirrored.r2i)
        XCTAssertEqual(ChatV3.b64(keys.i2r), link.str("keyI2R"))
        XCTAssertEqual(ChatV3.b64(keys.r2i), link.str("keyR2I"))
        for name in ["frame0", "frame1"] {
            let entry = link.obj(name)
            let data = b64(entry.str("data"))
            XCTAssertEqual(
                MeshLinkCrypto.open(key: keys.i2r, counter: UInt64(entry.int64("counter")), data: data), entry.obj("plain"))
        }
        XCTAssertNil(MeshLinkCrypto.open(key: keys.i2r, counter: 1, data: b64(link.obj("frame0").str("data"))))

        let initiator = try MeshLinkHandshake(
            keyring: alice, initiator: true, ephemeral: iEph, nonce: b64u(iHello.str("nonce")))
        let responder = try MeshLinkHandshake(
            keyring: bob, initiator: false, ephemeral: rEph, nonce: b64u(rHello.str("nonce")))
        XCTAssertEqual(initiator.hello, iHello)
        XCTAssertEqual(responder.hello, rHello)
        let iAuth = try initiator.receiveHello(responder.hello)
        let rAuth = try responder.receiveHello(initiator.hello)
        XCTAssertEqual(iAuth["sig"]?.stringValue, link.str("initiatorAuthSig"))
        XCTAssertEqual(rAuth["sig"]?.stringValue, link.str("responderAuthSig"))
        XCTAssertThrowsError(try initiator.receiveAuth(iAuth), "a side cannot pass its own signature off as the peer's")
        XCTAssertFalse(initiator.authenticated)
        XCTAssertEqual(try initiator.receiveAuth(rAuth), bob.kid)
        XCTAssertEqual(try responder.receiveAuth(iAuth), alice.kid)
        let hello: JSONObject = ["type": "hello", "senderId": .string(node("alice"))]
        let sealed = try initiator.seal(hello)
        XCTAssertEqual(ChatV3.b64(sealed), link.obj("frame0").str("data"))
        XCTAssertEqual(try responder.open(sealed), hello)
        XCTAssertEqual(ChatV3.b64(try initiator.seal(hello)), link.obj("frame1").str("data"))
        XCTAssertThrowsError(try responder.open(sealed), "a replayed frame fails under the next counter")
    }

    func testALinkRefusesItsOwnIdentity() throws {
        let alice = try device("alice", store: nil)
        let one = try MeshLinkHandshake(keyring: alice, initiator: true)
        let two = try MeshLinkHandshake(keyring: alice, initiator: false)
        XCTAssertThrowsError(try one.receiveHello(two.hello))
    }
}

final class RoomSealRoundTripTests: XCTestCase {
    let room = "chat-direct:0123456789abcdef0123456789abcdef:v1"
    let scope = "direct:0123456789abcdef0123456789abcdef"
    let now: Int64 = 1_790_000_000_000

    func seal(_ store: SecureStore, acct: String, node: String? = nil, policy: ChatTrustPolicy = .strict) throws -> RoomSeal {
        try RoomSeal.loadOrCreate(
            room, scope: scope, policy: policy, acct: acct, node: node ?? "\(acct)-node", secureStore: store, now: now)
    }

    func testDevicesExchangeBundlesSealAndOpen() throws {
        let alice = try seal(InMemorySecureStore(), acct: "alice")
        let bob = try seal(InMemorySecureStore(), acct: "bob")
        XCTAssertTrue(RoomSeal.isKeyFrame(alice.keyFrame(now: now)))
        XCTAssertEqual(alice.acceptBundle(bob.keyFrame(now: now), now: now)?.state, .approved)
        XCTAssertEqual(bob.acceptBundle(alice.keyFrame(now: now), now: now)?.state, .approved)
        XCTAssertEqual(alice.approvedKeyIds, [bob.keyId])

        let frame = try alice.seal(["type": "chat", "id": "m1", "senderId": "alice-node", "text": "no setup required"], now: now)
        XCTAssertTrue(RoomSeal.isSealedFrame(frame))
        XCTAssertEqual(frame["v"], .int(ChatV4.version), "a DM room seals v4")
        XCTAssertFalse(JSON.encode(frame).contains("no setup required"))
        XCTAssertEqual(RoomSeal.wrappedKeyIds(frame), [bob.keyId], "a v4 frame has no entry for its sender")
        let opened = bob.open(frame, now: now)
        XCTAssertEqual(opened.plain?["text"], "no setup required")
        XCTAssertEqual(opened.senderAcct, "alice")
        XCTAssertEqual(bob.open(frame, now: now), .replay)
        XCTAssertEqual(alice.open(frame, now: now), .own, "the sender keeps its own copy in its history")

        let stranger = try seal(InMemorySecureStore(), acct: "eve")
        XCTAssertEqual(stranger.acceptBundle(alice.keyFrame(now: now), now: now)?.state, .approved)
        XCTAssertEqual(stranger.open(frame, now: now), .notForMe)

        var relabelled = frame
        relabelled["ctr"] = 99
        XCTAssertEqual(bob.open(relabelled, now: now), .invalid)
        let lying = try alice.seal(["type": "chat", "id": "m2", "senderId": "bob-node", "text": "I am Bob"], now: now)
        XCTAssertEqual(bob.open(lying, now: now), .invalid)
        let silent = try alice.seal(["type": "chat", "id": "m3", "text": "no sender"], now: now)
        XCTAssertEqual(bob.open(silent, now: now).authenticatedPlain?["senderId"], "alice-node", "the signed device id fills in")
        XCTAssertEqual(bob.open(try alice.seal(["type": "chat", "id": "m4"], now: now), now: now).senderAcct, "alice")
    }

    func testRelayDirectMessagesGoOnlyToTheRecipient() throws {
        let room = "world-general"
        func member(_ name: String) throws -> RoomSeal {
            try RoomSeal.loadOrCreate(
                room, policy: .open, acct: name, node: "\(name)-node", secureStore: InMemorySecureStore(), now: now)
        }
        let alice = try member("alice")
        let bob = try member("bob")
        let carol = try member("carol")
        for peer in [bob, carol] {
            alice.acceptBundle(peer.keyFrame(now: now), now: now)
            peer.acceptBundle(alice.keyFrame(now: now), now: now)
        }
        let dm = try alice.seal(["type": "dm", "to": "bob-node", "senderId": "alice-node", "text": "psst"], forNodes: ["bob-node"], now: now)
        XCTAssertEqual(Set(RoomSeal.wrappedKeyIds(dm)), [alice.keyId, bob.keyId])
        XCTAssertEqual(bob.open(dm, now: now).plain?["text"], "psst")
        XCTAssertEqual(carol.open(dm, now: now), .notForMe)
    }

    func testAnOpenRoomOverSixtyFourDevicesKeepsTheMostRecentlySeen() throws {
        let room = "busy-room"
        let me = try RoomSeal.loadOrCreate(room, policy: .open, acct: "me", node: "me-node", secureStore: InMemorySecureStore(), now: now)
        var kids: [String] = []
        for index in 0..<70 {
            let peer = try ChatKeyring.fromSecrets(identitySeed: Ed25519.generateSeed())
            peer.addPrekey(try ChatPrekeySecret(xPriv: X25519.generatePrivateKey(), pqSeed: CryptoBytes.random(64), ts: now))
            let bundle = try RoomSeal.bundle(
                room: room, acct: "peer-\(index)", node: "peer-\(index)-node", keyring: peer,
                prekey: try XCTUnwrap(peer.newestPrekey()))
            XCTAssertEqual(me.acceptBundle(bundle, now: now + Int64(index))?.state, .approved)
            kids.append(peer.kid)
        }
        let frame = try me.seal(["type": "chat", "id": "x"], now: now + 100)
        let sealedTo = Set(RoomSeal.wrappedKeyIds(frame))
        XCTAssertEqual(sealedTo.count, ChatV3.maxRecipients)
        XCTAssertTrue(sealedTo.contains(me.keyId))
        XCTAssertTrue(sealedTo.isSuperset(of: kids.suffix(63)))
        XCTAssertFalse(sealedTo.contains(kids[0]))
    }

    func testHistoryReplaySkipsOnlyTheSeenCheckInOpenRooms() throws {
        let alice = try seal(InMemorySecureStore(), acct: "alice", policy: .open)
        let bob = try seal(InMemorySecureStore(), acct: "bob", policy: .open)
        alice.acceptBundle(bob.keyFrame(now: now), now: now)
        bob.acceptBundle(alice.keyFrame(now: now), now: now)
        let frame = try alice.seal(["type": "chat", "id": "h1", "senderId": "alice-node", "text": "x"], now: now)
        XCTAssertEqual(bob.open(frame, now: now).name, "opened")
        XCTAssertEqual(bob.open(frame, now: now), .replay)
        XCTAssertEqual(bob.open(frame, now: now, history: true).name, "opened")
        var tampered = frame
        tampered["ts"] = .int(now + 1)
        XCTAssertEqual(bob.open(tampered, now: now, history: true), .invalid, "the signature still applies")
        XCTAssertEqual(bob.open(frame, now: now + ChatV3.prekeyLifeMs + 1, history: true), .invalid, "so does the time")
        let dmAlice = try seal(InMemorySecureStore(), acct: "alice")
        let dmBob = try seal(InMemorySecureStore(), acct: "bob")
        dmAlice.acceptBundle(dmBob.keyFrame(now: now), now: now)
        dmBob.acceptBundle(dmAlice.keyFrame(now: now), now: now)
        let dm = try dmAlice.seal(["type": "chat", "id": "h2", "senderId": "alice-node", "text": "x"], now: now)
        XCTAssertEqual(dmBob.open(dm, now: now).name, "opened")
        XCTAssertEqual(dmBob.open(dm, now: now, history: true), .replay, "v4 has no history exception: the key is gone")
    }

    func testStrictTrustHoldsANewDeviceOfAKnownAccount() throws {
        let bob = try seal(InMemorySecureStore(), acct: "bob")
        let alice = try seal(InMemorySecureStore(), acct: "alice")
        let alicePhone = try seal(InMemorySecureStore(), acct: "alice", node: "alice-phone")
        _ = bob.keyFrame(now: now)
        XCTAssertEqual(bob.acceptBundle(alice.keyFrame(now: now), now: now)?.state, .approved)
        XCTAssertEqual(bob.acceptBundle(alicePhone.keyFrame(now: now), now: now)?.state, .pending)
        alicePhone.acceptBundle(bob.keyFrame(now: now), now: now)
        let frame = try alicePhone.seal(["type": "chat", "id": "p1", "senderId": "alice-phone", "text": "new phone"], now: now)
        XCTAssertEqual(bob.open(frame, now: now), .held(alicePhone.keyId))
        let toAlice = try bob.seal(["type": "chat", "id": "b1", "senderId": "bob-node", "text": "hi"], now: now)
        XCTAssertFalse(RoomSeal.wrappedKeyIds(toAlice).contains(alicePhone.keyId), "nothing is sealed to a pending device")
        XCTAssertEqual(bob.trust.records(in: .pending).map(\.kid), [alicePhone.keyId])
        bob.approve(alicePhone.keyId)
        XCTAssertEqual(bob.reopenHeld(now: now).first?.plain?["text"], "new phone")
        let open = try seal(InMemorySecureStore(), acct: "bob", policy: .open)
        open.acceptBundle(alice.keyFrame(now: now), now: now)
        XCTAssertEqual(open.acceptBundle(alicePhone.keyFrame(now: now), now: now)?.state, .approved, "open rooms approve")
    }

    func testStateSurvivesAReload() throws {
        let store = InMemorySecureStore()
        let bob = try seal(store, acct: "bob")
        let alice = try seal(InMemorySecureStore(), acct: "alice")
        let alicePhone = try seal(InMemorySecureStore(), acct: "alice", node: "alice-phone")
        bob.acceptBundle(alice.keyFrame(now: now), now: now)
        bob.acceptBundle(alicePhone.keyFrame(now: now), now: now)
        alice.acceptBundle(bob.keyFrame(now: now), now: now)
        alicePhone.acceptBundle(bob.keyFrame(now: now), now: now)
        let first = try alice.seal(["type": "chat", "id": "a1", "senderId": "alice-node", "text": "one"], now: now)
        XCTAssertEqual(bob.open(first, now: now).name, "opened")
        let held = try alicePhone.seal(["type": "chat", "id": "a2", "senderId": "alice-phone", "text": "two"], now: now)
        XCTAssertEqual(bob.open(held, now: now).name, "held")
        XCTAssertNotNil(store.read(ChatKeyring.identityStoreKey))
        XCTAssertNotNil(store.read(ChatKeyring.prekeysStoreKey))
        XCTAssertNotNil(store.read(ChatTrustStore.storeKey(scope)))

        let reloadedKeyring = try XCTUnwrap(try ChatKeyring.load(store, create: false))
        XCTAssertEqual(reloadedKeyring.kid, bob.keyId)
        let reloaded = try RoomSeal(
            room: room, scope: scope, policy: .strict, acct: "bob", node: "bob-node", keyring: reloadedKeyring, store: store)
        XCTAssertEqual(reloaded.open(first, now: now), .replay, "the seen window is persisted")
        XCTAssertEqual(reloaded.trust.records(in: .pending).map(\.kid), [alicePhone.keyId])
        XCTAssertEqual(reloaded.trust.heldCount(room), 1)
        reloaded.approve(alicePhone.keyId)
        XCTAssertEqual(reloaded.reopenHeld(now: now).first?.plain?["text"], "two")
        let next = try reloaded.seal(["type": "chat", "id": "b1", "senderId": "bob-node", "text": "x"], now: now)
        XCTAssertEqual(next["ctr"], 1)
        let again = try XCTUnwrap(try ChatKeyring.load(store, create: false))
        let afterReload = try RoomSeal(
            room: room, scope: scope, policy: .strict, acct: "bob", node: "bob-node", keyring: again, store: store)
        XCTAssertEqual(try afterReload.seal(["type": "chat", "id": "b2", "senderId": "bob-node"], now: now)["ctr"], 2)
        XCTAssertNil(try ChatKeyring.load(InMemorySecureStore(), create: false), "the extension never creates keys")
    }

    func testPrekeysRotateDailyAreWipedAtExpiryAndCappedAtTen() throws {
        let store = InMemorySecureStore()
        let keyring = try XCTUnwrap(try ChatKeyring.load(store, create: true))
        XCTAssertTrue(keyring.rotateIfNeeded(now: now))
        XCTAssertFalse(keyring.rotateIfNeeded(now: now + ChatV3.dayMs - 1))
        XCTAssertTrue(keyring.rotateIfNeeded(now: now + ChatV3.dayMs))
        XCTAssertEqual(keyring.prekeyIds.count, 2)
        var t = now + ChatV3.dayMs
        for _ in 0..<12 {
            t += ChatV3.dayMs / 2
            keyring.addPrekey(try ChatPrekeySecret(xPriv: X25519.generatePrivateKey(), pqSeed: CryptoBytes.random(64), ts: t))
        }
        XCTAssertEqual(keyring.prekeyIds.count, ChatV3.maxPrekeys)
        keyring.wipeExpired(now: t + ChatV3.prekeyLifeMs + 1)
        XCTAssertTrue(keyring.prekeyIds.isEmpty)
        XCTAssertEqual(try ChatKeyring.load(store, create: false)?.prekeyIds.count, 0, "the wipe reaches storage")
    }

    func testTheSharedKeyringIsOnePerStore() throws {
        let store = InMemorySecureStore()
        let one = try ChatKeyring.shared(store)
        let two = try ChatKeyring.shared(store)
        XCTAssertTrue(one === two)
        XCTAssertFalse(one === (try ChatKeyring.shared(InMemorySecureStore())))
        XCTAssertNotNil(one.newestPrekey())
    }

    func testMalformedFramesAreInvalid() throws {
        let alice = try seal(InMemorySecureStore(), acct: "alice")
        let bob = try seal(InMemorySecureStore(), acct: "bob")
        alice.acceptBundle(bob.keyFrame(now: now), now: now)
        bob.acceptBundle(alice.keyFrame(now: now), now: now)
        let frame = try alice.seal(["type": "chat", "id": "m1", "senderId": "alice-node", "text": "x"], now: now)
        func mutate(_ change: (inout JSONObject) -> Void) -> JSONObject {
            var copy = frame
            change(&copy)
            return copy
        }
        let bad: [JSONObject] = [
            mutate { $0["v"] = 2 },
            mutate { $0["nonce"] = "AAAA" },
            mutate { $0["body"] = "" },
            mutate { $0["ctr"] = .double(1) },
            mutate { $0["ctr"] = 0 },
            mutate { $0["keys"] = .array([]) },
            mutate { $0["keys"] = .array((frame["keys"]?.arrayValue ?? []) + (frame["keys"]?.arrayValue ?? [])) },
            mutate { $0["sig"] = "short" },
            mutate { $0["kid"] = .string(bob.keyId) },
        ]
        for (index, frame) in bad.enumerated() {
            XCTAssertEqual(bob.open(frame, now: now), .invalid, "case \(index)")
        }
        XCTAssertFalse(RoomSeal.isSealedFrame(["type": "chat", "text": "plaintext"]))
        XCTAssertFalse(RoomSeal.isSealedFrame(["type": "sealed", "v": 2, "kid": "short"]))
    }
}
