import FMApi
import FMBase
import Foundation
import Observation

public struct WaterAPI {
    public var call: ToolsApiCall

    public init(call: @escaping ToolsApiCall) {
        self.call = call
    }

    @MainActor
    public init(api: ApiService) {
        call = { method, suffix, body in try await api.waterApi(method, suffix, body) }
    }
}

public struct WaterEntryRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    public let amount: String
    public let fromHealth: Bool
}

public struct WaterPreset: Equatable, Identifiable, Sendable {
    public var id: Int { milliliters }
    public let milliliters: Int
    public let label: String
}

public struct WaterBar: Equatable, Identifiable, Sendable {
    public let id: String
    public let label: String
    public let ratio: Double
    public let metGoal: Bool
    public let caption: String
}

@MainActor
@Observable
public final class WaterModel {
    public static let title = "Water Intake"
    public static let installTitle = "Track what you drink"
    public static let installMessage =
        "Water Intake is private to your ForkMesh account. Log each glass here, on the web, or on your desktop; the same log appears everywhere, and with Apple Health turned on every drink is written to Health and water other apps logged there comes in too. Every drink is encrypted at rest with a key minted for your account alone."
    public static let installButton = "Install Water Intake"
    public static let unavailableTitle = "Water Intake is unavailable"
    public static let emptyLog = "Nothing logged today. Tap a glass after each drink."
    public static let drinks = ["water", "sparkling", "tea", "coffee", "juice", "milk", "other"]
    public static let reconcileDays = 7

    @ObservationIgnored public let api: WaterAPI
    @ObservationIgnored public let health: HealthBridge?
    @ObservationIgnored public let prefs: KeyValueStore
    @ObservationIgnored public let timeZone: TimeZone
    @ObservationIgnored private let now: () -> Date

    public private(set) var overview: JSONObject?
    public private(set) var installed = false
    public private(set) var loading = true
    public private(set) var error = ""
    public private(set) var healthEnabled: Bool
    public private(set) var busy = false
    public var notice: ToolsNotice?

    public init(
        api: WaterAPI, health: HealthBridge?, prefs: KeyValueStore = InMemoryKeyValueStore(),
        timeZone: TimeZone = .current, now: (() -> Date)? = nil
    ) {
        self.api = api
        self.health = health
        self.prefs = prefs
        self.timeZone = timeZone
        self.now = now ?? { Date() }
        healthEnabled = prefs.bool(forKey: HealthPrefs.water, or: false)
    }

    public var today: String { CaloriesText.isoDay(now(), timeZone: timeZone) }
    public var healthAvailable: Bool { health?.available == true }

