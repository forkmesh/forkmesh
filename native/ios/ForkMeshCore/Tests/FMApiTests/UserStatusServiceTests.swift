// UserStatusService and AgentListSignal, over `StatusServer`, a fake of
// POST /api/accounts/user-status: session follow, optimistic away with rollback, busy flag,
// lapse timer, stale re-read.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// Remembers every body and answers like the Worker,
/// or with `refuse` as a 400 error code when set.
private final class StatusServer: @unchecked Sendable {
    let bodies = SvcBox<[JSONObject]>([])
    let current = SvcBox(UserStatus())
    let refuse = SvcBox("")
    /// Holds each answer until the test opens it.
    let gate = SvcBox<SvcGate?>(nil)

    lazy var http = FakeHTTPClient { request in await self.handle(request) }

    private func handle(_ request: HTTPRequest) async -> HTTPResponse {
        guard request.method == "POST", request.url.path == "/api/accounts/user-status" else {
            return HTTPResponse(status: 404, text: "not found")
        }
        let body = JSON.parseObject(request.bodyText)
        bodies.mutate { $0.append(body) }
        if let gate = gate.value { await gate.wait() }
        let refusal = refuse.value
        if !refusal.isEmpty { return HTTPResponse(status: 400, json: ["error": .string(refusal)]) }
        current.mutate { status in
            if let away = body["away"]?.boolValue { status = status.copyWith(away: away) }
            if body["clear"] == true {
                status = UserStatus(away: status.away)
            } else if body.contains("emoji") {
                status = status.copyWith(
                    emoji: body.str("emoji"), text: body.str("text"), expiresAt: body.int("expiresAt", 0))
            }
        }
        return HTTPResponse(status: 200, json: ["ok": true, "nodeName": "alice", "userStatus": .object(current.value.toJSON())])
    }
}

/// RelayService.setOwnStatus: records every announcement.
@MainActor
private final class RecordingAnnouncer: OwnStatusAnnouncer {
    var announced: [UserStatus?] = []
    var ownStatus: UserStatus? { announced.last ?? nil }

    func setOwnStatus(_ status: UserStatus?) { announced.append(status) }
}

@MainActor
final class UserStatusServiceTests: XCTestCase {
    private struct Rig {
        let server: StatusServer
        let relay: RecordingAnnouncer
        let token: SvcBox<String>
        let clock: FixedClock
        let sleeper: SvcSleeper
        let service: UserStatusService
    }

    private func rig(signedIn: Bool = true) -> Rig {
        let server = StatusServer()
        let relay = RecordingAnnouncer()
        let token = SvcBox(signedIn ? "session-token" : "")
        let clock = FixedClock(epochMillis: svcEpochMillis)
        let sleeper = SvcSleeper()
        let api = svcApi(server.http, token: { token.value }, clock: clock)
        let service = UserStatusService(
            api: api, relay: relay, sessionToken: { token.value }, clock: clock, sleep: sleeper.sleep)
        return Rig(server: server, relay: relay, token: token, clock: clock, sleeper: sleeper, service: service)
    }

    func testASignInReadsTheStatusAndASignOutForgetsIt() async {
        let rig = rig(signedIn: false)
        rig.server.current.value = UserStatus(away: true, emoji: "🌴", text: "Vacationing")
        XCTAssertFalse(rig.service.known)
        XCTAssertFalse(rig.service.signedIn)

        // Signed out from the start: nothing to read.
        await rig.service.syncSession()
        XCTAssertTrue(rig.server.bodies.value.isEmpty)

        rig.token.value = "session-token"
        await rig.service.syncSession()
        // The read is the same endpoint with no changes.
        XCTAssertEqual(rig.server.bodies.value, [[:]])
        XCTAssertTrue(rig.service.known)
        XCTAssertEqual(rig.service.status, UserStatus(away: true, emoji: "🌴", text: "Vacationing"))
        XCTAssertEqual(rig.relay.ownStatus, UserStatus(away: true, emoji: "🌴", text: "Vacationing"))

        // The same session again reads nothing.
        await rig.service.syncSession()
        XCTAssertEqual(rig.server.bodies.value.count, 1)

        rig.token.value = ""
        await rig.service.syncSession()
        XCTAssertFalse(rig.service.known)
        XCTAssertEqual(rig.service.status, .empty)
        // Stops announcing it on the relay.
        XCTAssertEqual(rig.relay.announced.count, 2)
        XCTAssertNil(rig.relay.announced.last!)
        XCTAssertEqual(rig.server.bodies.value.count, 1)
    }

