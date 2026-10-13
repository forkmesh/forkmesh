// The logic of the agent screen: the agent surface for
// one repository on this phone (composer, transcript, pull-request offer),
// its setup sheet (provider, key, model; Gemma 4 on this iPhone among them)
// and the "reference a file" picker.

import FMApi
import FMBase
import FMCode
import FMLocalAI
import Foundation
import Observation

/// What a send did, so the view knows whether to open the setup sheet.
public enum AgentSendOutcome: Equatable, Sendable {
    /// Nothing to send.
    case empty
    /// No provider is configured: open setup. The prompt stays in the
    /// composer, so finishing setup and pressing send picks up from here.
    case needsSetup
    /// A session is running: the text steers it.
    case steered
    /// A new session started.
    case started
    /// The session could not start; `error` says why.
    case refused
}

/// The agent surface for one local repository.
///
/// Three states, and the screen is only ever in one of them: not configured
/// (no provider yet), idle with nothing said, or a running transcript. The
/// transcript belongs to the session service, not this model: navigating away
/// and back finds the same run still going.
@MainActor
@Observable
public final class AgentScreenModel {
    public let repoName: String
    @ObservationIgnored public let workspace: AgentWorkspace
    @ObservationIgnored private let gitWorkspace: GitWorkspace?
    public let record: LocalRepoRecord?
    @ObservationIgnored private let pullRequests: PullRequestService?
    public let sessions: AgentSessionService
    @ObservationIgnored public let store: AgentCredentialStore
    @ObservationIgnored private let author: AgentAuthor
    @ObservationIgnored private let agentFactory: (@MainActor (AgentToolset, AgentCredentials) -> CodingAgent)?
    @ObservationIgnored private let localModels: @MainActor () -> LocalModelManager?
    @ObservationIgnored private let autoStart: Bool
    @ObservationIgnored private var autoStarted = false
    @ObservationIgnored private let initialCredentials: AgentCredentials?
    @ObservationIgnored private let desktop: AgentHandoffDesktop?

    /// The composer. Text handed over (a ping, the start form's prompt) waits
    /// here; it is never sent on its own unless `autoStart` was asked for.
    public var draft: String
    /// Photos attached in the composer, sent with the draft as image blocks.
    public var draftImages: [PromptAttachment] = []
    public private(set) var credentials: AgentCredentials?
    public private(set) var isLoading = true
    public var error: String?
    public private(set) var prBusy = false
    /// Branches whose pull request was already opened from this screen.
    public private(set) var prOpenedFor: Set<String> = []
    public private(set) var moving = false
    public private(set) var movedToDesktop: AgentDesktopMove?

    public init(
        repoName: String,
        workspace: AgentWorkspace,
        gitWorkspace: GitWorkspace? = nil,
        record: LocalRepoRecord? = nil,
        pullRequests: PullRequestService? = nil,
        sessions: AgentSessionService,
        store: AgentCredentialStore,
        author: AgentAuthor,
        localModels: @escaping @MainActor () -> LocalModelManager? = { Services.get(LocalModelManager.self) },
        agentFactory: (@MainActor (AgentToolset, AgentCredentials) -> CodingAgent)? = nil,
        initialPrompt: String = "",
        initialImages: [PromptAttachment] = [],
        initialCredentials: AgentCredentials? = nil,
        autoStart: Bool = false,
        desktop: AgentHandoffDesktop? = nil
    ) {
        self.repoName = repoName
        self.workspace = workspace
        self.gitWorkspace = gitWorkspace
        self.record = record
        self.pullRequests = pullRequests
        self.sessions = sessions
        self.store = store
        self.author = author
        self.localModels = localModels
        self.agentFactory = agentFactory
        self.draft = initialPrompt
        self.draftImages = initialImages
        self.initialCredentials = initialCredentials
        self.autoStart = autoStart
        self.desktop = desktop
    }

    /// Read the stored provider. Returns true when the handed-over prompt
    /// should now be sent (once per screen).
    @discardableResult
    public func load() -> Bool {
        credentials = initialCredentials ?? store.read()
        isLoading = false
        return takeAutoStart()
    }

    /// Runs the handed-over prompt once credentials are known. With none
    /// configured the send opens setup, and the prompt stays in the composer.
    public func takeAutoStart() -> Bool {
        if !autoStart || autoStarted { return false }
        if draft.dartTrimmed.isEmpty && draftImages.isEmpty { return false }
        autoStarted = true
        return true
    }

