import FMApi
import FMBase
import Foundation

public struct GenieActivity: Equatable, Sendable {
    public var kind: String
    public var at: Date

    public init(kind: String, at: Date) {
        self.kind = kind
        self.at = at
    }

    public static func tab(_ tab: AppTab) -> String { "tab.\(tab.rawValue)" }

    public static func page(_ kind: Route.Kind) -> String { "page.\(kind.rawValue)" }

    public static let agentStarted: String = "agent.started"

    public static func genie(_ tool: String) -> String { "genie.\(tool)" }

    public var routeKind: Route.Kind? {
        if kind.hasPrefix("page.") { return Route.Kind(rawValue: String(kind.dropFirst(5))) }
        if kind.hasPrefix("tab."), let tab = AppTab(rawValue: String(kind.dropFirst(4))) { return tab.root.kind }
        return nil
    }

    var encoded: String { "\(Int64(at.timeIntervalSince1970))|\(kind)" }

    init?(encoded: String) {
        let parts: [Substring] = encoded.split(separator: "|", maxSplits: 1)
        guard parts.count == 2, let seconds = Int64(parts[0]), !parts[1].isEmpty else { return nil }
        kind = String(parts[1])
        at = Date(timeIntervalSince1970: TimeInterval(seconds))
    }
}

@MainActor
public final class GenieActivityLog {
    public nonisolated static let storeKey: String = "genie/activity"
    public nonisolated static let most: Int = 400
    public nonisolated static let repeatGap: TimeInterval = 20

    private let store: KeyValueStore?
    public private(set) var entries: [GenieActivity]

    public init(store: KeyValueStore?) {
        self.store = store
        entries = (store?.stringList(forKey: GenieActivityLog.storeKey) ?? []).compactMap(GenieActivity.init(encoded:))
    }

    public func record(_ kind: String, at now: Date = Date()) {
        guard !kind.isEmpty else { return }
        if let last = entries.last, last.kind == kind, now.timeIntervalSince(last.at) < GenieActivityLog.repeatGap { return }
        entries.append(GenieActivity(kind: kind, at: now))
        if entries.count > GenieActivityLog.most { entries.removeFirst(entries.count - GenieActivityLog.most) }
        store?.set(entries.map(\.encoded), forKey: GenieActivityLog.storeKey)
    }

    public func marketingVisits(since: Date) -> Int {
        entries.filter { $0.at >= since && ($0.routeKind.map(GenieVitals.isMarketing) ?? false) }.count
    }

    public func forget() {
        entries = []
        store?.set([String](), forKey: GenieActivityLog.storeKey)
    }
}

public struct GenieHint: Equatable, Sendable, Identifiable {
    public var id: String
    public var text: String
    public var spot: GenieSpot?
    public var weight: Double

    public init(id: String, text: String, spot: GenieSpot?, weight: Double) {
        self.id = id
        self.text = text
        self.spot = spot
        self.weight = weight
    }
}

public struct GenieHintContext: Equatable, Sendable {
    public var unread: Int
    public var running: Int
    public var finished: Int
    public var tab: AppTab?

    public init(unread: Int = 0, running: Int = 0, finished: Int = 0, tab: AppTab? = nil) {
        self.unread = unread
        self.running = running
        self.finished = finished
        self.tab = tab
    }
}

public enum GenieHints {
    public nonisolated static let habitDays: Int = 3
    public nonisolated static let nextShare: Double = 0.4
    public nonisolated static let nextCount: Int = 3
    public nonisolated static let most: Int = 3

    public nonisolated static let screens: [Route.Kind: String] = [
        .agents: "agents", .inbox: "inbox", .repos: "repositories", .chat: "chat", .tasks: "tasks", .notes: "notes",
        .calendar: "calendar", .crm: "crm", .marketing: "marketing", .analytics: "analytics", .ads: "ads",
        .competitors: "competitors", .stats: "stats", .status: "status", .desktops: "desktops",
        .settings: "settings", .market: "market",
    ]

    public static func suggest(
        log: [GenieActivity], vitals: GenieVitals?, context: GenieHintContext, now: Date = Date(),
        calendar: Calendar = .current
    ) -> [GenieHint] {
        var hints: [GenieHint] = []
        let current: Route.Kind? = context.tab?.root.kind
        if context.unread > 0, !visited(.inbox, in: log, within: 1800, now: now) {
            let many: Bool = context.unread > 1
            hints.append(GenieHint(
                id: "unread",
                text: "You have \(context.unread) unread \(many ? "pings" : "ping"). Shall I read \(many ? "the newest" : "it")?",
                spot: GenieSpotter.latestSpot("inbox", count: 1, phrase: 0..<0), weight: 0.9 + min(Double(context.unread), 10) / 100))
        }
        if context.finished > 0 {
            hints.append(GenieHint(
                id: "finished",
                text: context.finished == 1 ? "An agent just finished. Want to review it?" : "\(context.finished) agents finished. Want to review them?",
                spot: screenSpot(.agents), weight: 0.85))
        }
        if let next = habitNext(log, now: now), next != current, let spot = screenSpot(next) {
            hints.append(GenieHint(id: "next.\(next.rawValue)", text: "You usually open \(spot.page) next. Go there?", spot: spot, weight: 0.7))
        }
        if let usual = habitHour(log, now: now, calendar: calendar), usual != current, let spot = screenSpot(usual) {
            hints.append(GenieHint(
                id: "hour.\(usual.rawValue)", text: "Around this time you usually check \(spot.page).", spot: spot, weight: 0.6))
        }
        if let vitals = vitals {
            if vitals.marketing < 0.3 {
                hints.append(GenieHint(
                    id: "feed.marketing",
                    text: vitals.signs.emailsSent == 0
                        ? "Marketing is hungry: no outreach this week. Reach out to a contact in the CRM?"
                        : "Marketing is running low. A few more contacts would perk me up.",
                    spot: screenSpot(.crm), weight: 0.65 - vitals.marketing))
            }
            if vitals.code < 0.3 {
                hints.append(GenieHint(
                    id: "feed.code",
                    text: vitals.signs.commits == 0
                        ? "Code has been quiet this week. Start an agent on something?" : "Code is slowing down. An agent could help.",
                    spot: screenSpot(.agents), weight: 0.62 - vitals.code))
            }
        }
        if context.running > 0, hints.isEmpty {
            hints.append(GenieHint(
                id: "running",
                text: context.running == 1 ? "One agent is at work. Check on it?" : "\(context.running) agents are at work. Check on them?",
                spot: screenSpot(.agents), weight: 0.3))
        }
        var seen: Set<String> = []
        return hints.sorted { $0.weight > $1.weight }
            .filter { (hint: GenieHint) -> Bool in
                let key: String = hint.spot.map { "\($0.tool):\($0.arguments)" } ?? hint.id
                return seen.insert(key).inserted
            }
            .prefix(most).map { $0 }
    }

