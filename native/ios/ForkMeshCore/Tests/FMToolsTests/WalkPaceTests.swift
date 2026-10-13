import XCTest
@testable import FMTools

final class WalkPaceTests: XCTestCase {
    let t0 = Date(timeIntervalSince1970: 1_789_561_860)

    private func at(_ offset: TimeInterval) -> Date { t0.addingTimeInterval(offset) }

    func testReportedCadenceMeansWalking() {
        var pace = WalkPace()
        XCTAssertFalse(pace.moving)
        pace.record(steps: 10, cadence: 1.8, at: at(0))
        XCTAssertTrue(pace.moving)
        XCTAssertEqual(pace.stepsPerMinute, 108)
        XCTAssertFalse(pace.running)
    }

    func testStepDeltaFillsAMissingCadence() {
        var pace = WalkPace()
        pace.record(steps: 100, cadence: 0, at: at(0))
        XCTAssertFalse(pace.moving)
        pace.record(steps: 108, cadence: 0, at: at(4))
        XCTAssertEqual(pace.cadence, 2, accuracy: 0.001)
        pace.record(steps: 150, cadence: 0, at: at(4 + WalkPace.staleAfter * 3))
        XCTAssertFalse(pace.moving)
    }

    func testShufflingIsNotWalking() {
        var pace = WalkPace()
        pace.record(steps: 100, cadence: 0, at: at(0))
        pace.record(steps: 102, cadence: 0, at: at(8))
        XCTAssertFalse(pace.moving)
        pace.record(steps: 110, cadence: 0.3, at: at(9))
        XCTAssertFalse(pace.moving)
    }

    func testFastCadenceIsRunningAndCapped() {
        var pace = WalkPace()
        pace.record(steps: 10, cadence: 2.7, at: at(0))
        XCTAssertTrue(pace.running)
        pace.record(steps: 20, cadence: 9, at: at(2))
        XCTAssertEqual(pace.cadence, WalkPace.maxCadence)
    }

    func testStopsWhenUpdatesGoQuiet() {
        var pace = WalkPace()
        pace.record(steps: 10, cadence: 1.8, at: at(0))
        XCTAssertEqual(pace.expiresAt, at(WalkPace.staleAfter))
        XCTAssertFalse(pace.expire(at(WalkPace.staleAfter - 1)))
        XCTAssertTrue(pace.moving)
        XCTAssertTrue(pace.expire(at(WalkPace.staleAfter)))
        XCTAssertFalse(pace.moving)
        XCTAssertNil(pace.expiresAt)
    }
}
