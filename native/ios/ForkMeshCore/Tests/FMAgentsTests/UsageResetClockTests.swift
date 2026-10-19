import FMBase
import FMControl
import Foundation
import XCTest

@testable import FMAgents

final class UsageResetClockTests: XCTestCase {
    private let utc = TimeZone(identifier: "UTC")!
    private let now: Int64 = 1_792_404_300_000

    private func at(days: Int64, hour: Int64, minute: Int64) -> Int64 {
        let midnight = now - now % 86_400_000
        return midnight + days * 86_400_000 + hour * 3_600_000 + minute * 60_000
    }

    func testTodayShowsTwelveHourClock() {
        XCTAssertEqual(UsageFormat.resetClock(atMs: at(days: 0, hour: 15, minute: 45), nowMs: now, timeZone: utc), "Resets 3:45 PM")
        XCTAssertEqual(UsageFormat.resetClock(atMs: at(days: 0, hour: 23, minute: 59), nowMs: now, timeZone: utc), "Resets 11:59 PM")
    }

    func testLaterThisWeekNamesTheWeekday() {
        XCTAssertEqual(UsageFormat.resetClock(atMs: at(days: 1, hour: 0, minute: 0), nowMs: now, timeZone: utc), "Resets Tue 12:00 AM")
        XCTAssertEqual(UsageFormat.resetClock(atMs: at(days: 1, hour: 9, minute: 0), nowMs: now, timeZone: utc), "Resets Tue 9:00 AM")
        XCTAssertEqual(UsageFormat.resetClock(atMs: at(days: 6, hour: 12, minute: 30), nowMs: now, timeZone: utc), "Resets Sun 12:30 PM")
    }

    func testAWeekOrMoreNamesTheDate() {
        XCTAssertEqual(UsageFormat.resetClock(atMs: at(days: 7, hour: 10, minute: 0), nowMs: now, timeZone: utc), "Resets Oct 26, 10:00 AM")
    }

    func testNoUpcomingResetIsEmpty() {
        XCTAssertEqual(UsageFormat.resetClock(atMs: at(days: 0, hour: 9, minute: 0), nowMs: now, timeZone: utc), "")
        XCTAssertEqual(UsageFormat.resetClock(atMs: 0, nowMs: now, timeZone: utc), "")
        let window = UsageWindowMeter(id: "5h", label: "5-hour", usedPercent: 10, state: .ok)
        XCTAssertEqual(window.resetClockLabel(nowMs: now, timeZone: utc), "")
    }

    func testFollowsTheViewersTimeZone() {
        let kolkata = TimeZone(identifier: "Asia/Kolkata")!
        let evening = at(days: 0, hour: 17, minute: 30)
        XCTAssertEqual(UsageFormat.resetClock(atMs: evening + 2 * 3_600_000, nowMs: evening, timeZone: kolkata), "Resets Tue 1:00 AM")
    }

    @MainActor func testRefreshAsksTheDesktopToReadAgain() async throws {
        let channel = FakeAgentOpsChannel()
        let client = AgentOpsClient(channel: channel)
        _ = try await client.usage()
        XCTAssertNil(channel.params("agentops.usage")?["refresh"])
        _ = try await client.usage(refresh: true)
        XCTAssertEqual(channel.params("agentops.usage")?.bool("refresh"), true)
    }
}
