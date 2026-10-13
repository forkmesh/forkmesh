import FMApi
import FMBase
import Foundation
import Observation

public struct StepsAPI {
    public var call: ToolsApiCall

    public init(call: @escaping ToolsApiCall) {
        self.call = call
    }

    @MainActor
    public init(api: ApiService) {
        call = { method, suffix, body in try await api.stepsApi(method, suffix, body) }
    }
}

public struct StepsBar: Equatable, Identifiable, Sendable {
    public let id: String
    public let label: String
    public let steps: Int
    public let ratio: Double
    public let metGoal: Bool
    public let caption: String
}

public struct StepsStat: Equatable, Identifiable, Sendable {
    public var id: String { label }
    public let label: String
    public let value: String
}

@MainActor
@Observable
public final class StepsModel {
    public static let title = "Pedometer"
    public static let installTitle = "Count your steps"
    public static let installMessage =
        "Pedometer is private to your ForkMesh account. Install it, turn on Apple Health, and the steps your iPhone and Apple Watch count are synced to your account so you can see them on the web and on your desktop. Every day is encrypted at rest with a key minted for your account alone."
    public static let installButton = "Install Pedometer"
    public static let unavailableTitle = "Pedometer is unavailable"
    public static let healthTitle = "Apple Health"
    public static let healthOffMessage =
        "Turn on Apple Health to count your steps. ForkMesh reads steps, walking distance, and floors, and sends one total per day to your account."
    public static let healthUnavailableMessage = "Apple Health is not available on this device."
    public static let paceOffMessage = "Turn on Apple Health to see your pace as you walk."
    public static let cadenceStaleAfter: TimeInterval = 6
    public static let defaultStrideM: Double = 0.72
    public static let runStrideScale: Double = 1.35

    @ObservationIgnored public let api: StepsAPI
    @ObservationIgnored public let health: HealthBridge?
    @ObservationIgnored public let prefs: KeyValueStore
    @ObservationIgnored public let timeZone: TimeZone
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored private var lastLive: (steps: Int, at: Date)?
    @ObservationIgnored private var cadenceExpiry: Task<Void, Never>?
    @ObservationIgnored private var meter = StrideMeter()
    @ObservationIgnored private var strides: [StrideGait: Double] = [:]

    public private(set) var overview: JSONObject?
    public private(set) var installed = false
    public private(set) var loading = true
    public private(set) var syncing = false
    public private(set) var error = ""
    public private(set) var healthEnabled: Bool
    public private(set) var live: HealthStepDay?
    public private(set) var cadence: Double = 0
    public private(set) var gait: StrideGait = .still
    public private(set) var sensing = false
    public var notice: ToolsNotice?

    public init(
        api: StepsAPI, health: HealthBridge?, prefs: KeyValueStore = InMemoryKeyValueStore(),
        timeZone: TimeZone = .current, now: (() -> Date)? = nil
    ) {
        self.api = api
        self.health = health
        self.prefs = prefs
        self.timeZone = timeZone
        self.now = now ?? { Date() }
        healthEnabled = prefs.bool(forKey: HealthPrefs.steps, or: false)
    }

    public var today: String { CaloriesText.isoDay(now(), timeZone: timeZone) }

