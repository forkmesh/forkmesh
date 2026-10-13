import Foundation
import FMBase

/// A member's self-set away flag and custom status: one emoji, a short line of
/// text, and an optional moment it clears itself.
///
/// The account record on the server is the source of truth
/// (`POST /api/accounts/user-status`); profile lookups, the chat user
/// directory and the relay's hello/presence frames all carry it as
/// `userStatus: {away, emoji, text, expiresAt}`. Expiry is applied again on
/// every client before painting, so an edge-cached profile or a presence frame
/// sent an hour ago never shows a status that has already lapsed.
public struct UserStatus: Equatable, Hashable, Sendable, CustomStringConvertible {
    public static let empty = UserStatus()

    /// The longest status text the server keeps.
    public static let textMax = 100

    public var away: Bool
    public var emoji: String
    public var text: String
    /// Epoch milliseconds the custom status clears at; 0 means never.
    public var expiresAt: Int

    public init(away: Bool = false, emoji: String = "", text: String = "", expiresAt: Int = 0) {
        self.away = away
        self.emoji = emoji
        self.text = text
        self.expiresAt = expiresAt
    }

    /// Tolerant of whatever a frame or lookup carried: a missing or malformed
    /// value reads as no status rather than throwing.
    public init(json: JSONValue?) {
        guard let object = json?.objectValue else {
            self = .empty
            return
        }
        let raw = object["expiresAt"]
        let expiresAt: Int
        if let number = raw?.doubleValue {
            expiresAt = ModelJSON.int(.double(number))
        } else {
            expiresAt = DartCore.intTryParse(ModelJSON.string(raw)) ?? 0
        }
        self.init(
            away: object.bool("away"),
            emoji: object.str("emoji").dartTrimmed,
            text: object.str("text").dartTrimmed,
            expiresAt: max(expiresAt, 0)
        )
    }

    public func toJSON() -> JSONObject {
        ["away": .bool(away), "emoji": .string(emoji), "text": .string(text), "expiresAt": .int(Int64(expiresAt))]
    }

    public var hasCustom: Bool { !emoji.isEmpty || !text.isEmpty }

    public var isEmpty: Bool { !away && !hasCustom }

    /// True once the custom status' clear-after moment has passed.
    public func isExpired(_ now: Date) -> Bool {
        expiresAt != 0 && Int64(expiresAt) <= now.epochMillis
    }

    /// What should be painted at `now`: the away flag always, the custom status
    /// only while it has not lapsed.
    public func visibleAt(_ now: Date) -> UserStatus {
        isExpired(now) ? UserStatus(away: away) : self
    }

    /// When the painted status next changes on its own, or nil if never.
    public var lapsesAt: Date? {
        expiresAt == 0 || !hasCustom ? nil : Date(epochMillis: Int64(expiresAt))
    }

    public func copyWith(away: Bool? = nil, emoji: String? = nil, text: String? = nil, expiresAt: Int? = nil) -> UserStatus {
        UserStatus(
            away: away ?? self.away,
            emoji: emoji ?? self.emoji,
            text: text ?? self.text,
            expiresAt: expiresAt ?? self.expiresAt
        )
    }

    /// "until 3:45 PM", "until Tue 9:00 AM", "until Sep 25", or "" when the
    /// status never clears.
    public func untilLabel(_ now: Date, timeZone: TimeZone = .current) -> String {
        if expiresAt == 0 { return "" }
        return "until \(formatStatusMoment(Date(epochMillis: Int64(expiresAt)), now, timeZone: timeZone))"
    }

    /// One line for tooltips and screen readers: "🌴 Vacationing · until Sep 25",
    /// "Away", or "" when there is nothing to say.
    public func describe(_ now: Date, timeZone: TimeZone = .current) -> String {
        let visible = visibleAt(now)
        var parts: [String] = []
        if visible.hasCustom {
            let line = [visible.emoji, visible.text].filter { !$0.isEmpty }.joined(separator: " ")
            let until = visible.untilLabel(now, timeZone: timeZone)
            parts.append(until.isEmpty ? line : "\(line) · \(until)")
        }
        if visible.away { parts.append("Away") }
        return parts.joined(separator: " — ")
    }

    public var description: String { "UserStatus(\(JSON.encode(toJSON())))" }
}

/// The "Clear after" choices offered when setting a status.
public enum ClearAfter: String, CaseIterable, Sendable {
    case never, thirtyMinutes, oneHour, fourHours, today, thisWeek, custom

    public var label: String {
        switch self {
        case .never: return "Don't clear"
        case .thirtyMinutes: return "30 minutes"
        case .oneHour: return "1 hour"
        case .fourHours: return "4 hours"
        case .today: return "Today"
        case .thisWeek: return "This week"
        case .custom: return "Custom date & time"
        }
    }