    public nonisolated static let general: [String] = [
        "Touch and hold me for more, including Talk.",
        "Drag me to an edge to tuck me away.",
        "Pinch me to make me bigger or smaller.",
        "Agents send their work back as pull requests.",
    ]

    public static func lines(
        log: [GenieActivity], vitals: GenieVitals?, context: GenieHintContext, now: Date = Date(),
        calendar: Calendar = .current
    ) -> [String] {
        var lines: [String] = []
        let current: Route.Kind? = context.tab?.root.kind
        if context.unread > 0, !visited(.inbox, in: log, within: 1800, now: now) {
            lines.append(context.unread == 1 ? "You have one unread ping in your inbox." : "You have \(context.unread) unread pings in your inbox.")
        }
        if context.finished > 0 {
            lines.append(context.finished == 1 ? "An agent just finished; review it in Agents." : "\(context.finished) agents finished; review them in Agents.")
        }
        if let next = habitNext(log, now: now), next != current, let spot = screenSpot(next) {
            lines.append("You usually open \(spot.page) next.")
        }
        if let usual = habitHour(log, now: now, calendar: calendar), usual != current, let spot = screenSpot(usual) {
            lines.append("Around now you usually check \(spot.page).")
        }
        if let vitals = vitals {
            if vitals.marketing < 0.3 { lines.append("Marketing is hungry; reach out to a CRM contact.") }
            if vitals.code < 0.3 { lines.append("Code is quiet; start an agent on something.") }
        }
        if context.running > 0 {
            lines.append(context.running == 1 ? "One agent is at work right now." : "\(context.running) agents are at work right now.")
        }
        return lines + general
    }

    public static func words(_ line: String) -> Int {
        line.split(whereSeparator: { $0.isWhitespace }).count
    }

    public static func screenSpot(_ kind: Route.Kind) -> GenieSpot? {
        guard let name = screens[kind] else { return nil }
        let tab: AppTab? = AppTab.barOrder.first { $0.root.kind == kind }
        let title: String = tab?.title ?? RouteCatalog.title(kind)
        return GenieSpot(
            tool: "open_screen", arguments: ["screen": name], title: "Open \(title)", page: title,
            symbol: tab?.symbol ?? RouteCatalog.symbol(kind), phrase: 0..<0)
    }

    static func visited(_ kind: Route.Kind, in log: [GenieActivity], within: TimeInterval, now: Date) -> Bool {
        log.contains { $0.routeKind == kind && now.timeIntervalSince($0.at) < within }
    }

    public static func habitNext(_ log: [GenieActivity], now: Date) -> Route.Kind? {
        let visits: [Route.Kind] = log.compactMap(\.routeKind)
        guard let last = visits.last else { return nil }
        var after: [Route.Kind: Int] = [:]
        for (index, kind) in visits.enumerated().dropLast() where kind == last {
            let next: Route.Kind = visits[index + 1]
            if next != last { after[next, default: 0] += 1 }
        }
        let total: Int = after.values.reduce(0, +)
        guard let best = after.max(by: { $0.value == $1.value ? $0.key.rawValue > $1.key.rawValue : $0.value < $1.value }),
            best.value >= nextCount, Double(best.value) / Double(max(total, 1)) >= nextShare, screens[best.key] != nil
        else { return nil }
        return best.key
    }

    public static func habitHour(_ log: [GenieActivity], now: Date, calendar: Calendar = .current) -> Route.Kind? {
        let hour: Int = calendar.component(.hour, from: now)
        let today: Date = calendar.startOfDay(for: now)
        var days: [Route.Kind: Set<Date>] = [:]
        var doneToday: Set<Route.Kind> = []
        for entry in log {
            guard let kind = entry.routeKind, screens[kind] != nil else { continue }
            let day: Date = calendar.startOfDay(for: entry.at)
            if day == today {
                doneToday.insert(kind)
                continue
            }
            let gap: Int = abs(calendar.component(.hour, from: entry.at) - hour)
            guard min(gap, 24 - gap) <= 1 else { continue }
            days[kind, default: []].insert(day)
        }
        return days.filter { $0.value.count >= habitDays && !doneToday.contains($0.key) }
            .max { $0.value.count == $1.value.count ? $0.key.rawValue > $1.key.rawValue : $0.value.count < $1.value.count }?.key
    }

    public static func greeting(_ hint: GenieHint?) -> String {
        guard let hint = hint else { return GenieSession.greeting }
        return "\(hint.text) Or ask me anything."
    }
}
