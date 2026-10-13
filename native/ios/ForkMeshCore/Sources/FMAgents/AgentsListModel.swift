// The logic of the Agents tab: its load
// (owner-sealed desktop snapshots plus the relay queue, per repository), its
// grouping, ordering and counts, the rows' detail lines, the reload on an
// `agents` ring, and the two session screens' state
// (AgentSessionDetailScreen, LaunchedAgentSessionScreen). The views are in
// ForkMesh/Features/Agents.

import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import Observation

/// What the Agents screens read and write on the relay (`ApiService`
/// conforms), so the models are tested with a fake.
@MainActor
public protocol AgentsApi: AnyObject {
    func repositories() async throws -> [Repository]
    func agentSessionPage(_ owner: String, _ name: String, ownerAccount: String) async throws -> AgentSessionPage
    func agentTranscript(_ owner: String, _ name: String, _ id: Int, ownerAccount: String) async throws -> AgentTranscript
    func agentLaunchOptions(_ owner: String, _ repo: String) async throws -> AgentLaunchOptions
    func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull]
    func publishedPullHeads(_ owner: String, _ name: String) async throws -> [PublishedPull]
    func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String, model: String,
        targetNode: String, title: String, createPr: Bool, issueNumber: Int, orgScope: Bool
    ) async throws -> AgentQueueResult
    func steerAgentSession(_ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool) async throws -> AgentQueueResult
    func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String, model: String,
        targetNode: String, title: String, createPr: Bool, issueNumber: Int, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult
    func steerAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult
    func retryAgentSession(_ owner: String, _ repo: String, _ session: AgentLaunchSession) async throws -> AgentRetryResult
    func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
    func agentLiveStream(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> HTTPStreamedResponse
}

extension ApiService: AgentsApi {}

@MainActor
public final class AgentReadRefusals {
    public static let shared = AgentReadRefusals()

    private var refused: [String: Date] = [:]

    public init() {}

    public func refuse(_ key: String, now: Date = Date()) {
        refused[key] = now
    }

    public func isRefused(_ key: String, ttl: TimeInterval, now: Date = Date()) -> Bool {
        guard let at = refused[key] else { return false }
        if now.timeIntervalSince(at) < ttl { return true }
        refused.removeValue(forKey: key)
        return false
    }
}

extension AgentsApi {
    public func agentLiveStream(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> HTTPStreamedResponse
    {
        AgentLiveStream.unavailable
    }

    public func publishedPullHeads(_ owner: String, _ name: String) async throws -> [PublishedPull] {
        try await publishedPulls(owner, name)
    }

    public func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String, model: String,
        targetNode: String, title: String, createPr: Bool, issueNumber: Int, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult {
        if !attachments.isEmpty { throw AgentLaunchException("This relay cannot take photos with a prompt.") }
        return try await launchAgentSession(
            owner, repo, prompt: prompt, provider: provider, model: model, targetNode: targetNode, title: title,
            createPr: createPr, issueNumber: issueNumber, orgScope: orgScope)
    }

    public func steerAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult {
        if !attachments.isEmpty { throw AgentLaunchException("This relay cannot take photos with a prompt.") }
        return try await steerAgentSession(owner, repo, sessionId, prompt: prompt, orgScope: orgScope)
    }
}

/// One repository's agent work.
public struct AgentRepoGroup: Equatable, Sendable {
    public let repo: Repository
    public let sessions: [AgentSession]
    /// Sessions on the relay queue: the ones started from this app or the web.
    public let launched: [AgentLaunchSession]
    /// The repository's pull requests, read only when a queued task has a
    /// branch one of them could belong to.
    public let pulls: [PublishedPull]
    /// Owner-sealed sessions this device is not a recipient of.
    public let sealedForOthers: Int
    public let targets: [AgentLaunchTarget]

    public init(repo: Repository, sessions: [AgentSession], launched: [AgentLaunchSession] = [], pulls: [PublishedPull] = [], sealedForOthers: Int = 0, targets: [AgentLaunchTarget] = []) {
        self.repo = repo
        self.sessions = sessions
        self.launched = launched
        self.pulls = pulls
        self.sealedForOthers = sealedForOthers
        self.targets = targets
    }

    public func runsInCloud(_ session: AgentLaunchSession) -> Bool {
        AgentsLogic.runsInCloud(session, targets)
    }

