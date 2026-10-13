import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// How urgent a queued task is. A phone runs one agent at a time, so the
/// queue is what makes "do this next" meaningful. The raw value is the stored
/// priority.
public enum AgentTaskPriority: Int, Sendable {
    case high = 0
    case normal = 1
}

public enum AgentTaskState: String, Sendable {
    case queued, running, done, failed, cancelled
}

/// One unit of work: a prompt against one local repository.
public struct AgentTask: Equatable, Identifiable, Sendable {
    public let id: String
    public let repoName: String
    public let prompt: String
    public let priority: AgentTaskPriority
    public let createdAt: Date
    public let state: AgentTaskState
    /// Photos sent with `prompt` as image blocks. Held in memory only: a task
    /// that outlives the app is re-typed, not replayed with stale photos.
    public let images: [PromptAttachment]

    public init(
        id: String,
        repoName: String,
        prompt: String,
        priority: AgentTaskPriority = .normal,
        createdAt: Date = Date(),
        state: AgentTaskState = .queued,
        images: [PromptAttachment] = []
    ) {
        self.id = id
        self.repoName = repoName
        self.prompt = prompt
        self.priority = priority
        self.createdAt = createdAt
        self.state = state
        self.images = images
    }

    public func copyWith(state: AgentTaskState? = nil) -> AgentTask {
        AgentTask(
            id: id, repoName: repoName, prompt: prompt, priority: priority, createdAt: createdAt,
            state: state ?? self.state, images: images)
    }
}

/// A branch an agent session created, and what became of it.
///
/// This record is what makes an interrupted session recoverable. The app can
/// be killed mid-run (by the user, by iOS reclaiming memory, by a crash) and
/// nothing in the repository says "an agent was here". The record does.
public struct AgentBranchRecord: Equatable, Sendable {
    public let sessionId: String
    public let repoName: String
    public let branch: String
    public let baseBranch: String
    public let startedAt: Date
    public let finishedAt: Date?

    public init(sessionId: String, repoName: String, branch: String, baseBranch: String, startedAt: Date, finishedAt: Date? = nil) {
        self.sessionId = sessionId
        self.repoName = repoName
        self.branch = branch
        self.baseBranch = baseBranch
        self.startedAt = startedAt
        self.finishedAt = finishedAt
    }

    public var isFinished: Bool { finishedAt != nil }

    /// Dates as ISO 8601 local time without a zone.
    public var json: JSONObject {
        var out: JSONObject = [
            "sessionId": .string(sessionId),
            "repoName": .string(repoName),
            "branch": .string(branch),
            "baseBranch": .string(baseBranch),
            "startedAt": .string(DartDateFormat.iso8601(startedAt)),
        ]
        if let finishedAt { out["finishedAt"] = .string(DartDateFormat.iso8601(finishedAt)) }
        return out
    }

    public init?(json value: JSONValue) {
        guard let object = value.objectValue else { return nil }
        guard let started = Self.parseDate(object["startedAt"]) else { return nil }
        let branch = object["branch"].map(lenientText) ?? ""
        if branch.isEmpty { return nil }
        let base = object["baseBranch"].flatMap { $0.isNull ? nil : $0.text } ?? "main"
        self.init(
            sessionId: object["sessionId"].map(lenientText) ?? "",
            repoName: object["repoName"].map(lenientText) ?? "",
            branch: branch,
            baseBranch: base,
            startedAt: started,
            finishedAt: Self.parseDate(object["finishedAt"])
        )
    }

    /// `DateTime.tryParse('${value}')`.
    static func parseDate(_ value: JSONValue?) -> Date? {
        guard let value, !value.isNull else { return nil }
        guard let ms = DartCore.dateTimeTryParseMs(value.text) else { return nil }
        return Date(epochMillis: Int64(ms))
    }
}

