// View model of the Ping settings screen, after native/android
// core/.../account/PingSettingsLogic.kt: load, the optimistic save (show the patch
// now, then whatever the Worker stored; put it back on a refusal), and the patches every control
// on the page sends, plus the page's time and day labels.

import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class PingSettingsModel {
    /// The longest one-off pause the page offers (`_maxPause`).
    public static let maxPause: TimeInterval = 30 * 24 * 60 * 60
    public static let weekdays = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]

    public private(set) var settings: PingSettings?
    public private(set) var loading = false
    /// Why the settings could not be read; empty when they were (or before the first load).
    public private(set) var error = ""
    /// Why the last change was not saved, shown inline in red; the next change clears it.
    public var saveError: String?

    @ObservationIgnored private let loadSettings: @MainActor (String) async throws -> PingSettings
    @ObservationIgnored private let saveSettings: @MainActor (String, JSONObject) async throws -> PingSettings
    @ObservationIgnored private let node: @MainActor () -> String
    @ObservationIgnored private let now: @MainActor () -> Date
    @ObservationIgnored private var saveSeq = 0
    @ObservationIgnored private var started = false

    public init(
        load: @escaping @MainActor (String) async throws -> PingSettings,
        save: @escaping @MainActor (String, JSONObject) async throws -> PingSettings,
        node: @escaping @MainActor () -> String,
        now: @escaping @MainActor () -> Date = { Date() }
    ) {
        self.loadSettings = load
        self.saveSettings = save
        self.node = node
        self.now = now
    }

    public convenience init(api: ApiService, node: @escaping @MainActor () -> String) {
        self.init(
            load: { (account: String) in try await api.pingSettings(node: account) },
            save: { (account: String, patch: JSONObject) in try await api.savePingSettings(node: account, patch: patch) },
            node: node)
    }

    private var account: String { node().dartTrimmed.lowercased() }

    /// First appearance (`didChangeDependencies`).
    public func start() async {
        if started { return }
        started = true
        await load()
    }

    /// `_load`, also the error card's Retry.
    public func load() async {
        loading = true
        error = ""
        do {
            let loaded = try await loadSettings(account)
            settings = loaded
            loading = false
        } catch {
            loading = false
            self.error = "\(error)"
        }
    }

    /// `_save`: optimistic — show `patch` now, then whatever the Worker stored. A refusal puts the
    /// page back, unless a later save has already moved it on.
    public func save(_ patch: JSONObject) async {
        guard let before = settings else { return }
        saveSeq += 1
        let seq = saveSeq
        saveError = nil
        settings = before.applyPatch(patch)
        do {
            let saved = try await saveSettings(account, patch)
            if seq != saveSeq { return }
            settings = saved
        } catch {
            if seq == saveSeq { settings = before }
            saveError = "Could not save ping settings: \(error)"
        }
    }

    // MARK: The page's controls

    public func setCodePushes(_ on: Bool) async { await save(["codePushes": .bool(on)]) }

    public func setPingEnabled(_ kind: String, _ on: Bool) async {
        await save(["pingMutedKinds": .object([kind: .bool(on)])])
    }

    public func setEmailEnabled(_ kind: String, _ on: Bool) async {
        await save(["notificationPreferences": .object([kind: .bool(on)])])
    }

    public func setEmailNotifications(_ on: Bool) async { await save(["emailNotifications": .bool(on)]) }

    public func setDelivery(_ delivery: String) async {
        await save(["notificationDelivery": .string(delivery == "instant" ? "instant" : "digest")])
    }

    /// The pause chips: 1 hour, 1 day, 1 week (`pauseFor`).
    public func pause(for duration: TimeInterval) async {
        let until = now().addingTimeInterval(min(duration, PingSettingsModel.maxPause))
        await save(["notificationPauseUntil": .int(until.epochMillis)])
    }

    /// "Custom…": a picked moment, capped at the longest pause. False (and nothing saved) for a
    /// moment that is not in the future.
    @discardableResult
    public func pause(until picked: Date) async -> Bool {
        let latest = now().addingTimeInterval(PingSettingsModel.maxPause)
        let until = picked > latest ? latest : picked
        if until <= now() {
            saveError = "Pick a time in the future."
            return false
        }
        await save(["notificationPauseUntil": .int(until.epochMillis)])
        return true
    }

    public func resume() async { await save(["notificationPauseUntil": .int(0)]) }

    public func setQuietEnabled(_ on: Bool) async { await saveQuiet(["enabled": .bool(on)]) }

    public func setQuietStart(_ minutes: Int) async { await saveQuiet(["start": .int(Int64(minutes))]) }

    public func setQuietEnd(_ minutes: Int) async { await saveQuiet(["end": .int(Int64(minutes))]) }

    /// A weekday chip: `day` (Monday = 0) on or off.
    public func setQuietDay(_ day: Int, _ on: Bool) async {
        guard let quiet = settings?.quietHours else { return }
        var days = Set(quiet.days)
        if on {
            days.insert(day)
        } else {
            days.remove(day)
        }
        await saveQuiet(["days": .array(days.sorted().map { .int(Int64($0)) })])
    }

    public func setQuietTimezone(_ zone: String) async {
        let clean = zone.dartTrimmed
        guard let current = settings?.quietHours.timezone, !clean.isEmpty, clean != current else { return }
        await saveQuiet(["timezone": .string(clean)])
    }

    /// One administrator monitor switch: `field` is `pings`, `emails` or `continual`.
    public func setMonitor(_ id: String, _ field: String, _ on: Bool) async {
        await save(["statusMonitorAlerts": .object([id: .object([field: .bool(on)])])])
    }

    private func saveQuiet(_ patch: JSONObject) async {
        await save(["notificationQuietHours": .object(patch)])
    }

    // MARK: Labels

    public var pauseLabel: String {
        guard let settings = settings, settings.isPaused(now()) else {
            return "Pings are delivered as they happen"
        }
        return "Paused until \(PingSettingsModel.when(settings.pauseUntil, now: now()))"
    }

    public var quietLabel: String {
        guard let settings = settings else { return "" }
        if settings.isQuiet(now()) {
            return "Quiet until \(PingSettingsModel.when(settings.quietUntil, now: now()))"
        }
        let quiet = settings.quietHours
        return "Every \(PingSettingsModel.daysLabel(quiet.days)), "
            + "\(PingSettingsModel.hhmm(quiet.start))–\(PingSettingsModel.hhmm(quiet.end))"
    }

    public var deliveryNote: String {
        settings?.delivery == "instant"
            ? "One email per ping, as it happens."
            : "Unread pings are rolled into one email."
    }

    private static func two(_ value: Int) -> String { value < 10 ? "0\(value)" : "\(value)" }

    /// `_hhmm`: minutes after midnight as `07:30`.
    public static func hhmm(_ minutes: Int) -> String { "\(two(minutes / 60)):\(two(minutes % 60))" }

    /// `_daysLabel`: "day", "weekday", "weekend", "no day", or the named days.
    public static func daysLabel(_ days: [Int]) -> String {
        if days.count == 7 { return "day" }
        if days.isEmpty { return "no day" }
        let set = Set(days)
        if set.count == 5 && set.isSuperset(of: [0, 1, 2, 3, 4]) { return "weekday" }
        if set.count == 2 && set.isSuperset(of: [5, 6]) { return "weekend" }
        return days.filter { $0 >= 0 && $0 < 7 }.map { weekdays[$0] }.joined(separator: ", ")
    }

    /// `_when`: a moment as `14:05`, `tomorrow 14:05`, `Wed 14:05` or `3 Oct 14:05`, in the
    /// phone's own calendar and zone.
    public static func when(_ millis: Int64, now: Date, calendar: Calendar = Calendar.current) -> String {
        let time = Date(timeIntervalSince1970: Double(millis) / 1000)
        let parts = calendar.dateComponents([.hour, .minute, .day, .month, .weekday], from: time)
        let clock = "\(two(parts.hour ?? 0)):\(two(parts.minute ?? 0))"
        let today = calendar.startOfDay(for: now)
        let day = calendar.startOfDay(for: time)
        let days = calendar.dateComponents([.day], from: today, to: day).day ?? 0
        if days == 0 { return clock }
        if days == 1 { return "tomorrow \(clock)" }
        if days > 1 && days < 7 {
            // Calendar weekday: Sunday = 1 … Saturday = 7; the page's list starts on Monday.
            let index = ((parts.weekday ?? 2) + 5) % 7
            return "\(weekdays[index]) \(clock)"
        }
        let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        let month = max(1, min(12, parts.month ?? 1))
        return "\(parts.day ?? 1) \(months[month - 1]) \(clock)"
    }
}
