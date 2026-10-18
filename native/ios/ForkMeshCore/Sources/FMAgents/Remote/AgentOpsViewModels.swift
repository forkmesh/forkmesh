import FMApi
import FMBase
import FMCode
import FMControl
import FMDiff
import Foundation
import Observation

@MainActor
@Observable
public final class MergeCleanupModel {
    public let sessionId: Int
    public private(set) var confirmation: AgentOpsConfirmation?
    public private(set) var preparing = false
    public private(set) var running = false
    public private(set) var errorText = ""
    public private(set) var mergedBranch: String?
    public private(set) var mergeStats: MergeStats?
    public private(set) var conflicted = false

    @ObservationIgnored private let client: AgentOpsClient

    public init(client: AgentOpsClient, sessionId: Int) {
        self.client = client
        self.sessionId = sessionId
    }

    public var canRun: Bool {
        confirmation != nil && !running && mergedBranch == nil
    }

    public func prepare() async {
        preparing = true
        defer { preparing = false }
        do {
            confirmation = try await client.prepareMerge(sessionId: sessionId)
            errorText = ""
        } catch {
            confirmation = nil
            errorText = AgentOpsClient.describe(error)
        }
    }

    public var busy: Bool { preparing || running }

    @discardableResult
    public func mergeNow() async -> Bool {
        guard !busy, mergedBranch == nil else { return false }
        await prepare()
        return await run()
    }

    @discardableResult
    public func run() async -> Bool {
        guard let confirmation, canRun else { return false }
        if confirmation.isExpired(nowMs: client.nowMs) {
            self.confirmation = nil
            errorText = AgentOpsClient.describe(code: "confirmation_expired")
            return false
        }
        running = true
        defer { running = false }
        do {
            let outcome = try await client.merge(sessionId: sessionId, token: confirmation.token, typed: confirmation.typeToConfirm)
            mergedBranch = outcome.branch.isEmpty ? confirmation.typeToConfirm : outcome.branch
            mergeStats = outcome.stats
            errorText = ""
            conflicted = false
            self.confirmation = nil
            return true
        } catch {
            self.confirmation = nil
            conflicted = AgentOpsClient.errorCode(error) == "conflicts"
            errorText = AgentOpsClient.describe(error)
            return false
        }
    }
}

@MainActor
@Observable
public final class ConflictFixModel {
    public private(set) var sending = false
    public private(set) var sent = false
    public private(set) var errorText = ""

    public init() {}

    @discardableResult
    public func send(_ work: () async throws -> Void) async -> Bool {
        guard !sending, !sent else { return false }
        sending = true
        defer { sending = false }
        do {
            try await work()
            sent = true
            errorText = ""
            return true
        } catch {
            errorText = (error as? AgentLaunchException)?.message ?? AgentOpsClient.describe(error)
            return false
        }
    }
}

@MainActor
@Observable
public final class AgentDiffModel {
    public let sessionId: Int
    public private(set) var diff: AgentDiff?
    public private(set) var review: DiffReviewModel?
    public private(set) var loading = false
    public private(set) var errorText = ""

    @ObservationIgnored private let client: AgentOpsClient
    @ObservationIgnored private let store: KeyValueStore?

    public init(client: AgentOpsClient, sessionId: Int, store: KeyValueStore? = nil) {
        self.client = client
        self.sessionId = sessionId
        self.store = store
    }

    public static func reviewSections(_ diff: AgentDiff) -> [DiffReviewSection] {
        [
            DiffReviewSection(title: "Committed on the branch", files: UnifiedDiff.parse(diff.committed), group: "committed"),
            DiffReviewSection(title: "Uncommitted in the worktree", files: UnifiedDiff.parse(diff.uncommitted), group: "uncommitted"),
        ]
    }

    public static func reviewScope(_ diff: AgentDiff, sessionId: Int) -> String {
        diff.branch.isEmpty ? "agent/\(sessionId)" : "agent/\(diff.branch)"
    }