/// One run's transcript and lifecycle, owned by `AgentSessionService`.
///
/// The events list is the source of truth a screen renders from: it exists
/// independently of any view, so navigating away and back does not lose the
/// conversation. `waitUntilDone()` returns only after the post-run cleanup
/// (base-branch restore, record close) has finished; when it returns, the
/// repository is safe to operate on again.
@MainActor
@Observable
public final class AgentRunHandle {
    public let task: AgentTask

    /// The branch this run works on, and the branch it forked from: what a
    /// "create pull request" flow needs once the run is over.
    public let branch: String
    public let baseBranch: String

    public private(set) var events: [AgentEvent] = []
    public private(set) var eventImages: [Int: [PromptAttachment]] = [:]
    public private(set) var isComplete = false
    @ObservationIgnored private var waiters: [CheckedContinuation<Void, Never>] = []

    init(task: AgentTask, branch: String, baseBranch: String) {
        self.task = task
        self.branch = branch
        self.baseBranch = baseBranch
    }

    /// Returns after the run AND its cleanup have finished. Never throws: a
    /// failed run is reported through its events.
    public func waitUntilDone() async {
        if isComplete { return }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            waiters.append(continuation)
        }
    }

    /// The run's last `AgentDone`, once there is one.
    public var lastDone: AgentDone? {
        events.reversed().lazy.compactMap(\.done).first
    }

    func append(_ event: AgentEvent, images: [PromptAttachment] = []) {
        if !images.isEmpty { eventImages[events.count] = images }
        events.append(event)
    }

    public func attachments(at index: Int) -> [AgentAttachmentRef] {
        (eventImages[index] ?? []).map(AgentAttachmentRef.local)
    }

    func complete() {
        if isComplete { return }
        isComplete = true
        let pending = waiters
        waiters.removeAll()
        for waiter in pending { waiter.resume() }
    }
}

/// Why a session could not start. Each maps to something the user can act on.
public enum AgentStartRefusal: Sendable {
    case notConfigured, dirtyWorktree, alreadyRunning
}

public struct AgentStartException: Error, Equatable, Sendable, CustomStringConvertible {
    public let refusal: AgentStartRefusal
    public let message: String

    public init(_ refusal: AgentStartRefusal, _ message: String) {
        self.refusal = refusal
        self.message = message
    }

    public var description: String { message }
}

/// Owns the lifecycle around a run: branch isolation, the task queue, and
/// cleanup of sessions that never finished.
///
/// **Branch isolation, not linked worktrees.** The desktop gives each agent
/// its own `git worktree`, so several can run at once against one repository.
/// The owned Git engine has no linked-worktree support, so a phone session
/// isolates with a *branch* instead: the agent branches from HEAD, works,
/// commits, and the base branch is restored afterwards. The consequences are
/// honest ones: one session per repository at a time, and a clean worktree is
/// required to start.
@MainActor
@Observable
public final class AgentSessionService {
    nonisolated public static let recordsKey = "agent/branch-records-v1"

    /// An unfinished session older than this is assumed dead (the app was
    /// killed, the run crashed, or cleanup itself failed) and its branch is
    /// released so the repository does not stay parked on it forever.
    public let staleAfter: TimeInterval

    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored public let listeners = Listeners()

    public private(set) var queue: [AgentTask] = []
    public private(set) var records: [AgentBranchRecord] = []
    private var active: CodingAgent?
    public private(set) var runningRepo: String?
    private var activeRunHandle: AgentRunHandle?
    private var lastRunHandle: AgentRunHandle?

    public init(prefs: KeyValueStore, staleAfter: TimeInterval = 15 * 24 * 3600, clock: Clock = SystemClock()) {
        self.prefs = prefs
        self.staleAfter = staleAfter
        self.clock = clock
    }

    public var isRunning: Bool { active != nil }

    /// The run in flight, or nil. Stays non-nil through cleanup, so nothing
    /// can start a second session while the repository is being restored.
    public var activeRun: AgentRunHandle? {
        if let activeRunHandle { return activeRunHandle }
        if let lastRunHandle, !lastRunHandle.isComplete { return lastRunHandle }
        return nil
    }

