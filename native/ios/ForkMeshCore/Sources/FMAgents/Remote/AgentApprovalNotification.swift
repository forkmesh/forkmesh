import FMApi
import FMBase
import Foundation

public struct AgentApprovalNotification: Equatable, Sendable {
    public static let category = "agent-approval"

    public enum Action: String, CaseIterable, Sendable {
        case approve = "agent-approval.approve"
        case deny = "agent-approval.deny"
        case open = "agent-approval.open"

        public var title: String {
            switch self {
            case .approve: return "Approve"
            case .deny: return "Deny"
            case .open: return "Open"
            }
        }

        public var requiresAuthentication: Bool { self == .approve }
        public var isDestructive: Bool { self == .deny }
        public var opensApp: Bool { self == .open }
    }

    public var approvalId: String
    public var sessionId: String

    public init(approvalId: String, sessionId: String = "") {
        self.approvalId = approvalId
        self.sessionId = sessionId
    }

    public init?(payload: JSONObject) {
        let layers = [payload, payload.obj("alert"), payload.obj("data"), payload.obj("aps")]
        let category = layers.map { $0.str("category") }.first { !$0.isEmpty } ?? ""
        guard category == Self.category else { return nil }
        let approval = layers.map { $0.str("approvalId") }.first { !$0.isEmpty } ?? ""
        guard !approval.isEmpty, approval.utf8.count <= 64 else { return nil }
        let session = layers.map { $0.str("sessionId") }.first { !$0.isEmpty } ?? ""
        self.init(approvalId: approval, sessionId: session)
    }

    public var route: Route { .approval(id: approvalId) }

    @MainActor
    public func perform(_ action: Action, queue: ApprovalsQueue) async -> Route? {
        switch action {
        case .approve:
            if await queue.approve(approvalId, lifetime: .once) { return nil }
            return route
        case .deny:
            if await queue.deny(approvalId) { return nil }
            return route
        case .open:
            return route
        }
    }
}
