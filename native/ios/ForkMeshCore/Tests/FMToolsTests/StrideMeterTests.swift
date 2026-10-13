import Foundation
import XCTest
@testable import FMTools

func strideSignal(from start: TimeInterval, seconds: TimeInterval, cadence: Double, impact: Double) -> [MotionSample] {
    let count = Int(seconds / StrideMeter.sampleInterval)
    var seed: UInt64 = 0x9E37_79B9_7F4A_7C15
    return (0..<count).map { index in
        let t = start + Double(index) * StrideMeter.sampleInterval
        seed = seed &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        let noise = (Double(seed >> 40) / Double(1 << 24) - 0.5) * 0.02
        let bounce = cadence > 0 ? impact * sin(2 * Double.pi * cadence * (t - start)) : 0
        return MotionSample(x: 0.04 + noise, y: -(1 + bounce), z: 0.1 - noise, time: t)
    }
}

final class StrideMeterTests: XCTestCase {
    private func run(_ meter: inout StrideMeter, _ samples: [MotionSample]) -> StrideReading {
        var reading = meter.reading
        for sample in samples { reading = meter.add(sample) }
        return reading
    }

    func testStillPhoneCountsNoSteps() {
        var meter = StrideMeter()
        let reading = run(&meter, strideSignal(from: 100, seconds: 6, cadence: 0, impact: 0))
        XCTAssertEqual(reading.cadence, 0)
        XCTAssertEqual(reading.gait, .still)
    }

    func testWalkingCadenceFollowsTheBounce() {
        var meter = StrideMeter()
        let reading = run(&meter, strideSignal(from: 100, seconds: 6, cadence: 1.8, impact: 0.35))
        XCTAssertEqual(reading.cadence, 1.8, accuracy: 0.05)
        XCTAssertEqual(reading.gait, .walking)
    }

    func testBriskWalkIsNotRunning() {
        var meter = StrideMeter()
        let reading = run(&meter, strideSignal(from: 100, seconds: 6, cadence: 2.3, impact: 0.5))
        XCTAssertEqual(reading.cadence, 2.3, accuracy: 0.05)
        XCTAssertEqual(reading.gait, .walking)
    }

    func testRunningIsDetectedAndCadenceTracksInRealTime() {
        var meter = StrideMeter()
        var reading = run(&meter, strideSignal(from: 100, seconds: 5, cadence: 1.7, impact: 0.3))
        XCTAssertEqual(reading.gait, .walking)
        reading = run(&meter, strideSignal(from: 105, seconds: 3, cadence: 2.8, impact: 1.5))
        XCTAssertEqual(reading.cadence, 2.8, accuracy: 0.1)
        XCTAssertEqual(reading.gait, .running)
        XCTAssertGreaterThan(reading.impact, StrideMeter.runEnterImpact)
    }

    func testRunningEasesBackToWalkingAndStops() {
        var meter = StrideMeter()
        _ = run(&meter, strideSignal(from: 100, seconds: 5, cadence: 2.8, impact: 1.5))
        var reading = run(&meter, strideSignal(from: 105, seconds: 6, cadence: 1.8, impact: 0.3))
        XCTAssertEqual(reading.gait, .walking)
        XCTAssertEqual(reading.cadence, 1.8, accuracy: 0.05)
        reading = run(&meter, strideSignal(from: 111, seconds: 2.5, cadence: 0, impact: 0))
        XCTAssertEqual(reading.cadence, 0)
        XCTAssertEqual(reading.gait, .still)
    }

    func testOutOfOrderSamplesAreIgnored() {
        var meter = StrideMeter()
        let before = run(&meter, strideSignal(from: 100, seconds: 4, cadence: 1.8, impact: 0.35))
        let after = meter.add(MotionSample(x: 0, y: -3, z: 0, time: 50))
        XCTAssertEqual(before, after)
    }
}