    public var hasSealed: Bool { sealedForOthers > 0 }
    public var total: Int { sessions.count + launched.count }
    public var hasActive: Bool { sessions.contains(where: \.isActive) || launched.contains(where: \.isActive) }
}

/// One row of the flat list: a desktop-pushed session or a queued run, with
/// the repository it belongs to.
public enum AgentEntry: Identifiable, Equatable, Sendable {
    case desktop(Repository, AgentSession)
    case launched(Repository, AgentLaunchSession, [PublishedPull], cloud: Bool)

    public var repo: Repository {
        switch self {
        case .desktop(let repo, _), .launched(let repo, _, _, _): return repo
        }
    }

    public var isActive: Bool {
        switch self {
        case .desktop(_, let session): return session.isActive
        case .launched(_, let session, _, _): return session.isActive
        }
    }

    public var isCloud: Bool {
        switch self {
        case .desktop: return false
        case .launched(_, _, _, let cloud): return cloud
        }
    }

    public var id: String {
        switch self {
        case .desktop(let repo, let session): return "desktop:\(repo.fullName):\(session.id)"
        case .launched(let repo, let session, _, _): return "queued:\(repo.fullName):\(session.sessionId)"
        }
    }
}

/// A desktop session that is running now, with its repository.
public struct AgentRunningItem: Identifiable, Equatable, Sendable {
    public let repo: Repository
    public let session: AgentSession
    public var id: String { "\(repo.fullName)-\(session.id)" }
}

/// The colour family a status label draws in; the views map it to the theme.
public enum AgentBadgeTone: Sendable {
    case accent, success, danger, secondary, tertiary
}

public enum AgentsLogic {
    /// `_AgentStatusBadge`: a desktop session's status, by exact label.
    public static func statusTone(_ label: String) -> AgentBadgeTone {
        switch label {
        case "Merged", "Done": return .success
        case "Failed", "Stopped": return .danger
        case "Running", "Queued", "Waiting": return .accent
        default: return .tertiary
        }
    }

    /// `_agentBadgeColor`: any chip, by what its label says.
    public static func chipTone(_ label: String) -> AgentBadgeTone {
        let normalized = label.lowercased()
        if normalized.contains("running") || normalized.contains("queued") || normalized.contains("waiting") { return .accent }
        if normalized.contains("done") || normalized.contains("completed") { return .success }
        if normalized.contains("failed") || normalized.contains("stopped") { return .danger }
        return .secondary
    }

