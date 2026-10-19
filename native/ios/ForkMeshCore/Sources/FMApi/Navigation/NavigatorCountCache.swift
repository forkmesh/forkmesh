import Foundation

public struct NavigatorCountCache: Codable, Equatable, Sendable {
    public static let daily: Set<Route.Kind> = [.calendar, .calories, .steps, .water, .sleep, .sugarBreak, .stretch]

    public var day: String
    public var counts: [String: Int]
    public var assignedTasks: Int
    public var bugReports: Int?

    public init(day: String, counts: [Route.Kind: Int] = [:], assignedTasks: Int = 0, bugReports: Int? = nil) {
        self.day = day
        self.counts = Dictionary(uniqueKeysWithValues: counts.map { ($0.key.rawValue, $0.value) })
        self.assignedTasks = assignedTasks
        self.bugReports = bugReports
    }

    public var kinds: [Route.Kind: Int] {
        var out: [Route.Kind: Int] = [:]
        for (name, count) in counts {
            if let kind = Route.Kind(rawValue: name) { out[kind] = count }
        }
        return out
    }

    public func on(_ today: String) -> NavigatorCountCache {
        guard day != today else { return self }
        var next: NavigatorCountCache = self
        next.day = today
        next.counts = counts.filter { (entry: (key: String, value: Int)) -> Bool in
            guard let kind = Route.Kind(rawValue: entry.key) else { return false }
            return !NavigatorCountCache.daily.contains(kind)
        }
        return next
    }

    public var encoded: String {
        let encoder: JSONEncoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(self) else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    public static func decode(_ text: String?) -> NavigatorCountCache? {
        guard let text, let data = text.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(NavigatorCountCache.self, from: data)
    }

    public static func key(project: String, user: String) -> String {
        "shell.navigatorCounts.\(project).\(user)"
    }

    public static func day(_ date: Date, calendar: Calendar = .current) -> String {
        let parts: DateComponents = calendar.dateComponents([.year, .month, .day], from: date)
        return String(format: "%04d-%02d-%02d", parts.year ?? 0, parts.month ?? 0, parts.day ?? 0)
    }
}