    /// The most recent run, finished or not: what a remounted screen shows.
    public var lastRun: AgentRunHandle? { activeRunHandle ?? lastRunHandle }

    /// ChangeNotifier's `addListener`, for code that is not a SwiftUI view.
    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    private func notifyListeners() { listeners.notify() }

    /// Load persisted branch records. Call once at startup.
    public func load() {
        records = Self.decodeRecords(prefs.string(forKey: Self.recordsKey))
        notifyListeners()
    }

    static func decodeRecords(_ raw: String?) -> [AgentBranchRecord] {
        guard let raw, !raw.isEmpty, let list = (try? JSON.parse(raw))?.arrayValue else { return [] }
        return list.compactMap { AgentBranchRecord(json: $0) }
    }

    private func persist() {
        prefs.set(JSON.encode(.array(records.map { .object($0.json) })), forKey: Self.recordsKey)
    }

    // MARK: - Queue

    /// Add work. A message typed while a session for the same repository is
    /// running steers that session instead of queueing behind it: that is what
    /// makes mid-run correction feel immediate rather than deferred.
    ///
    /// Returns the task when it was queued, or nil when it was delivered as
    /// steering.
    @discardableResult
    public func submit(
        repoName: String, prompt: String, priority: AgentTaskPriority = .normal, id: String? = nil,
        images: [PromptAttachment] = []
    ) -> AgentTask? {
        var trimmed = prompt.dartTrimmed
        if trimmed.isEmpty && images.isEmpty { return nil }
        if let agent = active, runningRepo == repoName {
            agent.steer(trimmed, images: images)
            return nil
        }
        if trimmed.isEmpty { trimmed = "See the attached photos." }
        let now = clock.now
        let task = AgentTask(
            id: id ?? "task-\(Int64((now.timeIntervalSince1970 * 1_000_000).rounded()))",
            repoName: repoName,
            prompt: trimmed,
            priority: priority,
            createdAt: now,
            images: images
        )
        queue.append(task)
        notifyListeners()
        return task
    }

    /// The next task to run: highest priority first, oldest first within a
    /// priority, so a queue drains in a predictable order.
    public var nextTask: AgentTask? {
        let pending = queue.enumerated().filter { $0.element.state == .queued }
        let sorted = pending.sorted { a, b in
            if a.element.priority != b.element.priority {
                return a.element.priority.rawValue < b.element.priority.rawValue
            }
            if a.element.createdAt != b.element.createdAt { return a.element.createdAt < b.element.createdAt }
            return a.offset < b.offset
        }
        return sorted.first?.element
    }

    private func setTaskState(_ id: String, _ state: AgentTaskState) {
        guard let index = queue.firstIndex(where: { $0.id == id }) else { return }
        queue[index] = queue[index].copyWith(state: state)
        notifyListeners()
    }

    public func removeTask(_ id: String) {
        queue.removeAll { $0.id == id }
        notifyListeners()
    }

    // MARK: - Running

    /// Ask the running session to stop at its next safe point.
    ///
    /// The run still finishes cleanly: pending tool calls are answered and the
    /// base branch is restored, so a stop never leaves a half-formed
    /// conversation or a repository parked on an agent branch.
    public func cancelActive() {
        active?.cancel()
        notifyListeners()
    }