    /// Everything about a desktop session on one line (`_AgentSessionRow`).
    public static func desktopDetail(_ repo: Repository, _ session: AgentSession, now: Date = Date()) -> String {
        var parts: [String] = [placeLabel(cloud: false), repo.fullName]
        if session.issueNumber > 0 { parts.append("Issue #\(session.issueNumber)") }
        parts += [session.providerLabel, session.model, session.branchName, session.rateLimitCountdownLabel(now: now)]
        if session.numTurns > 0 { parts.append("\(session.numTurns) turns") }
        parts += [session.durationLabel, session.costLabel, session.prLabel, session.diffLabel]
        if session.conflicted { parts.append("Conflict") }
        parts.append(session.tokenLabel)
        if session.isFailed { parts.append(session.statusFailureReason(80)) }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    /// Everything about a queued run on one line (`_LaunchedSessionRow`).
    public static func launchedDetail(_ repo: Repository, _ session: AgentLaunchSession, pulls: [PublishedPull], cloud: Bool = false) -> String {
        let chain = taskChain(session, pulls)
        var parts: [String] = [placeLabel(cloud: cloud), repo.fullName, session.targetNode, session.provider, session.branchName]
        if !chain.summary.isEmpty {
            parts.append(chain.summary)
        } else if session.createPr {
            parts.append("opens PR")
        }
        let last = session.history.last?.text.dartTrimmed ?? ""
        parts.append(last.components(separatedBy: "\n").first ?? "")
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public static func placeLabel(cloud: Bool) -> String {
        cloud ? "Cloud" : "Desktop"
    }

    public static func placeIcon(cloud: Bool) -> String {
        cloud ? "cloud_outlined" : "desktop_windows"
    }

    public static func runsInCloud(_ session: AgentLaunchSession, _ targets: [AgentLaunchTarget]) -> Bool {
        if session.onRunner { return true }
        let node = session.targetNode.dartTrimmed.lowercased()
        if node.isEmpty { return false }
        return targets.first { $0.node.dartTrimmed.lowercased() == node }?.isMirror ?? false
    }

    /// The chain for `session`, with the pull request looked up in `pulls` by
    /// number, or by the task's branch when the node has not reported one:
    /// the same match the web's pull timeline makes.
    public static func taskChain(_ session: AgentLaunchSession, _ pulls: [PublishedPull]) -> AgentTaskChain {
        let pull = pulls.first { item in
            session.prNumber > 0
                ? item.number == session.prNumber
                : !session.branchName.isEmpty && item.head == session.branchName
        }
        return AgentTaskChain(session, pullStatus: pull?.status ?? "", pullNumber: pull?.number ?? 0)
    }

    /// What running `session` again would do, in one sentence.
    public static func retryDetail(_ session: AgentLaunchSession) -> String {
        let node = session.targetNode.isEmpty ? "the mirror" : session.targetNode
        return session.canResume
            ? "Sends the same task back to \(node), which picks this session up on the branch it already cut."
            : "No node ever started this one, so there is nothing to resume. The same task is queued again on \(node)."
    }

    /// Running first, then by id, newest first.
    static func sortSessions(_ sessions: [AgentSession]) -> [AgentSession] {
        stableSorted(sessions) { a, b in
            if a.isActive != b.isActive { return a.isActive }
            return a.id > b.id
        }
    }

    /// Running first, then most recently updated.
    static func sortLaunched(_ sessions: [AgentLaunchSession]) -> [AgentLaunchSession] {
        stableSorted(sessions) { a, b in
            if a.isActive != b.isActive { return a.isActive }
            return a.updatedAtMs > b.updatedAtMs
        }
    }

    /// Repositories with running work first, then by name.
    static func sortGroups(_ groups: [AgentRepoGroup]) -> [AgentRepoGroup] {
        stableSorted(groups) { a, b in
            if a.hasActive != b.hasActive { return a.hasActive }
            return a.repo.fullName.lowercased().utf16.lexicographicallyPrecedes(b.repo.fullName.lowercased().utf16)
        }
    }

    /// Running work first, in repository order; everything else after it.
    public static func flatten(_ groups: [AgentRepoGroup]) -> [AgentEntry] {
        var all: [AgentEntry] = []
        for group in groups {
            all += group.sessions.map { AgentEntry.desktop(group.repo, $0) }
            all += group.launched.map { AgentEntry.launched(group.repo, $0, group.pulls, cloud: group.runsInCloud($0)) }
        }
        return all.filter(\.isActive) + all.filter { !$0.isActive }
    }

    static func stableSorted<T>(_ items: [T], by less: (T, T) -> Bool) -> [T] {
        items.enumerated().sorted { a, b in
            if less(a.element, b.element) { return true }
            if less(b.element, a.element) { return false }
            return a.offset < b.offset
        }.map(\.element)
    }
}

/// The Agents tab.
///
/// Loaded when the tab is opened, when the reader refreshes, and when the
/// relay says the account's sessions changed. There is no timer: a poll here
/// cost two or more requests per repository every ten seconds, even in the
/// background.
@MainActor
@Observable
public final class AgentsListModel {
    @ObservationIgnored private let api: AgentsApi
    @ObservationIgnored private let signal: AgentListSignal?
    @ObservationIgnored private let accountName: @MainActor () -> String
    @ObservationIgnored private let desktopNode: @MainActor () -> Bool
    @ObservationIgnored private let debounce: TimeInterval
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var signalToken: ListenerToken?
    @ObservationIgnored private var debounceTask: Task<Void, Never>?
    @ObservationIgnored private var loadTask: Task<Void, Never>?
    @ObservationIgnored private var ringLoading = false
    @ObservationIgnored private var rangDuringLoad = false
    @ObservationIgnored private let refusals: AgentReadRefusals
    @ObservationIgnored private let now: @MainActor () -> Date
    @ObservationIgnored private var pullsReadAt: [String: Date] = [:]
    public nonisolated static let refusalTtl: TimeInterval = 600
    public nonisolated static let pullsRefreshInterval: TimeInterval = 300

    public private(set) var groups: [AgentRepoGroup] = []
    public private(set) var isLoading = false
    public private(set) var hasLoaded = false
    /// The load failure's text, shown with a Retry.
    public private(set) var loadError: String?
    /// Queued runs being retried from the list (their row shows a spinner).
    public private(set) var retrying: Set<String> = []

    /// A hidden tab ignores rings and drops in-flight loads; it loads afresh
    /// when it is next shown.
    public var enabled = true {
        didSet {
            if enabled == oldValue { return }
            if enabled {
                if !hasLoaded && !isLoading { reload() }
            } else {
                generation += 1
                isLoading = false
            }
        }
    }

    public init(
        api: AgentsApi,
        signal: AgentListSignal? = nil,
        ownerAccountName: @escaping @MainActor () -> String = { "" },
        hasDesktopNode: @escaping @MainActor () -> Bool = { true },
        debounce: TimeInterval = 0.6,
        refusals: AgentReadRefusals? = nil,
        now: @escaping @MainActor () -> Date = { Date() }
    ) {
        self.api = api
        self.refusals = refusals ?? AgentReadRefusals()
        self.now = now
        self.signal = signal
        self.accountName = ownerAccountName
        self.desktopNode = hasDesktopNode
        self.debounce = debounce
    }

    /// Agents run on a host. A `mobile_node` key never carries `host_repo`,
    /// so with no desktop node enrolled there is nothing on this account that
    /// can execute a session: the screens say so instead of an empty list.
    public var hasDesktopNode: Bool { desktopNode() }

    /// The first load and the ring subscription. Idempotent.
    public func start() {
        if signalToken == nil, let signal {
            signalToken = signal.addListener { [weak self] in
                MainActor.assumeIsolated { self?.onSignal() }
            }
        }
        if enabled && !hasLoaded && !isLoading { reload() }
    }

    public func stop() {
        if let signalToken { signal?.removeListener(signalToken) }
        signalToken = nil
        debounceTask?.cancel()
        rangDuringLoad = false
    }

    /// A desktop pushes a new snapshot at each step of a running session, and
    /// every reload re-reads each repository, so a burst of bells is one load.
    private func onSignal() {
        if ringLoading {
            rangDuringLoad = true
            return
        }
        debounceTask?.cancel()
        let delay = debounce
        debounceTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard let self, !Task.isCancelled, self.enabled else { return }
            await self.ringRefresh()
        }
    }

    private func ringRefresh() async {
        ringLoading = true
        rangDuringLoad = false
        await load(ring: true)
        ringLoading = false
        if rangDuringLoad, signalToken != nil, !Task.isCancelled {
            rangDuringLoad = false
            onSignal()
        }
    }

    /// Start a load without waiting for it.
    public func reload() {
        loadTask = Task { [weak self] in await self?.refresh() }
    }

    /// Read everything again (the header button, a pull down).
    public func refresh() async {
        await load(ring: false)
    }

    private func load(ring: Bool) async {
        if !enabled { return }
        generation += 1
        let mine = generation
        isLoading = true
        do {
            let loaded = try await loadGroups(mine, ring: ring)
            guard mine == generation, enabled else { return }
            if loaded != groups { groups = loaded }
            if loadError != nil { loadError = nil }
        } catch {
            guard mine == generation, enabled else { return }
            if !Task.isCancelled { loadError = dartErrorText(error) }
        }
        if !hasLoaded { hasLoaded = true }
        isLoading = false
    }

    /// Wait for the load `start()` or `reload()` began (tests).
    public func waitForLoad() async {
        await loadTask?.value
    }

    private func loadGroups(_ mine: Int, ring: Bool) async throws -> [AgentRepoGroup] {
        let repos = try await api.repositories()
        if mine != generation || !enabled { return [] }
        var loaded: [AgentRepoGroup?] = Array(repeating: nil, count: repos.count)
        let shown = Dictionary(groups.map { ($0.repo.fullName, $0) }, uniquingKeysWith: { first, _ in first })
        await withTaskGroup(of: (Int, AgentRepoGroup).self) { group in
            for (index, repo) in repos.enumerated() {
                let previous = shown[repo.fullName]
                group.addTask { @MainActor in (index, await self.loadGroup(repo, previous: previous, ring: ring)) }
            }
            for await (index, result) in group { loaded[index] = result }
        }
        if mine != generation || !enabled { return [] }
        // A repository whose sessions are all sealed to other owner devices
        // still belongs in the list: its card explains why it is empty here,
        // which is very different from having no agent work at all.
        let visible = loaded.compactMap { $0 }.filter { !$0.sessions.isEmpty || !$0.launched.isEmpty || $0.hasSealed }
        return AgentsLogic.sortGroups(visible)
    }

    private func loadGroup(_ repo: Repository, previous: AgentRepoGroup?, ring: Bool) async -> AgentRepoGroup {
        // Two stores, two reads. The owner-sealed snapshots are what a desktop
        // pushes and only an enrolled device can open; the relay queue is
        // what this phone writes to when it starts a run. A session started
        // here would be invisible if only the first were read.
        async let sealed = sealedPage(repo)
        async let queued = launchOptions(repo)
        let page = await sealed
        let options = await queued
        let launched = options.map { AgentsLogic.sortLaunched($0.sessionList.keeping(previous?.launched ?? [])) }
            ?? previous?.launched ?? []
        // A mirror never hears that its pull request was merged, so the pull
        // ledger is read for any task that got as far as a branch.
        var pulls: [PublishedPull] = []
        if launched.contains(where: { !$0.branchName.isEmpty }) {
            if ring, let previous, !pullsStale(repo, launched, previous) {
                pulls = previous.pulls
            } else if let read = try? await api.publishedPullHeads(repo.owner, repo.name) {
                pulls = read
                pullsReadAt[repo.fullName] = now()
            } else {
                pulls = previous?.pulls ?? []
            }
        }
        return AgentRepoGroup(
            repo: repo,
            sessions: page.map { AgentsLogic.sortSessions($0.sessions) } ?? previous?.sessions ?? [],
            launched: launched,
            pulls: pulls,
            sealedForOthers: page?.sealedForOthers ?? previous?.sealedForOthers ?? 0,
            targets: options?.targets ?? previous?.targets ?? []
        )
    }

    private func pullsStale(_ repo: Repository, _ launched: [AgentLaunchSession], _ previous: AgentRepoGroup) -> Bool {
        guard let readAt = pullsReadAt[repo.fullName] else { return true }
        let known = Dictionary(previous.launched.map { ($0.sessionId, $0) }, uniquingKeysWith: { first, _ in first })
        let branched = launched.filter { !$0.branchName.isEmpty }
        let moved = branched.contains { session in
            guard let before = known[session.sessionId] else { return true }
            return before.status != session.status || before.branchName != session.branchName
                || before.prNumber != session.prNumber || before.merged != session.merged
        }
        if moved { return true }
        if now().timeIntervalSince(readAt) < AgentsListModel.pullsRefreshInterval { return false }
        return branched.contains { session in
            let chain = AgentsLogic.taskChain(session, previous.pulls)
            return !chain.isMerged && !chain.isPullClosed
        }
    }

    private func sealedPage(_ repo: Repository) async -> AgentSessionPage? {
        let account = ownerAccount(for: repo)
        let key = "sealed|\(account)|\(repo.fullName)"
        if isRefused(key) { return nil }
        do {
            return try await api.agentSessionPage(repo.owner, repo.name, ownerAccount: account)
        } catch {
            if AgentsListModel.isRefusal(error) { refusals.refuse(key) }
            return nil
        }
    }

    private func launchOptions(_ repo: Repository) async -> AgentLaunchOptions? {
        let key = "queued|\(accountName().dartTrimmed.lowercased())|\(repo.fullName)"
        if isRefused(key) { return nil }
        do {
            return try await api.agentLaunchOptions(repo.owner, repo.name)
        } catch {
            if AgentsListModel.isRefusal(error) { refusals.refuse(key) }
            return nil
        }
    }

    private func isRefused(_ key: String) -> Bool {
        refusals.isRefused(key, ttl: AgentsListModel.refusalTtl)
    }

    public nonisolated static func isRefusal(_ error: Error) -> Bool {
        if let launch = error as? AgentLaunchException { return launch.status == 403 || launch.status == 404 }
        if let api = error as? ApiException {
            return api.message.hasPrefix("HTTP 403 ") || api.message.hasPrefix("HTTP 404 ")
        }
        return false
    }

    /// The account the owner-sealed snapshots are addressed to: this
    /// session's node name, or the repository's owner signed out.
    public func ownerAccount(for repo: Repository) -> String {
        let node = accountName().dartTrimmed.lowercased()
        return node.isEmpty ? repo.owner : node
    }

    // MARK: - Derived

    public var sessionCount: Int { groups.reduce(0) { $0 + $1.total } }

    public var runningCount: Int {
        groups.reduce(0) { $0 + $1.sessions.filter(\.isActive).count + $1.launched.filter(\.isActive).count }
    }

    public var desktopRunningCount: Int { groups.reduce(0) { $0 + $1.sessions.filter(\.isActive).count } }

    public var sealedCount: Int { groups.reduce(0) { $0 + $1.sealedForOthers } }

    public var entries: [AgentEntry] { AgentsLogic.flatten(groups) }

    public var desktopRunning: [AgentRunningItem] {
        groups.flatMap { group in group.sessions.filter(\.isActive).map { AgentRunningItem(repo: group.repo, session: $0) } }
    }

    /// Loading with nothing to show yet.
    public var isPendingFirstLoad: Bool { groups.isEmpty && loadError == nil && (isLoading || !hasLoaded) }

    /// A failed load with nothing to show.
    public var isFailed: Bool { groups.isEmpty && loadError != nil }

    /// Which body the list tab draws, for `FmSwap`.
    public var listState: String {
        if isPendingFirstLoad { return "loading" }
        if isFailed { return "error" }
        if groups.isEmpty { return hasDesktopNode ? "empty" : "no-host" }
        return "list-\(sessionCount)"
    }

    // MARK: - Retry from the list

    /// Run a failed or stopped queued session again without opening it.
    /// Returns the sentence for the inline notice, then reloads the list.
    public func retry(_ repo: Repository, _ session: AgentLaunchSession) async -> String? {
        if retrying.contains(session.sessionId) { return nil }
        retrying.insert(session.sessionId)
        defer { retrying.remove(session.sessionId) }
        let message: String
        do {
            let outcome = try await api.retryAgentSession(repo.owner, repo.name, session)
            message = outcome.label
            await refresh()
        } catch let error as AgentLaunchException {
            message = error.message
        } catch {
            message = dartErrorText(error)
        }
        return message
    }
}

/// `AgentSessionDetailScreen`: a desktop session and its pushed transcript.
@MainActor
@Observable
public final class AgentTranscriptModel {
    @ObservationIgnored private let api: AgentsApi
    public let repo: Repository
    public let session: AgentSession
    public let ownerAccount: String

