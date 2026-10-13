import FMBase
import XCTest

@testable import FMPlanning

final class TimersTests: XCTestCase {
    @MainActor private func makeStore(_ kv: InMemoryKeyValueStore = InMemoryKeyValueStore(), clock: FixedClock) -> PlanningTimerStore {
        var next = 0
        return PlanningTimerStore(store: kv, clock: clock, makeId: {
            next += 1
            return "t\(next)"
        })
    }

    func testClockFormatting() {
        XCTAssertEqual(PlanningTimer.clock(0, roundUp: false), "0:00")
        XCTAssertEqual(PlanningTimer.clock(61_500, roundUp: false), "1:01")
        XCTAssertEqual(PlanningTimer.clock(61_500, roundUp: true), "1:02")
        XCTAssertEqual(PlanningTimer.clock(3_723_000, roundUp: false), "1:02:03")
        XCTAssertEqual(PlanningTimer.clock(-5, roundUp: true), "0:00")
    }

    @MainActor func testStopwatchPauseResumeSurvivesRestart() async throws {
        let clock = FixedClock(epochMillis: 1_000_000)
        let kv = InMemoryKeyValueStore()
        let store = makeStore(kv, clock: clock)
        let timer = try store.startStopwatch(label: "  Deep work ")
        XCTAssertEqual(timer.label, "Deep work")
        clock.advance(by: 90)
        store.pause(timer.id)
        clock.advance(by: 600)
        XCTAssertEqual(store.timers[0].elapsedMs(now: clock.nowMillis), 90_000)
        XCTAssertTrue(store.timers[0].isPaused)

        let reopened = makeStore(kv, clock: clock)
        XCTAssertEqual(reopened.timers.count, 1)
        reopened.resume(timer.id)
        clock.advance(by: 10)
        XCTAssertEqual(reopened.timers[0].display(now: clock.nowMillis), "1:40")
        let snapshot = reopened.activitySnapshot(reopened.timers[0])
        XCTAssertEqual(snapshot.countsFromMs, clock.nowMillis - 100_000)
        XCTAssertFalse(snapshot.paused)
    }

    @MainActor func testCountdownRingsOnceAndPlansNotification() async throws {
        let clock = FixedClock(epochMillis: 5_000_000)
        let store = makeStore(clock: clock)
        let countdown = try store.startCountdown(label: "", seconds: 300)
        XCTAssertEqual(countdown.label, "Countdown")
        XCTAssertEqual(store.notificationPlan(), [PlanningTimerNotification(
            id: "timer-t1", fireAtMs: 5_300_000, title: "Countdown", body: "Countdown finished")])
        clock.advance(by: 100)
        store.pause(countdown.id)
        XCTAssertTrue(store.notificationPlan().isEmpty, "a paused countdown schedules nothing")
        XCTAssertEqual(store.timers[0].display(now: clock.nowMillis), "3:20")
        store.resume(countdown.id)
        XCTAssertEqual(store.timers[0].fireAtMs, clock.nowMillis + 200_000)
        clock.advance(by: 199)
        XCTAssertTrue(store.settle().isEmpty)
        clock.advance(by: 5)
        let rung = store.settle()
        XCTAssertEqual(rung.map(\.id), ["t1"])
        XCTAssertTrue(store.timers[0].isFinished)
        XCTAssertEqual(store.timers[0].display(now: clock.nowMillis), "Done")
        XCTAssertTrue(store.settle().isEmpty, "rings once")
    }

    @MainActor func testValidationAndBounds() async throws {
        let clock = FixedClock(epochMillis: 10_000)
        let store = makeStore(clock: clock)
        XCTAssertThrowsError(try store.startCountdown(label: "x", seconds: 0))
        XCTAssertThrowsError(try store.startCountdown(label: "x", seconds: 24 * 3_600 + 1))
        XCTAssertThrowsError(try store.setAlarm(label: "x", atMs: 9_000))
        for _ in 0..<PlanningTimerStore.maxTimers { try store.startStopwatch(label: "") }
        XCTAssertThrowsError(try store.startStopwatch(label: "")) { error in
            XCTAssertEqual(error as? PlanningTimerStore.StoreError, .full)
        }
        let long = String(repeating: "a", count: 200)
        store.remove("t1")
        XCTAssertEqual(try store.startStopwatch(label: long).label.count, PlanningTimerStore.maxLabelLength)
    }

    @MainActor func testActivityPrefersSoonestCountdownThenAlarm() async throws {
        let clock = FixedClock(epochMillis: 0)
        clock.advance(by: 1_000)
        let store = makeStore(clock: clock)
        try store.startStopwatch(label: "watch")
        let alarm = try store.setAlarm(label: "wake", atMs: clock.nowMillis + 3_600_000)
        XCTAssertEqual(store.activityTimer()?.id, alarm.id)
        let long = try store.startCountdown(label: "long", seconds: 600)
        let short = try store.startCountdown(label: "short", seconds: 60)
        XCTAssertEqual(store.activityTimer()?.id, short.id)
        store.pause(short.id)
        XCTAssertEqual(store.activityTimer()?.id, long.id)
        let snapshot = store.activitySnapshot(store.timers.first { $0.id == short.id }!)
        XCTAssertTrue(snapshot.paused)
        XCTAssertEqual(snapshot.pausedText, "1:00")
        XCTAssertEqual(snapshot.endsAtMs, 0)
    }

    @MainActor func testOnChangeFiresAndCorruptStorageIsIgnored() async throws {
        let kv = InMemoryKeyValueStore()
        kv.set("not json", forKey: PlanningTimerStore.storageKey)
        let store = makeStore(kv, clock: FixedClock(epochMillis: 1))
        XCTAssertTrue(store.timers.isEmpty)
        var seen = 0
        store.onChange = { _ in seen += 1 }
        let timer = try store.startStopwatch(label: "a")
        store.stop(timer.id)
        store.removeFinished()
        XCTAssertEqual(seen, 3)
        XCTAssertTrue(store.timers.isEmpty)
    }
}
