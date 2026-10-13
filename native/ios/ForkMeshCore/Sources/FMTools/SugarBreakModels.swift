import FMApi
import FMBase
import Foundation
import Observation

public struct SugarBreakAPI {
    public var call: ToolsApiCall

    public init(call: @escaping ToolsApiCall) {
        self.call = call
    }

    @MainActor
    public init(api: ApiService) {
        call = { method, suffix, body in try await api.sugarBreakApi(method, suffix, body) }
    }
}

public enum SugarDayStatus: String, Equatable, Sendable {
    case sugarFree = "sugar-free"
    case slip
    case open
    case upcoming
}

public struct SugarBreakDay: Equatable, Identifiable, Sendable {
    public var id: String { day }
    public let day: String
    public let number: Int
    public let status: SugarDayStatus
    public let skips: Int
    public let slips: Int
    public let healthGrams: Double?
    public let isToday: Bool
    public let caption: String
}

public struct SugarBreakEventRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let slip: Bool
    public let title: String
    public let time: String
}

public struct SugarBreakStat: Equatable, Identifiable, Sendable {
    public var id: String { label }
    public let label: String
    public let value: String
}

public struct SugarBreakHistoryRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let detail: String
    public let status: String
}

public struct SugarBreakLength: Equatable, Identifiable, Sendable {
    public var id: Int { days }
    public let days: Int
    public let label: String
}

@MainActor
@Observable
public final class SugarBreakModel {
    public static let title = "Sugar Break"
    public static let installTitle = "Take a break from sugar"
    public static let installMessage =
        "Sugar Break is private to your ForkMesh account. Pick how long, one month by default, press Start, and tap +1 every time you skip sugar. Your progress shows here, on the web, and on your desktop. With Apple Health turned on, the dietary sugar other apps logged appears beside each day; nothing is written to Health."
    public static let installButton = "Install Sugar Break"
    public static let unavailableTitle = "Sugar Break is unavailable"
    public static let startTitle = "Start a sugar break"
    public static let startMessage =
        "Choose how long. A day counts as sugar-free when you log at least one +1 and no sugar."
    public static let emptyToday = "Nothing logged today. Tap +1 each time you skip sugar."
    public static let defaultLengths = [7, 14, 21, 30, 60, 90]
    public static let defaultLength = 30

    @ObservationIgnored public let api: SugarBreakAPI
    @ObservationIgnored public let health: SugarBridge?
    @ObservationIgnored public let prefs: KeyValueStore
    @ObservationIgnored public let timeZone: TimeZone
    @ObservationIgnored private let now: () -> Date

    public private(set) var overview: JSONObject?
    public private(set) var installed = false
    public private(set) var loading = true
    public private(set) var error = ""
    public private(set) var busy = false
    public private(set) var pendingSkips = 0
    public private(set) var healthEnabled: Bool
    public private(set) var healthGrams: [String: Double] = [:]
    public var selectedLength: Int = SugarBreakModel.defaultLength
    public var notice: ToolsNotice?

    public init(
        api: SugarBreakAPI, health: SugarBridge?, prefs: KeyValueStore = InMemoryKeyValueStore(),
        timeZone: TimeZone = .current, now: (() -> Date)? = nil
    ) {
        self.api = api
        self.health = health
        self.prefs = prefs
        self.timeZone = timeZone
        self.now = now ?? { Date() }
        healthEnabled = prefs.bool(forKey: HealthPrefs.sugarBreak, or: false)
    }

    public var today: String { CaloriesText.isoDay(now(), timeZone: timeZone) }
    public var healthAvailable: Bool { health?.available == true }

    public func load() async {
        loading = true
        error = ""
        do {
            try await fetch()
            loading = false
            await readHealth()
        } catch let failure as OrgApiException where failure.code == "plugin_not_installed" {
            installed = false
            overview = nil
            loading = false
        } catch {
            loading = false
            self.error = message(error)
        }
    }

    private func fetch() async throws {
        let first = overview == nil
        let payload = try await api.call("GET", "?day=\(today)", nil)
        installed = payload["installed"]?.boolValue == true
        overview = installed ? payload : nil
        if installed && first {
            let preferred = ToolsJSON.int(profile["defaultLengthDays"])
            selectedLength = (1...365).contains(preferred) ? preferred : Self.defaultLength
        }
    }

    func message(_ error: Error) -> String {
        guard let failure = error as? OrgApiException else { return "Sugar Break is unavailable right now." }
        switch failure.code {
        case "no_active_break": return "Start a sugar break first."
        case "day_outside_break": return "That day is outside your sugar break."
        case "day_full": return "That day already holds as many entries as it can."
        case "break_not_found", "event_not_found": return "That was already removed."
        case "invalid_kind": return "Sugar Break did not understand that entry."
        default: return failure.message
        }
    }