    func testAFailedReadOnSignInIsSwallowed() async {
        let rig = rig()
        rig.server.refuse.value = "server_error"
        await rig.service.syncSession()
        XCTAssertFalse(rig.service.known)
        XCTAssertEqual(rig.server.bodies.value.count, 1)
    }

    func testASignOutRacingTheReadDropsItsAnswer() async {
        let rig = rig()
        let gate = SvcGate()
        rig.server.gate.value = gate
        let read = Task { try await rig.service.refresh() }
        let held = await svcEventually { gate.arrived == 1 }
        XCTAssertTrue(held)
        rig.token.value = ""
        gate.release()
        _ = try? await read.value
        XCTAssertFalse(rig.service.known)
        XCTAssertTrue(rig.relay.announced.isEmpty)
    }

    func testSetAwayAppliesAtOnceAndSticksWhenAccepted() async throws {
        let rig = rig()
        await rig.service.syncSession()
        let gate = SvcGate()
        rig.server.gate.value = gate
        let write = Task { try await rig.service.setAway(true) }
        let held = await svcEventually { gate.arrived == 1 }
        XCTAssertTrue(held)
        // Optimistic: painted and announced before the server answers.
        XCTAssertTrue(rig.service.status.away)
        XCTAssertEqual(rig.relay.ownStatus?.away, true)
        XCTAssertTrue(rig.service.busy)
        gate.release()
        try await write.value

        XCTAssertEqual(rig.server.bodies.value.last, ["away": true])
        XCTAssertTrue(rig.service.status.away)
        XCTAssertFalse(rig.service.busy)
        XCTAssertEqual(rig.relay.ownStatus, UserStatus(away: true))
    }

    func testSetAwayRollsBackWhenTheServerRefuses() async {
        let rig = rig()
        rig.server.current.value = UserStatus(emoji: "🚌", text: "Commuting")
        await rig.service.syncSession()
        rig.server.refuse.value = "rate_limited"

        let error = await svcThrown { try await rig.service.setAway(true) }
        XCTAssertEqual(error as? UserStatusException, UserStatusException("rate_limited", "Could not update your status. Please try again."))
        XCTAssertFalse(rig.service.status.away)
        XCTAssertEqual(rig.service.status, UserStatus(emoji: "🚌", text: "Commuting"))
        XCTAssertFalse(rig.service.busy)
        // Announced away, then the rollback.
        XCTAssertEqual(rig.relay.announced.suffix(2).map { $0?.away }, [true, false])
        XCTAssertEqual(rig.relay.ownStatus, UserStatus(emoji: "🚌", text: "Commuting"))
    }

    func testSetCustomTrimsAndClearCustomKeepsTheAwayFlag() async throws {
        let rig = rig()
        await rig.service.syncSession()
        try await rig.service.setAway(true)
        try await rig.service.setCustom(emoji: " 🚌 ", text: "  Commuting ", expiresAt: 0)
        XCTAssertEqual(rig.server.bodies.value.last, ["emoji": "🚌", "text": "Commuting", "expiresAt": 0])
        XCTAssertEqual(JSON.encode(.object(rig.server.bodies.value.last!)), "{\"emoji\":\"🚌\",\"text\":\"Commuting\",\"expiresAt\":0}")
        XCTAssertEqual(rig.service.status, UserStatus(away: true, emoji: "🚌", text: "Commuting"))
        XCTAssertEqual(rig.relay.ownStatus, UserStatus(away: true, emoji: "🚌", text: "Commuting"))

        try await rig.service.clearCustom()
        XCTAssertEqual(rig.server.bodies.value.last, ["clear": true])
        XCTAssertEqual(rig.service.status, UserStatus(away: true))
    }

    func testARefusedCustomStatusKeepsTheOldOneAndSaysWhy() async {
        let rig = rig()
        await rig.service.syncSession()
        rig.server.refuse.value = "status_text_too_long"
        let error = await svcThrown { try await rig.service.setCustom(emoji: "", text: "x", expiresAt: 0) }
        XCTAssertEqual((error as? UserStatusException)?.message, "Keep your status under 100 characters.")
        XCTAssertEqual(rig.service.status, .empty)
        XCTAssertFalse(rig.service.busy)
    }

    func testBusyIsRaisedWhileAWriteIsInFlight() async throws {
        let rig = rig()
        await rig.service.syncSession()
        let busySeen = SvcBox<[Bool]>([])
        let service = rig.service
        service.addListener {
            let busy = MainActor.assumeIsolated { service.busy }
            busySeen.mutate { $0.append(busy) }
        }
        let gate = SvcGate()
        rig.server.gate.value = gate
        let write = Task { try await service.clearCustom() }
        let held = await svcEventually { gate.arrived == 1 }
        XCTAssertTrue(held)
        XCTAssertTrue(service.busy)
        gate.release()
        try await write.value
        XCTAssertFalse(service.busy)
        XCTAssertEqual(busySeen.value.first, true)
        XCTAssertEqual(busySeen.value.last, false)
    }

