import FMApi
import FMBase
import Foundation
import Observation

public struct PullChecksApproval: Equatable, Sendable {
    public var required: Bool
    public var reason: String
    public var author: String
    public var authorTrust: String
    public var approvedBy: String

    public init(required: Bool = false, reason: String = "", author: String = "", authorTrust: String = "", approvedBy: String = "") {
        self.required = required
        self.reason = reason
        self.author = author
        self.authorTrust = authorTrust
        self.approvedBy = approvedBy
    }

    public init(json: JSONObject) {
        self.init(
            required: json.bool("required"), reason: json.str("reason"), author: String(json.str("author").prefix(64)),
            authorTrust: json.str("authorTrust"), approvedBy: String(json.str("approvedBy").prefix(64)))
    }

    public var reasonLabel: String {
        switch reason {
        case "author":
            return author.isEmpty
                ? "The author is not trusted to run code here."
                : "@\(author) is not trusted to run code here."
        case "workflows": return "This pull request changes files under .forkmesh/."
        default: return ""
        }
    }

    public var trustLabel: String {
        switch authorTrust {
        case "writer": return "@\(author) can write to this repository, so checks run automatically."
        case "allowlist": return "Checks run automatically for @\(author)."
        default: return ""
        }
    }
}

public struct PullChecks: Equatable, Sendable {
    public var number: Int
    public var configured: Bool
    public var head: String
    public var runs: [ActionsRunSummary]
    public var approval: PullChecksApproval
    public var canApprove: Bool
    public var viewer: String
    public var previousHeads: Int

    public init(
        number: Int = 0, configured: Bool = false, head: String = "", runs: [ActionsRunSummary] = [],
        approval: PullChecksApproval = PullChecksApproval(), canApprove: Bool = false, viewer: String = "",
        previousHeads: Int = 0
    ) {
        self.number = number
        self.configured = configured
        self.head = head
        self.runs = runs
        self.approval = approval
        self.canApprove = canApprove
        self.viewer = viewer
        self.previousHeads = previousHeads
    }

    public init(json: JSONObject) {
        self.init(
            number: ModelJSON.int(json["number"]), configured: json.bool("configured"),
            head: json.str("head").lowercased(),
            runs: json.maps("runs").prefix(64).compactMap(ActionsRunSummary.init(json:)).sorted { $0.displayName < $1.displayName },
            approval: PullChecksApproval(json: json.obj("approval")), canApprove: json.bool("canApprove"),
            viewer: json.str("viewer"), previousHeads: max(0, ModelJSON.int(json["previousHeads"])))
    }

    public var shortHead: String { String(head.prefix(7)) }
    public var hasValidHead: Bool { NotificationActionRouter.isCommitId(head) }
    public var summary: PullChecksSummary {
        PullChecksSummary(runs: runs.map { run in
            var copy = run
            copy.pullNumber = number
            return copy
        }, pull: number)
    }
}

public enum PullChecksAction: String, Sendable, CaseIterable, Identifiable {
    case approve, trust, untrust

    public var id: String { rawValue }

    public func label(author: String) -> String {
        switch self {
        case .approve: return "Approve and run"
        case .trust: return "Always run for @\(author)"
        case .untrust: return "Stop running automatically"
        }
    }

    public var needsAuthentication: Bool { self != .untrust }
    public var isDestructive: Bool { self == .untrust }
}

@MainActor
@Observable
public final class PullChecksModel {
    public let repo: RepoRef
    public let number: Int
    public private(set) var checks: PullChecks?
    public private(set) var unavailable = false
    public private(set) var loading = false
    public private(set) var busy: PullChecksAction?
    public private(set) var error: String?
    public private(set) var notice: ActionsNotice?

    @ObservationIgnored private let client: CIClient

    public init(client: CIClient, repo: RepoRef, number: Int) {
        self.client = client
        self.repo = repo
        self.number = number
    }

    public var isVisible: Bool { !unavailable && checks != nil }

    public var actions: [PullChecksAction] {
        guard let checks, checks.canApprove, busy == nil else { return [] }
        var actions: [PullChecksAction] = []
        let approval = checks.approval
        if approval.required && checks.hasValidHead { actions.append(.approve) }
        if !approval.author.isEmpty && approval.authorTrust.isEmpty { actions.append(.trust) }
        if !approval.author.isEmpty && approval.authorTrust == "allowlist" { actions.append(.untrust) }
        return actions
    }

    public var hasActiveRuns: Bool { checks?.runs.contains { $0.status.isActive } ?? false }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            checks = try await client.pullChecks(repo, number: number)
            unavailable = false
            error = nil
        } catch let failure as CIError where failure.status == 404 {
            checks = nil
            unavailable = true
            error = nil
        } catch {
            self.error = "\(error)"
        }
    }

    public func body(for action: PullChecksAction) -> JSONObject? {
        guard let checks else { return nil }
        switch action {
        case .approve:
            guard checks.hasValidHead else { return nil }
            return ["action": "approve", "head": .string(checks.head)]
        case .trust, .untrust:
            let author = checks.approval.author
            guard !author.isEmpty else { return nil }
            return ["action": .string(action.rawValue), "account": .string(author)]
        }
    }

    @discardableResult
    public func perform(_ action: PullChecksAction) async -> Bool {
        guard busy == nil, let body = body(for: action) else { return false }
        let author = checks?.approval.author ?? ""
        busy = action
        notice = nil
        var ok = false
        do {
            let reply = try await client.pullChecksAction(repo, number: number, body)
            notice = ActionsNotice(text: PullChecksModel.outcome(action, reply: reply, author: author), good: true)
            ok = true
        } catch {
            notice = ActionsNotice(text: "\(action.label(author: author)): \(error)", good: false)
        }
        busy = nil
        await load()
        return ok
    }

    public static func outcome(_ action: PullChecksAction, reply: JSONObject, author: String) -> String {
        let approved = ModelJSON.int(reply["approved"])
        let runs = approved == 1 ? "1 check" : "\(approved) checks"
        switch action {
        case .approve:
            return approved > 0 ? "Approved: \(runs) started." : "Approved."
        case .trust:
            return approved > 0
                ? "Checks now run automatically for @\(author). Started \(runs)."
                : "Checks now run automatically for @\(author)."
        case .untrust:
            return "Checks for @\(author) wait for approval again."
        }
    }
}