    public private(set) var transcript: AgentTranscript?
    public private(set) var attachments: [AgentAttachmentRef] = []
    public private(set) var error: String?
    public private(set) var isLoading = false
    @ObservationIgnored private let desktopAttachments: (@MainActor () async throws -> [AgentAttachmentRef]?)?
    @ObservationIgnored private var desktopListed: [AgentAttachmentRef] = []

    public init(
        api: AgentsApi, repo: Repository, session: AgentSession, ownerAccount: String,
        desktopAttachments: (@MainActor () async throws -> [AgentAttachmentRef]?)? = nil
    ) {
        self.api = api
        self.repo = repo
        self.session = session
        self.ownerAccount = ownerAccount
        self.desktopAttachments = desktopAttachments
    }

    public func load() async {
        isLoading = true
        do {
            let loaded = try await api.agentTranscript(repo.owner, repo.name, session.id, ownerAccount: ownerAccount)
            transcript = loaded
            let named = loaded.sealedForOtherDevice ? [] : AgentTranscriptAttachments.references(in: loaded.transcript)
            attachments = desktopListed.isEmpty ? named : desktopListed
            error = nil
        } catch {
            self.error = dartErrorText(error)
        }
        isLoading = false
        await loadDesktopAttachments()
    }

    private func loadDesktopAttachments() async {
        guard let desktopAttachments, let listed = try? await desktopAttachments(), !listed.isEmpty else { return }
        desktopListed = listed
        attachments = listed
    }

