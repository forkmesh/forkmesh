import FMApi
import FMBase
import Foundation
import Observation

@MainActor
public protocol SleepBridge: AnyObject {
    var available: Bool { get }
    var motionAvailable: Bool { get }
    func authorizeSleep(write: Bool) async -> Bool
    func restHistory(from: Date, to: Date) async -> [SleepRestSample]?
    func sleepSamples(from: Date, to: Date) async -> [HealthSleepSample]?
    func saveSleep(_ writes: [HealthSleepWrite]) async throws
    func deleteSleep(entryIds: [String]) async throws
}

public struct SleepAPI {
    public var call: ToolsApiCall

    public init(call: @escaping ToolsApiCall) {
        self.call = call
    }

    @MainActor
    public init(api: ApiService) {
        call = { method, suffix, body in try await api.sleepApi(method, suffix, body) }
    }
}

public enum SleepHealthSync {
    public static let maxNightsPerRequest = 62
    public static let motionHistory: TimeInterval = 7 * 86400 - 600

    @MainActor
    public static func schedule(_ prefs: KeyValueStore) -> SleepSchedule {
        SleepSchedule(
            bedtime: prefs.string(forKey: HealthPrefs.sleepBedtime, or: ""),
            wakeTime: prefs.string(forKey: HealthPrefs.sleepWakeTime, or: ""))
    }

    public static func nightKeys(ending now: Date, count: Int, schedule: SleepSchedule, timeZone: TimeZone) -> [String] {
        HealthDates.days(ending: schedule.nightKey(now, timeZone: timeZone), count: max(1, count), timeZone: timeZone)
    }

    @MainActor
    public static func push(_ nights: [JSONObject], source: String, timeZone: TimeZone, now: Date, call: ToolsApiCall) async throws -> Int {
        var synced = 0
        var index = 0
        while index < nights.count {
            let chunk = Array(nights[index..<min(nights.count, index + maxNightsPerRequest)])
            let reply = try await call("POST", "/sync", [
                "source": .string(source), "device": .string(source == "phone" ? "iphone" : "health"),
                "tzOffsetMinutes": .int(Int64(CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now))),
                "nights": .array(chunk.map { .object($0) }),
            ])
            synced += ToolsJSON.int(reply["synced"])
            index += maxNightsPerRequest
        }
        return synced
    }

    @MainActor
    public static func collect(
        bridge: SleepBridge, prefs: KeyValueStore, now: Date, nights: Int, timeZone: TimeZone
    ) async -> (phone: [JSONObject], health: [JSONObject]?, detected: [SleepNight], healthNights: [SleepNight], samples: [HealthSleepSample]) {
        let plan: SleepSchedule = Self.schedule(prefs)
        let keys: [String] = nightKeys(ending: now, count: nights, schedule: plan, timeZone: timeZone)
        var windows: [(String, SleepInterval)] = []
        for key in keys {
            if let window = plan.window(night: key, timeZone: timeZone) { windows.append((key, window)) }
        }
        guard let first = windows.first?.1.start else { return ([], nil, [], [], []) }
        var samples: [HealthSleepSample] = []
        var healthBodies: [JSONObject]?
        var healthNights: [SleepNight] = []
        if bridge.available, let read = await bridge.sleepSamples(from: first, to: now), !read.isEmpty {
            samples = read
            healthNights = SleepDetector.healthNights(read, schedule: plan, timeZone: timeZone)
                .filter { keys.contains($0.night) }
            let found = Set(healthNights.map(\.night))
            healthBodies = healthNights.map(\.body) + keys.filter { !found.contains($0) }.map { SleepNight.cleared($0) }
        }
        var phone: [JSONObject] = []
        var detected: [SleepNight] = []
        let motionStart = max(first, now.addingTimeInterval(-motionHistory))
        if bridge.motionAvailable, let rest = await bridge.restHistory(from: motionStart, to: now), !rest.isEmpty {
            let usage = SleepUsageLog.intervals(prefs, now: now)
            for (key, window) in windows where window.start >= motionStart && window.start < now {
                if let night = SleepDetector.detect(
                    night: key, rest: rest, usage: usage, schedule: plan, timeZone: timeZone, now: now) {
                    detected.append(night)
                    phone.append(night.body)
                } else {
                    phone.append(SleepNight.cleared(key))
                }
            }
        }
        return (phone, healthBodies, detected, healthNights, samples)
    }

    @MainActor
    public static func writeBack(
        bridge: SleepBridge, detected: [SleepNight], healthNights: [SleepNight], samples: [HealthSleepSample]
    ) async {
        let taken = Set(healthNights.map(\.night))
        let desired = SleepDetector.healthWrites(detected.filter { !taken.contains($0.night) && $0.confidence != "low" })
        let plan = SleepDetector.reconcile(desired: desired, existing: samples)
        do {
            if !plan.delete.isEmpty { try await bridge.deleteSleep(entryIds: plan.delete) }
            if !plan.save.isEmpty { try await bridge.saveSleep(plan.save) }
        } catch {
            return
        }
    }

    @MainActor
    public static func runIfDue(
        bridge: SleepBridge, prefs: KeyValueStore, call: ToolsApiCall, now: Date = Date(), nights: Int = 7,
        timeZone: TimeZone = .current, force: Bool = false
    ) async -> Int {
        guard prefs.bool(forKey: HealthPrefs.sleep, or: false) else { return 0 }
        let last = prefs.double(forKey: HealthPrefs.sleepSyncedAt, or: 0)
        if !force && now.timeIntervalSince1970 - last < HealthPrefs.syncInterval { return 0 }
        let collected = await collect(bridge: bridge, prefs: prefs, now: now, nights: nights, timeZone: timeZone)
        guard !collected.phone.isEmpty || !(collected.health ?? []).isEmpty else { return 0 }
        var synced = 0
        do {
            if !collected.phone.isEmpty {
                synced += try await push(collected.phone, source: "phone", timeZone: timeZone, now: now, call: call)
            }
            if let health = collected.health, !health.isEmpty {
                synced += try await push(health, source: "health", timeZone: timeZone, now: now, call: call)
            }
        } catch {
            return synced
        }
        prefs.set(now.timeIntervalSince1970, forKey: HealthPrefs.sleepSyncedAt)
        if prefs.bool(forKey: HealthPrefs.sleepWriteHealth, or: false) && bridge.available {
            await writeBack(
                bridge: bridge, detected: collected.detected, healthNights: collected.healthNights,
                samples: collected.samples)
        }
        return synced
    }
}

