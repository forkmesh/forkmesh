import XCTest

@testable import FMChat

final class DirectCallKitLedgerTests: XCTestCase {
    static let conversation = "direct:" + String(repeating: "a", count: 32)
    static let callId = "0b6c1d2e-3f40-4a5b-8c6d-7e8f90a1b2c3"

    private func push(_ callId: String = DirectCallKitLedgerTests.callId) -> DirectCallRingPush {
        DirectCallRingPush(callId: callId, conversation: Self.conversation, caller: "bob")
    }

    private func invite(_ callId: String = DirectCallKitLedgerTests.callId, media: DirectCallKind = .video)
        -> DirectCallInvite
    {
        DirectCallInvite(conversation: Self.conversation, callId: callId, from: "bob-phone", partner: "bob", media: media)
    }

    @MainActor func testAPushParsesOnlyACallForADirectConversation() async {
        let fields = [
            "topic": "calls", "conversation": Self.conversation, "callId": Self.callId.uppercased(), "caller": " Bob ",
            "origin": "https://app.example",
        ]
        XCTAssertEqual(
            DirectCallRingPush(fields: fields),
            DirectCallRingPush(callId: Self.callId, conversation: Self.conversation, caller: "", origin: "https://app.example"),
            "a push names no caller: CallKit shows a generic name until the sealed invite opens")
        let ledger = DirectCallKitLedger()
        let pushed = try? XCTUnwrap(DirectCallRingPush(fields: fields))
        XCTAssertEqual(ledger.push(pushed ?? push()).first, .report(id: Self.callId, caller: "", video: false))
        XCTAssertEqual(
            ledger.engine(invite: invite(), activeCallId: ""), [.update(id: Self.callId, caller: "bob", video: true)])
        XCTAssertNil(DirectCallRingPush(fields: fields.merging(["topic": "direct-messages"]) { $1 }))
        XCTAssertNil(DirectCallRingPush(fields: fields.merging(["conversation": "#general"]) { $1 }))
        XCTAssertNil(DirectCallRingPush(fields: fields.merging(["conversation": "direct:short"]) { $1 }))
        XCTAssertEqual(DirectCallRingPush(fields: fields.merging(["callId": "nope"]) { $1 })?.callId, "")
    }

    @MainActor func testAPushRingsThenTheInviteFillsInTheCallAndCallKitAnswers() async {
        let ledger = DirectCallKitLedger()
        XCTAssertEqual(
            ledger.push(push()),
            [
                .report(id: Self.callId, caller: "bob", video: false), .open(conversation: Self.conversation),
                .wait(id: Self.callId),
            ])
        XCTAssertTrue(ledger.ringing(Self.callId))
        XCTAssertEqual(
            ledger.engine(invite: invite(), activeCallId: ""), [.update(id: Self.callId, caller: "bob", video: true)])
        XCTAssertEqual(ledger.engine(invite: invite(), activeCallId: ""), [])
        XCTAssertEqual(ledger.answered(Self.callId), [.accept(callId: Self.callId)])
        XCTAssertEqual(ledger.engine(invite: nil, activeCallId: Self.callId), [])
        XCTAssertEqual(ledger.timedOut(Self.callId), [])
        XCTAssertEqual(ledger.engine(invite: nil, activeCallId: ""), [.end(id: Self.callId, reason: .remoteEnded)])
        XCTAssertFalse(ledger.ringing(Self.callId))
    }

    @MainActor func testAnAnswerBeforeTheInviteArrivesAcceptsItWhenItDoes() async {
        let ledger = DirectCallKitLedger()
        _ = ledger.push(push())
        XCTAssertEqual(ledger.answered(Self.callId), [])
        XCTAssertEqual(
            ledger.engine(invite: invite(media: .audio), activeCallId: ""),
            [.update(id: Self.callId, caller: "bob", video: false), .accept(callId: Self.callId)])
    }

    @MainActor func testADeclineBeforeTheInviteArrivesDeclinesItAndARepeatPushEndsAtOnce() async {
        let ledger = DirectCallKitLedger()
        _ = ledger.push(push())
        XCTAssertEqual(ledger.endRequested(Self.callId), [])
        XCTAssertFalse(ledger.ringing(Self.callId))
        XCTAssertEqual(ledger.engine(invite: invite(), activeCallId: ""), [.decline(callId: Self.callId)])
        XCTAssertEqual(ledger.engine(invite: invite(), activeCallId: ""), [])
        XCTAssertEqual(
            ledger.push(push()),
            [.report(id: Self.callId, caller: "bob", video: false), .end(id: Self.callId, reason: .remoteEnded)])
    }

