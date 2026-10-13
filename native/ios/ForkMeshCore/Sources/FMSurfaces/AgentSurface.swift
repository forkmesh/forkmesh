import FMApi
import Foundation

public enum AgentRunState: String, Codable, Hashable, Sendable, CaseIterable {
    case queued
    case running
    case waiting
    case needsApproval = "needs_approval"
    case completed
    case failed
    case stopped

    public init(status: String, isActive: Bool, needsApproval: Bool = false) {
        if needsApproval {
            self = .needsApproval
            return
        }
        let key = status.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            .replacingOccurrences(of: "_", with: "-")
        switch key {
        case "queued", "pending", "claimed": self = .queued
        case "running", "started", "in-progress": self = .running
        case "waiting", "waiting-for-input": self = .waiting
        case "needs-approval": self = .needsApproval
        case "done", "completed", "success", "succeeded", "merged": self = .completed
        case "failed", "error": self = .failed
        case "stopped", "cancelled", "canceled": self = .stopped
        default: self = isActive ? .running : .completed
        }
    }

    public var isActive: Bool {
        switch self {
        case .queued, .running, .waiting, .needsApproval: return true
        case .completed, .failed, .stopped: return false
        }
    }

    public var label: String {
        switch self {
        case .queued: return "Queued"
        case .running: return "Running"
        case .waiting: return "Waiting"
        case .needsApproval: return "Needs you"
        case .completed: return "Done"
        case .failed: return "Failed"
        case .stopped: return "Stopped"
        }
    }

    public var systemImage: String {
        switch self {
        case .queued: return "clock"
        case .running: return "bolt.fill"
        case .waiting: return "hourglass"
        case .needsApproval: return "hand.raised.fill"
        case .completed: return "checkmark.circle.fill"
        case .failed: return "xmark.octagon.fill"
        case .stopped: return "stop.circle.fill"
        }
    }

    var rank: Int {
        switch self {
        case .needsApproval: return 0
        case .waiting: return 1
        case .running: return 2
        case .queued: return 3
        case .failed: return 4
        case .completed, .stopped: return 5
        }
    }
}

public struct AgentRunInput: Hashable, Sendable {
    public var id: String
    public var title: String
    public var repoOwner: String
    public var repoName: String
    public var status: String
    public var isActive: Bool
    public var provider: String
    public var model: String
    public var detail: String
    public var updatedAtMs: Int64
    public var startedAtMs: Int64
    public var localId: Int
    public var isDesktop: Bool
    public var canStop: Bool

    public init(
        id: String, title: String, repoOwner: String, repoName: String, status: String, isActive: Bool,
        provider: String = "", model: String = "", detail: String = "", updatedAtMs: Int64 = 0,
        startedAtMs: Int64 = 0, localId: Int = 0, isDesktop: Bool = false, canStop: Bool = false
    ) {
        self.id = id
        self.title = title
        self.repoOwner = repoOwner
        self.repoName = repoName
        self.status = status
        self.isActive = isActive
        self.provider = provider
        self.model = model
        self.detail = detail
        self.updatedAtMs = updatedAtMs
        self.startedAtMs = startedAtMs
        self.localId = localId
        self.isDesktop = isDesktop
        self.canStop = canStop
    }
}

public struct AgentApprovalInput: Hashable, Sendable {
    public var id: String
    public var sessionId: Int
    public var question: String
    public var askedAtMs: Int64

    public init(id: String, sessionId: Int, question: String, askedAtMs: Int64) {
        self.id = id
        self.sessionId = sessionId
        self.question = question
        self.askedAtMs = askedAtMs
    }
}

public struct AgentRunCard: Codable, Hashable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var repo: String
    public var detail: String
    public var provider: String
    public var model: String
    public var state: AgentRunState
    public var updatedAtMs: Int64
    public var startedAtMs: Int64
    public var approvalId: String
    public var canStop: Bool

    public init(
        id: String, title: String, repo: String, detail: String = "", provider: String = "", model: String = "",
        state: AgentRunState, updatedAtMs: Int64 = 0, startedAtMs: Int64 = 0, approvalId: String = "",
        canStop: Bool = false
    ) {
        self.id = id
        self.title = title
        self.repo = repo
        self.detail = detail
        self.provider = provider
        self.model = model
        self.state = state
        self.updatedAtMs = updatedAtMs
        self.startedAtMs = startedAtMs
        self.approvalId = approvalId
        self.canStop = canStop
    }

    public var route: Route {
        approvalId.isEmpty ? .agent(id: id) : .approval(id: approvalId)
    }

    public var repoRef: RepoRef? { RepoRef(slug: repo) }
}