public struct SleepBar: Equatable, Identifiable, Sendable {
    public let id: String
    public let label: String
    public let minutes: Int
    public let ratio: Double
    public let metGoal: Bool
    public let caption: String
}

public struct SleepStat: Equatable, Identifiable, Sendable {
    public var id: String { label }
    public let label: String
    public let value: String
}

@MainActor
@Observable
public final class SleepModel {
    public static let title = "Sleep"
    public static let installTitle = "Track your sleep"
    public static let installMessage =
        "Sleep is private to your ForkMesh account. Install it, turn on sleep tracking, and leave your iPhone by your bed at night: the stretch it lies still and unused counts as sleep. When your Apple Watch records sleep in Apple Health, that is used instead. Every night is encrypted at rest with a key minted for your account alone."
    public static let installButton = "Install Sleep"
    public static let unavailableTitle = "Sleep is unavailable"
    public static let trackingOffMessage =
        "Turn on sleep tracking. ForkMesh reads when your phone lay still from the motion sensor and your sleep from Apple Health, and sends one entry per night to your account."
    public static let unavailableMessage = "This device has no motion sensor or Apple Health."

    @ObservationIgnored public let api: SleepAPI
    @ObservationIgnored public let bridge: SleepBridge?
    @ObservationIgnored public let prefs: KeyValueStore
    @ObservationIgnored public let timeZone: TimeZone
    @ObservationIgnored private let now: () -> Date

    public private(set) var overview: JSONObject?
    public private(set) var installed = false
    public private(set) var loading = true
    public private(set) var syncing = false
    public private(set) var error = ""
    public private(set) var trackingEnabled: Bool
    public private(set) var writesHealth: Bool
    public var notice: ToolsNotice?

    public init(
        api: SleepAPI, bridge: SleepBridge?, prefs: KeyValueStore = InMemoryKeyValueStore(),
        timeZone: TimeZone = .current, now: (() -> Date)? = nil
    ) {
        self.api = api
        self.bridge = bridge
        self.prefs = prefs
        self.timeZone = timeZone
        self.now = now ?? { Date() }
        trackingEnabled = prefs.bool(forKey: HealthPrefs.sleep, or: false)
        writesHealth = prefs.bool(forKey: HealthPrefs.sleepWriteHealth, or: false)
    }