    /// The setup sheet saved (or removed) a provider.
    public func credentialsChanged(_ saved: AgentCredentials?) {
        credentials = saved
    }

    public var handle: AgentRunHandle? { sessions.lastRun }
    public var events: [AgentEvent] { handle?.events ?? [] }
    public var isRunning: Bool { sessions.isRunning }

    /// The chip next to the prompt: the model it will run on.
    public var modelChipLabel: String {
        guard let credentials else { return "Choose a model" }
        if credentials.kind == .onDevice, let manager = localModels() {
            return manager.selectedModel.displayName
        }
        return credentials.modelLabel
    }

    /// When the session runs on Gemma: whether the model is ready.
    public var gemmaReadiness: OnDeviceAgentReadiness? {
        credentials?.kind == .onDevice ? OnDeviceAgentReadiness.of(localModels()) : nil
    }

    /// Whether the composer's "+" can attach photos: the API providers see
    /// images; Gemma on this phone reads text only.
    public var acceptsImages: Bool { credentials?.kind != .onDevice }

    public var composerHint: String {
        isRunning ? "Add an instruction while it works…" : "Ask anything…"
    }

    public func send() async -> AgentSendOutcome {
        let text = draft.dartTrimmed
        let images = draftImages
        if text.isEmpty && images.isEmpty { return .empty }
        guard let credentials else { return .needsSetup }

        // While a session runs, this steers it rather than starting a second
        // one; the service decides, so the rule lives in one place.
        let task = sessions.submit(repoName: repoName, prompt: text, images: images)
        draft = ""
        draftImages = []
        guard let task else { return .steered }

        error = nil
        do {
            let factory = agentFactory
            let models = localModels
            _ = try await sessions.start(
                task: task,
                workspace: workspace,
                buildAgent: { toolset in
                    if let factory { return factory(toolset, credentials) }
                    return credentials.makeAgent(toolset: toolset, localModels: models())
                },
                author: author
            )
            return .started
        } catch let failure as AgentStartException {
            error = failure.message
        } catch {
            self.error = dartErrorText(error)
        }
        return .refused
    }

    public func stop() {
        sessions.cancelActive()
    }

    // MARK: - Pull request

    /// A finished run whose work could become a pull request.
    public var prCandidate: AgentRunHandle? {
        guard let handle, handle.isComplete else { return nil }
        guard record != nil, pullRequests != nil, gitWorkspace != nil else { return nil }
        if prOpenedFor.contains(handle.branch) { return nil }
        guard let done = handle.lastDone else { return nil }
        // Errors and refusals produced nothing worth sending; anything else
        // may have commits, and prepare() is what verifies that honestly.
        if done.reason == .error || done.reason == .refused { return nil }
        return handle
    }

    /// The pull-request title suggested from the run's prompt.
    public var suggestedPullTitle: String {
        (prCandidate?.task.prompt.components(separatedBy: "\n").first ?? "").dartTrimmed
    }

    /// Inspect the branch; nil (with `error` set) when that failed.
    public func preparePullRequest() async -> PullRequestPreparation? {
        guard let handle = prCandidate, let record, let service = pullRequests, let gitWorkspace, !prBusy else { return nil }
        prBusy = true
        error = nil
        defer { prBusy = false }
        do {
            return try await service.prepare(
                workspace: gitWorkspace,
                record: record,
                headBranch: handle.branch,
                baseBranch: handle.baseBranch
            )
        } catch {
            self.error = "Could not inspect the branch: \(dartErrorText(error))"
            return nil
        }
    }

    /// Push and open the pull request. Returns the inline notice on success.
    public func submitPullRequest(_ preparation: PullRequestPreparation, title: String) async -> String? {
        guard preparation.isReady, let handle = prCandidate, let service = pullRequests, let gitWorkspace else { return nil }
        prBusy = true
        defer { prBusy = false }
        do {
            try await service.submit(workspace: gitWorkspace, preparation: preparation, title: title)
            prOpenedFor.insert(handle.branch)
            return "Pull request opened: \(preparation.summary)"
        } catch {
            self.error = dartErrorText(error)
            return nil
        }
    }

    public var canMoveToDesktop: Bool {
        guard let handle, desktop != nil, gitWorkspace != nil, pullRequests != nil else { return false }
        return AgentDesktopHandoff.publishedRepo(record) != nil && movedToDesktop?.branch != handle.branch
    }