    @MainActor func testAPushWithNoInviteEndsAsUnansweredWhenTheWaitRunsOut() async {
        let ledger = DirectCallKitLedger()
        _ = ledger.push(push())
        XCTAssertEqual(ledger.timedOut(Self.callId), [.end(id: Self.callId, reason: .unanswered)])
        XCTAssertEqual(ledger.timedOut(Self.callId), [])
        XCTAssertEqual(ledger.engine(invite: invite(), activeCallId: ""), [])
    }

    @MainActor func testARepeatPushForARingingCallOnlyReportsItAgain() async {
        let ledger = DirectCallKitLedger()
        _ = ledger.push(push())
        XCTAssertEqual(ledger.push(push()), [.report(id: Self.callId, caller: "bob", video: false)])
        XCTAssertEqual(ledger.push(push("")), [.report(id: Self.callId, caller: "bob", video: false)])
    }

    @MainActor func testASocketInviteWithNoPushIsReportedAndEndsWhenTheCallerHangsUp() async {
        let ledger = DirectCallKitLedger()
        XCTAssertEqual(
            ledger.engine(invite: invite(), activeCallId: ""), [.report(id: Self.callId, caller: "bob", video: true)])
        XCTAssertTrue(ledger.ringing(conversation: Self.conversation))
        XCTAssertEqual(ledger.engine(invite: nil, activeCallId: ""), [.end(id: Self.callId, reason: .remoteEnded)])
        XCTAssertEqual(ledger.engine(invite: invite(), activeCallId: ""), [])
    }

    @MainActor func testAnswerInTheAppTellsCallKitAndEndingFromCallKitHangsUp() async {
        let ledger = DirectCallKitLedger()
        _ = ledger.engine(invite: invite(), activeCallId: "")
        XCTAssertEqual(ledger.engine(invite: nil, activeCallId: Self.callId), [.answerInApp(id: Self.callId)])
        XCTAssertEqual(ledger.answered(Self.callId), [])
        XCTAssertEqual(ledger.endRequested(Self.callId), [.hangUp])
        XCTAssertEqual(ledger.engine(invite: nil, activeCallId: ""), [])
    }

    @MainActor func testAPushWithoutACallIdMatchesTheInviteByConversation() async {
        let ledger = DirectCallKitLedger(makeId: { "11111111-2222-4333-8444-555555555555" })
        let id = "11111111-2222-4333-8444-555555555555"
        XCTAssertEqual(ledger.push(push("")).first, .report(id: id, caller: "bob", video: false))
        XCTAssertEqual(ledger.engine(invite: invite(), activeCallId: ""), [.update(id: id, caller: "bob", video: true)])
        XCTAssertTrue(ledger.ringing(Self.callId))
        XCTAssertEqual(ledger.endRequested(id), [.decline(callId: Self.callId)])
    }

    @MainActor func testAnOutgoingCallIsNeverTracked() async {
        let ledger = DirectCallKitLedger()
        XCTAssertEqual(ledger.engine(invite: nil, activeCallId: Self.callId), [])
        XCTAssertEqual(ledger.engine(invite: nil, activeCallId: ""), [])
    }

    func testCallKitIsOffForTheChinaStorefrontOrRegion() {
        XCTAssertTrue(DirectCallKitRegion.allowed(storefront: "USA", region: "US"))
        XCTAssertTrue(DirectCallKitRegion.allowed(storefront: nil, region: nil))
        XCTAssertTrue(DirectCallKitRegion.allowed(storefront: "HKG", region: "HK"))
        XCTAssertFalse(DirectCallKitRegion.allowed(storefront: "CHN", region: "US"))
        XCTAssertFalse(DirectCallKitRegion.allowed(storefront: " chn ", region: nil))
        XCTAssertFalse(DirectCallKitRegion.allowed(storefront: "USA", region: "CN"))
        XCTAssertFalse(DirectCallKitRegion.allowed(storefront: nil, region: "cn"))
    }

    @MainActor func testTurningCallKitOffEndsEveryCallItShowsWithoutHangingUp() async {
        let ledger = DirectCallKitLedger()
        _ = ledger.push(push())
        XCTAssertEqual(ledger.endAll(), [.end(id: Self.callId, reason: .failed)])
        XCTAssertFalse(ledger.ringing(Self.callId))
        XCTAssertEqual(ledger.endAll(), [])
        XCTAssertEqual(ledger.engine(invite: invite(), activeCallId: ""), [])
    }

    @MainActor func testAResetHangsUpALiveCallAndForgetsTheRest() async {
        let ledger = DirectCallKitLedger()
        _ = ledger.engine(invite: invite(), activeCallId: "")
        _ = ledger.answered(Self.callId)
        _ = ledger.engine(invite: nil, activeCallId: Self.callId)
        XCTAssertEqual(ledger.reset(), [.hangUp])
        XCTAssertFalse(ledger.ringing(Self.callId))
    }
}