    public var currentNight: String {
        SleepHealthSync.schedule(prefs).nightKey(now(), timeZone: timeZone)
    }

    public func load(syncIfDue: Bool = true) async {
        loading = true
        error = ""
        do {
            try await fetch()
            loading = false
            if syncIfDue && installed && trackingEnabled, let bridge {
                let synced = await SleepHealthSync.runIfDue(
                    bridge: bridge, prefs: prefs, call: api.call, now: now(), nights: 7, timeZone: timeZone)
                if synced > 0 { try? await fetch() }
            }
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
        let payload = try await api.call("GET", "?night=\(currentNight)", nil)
        installed = payload["installed"]?.boolValue == true
        overview = installed ? payload : nil
        if installed {
            prefs.set(profile["bedtime"]?.stringValue ?? "", forKey: HealthPrefs.sleepBedtime)
            prefs.set(profile["wakeTime"]?.stringValue ?? "", forKey: HealthPrefs.sleepWakeTime)
        }
    }

    func message(_ error: Error) -> String {
        (error as? OrgApiException)?.message ?? "Sleep is unavailable right now."
    }

    func show(_ text: String, error: Bool = false) { notice = ToolsNotice(text, isError: error) }

    public var showsSpinner: Bool { loading && overview == nil && error.isEmpty }
    public var showsError: Bool { !error.isEmpty && overview == nil }
    public var showsInstall: Bool { !loading && error.isEmpty && !installed }
    public var trackingAvailable: Bool { bridge?.available == true || bridge?.motionAvailable == true }
    public var healthAvailable: Bool { bridge?.available == true }

    public func install() async {
        do {
            _ = try await api.call("POST", "/install", [
                "tzOffsetMinutes": .int(Int64(CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now()))),
            ])
            show("Sleep installed.")
            await load(syncIfDue: false)
            if trackingAvailable && !trackingEnabled { await enableTracking() }
        } catch {
            show(message(error), error: true)
        }
    }

    public func enableTracking() async {
        guard let bridge, trackingAvailable else {
            show(Self.unavailableMessage, error: true)
            return
        }
        _ = await bridge.authorizeSleep(write: writesHealth)
        prefs.set(true, forKey: HealthPrefs.sleep)
        trackingEnabled = true
        SleepUsageLog.begin(prefs, now: now())
        await syncNow()
    }

    public func disableTracking() {
        prefs.set(false, forKey: HealthPrefs.sleep)
        SleepUsageLog.end(prefs, now: now())
        trackingEnabled = false
        show("Sleep tracking is off. The nights already synced stay on your account.")
    }

    public func setWritesHealth(_ on: Bool) async {
        guard let bridge, bridge.available else {
            show("Apple Health is not available on this device.", error: true)
            return
        }
        if on {
            guard await bridge.authorizeSleep(write: true) else {
                show("ForkMesh was not allowed to write sleep. Allow it in Settings > Health > Data Access & Devices.", error: true)
                return
            }
        }
        prefs.set(on, forKey: HealthPrefs.sleepWriteHealth)
        writesHealth = on
        if on { await syncNow() }
    }

    public func syncNow(nights: Int = 7) async {
        guard let bridge, trackingEnabled, installed else { return }
        syncing = true
        let synced = await SleepHealthSync.runIfDue(
            bridge: bridge, prefs: prefs, call: api.call, now: now(), nights: nights, timeZone: timeZone, force: true)
        syncing = false
        if synced > 0 {
            try? await fetch()
        } else if lastMinutes == 0 {
            show("No sleep found yet. Leave your phone by your bed tonight, or check that ForkMesh may read Sleep and Motion & Fitness.")
        }
    }

