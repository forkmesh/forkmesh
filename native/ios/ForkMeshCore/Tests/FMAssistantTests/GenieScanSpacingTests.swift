import Foundation
import XCTest
@testable import FMAssistant

final class GenieScanSpacingTests: XCTestCase {
    func testACheapScanKeepsTheTenSecondRefresh() {
        XCTAssertEqual(GenieLandmarks.refresh, 10)
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0), GenieLandmarks.refresh)
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0.05), GenieLandmarks.refresh)
    }

    func testASlowScanWaitsLongEnoughToStayUnderOnePercentOfTheMainThread() {
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0.2), 20, accuracy: 1e-9)
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 0.4), 40, accuracy: 1e-9)
    }

    func testTheWaitIsCappedSoTheSpiderStillSeesTheScreen() {
        XCTAssertEqual(GenieLandmarks.spacing(afterScanTaking: 5), GenieLandmarks.slowest)
    }

    func testTheFirstReadScans() {
        XCTAssertTrue(GenieScanSchedule().due(at: 0))
    }

    func testTheKeepClearTickReusesTheLastScan() {
        var schedule = GenieScanSchedule()
        schedule.scanned(at: 100, taking: 0.03)
        for tick in stride(from: 101.5, to: 110, by: 1.5) {
            XCTAssertFalse(schedule.due(at: tick), "rescanned at \(tick)")
        }
        XCTAssertTrue(schedule.due(at: 110))
    }

    func testAPageChangeRescansAtOnce() {
        var schedule = GenieScanSchedule()
        schedule.scanned(at: 100, taking: 0.03)
        schedule.invalidate()
        XCTAssertTrue(schedule.due(at: 100.1))
        schedule.scanned(at: 100.1, taking: 0.03)
        XCTAssertFalse(schedule.due(at: 101.6))
    }

    func testASlowScanPushesTheNextOneBack() {
        var schedule = GenieScanSchedule()
        schedule.scanned(at: 100, taking: 0.3)
        XCTAssertEqual(schedule.spacing, 30, accuracy: 1e-9)
        XCTAssertFalse(schedule.due(at: 129))
        XCTAssertTrue(schedule.due(at: 130))
    }
}
