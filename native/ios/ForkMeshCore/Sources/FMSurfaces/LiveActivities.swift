import FMApi
import FMPlanning
import Foundation

#if canImport(ActivityKit) && os(iOS)
import ActivityKit
#endif

public struct ActivityShortcutInput: Hashable, Sendable {
    public var id: Int
    public var title: String
    public var repo: String
    public var running: Bool
    public var startedAtMs: Int64
    public var expectedMs: Int64

    public init(id: Int, title: String, repo: String, running: Bool, startedAtMs: Int64, expectedMs: Int64) {
        self.id = id
        self.title = title
        self.repo = repo
        self.running = running
        self.startedAtMs = startedAtMs
        self.expectedMs = expectedMs
    }
}

public struct ActivitySummaryAttributes: Codable, Hashable, Sendable {
    public static let maxAgents = 3
    public static let maxShortcuts = 2

    public var scope: String

    public init(scope: String = "summary") {
        self.scope = scope
    }

    public struct Agent: Codable, Hashable, Sendable, Identifiable {
        public var id: String
        public var title: String
        public var repo: String
        public var detail: String
        public var provider: String
        public var state: AgentRunState
        public var startedAtMs: Int64
        public var approvalId: String
        public var canStop: Bool

        public init(
            id: String, title: String, repo: String = "", detail: String = "", provider: String = "",
            state: AgentRunState, startedAtMs: Int64 = 0, approvalId: String = "", canStop: Bool = false
        ) {
            self.id = id
            self.title = title
            self.repo = repo
            self.detail = detail
            self.provider = provider
            self.state = state
            self.startedAtMs = startedAtMs
            self.approvalId = approvalId
            self.canStop = canStop
        }

        public init(_ card: AgentRunCard) {
            self.init(
                id: card.id, title: SurfaceText.clip(card.title, 48), repo: SurfaceText.clip(card.repo, 48),
                detail: SurfaceText.clip(card.detail, 80), provider: SurfaceText.clip(card.provider, 20),
                state: card.state, startedAtMs: card.startedAtMs, approvalId: card.approvalId,
                canStop: card.canStop)
        }

        public var startedAt: Date? { ActivitySummaryAttributes.date(startedAtMs) }

        public var route: Route { approvalId.isEmpty ? .agent(id: id) : .approval(id: approvalId) }
    }

    public struct Shortcut: Codable, Hashable, Sendable, Identifiable {
        public var id: Int
        public var title: String
        public var repo: String
        public var startedAtMs: Int64
        public var expectedMs: Int64

        public init(id: Int, title: String, repo: String, startedAtMs: Int64, expectedMs: Int64) {
            self.id = id
            self.title = title
            self.repo = repo
            self.startedAtMs = startedAtMs
            self.expectedMs = expectedMs
        }

        public var startedAt: Date? { ActivitySummaryAttributes.date(startedAtMs) }

        public var expectedEnd: Date? {
            guard expectedMs > 0, let startedAt else { return nil }
            return startedAt.addingTimeInterval(TimeInterval(expectedMs) / 1_000)
        }

        public var route: Route { .shortcuts(RepoRef(slug: repo)) }
    }

    public struct ContentState: Codable, Hashable, Sendable {
        public var needsYou: Int
        public var running: Int
        public var queued: Int
        public var agents: [Agent]
        public var shortcutsRunning: Int
        public var shortcuts: [Shortcut]
        public var timer: TimerActivitySnapshot?
        public var unread: Int

        public init(
            needsYou: Int = 0, running: Int = 0, queued: Int = 0, agents: [Agent] = [], shortcutsRunning: Int? = nil,
            shortcuts: [Shortcut] = [], timer: TimerActivitySnapshot? = nil, unread: Int = 0
        ) {
            self.needsYou = needsYou
            self.running = running
            self.queued = queued
            self.agents = agents
            self.shortcutsRunning = max(shortcutsRunning ?? shortcuts.count, shortcuts.count)
            self.shortcuts = shortcuts
            self.timer = timer
            self.unread = unread
        }

        public var activeAgents: Int { needsYou + running + queued }

        public var isEmpty: Bool { activeAgents == 0 && shortcutsRunning == 0 && timer == nil }

        public var lead: Agent? { agents.first }

        public var leadShortcut: Shortcut? { shortcuts.first }

        public var pendingApproval: Agent? { agents.first { !$0.approvalId.isEmpty } }

        public var approvalRoute: Route? { pendingApproval.map { .approval(id: $0.approvalId) } }

        public var stoppable: Agent? {
            guard activeAgents == 1, pendingApproval == nil, let lead, lead.canStop else { return nil }
            return lead
        }

        public var route: Route {
            if let approvalRoute { return approvalRoute }
            if activeAgents == 1, agents.count == 1, let lead { return lead.route }
            if activeAgents > 0 { return .agents(needsYou > 0 ? .needsYou : .running) }
            if let leadShortcut { return leadShortcut.route }
            if timer != nil { return .timers }
            return .agents(.all)
        }