    public func saveGoal(minutes: Int, bedtime: String, wakeTime: String) async {
        do {
            _ = try await api.call("PUT", "/profile", [
                "goalMinutes": .int(Int64(max(0, minutes))), "bedtime": .string(bedtime), "wakeTime": .string(wakeTime),
                "tzOffsetMinutes": .int(Int64(CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now()))),
            ])
            show("Goal and schedule saved.")
            try await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func addNight(night: String, bedAt: Date, wakeAt: Date, awakeMinutes: Int) async {
        do {
            _ = try await api.call("PUT", "/nights/\(night)", [
                "bedAt": .int(HealthDates.epochMs(bedAt)), "wakeAt": .int(HealthDates.epochMs(wakeAt)),
                "awakeMinutes": .int(Int64(max(0, awakeMinutes))),
            ])
            show("Saved the night you added for \(nightLabel(night)).")
            try await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func removeAddedNight(_ night: String) async {
        do {
            _ = try await api.call("DELETE", "/nights/\(night)?source=manual", nil)
            show("Removed the night you added for \(nightLabel(night)).")
            try await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func exportData() async -> JSONObject? {
        do {
            return try await api.call("GET", "/export", nil)
        } catch {
            show(message(error), error: true)
            return nil
        }
    }

    @discardableResult
    public func uninstall() async -> Bool {
        do {
            _ = try await api.call("POST", "/uninstall", [:])
            prefs.set(false, forKey: HealthPrefs.sleep)
            trackingEnabled = false
            show("Sleep uninstalled from your account.")
            await load(syncIfDue: false)
            return true
        } catch {
            show(message(error), error: true)
            return false
        }
    }

    public var profile: JSONObject { ToolsJSON.object(overview?["profile"]) }
    public var summary: JSONObject { ToolsJSON.object(overview?["summary"]) }
    public var lastNight: JSONObject { ToolsJSON.object(overview?["lastNight"]) }
    public var goal: Int { ToolsJSON.int(profile["goalMinutes"]) }
    public var bedtime: String { profile["bedtime"]?.stringValue ?? "23:00" }
    public var wakeTime: String { profile["wakeTime"]?.stringValue ?? "07:00" }
    public var lastMinutes: Int { ToolsJSON.int(lastNight["asleepMinutes"]) }

    public var durationText: String { lastMinutes > 0 ? SleepText.duration(lastMinutes) : "—" }
    public var ringRatio: Double { goal > 0 ? min(1, Double(lastMinutes) / Double(goal)) : 0 }
    public var goalMet: Bool { goal > 0 && lastMinutes >= goal }

    public var goalLine: String {
        guard goal > 0 else { return "No sleep goal" }
        return "of \(SleepText.duration(goal))"
    }

    public var detailLine: String {
        let night = lastNight
        guard lastMinutes > 0 else { return "Nothing for last night yet." }
        var parts = ["\(clock(night["bedAt"])) – \(clock(night["wakeAt"]))"]
        let awake = ToolsJSON.int(night["awakeMinutes"])
        if awake > 0 { parts.append("\(SleepText.duration(awake)) awake") }
        let interruptions = ToolsJSON.int(night["interruptions"])
        if interruptions > 0 { parts.append(interruptions == 1 ? "1 interruption" : "\(interruptions) interruptions") }
        let deep = ToolsJSON.int(night["deepMinutes"])
        if deep > 0 { parts.append("\(SleepText.duration(deep)) deep") }
        let rem = ToolsJSON.int(night["remMinutes"])
        if rem > 0 { parts.append("\(SleepText.duration(rem)) REM") }
        if goal > 0 { parts.append(goalMet ? "Goal reached" : "\(SleepText.duration(goal - lastMinutes)) short") }
        return parts.joined(separator: " · ")
    }

    public var sourceLine: String {
        if !trackingAvailable { return Self.unavailableMessage }
        if !trackingEnabled { return Self.trackingOffMessage }
        let source = lastNight["source"]?.stringValue ?? ""
        guard !source.isEmpty else {
            return "Tracking. Leave your phone by your bed; last night fills in when you open ForkMesh in the morning."
        }
        let labels = ToolsJSON.object(overview?["sourceLabels"])
        var parts = [labels[source]?.stringValue ?? source]
        let confidence = lastNight["confidence"]?.stringValue ?? ""
        if source == "phone" && !confidence.isEmpty { parts.append("\(confidence) confidence") }
        let reading = ToolsJSON.object(ToolsJSON.object(lastNight["sources"])[source])
        let synced = ToolsJSON.epoch(reading["updatedAt"])
        if synced > 0 {
            parts.append("synced \(StepsText.clock(Date(timeIntervalSince1970: TimeInterval(synced) / 1000), timeZone: timeZone))")
        }
        return parts.joined(separator: " · ")
    }

    public var stats: [SleepStat] {
        let weekNights = ToolsJSON.int(summary["weekNights"])
        let logged = ToolsJSON.int(summary["loggedNights"])
        let streak = ToolsJSON.int(summary["streak"])
        let spread = ToolsJSON.int(summary["bedtimeSpreadMinutes"])
        var averageBed: String = SleepText.clock(minutes: ToolsJSON.int(summary["averageBedtimeMinutes"] ?? .int(-1)))
        if spread > 0 && averageBed != "—" { averageBed += " ± \(SleepText.duration(spread))" }
        return [
            SleepStat(label: "Last 7 nights", value: weekNights > 0 ? SleepText.duration(ToolsJSON.int(summary["weekAverageMinutes"])) : "—"),
            SleepStat(label: "Sleep debt", value: goal > 0 ? SleepText.duration(ToolsJSON.int(summary["debtMinutes"])) : "—"),
            SleepStat(label: "30-night average", value: logged > 0 ? SleepText.duration(ToolsJSON.int(summary["averageMinutes"])) : "—"),
            SleepStat(label: "Goal streak", value: streak == 1 ? "1 night" : "\(streak) nights"),
            SleepStat(label: "Average bedtime", value: averageBed),
            SleepStat(label: "Average wake", value: SleepText.clock(minutes: ToolsJSON.int(summary["averageWakeMinutes"] ?? .int(-1)))),
        ]
    }

    private var peak: Double {
        var peak = Double(max(600, Int(Double(goal) * 1.15)))
        for night in ToolsJSON.maps(overview?["nights"]) { peak = max(peak, CaloriesText.num(night["asleepMinutes"]) * 1.05) }
        return peak
    }

    public var bars: [SleepBar] {
        let nights = ToolsJSON.maps(overview?["nights"])
        let top = peak
        return nights.enumerated().map { index, night in
            let id = night["night"]?.stringValue ?? "\(index)"
            let minutes = ToolsJSON.int(night["asleepMinutes"])
            return SleepBar(
                id: id, label: index % 5 == 0 || index == nights.count - 1 ? StepsText.shortDay(id) : "", minutes: minutes,
                ratio: top > 0 ? Double(minutes) / top : 0, metGoal: goal > 0 && minutes >= goal,
                caption: "\(nightLabel(id)): \(minutes > 0 ? SleepText.duration(minutes) : "no sleep recorded")")
        }
    }

    public var goalRatio: Double {
        let top = peak
        return goal > 0 && top > 0 ? Double(goal) / top : 0
    }

    func clock(_ value: JSONValue?) -> String {
        let ms = ToolsJSON.epoch(value)
        return StepsText.clock(Date(timeIntervalSince1970: TimeInterval(ms) / 1000), timeZone: timeZone)
    }

    public func nightLabel(_ night: String) -> String {
        if night == currentNight { return "Last night" }
        return CaloriesText.dayLabel(night, timeZone: timeZone, now: now())
    }

    public func span(night: String, bed: Date, wake: Date) -> (bedAt: Date, wakeAt: Date)? {
        guard let midnight = CaloriesText.tryParseDay(night, timeZone: timeZone) else { return nil }
        let calendar = CaloriesText.calendar(timeZone)
        let bedParts = calendar.dateComponents([.hour, .minute], from: bed)
        let wakeParts = calendar.dateComponents([.hour, .minute], from: wake)
        let bedMinutes = (bedParts.hour ?? 0) * 60 + (bedParts.minute ?? 0)
        let wakeMinutes = (wakeParts.hour ?? 0) * 60 + (wakeParts.minute ?? 0)
        guard let wakeAt = calendar.date(byAdding: .minute, value: wakeMinutes, to: midnight),
            let sameDay = calendar.date(byAdding: .minute, value: bedMinutes, to: midnight)
        else { return nil }
        let bedAt = bedMinutes >= wakeMinutes ? (calendar.date(byAdding: .day, value: -1, to: sameDay) ?? sameDay) : sameDay
        return (bedAt, wakeAt)
    }
}

public enum SleepText {
    public static func duration(_ minutes: Int) -> String {
        let total = max(0, minutes)
        let hours = total / 60, rest = total % 60
        if hours == 0 { return "\(rest) min" }
        return rest == 0 ? "\(hours) h" : "\(hours) h \(rest) min"
    }

    public static func clock(minutes: Int) -> String {
        guard minutes >= 0 else { return "—" }
        let hour = minutes / 60 % 24, minute = minutes % 60
        let twelve = hour % 12 == 0 ? 12 : hour % 12
        return "\(twelve):\(minute < 10 ? "0" : "")\(minute) \(hour < 12 ? "AM" : "PM")"
    }
}
