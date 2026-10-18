import FMPackages
import XCTest

final class DjPaceTests: XCTestCase {
    func testASteadyRecordAndTheWaveformDoNotHoldAProMotionScreenAt120Hz() {
        let pace = DjPace(displayFps: 120)
        XCTAssertEqual(pace.steady, DjPace.Range(minimum: 30, maximum: 60, preferred: 60))
        XCTAssertEqual(pace.motionFps, 60)
        XCTAssertEqual(pace.touched, DjPace.Range(minimum: 60, maximum: 120, preferred: 120))
        XCTAssertEqual(pace.peopleFps, 24)
        XCTAssertFalse(pace.strained)
    }

    func testAHotPhoneOrLowPowerModeEasesEveryContinuousAnimation() {
        for pace in [DjPace(hot: true, displayFps: 120), DjPace(lowPower: true, displayFps: 120)] {
            XCTAssertTrue(pace.strained)
            XCTAssertEqual(pace.steady, DjPace.Range(minimum: 15, maximum: 30, preferred: 30))
            XCTAssertEqual(pace.motionFps, 30)
            XCTAssertEqual(pace.touched, DjPace.Range(minimum: 60, maximum: 60, preferred: 60))
            XCTAssertEqual(pace.peopleFps, 12)
        }
        XCTAssertTrue(DjPace(hot: true).label.hasSuffix("(eased: hot)"), DjPace(hot: true).label)
    }

    func testA60HzScreenIsNeverAskedForMore() {
        let pace = DjPace(displayFps: 60)
        XCTAssertEqual(pace.touched.maximum, 60)
        XCTAssertEqual(pace.steady.maximum, 60)
        XCTAssertEqual(DjPace(displayFps: 0).motionFps, 30)
    }

    func testIntervalsLeaveRoomForFrameJitter() {
        let pace = DjPace(displayFps: 120)
        XCTAssertLessThan(pace.motionInterval, 1.0 / 60)
        XCTAssertGreaterThan(pace.motionInterval, 1.0 / 120)
        XCTAssertLessThan(DjPace(hot: true).motionInterval, 1.0 / 30)
        XCTAssertGreaterThan(DjPace(hot: true).motionInterval, 1.0 / 60)
        XCTAssertGreaterThan(pace.peopleInterval, 1.0 / 30)
    }
}
