import XCTest
@testable import FMBase

final class UserActivityTests: XCTestCase {
    private var calendar: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        return calendar
    }

    private func at(_ day: Int, _ hour: Int, _ minute: Int) -> Date {
        calendar.date(from: DateComponents(year: 2026, month: 10, day: day, hour: hour, minute: minute))!
    }

    func testSlotsAreHourColumnsOfFiveMinuteRows() {
        XCTAssertEqual(ActivityGrid.slots, 288)
        XCTAssertEqual(ActivityGrid.slot(hour: 0, minute: 4), 0)
        XCTAssertEqual(ActivityGrid.slot(hour: 0, minute: 5), 1)
        XCTAssertEqual(ActivityGrid.slot(hour: 14, minute: 37), 14 * 12 + 7)
        XCTAssertEqual(ActivityGrid.slot(hour: 23, minute: 59), 287)
        XCTAssertEqual(ActivityGrid.label(14 * 12 + 7), "14:35")
    }

    func testRecordCountsAndRollsOverAtMidnight() {
        let clock = FixedClock(at(2, 9, 1))
        let log = UserActivityLog(store: nil, clock: clock, calendar: calendar)
        log.record()
        log.record(at: at(2, 9, 3))
        log.record(at: at(2, 17, 55))
        let counts = log.counts()
        XCTAssertEqual(counts.count, 288)
        XCTAssertEqual(counts[9 * 12], 2)
        XCTAssertEqual(counts[17 * 12 + 11], 1)
        XCTAssertEqual(log.counts(on: at(3, 1, 0)).reduce(0, +), 0)
        log.record(at: at(3, 0, 2))
        XCTAssertEqual(log.counts().reduce(0, +), 0)
        XCTAssertEqual(log.counts(on: at(3, 0, 0))[0], 1)
    }

    func testCountsSurviveARestart() {
        let store = InMemoryKeyValueStore()
        let clock = FixedClock(at(2, 11, 20))
        let log = UserActivityLog(store: store, clock: clock, calendar: calendar)
        for _ in 0..<5 { log.record() }
        XCTAssertEqual(UserActivityLog(store: store, clock: clock, calendar: calendar).counts()[11 * 12 + 4], 1)
        log.flush()
        XCTAssertEqual(UserActivityLog(store: store, clock: clock, calendar: calendar).counts()[11 * 12 + 4], 5)
        store.set("garbage", forKey: UserActivityLog.key)
        XCTAssertEqual(UserActivityLog(store: store, clock: clock, calendar: calendar).counts().reduce(0, +), 0)
    }

    func testDeviceKeyIsKeptAndOtherDevicesAddToTheDay() {
        let store = InMemoryKeyValueStore()
        let clock = FixedClock(at(6, 8, 0))
        let log = UserActivityLog(store: store, clock: clock, calendar: calendar)
        XCTAssertTrue((8...64).contains(log.deviceKey.count))
        XCTAssertEqual(UserActivityLog(store: store, clock: clock, calendar: calendar).deviceKey, log.deviceKey)
        XCTAssertNotEqual(UserActivityLog(store: InMemoryKeyValueStore(), clock: clock).deviceKey, log.deviceKey)
        let before = log.revision
        log.record()
        XCTAssertEqual(log.revision, before + 1)
        XCTAssertEqual(log.today, "2026-10-06")
        var others = [Int](repeating: 0, count: 288)
        others[8 * 12] = 4
        others[20 * 12] = 2
        log.setOthers(day: "2026-10-06", counts: others, kinds: ["desktop", "web"])
        XCTAssertEqual(log.counts()[8 * 12], 5)
        XCTAssertEqual(log.counts()[20 * 12], 2)
        XCTAssertEqual(log.localCounts()[8 * 12], 1)
        XCTAssertEqual(log.counts(on: at(7, 8, 0)).reduce(0, +), 0)
        XCTAssertEqual(log.otherKinds(), ["desktop", "web"])
        log.setOthers(day: "2026-10-06", counts: [1, 2], kinds: [])
        XCTAssertEqual(log.counts()[8 * 12], 5)
        log.clearOthers()
        XCTAssertEqual(log.counts().reduce(0, +), 1)
        XCTAssertEqual(log.otherKinds(), [])
    }

    func testSourcesText() {
        XCTAssertEqual(ActivityGrid.sourcesText(self: "ios", others: []), "this device")
        XCTAssertEqual(ActivityGrid.sourcesText(self: "ios", others: ["desktop", "web", "desktop"]), "this device, desktop and browser")
        XCTAssertEqual(ActivityGrid.sourcesText(self: "ios", others: ["ios"]), "this device and another iOS device")
    }

    func testSummaryAndLevels() {
        var counts = [Int](repeating: 0, count: 288)
        counts[10 * 12 + 2] = 8
        counts[10 * 12 + 3] = 4
        counts[15 * 12] = 10
        let summary = ActivitySummary(counts)
        XCTAssertEqual(summary.total, 22)
        XCTAssertEqual(summary.peak, 10)
        XCTAssertEqual(summary.peakSlot, 15 * 12)
        XCTAssertEqual(summary.peakHour, 10)
        XCTAssertEqual(summary.peakHourTotal, 12)
        XCTAssertEqual(summary.activeSlots, 3)
        XCTAssertEqual(summary.activeText, "15m")
        XCTAssertEqual(ActivitySummary([]).peakSlot, -1)
        XCTAssertEqual(ActivityGrid.level(0, peak: 10), 0)
        XCTAssertEqual(ActivityGrid.level(1, peak: 100), 1)
        XCTAssertEqual(ActivityGrid.level(50, peak: 100), 2)
        XCTAssertEqual(ActivityGrid.level(100, peak: 100), 4)
    }
}