    private var imageLoader: DiffImageLoader {
        let client = client
        let sessionId = sessionId
        return { (request: DiffImageRequest) in
            do {
                return try await client.file(
                    sessionId: sessionId, path: request.path, uncommitted: request.group == "uncommitted", side: request.side)
            } catch {
                throw DiffImageError(
                    AgentOpsClient.isUnsupported(error)
                        ? "Update ForkMesh on the desktop to see images here." : AgentOpsClient.describe(error))
            }
        }
    }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            let loaded = try await client.diff(sessionId: sessionId)
            if loaded != diff || review == nil {
                review = DiffReviewModel(
                    sections: AgentDiffModel.reviewSections(loaded), scope: AgentDiffModel.reviewScope(loaded, sessionId: sessionId),
                    split: DiffViewSettings.shared.split, git: loaded.merged ? .merged : .unknown, store: store,
                    images: imageLoader)
            }
            diff = loaded
            errorText = ""
        } catch {
            errorText = AgentOpsClient.isUnsupported(error)
                ? "Update ForkMesh on the desktop to see an agent's changes here."
                : AgentOpsClient.describe(error)
        }
    }
}

@MainActor
@Observable
public final class AgentDeleteModel {
    public let sessionId: Int
    public private(set) var confirmation: AgentOpsConfirmation?
    public private(set) var preparing = false
    public private(set) var running = false
    public private(set) var errorText = ""
    public private(set) var deleted = false
    public private(set) var report: AgentDeleteReport?

    @ObservationIgnored private let client: AgentOpsClient

    public init(client: AgentOpsClient, sessionId: Int) {
        self.client = client
        self.sessionId = sessionId
    }

    public func prepare() async {
        preparing = true
        defer { preparing = false }
        do {
            confirmation = try await client.prepareDelete(sessionId: sessionId)
            errorText = ""
        } catch {
            confirmation = nil
            errorText = AgentOpsClient.isUnsupported(error)
                ? "Update ForkMesh on the desktop to delete agents from here."
                : AgentOpsClient.describe(error)
        }
    }

    @discardableResult
    public func run() async -> Bool {
        guard let confirmation, !running, !deleted else { return false }
        if confirmation.isExpired(nowMs: client.nowMs) {
            self.confirmation = nil
            errorText = AgentOpsClient.describe(code: "confirmation_expired")
            return false
        }
        running = true
        defer { running = false }
        do {
            let report = try await client.delete(sessionId: sessionId, token: confirmation.token)
            self.report = report
            self.confirmation = nil
            if let report, report.agentKept {
                errorText = report.summary(place: "on the desktop")
                return false
            }
            deleted = true
            errorText = ""
            return true
        } catch {
            self.confirmation = nil
            errorText = AgentOpsClient.describe(error)
            return false
        }
    }
}

@MainActor
@Observable
public final class BulkActionsModel {
    public private(set) var selection: Set<Int> = []
    public var selecting = false
    public private(set) var confirmation: AgentOpsConfirmation?
    public private(set) var pendingAction: BulkAgentAction?
    public private(set) var running = false
    public private(set) var errorText = ""
    public private(set) var lastOutcome: BulkOutcome?

    @ObservationIgnored private let client: AgentOpsClient
    @ObservationIgnored private var pendingIds: [Int] = []

    public init(client: AgentOpsClient) {
        self.client = client
    }

    public func toggle(_ id: Int) {
        if selection.contains(id) { selection.remove(id) } else { selection.insert(id) }
    }

    public func clear() {
        selection = []
        confirmation = nil
        pendingAction = nil
    }

    public func eligible(_ action: BulkAgentAction, in candidates: [BulkCandidate]) -> [Int] {
        let pool = selection.isEmpty ? candidates : candidates.filter { selection.contains($0.id) }
        return AgentOpsClient.canonical(pool.filter { $0.allows(action) }.map(\.id))
    }

    public func prepare(_ action: BulkAgentAction, in candidates: [BulkCandidate]) async {
        let ids = eligible(action, in: candidates)
        guard !ids.isEmpty else {
            errorText = action == .stop ? "Nothing to stop." : "Nothing to continue."
            return
        }
        do {
            confirmation = try await client.prepareBulk(action, ids: ids)
            pendingAction = action
            pendingIds = ids
            errorText = ""
        } catch {
            errorText = AgentOpsClient.describe(error)
        }
    }