    public func load(syncIfDue: Bool = true) async {
        loading = true
        error = ""
        do {
            try await fetch()
            loading = false
            if syncIfDue && installed && healthEnabled, let health {
                let synced = await StepsHealthSync.runIfDue(
                    health: health, prefs: prefs, call: api.call, now: now(), days: 7)
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
        let payload = try await api.call("GET", "?day=\(today)", nil)
        installed = payload["installed"]?.boolValue == true
        overview = installed ? payload : nil
        if installed { prefs.set(imperial, forKey: HealthPrefs.stepsImperial) }
    }

    func message(_ error: Error) -> String {
        (error as? OrgApiException)?.message ?? "Pedometer is unavailable right now."
    }

    func show(_ text: String, error: Bool = false) { notice = ToolsNotice(text, isError: error) }

    public var showsSpinner: Bool { loading && overview == nil && error.isEmpty }
    public var showsError: Bool { !error.isEmpty && overview == nil }
    public var showsInstall: Bool { !loading && error.isEmpty && !installed }
    public var healthAvailable: Bool { health?.available == true }

    public func install() async {
        do {
            _ = try await api.call("POST", "/install", [
                "tzOffsetMinutes": .int(Int64(CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now()))),
            ])
            show("Pedometer installed.")
            await load(syncIfDue: false)
            if healthAvailable && !healthEnabled { await enableHealth() }
        } catch {
            show(message(error), error: true)
        }
    }

    public func enableHealth() async {
        guard let health, health.available else {
            show(Self.healthUnavailableMessage, error: true)
            return
        }
        guard await health.authorize([.steps]) else {
            show("ForkMesh was not allowed to read your steps. Allow it in Settings > Health > Data Access & Devices.", error: true)
            return
        }
        prefs.set(true, forKey: HealthPrefs.steps)
        healthEnabled = true
        await syncNow(days: 30)
    }

    public func disableHealth() {
        prefs.set(false, forKey: HealthPrefs.steps)
        healthEnabled = false
        stopLive()
        show("Step syncing is off. The steps already synced stay on your account.")
    }

    public func syncNow(days: Int = 7) async {
        guard let health, healthEnabled, installed else { return }
        syncing = true
        let synced = await StepsHealthSync.runIfDue(
            health: health, prefs: prefs, call: api.call, now: now(), days: days, force: true)
        syncing = false
        if synced > 0 {
            try? await fetch()
        } else if (live?.steps ?? 0) == 0 && todaySteps == 0 {
            show("No steps found in Apple Health yet. Check that ForkMesh may read Steps in the Health app.")
        }
    }

    public func startLive() {
        guard let health, healthEnabled else { return }
        _ = health.startLiveSteps { [weak self] day in
            guard let self, day.day == self.today else { return }
            self.track(day)
            self.live = day
        }
        sensing = health.startMotionSamples { [weak self] samples in self?.feed(samples) }
    }

    public func stopLive() {
        health?.stopLiveSteps()
        health?.stopMotionSamples()
        cadenceExpiry?.cancel()
        lastLive = nil
        meter.reset()
        sensing = false
        cadence = 0
        gait = .still
    }

    func feed(_ samples: [MotionSample]) {
        guard sensing, !samples.isEmpty else { return }
        var reading = meter.reading
        for sample in samples { reading = meter.add(sample) }
        if reading.cadence != cadence { cadence = reading.cadence }
        if reading.gait != gait { gait = reading.gait }
    }

    func track(_ day: HealthStepDay) {
        if day.cadence > 0 && day.pace > 0 {
            let stride = 1 / (day.pace * day.cadence)
            let runningNow = sensing ? running : StepsText.gait(Int((day.cadence * 60).rounded())) == "Running"
            if stride >= 0.3 && stride <= 2.5 { strides[runningNow ? .running : .walking] = stride }
        }
        guard !sensing else { return }
        let at = now()
        var rate = day.cadence
        if rate <= 0, let last = lastLive, day.steps > last.steps {
            let elapsed = at.timeIntervalSince(last.at)
            if elapsed >= 1 && elapsed <= Self.cadenceStaleAfter * 2 { rate = Double(day.steps - last.steps) / elapsed }
        }
        lastLive = (day.steps, at)
        cadence = min(max(0, rate), 4)
        cadenceExpiry?.cancel()
        guard cadence > 0 else { return }
        cadenceExpiry = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(Self.cadenceStaleAfter * 1_000_000_000))
            guard !Task.isCancelled else { return }
            self?.cadence = 0
        }
    }

    public var stepsPerMinute: Int { Int((cadence * 60).rounded()) }

    public var running: Bool {
        sensing ? gait == .running && cadence > 0 : StepsText.gait(stepsPerMinute) == "Running"
    }

    public var strideM: Double {
        if let measured = strides[running ? .running : .walking] { return measured }
        let reading = todayReading
        let average = reading.steps >= 100 && reading.distanceM > 0 ? reading.distanceM / Double(reading.steps) : 0
        let base = average >= 0.3 && average <= 1.5 ? average : Self.defaultStrideM
        return running ? base * Self.runStrideScale : base
    }

    public var speedMetersPerSecond: Double { cadence * strideM }

    public var speedText: String { StepsText.speed(speedMetersPerSecond, imperial: true) }

    public var minutesToGoal: Int? {
        let perMinute = stepsPerMinute
        guard goal > 0, perMinute > 0, !goalMet else { return nil }
        return Int((Double(goal - todaySteps) / Double(perMinute)).rounded(.up))
    }

    public var gaitText: String {
        let perMinute = stepsPerMinute
        if running { return "Running" }
        return sensing ? StepsText.walkingGait(perMinute) : StepsText.gait(perMinute)
    }

    public var paceLine: String {
        guard healthAvailable && healthEnabled else { return Self.paceOffMessage }
        let perMinute = stepsPerMinute
        guard perMinute > 0 else { return "Standing still" }
        let line = "\(gaitText) · \(perMinute) steps/min · \(speedText)"
        guard let minutes = minutesToGoal else { return line }
        return "\(line) · \(StepsText.minutesToGoal(minutes, goal: goal))"
    }

    public func leave() async {
        stopLive()
        guard let health, healthEnabled, installed, let live, live.steps > serverToday.steps else { return }
        _ = await StepsHealthSync.runIfDue(health: health, prefs: prefs, call: api.call, now: now(), days: 2, force: true)
    }

    public func saveGoal(_ goal: Int, units: String) async {
        do {
            _ = try await api.call("PUT", "/profile", ["goal": .int(Int64(max(0, goal))), "unitsPreference": .string(units)])
            show("Goal saved.")
            try await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func addSteps(day: String, steps: Int, distance: Double) async {
        let meters = imperial ? distance * StepsText.metersPerMile : distance * 1000
        do {
            _ = try await api.call("PUT", "/days/\(day)", [
                "steps": .int(Int64(max(0, steps))), "distanceM": .double((meters * 10).rounded() / 10),
            ])
            show("Saved \(StepsText.count(steps)) steps for \(dayLabel(day)).")
            try await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func removeAddedSteps(day: String) async {
        do {
            _ = try await api.call("DELETE", "/days/\(day)?source=manual", nil)
            show("Removed the steps you added for \(dayLabel(day)).")
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
            prefs.set(false, forKey: HealthPrefs.steps)
            healthEnabled = false
            stopLive()
            show("Pedometer uninstalled from your account.")
            await load(syncIfDue: false)
            return true
        } catch {
            show(message(error), error: true)
            return false
        }
    }

    public var profile: JSONObject { ToolsJSON.object(overview?["profile"]) }
    public var summary: JSONObject { ToolsJSON.object(overview?["summary"]) }
    public var imperial: Bool { profile["units"]?.stringValue == "imperial" }
    public var goal: Int { ToolsJSON.int(profile["goal"]) }
    public var unitsPreference: String { profile["unitsPreference"]?.stringValue ?? "auto" }

    var serverToday: HealthStepDay {
        let day = ToolsJSON.object(overview?["today"])
        return HealthStepDay(
            day: day["day"]?.stringValue ?? today, steps: ToolsJSON.int(day["steps"]),
            distanceM: CaloriesText.num(day["distanceM"]), floors: ToolsJSON.int(day["floors"]),
            activeKcal: CaloriesText.num(day["activeKcal"]))
    }

    public var todayReading: HealthStepDay {
        let server = serverToday
        guard let live, live.day == server.day else { return server }
        return server.merged(with: live)
    }

    public var todaySteps: Int { todayReading.steps }
    public var stepsText: String { StepsText.count(todaySteps) }

    public var ringRatio: Double { goal > 0 ? min(1, Double(todaySteps) / Double(goal)) : 0 }
    public var goalMet: Bool { goal > 0 && todaySteps >= goal }

    public var goalLine: String {
        guard goal > 0 else { return "No daily goal" }
        return "of \(StepsText.count(goal)) steps"
    }

    public var detailLine: String {
        let reading = todayReading
        var parts: [String] = []
        if goal > 0 {
            parts.append(goalMet ? "Goal reached" : "\(StepsText.count(goal - reading.steps)) to go")
        }
        parts.append(StepsText.distance(reading.distanceM, imperial: imperial))
        if reading.floors > 0 { parts.append("\(reading.floors) floors") }
        if reading.activeKcal > 0 { parts.append("\(Int(reading.activeKcal.rounded())) active kcal") }
        return parts.joined(separator: " · ")
    }

    public var sourceLine: String {
        if !healthAvailable { return Self.healthUnavailableMessage }
        if !healthEnabled { return Self.healthOffMessage }
        let health = ToolsJSON.object(ToolsJSON.object(ToolsJSON.object(overview?["today"])["sources"])["health"])
        let synced = ToolsJSON.epoch(health["updatedAt"])
        if synced <= 0 { return "Counting with Apple Health. Today's steps sync when you open this screen." }
        let date = Date(timeIntervalSince1970: TimeInterval(synced) / 1000)
        return "Counting with Apple Health · synced \(StepsText.clock(date, timeZone: timeZone))"
    }

    public var stats: [StepsStat] {
        let streak = ToolsJSON.int(summary["streak"])
        let best = summary["bestDay"]?.stringValue ?? ""
        return [
            StepsStat(label: "Last 7 days", value: "\(StepsText.count(ToolsJSON.int(summary["weekSteps"]))) steps"),
            StepsStat(label: "30-day average", value: "\(StepsText.count(ToolsJSON.int(summary["averageSteps"]))) steps"),
            StepsStat(label: "Goal streak", value: streak == 1 ? "1 day" : "\(streak) days"),
            StepsStat(
                label: "Best day",
                value: best.isEmpty ? "—" : "\(StepsText.count(ToolsJSON.int(summary["bestSteps"]))) · \(dayLabel(best))"),
        ]
    }

    public var bars: [StepsBar] {
        let days = ToolsJSON.maps(overview?["days"])
        var peak = Double(max(2000, Int(Double(goal) * 1.15)))
        for day in days { peak = max(peak, CaloriesText.num(day["steps"]) * 1.05) }
        return days.enumerated().map { index, day in
            let id = day["day"]?.stringValue ?? "\(index)"
            var steps = ToolsJSON.int(day["steps"])
            if id == todayReading.day { steps = max(steps, todaySteps) }
            return StepsBar(
                id: id, label: index % 5 == 0 || index == days.count - 1 ? StepsText.shortDay(id) : "", steps: steps,
                ratio: peak > 0 ? Double(steps) / peak : 0, metGoal: goal > 0 && steps >= goal,
                caption: "\(dayLabel(id)): \(StepsText.count(steps)) steps")
        }
    }

    public var goalRatio: Double {
        let days = ToolsJSON.maps(overview?["days"])
        var peak = Double(max(2000, Int(Double(goal) * 1.15)))
        for day in days { peak = max(peak, CaloriesText.num(day["steps"]) * 1.05) }
        return goal > 0 && peak > 0 ? Double(goal) / peak : 0
    }

    public var distanceUnit: String { imperial ? "mi" : "km" }

    public func dayLabel(_ day: String) -> String {
        if day == today { return "Today" }
        return CaloriesText.dayLabel(day, timeZone: timeZone, now: now())
    }
}

public enum StepsText {
    public static let metersPerMile = 1609.344

    public static func count(_ value: Int) -> String {
        let digits = String(abs(value))
        var grouped = ""
        for (index, character) in digits.enumerated() {
            if index > 0 && (digits.count - index) % 3 == 0 { grouped.append(",") }
            grouped.append(character)
        }
        return value < 0 ? "-" + grouped : grouped
    }

    public static func distance(_ meters: Double, imperial: Bool) -> String {
        let value = imperial ? meters / metersPerMile : meters / 1000
        return "\(DartCore.toStringAsFixed(value, 2)) \(imperial ? "mi" : "km")"
    }

    public static func gait(_ stepsPerMinute: Int) -> String {
        if stepsPerMinute < 140 { return walkingGait(stepsPerMinute) }
        return "Running"
    }

    public static func walkingGait(_ stepsPerMinute: Int) -> String {
        if stepsPerMinute < 90 { return "Strolling" }
        if stepsPerMinute < 115 { return "Walking" }
        return "Brisk walk"
    }

    public static func speed(_ metersPerSecond: Double, imperial: Bool) -> String {
        let perHour = max(0, metersPerSecond) * 3600
        let value = imperial ? perHour / metersPerMile : perHour / 1000
        return "\(DartCore.toStringAsFixed(value, 1)) \(imperial ? "mph" : "km/h")"
    }

    public static func minutesToGoal(_ minutes: Int, goal: Int) -> String {
        "\(minutes) min to \(count(goal))"
    }

    public static func shortDay(_ day: String) -> String {
        let parts = day.split(separator: "-")
        guard parts.count == 3, let month = Int(parts[1]), let date = Int(parts[2]) else { return day }
        return "\(month)/\(date)"
    }

    public static func clock(_ date: Date, timeZone: TimeZone) -> String {
        let components = CaloriesText.calendar(timeZone).dateComponents([.hour, .minute], from: date)
        let hour = components.hour ?? 0, minute = components.minute ?? 0
        let twelve = hour % 12 == 0 ? 12 : hour % 12
        return "\(twelve):\(minute < 10 ? "0" : "")\(minute) \(hour < 12 ? "AM" : "PM")"
    }
}
