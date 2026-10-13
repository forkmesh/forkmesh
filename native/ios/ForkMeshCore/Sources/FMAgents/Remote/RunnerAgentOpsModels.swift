import FMApi
import FMBase
import FMCode
import FMDiff
import Foundation
import Observation

@MainActor
public protocol RunnerAgentsApi: AnyObject {
    func runnerAgentDiff(
        _ owner: String, _ repo: String, _ sessionId: String, page: Int, pageSize: Int, orgScope: Bool
    ) async throws -> RunnerAgentDiffPage
    func mergeRunnerAgent(
        _ owner: String, _ repo: String, _ sessionId: String, cleanup: Bool, orgScope: Bool
    ) async throws -> RunnerAgentMergeResult
    func cleanupRunnerAgent(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
    ) async throws -> RunnerAgentMergeResult
    func discardRunnerAgentBranch(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
    ) async throws -> RunnerAgentMergeResult
    func stopAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> AgentStopResult
    func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
    func linkRunnerAgentPull(
        _ owner: String, _ repo: String, _ sessionId: String, number: Int, orgScope: Bool
    ) async throws -> RunnerAgentPullLink
    func agentLaunchSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> AgentLaunchSession
    func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison
    func runnerDefaultBranch(_ owner: String, _ repo: String) async -> String
    func updateRunnerAgent(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
    ) async throws -> RunnerAgentMergeResult
    func completeTask(_ taskId: String, note: String) async throws
}

extension ApiService: RunnerAgentsApi {}

@MainActor
public protocol RunnerPullInbox: AnyObject {
    func openNewPull(
        _ owner: String, _ name: String, title: String, base: String, head: String, patch: String, commits: String,
        description: String, authorName: String
    ) async throws -> Int
}

extension InboxService: RunnerPullInbox {}

@MainActor
public protocol RunnerAgentStarting: AnyObject {
    func runnerBranchNames(_ owner: String, _ repo: String) async -> [String]
    func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String, model: String,
        targetNode: String, title: String, createPr: Bool, issueNumber: Int, orgScope: Bool,
        attachments: [PromptAttachment], baseBranch: String, branchName: String
    ) async throws -> AgentQueueResult
}

extension ApiService: RunnerAgentStarting {}

public struct RunnerAgentRef: Equatable, Sendable {
    public var owner: String
    public var repo: String
    public var sessionId: String
    public var orgScope: Bool
    public var branch: String
    public var base: String
    public var pull: Int

    public init(
        owner: String, repo: String, sessionId: String, orgScope: Bool = false, branch: String = "", base: String = "",
        pull: Int = 0
    ) {
        self.owner = owner
        self.repo = repo
        self.sessionId = sessionId
        self.orgScope = orgScope
        self.branch = branch
        self.base = base
        self.pull = pull
    }
}

public enum RunnerAgentOps {
    public static func diff(from pages: [RunnerAgentDiffPage]) -> AgentDiff? {
        guard var raw = pages.first?.raw else { return nil }
        raw["committed"] = .string(pages.map(\.committed).joined())
        raw["truncated"] = .bool(pages.last?.hasMore ?? false)
        raw["id"] = .int(0)
        return AgentDiff(json: raw)
    }

    public static func describe(_ error: Error) -> String {
        if let refusal = error as? AgentLaunchException { return refusal.message }
        return dartErrorText(error)
    }

    public static func deleteNote(branch: String, merged: Bool) -> String {
        if branch.isEmpty { return "The run is removed from your agents. Nothing on ForkMesh changes." }
        if merged { return "The run is removed from your agents, and its branch \(branch) is deleted on ForkMesh." }
        return "The run is removed from your agents, and its branch \(branch) is deleted on ForkMesh with any unmerged work."
    }

    public static func validOid(_ value: String) -> Bool {
        let hex = value.lowercased()
        guard hex.count == 40 || hex.count == 64 else { return false }
        return hex.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func describePull(_ error: Error) -> String {
        if let failure = error as? ApiException { return failure.message }
        return describe(error)
    }

    public static func keepsRunDelete(_ code: String) -> Bool { code == "no_branch" || code == "forbidden" }

    public static func updateTitle(base: String) -> String {
        "Update from \(base.dartTrimmed.isEmpty ? "base" : base.dartTrimmed)"
    }

    public static func updateSummary(_ result: RunnerAgentMergeResult, branch: String, base: String) -> String {
        if !result.message.dartTrimmed.isEmpty { return result.message.dartTrimmed }
        let name = result.branch.isEmpty ? (branch.isEmpty ? "The branch" : branch) : result.branch
        let from = result.base.isEmpty ? (base.isEmpty ? "its base" : base) : result.base
        return result.status == "already" ? "\(name) is already up to date with \(from)." : "Updated \(name) from \(from)."
    }

    public static func cleanupSummary(_ result: RunnerAgentMergeResult, branch: String) -> String {
        let name = result.branch.isEmpty ? branch : result.branch
        if !result.cleanupPending.isEmpty {
            return "Removed \(name) here; \(result.cleanupPending.joined(separator: ", ")) still has it and is retried."
        }
        return name.isEmpty ? "Cleaned up." : "Cleaned up \(name)."
    }
}

@MainActor
@Observable
public final class RunnerDiffModel {
    public let ref: RunnerAgentRef
    public private(set) var pages: [RunnerAgentDiffPage] = []
    public private(set) var diff: AgentDiff?
    public private(set) var review: DiffReviewModel?
    public private(set) var loading = false
    public private(set) var errorText = ""