    func show(_ text: String, error: Bool = false) { notice = ToolsNotice(text, isError: error) }

    public var showsSpinner: Bool { loading && overview == nil && error.isEmpty }
    public var showsError: Bool { !error.isEmpty && overview == nil }
    public var showsInstall: Bool { !loading && error.isEmpty && !installed }
    public var showsStart: Bool { installed && overview != nil && !isActive }

    public func install() async {
        do {
            _ = try await api.call("POST", "/install", [:])
            show("Sugar Break installed.")
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    public func startBreak(lengthDays: Int? = nil) async {
        let length = min(365, max(1, lengthDays ?? selectedLength))
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("POST", "/breaks", ["lengthDays": .int(Int64(length)), "startDay": .string(today)])
            show("Sugar break started: \(SugarBreakText.days(length)). Tap +1 each time you skip sugar.")
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    public func skip() async {
        pendingSkips += 1
        defer { pendingSkips -= 1 }
        do {
            _ = try await api.call("POST", "/events", ["kind": "skip", "day": .string(today)])
            try? await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func slip() async {
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("POST", "/events", ["kind": "slip", "day": .string(today)])
            show("Logged. One slip is one day; keep going tomorrow.")
            try? await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func undo(_ eventId: String) async {
        guard !eventId.isEmpty else { return }
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("DELETE", "/events/\(eventId)", nil)
            show("Removed.")
            try? await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func endEarly() async {
        guard isActive, !currentId.isEmpty else { return }
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("PATCH", "/breaks/\(currentId)", ["end": true])
            show("Sugar break ended with \(SugarBreakText.days(sugarFreeDays)) sugar-free.")
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    public func deleteBreak(_ id: String) async {
        guard !id.isEmpty else { return }
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("DELETE", "/breaks/\(id)", nil)
            show("Sugar break deleted.")
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    @discardableResult
    public func uninstall() async -> Bool {
        do {
            _ = try await api.call("POST", "/uninstall", [:])
            prefs.set(false, forKey: HealthPrefs.sugarBreak)
            healthEnabled = false
            healthGrams = [:]
            show("Sugar Break uninstalled from your account.")
            await load()
            return true
        } catch {
            show(message(error), error: true)
            return false
        }
    }

    public func enableHealth() async {
        guard let health, health.available else {
            show("Apple Health is not available on this device.", error: true)
            return
        }
        guard await health.authorizeSugar() else {
            show("ForkMesh could not ask Apple Health for Dietary Sugar. Allow it in Settings > Health > Data Access & Devices.", error: true)
            return
        }
        prefs.set(true, forKey: HealthPrefs.sugarBreak)
        healthEnabled = true
        await readHealth()
        show("Sugar Break now shows the sugar Apple Health recorded each day.")
    }

    public func disableHealth() {
        prefs.set(false, forKey: HealthPrefs.sugarBreak)
        healthEnabled = false
        healthGrams = [:]
        show("Apple Health is off for Sugar Break.")
    }

    public func readHealth() async {
        guard healthEnabled, let health, health.available, let range = healthRange else {
            healthGrams = [:]
            return
        }
        let read = await health.sugarGrams(from: range.from, to: range.to)
        healthGrams = read.filter { $0.value > 0 }
    }

    public var healthRange: (from: Date, to: Date)? {
        guard currentBreak != nil, let start = HealthDates.startOfDay(startDay, timeZone: timeZone),
            let last = HealthDates.startOfDay(endDay, timeZone: timeZone)
        else { return nil }
        let after = CaloriesText.calendar(timeZone).date(byAdding: .day, value: 1, to: last) ?? last
        let end = min(now(), after)
        return start < end ? (start, end) : nil
    }

    public var profile: JSONObject { ToolsJSON.object(overview?["profile"]) }
    public var summary: JSONObject { ToolsJSON.object(overview?["summary"]) }
    public var todayInfo: JSONObject { ToolsJSON.object(overview?["today"]) }
    public var currentBreak: JSONObject? { overview?["current"]?.objectValue }
    public var status: String { currentBreak?["status"]?.stringValue ?? "" }
    public var isActive: Bool { status == "active" }
    public var currentId: String { currentBreak?["id"]?.stringValue ?? "" }
    public var startDay: String { currentBreak?["startDay"]?.stringValue ?? "" }
    public var endDay: String { currentBreak?["endDay"]?.stringValue ?? "" }
    public var endedDay: String { currentBreak?["endedDay"]?.stringValue ?? "" }

    public var lengthDays: Int {
        let fromSummary = ToolsJSON.int(summary["lengthDays"])
        return fromSummary > 0 ? fromSummary : ToolsJSON.int(currentBreak?["lengthDays"])
    }

    public var dayNumber: Int { ToolsJSON.int(summary["dayNumber"]) }
    public var sugarFreeDays: Int {
        ToolsJSON.int(summary["sugarFreeDays"]) + (optimisticSugarFree ? 1 : 0)
    }
    public var skipsTotal: Int { ToolsJSON.int(summary["skips"]) + pendingSkips }
    public var slipsTotal: Int { ToolsJSON.int(summary["slips"]) }
    public var streak: Int { ToolsJSON.int(summary["streak"]) + (optimisticSugarFree ? 1 : 0) }
    public var bestStreak: Int { max(ToolsJSON.int(summary["bestStreak"]), streak) }
    public var remainingDays: Int { ToolsJSON.int(summary["remainingDays"]) }
    public var todaySkips: Int { ToolsJSON.int(todayInfo["skips"]) + pendingSkips }
    public var todaySlips: Int { ToolsJSON.int(todayInfo["slips"]) }

    private var optimisticSugarFree: Bool {
        pendingSkips > 0 && isActive && ToolsJSON.int(todayInfo["skips"]) == 0 && todaySlips == 0
    }

    public var progressRatio: Double {
        lengthDays > 0 ? min(1, Double(sugarFreeDays) / Double(lengthDays)) : 0
    }

    public var headline: String {
        guard currentBreak != nil else { return "" }
        switch status {
        case "active": return dayNumber > 0 ? "Day \(dayNumber) of \(lengthDays)" : "Starts \(dayLabel(startDay))"
        case "completed": return "Sugar break complete"
        default: return "Sugar break ended"
        }
    }

    public var progressLine: String { "\(sugarFreeDays) of \(SugarBreakText.days(lengthDays)) sugar-free" }

    public var remainingLine: String {
        guard currentBreak != nil else { return "" }
        switch status {
        case "active":
            if remainingDays == 0 { return "Last day today" }
            return "\(SugarBreakText.days(remainingDays)) left · ends \(dayLabel(endDay))"
        case "completed":
            return "\(SugarBreakText.monthDay(startDay)) – \(SugarBreakText.monthDay(endDay))"
        default:
            let ended = endedDay.isEmpty ? endDay : endedDay
            return "\(SugarBreakText.monthDay(startDay)) – ended early on \(SugarBreakText.monthDay(ended))"
        }
    }

    public var resultLine: String {
        guard currentBreak != nil, !isActive else { return "" }
        let lead = status == "completed" ? "You finished" : "You stopped"
        return "\(lead) with \(sugarFreeDays) of \(SugarBreakText.days(lengthDays)) sugar-free and +\(skipsTotal) in all. Best streak \(SugarBreakText.days(bestStreak))."
    }

    public var todayLine: String {
        if todaySlips > 0 { return "You had sugar today. Tomorrow is a fresh start." }
        if todaySkips > 0 { return "+\(todaySkips) today. Sugar-free so far." }
        return Self.emptyToday
    }

    public var healthTodayGrams: Double? {
        guard healthEnabled, let grams = healthGrams[today], grams > 0 else { return nil }
        return grams
    }

    public var healthTodayLine: String {
        guard let grams = healthTodayGrams else { return "" }
        return "Apple Health shows \(SugarBreakText.grams(grams)) of sugar today"
    }

    public var suggestsSlip: Bool { isActive && healthTodayGrams != nil && todaySlips == 0 }

    public var healthLine: String {
        if !healthAvailable { return "Apple Health is not available on this device." }
        if !healthEnabled {
            return "Turn on Apple Health to see the dietary sugar other apps logged beside each day. Nothing is written to Health."
        }
        return "Showing Dietary Sugar from Apple Health beside each day. Nothing is written to Health."
    }

    public var stats: [SugarBreakStat] {
        [
            SugarBreakStat(label: "+1 total", value: String(skipsTotal)),
            SugarBreakStat(label: "Sugar-free days", value: "\(sugarFreeDays) of \(lengthDays)"),
            SugarBreakStat(label: "Streak", value: SugarBreakText.days(streak)),
            SugarBreakStat(label: "Best streak", value: SugarBreakText.days(bestStreak)),
            SugarBreakStat(label: "Days left", value: String(remainingDays)),
        ]
    }

    public var days: [SugarBreakDay] {
        let current = today
        return ToolsJSON.maps(overview?["days"]).map { entry in
            let day = entry["day"]?.stringValue ?? ""
            let isToday = day == current
            let slips = ToolsJSON.int(entry["slips"])
            var skips = ToolsJSON.int(entry["skips"])
            var status = SugarDayStatus(rawValue: entry["status"]?.stringValue ?? "") ?? .open
            if isToday && pendingSkips > 0 {
                skips += pendingSkips
                if status == .open && slips == 0 { status = .sugarFree }
            }
            let grams = healthEnabled ? healthGrams[day] : nil
            return SugarBreakDay(
                day: day, number: SugarBreakText.dayOfMonth(day), status: status, skips: skips, slips: slips,
                healthGrams: grams, isToday: isToday,
                caption: Self.caption(
                    label: isToday ? "Today" : dayLabel(day), status: status, skips: skips, grams: grams))
        }
    }

    static func caption(label: String, status: SugarDayStatus, skips: Int, grams: Double?) -> String {
        var parts = [label]
        switch status {
        case .sugarFree: parts.append("sugar-free, +\(skips)")
        case .slip: parts.append("had sugar")
        case .open: parts.append("nothing logged")
        case .upcoming: parts.append("upcoming")
        }
        if let grams { parts.append("Apple Health \(SugarBreakText.grams(grams)) sugar") }
        return parts.joined(separator: ", ")
    }

    public var events: [SugarBreakEventRow] {
        ToolsJSON.maps(overview?["events"]).reversed().map { event in
            let slip = event["kind"]?.stringValue == "slip"
            var at = ToolsJSON.epoch(event["at"])
            if at <= 0 { at = ToolsJSON.epoch(event["loggedAt"]) }
            let time = at > 0 ? StepsText.clock(Date(timeIntervalSince1970: TimeInterval(at) / 1000), timeZone: timeZone) : ""
            return SugarBreakEventRow(
                id: event["id"]?.stringValue ?? "", slip: slip, title: slip ? "Had sugar" : "+1 Skipped sugar", time: time)
        }
    }

    public var history: [SugarBreakHistoryRow] {
        ToolsJSON.maps(overview?["history"]).map { past in
            let totals = ToolsJSON.object(past["summary"])
            let length = ToolsJSON.int(past["lengthDays"])
            let start = past["startDay"]?.stringValue ?? ""
            let end = past["endDay"]?.stringValue ?? ""
            let state = past["status"]?.stringValue ?? ""
            let label: String
            switch state {
            case "completed": label = "Completed"
            case "ended": label = "Ended early"
            default: label = "Active"
            }
            return SugarBreakHistoryRow(
                id: past["id"]?.stringValue ?? "",
                title: "\(SugarBreakText.monthDay(start)) – \(SugarBreakText.monthDay(end))",
                detail: "\(ToolsJSON.int(totals["sugarFreeDays"])) of \(SugarBreakText.days(length)) sugar-free · +\(ToolsJSON.int(totals["skips"])) · best streak \(SugarBreakText.days(ToolsJSON.int(totals["bestStreak"])))",
                status: label)
        }
    }

    public var lengths: [SugarBreakLength] {
        var values = (overview?["lengths"]?.arrayValue ?? []).compactMap(\.intValue).filter { (1...365).contains($0) }
        if values.isEmpty { values = Self.defaultLengths }
        if !values.contains(selectedLength) {
            values.append(selectedLength)
            values.sort()
        }
        return values.map { SugarBreakLength(days: $0, label: SugarBreakText.length($0)) }
    }

    public func dayLabel(_ day: String) -> String {
        CaloriesText.dayLabel(day, timeZone: timeZone, now: now())
    }
}

public enum SugarBreakText {
    static let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

    public static func length(_ days: Int) -> String {
        switch days {
        case 7: return "1 week (7 days)"
        case 14, 21: return "\(days / 7) weeks (\(days) days)"
        case 30: return "1 month (30 days)"
        case 60, 90: return "\(days / 30) months (\(days) days)"
        default: return Self.days(days)
        }
    }

    public static func days(_ count: Int) -> String { count == 1 ? "1 day" : "\(count) days" }

    public static func grams(_ value: Double) -> String {
        if value >= 10 { return "\(Int(value.rounded())) g" }
        return "\(CaloriesText.grams((value * 10).rounded() / 10)) g"
    }

    public static func dayOfMonth(_ day: String) -> Int {
        let parts = day.split(separator: "-")
        guard parts.count == 3 else { return 0 }
        return Int(parts[2]) ?? 0
    }

    public static func monthDay(_ day: String) -> String {
        let parts = day.split(separator: "-")
        guard parts.count == 3, let month = Int(parts[1]), let date = Int(parts[2]), (1...12).contains(month) else {
            return day
        }
        return "\(months[month - 1]) \(date)"
    }
}
