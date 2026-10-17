import FMBase
import Foundation
import XCTest

@testable import FMCrypto

func chatV4Vectors() throws -> JSONObject {
    var root = URL(fileURLWithPath: #filePath)
    for _ in 0..<6 { root.deleteLastPathComponent() }
    let data = try Data(contentsOf: root.appendingPathComponent("conformance/chat/v4/vectors.json"))
    return try XCTUnwrap(try JSON.parse(data).objectValue)
}

struct ShuffleSeed: RandomNumberGenerator {
    var state: UInt64

    mutating func next() -> UInt64 {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        return state
    }
}

struct ChatV4Fixture {
    let vectors: JSONObject
    var room: String { vectors.str("room") }
    var scope: String { vectors.str("scope") }
    var now: Int64 { vectors.int64("now") }
    var participants: Set<String> { Set(vectors.strings("participants")) }

    func b64(_ text: String) -> [UInt8] { ChatV3.unb64(text) ?? [] }

    func b64u(_ text: String) -> [UInt8] { ChatV3.unb64u(text) ?? [] }

    func kid(_ name: String) -> String { vectors.obj("devices").obj(name).str("kid") }

    func frame(_ name: String) -> JSONObject { vectors.obj("frames").obj(name) }

    func device(_ name: String, state: String? = nil, store: InMemorySecureStore = InMemorySecureStore(), roomName: String? = nil)
        throws -> RoomSeal
    {
        let secrets = vectors.obj("devices").obj(name)
        var stored: [JSONValue] = []
        for spk in secrets.maps("prekeys") {
            let made = try ChatPrekeySecret(xPriv: b64(spk.str("xPriv")), pqSeed: b64(spk.str("pqSeed")), ts: spk.int64("ts"))
            XCTAssertEqual(made.prekey.id, spk.str("id"))
            XCTAssertEqual(ChatV3.b64u(made.prekey.pq), spk.str("pq"))
            stored.append(.object(spk))
        }
        if store.read(ChatKeyring.identityStoreKey) == nil {
            store.write(ChatKeyring.identityStoreKey, secrets.str("identitySeed"))
            store.write(ChatKeyring.prekeysStoreKey, JSON.encode(.array(stored)))
        }
        let keyring = try XCTUnwrap(try ChatKeyring.load(store, create: false))
        XCTAssertEqual(keyring.kid, secrets.str("kid"))
        let seal = try RoomSeal(
            room: room, scope: scope, policy: .strict, acct: name, node: vectors.obj("nodes").str(name),
            participants: participants, keyring: keyring, store: store)
        XCTAssertEqual(seal.version, ChatV4.version)
        for (_, bundle) in vectors.obj("bundles") {
            XCTAssertEqual(seal.acceptBundle(bundle.objectValue ?? [:], now: now)?.state, .approved)
        }
        if let state { keyring.importRatchets(room, vectors.obj("states").obj(state)) }
        guard let roomName else { return seal }
        return try RoomSeal(
            room: roomName, scope: scope, policy: .strict, acct: name, node: vectors.obj("nodes").str(name),
            participants: participants, keyring: keyring, store: store)
    }
}

final class ChatRatchetVectorTests: XCTestCase {
    var fixture = ChatV4Fixture(vectors: [:])
    var vectors: JSONObject { fixture.vectors }
    var room: String { fixture.room }
    var now: Int64 { fixture.now }
    var scope: String { fixture.scope }

    override func setUpWithError() throws {
        fixture = ChatV4Fixture(vectors: try chatV4Vectors())
    }

    func b64(_ text: String) -> [UInt8] { fixture.b64(text) }

    func b64u(_ text: String) -> [UInt8] { fixture.b64u(text) }

    func kid(_ name: String) -> String { fixture.kid(name) }

    func frame(_ name: String) -> JSONObject { fixture.frame(name) }

    func device(_ name: String, state: String? = nil, store: InMemorySecureStore = InMemorySecureStore()) throws -> RoomSeal {
        try fixture.device(name, state: state, store: store)
    }

    func testKdfVectors() {
        let k = vectors.obj("kdf")
        let s = k.obj("sessionId")
        XCTAssertEqual(ChatV4.sessionId(dh: b64u(s.str("dh")), ct: b64(s.str("ct"))), s.str("sid"))
        let i = k.obj("init")
        let initial = ChatV4.kdfInit(
            room: i.str("room"), sender: i.str("sender"), recipient: i.str("recipient"), spk: i.str("spk"),
            sid: i.str("sid"), ss1: b64(i.str("ss1")), ss2: b64(i.str("ss2")))
        XCTAssertEqual(ChatV3.b64(initial.rk), i.str("rk"))
        XCTAssertEqual(ChatV3.b64(initial.ck), i.str("ck"))
        let r = k.obj("rootStep")
        let step = ChatV4.kdfRk(b64(r.str("rk")), dhOut: b64(r.str("dhOut")), ss: b64(r.str("ss")))
        XCTAssertEqual(ChatV3.b64(step.rk), r.str("nextRk"))
        XCTAssertEqual(ChatV3.b64(step.ck), r.str("ck"))
        let c = k.obj("chainStep")
        let chain = ChatV4.kdfCk(b64(c.str("ck")))
        XCTAssertEqual(ChatV3.b64(chain.mk), c.str("mk"))
        XCTAssertEqual(ChatV3.b64(chain.next), c.str("nextCk"))
    }

    func testTheVectorSequenceOpensAsTheReferenceDoes() throws {
        var devices: [String: (seal: RoomSeal, store: InMemorySecureStore)] = [:]
        for step in vectors.maps("sequence") {
            let name = step.str("device")
            if step["state"] != nil {
                let store = InMemorySecureStore()
                let state = step["state"]?.stringValue
                devices[name] = (try device(name, state: state, store: store), store)
            }
            var current = try XCTUnwrap(devices[name])
            if step.bool("clearSeen") {
                current.store.delete("chat/e2ee/seenV3/\(ChatV3.storeSuffix(room))")
                current = (try device(name, store: current.store), current.store)
                devices[name] = current
            }
            let result = current.seal.open(frame(step.str("frame")), now: step.int64("at"))
            XCTAssertEqual(result.name, step.str("expect"), "\(step)")
            if step.str("expect") == "opened" {
                guard case .opened(let plain, let senderKid, _, _) = result else { continue }
                XCTAssertEqual(plain, vectors.obj("plain").obj(step.str("plain")))
                XCTAssertEqual(senderKid, kid(name == "alice" ? "bob" : "alice"))
            }
            if step.str("expect") == "noSession" { XCTAssertEqual(result, .noSession(kid("alice"))) }
            if step.str("frame") == "m1" && step.str("expect") == "opened" {
                let exported = current.seal.keyring.exportRatchets(room)
                let session = try XCTUnwrap(exported.obj("peers").obj(kid("alice")).maps("sessions").first)
                let want = vectors.obj("bobAfterM1")
                XCTAssertEqual(session.str("rk"), want.str("rk"))
                XCTAssertEqual(session.str("ckr"), want.str("ckr"))
                XCTAssertEqual(session.int64("nr"), want.int64("nr"))
                XCTAssertEqual(session.list("skipped").count, want.int("skipped"))
            }
        }
    }

    func testStatesImportAndExportUnchanged() throws {
        for name in ["aliceAfterM2", "bobAfterM1", "bobAfterR1"] {
            let owner = name.hasPrefix("alice") ? "alice" : "bob"
            let store = InMemorySecureStore()
            let seal = try device(owner, state: name, store: store)
            XCTAssertEqual(seal.keyring.exportRatchets(room), vectors.obj("states").obj(name), name)
            let text = try XCTUnwrap(store.read(ChatKeyring.ratchetStoreKey(room)))
            XCTAssertEqual(try JSON.parse(text).objectValue, vectors.obj("states").obj(name), name)
        }
        XCTAssertEqual(ChatKeyring.ratchetStoreKey(room), "chat/e2ee/ratchetV4/\(ChatV3.b64u(Array(room.utf8)))")
    }

    func testHeadersAndShapes() {
        let frames = vectors.obj("frames")
        for name in ["m1", "m2", "r1", "m3", "m4", "empty"] {
            XCTAssertNotNil(RoomSeal.shapeV4(frames.obj(name)), name)
            XCTAssertTrue(RoomSeal.isSealedFrame(frames.obj(name)), name)
            XCTAssertEqual(RoomSeal.frameVersion(frames.obj(name)), ChatV4.version)
        }
        XCTAssertEqual(RoomSeal.wrappedKeyIds(frame("m1")), [kid("bob")])
        XCTAssertTrue(RoomSeal.wrappedKeyIds(frame("empty")).isEmpty)
    }

    func testAliceSeesHerOwnFrameAsOwn() throws {
        let alice = try device("alice", state: "aliceAfterM2")
        XCTAssertEqual(alice.open(frame("m1"), now: now + 4000), .own)
        XCTAssertEqual(alice.open(frame("m1"), now: now + 4000, commit: false), .own)
    }

    func testAnOpenRoomIgnoresV4AndAStrictRoomIgnoresV3() throws {
        let bob = try device("bob")
        let keyring = bob.keyring
        let open = try RoomSeal(room: room, scope: scope, policy: .open, acct: "bob", node: "node-bob-phone", keyring: keyring)
        XCTAssertEqual(open.version, ChatV3.version)
        XCTAssertEqual(open.open(frame("m1"), now: now + 2000), .invalid)
        let v3 = try chatV3Vectors()
        let v3Frame = v3.obj("sealed").obj("first").obj("frame")
        XCTAssertEqual(bob.open(v3Frame, now: v3.int64("now")), .invalid)
    }
}

final class ChatRatchetRoundTripTests: XCTestCase {
    var helper = ChatV4Fixture(vectors: [:])
    var room: String { helper.room }
    var now: Int64 { helper.now }

    override func setUpWithError() throws {
        helper = ChatV4Fixture(vectors: try chatV4Vectors())
    }

    func pair() throws -> (alice: RoomSeal, bob: RoomSeal) {
        (try helper.device("alice"), try helper.device("bob"))
    }

    func chat(_ text: String) -> JSONObject { ["type": "chat", "id": .string(text), "text": .string(text)] }

    func testAConversationWithShuffledDelivery() throws {
        let (alice, bob) = try pair()
        var rng = ShuffleSeed(state: 4)
        var at = now
        for turn in 0..<6 {
            let (sender, receiver) = turn % 2 == 0 ? (alice, bob) : (bob, alice)
            var frames = try (0..<5).map { try sender.seal(chat("\(turn)-\($0)"), now: at + Int64($0)) }
            frames.shuffle(using: &rng)
            for frame in frames {
                let result = receiver.open(frame, now: at + 10)
                XCTAssertEqual(result.name, "opened", "turn \(turn)")
            }
            at += 100
        }
    }

    func testALateMessageFromAnOldChainOpens() throws {
        let (alice, bob) = try pair()
        let late = try alice.seal(chat("late"), now: now)
        XCTAssertEqual(bob.open(try alice.seal(chat("a2"), now: now + 1), now: now + 2).name, "opened")
        XCTAssertEqual(alice.open(try bob.seal(chat("b1"), now: now + 3), now: now + 4).name, "opened")
        XCTAssertEqual(bob.open(try alice.seal(chat("a3"), now: now + 5), now: now + 6).name, "opened")
        XCTAssertEqual(bob.open(late, now: now + 7).plain?["text"], "late")
        XCTAssertEqual(bob.open(late, now: now + 7), .replay)
    }

    func testSimultaneousFirstMessagesConverge() throws {
        let (alice, bob) = try pair()
        let fromAlice = try alice.seal(chat("a"), now: now)
        let fromBob = try bob.seal(chat("b"), now: now)
        XCTAssertEqual(bob.open(fromAlice, now: now + 1).name, "opened")
        XCTAssertEqual(alice.open(fromBob, now: now + 1).name, "opened")
        for i in 0..<3 {
            let t = now + Int64(10 * i)
            XCTAssertEqual(bob.open(try alice.seal(chat("a\(i)"), now: t), now: t).name, "opened")
            XCTAssertEqual(alice.open(try bob.seal(chat("b\(i)"), now: t + 5), now: t + 5).name, "opened")
        }
    }

    func testAResetRecoversALostSession() throws {
        let (alice, bob) = try pair()
        XCTAssertEqual(bob.open(try alice.seal(chat("a1"), now: now), now: now).name, "opened")
        XCTAssertEqual(alice.open(try bob.seal(chat("b1"), now: now + 1), now: now + 1).name, "opened")
        bob.resetSessions(with: alice.keyId)
        let broken = try alice.seal(chat("a2"), now: now + 2)
        XCTAssertEqual(bob.open(broken, now: now + 3), .noSession(alice.keyId))
        bob.resetSessions(with: alice.keyId)
        let reset = try bob.seal(["type": .string(ChatV4.resetType), "senderId": "node-bob-phone"], for: [alice.keyId], now: now + 4)
        let entry = try XCTUnwrap(reset["keys"]?.arrayValue?.first?.objectValue)
        XCTAssertEqual(entry.str("sid"), ChatV4.sessionId(dh: helper.b64u(entry.str("dh")), ct: helper.b64(entry.str("ct"))))
        let opened = alice.open(reset, now: now + 5)
        XCTAssertEqual(opened.plain?["type"], .string(ChatV4.resetType))
        XCTAssertEqual(bob.open(try alice.seal(chat("a2 again"), now: now + 6), now: now + 7).name, "opened")
    }

    func testCommitFalseChangesNothing() throws {
        let store = InMemorySecureStore()
        let alice = try helper.device("alice")
        let bob = try helper.device("bob", store: store)
        let frame = try alice.seal(chat("a1"), now: now)
        let before = bob.keyring.exportRatchets(room)
        let stored = store.readAll()
        XCTAssertEqual(bob.open(frame, now: now, commit: false).name, "opened")
        XCTAssertEqual(bob.peek(frame, now: now).name, "opened")
        XCTAssertEqual(bob.keyring.exportRatchets(room), before)
        XCTAssertEqual(store.readAll(), stored, "nothing is written")
        XCTAssertEqual(bob.open(frame, now: now).name, "opened")
        XCTAssertEqual(bob.open(frame, now: now), .replay)
    }

    func testTheExtensionOverlayOpensWithoutWriting() throws {
        let alice = try helper.device("alice")
        let store = InMemorySecureStore()
        let bob = try helper.device("bob", store: store)
        XCTAssertEqual(bob.open(try alice.seal(chat("a1"), now: now), now: now).name, "opened")
        let second = try alice.seal(chat("a2"), now: now + 1)
        let stored = store.readAll()
        let overlay = OverlaySecureStore(base: store)
        let keyring = try XCTUnwrap(try ChatKeyring.load(overlay, create: false))
        let peeker = try RoomSeal(room: room, scope: bob.scope, policy: .strict, acct: nil, node: nil, keyring: keyring, store: overlay)
        XCTAssertEqual(peeker.open(second, now: now + 1, commit: false).plain?["text"], "a2")
        XCTAssertEqual(store.readAll(), stored)
        XCTAssertEqual(bob.open(second, now: now + 1).plain?["text"], "a2")
    }

    func testTooManySkippedNeedsAReset() throws {
        let (alice, bob) = try pair()
        for _ in 0...ChatV4.maxSkip { _ = try alice.seal(chat("lost"), now: now) }
        let far = try alice.seal(chat("far"), now: now)
        XCTAssertEqual(bob.open(far, now: now), .noSession(alice.keyId))
    }

    func testAnExpiredPrekeyCannotStartASession() throws {
        let (alice, bob) = try pair()
        let frame = try alice.seal(chat("a1"), now: now)
        let exp = try XCTUnwrap(bob.keyring.newestPrekey()).exp
        bob.keyring.wipeExpired(now: exp + 1)
        XCTAssertEqual(bob.open(frame, now: now), .noSession(alice.keyId))
    }

    func testAnotherRoomAndASelfEntryAreInvalid() throws {
        let alice = try helper.device("alice")
        let other = try helper.device("bob", roomName: "another-room")
        let frame = try alice.seal(chat("a1"), now: now)
        XCTAssertEqual(other.open(frame, now: now), .invalid)
        let bob = try helper.device("bob")
        var keys = frame["keys"]?.arrayValue ?? []
        var selfEntry = try XCTUnwrap(keys.first?.objectValue)
        selfEntry["kid"] = .string(alice.keyId)
        keys.append(.object(selfEntry))
        keys.sort { ChatV3.ascending($0.str("kid"), $1.str("kid")) }
        var withSelf = frame
        withSelf["keys"] = .array(keys)
        XCTAssertEqual(bob.open(withSelf, now: now), .invalid)
        var tampered = frame
        var entry = try XCTUnwrap(frame["keys"]?.arrayValue?.first?.objectValue)
        entry["n"] = 7
        tampered["keys"] = .array([.object(entry)])
        XCTAssertEqual(bob.open(tampered, now: now), .invalid)
        XCTAssertEqual(bob.open(frame, now: now).name, "opened", "a bad frame burns nothing")
    }

    func testSessionsAreBounded() throws {
        let (alice, bob) = try pair()
        for i in 0..<(ChatV4.sessionsKept + 3) {
            alice.resetSessions(with: bob.keyId)
            XCTAssertEqual(bob.open(try alice.seal(chat("\(i)"), now: now + Int64(i)), now: now + Int64(i)).name, "opened")
        }
        let peer = bob.keyring.ratchetPeer(room, alice.keyId)
        XCTAssertEqual(peer.sessions.count, ChatV4.sessionsKept)
        XCTAssertEqual(peer.sessions.first?.sid, peer.current)
    }

    func testSkippedKeysAreWipedAfterEightDays() throws {
        let (alice, bob) = try pair()
        let early = try alice.seal(chat("early"), now: now)
        XCTAssertEqual(bob.open(try alice.seal(chat("second"), now: now), now: now).name, "opened")
        XCTAssertEqual(bob.keyring.ratchetPeer(room, alice.keyId).sessions.first?.skipped.count, 1)
        let later = now + ChatV4.skippedLifeMs + 1
        let fresh = try alice.seal(chat("third"), now: now + ChatV3.dayMs)
        XCTAssertEqual(bob.open(fresh, now: later - ChatV3.dayMs).name, "opened")
        XCTAssertEqual(bob.keyring.ratchetPeer(room, alice.keyId).sessions.first?.skipped.count, 1)
        let last = try alice.seal(chat("fourth"), now: now + 2 * ChatV3.dayMs)
        XCTAssertEqual(bob.open(last, now: later).name, "opened")
        XCTAssertEqual(bob.keyring.ratchetPeer(room, alice.keyId).sessions.first?.skipped.count, 0)
        XCTAssertEqual(bob.open(early, now: now).name, "replay")
    }

    func testOwnZeroEntryAndHeldFrames() throws {
        let (alice, bob) = try pair()
        let empty = try alice.seal(chat("nobody"), for: [], now: now)
        XCTAssertEqual(RoomSeal.wrappedKeyIds(empty), [])
        XCTAssertTrue(RoomSeal.isSealedFrame(empty))
        XCTAssertEqual(alice.open(empty, now: now), .own)
        XCTAssertEqual(bob.open(empty, now: now), .notForMe)
        let frame = try alice.seal(chat("to bob"), now: now)
        XCTAssertFalse(RoomSeal.wrappedKeyIds(frame).contains(alice.keyId), "no entry for the sender")
        XCTAssertEqual(alice.open(frame, now: now), .own)
        bob.block(alice.keyId)
        XCTAssertEqual(bob.open(frame, now: now), .held(alice.keyId))
        XCTAssertEqual(bob.trust.heldCount(room), 0)
        bob.approve(alice.keyId)
        bob.trust.setState(alice.keyId, .pending)
        XCTAssertEqual(bob.open(frame, now: now, commit: false), .held(alice.keyId))
        XCTAssertEqual(bob.trust.heldCount(room), 0, "a peek holds nothing")
        XCTAssertEqual(bob.open(frame, now: now), .held(alice.keyId))
        XCTAssertEqual(bob.trust.heldCount(room), 1)
        bob.approve(alice.keyId)
        let reopened = bob.reopenHeld(now: now)
        XCTAssertEqual(reopened.first?.plain?["text"], "to bob")
        XCTAssertEqual(bob.open(frame, now: now), .replay)
    }

    func testStateIsStoredBeforeTheFrameLeavesAndSurvivesAReload() throws {
        let aliceStore = InMemorySecureStore()
        let alice = try helper.device("alice", store: aliceStore)
        let bob = try helper.device("bob")
        XCTAssertNil(aliceStore.read(ChatKeyring.ratchetStoreKey(room)))
        let first = try alice.seal(chat("one"), now: now)
        let saved = try XCTUnwrap(aliceStore.read(ChatKeyring.ratchetStoreKey(room)))
        let book = try XCTUnwrap(try JSON.parse(saved).objectValue)
        XCTAssertEqual(book.int64("v"), 4)
        XCTAssertEqual(book.obj("peers").obj(bob.keyId).maps("sessions").first?.int64("ns"), 1)
        let reloaded = try helper.device("alice", store: aliceStore)
        let second = try reloaded.seal(chat("two"), now: now + 1)
        XCTAssertEqual(second["keys"]?.arrayValue?.first?.int64("n"), 1)
        XCTAssertEqual(bob.open(second, now: now + 1).name, "opened")
        XCTAssertEqual(bob.open(first, now: now + 1).name, "opened")
    }

    func testTheResetPlaintextIsRecognised() {
        XCTAssertEqual(ChatV4.resetType, "e2ee-reset")
        XCTAssertEqual([RoomSealOpen.own, .noSession("k")].map(\.name), ["own", "noSession"])
    }
}
