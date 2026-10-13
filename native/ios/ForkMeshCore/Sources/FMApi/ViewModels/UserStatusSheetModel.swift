// View model of the user status sheet and of the Settings screen's Away
// switch. Refusals are shown inside the sheet with the fields still filled
// in, so nothing typed is lost.

import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class UserStatusSheetModel {
    public var emoji: String
    public var text: String
    public private(set) var clearAfter: ClearAfter
    public private(set) var customAt: Date?
    /// The refusal to show, or "".
    public var problem = ""

    public let service: UserStatusService
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored private let timeZone: TimeZone

    public init(service: UserStatusService, now: @escaping () -> Date = Date.init, timeZone: TimeZone = .current) {
        self.service = service
        self.now = now
        self.timeZone = timeZone
        let status = service.status
        emoji = status.emoji
        text = status.text
        clearAfter = initialClearAfter(status)
        customAt = initialCustomAt(status)
    }

    public var busy: Bool { service.busy }
    public var away: Bool { service.status.away }
    public var canClear: Bool { !busy && service.status.hasCustom }
    public var saveLabel: String { busy ? "Saving…" : "Save" }
    public var customLabel: String { userStatusCustomLabel(customAt, now: now(), timeZone: timeZone) }
    /// The date picker's starting moment.
    public var pickerStart: Date { userStatusPickerStart(customAt, now: now()) }
    /// The picker's range: today through a year from now.
    public var pickerRange: ClosedRange<Date> {
        let current = now()
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        return calendar.startOfDay(for: current)...current.addingTimeInterval(365 * 86_400)
    }

    public func isSelected(_ quick: String) -> Bool { emoji.dartTrimmed == quick }

    public func pickEmoji(_ quick: String) {
        emoji = quick
        problem = ""
    }

    public func applyPreset(_ preset: StatusPreset) {
        emoji = preset.emoji
        text = preset.text
        clearAfter = preset.clearAfter
        problem = ""
    }

    /// A "Clear after" choice other than custom (custom goes through `setCustom`).
    public func chooseClearAfter(_ choice: ClearAfter) {
        clearAfter = choice
        problem = ""
    }

    /// The picked custom moment.
    public func setCustom(_ moment: Date) {
        customAt = moment
        clearAfter = .custom
        problem = ""
    }

    /// Save. True when the sheet should close.
    public func save() async -> Bool {
        switch userStatusSaveAction(
            emoji: emoji, text: text, clearAfter: clearAfter, customAt: customAt, now: now(), timeZone: timeZone)
        {
        case .problem(let message):
            problem = message
            return false
        case .clear:
            return await run { try await self.service.clearCustom() }
        case .setCustom(let cleanEmoji, let cleanText, let expiresAt):
            return await run { try await self.service.setCustom(emoji: cleanEmoji, text: cleanText, expiresAt: expiresAt) }
        }
    }

    /// "Clear status". True when the sheet should close.
    public func clear() async -> Bool {
        await run { try await self.service.clearCustom() }
    }

    /// The Away switch; the sheet stays open.
    public func setAway(_ away: Bool) async {
        _ = await run { try await self.service.setAway(away) }
    }

    private func run(_ action: () async throws -> Void) async -> Bool {
        problem = ""
        do {
            try await action()
            return true
        } catch let error as UserStatusException {
            problem = error.message
            return false
        } catch {
            problem = dartErrorText(error)
            return false
        }
    }
}

/// Settings' Away switch outside the sheet: a refusal is an inline notice sentence.
@MainActor
public func setAwayFromSettings(_ service: UserStatusService, _ away: Bool) async -> String? {
    do {
        try await service.setAway(away)
        return nil
    } catch let error as UserStatusException {
        return error.message
    } catch {
        return dartErrorText(error)
    }
}
