// Cases of native/android core/src/test format/FormatTest.kt.

import XCTest
@testable import FMBase

final class FormatTests: XCTestCase {
    private let now: Int64 = 1_786_636_663_064
    private let minute: Int64 = 60_000
    private var hour: Int64 { 60 * minute }
    private var day: Int64 { 24 * hour }

    func testRelativeTimeSteps() {
        XCTAssertEqual(Format.relativeTimeFrom(now - 59_999, nowMillis: now), "just now")
        XCTAssertEqual(Format.relativeTimeFrom(now - minute, nowMillis: now), "1m ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - hour + 1, nowMillis: now), "59m ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - hour, nowMillis: now), "1h ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - day + 1, nowMillis: now), "23h ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - day, nowMillis: now), "1d ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - 29 * day, nowMillis: now), "29d ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - 30 * day, nowMillis: now), "1mo ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - 359 * day, nowMillis: now), "11mo ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - 360 * day, nowMillis: now), "0y ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - 365 * day, nowMillis: now), "1y ago")
        XCTAssertEqual(Format.relativeTimeFrom(now - 800 * day, nowMillis: now), "2y ago")
    }

    func testRelativeTimeFromDates() {
        let reference = Date(epochMillis: now)
        XCTAssertEqual(Format.relativeTimeFrom(reference.addingTimeInterval(-7_200), now: reference), "2h ago")
    }

    func testClockAheadReadsAsJustNow() {
        XCTAssertEqual(Format.relativeTimeFrom(now + 5 * minute, nowMillis: now), "just now")
    }

    func testZeroEpochIsNever() {
        XCTAssertEqual(Format.relativeTime(0, nowMillis: now), "never")
        XCTAssertEqual(Format.relativeTime(-1, nowMillis: now), "never")
        XCTAssertEqual(Format.relativeTime(now - 3 * hour, nowMillis: now), "3h ago")
        XCTAssertEqual(Format.absoluteDate(0), "never")
    }

    func testAbsoluteDateIsUtc() {
        // 2026-08-12T23:30:00Z stays on the 12th whatever the host time zone.
        XCTAssertEqual(Format.absoluteDate(1_786_577_400_000), "2026-08-12")
        XCTAssertEqual(Format.absoluteDate(86_400_000), "1970-01-02")
        XCTAssertEqual(Format.absoluteDate(1), "1970-01-01")
        XCTAssertEqual(Format.absoluteDate(951_782_400_000), "2000-02-29")
        XCTAssertEqual(Format.absoluteDate(1_735_689_599_999), "2024-12-31")
        XCTAssertEqual(Format.absoluteDate(1_735_689_600_000), "2025-01-01")
    }

    func testCountOrNilHidesZero() {
        XCTAssertNil(Format.countOrNil(0))
        XCTAssertNil(Format.countOrNil(-2, singular: "star"))
        XCTAssertEqual(Format.countOrNil(3), "3")
        XCTAssertEqual(Format.countOrNil(1, singular: "star"), "1 star")
        XCTAssertEqual(Format.countOrNil(2, singular: "star"), "2 stars")
        XCTAssertEqual(Format.countOrNil(2, singular: "fork"), "2 forks")
        XCTAssertEqual(Format.countOrNil(2, singular: "reply", plural: "replies"), "2 replies")
    }

    func testClockTime12From24() {
        XCTAssertEqual(Format.clockTime12From24("00:00:00"), "12:00:00 AM")
        XCTAssertEqual(Format.clockTime12From24("09:05:07"), "9:05:07 AM")
        XCTAssertEqual(Format.clockTime12From24("12:00:00"), "12:00:00 PM")
        XCTAssertEqual(Format.clockTime12From24("15:04:05"), "3:04:05 PM")
        XCTAssertEqual(Format.clockTime12From24("23:59"), "11:59:00 PM")
        for untouched in ["24:00:00", "12:60:00", "12:00:60", "noon", "1:2:3:4", "aa:bb", "", "12"] {
            XCTAssertEqual(Format.clockTime12From24(untouched), untouched)
        }
    }

    func testClockTime12ReadsTheGivenZone() throws {
        let time = Date(epochMillis: 1_786_577_400_000) // 23:30:00Z
        XCTAssertEqual(Format.clockTime12(time, timeZone: try XCTUnwrap(TimeZone(secondsFromGMT: 0))), "11:30:00 PM")
        XCTAssertEqual(Format.clockTime12(time, timeZone: try XCTUnwrap(TimeZone(secondsFromGMT: 3_600))), "12:30:00 AM")
        XCTAssertEqual(Format.clockTime12(time, timeZone: try XCTUnwrap(TimeZone(secondsFromGMT: -5 * 3_600))), "6:30:00 PM")
    }
}
