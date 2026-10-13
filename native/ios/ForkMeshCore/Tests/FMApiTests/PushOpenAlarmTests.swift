@testable import FMApi
import XCTest

final class PushOpenAlarmTests: XCTestCase {
    func testADesktopPowerPushIsAnAlarm() {
        let open = PushOpen(arguments: [
            "alertId": "p1", "title": "tower lost power", "body": "Running on battery.", "alarm": "power",
        ])
        XCTAssertTrue(open.isAlarm)
        XCTAssertEqual(open.alarm, "power")
        XCTAssertEqual(open.ping?.title, "tower lost power")
    }

    func testAnUnknownOrMissingAlarmIsAnOrdinaryPing() {
        XCTAssertFalse(PushOpen(arguments: ["alertId": "p2", "title": "Agent #1"]).isAlarm)
        XCTAssertFalse(PushOpen(arguments: ["alertId": "p3", "title": "x", "alarm": "fire"]).isAlarm)
        XCTAssertFalse(PushOpen(alarm: " ").isAlarm)
    }
}
