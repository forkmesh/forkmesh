import Foundation

public enum ActivityGrid {
    public static let hours = 24
    public static let rows = 12
    public static let slotMinutes = 60 / rows
    public static let slots = hours * rows
    public static let levels = 5

    public static func slot(hour: Int, minute: Int) -> Int {
        hour * rows + minute / slotMinutes
    }

    public static func slot(of date: Date, calendar: Calendar = .current) -> Int {
        let parts = calendar.dateComponents([.hour, .minute], from: date)
        return slot(hour: parts.hour ?? 0, minute: parts.minute ?? 0)
    }

    public static func level(_ value: Int, peak: Int) -> Int {
        guard value > 0, peak > 0 else { return 0 }
        return min(max(Int((4.0 * Double(value) / Double(peak)).rounded(.up)), 1), levels - 1)
    }

    public static func label(_ slot: Int) -> String {
        String(format: "%02d:%02d", slot / rows, (slot % rows) * slotMinutes)
    }

    public static func sourcesText(self selfKind: String, others: [String]) -> String {
        func name(_ kind: String) -> String {
            switch kind {
            case "desktop": return "desktop"
            case "ios": return "iOS device"
            case "android": return "Android device"
            case "web": return "browser"
            default: return "other device"
            }
        }
        let mine = selfKind == "desktop" ? "this desktop" : selfKind == "web" ? "this browser" : "this device"
        var parts = [mine]
        var seen: [String] = []
        for kind in others where !seen.contains(kind) {
            seen.append(kind)
            parts.append(kind == selfKind ? "another " + name(kind) : name(kind))
        }
        guard parts.count > 1, let last = parts.popLast() else { return parts[0] }
        return parts.joined(separator: ", ") + " and " + last
    }
}

public struct ActivitySummary: Equatable, Sendable {
    public var total = 0
    public var peak = 0
    public var peakSlot = -1
    public var peakHour = -1
    public var peakHourTotal = 0
    public var activeSlots = 0

    public init(_ counts: [Int]) {
        var hours = [Int](repeating: 0, count: ActivityGrid.hours)
        for (slot, value) in counts.prefix(ActivityGrid.slots).enumerated() where value > 0 {
            total += value
            activeSlots += 1
            hours[slot / ActivityGrid.rows] += value
            if value > peak {
                peak = value
                peakSlot = slot
            }
        }
        for (hour, value) in hours.enumerated() where value > peakHourTotal {
            peakHourTotal = value
            peakHour = hour
        }
    }

    public var activeMinutes: Int { activeSlots * ActivityGrid.slotMinutes }

    public var activeText: String {
        activeMinutes >= 60 ? "\(activeMinutes / 60)h \(activeMinutes % 60)m" : "\(activeMinutes)m"
    }
}

public final class UserActivityLog: @unchecked Sendable {
    public static let key = "user_activity_today"
    public static let deviceKeyName = "user_activity_device"
    public static let saveInterval: TimeInterval = 60

    private let lock = NSLock()
    private let store: KeyValueStore?
    private let clock: Clock
    private let calendar: Calendar
    private var day = ""
    private var counts = [Int](repeating: 0, count: ActivityGrid.slots)
    private var lastSave = Date.distantPast
    private var dirty = false
    private var revisionValue = 0
    private var othersDay = ""
    private var others = [Int](repeating: 0, count: ActivityGrid.slots)
    private var kinds: [String] = []
    public let deviceKey: String

    public init(store: KeyValueStore?, clock: Clock = SystemClock(), calendar: Calendar = .current) {
        self.store = store
        self.clock = clock
        self.calendar = calendar
        if let stored = store?.string(forKey: Self.deviceKeyName), (8...64).contains(stored.count) {
            deviceKey = stored
        } else {
            deviceKey = UUID().uuidString.lowercased()
            store?.set(deviceKey, forKey: Self.deviceKeyName)
        }
        load()
    }

    public var revision: Int {
        lock.lock()
        defer { lock.unlock() }
        return revisionValue
    }

    public var today: String { dayKey(clock.now) }

    public func record(at date: Date? = nil) {
        let at = date ?? clock.now
        lock.lock()
        defer { lock.unlock() }
        let today = dayKey(at)
        if today != day {
            day = today
            counts = [Int](repeating: 0, count: ActivityGrid.slots)
        }
        counts[ActivityGrid.slot(of: at, calendar: calendar)] += 1
        revisionValue += 1
        dirty = true
        if clock.now.timeIntervalSince(lastSave) >= Self.saveInterval { save() }
    }

    public func localCounts(on date: Date? = nil) -> [Int] {
        lock.lock()
        defer { lock.unlock() }
        return dayKey(date ?? clock.now) == day ? counts : [Int](repeating: 0, count: ActivityGrid.slots)
    }

    public func counts(on date: Date? = nil) -> [Int] {
        var merged = localCounts(on: date)
        lock.lock()
        defer { lock.unlock() }
        if dayKey(date ?? clock.now) == othersDay {
            for slot in merged.indices { merged[slot] += others[slot] }
        }
        return merged
    }

    public func setOthers(day: String, counts values: [Int], kinds otherKinds: [String]) {
        guard values.count == ActivityGrid.slots, !day.isEmpty else { return }
        lock.lock()
        defer { lock.unlock() }
        othersDay = day
        others = values.map { max(0, $0) }
        kinds = otherKinds
    }

    public func otherKinds(on date: Date? = nil) -> [String] {
        lock.lock()
        defer { lock.unlock() }
        return dayKey(date ?? clock.now) == othersDay ? kinds : []
    }

    public func clearOthers() {
        lock.lock()
        defer { lock.unlock() }
        othersDay = ""
        others = [Int](repeating: 0, count: ActivityGrid.slots)
        kinds = []
    }

    public var nowSlot: Int { ActivityGrid.slot(of: clock.now, calendar: calendar) }

    public func flush() {
        lock.lock()
        defer { lock.unlock() }
        save()
    }

    private func save() {
        lastSave = clock.now
        guard dirty, !day.isEmpty, let store else { return }
        store.set(day + "|" + counts.map(String.init).joined(separator: ","), forKey: Self.key)
        dirty = false
    }

    private func load() {
        guard let raw = store?.string(forKey: Self.key) else { return }
        let parts = raw.split(separator: "|", maxSplits: 1, omittingEmptySubsequences: false)
        guard parts.count == 2, parts[0].count == 10 else { return }
        let values = parts[1].split(separator: ",").compactMap { Int($0) }
        guard values.count == ActivityGrid.slots else { return }
        day = String(parts[0])
        counts = values.map { max(0, $0) }
    }

    private func dayKey(_ date: Date) -> String {
        let parts = calendar.dateComponents([.year, .month, .day], from: date)
        return String(format: "%04d-%02d-%02d", parts.year ?? 0, parts.month ?? 0, parts.day ?? 0)
    }
}