    /// Epoch milliseconds the status should clear at, or 0 for `never`.
    ///
    /// "Today" ends at local midnight, "This week" at the end of Sunday. A
    /// `custom` choice uses `customAt`; without one it never clears.
    public func expiresAt(_ now: Date, customAt: Date? = nil, timeZone: TimeZone = .current) -> Int {
        let ms = Int(now.epochMillis)
        switch self {
        case .never: return 0
        case .thirtyMinutes: return ms + 30 * 60_000
        case .oneHour: return ms + 60 * 60_000
        case .fourHours: return ms + 4 * 60 * 60_000
        case .today: return Self.endOfDay(now, 0, timeZone)
        case .thisWeek:
            // ISO weekday: Monday 1 ... Sunday 7; Calendar: Sunday 1 ... Saturday 7.
            let weekday = statusCalendar(timeZone).component(.weekday, from: now)
            let dartWeekday = (weekday + 5) % 7 + 1
            return Self.endOfDay(now, 7 - dartWeekday, timeZone)
        case .custom: return customAt.map { Int($0.epochMillis) } ?? 0
        }
    }

    /// The last millisecond of the local day `days` after `now`'s.
    private static func endOfDay(_ now: Date, _ days: Int, _ timeZone: TimeZone) -> Int {
        let calendar = statusCalendar(timeZone)
        let start = calendar.startOfDay(for: now)
        let next = calendar.date(byAdding: .day, value: days + 1, to: start) ?? start
        return Int(next.epochMillis) - 1
    }
}

private func statusCalendar(_ timeZone: TimeZone) -> Calendar {
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = timeZone
    calendar.locale = Locale(identifier: "en_US_POSIX")
    return calendar
}

/// A ready-made status the sheet offers in one tap.
public struct StatusPreset: Equatable, Hashable, Sendable {
    public let emoji: String
    public let text: String
    public let clearAfter: ClearAfter

    public init(_ emoji: String, _ text: String, _ clearAfter: ClearAfter) {
        self.emoji = emoji
        self.text = text
        self.clearAfter = clearAfter
    }

    public var label: String { "\(emoji) \(text) — \(clearAfter.label)" }
}

public let statusQuickEmojis = ["📅", "🚌", "🤒", "🌴", "🏡", "🎧", "🍔", "💻", "🎉", "🔕"]

public let statusPresets = [
    StatusPreset("📅", "In a meeting", .oneHour),
    StatusPreset("🚌", "Commuting", .thirtyMinutes),
    StatusPreset("🤒", "Out sick", .today),
    StatusPreset("🌴", "Vacationing", .never),
    StatusPreset("🏡", "Working remotely", .today),
]

/// A cheap client-side check that `value` is a single emoji grapheme, so an
/// obvious typo is caught before the round trip. The server's parser is the
/// authority and answers `bad_status_emoji` for anything it refuses.
public func looksLikeSingleEmoji(_ value: String) -> Bool {
    let trimmed = value.dartTrimmed
    if trimmed.isEmpty || trimmed.count != 1 { return false }
    guard let first = trimmed.unicodeScalars.first else { return false }
    // Letters, digits and punctuation are single graphemes too; keycaps
    // ("1️⃣") start with a digit but carry the combining enclosing keycap.
    if first.value < 0xA9 { return trimmed.unicodeScalars.contains { $0.value == 0x20E3 } }
    return true
}

/// A status moment written for people, relative to `now`: the time alone
/// today, weekday and time within the week ahead, a date beyond that. The
/// en_US patterns `jm`, `E`, `MMMd` and `yMMMd`, read in `timeZone`.
public func formatStatusMoment(_ moment: Date, _ now: Date, timeZone: TimeZone = .current) -> String {
    let calendar = statusCalendar(timeZone)
    let today = calendar.startOfDay(for: now)
    let day = calendar.startOfDay(for: moment)
    // Rounded, so a daylight-saving day of 23 or 25 hours still counts as one.
    let hours = Int(day.timeIntervalSince(today) / 3_600)
    let days = Int((Double(hours) / 24).rounded())
    let time = statusFormat("h:mm a", moment, timeZone)
    if days == 0 { return time }
    if days == 1 { return "tomorrow \(time)" }
    if days > 1 && days < 7 { return "\(statusFormat("EEE", moment, timeZone)) \(time)" }
    if calendar.component(.year, from: moment) == calendar.component(.year, from: now) {
        return statusFormat("MMM d", moment, timeZone)
    }
    return statusFormat("MMM d, y", moment, timeZone)
}

private func statusFormat(_ pattern: String, _ date: Date, _ timeZone: TimeZone) -> String {
    let formatter = DateFormatter()
    formatter.locale = Locale(identifier: "en_US_POSIX")
    formatter.timeZone = timeZone
    formatter.dateFormat = pattern
    return formatter.string(from: date)
}

/// Server error codes from `POST /api/accounts/user-status`, written for people.
public func userStatusErrorMessage(_ code: String) -> String {
    switch code {
    case "bad_status_emoji": return "Pick a single emoji for your status."
    case "status_text_too_long": return "Keep your status under \(UserStatus.textMax) characters."
    case "bad_status_expiry": return "That clear-after time is not valid."
    case "status_expiry_in_past": return "Choose a clear-after time in the future."
    case "status_expiry_too_far":
        return "A status can clear at most a year from now. Choose \"Don't clear\" to keep it."
    case "unauthorized": return "Your session expired. Please sign in again."
    default: return "Could not update your status. Please try again."
    }
}