    /// The transcript's own status when it has one, else the list's.
    public var statusLabel: String {
        let status = transcript?.status ?? ""
        if status.isEmpty { return session.statusLabel }
        var probe = session
        probe.status = status
        return probe.statusLabel
    }

    public var bodyText: String {
        let detail = transcript ?? AgentTranscript(status: "", transcript: "")
        if detail.sealedForOtherDevice {
            return "This transcript is encrypted for another owner device. Sign in on this device and let it enroll; new sessions are then sealed to it too."
        }
        let text = detail.transcript.dartTrimmed
        return text.isEmpty ? "No transcript has been pushed by the desktop node yet." : text
    }
}

/// `LaunchedAgentSessionScreen`: a queued session in full, a composer that
/// steers it, a retry, and dropping it from the queue.
@MainActor
@Observable
public final class LaunchedAgentSessionModel {
    @ObservationIgnored private let api: AgentsApi
    public let repo: Repository

    public private(set) var session: AgentLaunchSession
    public private(set) var pulls: [PublishedPull]
    public private(set) var busy = false
    public var error: String?
    /// What the relay said about the last retry: the only sign anything
    /// happened until the mirror picks the work up.
    public private(set) var notice: String?
    public var followUp = ""
    /// Photos attached to the follow-up (the composer's "+").
    public var followUpAttachments: [PromptAttachment] = []
    public private(set) var pending: AgentLaunchMessage?
    @ObservationIgnored private var pendingPhotos: [PromptAttachment] = []
    @ObservationIgnored public let attachmentLoader: AgentAttachmentLoader
    @ObservationIgnored private let sentPhotos: AgentSentPhotos
    @ObservationIgnored private var following = false
    @ObservationIgnored private var followAgain = false
    @ObservationIgnored private var pullsRead = false
    @ObservationIgnored private var streamLive: AgentLiveText?
    public private(set) var liveRefused = false

