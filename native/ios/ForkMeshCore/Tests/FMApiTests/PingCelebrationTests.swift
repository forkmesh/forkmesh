import FMBase
import XCTest
@testable import FMApi

final class PingCelebrationTests: XCTestCase {
    private func ping(_ meta: JSONObject) -> ForkNotification {
        ForkNotification(id: "b", kind: "badge", title: "You earned a badge", meta: meta)
    }

    func testBadgePingsGiveTheGenieALine() {
        XCTAssertEqual(
            PingCelebration.badgeLine(ping(["celebrate": "badge", "badgeName": "Stargazer", "tier": "default"])),
            "You earned Stargazer!")
        XCTAssertEqual(
            PingCelebration.badgeLine(ping(["celebrate": "badge", "badgeName": "Fan Club", "tier": "silver"])),
            "You reached Silver Fan Club!")
        XCTAssertEqual(PingCelebration.badgeLine(ping(["celebrate": "badge"])), "You earned a new badge!")
    }

    func testOtherCelebrationsLeaveTheGenieQuiet() {
        XCTAssertNil(PingCelebration.badgeLine(ping(["celebrate": "crm-email-tracked", "badgeName": "X"])))
        XCTAssertNil(PingCelebration.badgeLine(ping([:])))
    }
}
