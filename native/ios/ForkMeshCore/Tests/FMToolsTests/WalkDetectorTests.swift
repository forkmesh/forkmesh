import XCTest
@testable import FMTools

final class WalkDetectorTests: XCTestCase {
    let t0 = Date(timeIntervalSince1970: 1_789_561_860)

    private func sample(_ motion: WalkMotion, _ offset: TimeInterval, confident: Bool = true) -> WalkMotionSample {
        WalkMotionSample(motion: motion, confident: confident, date: t0.addingTimeInterval(offset))
    }

    private func at(_ offset: TimeInterval) -> Date { t0.addingTimeInterval(offset) }

    func testStartsAfterSustainedWalking() {
        var detector = WalkDetector()
        XCTAssertNil(detector.observe(sample(.walking, 0), now: at(0)))
        XCTAssertEqual(detector.nextCheck, at(WalkDetector.startAfter))
        XCTAssertNil(detector.tick(at(WalkDetector.startAfter - 1)))
        XCTAssertEqual(detector.tick(at(WalkDetector.startAfter)), .started(t0))
        XCTAssertTrue(detector.walking)
        XCTAssertNil(detector.nextCheck)
    }

    func testIgnoresLowConfidenceAndUnknown() {
        var detector = WalkDetector()
        XCTAssertNil(detector.observe(sample(.walking, 0, confident: false), now: at(0)))
        XCTAssertNil(detector.observe(sample(.unknown, 5), now: at(5)))
        XCTAssertNil(detector.tick(at(600)))
        XCTAssertFalse(detector.walking)
        XCTAssertNil(detector.nextCheck)
    }

    func testShortWalkNeverStarts() {
        var detector = WalkDetector()
        _ = detector.observe(sample(.walking, 0), now: at(0))
        XCTAssertNil(detector.observe(sample(.still, 10), now: at(10)))
        XCTAssertNil(detector.tick(at(600)))
        XCTAssertFalse(detector.walking)
    }

    func testRunningCountsAsAWalk() {
        var detector = WalkDetector()
        XCTAssertEqual(detector.observe(sample(.running, 0), now: at(60)), .started(t0))
    }

    func testCatchesUpOnAWalkThatBeganWhileSuspended() {
        var detector = WalkDetector()
        XCTAssertEqual(detector.observe(sample(.walking, -1200), now: at(0)), .started(at(-1200)))
    }

    func testForgetPendingStartKeepsAWalkInProgress() {
        var pending = WalkDetector()
        _ = pending.observe(sample(.walking, 0), now: at(10))
        pending.forgetPendingStart()
        XCTAssertNil(pending.nextCheck)
        XCTAssertNil(pending.tick(at(3600)))
        XCTAssertFalse(pending.walking)

        var walking = WalkDetector()
        _ = walking.observe(sample(.walking, 0), now: at(60))
        _ = walking.observe(sample(.still, 300), now: at(300))
        walking.forgetPendingStart()
        XCTAssertTrue(walking.walking)
        XCTAssertEqual(walking.tick(at(300 + WalkDetector.endAfter)), .ended(at(300)))
    }

    func testBriefStopKeepsTheWalk() {
        var detector = WalkDetector()
        _ = detector.observe(sample(.walking, 0), now: at(60))
        XCTAssertNil(detector.observe(sample(.still, 300), now: at(300)))
        XCTAssertEqual(detector.nextCheck, at(300 + WalkDetector.endAfter))
        XCTAssertNil(detector.observe(sample(.walking, 360), now: at(360)))
        XCTAssertNil(detector.nextCheck)
        XCTAssertNil(detector.tick(at(1000)))
        XCTAssertTrue(detector.walking)
        XCTAssertEqual(detector.walkStartedAt, t0)
    }

    func testEndsAfterStayingStill() {
        var detector = WalkDetector()
        _ = detector.observe(sample(.walking, 0), now: at(60))
        XCTAssertNil(detector.observe(sample(.still, 600), now: at(600)))
        XCTAssertNil(detector.tick(at(600 + WalkDetector.endAfter - 1)))
        XCTAssertEqual(detector.tick(at(600 + WalkDetector.endAfter)), .ended(at(600)))
        XCTAssertEqual(detector, WalkDetector())
    }

    func testStaleStillSampleEndsAtOnceButNotBeforeTheWalk() {
        var detector = WalkDetector()
        _ = detector.observe(sample(.walking, 0), now: at(60))
        XCTAssertEqual(detector.observe(sample(.still, -3600), now: at(3600)), .ended(t0))
    }

    func testReplayEndsAtTheFirstLongStop() {
        var detector = WalkDetector()
        _ = detector.observe(sample(.walking, 0), now: at(60))
        let history = [sample(.walking, 900), sample(.still, 600), sample(.walking, 660), sample(.still, 1200)]
        XCTAssertEqual(detector.replay(history, until: at(1500)), .ended(at(1200)))
        XCTAssertEqual(detector, WalkDetector())
    }

    func testReplayEndsAStopFollowedByAnotherWalk() {
        var detector = WalkDetector()
        _ = detector.observe(sample(.walking, 0), now: at(60))
        let history = [sample(.still, 600), sample(.walking, 600 + WalkDetector.endAfter + 60)]
        XCTAssertEqual(detector.replay(history, until: at(3600)), .ended(at(600)))
    }

    func testReplayKeepsAnOngoingWalk() {
        var detector = WalkDetector()
        _ = detector.observe(sample(.walking, 0), now: at(60))
        XCTAssertNil(detector.replay([sample(.still, 600), sample(.walking, 650)], until: at(3600)))
        XCTAssertTrue(detector.walking)
        XCTAssertNil(detector.nextCheck)
    }

    func testProgressKeepsTheHighestReading() {
        var progress = WalkProgress(startedAt: t0, baseline: 4200)
        progress.record(steps: 120, distanceM: 90)
        progress.record(steps: 100, distanceM: 95)
        XCTAssertEqual(progress.steps, 120)
        XCTAssertEqual(progress.distanceM, 95)
        XCTAssertEqual(progress.todaySteps, 4320)
    }
}
