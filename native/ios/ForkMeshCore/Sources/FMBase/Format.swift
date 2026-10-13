// How raw values are written for people
// (relative and absolute timestamps, hide-zero counts, 12-hour clock times),
// the same on every screen.

import Foundation

public enum Format {
    /// How long ago `timeMillis` was, in the shortest unambiguous form
    /// ("5d ago"). A device clock a little ahead of the server reads as
    /// "just now". Pass `nowMillis` for a deterministic result.
    public static func relativeTimeFrom(_ timeMillis: Int64, nowMillis: Int64 = SystemClock().nowMillis) -> String {
        let diff = nowMillis - timeMillis
        if diff < 0 { return "just now" }
        let seconds = diff / 1_000
        let minutes = diff / 60_000
        let hours = diff / 3_600_000
        let days = diff / 86_400_000
        if seconds < 60 { return "just now" }
        if minutes < 60 { return "\(minutes)m ago" }
        if hours < 24 { return "\(hours)h ago" }
        if days < 30 { return "\(days)d ago" }
        let months = days / 30
        if months < 12 { return "\(months)mo ago" }
        return "\(days / 365)y ago"
    }

    public static func relativeTimeFrom(_ time: Date, now: Date = Date()) -> String {
        relativeTimeFrom(time.epochMillis, nowMillis: now.epochMillis)
    }

    /// `relativeTimeFrom` for a millisecond epoch, where 0 means "no timestamp".
    public static func relativeTime(_ epochMs: Int64, nowMillis: Int64 = SystemClock().nowMillis) -> String {
        if epochMs <= 0 { return "never" }
        return relativeTimeFrom(epochMs, nowMillis: nowMillis)
    }

    /// The full UTC date (yyyy-MM-dd), for tooltips and detail screens where the exact day matters.
    public static func absoluteDate(_ epochMs: Int64) -> String {
        if epochMs <= 0 { return "never" }
        // Civil date from a day count (Howard Hinnant's algorithm): no
        // Calendar, so no locale or time zone can leak in.
        let z = epochMs / 86_400_000 + 719_468
        let era = z / 146_097
        let doe = z - era * 146_097
        let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100)
        let mp = (5 * doy + 2) / 153
        let day = doy - (153 * mp + 2) / 5 + 1
        let month = mp < 10 ? mp + 3 : mp - 9
        let year = yoe + era * 400 + (month <= 2 ? 1 : 0)
        return "\(year)-\(pad2(month))-\(pad2(day))"
    }

    /// A count worth showing, or nil when it is zero: "3", or "3 stars" with a `singular`.
    public static func countOrNil(_ value: Int, singular: String? = nil, plural: String? = nil) -> String? {
        if value <= 0 { return nil }
        guard let singular else { return "\(value)" }
        return "\(value) \(value == 1 ? singular : (plural ?? "\(singular)s"))"
    }

    /// A wall-clock time the way a phone's own clock writes it: 12-hour with
    /// a meridiem, as in "3:04:05 PM". The time is read in `timeZone`.
    public static func clockTime12(_ time: Date, timeZone: TimeZone = .current) -> String {
        let local = time.epochMillis / 1_000 + Int64(timeZone.secondsFromGMT(for: time))
        let ofDay = ((local % 86_400) + 86_400) % 86_400
        return clock12(Int(ofDay / 3_600), Int(ofDay / 60 % 60), Int(ofDay % 60))
    }

    /// `clockTime12` for a 24-hour "HH:MM:SS" (or "HH:MM") stamp, such as the
    /// one each relay log line carries. Anything that is not such a stamp is
    /// returned untouched: a log line with an unexpected shape should still
    /// print whatever it does have.
    public static func clockTime12From24(_ stamp: String) -> String {
        let parts = stamp.split(separator: ":", omittingEmptySubsequences: false)
        if parts.count < 2 || parts.count > 3 { return stamp }
        guard let hour = dartInt(parts[0]), let minute = dartInt(parts[1]),
              let second = parts.count == 3 ? dartInt(parts[2]) : 0 else { return stamp }
        if hour < 0 || hour > 23 || minute < 0 || minute > 59 { return stamp }
        if second < 0 || second > 59 { return stamp }
        return clock12(hour, minute, second)
    }

    private static func clock12(_ hour: Int, _ minute: Int, _ second: Int) -> String {
        let meridiem = hour < 12 ? "AM" : "PM"
        let h = hour % 12 == 0 ? 12 : hour % 12
        return "\(h):\(pad2(minute)):\(pad2(second)) \(meridiem)"
    }

    /// `int.tryParse` tolerates surrounding whitespace; `Int("...")` does not.
    private static func dartInt(_ text: Substring) -> Int? { Int(String(text).dartTrimmed) }

    private static func pad2<T: BinaryInteger>(_ value: T) -> String {
        value < 10 ? "0\(value)" : "\(value)"
    }
}