    @ObservationIgnored private let api: RunnerAgentsApi
    @ObservationIgnored private let store: KeyValueStore?
    @ObservationIgnored private let pageSize: Int

    public init(api: RunnerAgentsApi, ref: RunnerAgentRef, store: KeyValueStore? = nil, pageSize: Int = 25) {
        self.api = api
        self.ref = ref
        self.store = store
        self.pageSize = pageSize
    }

    public var first: RunnerAgentDiffPage? { pages.first }
    public var hasMore: Bool { pages.last?.hasMore ?? false }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            let page = try await api.runnerAgentDiff(
                ref.owner, ref.repo, ref.sessionId, page: 0, pageSize: pageSize, orgScope: ref.orgScope)
            pages = [page]
            rebuild(force: true)
            errorText = ""
        } catch {
            errorText = RunnerAgentOps.describe(error)
        }
    }

    public func loadMore() async {
        guard let last = pages.last, last.hasMore, !loading else { return }
        loading = true
        defer { loading = false }
        do {
            let page = try await api.runnerAgentDiff(
                ref.owner, ref.repo, ref.sessionId, page: last.page + 1, pageSize: pageSize, orgScope: ref.orgScope)
            pages.append(page)
            rebuild(force: true)
            errorText = ""
        } catch {
            errorText = RunnerAgentOps.describe(error)
        }
    }

    private func rebuild(force: Bool) {
        guard let built = RunnerAgentOps.diff(from: pages) else { return }
        if force || built != diff {
            review = DiffReviewModel(
                sections: [DiffReviewSection(title: "Committed on the branch", files: UnifiedDiff.parse(built.committed))],
                scope: built.branch.isEmpty ? "agent/\(ref.sessionId)" : built.branch,
                split: DiffViewSettings.shared.split, git: built.merged ? .merged : .unknown, store: store)
        }
        diff = built
    }
}

@MainActor
@Observable
public final class RunnerMergeModel {
    public let ref: RunnerAgentRef
    public private(set) var running = false
    public private(set) var errorText = ""
    public private(set) var result: RunnerAgentMergeResult?
    public private(set) var outcome: MergeOutcome?
    public private(set) var notice = ""
    public private(set) var conflicted = false
    public private(set) var taskNote = ""
    public private(set) var taskId = ""

    @ObservationIgnored private let api: RunnerAgentsApi

    public init(api: RunnerAgentsApi, ref: RunnerAgentRef) {
        self.api = api
        self.ref = ref
    }

    public var done: Bool { outcome != nil || (result?.isCleaned ?? false) }

    public var updated: Bool { result?.isUpdated ?? false }

    public var mergeOid: String { result?.isMerged == true ? result?.mergeOid ?? "" : "" }

    public var repo: RepoRef { RepoRef(owner: ref.owner, name: ref.repo) }

    public var landedNotice: String { [notice, taskNote].filter { !$0.isEmpty }.joined(separator: "\n") }

    public var updateTitle: String { RunnerAgentOps.updateTitle(base: ref.base) }

    public var updateProgress: String {
        let branch = ref.branch.isEmpty ? "the agent's branch" : ref.branch
        let base = ref.base.isEmpty ? "its base branch" : ref.base
        return "Merging \(base) into \(branch) on ForkMesh so the branch has the latest changes."
    }

    public var confirmText: String {
        let branch = ref.branch.isEmpty ? "the agent's branch" : ref.branch
        let base = ref.base.isEmpty ? "its base branch" : ref.base
        return "Merges \(branch) into \(base) on ForkMesh, then deletes \(branch). Its last commit is kept as a recovery ref."
    }

    @discardableResult
    public func merge() async -> Bool {
        let ok = await run { try await self.api.mergeRunnerAgent(self.ref.owner, self.ref.repo, self.ref.sessionId, cleanup: true, orgScope: self.ref.orgScope) }
        if ok && (result?.isMerged ?? false) { await completeLinkedTask() }
        return ok
    }

    @discardableResult
    public func cleanup() async -> Bool {
        await run { try await self.api.cleanupRunnerAgent(self.ref.owner, self.ref.repo, self.ref.sessionId, orgScope: self.ref.orgScope) }
    }

