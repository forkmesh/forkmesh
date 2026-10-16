import FMBase
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class FakeCallPeer: DirectCallPeer {
    var onIceCandidate: (@MainActor (JSONObject) -> Void)?
    var onLive: (@MainActor () -> Void)?
    var onClosed: (@MainActor () -> Void)?
    var offered = false
    var answeredOffer: JSONObject?
    var acceptedAnswer: JSONObject?
    var added: [JSONObject] = []
    var muted: [Bool] = []
    var cameraOff: [Bool] = []
    var faces: [String] = []
    var backgrounds: [String] = []
    var voices: [String] = []
    var voiceObserver: (@MainActor (String) -> Void)?
    var closed = false

    func offer() async throws -> JSONObject {
        offered = true
        return ["type": "offer", "sdp": "caller-sdp"]
    }

    func answer(offer: JSONObject) async throws -> JSONObject {
        answeredOffer = offer
        return ["type": "answer", "sdp": "callee-sdp"]
    }

    func acceptAnswer(_ answer: JSONObject) async throws { acceptedAnswer = answer }

    func add(candidate: JSONObject) async { added.append(candidate) }

    func setMuted(_ value: Bool) { muted.append(value) }

    func setCameraOff(_ value: Bool) { cameraOff.append(value) }

    func setFace(_ id: String) { faces.append(id) }

    func setFace(_ id: String, image: String) { faces.append(image.isEmpty ? id : "\(id):\(image)") }

    func setBackground(_ id: String, image: String) { backgrounds.append(image.isEmpty ? id : "\(id):\(image)") }

    func setVoice(_ id: String) { voices.append(id) }

    func observeVoice(_ handler: @escaping @MainActor (String) -> Void) { voiceObserver = handler }

    func close() { closed = true }
}

@MainActor
final class FakeCallPeerFactory: DirectCallPeerMaking {
    var peers: [FakeCallPeer] = []
    var iceServers: [String] = []

    func makePeer(media: DirectCallKind, iceServers: [String]) async throws -> DirectCallPeer {
        self.iceServers = iceServers
        let peer = FakeCallPeer()
        peers.append(peer)
        return peer
    }
}

final class SleepGate: @unchecked Sendable {
    private let lock = NSLock()
    private var waiters: [(TimeInterval, CheckedContinuation<Void, Error>)] = []

    func sleep(_ seconds: TimeInterval) async throws {
        try await withCheckedThrowingContinuation { continuation in
            lock.lock()
            waiters.append((seconds, continuation))
            lock.unlock()
        }
    }

    private func take(_ seconds: TimeInterval) -> [CheckedContinuation<Void, Error>] {
        lock.withLock {
            let matched = waiters.filter { $0.0 == seconds }.map(\.1)
            waiters.removeAll { $0.0 == seconds }
            return matched
        }
    }

    func fire(_ seconds: TimeInterval) async {
        for _ in 0..<4000 {
            let matched = take(seconds)
            if matched.isEmpty {
                try? await Task.sleep(nanoseconds: 1_000_000)
                continue
            }
            for continuation in matched { continuation.resume() }
            return
        }
    }

    func drain() {
        lock.lock()
        let all = waiters
        waiters = []
        lock.unlock()
        for (_, continuation) in all { continuation.resume(throwing: CancellationError()) }
    }
}

final class DirectCallsTests: XCTestCase {
    static let conversation = "direct:" + String(repeating: "d", count: 32)
    nonisolated(unsafe) var signals: [(String, String, JSONObject)] = []
    nonisolated(unsafe) var gate = SleepGate()

    override func tearDown() {
        gate.drain()
        super.tearDown()
    }

    @MainActor func makeEngine(partner: String = "pat") -> DirectCallEngine {
        signals = []
        gate = SleepGate()
        let gate = gate
        let engine = DirectCallEngine(
            selfId: { "me-node" }, accountName: { "me" }, partnerFor: { _ in partner },
            sendSignal: { [weak self] conversation, type, extra in
                self?.signals.append((conversation, type, extra))
                return true
            },
            sleep: { try await gate.sleep($0) })
        return engine
    }