    @discardableResult
    public func run() async -> BulkOutcome? {
        guard let confirmation, let action = pendingAction, !running else { return nil }
        if confirmation.isExpired(nowMs: client.nowMs) {
            self.confirmation = nil
            errorText = AgentOpsClient.describe(code: "confirmation_expired")
            return nil
        }
        running = true
        defer { running = false }
        do {
            let outcome = try await client.bulk(action, ids: pendingIds, token: confirmation.token)
            lastOutcome = outcome
            errorText = ""
            self.confirmation = nil
            pendingAction = nil
            selection = []
            selecting = false
            return outcome
        } catch {
            self.confirmation = nil
            errorText = AgentOpsClient.describe(error)
            return nil
        }
    }

    public func cancel() {
        confirmation = nil
        pendingAction = nil
    }
}

@MainActor
@Observable
public final class AgentUsageModel {
    public private(set) var summary: UsageSummary?
    public private(set) var loading = false
    public private(set) var errorText = ""
    public private(set) var unsupported = false

    @ObservationIgnored private let client: AgentOpsClient

    public init(client: AgentOpsClient) {
        self.client = client
    }

    public var nowMs: Int64 { client.nowMs }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            summary = try await client.usage()
            errorText = ""
            unsupported = false
        } catch {
            unsupported = AgentOpsClient.isUnsupported(error)
            errorText = unsupported ? "" : AgentOpsClient.describe(error)
        }
    }

    public var reminders: [UsageReminder] { summary?.reminders(nowMs: client.nowMs) ?? [] }

    public func selectAccount(provider: String, account: String) async -> String {
        do {
            summary = try await client.selectAccount(provider: provider, account: account)
            return ""
        } catch {
            return AgentOpsClient.describe(error)
        }
    }

    public func renameAccount(provider: String, account: String, label: String) async -> String {
        do {
            summary = try await client.renameAccount(provider: provider, account: account, label: label)
            return ""
        } catch {
            return AgentOpsClient.describe(error)
        }
    }

    public func addAccount(provider: String, label: String) async -> (account: String, error: String) {
        do {
            let (next, account) = try await client.addAccount(provider: provider, label: label)
            summary = next
            return (account, "")
        } catch {
            return ("", AgentOpsClient.describe(error))
        }
    }

    public func deleteAccount(provider: String, account: String) async -> String {
        do {
            summary = try await client.deleteAccount(provider: provider, account: account)
            return ""
        } catch {
            return AgentOpsClient.describe(error)
        }
    }
}

@MainActor
@Observable
public final class MergeLeaderboardModel {
    public let sessionId: Int?
    public private(set) var board = MergeLeaderboard()
    public private(set) var loading = false
    public private(set) var loaded = false
    public private(set) var errorText = ""

    @ObservationIgnored private let client: AgentOpsClient

    public init(client: AgentOpsClient, sessionId: Int? = nil) {
        self.client = client
        self.sessionId = sessionId
    }

    public func refresh() async {
        loading = true
        defer {
            loading = false
            loaded = true
        }
        do {
            board = try await client.mergeStats(sessionId: sessionId)
            errorText = ""
        } catch {
            errorText = AgentOpsClient.isUnsupported(error)
                ? "Update ForkMesh on the desktop to see the merge leaderboard here."
                : AgentOpsClient.describe(error)
        }
    }
}

@MainActor
@Observable
public final class WorktreesModel {
    public let owner: String
    public let name: String
    public private(set) var list = WorktreeList()
    public private(set) var loading = false
    public private(set) var loaded = false
    public private(set) var errorText = ""

    @ObservationIgnored private let client: AgentOpsClient

    public init(client: AgentOpsClient, owner: String, name: String) {
        self.client = client
        self.owner = owner
        self.name = name
    }

    public var agentWorktrees: [AgentWorktree] { list.worktrees.filter { !$0.isMain } }

    public func refresh() async {
        loading = true
        defer {
            loading = false
            loaded = true
        }
        do {
            list = try await client.worktrees(owner: owner, name: name)
            errorText = ""
        } catch {
            errorText = AgentOpsClient.describe(error)
        }
    }
}

@MainActor
@Observable
public final class CapabilityGrantsModel {
    public private(set) var grants: [CapabilityGrant] = []
    public private(set) var loading = false
    public private(set) var errorText = ""
    public private(set) var revoking: Set<String> = []