    @discardableResult
    public func update() async -> Bool {
        await run { try await self.api.updateRunnerAgent(self.ref.owner, self.ref.repo, self.ref.sessionId, orgScope: self.ref.orgScope) }
    }

    @discardableResult
    public func completeLinkedTask() async -> Bool {
        guard let result, result.isMerged, taskId.isEmpty else { return false }
        let session: AgentLaunchSession
        do {
            session = try await api.agentLaunchSession(ref.owner, ref.repo, ref.sessionId, orgScope: ref.orgScope)
        } catch {
            return false
        }
        let id = RunnerAgentText.taskId(session)
        guard !id.isEmpty else { return false }
        taskId = id
        let branch = result.branch.isEmpty ? (ref.branch.isEmpty ? session.branchName : ref.branch) : result.branch
        var base = result.base.isEmpty ? ref.base : result.base
        if base.isEmpty { base = session.baseBranch }
        let note = RunnerAgentText.completionNote(
            branch: branch, base: base, mergeOid: result.mergeOid, message: result.message)
        do {
            try await api.completeTask(id, note: note)
            taskNote = RunnerAgentText.taskCompleted
            return true
        } catch {
            taskNote = RunnerAgentText.taskNotCompleted(RunnerAgentOps.describePull(error))
            return false
        }
    }

    private func run(_ work: () async throws -> RunnerAgentMergeResult) async -> Bool {
        guard !running else { return false }
        running = true
        defer { running = false }
        do {
            let answer = try await work()
            result = answer
            errorText = ""
            conflicted = false
            if answer.isPending {
                notice = answer.message
                return false
            }
            if answer.isUpdated {
                notice = RunnerAgentOps.updateSummary(answer, branch: ref.branch, base: ref.base)
                return true
            }
            if answer.isMerged {
                var merged = MergeOutcome(json: answer.raw)
                if merged.branch.isEmpty { merged.branch = ref.branch }
                outcome = merged
                notice = answer.cleanupPending.isEmpty ? "" : RunnerAgentOps.cleanupSummary(answer, branch: ref.branch)
            } else {
                notice = RunnerAgentOps.cleanupSummary(answer, branch: ref.branch)
            }
            return true
        } catch {
            errorText = RunnerAgentOps.describe(error)
            conflicted = (error as? AgentLaunchException)?.code == "merge_conflict"
            return false
        }
    }

    public var successLine: String {
        if let result, result.isUpdated { return RunnerAgentOps.updateSummary(result, branch: ref.branch, base: ref.base) }
        if let outcome { return outcome.message(fallbackBranch: ref.branch) }
        if let result, result.isCleaned { return RunnerAgentOps.cleanupSummary(result, branch: ref.branch) }
        return ""
    }
}

@MainActor
@Observable
public final class RunnerDeleteModel {
    public let ref: RunnerAgentRef
    public let merged: Bool
    public let active: Bool
    public private(set) var running = false
    public private(set) var deleted = false
    public private(set) var errorText = ""
    public private(set) var branchPart: AgentDeletePart?

    public static let discardTries = 10
    public static let discardPauseMs = 2000

    @ObservationIgnored private let api: RunnerAgentsApi
    @ObservationIgnored private let pause: (Int) async -> Void

    public init(
        api: RunnerAgentsApi, ref: RunnerAgentRef, merged: Bool = false, active: Bool = false,
        pause: @escaping (Int) async -> Void = { try? await Task.sleep(nanoseconds: UInt64($0) * 1_000_000) }
    ) {
        self.api = api
        self.ref = ref
        self.merged = merged
        self.active = active
        self.pause = pause
    }

    public var note: String { RunnerAgentOps.deleteNote(branch: ref.branch, merged: merged) }

    public var report: AgentDeleteReport {
        var parts = [AgentDeletePart(kind: "agent", status: deleted ? "deleted" : "kept")]
        if let branchPart { parts.append(branchPart) }
        if ref.pull > 0 {
            parts.append(
                AgentDeletePart(kind: "pull", name: String(ref.pull), status: "kept", reason: merged ? "merged" : "relay"))
        }
        return AgentDeleteReport(parts: parts, known: ["branch", "pull"])
    }

    @discardableResult
    public func run() async -> Bool {
        guard !running, !deleted else { return false }
        running = true
        defer { running = false }
        do {
            if active {
                _ = try? await api.stopAgentSession(ref.owner, ref.repo, ref.sessionId, orgScope: ref.orgScope)
            }
            branchPart = nil
            if !ref.branch.isEmpty { branchPart = try await discardBranch() }
            try await api.cancelAgentSession(ref.owner, ref.repo, ref.sessionId, orgScope: ref.orgScope)
            deleted = true
            errorText = ""
            return true
        } catch {
            errorText = RunnerAgentOps.describe(error)
            return false
        }
    }