public struct AgentsSurface: Codable, Hashable, Sendable {
    public static let maxRuns = 6

    public var needsYou: Int
    public var running: Int
    public var queued: Int
    public var total: Int
    public var runs: [AgentRunCard]

    public init(needsYou: Int = 0, running: Int = 0, queued: Int = 0, total: Int? = nil, runs: [AgentRunCard] = []) {
        self.needsYou = needsYou
        self.running = running
        self.queued = queued
        self.total = max(total ?? runs.count, 0)
        self.runs = runs
    }

    public static let empty = AgentsSurface()

    public var latest: AgentRunCard? { runs.first }

    public var active: [AgentRunCard] { runs.filter(\.state.isActive) }

    public var activeCount: Int { running + queued + needsYou }

    public static func build(
        runs: [AgentRunInput], approvals: [AgentApprovalInput], limit: Int = AgentsSurface.maxRuns
    ) -> AgentsSurface {
        var approvalBySession: [Int: AgentApprovalInput] = [:]
        for approval in approvals.sorted(by: { $0.askedAtMs > $1.askedAtMs }) where approval.sessionId > 0 {
            if approvalBySession[approval.sessionId] == nil {
                approvalBySession[approval.sessionId] = approval
            }
        }
        var seen: Set<String> = []
        var cards: [AgentRunCard] = []
        for run in runs where !run.id.isEmpty && !seen.contains(run.id) {
            seen.insert(run.id)
            let approval: AgentApprovalInput? = run.isDesktop && run.localId > 0 ? approvalBySession[run.localId] : nil
            let state = AgentRunState(status: run.status, isActive: run.isActive, needsApproval: approval != nil)
            let repo = run.repoOwner.isEmpty ? run.repoName : "\(run.repoOwner)/\(run.repoName)"
            let detail = approval.map { SurfaceText.clip($0.question, 160) } ?? SurfaceText.clip(run.detail, 160)
            cards.append(AgentRunCard(
                id: SurfaceText.clip(run.id, 80),
                title: SurfaceText.clip(run.title.isEmpty ? "Agent run" : run.title, 80),
                repo: SurfaceText.clip(repo, 120),
                detail: detail,
                provider: SurfaceText.clip(run.provider, 40),
                model: SurfaceText.clip(run.model, 60),
                state: state,
                updatedAtMs: max(run.updatedAtMs, approval?.askedAtMs ?? 0),
                startedAtMs: run.startedAtMs,
                approvalId: approval.map { SurfaceText.clip($0.id, 120) } ?? "",
                canStop: run.canStop && state.isActive))
        }
        let orphanApprovals = approvals.filter { approval in
            !cards.contains { $0.approvalId == approval.id }
        }
        let needsYou = cards.filter { $0.state == .needsApproval }.count + orphanApprovals.count
        let running = cards.filter { $0.state == .running || $0.state == .waiting }.count
        let queued = cards.filter { $0.state == .queued }.count
        let ordered = cards.sorted { a, b in
            if a.state.isActive != b.state.isActive { return a.state.isActive }
            if a.state.rank != b.state.rank { return a.state.rank < b.state.rank }
            if a.updatedAtMs != b.updatedAtMs { return a.updatedAtMs > b.updatedAtMs }
            return a.id < b.id
        }
        return AgentsSurface(
            needsYou: needsYou, running: running, queued: queued, total: cards.count,
            runs: Array(ordered.prefix(max(0, limit))))
    }

    public var summaryLine: String {
        if needsYou == 0 && running == 0 && queued == 0 { return "No agents running" }
        var parts: [String] = []
        if needsYou > 0 { parts.append("\(needsYou) need\(needsYou == 1 ? "s" : "") you") }
        if running > 0 { parts.append("\(running) running") }
        if queued > 0 { parts.append("\(queued) queued") }
        return parts.joined(separator: ", ")
    }
}

enum SurfaceText {
    static func clip(_ text: String, _ limit: Int) -> String {
        let flat = text.replacingOccurrences(of: "\n", with: " ").replacingOccurrences(of: "\r", with: " ")
            .trimmingCharacters(in: .whitespaces)
        if flat.count <= limit { return flat }
        return String(flat.prefix(max(0, limit - 1))) + "…"
    }
}