    /// Start `task` against `workspace`, isolated on its own branch.
    ///
    /// Preflight (stale sweep, dirty check, branch creation) happens before
    /// this returns, so refusals surface as thrown `AgentStartException`s at
    /// the call site. The run itself then executes eagerly, owned by this
    /// service: it does not pause when a screen stops observing it, and its
    /// transcript lives on the returned `AgentRunHandle`, which `activeRun`
    /// keeps exposing so a remounted screen finds the conversation again.
    /// `waitUntilDone()` returns only after cleanup, so "the run is over"
    /// always means the repository has been put back in order.
    public func start(
        task: AgentTask,
        workspace: AgentWorkspace,
        buildAgent: @escaping @MainActor (AgentToolset) -> CodingAgent,
        author: AgentAuthor,
        readOnly: Bool = false
    ) async throws -> AgentRunHandle {
        if active != nil || activeRunHandle != nil {
            throw AgentStartException(
                .alreadyRunning,
                "An agent session is already running. Wait for it, or steer it with a follow-up message."
            )
        }

        // Release dead sessions before taking a branch, so an interrupted run
        // from weeks ago cannot leave this repository parked on its branch.
        await sweepStaleRecords(workspace: workspace, repoName: task.repoName)

        let status = try await workspace.status()
        if !status.isClean {
            let count = status.changeCount
            throw AgentStartException(
                .dirtyWorktree,
                "This repository has \(count) uncommitted \(count == 1 ? "change" : "changes"). Commit or discard them first — an agent session needs a clean starting point so its own edits stay separable from yours."
            )
        }

        let baseBranch = try await workspace.currentBranch()
        let branchName = Self.agentBranchName(task.id, task.prompt)
        let record = AgentBranchRecord(
            sessionId: task.id,
            repoName: task.repoName,
            branch: branchName,
            baseBranch: baseBranch.isEmpty ? "main" : baseBranch,
            startedAt: clock.now
        )

        // Record BEFORE touching git: a crash between creating the branch and
        // writing the record would otherwise leave an orphan nothing knows
        // about.
        records.append(record)
        persist()

        setTaskState(task.id, .running)
        try await workspace.createBranch(branchName, checkout: true)

        let handle = AgentRunHandle(task: task, branch: record.branch, baseBranch: record.baseBranch)
        activeRunHandle = handle
        lastRunHandle = handle
        Task { @MainActor in
            await self.execute(handle, record, task, workspace, buildAgent, author, readOnly: readOnly)
        }
        return handle
    }

    private func execute(
        _ handle: AgentRunHandle,
        _ record: AgentBranchRecord,
        _ task: AgentTask,
        _ workspace: AgentWorkspace,
        _ buildAgent: @MainActor (AgentToolset) -> CodingAgent,
        _ author: AgentAuthor,
        readOnly: Bool
    ) async {
        func emit(_ event: AgentEvent, images: [PromptAttachment] = []) {
            handle.append(event, images: images)
            notifyListeners()
        }

        emit(.notice("Working on branch \(record.branch)."))

        let toolset = AgentToolset(workspace, readOnly: readOnly)
        toolset.author = author
        let agent = buildAgent(toolset)
        active = agent
        runningRepo = task.repoName
        notifyListeners()

        var failed = false
        let loader = AgentAttachmentLoader()
        await loader.seedLocal(task.images)
        await agent.run(task.prompt, images: task.images) { event in
            if let done = event.done {
                failed = done.reason == .error || done.reason == .refused
            }
            if event.userText != nil {
                let photos = agent.lastUserImages
                emit(event, images: photos)
                if !photos.isEmpty { Task { await loader.seedLocal(photos) } }
            } else {
                emit(event)
            }
        }

        active = nil
        runningRepo = nil

        // Whatever happened, close the record and put the repository back
        // where the user left it. Leaving them on an agent branch is the
        // failure mode this whole record exists to prevent.
        if let restored = await finishSession(record, workspace) {
            emit(.notice(restored))
        }
        setTaskState(task.id, failed ? .failed : .done)
        activeRunHandle = nil
        handle.complete()
        notifyListeners()
    }