    public init(
        api: AgentsApi, repo: Repository, session: AgentLaunchSession, pulls: [PublishedPull] = [],
        attachmentLoader: AgentAttachmentLoader = AgentAttachmentLoader(), sentPhotos: AgentSentPhotos? = nil
    ) {
        self.api = api
        self.repo = repo
        self.session = session
        self.pulls = pulls
        self.attachmentLoader = attachmentLoader
        self.sentPhotos = sentPhotos ?? .shared
    }

    public var displayHistory: [AgentLaunchMessage] {
        guard let pending else { return session.history }
        return session.history + [pending]
    }

    public func isPending(_ index: Int) -> Bool { pending != nil && index == session.history.count }

    public func attachments(at index: Int) -> [AgentAttachmentRef] {
        if isPending(index) { return pendingPhotos.map(AgentAttachmentRef.local) }
        guard session.history.indices.contains(index) else { return [] }
        let message = session.history[index]
        if !message.attachments.isEmpty {
            return session.attachmentRefs(message, owner: repo.owner, repo: repo.name)
        }
        if message.isUser, let photos = sentPhotos.photos(sessionId: session.sessionId, text: message.text) {
            return photos.map(AgentAttachmentRef.local)
        }
        return message.isUser ? [] : AgentTranscriptAttachments.references(in: message.text)
    }