    @ObservationIgnored private let client: AgentOpsClient

    public init(client: AgentOpsClient) {
        self.client = client
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            grants = try await client.grants()
            errorText = ""
        } catch {
            errorText = AgentOpsClient.describe(error)
        }
    }

    public func prepareRevoke(_ grant: CapabilityGrant) async -> AgentOpsConfirmation? {
        do {
            return try await client.prepareRevoke(grantId: grant.id)
        } catch {
            errorText = AgentOpsClient.describe(error)
            return nil
        }
    }

    @discardableResult
    public func revoke(_ grant: CapabilityGrant, confirmation: AgentOpsConfirmation) async -> Bool {
        revoking.insert(grant.id)
        defer { revoking.remove(grant.id) }
        do {
            try await client.revoke(grantId: grant.id, token: confirmation.token)
            grants.removeAll { $0.id == grant.id }
            errorText = ""
            return true
        } catch {
            errorText = AgentOpsClient.describe(error)
            return false
        }
    }
}

@MainActor
@Observable
public final class RecycleBinModel {
    public private(set) var bin = RecycleBin()
    public private(set) var loading = false
    public private(set) var loaded = false
    public private(set) var busy = false
    public private(set) var unsupported = false
    public private(set) var errorText = ""
    public private(set) var note = ""
    public private(set) var selection: Set<Int> = []
    public private(set) var confirmation: AgentOpsConfirmation?

    @ObservationIgnored private let client: AgentOpsClient
    @ObservationIgnored private var pendingIds: [Int] = []

    public init(client: AgentOpsClient) {
        self.client = client
    }

    public var selectedIds: [Int] { bin.ids.filter { selection.contains($0) } }

    public func refresh() async {
        loading = true
        defer {
            loading = false
            loaded = true
        }
        do {
            apply(try await client.recycleBin())
            errorText = ""
            unsupported = false
        } catch {
            unsupported = AgentOpsClient.isUnsupported(error)
            errorText = unsupported
                ? "Update ForkMesh on the desktop to manage its recycle bin from here."
                : AgentOpsClient.describe(error)
        }
    }

    public func toggle(_ id: Int) {
        if selection.contains(id) { selection.remove(id) } else { selection.insert(id) }
    }

    public func selectAll() {
        selection = Set(bin.ids)
    }

    public func clearSelection() {
        selection = []
    }

    @discardableResult
    public func restore(_ ids: [Int]) async -> Bool {
        guard !ids.isEmpty, !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            let (next, change) = try await client.restoreRecycled(ids: ids)
            apply(next)
            note = change.message
            errorText = ""
            if case .restored(let done, _) = change { return !done.isEmpty }
            return false
        } catch {
            errorText = AgentOpsClient.describe(error)
            return false
        }
    }

    public func preparePurge(_ ids: [Int]) async {
        guard !ids.isEmpty, !busy else { return }
        do {
            confirmation = try await client.preparePurge(ids: ids)
            pendingIds = ids
            errorText = ""
        } catch {
            confirmation = nil
            errorText = AgentOpsClient.describe(error)
        }
    }

    @discardableResult
    public func purge() async -> Bool {
        guard let confirmation, !busy else { return false }
        self.confirmation = nil
        if confirmation.isExpired(nowMs: client.nowMs) {
            errorText = AgentOpsClient.describe(code: "confirmation_expired")
            return false
        }
        busy = true
        defer { busy = false }
        do {
            let (next, change) = try await client.purgeRecycled(ids: pendingIds, token: confirmation.token)
            apply(next)
            note = change.message
            errorText = ""
            return true
        } catch {
            errorText = AgentOpsClient.describe(error)
            return false
        }
    }

    public func cancelPurge() {
        confirmation = nil
        pendingIds = []
    }

    public func setRetention(hours: Int) async {
        let clamped = min(max(hours, bin.minHours), bin.maxHours)
        guard clamped != bin.retentionHours, !busy else { return }
        busy = true
        defer { busy = false }
        do {
            apply(try await client.setRecycleRetention(hours: clamped))
            errorText = ""
        } catch {
            errorText = AgentOpsClient.describe(error)
        }
    }

    private func apply(_ next: RecycleBin) {
        bin = next
        selection = selection.intersection(next.ids)
    }
}
