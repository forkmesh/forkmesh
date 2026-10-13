import FMApi
import Foundation

public enum AgentListSort: String, CaseIterable, Codable, Hashable, Sendable {
    case priority, newestAdded, oldestAdded, recentActivity, status, title, provider, issueNumber
    case tokenUsage, cost, runtime, filesChanged

    public var title: String {
        switch self {
        case .priority: return "Priority (highest first)"
        case .newestAdded: return "Date added (newest first)"
        case .oldestAdded: return "Date added (oldest first)"
        case .recentActivity: return "Recent activity"
        case .status: return "Status (active first)"
        case .title: return "Title (A-Z)"
        case .provider: return "Provider and model"
        case .issueNumber: return "Issue number"
        case .tokenUsage: return "Token usage (highest first)"
        case .cost: return "Cost (highest first)"
        case .runtime: return "Runtime (longest first)"
        case .filesChanged: return "Files changed (most first)"
        }
    }
}

public struct AgentListItem: Equatable, Sendable {
    public var session: AgentSession
    public var title: String
    public var priority: Int
    public var needsYou: Bool

    public init(session: AgentSession, title: String? = nil, priority: Int? = nil, needsYou: Bool = false) {
        self.session = session
        self.title = title ?? session.displayTitle
        self.priority = priority ?? session.priority
        self.needsYou = needsYou
    }
}

public enum AgentListOrdering {
    public static func activity(_ session: AgentSession) -> Int {
        max(session.finishedAtMs, session.startedAtMs, session.createdAtMs, session.mergedAtMs)
    }

    public static func runtime(_ session: AgentSession, nowMs: Int) -> Int {
        if session.durationMs > 0 { return session.durationMs }
        guard session.startedAtMs > 0 else { return 0 }
        let end = session.finishedAtMs > 0 ? session.finishedAtMs : nowMs
        return max(0, end - session.startedAtMs)
    }

    private static func statusRank(_ item: AgentListItem) -> Int {
        if item.needsYou { return 0 }
        switch item.session.statusLabel {
        case "Waiting": return 1
        case "Running": return 2
        case "Queued": return 3
        default: return item.session.isActive ? 2 : 4
        }
    }

    public static func sorted(_ items: [AgentListItem], by order: AgentListSort, nowMs: Int = 0) -> [AgentListItem] {
        sorted(items, by: order, nowMs: nowMs, item: { (value: AgentListItem) -> AgentListItem in value })
    }

    public static func sorted<T>(
        _ values: [T], by order: AgentListSort, nowMs: Int = 0, item: (T) -> AgentListItem
    ) -> [T] {
        let items: [AgentListItem] = values.map(item)
        let positions: [Int] = Array(items.indices).sorted { (lhs: Int, rhs: Int) -> Bool in
            if let decided = compare(items[lhs], items[rhs], order, nowMs: nowMs) { return decided }
            return lhs < rhs
        }
        return positions.map { (position: Int) -> T in values[position] }
    }

    private static func compare(_ a: AgentListItem, _ b: AgentListItem, _ order: AgentListSort, nowMs: Int) -> Bool? {
        func desc<T: Comparable>(_ x: T, _ y: T) -> Bool? { x == y ? nil : x > y }
        func asc<T: Comparable>(_ x: T, _ y: T) -> Bool? { x == y ? nil : x < y }
        let s = a.session
        let t = b.session
        switch order {
        case .priority:
            return asc(a.priority, b.priority) ?? desc(activity(s), activity(t))
        case .newestAdded:
            return desc(s.createdAtMs, t.createdAtMs) ?? desc(s.id, t.id)
        case .oldestAdded:
            return asc(s.createdAtMs, t.createdAtMs) ?? asc(s.id, t.id)
        case .recentActivity:
            return desc(activity(s), activity(t))
        case .status:
            return asc(statusRank(a), statusRank(b)) ?? desc(activity(s), activity(t))
        case .title:
            let x = a.title.lowercased()
            let y = b.title.lowercased()
            return x == y ? nil : x < y
        case .provider:
            let x = (s.providerLabel + " " + s.model).lowercased()
            let y = (t.providerLabel + " " + t.model).lowercased()
            return x == y ? desc(activity(s), activity(t)) : x < y
        case .issueNumber:
            if (s.issueNumber > 0) != (t.issueNumber > 0) { return s.issueNumber > 0 }
            return asc(s.issueNumber, t.issueNumber)
        case .tokenUsage:
            return desc(s.totalTokens, t.totalTokens)
        case .cost:
            return desc(s.costUsd, t.costUsd)
        case .runtime:
            return desc(runtime(s, nowMs: nowMs), runtime(t, nowMs: nowMs))
        case .filesChanged:
            return desc(s.filesChanged, t.filesChanged)
        }
    }

    public static func matches(_ item: AgentListItem, _ filter: AgentFilter, nowMs: Int = 0, recentWindowMs: Int = 86_400_000)
        -> Bool
    {
        let session = item.session
        switch filter {
        case .all, .desktop, .cloud:
            return true
        case .needsYou:
            return item.needsYou || session.statusLabel == "Waiting"
        case .running:
            return session.isActive && session.statusLabel != "Queued"
        case .queued:
            return session.statusLabel == "Queued"
        case .recent:
            return !session.isActive && (nowMs == 0 || nowMs - activity(session) <= recentWindowMs)
        }
    }

    public static func apply(
        _ items: [AgentListItem], filter: AgentFilter, sort: AgentListSort, nowMs: Int = 0
    ) -> [AgentListItem] {
        sorted(items.filter { matches($0, filter, nowMs: nowMs) }, by: sort, nowMs: nowMs)
    }
}