    private func reconcile(_ photos: [PromptAttachment]) async {
        guard let index = AgentAttachmentReconcile.turn(for: photos, in: session.history),
            let pairs = AgentAttachmentReconcile.pair(sent: photos, listed: session.history[index].attachments)
        else { return }
        await attachmentLoader.seed(
            pairs,
            origin: .relay(owner: repo.owner, repo: repo.name, sessionId: session.sessionId, orgScope: session.orgScope))
    }

    public var chain: AgentTaskChain { AgentsLogic.taskChain(session, pulls) }

    /// Shown instead of the composer when the session cannot take a follow-up.
    public var followUpClosedText: String? {
        if session.isPromptable { return nil }
        return session.isActive
            ? "Follow-ups open once the mirror has picked this up."
            : "This session never started on a node, so there is nothing to resume."
    }

    public var followUpHint: String {
        session.isActive ? "Tell the agent what to do next…" : "Continue this session on its branch…"
    }

    public var runLog: AgentRunLog { AgentRunLog(chain, repo: "\(repo.owner)/\(repo.name)") }

    public var followsLive: Bool { session.isActive && !liveRefused }

    public func followLive() async {
        if !followsLive { return }
        let api = self.api
        let owner = repo.owner
        let name = repo.name
        let sessionId = session.sessionId
        let orgScope = session.orgScope
        let stream = AgentLiveStream(open: {
            try await api.agentLiveStream(owner, name, sessionId, orgScope: orgScope)
        })
        let outcome = await stream.run { live in
            guard session.sessionId == sessionId else { return }
            streamLive = live
            applyLive()
        }
        streamLive = nil
        switch outcome {
        case .ended:
            await follow()
        case .refused:
            liveRefused = true
        case .cancelled:
            break
        }
    }

