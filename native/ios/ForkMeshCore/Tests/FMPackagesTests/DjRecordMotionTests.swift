import FMPackages
import Foundation
import XCTest

final class DjRecordMotionTests: XCTestCase {
    let idle = 1.0 / 15

    func writes(_ actions: [DjRecordMotion.Action]) -> Int {
        actions.filter { $0 != .none }.count
    }

    func testAStoppedRecordIsDrawnOnceNotOnEveryStep() {
        var motion = DjRecordMotion()
        let actions = (0..<150).map { index in motion.step(angle: 1.25, at: Double(index) * idle, steady: nil) }
        XCTAssertEqual(actions.first, .turn(1.25))
        XCTAssertEqual(writes(actions), 1)
    }

    func testAMovedStoppedRecordIsTurnedAgain() {
        var motion = DjRecordMotion()
        _ = motion.step(angle: 0.5, at: 0, steady: nil)
        XCTAssertEqual(motion.step(angle: 0.5, at: idle, steady: nil), .none)
        XCTAssertEqual(motion.step(angle: 0.8, at: 2 * idle, steady: nil), .turn(0.8))
        XCTAssertEqual(motion.step(angle: 0.8 + 2 * Double.pi, at: 3 * idle, steady: nil), .none)
    }

    func testASteadyRecordStartsOneAnimationAndLeavesItRunning() {
        var motion = DjRecordMotion()
        let speed = 2 * Double.pi / DjStudio.secondsPerTurn
        var actions: [DjRecordMotion.Action] = []
        for index in 0..<300 {
            let now = Double(index) * idle
            actions.append(motion.step(angle: 0.3 + speed * now, at: now, steady: speed))
        }
        XCTAssertEqual(actions.first, .spin(from: 0.3, speed: speed))
        XCTAssertEqual(writes(actions), 1)
        XCTAssertTrue(motion.spinning)
    }

    func testASteadyRecordThatSlipsIsReanchoredAndATouchStopsTheSpin() {
        var motion = DjRecordMotion()
        let speed = 2 * Double.pi / DjStudio.secondsPerTurn
        _ = motion.step(angle: 0, at: 0, steady: speed)
        XCTAssertEqual(motion.step(angle: speed * idle + 0.05, at: idle, steady: speed), .spin(from: speed * idle + 0.05, speed: speed))
        XCTAssertEqual(motion.step(angle: 1, at: 2 * idle, steady: nil), .turn(1))
        XCTAssertFalse(motion.spinning)
        XCTAssertEqual(motion.step(angle: 1, at: 3 * idle, steady: nil), .none)
    }

    func testAForgottenRecordIsDrawnAgain() {
        var motion = DjRecordMotion()
        _ = motion.step(angle: 2, at: 0, steady: nil)
        motion.forget()
        XCTAssertEqual(motion.step(angle: 2, at: idle, steady: nil), .turn(2))
    }

    func testSteadySpeedNeedsAPlayingRecordWhoseMotorHasCaughtUp() {
        let turn = 2 * Double.pi / DjStudio.secondsPerTurn
        XCTAssertEqual(DjRecordMotion.steadySpeed(playing: true, scratching: false, drive: 1, motor: 1) ?? 0, turn, accuracy: 1e-9)
        XCTAssertNil(DjRecordMotion.steadySpeed(playing: true, scratching: false, drive: 1, motor: 0.9))
        XCTAssertNil(DjRecordMotion.steadySpeed(playing: true, scratching: true, drive: 1, motor: 1))
        XCTAssertNil(DjRecordMotion.steadySpeed(playing: false, scratching: false, drive: 1, motor: 1))
        XCTAssertNil(DjRecordMotion.steadySpeed(playing: true, scratching: false, drive: 0.01, motor: 0.01))
    }
}
