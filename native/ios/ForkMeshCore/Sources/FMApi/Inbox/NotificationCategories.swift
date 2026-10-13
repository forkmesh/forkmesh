import FMBase
import Foundation

public enum NotificationActionId: String, CaseIterable, Sendable {
    case open = "fm.open"
    case markRead = "fm.markRead"
    case reply = "fm.reply"
    case approve = "fm.approve"
    case deny = "fm.deny"
    case merge = "fm.merge"
    case continueRun = "fm.continue"
    case retry = "fm.retry"
    case sendToAgent = "fm.sendToAgent"
    case approvePull = "fm.approvePull"
    case openLog = "fm.openLog"

    public var title: String {
        switch self {
        case .open: return "Open"
        case .markRead: return "Mark read"
        case .reply: return "Reply"
        case .approve: return "Approve"
        case .deny: return "Deny"
        case .merge: return "Merge & clean up"
        case .continueRun: return "Continue"
        case .retry: return "Retry"
        case .sendToAgent: return "Send to agent"
        case .approvePull: return "Approve PR"
        case .openLog: return "Open log"
        }
    }

    public var requiresAuthentication: Bool { self == .approve || self == .merge || self == .approvePull }
    public var isDestructive: Bool { self == .merge || self == .deny }
    public var opensApp: Bool { self == .open || self == .openLog || self == .sendToAgent }
    public var isTextInput: Bool { self == .reply || self == .continueRun }
}

public enum NotificationCategoryId: String, CaseIterable, Sendable {
    case agentApproval = "agent-approval"
    case agentFinished = "agent-finished"
    case agentFailed = "agent-failed"
    case pullReview = "pr-review"
    case mention
    case dm
    case ciFailed = "ci-failed"
    case ping

    public var actions: [NotificationActionId] {
        switch self {
        case .agentApproval: return [.approve, .deny, .open]
        case .agentFinished: return [.open, .continueRun, .merge]
        case .agentFailed: return [.open, .retry, .sendToAgent]
        case .pullReview: return [.open, .approvePull]
        case .mention, .dm: return [.reply, .markRead]
        case .ciFailed: return [.openLog, .sendToAgent]
        case .ping: return [.open, .markRead]
        }
    }

    public var isTimeSensitive: Bool { self == .agentApproval }

    public static func forPing(kind: String, topic: String = "") -> NotificationCategoryId {
        switch topic.lowercased() {
        case "direct-messages", "dm": return .dm
        case "chat": return .mention
        default: break
        }
        switch kind.lowercased() {
        case "agent_approval", "agent-approval": return .agentApproval
        case "agent_finished", "agent-finished": return .agentFinished
        case "agent_failed", "agent-failed": return .agentFailed
        case "pull_submitted", "review_requested", "pr-review": return .pullReview
        case "mention": return .mention
        case "ci_failed", "ci-failed", "action_failed": return .ciFailed
        default: return .ping
        }
    }
}

public struct NotificationPayload: Equatable, Sendable {
    public var category: NotificationCategoryId
    public var alertId: String
    public var href: String
    public var topic: String
    public var sessionId: String
    public var approvalId: String
    public var repo: String
    public var number: Int
    public var channel: String
    public var runId: String

    public init(category: String, fields: [String: String]) {
        func value(_ key: String) -> String { fields[key]?.trimmingCharacters(in: .whitespaces) ?? "" }
        self.category = NotificationCategoryId(rawValue: category)
            ?? NotificationCategoryId.forPing(kind: value("kind"), topic: value("topic"))
        alertId = value("alertId")
        href = value("href")
        topic = value("topic")
        sessionId = value("sessionId")
        approvalId = value("approvalId")
        repo = value("repo")
        number = Int(value("number")) ?? 0
        channel = value("channel")
        runId = value("runId")
    }

    public static let userInfoKeys = [
        "kind", "topic", "alertId", "href", "sessionId", "approvalId", "repo", "number", "channel", "runId", "origin", "host",
    ]
}