    private func applyLive() {
        guard let live = streamLive, session.isActive, live.atMs >= session.liveAtMs else { return }
        if session.liveText == live.text && session.liveAtMs == live.atMs { return }
        session.liveText = live.text
        session.liveAtMs = live.atMs
    }

    public func follow() async {
        if following {
            followAgain = true
            return
        }
        following = true
        defer { following = false }
        repeat {
            followAgain = false
            await refresh(live: true)
        } while followAgain
    }

    /// Reads the run again. The Refresh button, this screen's own actions and
    /// the account socket's agent topics call it; nothing polls.
    public func refresh(live: Bool = false) async {
        do {
            let options = try await api.agentLaunchOptions(repo.owner, repo.name)
            let current = options.sessions.first { $0.sessionId == session.sessionId } ?? session
            var nextPulls = pulls
            let moved = current.status != session.status || current.branchName != session.branchName
                || current.prNumber != session.prNumber || current.merged != session.merged
            if !current.branchName.isEmpty && (!live || moved || !pullsRead) {
                if let read = try? await api.publishedPullHeads(repo.owner, repo.name) {
                    nextPulls = read
                    pullsRead = true
                }
            }
            session = current
            pulls = nextPulls
            applyLive()
        } catch {
            if !live { self.error = dartErrorText(error) }
        }
    }

    public func send() async {
        let text = followUp.dartTrimmed
        let photos = followUpAttachments
        if (text.isEmpty && photos.isEmpty) || busy { return }
        busy = true
        error = nil
        defer { busy = false }
        let turnText = promptOrPhotoNote(text, photos)
        pending = AgentLaunchMessage(
            role: "user", text: turnText, state: "sending",
            attachments: photos.map { AgentAttachmentRef.local($0).meta })
        pendingPhotos = photos
        followUp = ""
        followUpAttachments = []
        await attachmentLoader.seedLocal(photos)
        do {
            _ = try await api.steerAgentSession(
                repo.owner, repo.name, session.sessionId, prompt: text, orgScope: session.orgScope, attachments: photos)
            sentPhotos.record(sessionId: session.sessionId, text: turnText, photos: photos)
            await refresh()
            await reconcile(photos)
        } catch let failure as AgentLaunchException {
            restoreDraft(text, photos)
            error = failure.message
        } catch {
            restoreDraft(text, photos)
            self.error = dartErrorText(error)
        }
        pending = nil
        pendingPhotos = []
    }

    private func restoreDraft(_ text: String, _ photos: [PromptAttachment]) {
        if followUp.isEmpty { followUp = text }
        if followUpAttachments.isEmpty { followUpAttachments = photos }
    }

    /// Put a failed or stopped run back to work. A run a mirror claimed is
    /// resumed on the branch it already cut; one that never started is queued
    /// again as a new session, and this screen follows that one from here.
    public func retry() async {
        if busy { return }
        busy = true
        error = nil
        notice = nil
        defer { busy = false }
        do {
            let outcome = try await api.retryAgentSession(repo.owner, repo.name, session)
            if let started = outcome.session {
                sentPhotos.rename(session.sessionId, to: started.sessionId)
                session = started
            }
            notice = outcome.label
            await refresh()
        } catch let failure as AgentLaunchException {
            error = failure.message
        } catch {
            self.error = dartErrorText(error)
        }
    }

    /// Drop the session from the queue. True when it is gone (close the
    /// screen).
    public func cancel() async -> Bool {
        busy = true
        do {
            try await api.cancelAgentSession(repo.owner, repo.name, session.sessionId, orgScope: session.orgScope)
            return true
        } catch let failure as AgentLaunchException {
            busy = false
            error = failure.message
            return false
        } catch {
            busy = false
            self.error = dartErrorText(error)
            return false
        }
    }
}
