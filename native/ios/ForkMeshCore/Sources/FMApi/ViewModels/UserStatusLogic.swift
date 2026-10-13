// The decisions of the user status sheet (Save, the initial clear-after
// choice, the custom label) and of the Settings screen's status summary, plus
// the lapse timing of a custom status.

import FMBase
import Foundation

/// What Save does with the sheet's fields.
public enum UserStatusSaveAction: Equatable, Sendable {
    /// Refused before any request; the sheet shows the sentence.
    case problem(String)
    /// Emoji and text both empty: clear the custom status.
    case clear
    /// Write the custom status.
    case setCustom(emoji: String, text: String, expiresAt: Int)
}

public func userStatusSaveAction(
    emoji rawEmoji: String,
    text rawText: String,
    clearAfter: ClearAfter,
    customAt: Date?,
    now: Date,
    timeZone: TimeZone = .current
) -> UserStatusSaveAction {
    let emoji = rawEmoji.dartTrimmed
    let text = rawText.dartTrimmed
    if !emoji.isEmpty && !looksLikeSingleEmoji(emoji) {
        return .problem(userStatusErrorMessage("bad_status_emoji"))
    }
    if emoji.isEmpty && text.isEmpty { return .clear }
    if clearAfter == .custom {
        guard let customAt, customAt > now else {
            return .problem(userStatusErrorMessage("status_expiry_in_past"))
        }
    }
    return .setCustom(
        emoji: emoji, text: text, expiresAt: clearAfter.expiresAt(now, customAt: customAt, timeZone: timeZone))
}

/// The sheet's starting "Clear after": never for a status that never clears,
/// otherwise custom at the stored moment.
public func initialClearAfter(_ status: UserStatus) -> ClearAfter { status.expiresAt == 0 ? .never : .custom }

public func initialCustomAt(_ status: UserStatus) -> Date? {
    status.expiresAt == 0 ? nil : Date(epochMillis: Int64(status.expiresAt))
}

/// `_customLabel`.
public func userStatusCustomLabel(_ customAt: Date?, now: Date, timeZone: TimeZone = .current) -> String {
    guard let customAt else { return "Pick a date and time" }
    return formatStatusMoment(customAt, now, timeZone: timeZone)
}

/// `_pickCustom`'s starting moment: the chosen one while it is ahead, else an
/// hour from now.
public func userStatusPickerStart(_ customAt: Date?, now: Date) -> Date {
    if let customAt, customAt > now { return customAt }
    return now.addingTimeInterval(3600)
}

/// `_UserStatusControls`' row title: "🌴 Vacationing · until Sep 25", or
/// "Set a status" when there is no custom status.
public func userStatusSummary(_ status: UserStatus, now: Date, timeZone: TimeZone = .current) -> String {
    if !status.hasCustom { return "Set a status" }
    let line = [status.emoji, status.text].filter { !$0.isEmpty }.joined(separator: " ")
    return [line, status.untilLabel(now, timeZone: timeZone)].filter { !$0.isEmpty }.joined(separator: " · ")
}

/// `UserStatusBuilder._schedule`: how long until the painted status changes
/// on its own, or nil when it never will (or already has).
public func userStatusRepaintDelay(_ status: UserStatus, now: Date) -> TimeInterval? {
    guard let lapsesAt = status.visibleAt(now).lapsesAt else { return nil }
    return max(0, lapsesAt.timeIntervalSince(now))
}