    /// Close `record` and return the worktree to its base branch. Returns a
    /// line for the transcript, or nil when there was nothing to say.
    private func finishSession(_ record: AgentBranchRecord, _ workspace: AgentWorkspace) async -> String? {
        replaceRecord(record, AgentBranchRecord(
            sessionId: record.sessionId,
            repoName: record.repoName,
            branch: record.branch,
            baseBranch: record.baseBranch,
            startedAt: record.startedAt,
            finishedAt: clock.now
        ))
        persist()
        return await restoreBase(record, workspace)
    }

    /// Check the base branch back out, unless uncommitted work would be lost.
    private func restoreBase(_ record: AgentBranchRecord, _ workspace: AgentWorkspace) async -> String? {
        do {
            let status = try await workspace.status()
            if !status.isClean {
                // Uncommitted edits mean the run stopped mid-change. Switching
                // away would strand that work, so leave it and say where it is.
                return "Left you on \(record.branch): it has uncommitted changes. Review them, then switch back to \(record.baseBranch) yourself."
            }
            let branches = try await workspace.branches()
            let current = branches.filter(\.isCurrent).map(\.name).joined()
            if current != record.branch { return nil } // already moved on
            if !branches.contains(where: { $0.name == record.baseBranch }) { return nil }
            try await workspace.checkout(record.baseBranch)
            return "Back on \(record.baseBranch). The work is on \(record.branch)."
        } catch {
            return nil // never let cleanup failure mask the run's own outcome
        }
    }

    private func replaceRecord(_ from: AgentBranchRecord, _ to: AgentBranchRecord) {
        if let index = records.firstIndex(where: { $0.sessionId == from.sessionId }) {
            records[index] = to
        } else {
            records.append(to)
        }
    }

    // MARK: - Stale cleanup

    /// Release sessions that never finished.
    ///
    /// An unfinished record older than `staleAfter` means the run died without
    /// cleaning up. The branch itself is kept (the owned engine deliberately
    /// cannot delete refs, and the commits on it may be the only copy of that
    /// work) but the repository is returned to its base branch and the record
    /// is dropped, which is the part that was actually stuck.
    ///
    /// Returns the records it released.
    @discardableResult
    public func sweepStaleRecords(workspace: AgentWorkspace? = nil, repoName: String? = nil, now: Date? = nil) async -> [AgentBranchRecord] {
        let at = now ?? clock.now
        let stale = records.filter { !$0.isFinished && at.timeIntervalSince($0.startedAt) >= staleAfter }
        if stale.isEmpty { return [] }

        for record in stale {
            // Only touch git for the repository we actually hold open.
            if let workspace, record.repoName == repoName {
                _ = await restoreBase(record, workspace)
            }
            records.removeAll { $0.sessionId == record.sessionId }
        }
        persist()
        notifyListeners()
        return stale
    }

    /// Branch name for a session: stable, sortable, and readable in a branch
    /// list, e.g. `agent/1712-fix-login-redirect`.
    public static func agentBranchName(_ sessionId: String, _ prompt: String) -> String {
        let cleaned = String(prompt.lowercased().map { character -> Character in
            if character.isWhitespace || character == "-" { return character }
            if character.isASCII, character.isLetter || character.isNumber { return character }
            return " "
        })
        let words = cleaned
            .split(whereSeparator: { $0.isWhitespace })
            .map(String.init)
            .filter { !$0.isEmpty && !filler.contains($0) }
            .prefix(4)
            .joined(separator: "-")
        let units = Array(sessionId.utf16)
        let suffix = units.count > 6 ? String(decoding: units[(units.count - 6)...], as: UTF16.self) : sessionId
        let slug = words.isEmpty ? "task" : words
        let bounded = slug.utf16.count > 32 ? AgentText.prefixUTF16(slug, 32) : slug
        return "agent/\(suffix)-\(bounded)"
    }

    nonisolated static let filler: Set<String> = [
        "a", "an", "and", "the", "to", "of", "in", "on", "for", "please", "can",
        "you", "my", "is", "it", "that", "this", "with", "add", "make",
    ]

    public func dispose() {
        active?.cancel()
    }
}