    @MainActor func frame(_ type: String, call: String = "c-1", from: String = "pat-node", sender: String = "pat", extra: JSONObject = [:])
        -> JSONObject
    {
        var plain: JSONObject = [
            "type": .string(type), "call": .string(call), "senderId": .string(from), "sender": .string(sender),
            "channel": .string(DirectCallsTests.conversation),
        ]
        for (key, value) in extra { plain[key] = value }
        return plain
    }

    @MainActor func signalTypes() -> [String] { signals.map(\.1) }

    @MainActor func testAChoiceMadeInTheFaceChangerReachesALiveVideoCall() async throws {
        let look = FaceChangerLook()
        signals = []
        gate = SleepGate()
        let gate = gate
        let engine = DirectCallEngine(
            selfId: { "me-node" }, accountName: { "me" }, partnerFor: { _ in "pat" },
            sendSignal: { [weak self] conversation, type, extra in
                self?.signals.append((conversation, type, extra))
                return true
            },
            sleep: { try await gate.sleep($0) }, look: look)
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        await engine.start(DirectCallsTests.conversation, media: .video)
        await engine.handleFrame(
            frame("call-accept", call: engine.callId, extra: ["to": "me-node"]),
            conversation: DirectCallsTests.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        look.setFace("ingrid")
        look.setBackground("office")
        XCTAssertEqual(peer.faces, ["ingrid"])
        XCTAssertEqual(peer.voices.last, "ingrid")
        XCTAssertEqual(peer.backgrounds, ["office"])
        XCTAssertEqual(engine.face, "ingrid")
        engine.detach()
        look.setFace("maya")
        XCTAssertEqual(peer.faces, ["ingrid"])
        await engine.hangUp()
    }

    @MainActor func testTheLookTellsItsListenersWhatChanged() {
        let look = FaceChangerLook()
        var parts: [FaceChangerPart] = []
        let token = look.addListener { parts.append($0) }
        look.setFace("theo")
        look.setBackground("blur")
        look.setVoiceChoice("robot")
        look.setBackground("blur")
        look.setFace("nobody")
        XCTAssertEqual(parts, [.face, .background, .voice])
        XCTAssertEqual(look.effectiveVoice, "robot")
        look.removeListener(token)
        look.setFace("kofi")
        XCTAssertEqual(parts.count, 3)
    }

    @MainActor func testFaceIsAppliedToAVideoPeerBeforeItOffers() async throws {
        let engine = makeEngine()
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        var saved: [String] = []
        engine.look.onFaceChanged = { saved.append($0) }
        engine.setFace("nobody")
        XCTAssertEqual(engine.face, "")
        engine.setFace("kofi")
        XCTAssertEqual(saved, ["kofi"])
        await engine.start(DirectCallsTests.conversation, media: .video)
        let callId = engine.callId
        await engine.handleFrame(
            frame("call-accept", call: callId, extra: ["to": "me-node"]), conversation: DirectCallsTests.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        XCTAssertEqual(peer.faces, ["kofi"])
        XCTAssertTrue(peer.offered)
        engine.setFace("ren")
        engine.setFace("")
        XCTAssertEqual(peer.faces, ["kofi", "ren", ""])
        await engine.hangUp()
        XCTAssertEqual(engine.face, "")
        XCTAssertEqual(saved, ["kofi", "ren", ""])
    }

    @MainActor func testYourPhotoFaceNeedsAPictureAndReachesThePeer() async throws {
        let engine = makeEngine()
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        var saved: [String] = []
        var pictures: [String] = []
        engine.look.onFaceChanged = { saved.append($0) }
        engine.look.onFaceImageChanged = { pictures.append($0) }
        engine.setFace(FaceChangerLook.customFace)
        XCTAssertEqual(engine.face, "")
        engine.setFace(FaceChangerLook.customFace, image: "data:image/jpeg;base64,AA")
        XCTAssertEqual(engine.face, "custom")
        XCTAssertEqual(engine.effectiveVoice, "")
        await engine.start(DirectCallsTests.conversation, media: .video)
        await engine.handleFrame(
            frame("call-accept", call: engine.callId, extra: ["to": "me-node"]),
            conversation: DirectCallsTests.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        XCTAssertEqual(peer.faces, ["custom:data:image/jpeg;base64,AA"])
        engine.setFace(FaceChangerLook.customFace, image: "data:image/jpeg;base64,BB")
        engine.setFace("maya")
        engine.setFace(FaceChangerLook.customFace)
        XCTAssertEqual(peer.faces, ["custom:data:image/jpeg;base64,AA", "custom:data:image/jpeg;base64,BB", "maya", "custom:data:image/jpeg;base64,BB"])
        XCTAssertEqual(saved, ["custom", "maya", "custom"])
        XCTAssertEqual(pictures, ["data:image/jpeg;base64,AA", "data:image/jpeg;base64,BB"])
        XCTAssertEqual(FaceChangerLook.faceChoices.map(\.id), ["maya", "theo", "kofi", "ingrid", "ren", "custom"])
        await engine.hangUp()
    }

    @MainActor func testBackgroundIsAppliedToAVideoPeerAndRemembered() async throws {
        let engine = makeEngine()
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        var saved: [String] = []
        engine.look.onBackgroundChanged = { saved.append($1.isEmpty ? $0 : "\($0):\($1)") }
        engine.setBackground("nowhere")
        engine.setBackground(FaceChangerLook.customBackground)
        XCTAssertEqual(engine.background, "")
        engine.setBackground("world")
        await engine.start(DirectCallsTests.conversation, media: .video)
        await engine.handleFrame(
            frame("call-accept", call: engine.callId, extra: ["to": "me-node"]),
            conversation: DirectCallsTests.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        XCTAssertEqual(peer.backgrounds, ["world"])
        engine.setBackground(FaceChangerLook.customBackground, image: "data:image/jpeg;base64,AA")
        engine.setBackground("blur")
        engine.setBackground(FaceChangerLook.customBackground)
        XCTAssertEqual(peer.backgrounds, ["world", "custom:data:image/jpeg;base64,AA", "blur", "custom:data:image/jpeg;base64,AA"])
        XCTAssertEqual(saved, ["world", "custom:data:image/jpeg;base64,AA", "blur:data:image/jpeg;base64,AA", "custom:data:image/jpeg;base64,AA"])
        XCTAssertEqual(FaceChangerLook.backgrounds.map(\.id), ["", "blur", "blur-strong", "world", "office", "meeting", "library", "skyline", "custom"])
        await engine.hangUp()
    }

    @MainActor func testVoiceCallsNeverSendAFace() async throws {
        let engine = makeEngine()
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        engine.setFace("maya")
        await engine.start(DirectCallsTests.conversation, media: .audio)
        await engine.handleFrame(
            frame("call-accept", call: engine.callId, extra: ["to": "me-node"]),
            conversation: DirectCallsTests.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        XCTAssertEqual(peer.faces, [])
        XCTAssertEqual(engine.face, "maya")
        XCTAssertEqual(FaceChangerLook.faces.map(\.id), ["maya", "theo", "kofi", "ingrid", "ren"])
        await engine.hangUp()
    }

    @MainActor func testVoiceMatchesTheFaceUntilAnotherVoiceIsChosen() async throws {
        let engine = makeEngine()
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        var saved: [String] = []
        engine.look.onVoiceChanged = { saved.append($0) }
        XCTAssertEqual(engine.voiceChoice, "face")
        engine.setVoiceChoice("nobody")
        XCTAssertEqual(engine.voiceChoice, "face")
        engine.setFace("kofi")
        XCTAssertEqual(engine.effectiveVoice, "kofi")
        await engine.start(DirectCallsTests.conversation, media: .audio)
        await engine.handleFrame(
            frame("call-accept", call: engine.callId, extra: ["to": "me-node"]),
            conversation: DirectCallsTests.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        XCTAssertEqual(peer.faces, [])
        XCTAssertEqual(peer.voices, ["kofi"])
        XCTAssertEqual(engine.voice, "kofi")
        engine.setFace("maya")
        XCTAssertEqual(peer.voices, ["kofi", "maya"])
        engine.setVoiceChoice("off")
        XCTAssertEqual(peer.voices, ["kofi", "maya", ""])
        engine.setVoiceChoice("ren")
        engine.setFace("theo")
        XCTAssertEqual(peer.voices, ["kofi", "maya", "", "ren"])
        peer.voiceObserver?("")
        XCTAssertEqual(engine.voice, "")
        XCTAssertEqual(saved, ["off", "ren"])
        XCTAssertEqual(FaceChangerLook.voiceLabel("ren"), "Ren")
        XCTAssertEqual(FaceChangerLook.voiceLabel(""), "Your own")
        XCTAssertEqual(FaceChangerLook.voiceLabel("older-woman"), "Older woman")
        XCTAssertEqual(FaceChangerLook.voices.map(\.id), [
            "maya", "theo", "kofi", "ingrid", "ren",
            "deep", "older-man", "young-man", "warm", "low", "older-woman", "teen", "neutral", "robot",
        ])
        await engine.hangUp()
        XCTAssertEqual(engine.voice, "")
        XCTAssertEqual(engine.voiceChoice, "ren")
    }

    @MainActor func testCallerFlowOffersAfterAcceptAndQueuesIce() async throws {
        let engine = makeEngine()
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        let started = await engine.start(DirectCallsTests.conversation, media: .video)
        XCTAssertTrue(started)
        XCTAssertEqual(engine.phase, .ringing)
        XCTAssertEqual(engine.partner, "pat")
        XCTAssertEqual(signalTypes(), ["call-invite"])
        XCTAssertEqual(signals[0].2["media"]?.text, "video")
        let callId = signals[0].2["call"]?.text ?? ""
        XCTAssertFalse(callId.isEmpty)
        await engine.handleFrame(
            frame("call-ice", call: callId, extra: ["to": "me-node", "ice": ["candidate": "early"]]),
            conversation: DirectCallsTests.conversation)
        await engine.handleFrame(
            frame("call-accept", call: callId, extra: ["to": "me-node"]), conversation: DirectCallsTests.conversation)
        XCTAssertEqual(engine.phase, .connecting)
        XCTAssertEqual(factory.iceServers, ["stun:stun.cloudflare.com:3478"])
        let peer = try XCTUnwrap(factory.peers.first)
        XCTAssertTrue(peer.offered)
        XCTAssertEqual(signalTypes().last, "call-offer")
        XCTAssertEqual(signals.last?.2["to"]?.text, "pat-node")
        XCTAssertEqual(signals.last?.2["sdp"]?.objectValue?["sdp"]?.text, "caller-sdp")
        XCTAssertTrue(peer.added.isEmpty)
        await engine.handleFrame(
            frame("call-answer", call: callId, extra: ["to": "me-node", "sdp": ["type": "answer", "sdp": "callee-sdp"]]),
            conversation: DirectCallsTests.conversation)
        XCTAssertEqual(peer.acceptedAnswer?["sdp"]?.text, "callee-sdp")
        await engine.handleFrame(
            frame("call-ice", call: callId, extra: ["to": "me-node", "ice": ["candidate": "late"]]),
            conversation: DirectCallsTests.conversation)
        XCTAssertEqual(peer.added.map { $0["candidate"]?.text ?? "" }, ["late"])
        peer.onLive?()
        XCTAssertEqual(engine.phase, .live)
        engine.toggleMute()
        XCTAssertEqual(peer.muted, [true])
        engine.toggleCamera()
        XCTAssertEqual(peer.cameraOff, [true])
        await engine.hangUp()
        XCTAssertNil(engine.phase)
        XCTAssertTrue(peer.closed)
        XCTAssertEqual(signalTypes().last, "call-bye")
    }

    @MainActor func testCalleeFlowAcceptsAndAnswers() async throws {
        let engine = makeEngine()
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        await engine.handleFrame(frame("call-invite", extra: ["media": "audio"]), conversation: DirectCallsTests.conversation)
        let invite = try XCTUnwrap(engine.invite)
        XCTAssertEqual(invite.partner, "pat")
        XCTAssertEqual(invite.media, .audio)
        await engine.acceptInvite()
        XCTAssertNil(engine.invite)
        XCTAssertEqual(engine.phase, .connecting)
        XCTAssertEqual(signalTypes(), ["call-accept"])
        XCTAssertEqual(signals[0].2["to"]?.text, "pat-node")
        await engine.handleFrame(
            frame("call-offer", extra: ["to": "me-node", "sdp": ["type": "offer", "sdp": "caller-sdp"]]),
            conversation: DirectCallsTests.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        XCTAssertEqual(peer.answeredOffer?["sdp"]?.text, "caller-sdp")
        XCTAssertEqual(signalTypes().last, "call-answer")
        XCTAssertEqual(signals.last?.2["sdp"]?.objectValue?["sdp"]?.text, "callee-sdp")
        await engine.handleFrame(
            frame("call-bye"), conversation: DirectCallsTests.conversation)
        XCTAssertNil(engine.phase)
        XCTAssertTrue(peer.closed)
    }

    @MainActor func testInviteFiltersOwnAccountBusyAndOtherDevices() async throws {
        let engine = makeEngine()
        engine.peerFactory = FakeCallPeerFactory()
        await engine.handleFrame(
            frame("call-invite", from: "my-tablet", sender: "me", extra: ["media": "audio"]),
            conversation: DirectCallsTests.conversation)
        XCTAssertNil(engine.invite)
        await engine.handleFrame(
            frame("call-offer", extra: ["to": "someone-else", "sdp": ["type": "offer"]]),
            conversation: DirectCallsTests.conversation)
        XCTAssertNil(engine.phase)
        _ = await engine.start(DirectCallsTests.conversation, media: .audio)
        await engine.handleFrame(
            frame("call-invite", call: "c-2", extra: ["media": "audio"]), conversation: DirectCallsTests.conversation)
        XCTAssertNil(engine.invite)
        let busy = try XCTUnwrap(signals.last)
        XCTAssertEqual(busy.1, "call-decline")
        XCTAssertEqual(busy.2["reason"]?.text, "busy")
        XCTAssertEqual(busy.2["call"]?.text, "c-2")
        await engine.hangUp()
        await engine.handleFrame(frame("call-invite", extra: ["media": "audio"]), conversation: DirectCallsTests.conversation)
        XCTAssertNotNil(engine.invite)
        await engine.handleFrame(
            frame("call-accept", from: "my-tablet", sender: "me", extra: ["to": "pat-node"]),
            conversation: DirectCallsTests.conversation)
        XCTAssertNil(engine.invite)
    }

    @MainActor func testUnansweredRingEndsAsMissed() async throws {
        let engine = makeEngine()
        engine.peerFactory = FakeCallPeerFactory()
        var missed: [(String, DirectCallKind)] = []
        engine.onMissed = { missed.append(($0, $1)) }
        _ = await engine.start(DirectCallsTests.conversation, media: .audio)
        await gate.fire(DirectCallEngine.inviteResend)
        let resent = await waitUntil { self.signalTypes().filter { $0 == "call-invite" }.count >= 2 }
        XCTAssertTrue(resent)
        await gate.fire(DirectCallEngine.ringTimeout)
        let ended = await waitUntil { engine.phase == nil }
        XCTAssertTrue(ended)
        XCTAssertEqual(signalTypes().last, "call-bye")
        XCTAssertEqual(signals.last?.2["reason"]?.text, "missed")
        XCTAssertEqual(missed.map(\.0), [DirectCallsTests.conversation])
        XCTAssertEqual(missed.map(\.1), [.audio])
    }

    @MainActor func testDeclineEndsTheCallerQuietly() async throws {
        let engine = makeEngine()
        engine.peerFactory = FakeCallPeerFactory()
        _ = await engine.start(DirectCallsTests.conversation, media: .audio)
        let callId = signals[0].2["call"]?.text ?? ""
        await engine.handleFrame(
            frame("call-decline", call: callId, extra: ["to": "me-node", "reason": "declined"]),
            conversation: DirectCallsTests.conversation)
        XCTAssertNil(engine.phase)
        XCTAssertFalse(signalTypes().contains("call-bye"))
    }

    @MainActor func testCallsAreUnavailableWithoutAPeerFactory() async throws {
        let engine = makeEngine()
        XCTAssertFalse(engine.available)
        let started = await engine.start(DirectCallsTests.conversation, media: .audio)
        XCTAssertFalse(started)
        await engine.handleFrame(frame("call-invite", extra: ["media": "audio"]), conversation: DirectCallsTests.conversation)
        XCTAssertNil(engine.invite)
        XCTAssertTrue(signals.isEmpty)
    }

    @MainActor func testStaleInviteDismissesItself() async throws {
        let engine = makeEngine()
        engine.peerFactory = FakeCallPeerFactory()
        await engine.handleFrame(frame("call-invite", extra: ["media": "video"]), conversation: DirectCallsTests.conversation)
        XCTAssertNotNil(engine.invite)
        await gate.fire(DirectCallEngine.inviteStale)
        let cleared = await waitUntil { engine.invite == nil }
        XCTAssertTrue(cleared)
    }
}
