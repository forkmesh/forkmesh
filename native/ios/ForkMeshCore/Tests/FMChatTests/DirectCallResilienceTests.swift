import FMBase
import Foundation
import XCTest

@testable import FMChat

final class DirectCallResilienceTests: XCTestCase {
    static let conversation = "direct:" + String(repeating: "e", count: 32)
    nonisolated(unsafe) var signals: [(String, String, JSONObject)] = []
    nonisolated(unsafe) var gate = SleepGate()

    override func tearDown() {
        gate.drain()
        super.tearDown()
    }

    @MainActor func makeEngine() -> (DirectCallEngine, FakeCallPeerFactory) {
        signals = []
        gate = SleepGate()
        let gate = gate
        let engine = DirectCallEngine(
            selfId: { "me-node" }, accountName: { "me" }, partnerFor: { _ in "pat" },
            sendSignal: { [weak self] conversation, type, extra in
                self?.signals.append((conversation, type, extra))
                return true
            },
            sleep: { try await gate.sleep($0) })
        let factory = FakeCallPeerFactory()
        engine.peerFactory = factory
        return (engine, factory)
    }

    @MainActor func frame(_ type: String, call: String, from: String = "pat-node", extra: JSONObject = [:]) -> JSONObject {
        var plain: JSONObject = [
            "type": .string(type), "call": .string(call), "senderId": .string(from), "sender": "pat",
            "channel": .string(Self.conversation), "to": "me-node",
        ]
        for (key, value) in extra { plain[key] = value }
        return plain
    }

    @MainActor func sent(_ type: String) -> [JSONObject] { signals.filter { $0.1 == type }.map(\.2) }

    @MainActor func sdp(_ fields: JSONObject) -> String { fields["sdp"]?.objectValue?["sdp"]?.text ?? "" }

