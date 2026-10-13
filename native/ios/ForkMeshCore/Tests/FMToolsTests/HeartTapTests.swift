import XCTest
@testable import FMTools

@MainActor
final class FakeHeartRate: HeartRateBridge {
    var available = true
    var allow = true
    var fail = false
    var saved: [(bpm: Double, start: Date, end: Date)] = []
    var authorized = 0

    struct Failure: Error {}

    func authorizeHeartRate() async -> Bool {
        authorized += 1
        return allow
    }

    func saveHeartRate(beatsPerMinute: Double, start: Date, end: Date) async throws {
        if fail { throw Failure() }
        saved.append((beatsPerMinute, start, end))
    }
}

@MainActor
final class HeartTapTests: XCTestCase {
    let origin = Date(timeIntervalSince1970: 1_800_000_000)

    func times(_ gaps: [TimeInterval]) -> [Date] {
        var at = origin
        var result = [at]
        for gap in gaps {
            at = at.addingTimeInterval(gap)
            result.append(at)
        }
        return result
    }

    func model(_ bridge: FakeHeartRate?) -> (HeartTapModel, (TimeInterval) -> Void) {
        var clock = origin
        let model = HeartTapModel(bridge: bridge, now: { clock })
        return (model, { clock = clock.addingTimeInterval($0) })
    }

    func testSteadyTapsGiveTheRate() {
        XCTAssertEqual(HeartTap.bpm(times([1, 1, 1, 1])), 60)
        XCTAssertEqual(HeartTap.bpm(times([0.8, 0.8, 0.8, 0.8, 0.8])), 75)
    }

    func testTooFewTapsGiveNothing() {
        XCTAssertNil(HeartTap.bpm(times([1, 1, 1])))
        XCTAssertNil(HeartTap.bpm([]))
    }

    func testAMissedBeatIsLeftOut() {
        XCTAssertEqual(HeartTap.bpm(times([1, 1, 2, 1, 1])), 60)
    }

    func testRatesOutsideTheRangeAreRejected() {
        XCTAssertNil(HeartTap.bpm(times([0.2, 0.2, 0.2, 0.2])))
        XCTAssertNil(HeartTap.bpm(times([2.4, 2.4, 2.4, 2.4])))
    }

    func testOnlyTheLatestIntervalsCount() {
        let gaps = Array(repeating: 1.0, count: 4) + Array(repeating: 0.5, count: HeartTap.maxIntervals)
        XCTAssertEqual(HeartTap.bpm(times(gaps)), 120)
    }

    func testTappingCountsDownThenShowsTheRate() {
        let (heart, advance) = model(FakeHeartRate())
        XCTAssertEqual(heart.line, HeartTapModel.idleLine)
        heart.tap()
        XCTAssertEqual(heart.line, "Keep going: 4 more taps.")
        for _ in 0..<3 {
            advance(0.75)
            heart.tap()
        }
        XCTAssertEqual(heart.line, "Keep going: 1 more tap.")
        XCTAssertFalse(heart.canSave)
        advance(0.75)
        heart.tap()
        XCTAssertEqual(heart.bpm, 80)
        XCTAssertEqual(heart.line, "80 BPM from 5 taps.")
        XCTAssertTrue(heart.canSave)
    }

    func testAPauseStartsOver() {
        let (heart, advance) = model(FakeHeartRate())
        for _ in 0..<5 {
            heart.tap()
            advance(1)
        }
        XCTAssertEqual(heart.bpm, 60)
        advance(HeartTap.resetAfter)
        heart.tap()
        XCTAssertEqual(heart.taps.count, 1)
        XCTAssertNil(heart.bpm)
    }

    func testTapsAreCapped() {
        let (heart, advance) = model(FakeHeartRate())
        for _ in 0..<30 {
            heart.tap()
            advance(1)
        }
        XCTAssertEqual(heart.taps.count, HeartTap.maxIntervals + 1)
    }

    func testSaveWritesTheRateOverTheTappedSpan() async {
        let bridge = FakeHeartRate()
        let (heart, advance) = model(bridge)
        for index in 0..<5 {
            if index > 0 { advance(1) }
            heart.tap()
        }
        await heart.save()
        XCTAssertEqual(bridge.authorized, 1)
        XCTAssertEqual(bridge.saved.count, 1)
        XCTAssertEqual(bridge.saved.first?.bpm, 60)
        XCTAssertEqual(bridge.saved.first?.start, origin)
        XCTAssertEqual(bridge.saved.first?.end, origin.addingTimeInterval(4))
        XCTAssertEqual(heart.savedBpm, 60)
        XCTAssertEqual(heart.line, "60 BPM saved to Apple Health.")
        XCTAssertEqual(heart.notice?.message, "Saved 60 BPM to Apple Health.")
        XCTAssertFalse(heart.saving)
    }

    func testSaveDeniedSavesNothing() async {
        let bridge = FakeHeartRate()
        bridge.allow = false
        let (heart, advance) = model(bridge)
        for _ in 0..<5 {
            heart.tap()
            advance(1)
        }
        await heart.save()
        XCTAssertTrue(bridge.saved.isEmpty)
        XCTAssertNil(heart.savedBpm)
        XCTAssertEqual(heart.notice?.message, HeartTapModel.deniedMessage)
    }

    func testSaveFailureIsReported() async {
        let bridge = FakeHeartRate()
        bridge.fail = true
        let (heart, advance) = model(bridge)
        for _ in 0..<5 {
            heart.tap()
            advance(1)
        }
        await heart.save()
        XCTAssertNil(heart.savedBpm)
        XCTAssertTrue(heart.notice?.message.hasPrefix("Could not save to Apple Health") == true)
    }

    func testNoHealthMeansNoSave() async {
        let (heart, advance) = model(nil)
        for _ in 0..<5 {
            heart.tap()
            advance(1)
        }
        XCTAssertFalse(heart.canSave)
        await heart.save()
        XCTAssertEqual(heart.notice?.message, HeartTapModel.unavailableMessage)
    }

    func testResetClears() {
        let (heart, advance) = model(FakeHeartRate())
        for _ in 0..<5 {
            heart.tap()
            advance(1)
        }
        heart.reset()
        XCTAssertTrue(heart.taps.isEmpty)
        XCTAssertNil(heart.bpm)
        XCTAssertEqual(heart.line, HeartTapModel.idleLine)
    }
}