    public func load(reconcile: Bool = true) async {
        loading = true
        error = ""
        do {
            try await fetch()
            loading = false
            if reconcile && installed && healthEnabled {
                if await reconcileHealth() { try? await fetch() }
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
    }

    func message(_ error: Error) -> String {
        (error as? OrgApiException)?.message ?? "Water Intake is unavailable right now."
    }

    func show(_ text: String, error: Bool = false) { notice = ToolsNotice(text, isError: error) }

    public var showsSpinner: Bool { loading && overview == nil && error.isEmpty }
    public var showsError: Bool { !error.isEmpty && overview == nil }
    public var showsInstall: Bool { !loading && error.isEmpty && !installed }

    public func install() async {
        do {
            _ = try await api.call("POST", "/install", [
                "tzOffsetMinutes": .int(Int64(CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now()))),
            ])
            show("Water Intake installed.")
            await load(reconcile: false)
            if healthAvailable && !healthEnabled { await enableHealth() }
        } catch {
            show(message(error), error: true)
        }
    }

    public func enableHealth() async {
        guard let health, health.available else {
            show("Apple Health is not available on this device.", error: true)
            return
        }
        guard await health.authorize([.water]) else {
            show("ForkMesh was not allowed to use Water in Apple Health. Allow it in Settings > Health > Data Access & Devices.", error: true)
            return
        }
        prefs.set(true, forKey: HealthPrefs.water)
        healthEnabled = true
        await load()
        show("Water Intake now shares every drink with Apple Health.")
    }

    public func disableHealth() {
        prefs.set(false, forKey: HealthPrefs.water)
        healthEnabled = false
        show("Apple Health sharing is off. Water already written to Health stays there.")
    }

    public func add(milliliters: Int, drink: String = "water", source: String = "quick") async {
        guard milliliters > 0 else {
            show("Enter an amount first.", error: true)
            return
        }
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("POST", "/entries", [
                "day": .string(today), "amountMl": .int(Int64(milliliters)), "drink": .string(drink),
                "source": .string(source), "at": .int(HealthDates.epochMs(now())),
            ])
            show("Logged \(WaterText.amount(Double(milliliters), units: units)) of \(drink).")
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    public func addTyped(_ text: String, drink: String) async {
        let value = CaloriesText.num(text.replacingOccurrences(of: ",", with: "."))
        let milliliters = Int((value * WaterText.unitMilliliters(units)).rounded())
        await add(milliliters: milliliters, drink: drink, source: "manual")
    }

    public func delete(_ id: String) async {
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("DELETE", "/entries/\(id)", nil)
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    public func saveGoal(_ goalMl: Int, units: String) async {
        do {
            _ = try await api.call("PUT", "/profile", ["goalMl": .int(Int64(max(0, goalMl))), "unitsPreference": .string(units)])
            show("Goal saved.")
            try await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    @discardableResult
    public func uninstall() async -> Bool {
        do {
            _ = try await api.call("POST", "/uninstall", [:])
            prefs.set(false, forKey: HealthPrefs.water)
            healthEnabled = false
            show("Water Intake uninstalled from your account.")
            await load(reconcile: false)
            return true
        } catch {
            show(message(error), error: true)
            return false
        }
    }

    @discardableResult
    public func reconcileHealth() async -> Bool {
        guard let health, health.available, healthEnabled else { return false }
        let days = HealthDates.days(ending: today, count: Self.reconcileDays, timeZone: timeZone)
        guard let first = days.first, let start = HealthDates.startOfDay(first, timeZone: timeZone) else { return false }
        let end = now()
        do {
            let entries = ToolsJSON.maps(try await api.call("GET", "/entries?from=\(first)&to=\(today)", nil)["entries"])
            let samples = try await health.samples(.water, from: start, to: end)
            let known = Set(entries.compactMap { $0["externalId"]?.stringValue })
            let incoming = HealthReconcile.imports(samples, known: known)
            var imported = 0
            var index = 0
            while index < incoming.count {
                let chunk = incoming[index..<min(incoming.count, index + 200)]
                let reply = try await api.call("POST", "/import", [
                    "entries": .array(chunk.map { sample in
                        .object([
                            "day": .string(CaloriesText.isoDay(sample.date, timeZone: timeZone)),
                            "amountMl": .int(Int64(sample.amount.rounded())),
                            "at": .int(HealthDates.epochMs(sample.date)),
                            "externalId": .string(sample.entryId),
                        ])
                    }),
                ])
                imported += ToolsJSON.int(reply["imported"])
                index += 200
            }
            let desired = Self.healthWrites(entries, timeZone: timeZone, now: end)
            let plan = HealthReconcile.plan(desired: desired, existing: samples.filter(\.ours))
            if !plan.delete.isEmpty {
                try await health.delete([.water], entryIds: plan.delete, from: start, to: end)
            }
            if !plan.save.isEmpty {
                try await health.save(plan.save)
            }
            return imported > 0
        } catch {
            return false
        }
    }

    public static func healthWrites(_ entries: [JSONObject], timeZone: TimeZone, now: Date) -> [HealthWrite] {
        entries.compactMap { entry in
            guard entry["source"]?.stringValue != "health", let id = entry["id"]?.stringValue, !id.isEmpty else { return nil }
            let amount = ToolsJSON.int(entry["amountMl"])
            guard amount > 0,
                let date = HealthDates.sampleDate(
                    day: entry["day"]?.stringValue ?? "", atMs: ToolsJSON.epoch(entry["at"]), timeZone: timeZone, now: now)
            else { return nil }
            let drink = entry["drink"]?.stringValue ?? "water"
            return HealthWrite(
                entryId: id, version: "\(amount)@\(HealthDates.epochMs(date))", date: date, name: drink,
                values: [.water: Double(amount)])
        }
    }

    public var profile: JSONObject { ToolsJSON.object(overview?["profile"]) }
    public var summary: JSONObject { ToolsJSON.object(overview?["summary"]) }
    public var units: String { profile["units"]?.stringValue ?? "metric" }
    public var imperial: Bool { units == "imperial" }
    public var goalMl: Int { ToolsJSON.int(profile["goalMl"]) }
    public var unitsPreference: String { profile["unitsPreference"]?.stringValue ?? "auto" }
    public var totalMl: Int { ToolsJSON.int(ToolsJSON.object(overview?["today"])["totalMl"]) }
    public var totalText: String { WaterText.amount(Double(totalMl), units: units) }
    public var ratio: Double { goalMl > 0 ? min(1, Double(totalMl) / Double(goalMl)) : 0 }
    public var goalMet: Bool { goalMl > 0 && totalMl >= goalMl }
    public var unit: String { units == "cups" ? "cups" : imperial ? "fl oz" : "ml" }
    public var amountHint: String { units == "cups" ? "1.5" : imperial ? "12" : "330" }

    public var goalLine: String {
        guard goalMl > 0 else { return "No daily goal" }
        if goalMet { return "Goal of \(WaterText.amount(Double(goalMl), units: units)) reached" }
        return "\(WaterText.amount(Double(goalMl - totalMl), units: units)) to go of \(WaterText.amount(Double(goalMl), units: units))"
    }

    public var presets: [WaterPreset] {
        let values = (overview?["profile"]?.objectValue?["presetsMl"]?.arrayValue ?? []).compactMap(\.intValue)
        return (values.isEmpty ? [250, 500, 750] : values).filter { $0 > 0 }.map {
            WaterPreset(milliliters: $0, label: WaterText.amount(Double($0), units: units))
        }
    }

    public var rows: [WaterEntryRow] {
        ToolsJSON.maps(overview?["entries"]).reversed().map { entry in
            let at = ToolsJSON.epoch(entry["at"])
            let time = at > 0 ? StepsText.clock(Date(timeIntervalSince1970: TimeInterval(at) / 1000), timeZone: timeZone) : ""
            let source = entry["source"]?.stringValue ?? "manual"
            let drink = WaterText.drinkLabel(entry["drink"]?.stringValue ?? "water")
            return WaterEntryRow(
                id: entry["id"]?.stringValue ?? "", title: drink,
                subtitle: [time, source == "health" ? "From Apple Health" : ""].filter { !$0.isEmpty }.joined(separator: " · "),
                amount: WaterText.amount(Double(ToolsJSON.int(entry["amountMl"])), units: units),
                fromHealth: source == "health")
        }
    }

    public var bars: [WaterBar] {
        let days = ToolsJSON.maps(overview?["days"])
        var peak = max(1000, Double(goalMl) * 1.15)
        for day in days { peak = max(peak, CaloriesText.num(day["totalMl"]) * 1.05) }
        return days.enumerated().map { index, day in
            let id = day["day"]?.stringValue ?? "\(index)"
            let total = ToolsJSON.int(day["totalMl"])
            return WaterBar(
                id: id, label: index % 5 == 0 || index == days.count - 1 ? StepsText.shortDay(id) : "",
                ratio: Double(total) / peak, metGoal: goalMl > 0 && total >= goalMl,
                caption: "\(id): \(WaterText.amount(Double(total), units: units))")
        }
    }

    public var summaryLine: String {
        let streak = ToolsJSON.int(summary["streak"])
        return "Average \(WaterText.amount(CaloriesText.num(summary["averageMl"]), units: units)) · goal met on \(ToolsJSON.int(summary["goalDays"])) of 30 days · streak \(streak) \(streak == 1 ? "day" : "days")"
    }

    public var healthLine: String {
        if !healthAvailable { return "Apple Health is not available on this device." }
        if !healthEnabled { return "Turn on Apple Health to write each drink to Health and bring in water other apps logged." }
        return "Sharing with Apple Health: drinks you log are written to Health, and water from other apps comes in."
    }
}

public enum WaterText {
    public static let mlPerFlOz = 29.5735295625
    public static let mlPerCup = 8 * mlPerFlOz

    public static func unitMilliliters(_ units: String) -> Double {
        units == "cups" ? mlPerCup : units == "imperial" ? mlPerFlOz : 1
    }

    public static func amount(_ milliliters: Double, units: String) -> String {
        if units == "cups" {
            var text = DartCore.toStringAsFixed(milliliters / mlPerCup, 1)
            if text.hasSuffix(".0") { text.removeLast(2) }
            return "\(text) \(text == "1" ? "cup" : "cups")"
        }
        if units == "imperial" {
            let ounces = milliliters / mlPerFlOz
            return "\(ounces >= 10 ? String(Int(ounces.rounded())) : DartCore.toStringAsFixed(ounces, 1)) fl oz"
        }
        if milliliters >= 1000 {
            var text = DartCore.toStringAsFixed(milliliters / 1000, 2)
            while text.hasSuffix("0") { text.removeLast() }
            if text.hasSuffix(".") { text.removeLast() }
            return "\(text) L"
        }
        return "\(Int(milliliters.rounded())) ml"
    }

    public static func drinkLabel(_ drink: String) -> String {
        guard let first = drink.first else { return "Water" }
        return first.uppercased() + drink.dropFirst()
    }
}