    private func discardBranch() async throws -> AgentDeletePart {
        let branch = ref.branch
        for attempt in 1...Self.discardTries {
            do {
                let result = try await api.discardRunnerAgentBranch(
                    ref.owner, ref.repo, ref.sessionId, orgScope: ref.orgScope)
                let gone = result.message.hasSuffix("already gone.")
                return AgentDeletePart(kind: "branch", name: branch, status: gone ? "absent" : "deleted")
            } catch let error as AgentLaunchException where RunnerAgentOps.keepsRunDelete(error.code) {
                return error.code == "no_branch"
                    ? AgentDeletePart(kind: "branch", name: branch, status: "absent")
                    : AgentDeletePart(kind: "branch", name: branch, status: "kept", reason: "forbidden")
            } catch let error as AgentLaunchException where error.code == "session_running" {
                if attempt == Self.discardTries {
                    return AgentDeletePart(kind: "branch", name: branch, status: "kept", reason: "running")
                }
                if attempt == 1 {
                    _ = try? await api.stopAgentSession(ref.owner, ref.repo, ref.sessionId, orgScope: ref.orgScope)
                }
                await pause(Self.discardPauseMs)
            }
        }
        return AgentDeletePart(kind: "branch", name: branch, status: "kept", reason: "running")
    }
}

@MainActor
@Observable
public final class RunnerPullModel {
    public let ref: RunnerAgentRef
    public private(set) var running = false
    public private(set) var errorText = ""
    public private(set) var notice = ""
    public private(set) var number = 0

    @ObservationIgnored private let api: RunnerAgentsApi
    @ObservationIgnored private let inbox: RunnerPullInbox?

    public init(api: RunnerAgentsApi, inbox: RunnerPullInbox?, ref: RunnerAgentRef) {
        self.api = api
        self.inbox = inbox
        self.ref = ref
    }

    @discardableResult
    public func open() async -> Bool {
        guard !running else { return false }
        running = true
        defer { running = false }
        errorText = ""
        notice = ""
        guard let inbox else {
            errorText = "Sign in on this phone before opening a pull request."
            return false
        }
        let session: AgentLaunchSession
        do {
            session = try await api.agentLaunchSession(ref.owner, ref.repo, ref.sessionId, orgScope: ref.orgScope)
        } catch {
            errorText = RunnerAgentOps.describe(error)
            return false
        }
        if session.prNumber > 0 {
            number = session.prNumber
            notice = "This run already has pull request #\(session.prNumber)."
            return true
        }
        var base = session.baseBranch.dartTrimmed
        if base.isEmpty { base = ref.base.dartTrimmed }
        if base.isEmpty { base = await api.runnerDefaultBranch(ref.owner, ref.repo) }
        let draft = RunnerPullDraft(session: session, branch: ref.branch, base: base)
        if draft.head.isEmpty {
            errorText = RunnerAgentText.linkMessage(["error": "no_branch"], 409)
            return false
        }
        let comparison: RepoComparison
        do {
            comparison = try await api.compare(ref.owner, ref.repo, base: draft.base, head: draft.head)
        } catch {
            errorText = RunnerAgentText.pullBranchMissing(draft.head)
            return false
        }
        if !RunnerAgentOps.validOid(comparison.baseOid) || !RunnerAgentOps.validOid(comparison.headOid) {
            errorText = RunnerAgentText.pullNotOpened("invalid_comparison")
            return false
        }
        if comparison.patch.dartTrimmed.isEmpty {
            errorText = RunnerAgentText.pullNoChanges(branch: draft.head, base: draft.base)
            return false
        }
        if comparison.patch.utf8.count + comparison.commits.utf8.count > RunnerAgentText.pullPatchLimit {
            errorText = RunnerAgentText.pullTooLarge(draft.head)
            return false
        }
        let opened: Int
        do {
            opened = try await inbox.openNewPull(
                ref.owner, ref.repo, title: draft.title, base: draft.base, head: draft.head, patch: comparison.patch,
                commits: comparison.commits, description: draft.description, authorName: "")
        } catch {
            errorText = RunnerAgentText.pullNotOpened(RunnerAgentOps.describePull(error))
            return false
        }
        guard opened > 0 else {
            errorText = RunnerAgentText.pullNumberPending
            return false
        }
        number = opened
        do {
            let link = try await api.linkRunnerAgentPull(
                ref.owner, ref.repo, ref.sessionId, number: opened, orgScope: ref.orgScope)
            notice = link.message.dartTrimmed.isEmpty ? RunnerAgentText.pullOpened(opened) : link.message
        } catch {
            errorText = RunnerAgentText.pullLinkFailed(opened, RunnerAgentOps.describe(error))
        }
        return true
    }
}
