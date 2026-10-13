import FMBase
import Foundation

public enum HealthKind: String, CaseIterable, Sendable {
    case steps
    case water
    case nutrition
    case sleep
}

public enum HealthQuantity: String, CaseIterable, Sendable {
    case water
    case energy
    case protein
    case carbs
    case fat
}

public struct HealthStepDay: Equatable, Sendable {
    public var day: String
    public var steps: Int
    public var distanceM: Double
    public var floors: Int
    public var activeKcal: Double
    public var cadence: Double
    public var pace: Double

    public init(
        day: String, steps: Int, distanceM: Double = 0, floors: Int = 0, activeKcal: Double = 0, cadence: Double = 0,
        pace: Double = 0
    ) {
        self.day = day
        self.steps = steps
        self.distanceM = distanceM
        self.floors = floors
        self.activeKcal = activeKcal
        self.cadence = cadence
        self.pace = pace
    }

    public var body: JSONObject {
        [
            "day": .string(day),
            "steps": .int(Int64(max(0, steps))),
            "distanceM": .double((max(0, distanceM) * 10).rounded() / 10),
            "floors": .int(Int64(max(0, floors))),
            "activeKcal": .double((max(0, activeKcal) * 10).rounded() / 10),
        ]
    }

    public func merged(with other: HealthStepDay) -> HealthStepDay {
        HealthStepDay(
            day: day, steps: max(steps, other.steps), distanceM: max(distanceM, other.distanceM),
            floors: max(floors, other.floors), activeKcal: max(activeKcal, other.activeKcal),
            cadence: max(cadence, other.cadence), pace: max(pace, other.pace))
    }
}

public struct HealthSample: Equatable, Sendable {
    public let entryId: String
    public let version: String
    public let amount: Double
    public let date: Date
    public let ours: Bool

    public init(entryId: String, version: String, amount: Double, date: Date, ours: Bool) {
        self.entryId = entryId
        self.version = version
        self.amount = amount
        self.date = date
        self.ours = ours
    }
}

public struct HealthWrite: Equatable, Sendable {
    public let entryId: String
    public let version: String
    public let date: Date
    public let name: String
    public let values: [HealthQuantity: Double]

    public init(entryId: String, version: String, date: Date, name: String = "", values: [HealthQuantity: Double]) {
        self.entryId = entryId
        self.version = version
        self.date = date
        self.name = name
        self.values = values
    }
}

public struct HealthPlan: Equatable, Sendable {
    public var save: [HealthWrite] = []
    public var delete: [String] = []

    public var isEmpty: Bool { save.isEmpty && delete.isEmpty }
}

@MainActor
public protocol HealthBridge: AnyObject {
    var available: Bool { get }
    func authorize(_ kinds: Set<HealthKind>) async -> Bool
    func stepDays(_ count: Int, now: Date) async -> [HealthStepDay]
    func startLiveSteps(_ update: @escaping @MainActor (HealthStepDay) -> Void) -> Bool
    func stopLiveSteps()
    func startMotionSamples(_ update: @escaping @MainActor ([MotionSample]) -> Void) -> Bool
    func stopMotionSamples()
    func samples(_ quantity: HealthQuantity, from: Date, to: Date) async throws -> [HealthSample]
    func save(_ writes: [HealthWrite]) async throws
    func delete(_ quantities: [HealthQuantity], entryIds: [String], from: Date, to: Date) async throws
}

public extension HealthBridge {
    func startMotionSamples(_ update: @escaping @MainActor ([MotionSample]) -> Void) -> Bool { false }
    func stopMotionSamples() {}
}

@MainActor
public protocol SugarBridge: AnyObject {
    var available: Bool { get }
    func authorizeSugar() async -> Bool
    func sugarGrams(from: Date, to: Date) async -> [String: Double]
}

public enum StretchWorkoutKind: String, CaseIterable, Sendable {
    case flexibility
    case mindAndBody
}

public struct StretchWorkout: Equatable, Sendable {
    public let kind: StretchWorkoutKind
    public let routine: String
    public let start: Date
    public let end: Date
    public let pauses: [DateInterval]
    public let seconds: Int

    public init(kind: StretchWorkoutKind, routine: String, start: Date, end: Date, pauses: [DateInterval] = [], seconds: Int) {
        self.kind = kind
        self.routine = routine
        self.start = start
        self.end = end
        self.pauses = pauses
        self.seconds = seconds
    }
}

@MainActor
public protocol StretchBridge: AnyObject {
    var available: Bool { get }
    func authorizeStretch() async -> Bool
    func saveStretchWorkout(_ workout: StretchWorkout) async throws
    func stretchMinutes(from: Date, to: Date) async -> [StretchWorkoutKind: Double]
}

