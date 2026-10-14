import Foundation
import XCTest
@testable import FMAssistant

final class GenieScanSpacingTests: XCTestCase {
    func testACheapScanKeepsTheTwoSecondRefresh() {
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0), GenieLandmarks.refresh)
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0.01), GenieLandmarks.refresh)
    }

    func testASlowScanWaitsLongEnoughToStayUnderTwoPercentOfTheMainThread() {
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0.2), 10, accuracy: 1e-9)
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0.4), 20, accuracy: 1e-9)
    }

    func testTheWaitIsCappedSoTheSpiderStillSeesTheScreen() {
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 5), GenieLandmarks.slowest)
    }
}