    func testALapsingCustomStatusIsDroppedWhenItsTimeComes() async throws {
        let rig = rig()
        await rig.service.syncSession()
        let expiresAt = Int(svcEpochMillis) + 60_000
        try await rig.service.setCustom(emoji: "🍔", text: "Lunch", expiresAt: expiresAt)
        XCTAssertEqual(rig.service.status, UserStatus(emoji: "🍔", text: "Lunch", expiresAt: expiresAt))
        // One timer, for the clear-after moment.
        let armed = await svcEventually { rig.sleeper.pending == [60] }
        XCTAssertTrue(armed, "\(rig.sleeper.pending)")

        let revision = rig.service.revision
        rig.clock.advance(by: 60)
        // Painted as lapsed straight away...
        XCTAssertEqual(rig.service.status, .empty)
        // ...and dropped (and announced) when the timer fires.
        rig.sleeper.fire(60)
        let dropped = await svcEventually { rig.relay.ownStatus == UserStatus() }
        XCTAssertTrue(dropped, "\(String(describing: rig.relay.ownStatus))")
        XCTAssertGreaterThan(rig.service.revision, revision)
        XCTAssertTrue(rig.sleeper.pending.isEmpty)
    }

    func testANewStatusReplacesTheLapseTimerAndDisposeStopsIt() async throws {
        let rig = rig()
        await rig.service.syncSession()
        try await rig.service.setCustom(emoji: "🍔", text: "Lunch", expiresAt: Int(svcEpochMillis) + 60_000)
        let armed = await svcEventually { rig.sleeper.pending == [60] }
        XCTAssertTrue(armed, "\(rig.sleeper.pending)")
        try await rig.service.setCustom(emoji: "📅", text: "Meeting", expiresAt: Int(svcEpochMillis) + 3_600_000)
        let replaced = await svcEventually { rig.sleeper.pending == [3_600] }
        XCTAssertTrue(replaced, "\(rig.sleeper.pending)")

        rig.service.dispose()
        let stopped = await svcEventually { rig.sleeper.pending.isEmpty }
        XCTAssertTrue(stopped)
    }

    func testAStatusThatLapsedBeforeItWasReadIsNeverShown() async {
        let rig = rig()
        rig.server.current.value = UserStatus(away: true, emoji: "🍔", text: "Lunch", expiresAt: Int(svcEpochMillis) - 1)
        await rig.service.syncSession()
        XCTAssertEqual(rig.service.status, UserStatus(away: true))
        XCTAssertEqual(rig.relay.ownStatus, UserStatus(away: true))
        await svcSettle()
        XCTAssertTrue(rig.sleeper.requested.isEmpty)
    }

    func testTheResumeReadWaitsTenMinutes() async throws {
        let rig = rig()
        // Never read: the first resume reads.
        try await rig.service.refreshIfStale()
        XCTAssertEqual(rig.server.bodies.value.count, 1)
        rig.clock.advance(by: 5 * 60)
        try await rig.service.refreshIfStale()
        XCTAssertEqual(rig.server.bodies.value.count, 1)
        rig.clock.advance(by: 5 * 60)
        try await rig.service.refreshIfStale()
        XCTAssertEqual(rig.server.bodies.value.count, 2)
        try await rig.service.refreshIfStale(maxAge: 0)
        XCTAssertEqual(rig.server.bodies.value.count, 3)
    }

    func testSignedOutRefreshAsksNothing() async throws {
        let rig = rig(signedIn: false)
        try await rig.service.refresh()
        XCTAssertTrue(rig.server.bodies.value.isEmpty)
        XCTAssertFalse(rig.service.known)
    }

    // MARK: AgentListSignal

    func testAgentListSignalRingsItsListeners() async {
        let signal = AgentListSignal()
        let heard = SvcBox(0)
        let token = signal.addListener { heard.mutate { $0 += 1 } }
        XCTAssertEqual(signal.rings, 0)
        signal.ring()
        signal.ring()
        XCTAssertEqual(signal.rings, 2)
        XCTAssertEqual(heard.value, 2)
        signal.removeListener(token)
        signal.ring()
        XCTAssertEqual(signal.rings, 3)
        XCTAssertEqual(heard.value, 2)
    }
}