public enum HealthPrefs {
    public static let steps = "health.steps.enabled"
    public static let water = "health.water.enabled"
    public static let nutrition = "health.calories.enabled"
    public static let stepsSyncedAt = "health.steps.syncedAt"
    public static let stepsImperial = "health.steps.imperial"
    public static let sleep = "health.sleep.enabled"
    public static let sleepWriteHealth = "health.sleep.writeHealth"
    public static let sleepSyncedAt = "health.sleep.syncedAt"
    public static let sleepUsage = "health.sleep.usage"
    public static let sleepUsageOpen = "health.sleep.usageOpen"
    public static let sleepBedtime = "health.sleep.bedtime"
    public static let sleepWakeTime = "health.sleep.wakeTime"
    public static let sugarBreak = "health.sugarBreak.enabled"
    public static let stretch = "health.stretch.enabled"
    public static let foodGuide = "health.foodGuide.enabled"
    public static let syncInterval: TimeInterval = 15 * 60
}

public enum HealthReconcile {
    public static func plan(desired: [HealthWrite], existing: [HealthSample]) -> HealthPlan {
        var versions: [String: Set<String>] = [:]
        for sample in existing where sample.ours && !sample.entryId.isEmpty {
            versions[sample.entryId, default: []].insert(sample.version)
        }
        var plan = HealthPlan()
        var wanted = Set<String>()
        for write in desired where !write.entryId.isEmpty && !wanted.contains(write.entryId) {
            wanted.insert(write.entryId)
            guard let stored = versions[write.entryId] else {
                plan.save.append(write)
                continue
            }
            if stored != [write.version] {
                plan.delete.append(write.entryId)
                plan.save.append(write)
            }
        }
        for id in versions.keys.sorted() where !wanted.contains(id) {
            plan.delete.append(id)
        }
        return plan
    }

    public static func imports(_ samples: [HealthSample], known: Set<String>) -> [HealthSample] {
        var seen = known
        var result: [HealthSample] = []
        for sample in samples.sorted(by: { $0.date < $1.date }) where !sample.ours && sample.amount > 0 {
            if seen.insert(sample.entryId).inserted { result.append(sample) }
        }
        return result
    }
}

public enum HealthDates {
    public static func sampleDate(day: String, atMs: Int64, timeZone: TimeZone = .current, now: Date = Date()) -> Date? {
        guard let midnight = CaloriesText.tryParseDay(day, timeZone: timeZone) else { return nil }
        if midnight > now { return nil }
        if atMs > 0 {
            let at = Date(timeIntervalSince1970: TimeInterval(atMs) / 1000)
            if CaloriesText.isoDay(at, timeZone: timeZone) == day { return min(at, now) }
        }
        let noon = CaloriesText.calendar(timeZone).date(byAdding: .hour, value: 12, to: midnight) ?? midnight
        return min(noon, now)
    }

    public static func startOfDay(_ day: String, timeZone: TimeZone = .current) -> Date? {
        CaloriesText.tryParseDay(day, timeZone: timeZone)
    }

    public static func days(ending day: String, count: Int, timeZone: TimeZone = .current) -> [String] {
        guard let end = CaloriesText.tryParseDay(day, timeZone: timeZone), count > 0 else { return [] }
        let calendar = CaloriesText.calendar(timeZone)
        return (0..<count).reversed().compactMap { offset in
            calendar.date(byAdding: .day, value: -offset, to: end).map { CaloriesText.isoDay($0, timeZone: timeZone) }
        }
    }

    public static func epochMs(_ date: Date) -> Int64 { Int64((date.timeIntervalSince1970 * 1000).rounded()) }
}

public enum StepsHealthSync {
    public static let maxDaysPerRequest = 62

    public static func push(_ days: [HealthStepDay], device: String, call: ToolsApiCall) async throws -> Int {
        let usable = days.filter { !$0.day.isEmpty }
        var synced = 0
        var index = 0
        while index < usable.count {
            let chunk = Array(usable[index..<min(usable.count, index + maxDaysPerRequest)])
            let reply = try await call("POST", "/sync", [
                "source": "health", "device": .string(device), "days": .array(chunk.map { .object($0.body) }),
            ])
            synced += ToolsJSON.int(reply["synced"])
            index += maxDaysPerRequest
        }
        return synced
    }

    @MainActor
    public static func runIfDue(
        health: HealthBridge, prefs: KeyValueStore, call: ToolsApiCall, now: Date = Date(), days: Int = 7,
        force: Bool = false
    ) async -> Int {
        guard prefs.bool(forKey: HealthPrefs.steps, or: false), health.available else { return 0 }
        let last = prefs.double(forKey: HealthPrefs.stepsSyncedAt, or: 0)
        if !force && now.timeIntervalSince1970 - last < HealthPrefs.syncInterval { return 0 }
        let collected = await health.stepDays(days, now: now)
        guard !collected.isEmpty else { return 0 }
        do {
            let synced = try await push(collected, device: "iphone", call: call)
            prefs.set(now.timeIntervalSince1970, forKey: HealthPrefs.stepsSyncedAt)
            return synced
        } catch {
            return 0
        }
    }
}