    @MainActor func liveCaller() async throws -> (DirectCallEngine, FakeCallPeer) {
        let (engine, factory) = makeEngine()
        _ = await engine.start(Self.conversation, media: .video)
        let call = engine.callId
        await engine.handleFrame(frame("call-accept", call: call), conversation: Self.conversation)
        await engine.handleFrame(
            frame("call-answer", call: call, extra: ["sdp": ["type": "answer", "sdp": "callee-sdp"]]),
            conversation: Self.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        peer.onLive?()
        XCTAssertEqual(engine.phase, .live)
        return (engine, peer)
    }

    @MainActor func liveCallee() async throws -> (DirectCallEngine, FakeCallPeer) {
        let (engine, factory) = makeEngine()
        await engine.handleFrame(frame("call-invite", call: "c-9", extra: ["media": "audio"]), conversation: Self.conversation)
        let accepted = await engine.acceptInvite()
        XCTAssertTrue(accepted)
        await engine.handleFrame(
            frame("call-offer", call: "c-9", extra: ["sdp": ["type": "offer", "sdp": "caller-sdp"]]),
            conversation: Self.conversation)
        let peer = try XCTUnwrap(factory.peers.first)
        peer.onLive?()
        XCTAssertEqual(engine.phase, .live)
        return (engine, peer)
    }

    @MainActor func testTheCalleeResendsItsAcceptUntilTheOfferArrives() async throws {
        let (engine, factory) = makeEngine()
        await engine.handleFrame(frame("call-invite", call: "c-1", extra: ["media": "video"]), conversation: Self.conversation)
        await engine.acceptInvite()
        XCTAssertEqual(sent("call-accept").count, 1)
        for round in 2...(DirectCallEngine.acceptResends + 1) {
            await gate.fire(DirectCallEngine.acceptResend)
            let resent = await waitUntil { self.sent("call-accept").count == round }
            XCTAssertTrue(resent, "accept \(round)")
        }
        XCTAssertTrue(sent("call-accept").allSatisfy { $0["call"]?.text == "c-1" && $0["to"]?.text == "pat-node" })
        XCTAssertEqual(DirectCallEngine.acceptResends, 8)
        XCTAssertTrue(factory.peers.isEmpty)
        await engine.hangUp()
    }

    @MainActor func testTheAcceptResendStopsOnceTheOfferArrives() async throws {
        let (engine, _) = makeEngine()
        await engine.handleFrame(frame("call-invite", call: "c-2", extra: ["media": "audio"]), conversation: Self.conversation)
        await engine.acceptInvite()
        await gate.fire(DirectCallEngine.acceptResend)
        let resent = await waitUntil { self.sent("call-accept").count == 2 }
        XCTAssertTrue(resent)
        await engine.handleFrame(
            frame("call-offer", call: "c-2", extra: ["sdp": ["type": "offer", "sdp": "caller-sdp"]]),
            conversation: Self.conversation)
        await gate.fire(DirectCallEngine.acceptResend)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(sent("call-accept").count, 2)
        XCTAssertEqual(sent("call-answer").count, 1)
        await engine.hangUp()
    }

    @MainActor func testADuplicateAcceptFromTheChosenDeviceResendsTheOffer() async throws {
        let (engine, factory) = makeEngine()
        _ = await engine.start(Self.conversation, media: .audio)
        let call = engine.callId
        await engine.handleFrame(frame("call-accept", call: call), conversation: Self.conversation)
        XCTAssertEqual(sent("call-offer").count, 1)
        await engine.handleFrame(frame("call-accept", call: call), conversation: Self.conversation)
        XCTAssertEqual(sent("call-offer").map(sdp), ["caller-sdp", "caller-sdp"])
        await engine.handleFrame(frame("call-accept", call: call, from: "pat-tablet"), conversation: Self.conversation)
        XCTAssertEqual(sent("call-offer").count, 2)
        XCTAssertEqual(factory.peers.count, 1)
        await engine.handleFrame(
            frame("call-answer", call: call, extra: ["sdp": ["type": "answer", "sdp": "callee-sdp"]]),
            conversation: Self.conversation)
        await engine.handleFrame(frame("call-accept", call: call), conversation: Self.conversation)
        XCTAssertEqual(sent("call-offer").count, 2)
        await engine.hangUp()
    }

    @MainActor func testTheCallerResendsItsOfferUntilAnAnswerIsApplied() async throws {
        let (engine, factory) = makeEngine()
        _ = await engine.start(Self.conversation, media: .video)
        let call = engine.callId
        await engine.handleFrame(frame("call-accept", call: call), conversation: Self.conversation)
        for round in 2...3 {
            await gate.fire(DirectCallEngine.offerResend)
            let resent = await waitUntil { self.sent("call-offer").count == round }
            XCTAssertTrue(resent, "offer \(round)")
        }
        XCTAssertTrue(sent("call-offer").allSatisfy { self.sdp($0) == "caller-sdp" && $0["to"]?.text == "pat-node" })
        await engine.handleFrame(
            frame("call-answer", call: call, extra: ["sdp": ["type": "answer", "sdp": "callee-sdp"]]),
            conversation: Self.conversation)
        await gate.fire(DirectCallEngine.offerResend)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(sent("call-offer").count, 3)
        XCTAssertEqual(factory.peers.first?.acceptedAnswers.count, 1)
        XCTAssertEqual(DirectCallEngine.offerResends, 5)
        await engine.hangUp()
    }

    @MainActor func testTheOfferIsResentAtMostFiveTimes() async throws {
        let (engine, _) = makeEngine()
        _ = await engine.start(Self.conversation, media: .audio)
        await engine.handleFrame(frame("call-accept", call: engine.callId), conversation: Self.conversation)
        for round in 2...(DirectCallEngine.offerResends + 1) {
            await gate.fire(DirectCallEngine.offerResend)
            let resent = await waitUntil { self.sent("call-offer").count == round }
            XCTAssertTrue(resent, "offer \(round)")
        }
        XCTAssertEqual(sent("call-offer").count, 6)
        await engine.hangUp()
    }

    @MainActor func testADuplicateOfferGetsTheStoredAnswerAndANewOneRenegotiates() async throws {
        let (engine, peer) = try await liveCallee()
        XCTAssertEqual(sent("call-answer").map(sdp), ["callee-sdp"])
        await engine.handleFrame(
            frame("call-offer", call: "c-9", extra: ["sdp": ["type": "offer", "sdp": "caller-sdp"]]),
            conversation: Self.conversation)
        XCTAssertEqual(sent("call-answer").map(sdp), ["callee-sdp", "callee-sdp"])
        XCTAssertEqual(peer.answeredOffers.count, 1)
        await engine.handleFrame(
            frame("call-offer", call: "c-9", extra: ["sdp": ["type": "offer", "sdp": "restart-sdp-1"]]),
            conversation: Self.conversation)
        XCTAssertEqual(peer.answeredOffers.map { $0["sdp"]?.text ?? "" }, ["caller-sdp", "restart-sdp-1"])
        XCTAssertEqual(sent("call-answer").map(sdp), ["callee-sdp", "callee-sdp", "callee-sdp-2"])
        await engine.handleFrame(
            frame("call-offer", call: "c-9", extra: ["sdp": ["type": "offer", "sdp": "restart-sdp-1"]]),
            conversation: Self.conversation)
        XCTAssertEqual(sent("call-answer").map(sdp).last, "callee-sdp-2")
        XCTAssertEqual(peer.answeredOffers.count, 2)
        XCTAssertEqual(engine.phase, .live)
        await engine.hangUp()
    }

    @MainActor func testADuplicateAnswerIsIgnoredAndNeverEndsTheCall() async throws {
        let (engine, peer) = try await liveCaller()
        let call = engine.callId
        await engine.handleFrame(
            frame("call-answer", call: call, extra: ["sdp": ["type": "answer", "sdp": "callee-sdp"]]),
            conversation: Self.conversation)
        XCTAssertEqual(peer.acceptedAnswers.count, 1)
        peer.answerFails = true
        await engine.handleFrame(
            frame("call-answer", call: call, extra: ["sdp": ["type": "answer", "sdp": "stale-sdp"]]),
            conversation: Self.conversation)
        XCTAssertEqual(peer.acceptedAnswers.count, 1)
        XCTAssertEqual(engine.phase, .live)
        XCTAssertFalse(peer.closed)
        XCTAssertTrue(sent("call-bye").isEmpty)
        await engine.hangUp()
    }

    @MainActor func testACallThatNeverConnectsEndsAfterThirtySeconds() async throws {
        let (engine, factory) = makeEngine()
        await engine.handleFrame(frame("call-invite", call: "c-3", extra: ["media": "video"]), conversation: Self.conversation)
        await engine.acceptInvite()
        await engine.handleFrame(
            frame("call-offer", call: "c-3", extra: ["sdp": ["type": "offer", "sdp": "caller-sdp"]]),
            conversation: Self.conversation)
        XCTAssertNil(engine.ending)
        await gate.fire(DirectCallEngine.connectTimeout)
        let ended = await waitUntil { engine.phase == nil }
        XCTAssertTrue(ended)
        XCTAssertEqual(engine.ending, DirectCallEngine.connectFailed)
        XCTAssertEqual(engine.ending?.title, "Could not connect")
        XCTAssertEqual(engine.ending?.message, "The call could not connect. Try again.")
        XCTAssertEqual(sent("call-bye").first?["call"]?.text, "c-3")
        XCTAssertEqual(factory.peers.first?.closed, true)
        XCTAssertEqual(DirectCallEngine.connectTimeout, 30)
        engine.dismissEnding()
        XCTAssertNil(engine.ending)
    }

    @MainActor func testALiveCallIsNotEndedByTheConnectTimer() async throws {
        let (engine, _) = try await liveCaller()
        await gate.fire(DirectCallEngine.connectTimeout)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(engine.phase, .live)
        XCTAssertNotNil(engine.liveSince)
        XCTAssertTrue(sent("call-bye").isEmpty)
        await engine.hangUp()
        XCTAssertNil(engine.liveSince)
    }

    @MainActor func testTheCallerRestartsIceAfterTwoSecondsDisconnectedAndThenEverySix() async throws {
        let (engine, peer) = try await liveCaller()
        let call = engine.callId
        peer.onConnection?(.disconnected)
        XCTAssertTrue(engine.reconnecting)
        XCTAssertEqual(engine.stateLine, "Reconnecting…")
        XCTAssertEqual(peer.restarts, 0)
        await gate.fire(DirectCallEngine.dropGrace)
        let first = await waitUntil { self.sent("call-offer").count == 2 }
        XCTAssertTrue(first)
        XCTAssertEqual(sdp(sent("call-offer")[1]), "restart-sdp-1")
        await gate.fire(DirectCallEngine.restartEvery)
        let second = await waitUntil { self.sent("call-offer").count == 3 }
        XCTAssertTrue(second)
        XCTAssertEqual(sdp(sent("call-offer")[2]), "restart-sdp-2")
        await engine.handleFrame(
            frame("call-answer", call: call, extra: ["sdp": ["type": "answer", "sdp": "callee-sdp-2"]]),
            conversation: Self.conversation)
        XCTAssertEqual(peer.acceptedAnswers.map { $0["sdp"]?.text ?? "" }, ["callee-sdp", "callee-sdp-2"])
        peer.onConnection?(.connected)
        XCTAssertFalse(engine.reconnecting)
        XCTAssertEqual(engine.stateLine, "Connected · end-to-end encrypted")
        await gate.fire(DirectCallEngine.restartEvery)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(peer.restarts, 2)
        XCTAssertEqual(engine.phase, .live)
        await engine.hangUp()
    }

    @MainActor func testAShortDisconnectNeedsNoRestart() async throws {
        let (engine, peer) = try await liveCaller()
        peer.onConnection?(.disconnected)
        peer.onConnection?(.connected)
        await gate.fire(DirectCallEngine.dropGrace)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(peer.restarts, 0)
        XCTAssertFalse(engine.reconnecting)
        await engine.hangUp()
    }

    @MainActor func testAFailedOrSilentConnectionRestartsAtOnce() async throws {
        let (engine, peer) = try await liveCaller()
        peer.onConnection?(.failed)
        let restarted = await waitUntil { peer.restarts == 1 }
        XCTAssertTrue(restarted)
        XCTAssertFalse(peer.closed)
        peer.onConnection?(.disconnected)
        try await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(peer.restarts, 1)
        peer.onConnection?(.connected)
        peer.onConnection?(.stalled)
        let again = await waitUntil { peer.restarts == 2 }
        XCTAssertTrue(again)
        XCTAssertEqual(engine.stateLine, "Reconnecting…")
        await engine.hangUp()
    }

    @MainActor func testTheCalleeShowsReconnectingAndWaitsForTheCaller() async throws {
        let (engine, peer) = try await liveCallee()
        peer.onConnection?(.failed)
        XCTAssertEqual(engine.stateLine, "Reconnecting…")
        try await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(peer.restarts, 0)
        XCTAssertFalse(peer.closed)
        XCTAssertEqual(sent("call-offer").count, 0)
        await engine.handleFrame(
            frame("call-offer", call: "c-9", extra: ["sdp": ["type": "offer", "sdp": "restart-sdp-1"]]),
            conversation: Self.conversation)
        XCTAssertEqual(sent("call-answer").map(sdp).last, "callee-sdp-2")
        peer.onLive?()
        XCTAssertFalse(engine.reconnecting)
        XCTAssertEqual(engine.phase, .live)
        await engine.hangUp()
    }

    @MainActor func testADropThatDoesNotComeBackEndsTheCallAfterThirtySeconds() async throws {
        let (engine, peer) = try await liveCallee()
        peer.onConnection?(.disconnected)
        await gate.fire(DirectCallEngine.reconnectBudget)
        let ended = await waitUntil { engine.phase == nil }
        XCTAssertTrue(ended)
        XCTAssertEqual(engine.ending, DirectCallEngine.dropped)
        XCTAssertEqual(engine.ending?.title, "Call lost")
        XCTAssertEqual(engine.ending?.message, "The connection dropped and did not come back. Try calling again.")
        XCTAssertEqual(sent("call-bye").count, 1)
        XCTAssertTrue(peer.closed)
    }

    @MainActor func testPeersWithoutRestartKeepTheirOldBehaviour() async throws {
        let (engine, peer) = try await liveCaller()
        peer.restartable = false
        peer.onConnection?(.failed)
        try await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(sent("call-offer").count, 1)
        peer.onClosed?()
        let ended = await waitUntil { engine.phase == nil }
        XCTAssertTrue(ended)
        XCTAssertNil(engine.ending)
    }

    @MainActor func testAHeldInviteOutlivesTheStaleTimerUntilAnswered() async throws {
        let (engine, _) = makeEngine()
        await engine.handleFrame(frame("call-invite", call: "c-4", extra: ["media": "audio"]), conversation: Self.conversation)
        engine.holdInvite()
        await gate.fire(DirectCallEngine.inviteStale)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(engine.invite?.callId, "c-4")
        let accepted = await engine.acceptInvite()
        XCTAssertTrue(accepted)
        XCTAssertEqual(engine.phase, .connecting)
        await engine.hangUp()
        await engine.handleFrame(frame("call-invite", call: "c-5", extra: ["media": "audio"]), conversation: Self.conversation)
        engine.holdInvite()
        await engine.handleFrame(frame("call-bye", call: "c-5"), conversation: Self.conversation)
        let gone = await engine.acceptInvite()
        XCTAssertFalse(gone)
        XCTAssertNil(engine.phase)
    }

    @MainActor func testRestartCandidatesFollowTheirOfferAndWaitForTheAnswer() async throws {
        let (engine, peer) = try await liveCaller()
        let call = engine.callId
        peer.onConnection?(.failed)
        let sent = await waitUntil { self.signals.contains { $0.1 == "call-ice" } }
        XCTAssertTrue(sent)
        let order = signals.map(\.1).filter { $0 == "call-offer" || $0 == "call-ice" }
        XCTAssertEqual(order, ["call-offer", "call-offer", "call-ice"])
        XCTAssertEqual(signals.last?.2["ice"]?.objectValue?["candidate"]?.text, "restart-candidate-1")
        await engine.handleFrame(
            frame("call-ice", call: call, extra: ["ice": ["candidate": "early-restart"]]), conversation: Self.conversation)
        XCTAssertTrue(peer.added.isEmpty)
        await engine.handleFrame(
            frame("call-answer", call: call, extra: ["sdp": ["type": "answer", "sdp": "callee-sdp-2"]]),
            conversation: Self.conversation)
        XCTAssertEqual(peer.added.map { $0["candidate"]?.text ?? "" }, ["early-restart"])
        await engine.handleFrame(
            frame("call-ice", call: call, extra: ["ice": ["candidate": "late"]]), conversation: Self.conversation)
        XCTAssertEqual(peer.added.count, 2)
        await engine.hangUp()
    }

    @MainActor func testTheCalleeSendsItsRestartCandidatesAfterItsAnswer() async throws {
        let (engine, _) = try await liveCallee()
        signals = []
        await engine.handleFrame(
            frame("call-offer", call: "c-9", extra: ["sdp": ["type": "offer", "sdp": "restart-sdp-1"]]),
            conversation: Self.conversation)
        let sent = await waitUntil { self.signals.contains { $0.1 == "call-ice" } }
        XCTAssertTrue(sent)
        XCTAssertEqual(signals.map(\.1), ["call-answer", "call-ice"])
        XCTAssertEqual(signals.last?.2["ice"]?.objectValue?["candidate"]?.text, "answer-candidate-2")
        await engine.hangUp()
    }

    @MainActor func testResumeMediaReachesThePeer() async throws {
        let (engine, peer) = try await liveCaller()
        engine.resumeMedia()
        XCTAssertEqual(peer.resumed, 1)
        await engine.hangUp()
    }
}