public enum NotificationCommand: Equatable, Sendable {
    case open(Route?, href: String)
    case markRead(alertId: String)
    case reply(channel: String, text: String)
    case approve(sessionId: String, approvalId: String)
    case deny(sessionId: String, approvalId: String)
    case merge(sessionId: String)
    case continueRun(sessionId: String, text: String)
    case retry(sessionId: String)
    case compose(ComposeTarget)
    case approvePull(RepoRef, number: Int)
    case ignore(reason: String)
}

public enum NotificationActionRouter {
    public static func route(for payload: NotificationPayload) -> Route? {
        switch payload.category {
        case .agentApproval where !payload.sessionId.isEmpty && !payload.approvalId.isEmpty:
            return .approval(id: "\(payload.sessionId)/\(payload.approvalId)")
        case .agentApproval, .agentFinished, .agentFailed:
            if !payload.sessionId.isEmpty { return .agent(id: payload.sessionId) }
        case .ciFailed:
            if let repo = RepoRef(slug: payload.repo), !payload.runId.isEmpty { return .run(repo, id: payload.runId) }
        case .dm:
            if !payload.channel.isEmpty { return .dm(user: payload.channel) }
        case .mention:
            if !payload.channel.isEmpty && payload.href.isEmpty { return .channel(id: payload.channel) }
        default:
            break
        }
        if let link = NotificationDeepLink.parse(payload.href) { return InboxRoutes.route(link) }
        if let repo = RepoRef(slug: payload.repo) {
            return payload.number > 0 ? .pull(repo, number: payload.number, tab: .conversation) : .repo(repo, section: nil)
        }
        return payload.category == .ping ? .inbox(.all) : nil
    }

    public static func command(actionId: String, payload: NotificationPayload, text: String?) -> NotificationCommand {
        guard let action = NotificationActionId(rawValue: actionId) else {
            return .open(route(for: payload), href: payload.href)
        }
        let session = payload.sessionId
        let typed = (text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        switch action {
        case .open, .openLog:
            return .open(route(for: payload), href: payload.href)
        case .markRead:
            return payload.alertId.isEmpty ? .ignore(reason: "no alert id") : .markRead(alertId: payload.alertId)
        case .reply:
            if payload.channel.isEmpty { return .ignore(reason: "no conversation") }
            if typed.isEmpty { return .ignore(reason: "empty reply") }
            return .reply(channel: payload.channel, text: String(typed.prefix(4_000)))
        case .approve, .deny:
            guard !session.isEmpty, !payload.approvalId.isEmpty else { return .ignore(reason: "no approval") }
            return action == .approve
                ? .approve(sessionId: session, approvalId: payload.approvalId)
                : .deny(sessionId: session, approvalId: payload.approvalId)
        case .merge:
            return session.isEmpty ? .ignore(reason: "no session") : .merge(sessionId: session)
        case .continueRun:
            if session.isEmpty { return .ignore(reason: "no session") }
            return .continueRun(sessionId: session, text: String(typed.prefix(4_000)))
        case .retry:
            return session.isEmpty ? .ignore(reason: "no session") : .retry(sessionId: session)
        case .sendToAgent:
            let repo = RepoRef(slug: payload.repo)
            let prompt = payload.category == .ciFailed
                ? "A CI run failed\(payload.repo.isEmpty ? "" : " in \(payload.repo)"). Find the cause and fix it."
                : "The agent run \(session) failed. Look at what went wrong and try again."
            return .compose(ComposeTarget(repo: repo, agentId: session.isEmpty ? nil : session, text: prompt))
        case .approvePull:
            guard let repo = RepoRef(slug: payload.repo), payload.number > 0 else { return .ignore(reason: "no pull request") }
            return .approvePull(repo, number: payload.number)
        }
    }
}

@MainActor
public protocol AgentApprovalDeciding: AnyObject {
    func decide(sessionId: String, approvalId: String, approve: Bool) async throws
}

@MainActor
public protocol AgentSessionActing: AnyObject {
    func continueRun(sessionId: String, text: String) async throws
    func mergeAndCleanUp(sessionId: String) async throws
    func retry(sessionId: String) async throws
}

@MainActor
public protocol NotificationReplySending: AnyObject {
    func reply(channel: String, text: String) async throws
}
