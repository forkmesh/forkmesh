import FMApi
import FMPlanning
import Foundation

#if canImport(ActivityKit) && os(iOS)
import ActivityKit
#endif

public struct AgentRunAttributes: Codable, Hashable, Sendable {
    public var sessionId: String
    public var repo: String
    public var title: String

    public init(sessionId: String, repo: String, title: String) {
        self.sessionId = sessionId
        self.repo = repo
        self.title = title
    }

    public struct ContentState: Codable, Hashable, Sendable {
        public var status: String
        public var title: String?
        public var repo: String?
        public var detail: String?
        public var provider: String?
        public var branch: String?
        public var turns: Int?
        public var filesChanged: Int?
        public var startedAt: Int?
        public var updatedAt: Int?
        public var progress: Double?
        public var needsApproval: Bool
        public var approvalId: String?

        public init(
            status: String, title: String? = nil, repo: String? = nil, detail: String? = nil, provider: String? = nil,
            branch: String? = nil, turns: Int? = nil, filesChanged: Int? = nil, startedAt: Int? = nil,
            updatedAt: Int? = nil, progress: Double? = nil, needsApproval: Bool = false, approvalId: String? = nil
        ) {
            self.status = status
            self.title = title
            self.repo = repo
            self.detail = detail
            self.provider = provider
            self.branch = branch
            self.turns = turns
            self.filesChanged = filesChanged
            self.startedAt = startedAt
            self.updatedAt = updatedAt
            self.progress = progress
            self.needsApproval = needsApproval
            self.approvalId = approvalId
        }

        enum CodingKeys: String, CodingKey {
            case status, title, repo, detail, provider, branch, turns, filesChanged, startedAt, updatedAt, progress
            case needsApproval, approvalId
        }

        public init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            status = (try? c.decodeIfPresent(String.self, forKey: .status)) ?? "running"
            title = try? c.decodeIfPresent(String.self, forKey: .title)
            repo = try? c.decodeIfPresent(String.self, forKey: .repo)
            detail = try? c.decodeIfPresent(String.self, forKey: .detail)
            provider = try? c.decodeIfPresent(String.self, forKey: .provider)
            branch = try? c.decodeIfPresent(String.self, forKey: .branch)
            turns = try? c.decodeIfPresent(Int.self, forKey: .turns)
            filesChanged = try? c.decodeIfPresent(Int.self, forKey: .filesChanged)
            startedAt = try? c.decodeIfPresent(Int.self, forKey: .startedAt)
            updatedAt = try? c.decodeIfPresent(Int.self, forKey: .updatedAt)
            progress = try? c.decodeIfPresent(Double.self, forKey: .progress)
            needsApproval = (try? c.decodeIfPresent(Bool.self, forKey: .needsApproval)) ?? false
            approvalId = try? c.decodeIfPresent(String.self, forKey: .approvalId)
        }

        public var state: AgentRunState {
            AgentRunState(status: status, isActive: true, needsApproval: needsApproval)
        }

        public var startedDate: Date? { AgentRunAttributes.date(from: startedAt) }

        public var approvalRoute: Route? {
            guard needsApproval, let approvalId, !approvalId.isEmpty else { return nil }
            return .approval(id: approvalId)
        }

        public static func from(_ card: AgentRunCard) -> ContentState {
            ContentState(
                status: card.state.rawValue,
                title: card.title.isEmpty ? nil : card.title,
                repo: card.repo.isEmpty ? nil : card.repo,
                detail: card.detail.isEmpty ? nil : card.detail,
                provider: card.provider.isEmpty ? nil : card.provider,
                startedAt: card.startedAtMs > 0 ? Int(card.startedAtMs) : nil,
                needsApproval: card.state == .needsApproval,
                approvalId: card.approvalId.isEmpty ? nil : card.approvalId)
        }
    }

    public static func from(_ card: AgentRunCard) -> AgentRunAttributes? {
        guard let key = AgentRunKey.make(card.id) else { return nil }
        return AgentRunAttributes(sessionId: key, repo: card.repo, title: card.title)
    }

    public var route: Route { .agent(id: sessionId) }

    static func date(from value: Int?) -> Date? {
        guard let value else { return nil }
        if value >= 1_500_000_000_000 && value < 10_000_000_000_000 {
            return Date(timeIntervalSince1970: TimeInterval(value) / 1_000)
        }
        if value >= 1_500_000_000 && value < 10_000_000_000 {
            return Date(timeIntervalSince1970: TimeInterval(value))
        }
        return nil
    }
}

public enum AgentRunKey {
    public static let maxLength = 64

    public static func isValid(_ key: String) -> Bool {
        guard !key.isEmpty, key.count <= maxLength else { return false }
        return key.unicodeScalars.allSatisfy { scalar in
            scalar.isASCII && (CharacterSet.alphanumerics.contains(scalar) || "_.:-".unicodeScalars.contains(scalar))
        }
    }

    public static func make(_ runId: String) -> String? {
        isValid(runId) ? runId : nil
    }
}

public struct PlanningTimerAttributes: Codable, Hashable, Sendable {
    public typealias ContentState = TimerActivitySnapshot

    public var timerId: String

    public init(timerId: String) {
        self.timerId = timerId
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

public enum LiveActivityAction: Equatable, Sendable {
    case start(AgentRunAttributes, AgentRunAttributes.ContentState)
    case update(String, AgentRunAttributes.ContentState)
    case end(String, AgentRunAttributes.ContentState?)
}

public enum LiveActivityPlanner {
    public static let maxAgentActivities = 3

    public static func plan(
        agents: AgentsSurface, existing: [String: AgentRunAttributes.ContentState], canStart: Bool,
        limit: Int = LiveActivityPlanner.maxAgentActivities
    ) -> [LiveActivityAction] {
        var actions: [LiveActivityAction] = []
        var byKey: [String: AgentRunCard] = [:]
        for card in agents.runs {
            if let key = AgentRunKey.make(card.id), byKey[key] == nil { byKey[key] = card }
        }
        var liveCount = 0
        for key in existing.keys.sorted() {
            guard let old = existing[key] else { continue }
            if let card = byKey[key], card.state.isActive {
                liveCount += 1
                let next = AgentRunAttributes.ContentState.from(card)
                if next != old { actions.append(.update(key, next)) }
            } else if let card = byKey[key] {
                actions.append(.end(key, AgentRunAttributes.ContentState.from(card)))
            } else {
                actions.append(.end(key, nil))
            }
        }
        guard canStart else { return actions }
        for card in agents.active where liveCount < limit {
            guard let attributes = AgentRunAttributes.from(card), existing[attributes.sessionId] == nil else { continue }
            actions.append(.start(attributes, AgentRunAttributes.ContentState.from(card)))
            liveCount += 1
        }
        return actions
    }
}

#if canImport(ActivityKit) && os(iOS)
extension AgentRunAttributes: ActivityAttributes {}

extension PlanningTimerAttributes: ActivityAttributes {}

extension WalkActivityAttributes: ActivityAttributes {}
#endif
