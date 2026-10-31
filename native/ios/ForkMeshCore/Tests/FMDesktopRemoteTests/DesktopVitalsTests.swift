import FMBase
import FMChat
@testable import FMDesktopRemote
import XCTest

final class DesktopVitalsTests: XCTestCase {
    @MainActor func testWatchLoadsHistoryAndAppendsBells() async {
        let channel = FakeRemoteChannel()
        channel.replies["diag.watch"] = [
            "memory": [40.0, 50.0], "cpu": [10.0, 20.0], "disk": [80.0, 80.0], "swap": [-1.0, -1.0],
        ]
        let vitals = DesktopVitals(channel: channel)
        await vitals.watch()
        XCTAssertTrue(vitals.watching)
        XCTAssertEqual(channel.count("diag.watch"), 1)
        XCTAssertEqual(vitals.values(.memory), [40, 50])
        XCTAssertEqual(vitals.latest(.cpu), 20)
        XCTAssertTrue(vitals.values(.swap).allSatisfy { $0.isNaN })
        XCTAssertNil(vitals.latest(.swap))

        channel.emit("diag.vitals", ["memory": 95.0, "cpu": 30.0, "disk": 81.0, "swap": 5.0])
        XCTAssertEqual(vitals.values(.memory), [40, 50, 95])
        XCTAssertEqual(vitals.latest(.swap), 5)
        channel.emit("diag.changed", ["memory": 1.0])
        XCTAssertEqual(vitals.values(.memory).count, 3)

        for _ in 0..<(DesktopVitals.seconds + 3) {
            channel.emit("diag.vitals", ["memory": 10.0, "cpu": 10.0, "disk": 10.0, "swap": 10.0])
        }
        XCTAssertEqual(vitals.values(.disk).count, DesktopVitals.seconds)
    }

    @MainActor func testTokenTotalsAndAgentReadingsFollowEachSample() async {
        let channel = FakeRemoteChannel()
        channel.replies["diag.watch"] = ["memory": [40.0]]
        let vitals = DesktopVitals(channel: channel)
        let agent = vitals.agentTokens(7)
        await vitals.watch()
        channel.emit("diag.vitals", ["memory": 40.0])
        XCTAssertEqual(vitals.tokenTotals, [])

        channel.emit("diag.vitals", ["memory": 40.0, "tokensIn": 1_000, "tokensOut": 50, "agentTokens": ["7": [900, 40]]])
        XCTAssertEqual(vitals.tokenTotals, [1_000, 50])
        XCTAssertEqual(agent.totals, [900, 40])
        channel.emit("diag.vitals", ["memory": 40.0, "tokensIn": 4_000, "tokensOut": 80, "agentTokens": ["7": [3_900, 70], "x": [1, 2]]])
        XCTAssertEqual(vitals.tokenTotals, [4_000, 80])
        XCTAssertIdentical(vitals.agentTokens(7), agent)
        XCTAssertEqual(agent.totals, [3_900, 70])

        await vitals.unwatch()
        XCTAssertEqual(vitals.tokenTotals, [])
    }

    @MainActor func testOlderDesktopLeavesTheChartsEmpty() async {
        let channel = FakeRemoteChannel()
        channel.failures["diag.watch"] = LocalControlException("unsupported")
        let vitals = DesktopVitals(channel: channel)
        await vitals.watch()
        XCTAssertFalse(vitals.watching)
        XCTAssertTrue(vitals.series.isEmpty)
        channel.emit("diag.vitals", ["memory": 50.0])
        XCTAssertTrue(vitals.values(.memory).isEmpty)
    }

    @MainActor func testUnwatchTellsTheDesktopOnce() async {
        let channel = FakeRemoteChannel()
        channel.replies["diag.watch"] = ["memory": [40.0]]
        let vitals = DesktopVitals(channel: channel)
        await vitals.watch()
        await vitals.unwatch()
        await vitals.unwatch()
        XCTAssertEqual(channel.count("diag.unwatch"), 1)
        XCTAssertTrue(vitals.series.isEmpty)
        channel.emit("diag.vitals", ["memory": 50.0])
        XCTAssertTrue(vitals.values(.memory).isEmpty)
    }

    func testToneJudgesTheWorstReading() {
        XCTAssertNil(DesktopVital.memory.tone([]))
        XCTAssertNil(DesktopVital.swap.tone([.nan]))
        XCTAssertEqual(DesktopVital.cpu.tone([10, 74]), .good)
        XCTAssertEqual(DesktopVital.cpu.tone([10, 75, 20]), .warning)
        XCTAssertEqual(DesktopVital.disk.tone([95, .nan]), .bad)
        XCTAssertEqual(DesktopVital.memory.format(42.4), "42%")
        XCTAssertEqual(DesktopVital.memory.format(nil), "unknown")
    }
}