        public var agentLine: String {
            var parts: [String] = []
            if needsYou > 0 { parts.append("\(needsYou) need\(needsYou == 1 ? "s" : "") you") }
            if running > 0 { parts.append("\(running) running") }
            if queued > 0 { parts.append("\(queued) queued") }
            return parts.joined(separator: ", ")
        }

        public var shortcutLine: String {
            shortcutsRunning == 0 ? "" : "\(shortcutsRunning) shortcut\(shortcutsRunning == 1 ? "" : "s")"
        }

        public var summaryLine: String {
            let parts = [agentLine, shortcutLine].filter { !$0.isEmpty }
            if !parts.isEmpty { return parts.joined(separator: " · ") }
            if let timer { return timer.label }
            return "All done"
        }

        public var compactCount: Int { activeAgents + shortcutsRunning }

        public var systemImage: String {
            if needsYou > 0 { return AgentRunState.needsApproval.systemImage }
            if activeAgents > 0 { return "cpu" }
            if shortcutsRunning > 0 { return "bolt.horizontal.fill" }
            if let timer { return timer.kind.systemImage }
            return AgentRunState.completed.systemImage
        }

        public var leadState: AgentRunState {
            if needsYou > 0 { return .needsApproval }
            if running > 0 { return .running }
            if queued > 0 { return .queued }
            return .completed
        }

        public static func build(
            agents: AgentsSurface, shortcuts: [ActivityShortcutInput] = [], timer: TimerActivitySnapshot? = nil,
            unread: Int = 0
        ) -> ContentState {
            let running = shortcuts.filter { $0.running && $0.id > 0 }.sorted { $0.id > $1.id }
            return ContentState(
                needsYou: agents.needsYou, running: agents.running, queued: agents.queued,
                agents: agents.active.prefix(ActivitySummaryAttributes.maxAgents).map { Agent($0) },
                shortcutsRunning: running.count,
                shortcuts: running.prefix(ActivitySummaryAttributes.maxShortcuts).map {
                    Shortcut(
                        id: $0.id, title: SurfaceText.clip($0.title, 48), repo: SurfaceText.clip($0.repo, 120),
                        startedAtMs: $0.startedAtMs, expectedMs: $0.expectedMs)
                },
                timer: timer, unread: max(0, unread))
        }

        public func keepingAgents(of previous: ContentState) -> ContentState {
            var kept = self
            kept.needsYou = previous.needsYou
            kept.running = previous.running
            kept.queued = previous.queued
            kept.agents = previous.agents
            return kept
        }
    }

    static func date(_ ms: Int64) -> Date? {
        guard ms >= 1_500_000_000_000 && ms < 10_000_000_000_000 else { return nil }
        return Date(timeIntervalSince1970: TimeInterval(ms) / 1_000)
    }
}

extension TimerActivitySnapshot {
    public var endsAt: Date? { endsAtMs > 0 ? Date(timeIntervalSince1970: TimeInterval(endsAtMs) / 1_000) : nil }

    public var countsFrom: Date? {
        countsFromMs > 0 ? Date(timeIntervalSince1970: TimeInterval(countsFromMs) / 1_000) : nil
    }

    public var route: Route { .timers }
}

public struct WalkActivityAttributes: Codable, Hashable, Sendable {
    public var startedAtMs: Int64

    public init(startedAtMs: Int64) {
        self.startedAtMs = startedAtMs
    }

    public init(startedAt: Date) {
        startedAtMs = Int64((startedAt.timeIntervalSince1970 * 1_000).rounded())
    }

    public struct ContentState: Codable, Hashable, Sendable {
        public var steps: Int
        public var todaySteps: Int
        public var distance: String
        public var endedAtMs: Int64

        public init(steps: Int, todaySteps: Int, distance: String, endedAtMs: Int64 = 0) {
            self.steps = steps
            self.todaySteps = todaySteps
            self.distance = distance
            self.endedAtMs = endedAtMs
        }

        public var ended: Bool { endedAtMs > 0 }

        public var endedAt: Date? {
            ended ? Date(timeIntervalSince1970: TimeInterval(endedAtMs) / 1_000) : nil
        }
    }

    public var startedAt: Date { Date(timeIntervalSince1970: TimeInterval(startedAtMs) / 1_000) }

    public var route: Route { .steps }
}

public enum ActivitySummaryAction: Equatable, Sendable {
    case start(ActivitySummaryAttributes.ContentState)
    case update(ActivitySummaryAttributes.ContentState)
    case end(ActivitySummaryAttributes.ContentState)
}

public enum ActivitySummaryPlanner {
    public static func plan(
        next: ActivitySummaryAttributes.ContentState, current: ActivitySummaryAttributes.ContentState?,
        canStart: Bool, agentsReliable: Bool = true
    ) -> ActivitySummaryAction? {
        guard let current else { return canStart && !next.isEmpty ? .start(next) : nil }
        var next = next
        if !agentsReliable && next.activeAgents == 0 && current.activeAgents > 0 {
            next = next.keepingAgents(of: current)
        }
        if next.isEmpty { return .end(next) }
        return next == current ? nil : .update(next)
    }
}

#if canImport(ActivityKit) && os(iOS)
extension ActivitySummaryAttributes: ActivityAttributes {}

extension WalkActivityAttributes: ActivityAttributes {}
#endif