    public var desktopName: String { desktop?.desktopName ?? "your desktop" }

    public func moveToDesktop() async -> AgentDesktopMove? {
        guard !moving, let handle, let desktop, let gitWorkspace, let service = pullRequests,
              let repo = AgentDesktopHandoff.publishedRepo(record) else { return nil }
        moving = true
        error = nil
        defer { moving = false }
        if !handle.isComplete {
            sessions.cancelActive()
            await handle.waitUntilDone()
        }
        do {
            try await commitLeftovers(handle)
            let push = try await service.pushBranch(gitWorkspace, repo.owner, repo.name, handle.branch)
            if push.isRejected {
                error = "ForkMesh rejected the push of \(handle.branch), so the agent stayed on this phone."
                return nil
            }
        } catch {
            self.error = "Could not push \(handle.branch): \(dartErrorText(error))"
            return nil
        }
        let task = AgentDesktopHandoff.task(
            prompt: handle.task.prompt, events: handle.events, branch: handle.branch, base: handle.baseBranch)
        do {
            let id = try await desktop.handoff(
                owner: repo.owner, name: repo.name, branch: handle.branch, base: handle.baseBranch,
                task: task, title: AgentDesktopHandoff.title(prompt: handle.task.prompt))
            let move = AgentDesktopMove(agentId: id, desktopName: desktop.desktopName, branch: handle.branch)
            movedToDesktop = move
            return move
        } catch {
            self.error = AgentDesktopHandoff.failureText(error, repo: "\(repo.owner)/\(repo.name)", branch: handle.branch)
            return nil
        }
    }

    private func commitLeftovers(_ handle: AgentRunHandle) async throws {
        let status = try await workspace.status()
        if status.isClean { return }
        if try await workspace.currentBranch() != handle.branch { return }
        try await workspace.stageAll()
        _ = try await workspace.commit(message: "Work in progress, moved to the desktop", author: author)
        if try await workspace.branches().contains(where: { $0.name == handle.baseBranch }) {
            try await workspace.checkout(handle.baseBranch)
        }
    }

    // MARK: - Referencing a file

    /// Names a working-tree file in the prompt. The model reaches for its
    /// `read_file` tool when a path is mentioned, so the shortcut people need
    /// is getting the path exactly right. Inserted at `caret` (the end when
    /// nil), backticked, with a trailing space so the sentence continues.
    public static func insertReference(_ path: String, into text: String, at caret: Int? = nil) -> (text: String, caret: Int) {
        let units = Array(text.utf16)
        let at = min(max(caret ?? units.count, 0), units.count)
        let before = String(decoding: units[0..<at], as: UTF16.self)
        let after = String(decoding: units[at...], as: UTF16.self)
        let spaced = before.isEmpty || before.hasSuffix(" ") || before.hasSuffix("\n")
        let insert = "\(spaced ? "" : " ")`\(path)` "
        return (before + insert + after, before.utf16.count + insert.utf16.count)
    }

    public func attach(_ path: String) {
        draft = Self.insertReference(path, into: draft).text
    }
}

/// Provider, key and model in one sheet.
///
/// A key is verified with one cheap request before it is stored, so a mistyped
/// key fails here rather than halfway through a coding task. Gemma 4 on this
/// iPhone takes no key; it is checked for being switched on and downloaded
/// instead, and the sheet links to On-device AI when it is not.
@MainActor
@Observable
public final class AgentSetupModel {
    @ObservationIgnored private let store: AgentCredentialStore
    @ObservationIgnored private let localModels: @MainActor () -> LocalModelManager?
    public let initial: AgentCredentials?

    public private(set) var kind: AgentProviderKind
    public var model: String
    public var key: String
    public private(set) var busy = false
    public private(set) var problem: String?

    public init(
        store: AgentCredentialStore,
        initial: AgentCredentials?,
        localModels: @escaping @MainActor () -> LocalModelManager? = { Services.get(LocalModelManager.self) }
    ) {
        self.store = store
        self.initial = initial
        self.localModels = localModels
        let kind = initial?.kind ?? .anthropic
        self.kind = kind
        self.model = initial?.model ?? kind.models.first?.id ?? ""
        self.key = initial?.apiKey ?? ""
    }

    /// Models offered for the selected provider; for Gemma, the Local AI
    /// catalog this build carries.
    public var models: [AgentModelOption] {
        if kind == .onDevice, let manager = localModels() {
            return manager.catalog.map { model in
                AgentModelOption(model.id, model.displayName, "On this iPhone · \(LocalAIFormat.bytes(model.approxDownloadBytes))")
            }
        }
        return kind.models
    }

