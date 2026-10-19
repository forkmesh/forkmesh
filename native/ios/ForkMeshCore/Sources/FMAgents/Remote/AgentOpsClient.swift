import FMBase
import FMChat
import FMCode
import FMControl
import Foundation

public struct AgentOpsError: Error, Equatable, Sendable {
    public var code: String

    public init(_ code: String) { self.code = code }
}

@MainActor
public final class AgentOpsClient {
    public let family: DesktopControlFamily
    private let clock: Clock

    public init(channel: DesktopControlCalling, clock: Clock = SystemClock()) {
        family = DesktopControlFamily(.agentOps, channel: channel)
        self.clock = clock
    }

    public var nowMs: Int64 { clock.nowMillis }

    public func approvals() async throws -> [PendingApproval] {
        let reply = try await family.call("approvals")
        return reply.maps("approvals").compactMap(PendingApproval.init(json:))
    }

    public func prepareAnswer(_ approvalId: String, allow: Bool, lifetime: ApprovalLifetime = .once) async throws
        -> AgentOpsConfirmation
    {
        var params: JSONObject = ["id": .string(approvalId)]
        if allow { params["lifetime"] = .string(lifetime.rawValue) }
        return try await prepare(allow ? .approve : .deny, params)
    }

    public func answer(_ approvalId: String, allow: Bool, lifetime: ApprovalLifetime = .once, token: String)
        async throws
    {
        var params: JSONObject = [
            "id": .string(approvalId), "decision": .string(allow ? "allow" : "deny"), "token": .string(token),
        ]
        if allow { params["lifetime"] = .string(lifetime.rawValue) }
        _ = try await family.call("answer", params)
    }

    public func prepareMerge(sessionId: Int) async throws -> AgentOpsConfirmation {
        try await prepare(.merge, ["id": .int(Int64(sessionId))])
    }

    public func merge(sessionId: Int, token: String, typed: String) async throws -> MergeOutcome {
        let reply = try await family.call(
            "merge", ["id": .int(Int64(sessionId)), "token": .string(token), "typed": .string(typed)],
            timeout: 180)
        return MergeOutcome(json: reply)
    }

    public func mergeStats(sessionId: Int? = nil) async throws -> MergeLeaderboard {
        var params: JSONObject = [:]
        if let sessionId, sessionId > 0 { params["id"] = .int(Int64(sessionId)) }
        return MergeLeaderboard(json: try await family.call("mergestats", params, timeout: 60))
    }

    public func diff(sessionId: Int) async throws -> AgentDiff {
        AgentDiff(json: try await family.call("diff", ["id": .int(Int64(sessionId))], timeout: 60))
    }

    public func file(sessionId: Int, path: String, uncommitted: Bool, side: DiffImageSide) async throws -> Data {
        let reply = try await family.call(
            "file",
            [
                "id": .int(Int64(sessionId)), "path": .string(path),
                "change": .string(uncommitted ? "uncommitted" : "committed"), "side": .string(side.rawValue),
            ],
            timeout: 60)
        guard let data = Data(base64Encoded: reply.str("data")) else { throw AgentOpsError("bad_reply") }
        return data
    }

    public func prepareDelete(sessionId: Int) async throws -> AgentOpsConfirmation {
        try await prepare(.delete, ["id": .int(Int64(sessionId))])
    }

    @discardableResult
    public func delete(sessionId: Int, token: String) async throws -> AgentDeleteReport? {
        AgentDeleteReport(json: try await family.call(
            "delete", ["id": .int(Int64(sessionId)), "token": .string(token)], timeout: 90))
    }

    public func prepareBulk(_ action: BulkAgentAction, ids: [Int]) async throws -> AgentOpsConfirmation {
        try await prepare(action.opsAction, ["ids": .array(Self.canonical(ids).map { .int(Int64($0)) })])
    }

    public func bulk(_ action: BulkAgentAction, ids: [Int], token: String) async throws -> BulkOutcome {
        let reply = try await family.call(
            "bulk",
            [
                "action": .string(action.rawValue), "ids": .array(Self.canonical(ids).map { .int(Int64($0)) }),
                "token": .string(token),
            ], timeout: 60)
        return BulkOutcome(
            action: action, done: reply.list("done").map { $0.int() }, skipped: reply.list("skipped").map { $0.int() })
    }

    public func recycleBin() async throws -> RecycleBin {
        RecycleBin(json: try await family.call("bin", timeout: 30))
    }

    public func restoreRecycled(ids: [Int]) async throws -> (RecycleBin, RecycleBinChange) {
        let reply = try await family.call(
            "bin.restore", ["ids": .array(Self.canonical(ids).map { .int(Int64($0)) })], timeout: 60)
        return (
            RecycleBin(json: reply),
            .restored(done: reply.list("restored").map { $0.int() }, failed: reply.list("failed").map { $0.int() })
        )
    }

    public func preparePurge(ids: [Int]) async throws -> AgentOpsConfirmation {
        try await prepare(.purge, ["ids": .array(Self.canonical(ids).map { .int(Int64($0)) })])
    }

    public func purgeRecycled(ids: [Int], token: String) async throws -> (RecycleBin, RecycleBinChange) {
        let reply = try await family.call(
            "bin.purge",
            ["ids": .array(Self.canonical(ids).map { .int(Int64($0)) }), "token": .string(token)], timeout: 60)
        return (
            RecycleBin(json: reply),
            .purged(done: reply.list("purged").map { $0.int() }, skipped: reply.list("skipped").map { $0.int() })
        )
    }

