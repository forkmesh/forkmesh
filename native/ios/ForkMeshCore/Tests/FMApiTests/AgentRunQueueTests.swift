import FMBase
import Foundation
import XCTest
@testable import FMApi

final class AgentRunQueueTests: XCTestCase {
    private let now = 1_000_000

    private func queue(_ runner: JSONObject) -> AgentRunQueue? {
        AgentRunQueue(
            json: [
                "jobId": 3, "position": 2, "ahead": 1, "waiting": 3, "running": 1, "repoBusy": false,
                "queuedAt": JSONValue(now - 125_000), "now": JSONValue(now), "checkEveryMs": 20_000, "leaseTimeoutMs": 7_200_000,
                "runner": .object(runner),
            ], receivedAtMs: now)
    }

    func testTheSessionCarriesItsPlaceInTheQueue() {
        let session = AgentLaunchSession(json: [
            "id": "s1", "status": "queued", "runner": "fmnext-r1",
            "queue": ["position": 1, "waiting": 1, "now": JSONValue(now), "runner": ["name": "fmnext-r1"]],
        ])
        XCTAssertEqual(session.queue?.position, 1)
        XCTAssertEqual(session.queue?.runner?.name, "fmnext-r1")
        XCTAssertNil(AgentLaunchSession(json: ["id": "s1", "status": "running"]).queue)
    }

    func testABusyRunnerCountsDownToItsLeaseTimeout() throws {
        let read = try XCTUnwrap(queue([
            "name": "fmnext-r1", "online": true, "busy": true, "activeJobs": 1,
            "busySince": JSONValue(now - 600_000), "freeBy": JSONValue(now + 3_725_000),
        ]))
        XCTAssertEqual(read.placeLine, "#2 in queue · 2 others waiting · 1 running")
        XCTAssertEqual(read.countdownLabel(now), "Slot frees by lease timeout in")
        XCTAssertEqual(AgentRunQueue.clock(read.countdownMs(now)), "1h 02m 05s")
        XCTAssertEqual(
            read.runnerLine(now), "fmnext-r1 is busy with a job for 10m 00s. It takes this one as soon as a slot frees.")
        XCTAssertEqual(read.waitedLine(now), "Queued 2m 05s ago")
    }

    func testAnIdleRunnerCountsDownToItsNextClaimAndRollsOver() throws {
        let read = try XCTUnwrap(queue([
            "name": "fmnext-r1", "online": true, "busy": false, "lastSeenAt": JSONValue(now - 5_000), "nextCheckAt": JSONValue(now + 15_000),
        ]))
        XCTAssertEqual(read.countdownLabel(now), "Next claim check in")
        XCTAssertEqual(read.countdownMs(now), 15_000)
        XCTAssertEqual(read.countdownMs(now + 16_000), 19_000)
        XCTAssertEqual(read.refreshAfterMs(now), 16_000)
    }

    func testAnOfflineRunnerSaysWhenItLastCheckedIn() throws {
        let read = try XCTUnwrap(queue(["name": "fmnext-r1", "online": false, "lastSeenAt": JSONValue(now - 300_000)]))
        XCTAssertEqual(read.countdownLabel(now), "")
        XCTAssertEqual(read.runnerLine(now), "fmnext-r1 last checked in 5m 00s ago. It may be offline.")
        XCTAssertEqual(read.refreshAfterMs(now), 15_000)
    }

    func testTheServerClockWinsOverTheDevicesClock() throws {
        let read = try XCTUnwrap(AgentRunQueue(json: ["position": 1, "queuedAt": JSONValue(now - 10_000), "now": JSONValue(now)], receivedAtMs: now - 60_000))
        XCTAssertEqual(read.waitedLine(now - 60_000), "Queued 10s ago")
        XCTAssertEqual(read.placeLine, "#1 in queue · no one else waiting")
    }
}