    public var keyFieldLabel: String { "\(kind.label) API key" }

    /// For Gemma: where the model stands (shown with a link to On-device AI).
    public var readiness: OnDeviceAgentReadiness? {
        kind == .onDevice ? OnDeviceAgentReadiness.of(localModels()) : nil
    }

    public var saveLabel: String { busy ? "Checking…" : "Save" }

    public func selectKind(_ kind: AgentProviderKind) {
        self.kind = kind
        // The stored model belongs to the previous provider; reset rather
        // than sending a Claude model id to OpenAI.
        if kind == .onDevice, let manager = localModels() {
            model = manager.selectedModelId
        } else {
            model = kind.models.first?.id ?? ""
        }
        problem = nil
    }

    public func selectModel(_ id: String) {
        model = id
        if kind == .onDevice, let manager = localModels(), let chosen = manager.catalog.first(where: { $0.id == id }) {
            manager.select(chosen)
        }
        problem = nil
    }

    /// Verify and store. Returns what was saved, or nil with `problem` set.
    public func save() async -> AgentCredentials? {
        let credentials = AgentCredentials(kind: kind, apiKey: kind.needsKey ? key.dartTrimmed : "", model: model)
        if kind.needsKey && !kind.looksLikeKey(credentials.apiKey) {
            problem = "That does not look like a \(kind.label) key. \(kind.label) keys start with \(kind.keyPrefix)."
            return nil
        }
        busy = true
        problem = nil
        let failure = await store.verify(credentials)
        busy = false
        if let failure {
            problem = failure
            return nil
        }
        store.write(credentials)
        return credentials
    }

    public func remove() {
        store.clear()
    }
}

/// Lists the working tree so a path can be picked instead of typed.
/// Read-only: choosing a file only writes its path into the composer.
@MainActor
@Observable
public final class AttachFileModel {
    /// A phone should not try to render an unbounded tree, and a repository
    /// large enough to hit this is one where searching beats scrolling anyway.
    nonisolated public static let limit = 2000

    public let root: URL
    public private(set) var paths: [String] = []
    public private(set) var isLoading = true
    public private(set) var truncated = false
    public var query = ""

    public init(root: URL) {
        self.root = root
    }

    public func scan() async {
        let root = self.root
        let (found, cut) = await Task.detached(priority: .userInitiated) { () -> ([String], Bool) in
            AttachFileModel.walk(root)
        }.value
        paths = found
        truncated = cut
        isLoading = false
    }

    public var matches: [String] {
        let needle = query.dartTrimmed.lowercased()
        if needle.isEmpty { return paths }
        return paths.filter { $0.lowercased().contains(needle) }
    }

    public var emptyText: String {
        paths.isEmpty ? "This working tree has no files yet." : "No file matches that."
    }

    public var truncatedText: String {
        "Showing the first \(AttachFileModel.limit) files. Filter to narrow it down."
    }

    /// Every regular file under `root`, relative, sorted; `.git` skipped.
    /// An unreadable tree yields an empty list rather than an error: the path
    /// can still be typed by hand.
    nonisolated static func walk(_ root: URL) -> ([String], Bool) {
        let base = root.standardizedFileURL.path
        guard let enumerator = FileManager.default.enumerator(
            at: root,
            includingPropertiesForKeys: [.isRegularFileKey, .isDirectoryKey, .isSymbolicLinkKey],
            options: []
        ) else { return ([], false) }
        var found: [String] = []
        var truncated = false
        while let item = enumerator.nextObject() as? URL {
            var relative = item.standardizedFileURL.path
            if relative.hasPrefix(base) { relative = String(relative.dropFirst(base.count)) }
            while relative.hasPrefix("/") { relative.removeFirst() }
            // The object database is not source. Listing it would bury the
            // files somebody actually wants to point the model at.
            if relative == ".git" || relative.hasPrefix(".git/") {
                enumerator.skipDescendants()
                continue
            }
            let values = try? item.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
            if values?.isSymbolicLink == true || values?.isRegularFile != true { continue }
            if found.count >= limit {
                truncated = true
                break
            }
            found.append(relative)
        }
        found.sort { $0.utf16.lexicographicallyPrecedes($1.utf16) }
        return (found, truncated)
    }
}