    public func setRecycleRetention(hours: Int) async throws -> RecycleBin {
        RecycleBin(json: try await family.call("bin.retention", ["hours": .int(Int64(hours))], timeout: 30))
    }

    public func worktrees(owner: String, name: String) async throws -> WorktreeList {
        WorktreeList(json: try await family.call(
            "worktrees", ["owner": .string(owner), "name": .string(name)], timeout: 45))
    }

    public func usage(refresh: Bool = false) async throws -> UsageSummary {
        if refresh {
            return UsageSummary(json: try await family.call("usage", ["refresh": .bool(true)], timeout: 30))
        }
        return UsageSummary(json: try await family.call("usage"))
    }

    public func selectAccount(provider: String, account: String) async throws -> UsageSummary {
        UsageSummary(json: try await family.call(
            "accounts.select", ["provider": .string(provider), "account": .string(account)]))
    }

    public func renameAccount(provider: String, account: String, label: String) async throws -> UsageSummary {
        UsageSummary(json: try await family.call(
            "accounts.rename",
            ["provider": .string(provider), "account": .string(account), "label": .string(label)]))
    }

    public func addAccount(provider: String, label: String) async throws -> (UsageSummary, String) {
        let reply = try await family.call("accounts.add", ["provider": .string(provider), "label": .string(label)])
        return (UsageSummary(json: reply), reply.str("account"))
    }

    public func deleteAccount(provider: String, account: String) async throws -> UsageSummary {
        UsageSummary(json: try await family.call(
            "accounts.delete", ["provider": .string(provider), "account": .string(account)]))
    }

    public func grants() async throws -> [CapabilityGrant] {
        try await family.call("grants").maps("grants").map(CapabilityGrant.init(json:))
    }

    public func prepareRevoke(grantId: String) async throws -> AgentOpsConfirmation {
        try await prepare(.revoke, ["grantId": .string(grantId)])
    }

    public func revoke(grantId: String, token: String) async throws {
        _ = try await family.call("revoke", ["grantId": .string(grantId), "token": .string(token)])
    }

    @discardableResult
    public func onChanged(_ handler: @escaping @MainActor (JSONObject) -> Void) -> UUID {
        family.addEventTap { suffix, data in
            if suffix == "changed" { handler(data) }
        }
    }

    public func removeTap(_ id: UUID) { family.removeEventTap(id) }

    private func prepare(_ action: AgentOpsAction, _ params: JSONObject) async throws -> AgentOpsConfirmation {
        var body = params
        body["action"] = .string(action.rawValue)
        let issued = nowMs
        let reply = try await family.call("prepare", body)
        guard let confirmation = AgentOpsConfirmation(json: reply, issuedAtMs: issued) else {
            throw AgentOpsError("confirmation_required")
        }
        return confirmation
    }

    public nonisolated static func canonical(_ ids: [Int]) -> [Int] {
        Array(Set(ids.filter { $0 > 0 })).sorted()
    }

    public nonisolated static func errorCode(_ error: Error) -> String? {
        if let ops = error as? AgentOpsError { return ops.code }
        for child in Mirror(reflecting: error).children where child.label == "code" {
            if let code = child.value as? String { return code }
        }
        return nil
    }

    public nonisolated static func isUnsupported(_ error: Error) -> Bool {
        guard let code = errorCode(error) else { return false }
        return code == "unsupported" || code == "unknown_method"
    }

    public nonisolated static func describe(_ error: Error) -> String {
        guard let code = errorCode(error) else { return String(describing: error) }
        if code == "merge_failed", let control = error as? LocalControlException,
            !control.message.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        {
            return ControlErrorText.withReason("The merge did not finish.", control)
        }
        return describe(code: code)
    }

    public nonisolated static func describe(code: String) -> String {
        switch code {
        case "not_owner": return "Only the account that owns this desktop can manage its agents."
        case "unknown_approval": return "That request was already answered, or the agent stopped waiting."
        case "not_delivered": return "The desktop could not deliver the answer; the request stays denied."
        case "unknown_session": return "The desktop no longer has that agent session."
        case "no_checkout": return "The desktop has no checkout of that repository."
        case "agent_running": return "The agent is still working on that branch. Stop it first."
        case "conflicts": return "The branch conflicts with its base. Resolve the conflicts on the desktop or hand them to an agent."
        case "conflict_check_unavailable": return "The desktop's Git is too old to check for conflicts first. Merge on the desktop."
        case "no_base_branch": return "The desktop could not find the branch to merge into."
        case "merge_failed": return "The merge did not finish. The desktop's repository page says why."
        case "repo_unavailable": return "The desktop could not open that repository."
        case "name_mismatch": return "The typed name does not match the branch."
        case "branch_gone": return "The agent's branch and worktree are gone from the desktop."
        case "file_missing": return "The desktop could not find that file at this point in the branch."
        case "too_large": return "The image is too large to send to the phone."
        case "unknown_grant": return "That saved permission no longer exists."
        case "unknown_account": return "That account is no longer on the desktop."
        case "account_failed": return "The desktop could not save that account change."
        case "default_account": return "The default account cannot be deleted. Log it out instead."
        case "not_saved": return "The desktop revoked it but could not save the change."
        case "unavailable": return "The desktop's agent store is not open yet. Try again in a moment."
        case "bad_request", "unknown_action": return "The desktop did not understand that request."
        default: return ControlErrorText.describe(code: code)
        }
    }
}
