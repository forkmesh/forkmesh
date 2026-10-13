// The user status model cases (the ApiService, relay and widget cases belong to the layers above).

import XCTest
import FMBase
@testable import FMApi

final class UserStatusTests: XCTestCase {
    private let zone = TimeZone(identifier: "America/New_York")!

    /// `DateTime(y, m, d, h, min)` in the test zone.
    private func local(_ year: Int, _ month: Int, _ day: Int, _ hour: Int = 0, _ minute: Int = 0) -> Date {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = zone
        return calendar.date(from: DateComponents(year: year, month: month, day: day, hour: hour, minute: minute))!
    }

    private func ms(_ date: Date) -> Int { Int(date.epochMillis) }

    func testRoundTripsTheWireShape() throws {
        let status = UserStatus(away: true, emoji: "🌴", text: "Vacationing", expiresAt: 1_760_000_000_000)
        let json = status.toJSON()
        XCTAssertEqual(json, ["away": true, "emoji": "🌴", "text": "Vacationing", "expiresAt": 1_760_000_000_000])
        XCTAssertEqual(UserStatus(json: try JSON.parse(JSON.encode(json))), status)
        XCTAssertEqual(JSON.encode(json), #"{"away":true,"emoji":"🌴","text":"Vacationing","expiresAt":1760000000000}"#)
    }

    func testReadsAMissingOrMalformedValueAsNoStatus() {
        XCTAssertEqual(UserStatus(json: nil), .empty)
        XCTAssertEqual(UserStatus(json: "🌴"), .empty)
        XCTAssertTrue(UserStatus(json: [:]).isEmpty)
        let loose = UserStatus(json: ["away": "yes", "emoji": " 🎧 ", "text": nil, "expiresAt": "-5"])
        XCTAssertFalse(loose.away)
        XCTAssertEqual(loose.emoji, "🎧")
        XCTAssertEqual(loose.text, "")
        XCTAssertEqual(loose.expiresAt, 0)
        XCTAssertEqual(UserStatus(json: ["expiresAt": 12.9]).expiresAt, 12)
        XCTAssertEqual(UserStatus(json: ["expiresAt": "42"]).expiresAt, 42)
    }

    func testAnExpiredCustomStatusIsHiddenButAwaySurvives() {
        let now = local(2026, 9, 18, 12)
        let lapsed = UserStatus(away: true, emoji: "📅", text: "In a meeting", expiresAt: ms(now))
        XCTAssertTrue(lapsed.isExpired(now))
        XCTAssertEqual(lapsed.visibleAt(now), UserStatus(away: true))
        let inAMinute = now.addingTimeInterval(60)
        let live = lapsed.copyWith(expiresAt: ms(inAMinute))
        XCTAssertFalse(live.isExpired(now))
        XCTAssertEqual(live.visibleAt(now), live)
        let forever = UserStatus(emoji: "🌴", text: "Vacationing")
        XCTAssertFalse(forever.isExpired(now))
        XCTAssertNil(forever.lapsesAt)
        XCTAssertEqual(live.lapsesAt, inAMinute)
        XCTAssertNil(UserStatus(away: true, expiresAt: 5).lapsesAt, "nothing custom to lapse")
    }

    func testDescribesTheStatusWithItsClearTime() {
        let now = local(2026, 9, 18, 9, 30)
        let status = UserStatus(emoji: "📅", text: "In a meeting", expiresAt: ms(local(2026, 9, 18, 15, 45)))
        XCTAssertEqual(status.untilLabel(now, timeZone: zone), "until 3:45 PM")
        XCTAssertEqual(status.describe(now, timeZone: zone), "📅 In a meeting · until 3:45 PM")
        XCTAssertEqual(status.copyWith(away: true).describe(now, timeZone: zone), "📅 In a meeting · until 3:45 PM — Away")
        XCTAssertEqual(UserStatus(away: true).describe(now, timeZone: zone), "Away")
        XCTAssertEqual(UserStatus(text: "Heads down").describe(now, timeZone: zone), "Heads down")
        XCTAssertEqual(UserStatus.empty.describe(now, timeZone: zone), "")
        XCTAssertEqual(UserStatus(emoji: "🌴").untilLabel(now, timeZone: zone), "")
    }

    // A Thursday.
    private var thursday: Date { local(2026, 9, 17, 10, 15) }

    func testFixedDurationsCountFromNow() {
        let now = thursday
        XCTAssertEqual(ClearAfter.never.expiresAt(now, timeZone: zone), 0)
        XCTAssertEqual(ClearAfter.thirtyMinutes.expiresAt(now, timeZone: zone), ms(local(2026, 9, 17, 10, 45)))
        XCTAssertEqual(ClearAfter.oneHour.expiresAt(now, timeZone: zone), ms(local(2026, 9, 17, 11, 15)))
        XCTAssertEqual(ClearAfter.fourHours.expiresAt(now, timeZone: zone), ms(local(2026, 9, 17, 14, 15)))
    }

    func testTodayEndsAtLocalMidnightAndThisWeekAtTheEndOfSunday() {
        let now = thursday
        XCTAssertEqual(ClearAfter.today.expiresAt(now, timeZone: zone), ms(local(2026, 9, 18)) - 1)
        XCTAssertEqual(ClearAfter.thisWeek.expiresAt(now, timeZone: zone), ms(local(2026, 9, 21)) - 1)
        // On a Sunday "this week" is the rest of today.
        let sunday = local(2026, 9, 20, 22)
        XCTAssertEqual(ClearAfter.thisWeek.expiresAt(sunday, timeZone: zone), ClearAfter.today.expiresAt(sunday, timeZone: zone))
        // A Monday runs to the Sunday after.
        XCTAssertEqual(ClearAfter.thisWeek.expiresAt(local(2026, 9, 21, 8), timeZone: zone), ms(local(2026, 9, 28)) - 1)
        // Across the end of daylight saving (Sunday 2026-11-01 in New York).
        XCTAssertEqual(ClearAfter.thisWeek.expiresAt(local(2026, 10, 29, 9), timeZone: zone), ms(local(2026, 11, 2)) - 1)
    }

    func testCustomUsesThePickedMomentOrNeverWithoutOne() {
        let picked = local(2026, 9, 25, 8)
        XCTAssertEqual(ClearAfter.custom.expiresAt(thursday, customAt: picked, timeZone: zone), ms(picked))
        XCTAssertEqual(ClearAfter.custom.expiresAt(thursday, timeZone: zone), 0)
    }

    func testOffersEveryChoiceTheSheetListsInOrder() {
        XCTAssertEqual(ClearAfter.allCases.map(\.label), [
            "Don't clear", "30 minutes", "1 hour", "4 hours", "Today", "This week", "Custom date & time",
        ])
    }

    func testPresetsAndQuickPicksMatchTheOtherClients() {
        XCTAssertEqual(statusPresets.map(\.label), [
            "📅 In a meeting — 1 hour",
            "🚌 Commuting — 30 minutes",
            "🤒 Out sick — Today",
            "🌴 Vacationing — Don't clear",
            "🏡 Working remotely — Today",
        ])
        XCTAssertEqual(statusQuickEmojis.count, 10)
        XCTAssertTrue(statusQuickEmojis.allSatisfy(looksLikeSingleEmoji))
    }

    func testFormatsClearTimesRelativeToNow() {
        let now = local(2026, 9, 17, 10) // Thursday
        XCTAssertEqual(formatStatusMoment(local(2026, 9, 17, 18, 5), now, timeZone: zone), "6:05 PM")
        XCTAssertEqual(formatStatusMoment(local(2026, 9, 18, 9), now, timeZone: zone), "tomorrow 9:00 AM")
        XCTAssertEqual(formatStatusMoment(local(2026, 9, 21, 9), now, timeZone: zone), "Mon 9:00 AM")
        XCTAssertEqual(formatStatusMoment(local(2026, 10, 2, 9), now, timeZone: zone), "Oct 2")
        XCTAssertEqual(formatStatusMoment(local(2027, 1, 4, 9), now, timeZone: zone), "Jan 4, 2027")
        XCTAssertEqual(formatStatusMoment(local(2026, 9, 17, 0, 5), now, timeZone: zone), "12:05 AM")
    }

    func testEmojiCheckAcceptsOneGraphemeAndRefusesText() {
        for ok in ["😀", "🧑🏽‍💻", "👨‍👩‍👧‍👦", "🏳️‍🌈", "🇺🇸", "1️⃣", "❤️"] {
            XCTAssertTrue(looksLikeSingleEmoji(ok), ok)
        }
        for bad in ["", " ", "a", "hi", "😀😀", "1", "#", "A😀"] {
            XCTAssertFalse(looksLikeSingleEmoji(bad), bad)
        }
    }

    func testServerErrorCodesReadAsSentences() {
        for code in [
            "bad_status_emoji", "status_text_too_long", "bad_status_expiry", "status_expiry_in_past",
            "status_expiry_too_far", "unauthorized", "something_new",
        ] {
            XCTAssertFalse(userStatusErrorMessage(code).contains("_"), code)
        }
        XCTAssertTrue(userStatusErrorMessage("status_text_too_long").contains("100"))
    }
}
