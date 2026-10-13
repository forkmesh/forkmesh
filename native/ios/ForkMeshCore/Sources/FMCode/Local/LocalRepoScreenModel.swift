// The state and actions of the local repository screen and its file editor,
// the row labels, and the commit author and relative time, so the SwiftUI
// screen only lays things out.

import FMAccount
import FMApi
import FMBase
import FMGit
import Foundation
import Observation

/// An inline notice; a new id per message so the same text shows twice.
public struct LocalNotice: Equatable, Identifiable, Sendable {
    public let id: UUID
    public let message: String
    public let isError: Bool

    public init(_ message: String, isError: Bool = false) {
        self.id = UUID()
        self.message = message
        self.isError = isError
    }
}

/// Labels the local repository surfaces share.
public enum LocalRepoText {
    /// `localRepoRelativeTime`: "3h ago" style, as every other screen.
    public static func relativeTime(_ date: Date, now: Date = Date()) -> String {
        Format.relativeTimeFrom(date, now: now)
    }

    /// `_humanSize`.
    public static func humanSize(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return "\(DartCore.toStringAsFixed(Double(bytes) / 1024, 1)) KB" }
        return "\(DartCore.toStringAsFixed(Double(bytes) / (1024 * 1024), 1)) MB"
    }

    /// `localRepoAuthor`: the commit identity for the signed-in account, or a
    /// deterministic fallback when there is none.
    public static func author(_ session: AuthSession?) -> GitAuthor {
        GitAuthor.fromAccount(nodeName: session?.nodeName ?? "", email: session?.email ?? "")
    }

    public static func author(_ session: AuthSession?, prefs: KeyValueStore?) -> GitAuthor {
        overriding(author(session), prefs: prefs)
    }

    public static func overriding(_ account: GitAuthor, prefs: KeyValueStore?) -> GitAuthor {
        guard let prefs, let override = SettingsService.localAuthorOverride(prefs) else { return account }
        let name: String = GitAuthor.identityField(override.name, fallback: account.name)
        let email: String = GitAuthor.identityField(override.email, fallback: "")
        return GitAuthor(name: name, email: SettingsService.isPlausibleEmail(email) ? email : account.email)
    }

    public static func defaultBranch(_ prefs: KeyValueStore?) -> String? {
        prefs.map { SettingsService.localDefaultBranch($0) }
    }

    /// `_FileRow` subtitle.
    public static func fileMeta(_ entry: LocalFileEntry) -> String {
        if entry.isDirectory { return entry.tracked ? "Folder" : "Folder · untracked" }
        return "\(humanSize(entry.size))\(entry.tracked ? "" : " · untracked")"
    }

    /// `_ChangeRow` subtitle.
    public static func changeLabel(_ change: LocalChange) -> String {
        switch change.kind {
        case .untracked: return "Untracked"
        case .added: return "Added"
        case .modified: return "Modified"
        case .deleted: return "Deleted"
        }
    }

    /// `_LocalRepoTile` subtitle: mesh identity (or branch) and last commit.
    public static func recordMeta(_ record: LocalRepoRecord, now: Date = Date()) -> String {
        let first = record.isPublished ? (record.publishedAs ?? "") : record.branch
        let second = record.lastCommitAt.map { "committed \(relativeTime($0, now: now))" } ?? "no commits yet"
        return "\(first) · \(second)"
    }

    /// The history row subtitle.
    public static func commitMeta(_ commit: LocalCommit, now: Date = Date()) -> String {
        "\(commit.shortId) · \(commit.authorName) · \(relativeTime(commit.time, now: now))"
    }

    /// The commit detail dialog body.
    public static func commitDetail(_ commit: LocalCommit, timeZone: TimeZone = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = timeZone
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        return "\(commit.id)\n\(commit.authorName) <\(commit.authorEmail)>\n\(formatter.string(from: commit.time))\n\n\(commit.message)"
    }

    /// Breadcrumb segments for `path`: ("Root", "") then each folder.
    public static func breadcrumbs(_ path: String) -> [(label: String, path: String)] {
        var out: [(label: String, path: String)] = [("Root", "")]
        var prefix = ""
        for segment in path.split(separator: "/") {
            prefix = prefix.isEmpty ? String(segment) : "\(prefix)/\(segment)"
            out.append((String(segment), prefix))
        }
        return out
    }
}

public enum LocalRepoTab: Int, CaseIterable, Sendable {
    case files, changes, branches, history

    public var label: String {
        switch self {
        case .files: return "Files"
        case .changes: return "Changes"
        case .branches: return "Branches"
        case .history: return "History"
        }
    }
}

/// What starting a publish needs next.
public enum LocalPublishStart: Equatable {
    /// This sign-in cannot publish; show the explanation in a dialog titled
    /// "This device is not a node yet".
    case notANode(String)
    /// Nothing to publish yet (a notice was set).
    case noCommits
    /// Ask for name, description and visibility.
    case ready
}

/// Workspace for one repository stored on this device: browse and edit files,
/// stage and commit, manage branches, read history, publish, push and fetch.
@MainActor
@Observable
public final class LocalRepoScreenModel {
    public let record: LocalRepoRecord
    public let workspace: GitWorkspace
    @ObservationIgnored let store: LocalRepoStore
    @ObservationIgnored let publisher: PublishService?
    @ObservationIgnored let session: @MainActor () -> AuthSession?
    @ObservationIgnored let prefs: KeyValueStore?

    public var tab: LocalRepoTab = .files
    public private(set) var directory = ""
    public private(set) var files: [LocalFileEntry]?
    public private(set) var status: LocalStatus = .empty
    public private(set) var branches: [LocalBranch] = []
    public private(set) var log: [LocalCommit] = []
    public private(set) var branch: String
    public private(set) var busyLabel: String?
    public private(set) var loadError: String?
    /// `owner/name` on the mesh once published; mirrors the store.
    public private(set) var publishedAs: String?
    public private(set) var origin: LocalRepoOrigin
    public private(set) var sync: LocalSyncState?
    public var commitMessage = ""
    public var notice: LocalNotice?

    @ObservationIgnored private var disposed = false
    @ObservationIgnored private var networkRun = 0

    public init(
        record: LocalRepoRecord,
        store: LocalRepoStore,
        publisher: PublishService?,
        session: @escaping @MainActor () -> AuthSession?,
        workspace: GitWorkspace? = nil,
        prefs: KeyValueStore? = nil
    ) {
        self.prefs = prefs
        self.record = record
        self.store = store
        self.publisher = publisher
        self.session = session
        self.workspace = workspace ?? store.workspace(record)
        self.branch = record.branch
        self.publishedAs = record.publishedAs
        self.origin = record.origin
    }

    deinit {
        // The screen is gone: stop anything still running on its workspace.
        workspace.dispose()
    }

    /// Stop anything running and release the workspace (the screen is gone).
    public func dispose() {
        disposed = true
        workspace.dispose()
    }

    public var isBusy: Bool { busyLabel != nil }
    public var isPublished: Bool { !(publishedAs ?? "").isEmpty }
    public var author: GitAuthor { LocalRepoText.author(session(), prefs: prefs) }
    public var hasCommitMessage: Bool { !commitMessage.dartTrimmed.isEmpty }
    public var canCommit: Bool { !status.staged.isEmpty && hasCommitMessage && busyLabel == nil }
    public var branchLabel: String { branch.isEmpty ? "HEAD" : branch }

    /// The mesh banner line.
    public var meshBanner: String {
        let name = publishedAs ?? ""
        return origin == .cloned ? "\(name) · cloned to this device" : "\(name) · published from this device"
    }

    public func tabCount(_ tab: LocalRepoTab) -> Int? {
        switch tab {
        case .files: return nil
        case .changes: return status.changeCount
        case .branches: return branches.count
        case .history: return log.count
        }
    }

    public func cancel() { workspace.cancel() }

    // MARK: - Loading

    public func refreshAll() async {
        _ = await guarded("Reading repository", onError: { [weak self] message in self?.loadError = message }) {
            let summary = try await self.workspace.summary()
            let files = try await self.workspace.listFiles(self.directory)
            let status = try await self.workspace.status()
            let branches = try await self.workspace.branches()
            let log = try await self.workspace.log()
            if self.disposed { return }
            self.branch = summary.branch
            self.files = files
            self.status = status
            self.branches = branches
            self.log = log
            self.loadError = nil
            _ = try? await self.store.updateSummary(self.record.name, summary)
        }
        if isPublished { await refreshSync() }
    }

    /// Cheap local comparison of the branch against its remote-tracking ref.
    public func refreshSync() async {
        if branch.isEmpty { return }
        do {
            let state = try await workspace.syncState(branch: branch)
            if !disposed { sync = state }
        } catch {
            // A repository that has never fetched simply has no tracking ref.
            if !disposed { sync = nil }
        }
    }

    func refreshFiles() async {
        _ = await guarded("Listing files") {
            let files = try await self.workspace.listFiles(self.directory)
            if !self.disposed { self.files = files }
        }
    }

    func refreshStatus() async {
        _ = await guarded("Checking status") {
            let status = try await self.workspace.status()
            if !self.disposed { self.status = status }
        }
    }

    /// Run an engine operation with the busy overlay, mapping failures to a
    /// notice (or `onError`).
    @discardableResult
    func guarded(_ label: String, onError: ((String) -> Void)? = nil, _ body: () async throws -> Void) async -> Bool {
        if disposed { return false }
        busyLabel = label
        defer { if !disposed { busyLabel = nil } }
        do {
            try await body()
            return true
        } catch {
            if disposed { return false }
            let message = LocalRepoScreenModel.message(error)
            if let onError { onError(message) } else { say(message, error: true) }
            return false
        }
    }

    static func message(_ error: Error) -> String {
        if let error = error as? GitWorkspaceException { return error.message }
        if let error = error as? LocalRepoStoreException { return error.message }
        return dartErrorText(error)
    }

    func say(_ message: String, error: Bool = false) {
        if disposed { return }
        notice = LocalNotice(message, isError: error)
    }

    // MARK: - Files

    public func openDirectory(_ path: String) async {
        directory = path
        files = nil
        await refreshFiles()
    }

    /// Creates an empty file; returns the entry to open in the editor.
    public func newFile(_ rawPath: String) async -> LocalFileEntry? {
        let path = rawPath.dartTrimmed
        if path.isEmpty { return nil }
        let ok = await guarded("Creating file") { try await self.workspace.writeFile(path, "") }
        if !ok { return nil }
        await refreshAll()
        let name = path.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? path
        return LocalFileEntry(name: name, path: path, kind: .file, size: 0, tracked: false)
    }

    /// The "New file" dialog's starting text.
    public var newFilePrefill: String { directory.isEmpty ? "" : "\(directory)/" }

    // MARK: - Changes

    public func stage(_ paths: [String]) async {
        if await guarded("Staging", { try await self.workspace.stage(paths) }) { await refreshStatus() }
    }

    public func unstage(_ paths: [String]) async {
        if await guarded("Unstaging", { try await self.workspace.unstage(paths) }) { await refreshStatus() }
    }

    public func stageAll() async {
        if await guarded("Staging all", { try await self.workspace.stageAll() }) { await refreshStatus() }
    }

    public func unstageAll() async {
        if await guarded("Unstaging all", { try await self.workspace.unstageAll() }) { await refreshStatus() }
    }

    /// Drop the changes to `path` (after the view's confirmation).
    public func discard(_ path: String) async {
        if await guarded("Discarding", { try await self.workspace.discard([path]) }) { await refreshAll() }
    }

    public func commit() async {
        let message = commitMessage.dartTrimmed
        let author = self.author
        var id = ""
        let ok = await guarded("Committing") {
            id = try await self.workspace.commit(message: message, author: author)
        }
        if !ok { return }
        commitMessage = ""
        say("Committed \(String(id.prefix(7)))")
        await refreshAll()
    }

    // MARK: - Branches

    public func checkout(_ target: LocalBranch) async {
        if target.isCurrent { return }
        if !status.isClean {
            say("Commit or discard your changes before switching branches.", error: true)
            tab = .changes
            return
        }
        if await guarded("Switching to \(target.name)", { try await self.workspace.checkout(target.name) }) {
            await refreshAll()
        }
    }

    /// The "Switch to it" default (and whether it may be changed).
    public var canSwitchToNewBranch: Bool { status.isClean }

    public func createBranch(_ rawName: String, checkout: Bool) async {
        let name = rawName.dartTrimmed
        if name.isEmpty { return }
        if await guarded("Creating branch", { try await self.workspace.createBranch(name, checkout: checkout) }) {
            await refreshAll()
        }
    }

    // MARK: - Publish, push, fetch

    /// Head facts advertised in the catalog record.
    public func head() -> PublishHead {
        guard let first = log.first, let last = log.last else { return PublishHead() }
        return PublishHead(
            commit: first.id,
            rootCommit: last.parents.isEmpty ? last.id : "",
            subject: first.subject,
            authorName: first.authorName,
            at: first.time,
            commitCount: log.count,
            branchCount: branches.count)
    }

    /// Why this session cannot publish or push, or nil.
    public var publishRefusal: String? { PublishService.describeSession(session()) }

    public var canPublishHere: Bool { publisher != nil }

    public func beginPublish() -> LocalPublishStart {
        if let refusal = publishRefusal { return .notANode(refusal) }
        if log.isEmpty {
            say("Make a commit before publishing.", error: true)
            return .noCommits
        }
        return .ready
    }

    /// Push the branch and publish the catalog record. Returns the outcome
    /// (the view offers a rename for `.mirrored`); nil when it never ran.
    public func publish(name: String, description: String, isPublic: Bool) async -> PublishOutcome? {
        guard let publisher else { return nil }
        let session = self.session()
        var result: PublishOutcome?
        await runNetwork("Publishing \(name)") { report in
            let outcome = try await publisher.publish(
                session: session,
                workspace: self.workspace,
                repo: name,
                branch: self.branch,
                isPublic: isPublic,
                description: description,
                head: self.head(),
                // A cloned repository published under this account is a fork of
                // where it came from, and `publishedAs` still names that
                // upstream until the publish rewrites it. Publishing back to
                // the same address is a mirror, not a fork; the normalizer
                // drops the self-edge.
                forkedFrom: self.record.isCloned ? (self.record.publishedAs ?? "") : "",
                onProgress: report)
            if outcome.isSuccess {
                try await self.store.markPublished(self.record.name, outcome.publishedAs)
                if !self.disposed { self.publishedAs = outcome.publishedAs }
            }
            if outcome.status != .mirrored { self.say(outcome.message, error: !outcome.isSuccess) }
            result = outcome
        }
        await refreshSync()
        return result
    }

    private var remoteTarget: (owner: String, repo: String)? {
        let target = publishedAs ?? ""
        guard target.contains("/") else { return nil }
        let parts = target.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        return (parts.first ?? "", parts.last ?? "")
    }

    public func push() async {
        guard let publisher, let target = remoteTarget else { return }
        let branch = self.branch
        let label = publishedAs ?? ""
        await runNetwork("Pushing \(branch)") { report in
            let result = try await publisher.push(
                workspace: self.workspace, owner: target.owner, repo: target.repo, branch: branch, onProgress: report)
            if let rejected = result.refs.first(where: { $0.isRejected }) {
                self.say(rejected.message.isEmpty
                    ? "ForkMesh refused \(rejected.branch)."
                    : "ForkMesh refused \(rejected.branch): \(rejected.message)", error: true)
            } else {
                self.say(result.isUpToDate ? "ForkMesh already has \(branch)." : "Pushed \(branch) to \(label).")
            }
        }
        await refreshSync()
    }

    public func fetch() async {
        guard let publisher, let target = remoteTarget else { return }
        let label = publishedAs ?? ""
        await runNetwork("Fetching \(label)") { report in
            let result = try await publisher.fetch(workspace: self.workspace, owner: target.owner, repo: target.repo, onProgress: report)
            self.say(LocalRepoScreenModel.fetchMessage(result))
        }
        await refreshSync()
    }

    public func pull() async {
        guard let publisher, let target = remoteTarget else { return }
        let branch = self.branch
        await runNetwork("Syncing \(branch)") { report in
            let result = try await publisher.pull(
                workspace: self.workspace, owner: target.owner, repo: target.repo, branch: branch, onProgress: report)
            let from = result.fetch.servedBy.map { " (from \($0))" } ?? ""
            self.say(result.message + from)
        }
        await refreshAll()
    }

    static func fetchMessage(_ result: LocalFetchResult) -> String {
        let count = result.updatedRefs.count
        let from = result.servedBy.map { " from \($0)" } ?? ""
        return result.isUpToDate
            ? "Already up to date with ForkMesh\(from)."
            : "Fetched \(count) branch\(count == 1 ? "" : "es")\(from)."
    }

    /// Busy overlay whose label follows the engine's progress.
    func runNetwork(
        _ label: String,
        _ body: (@escaping @Sendable (GitTransferProgress) -> Void) async throws -> Void
    ) async {
        if disposed { return }
        networkRun += 1
        let run = networkRun
        busyLabel = label
        defer { if !disposed { busyLabel = nil } }
        let report: @Sendable (GitTransferProgress) -> Void = { [weak self] progress in
            Task { @MainActor in
                guard let self, !self.disposed, self.networkRun == run, self.busyLabel != nil else { return }
                self.busyLabel = progress.label
            }
        }
        do {
            try await body(report)
        } catch {
            if !disposed { say(LocalRepoScreenModel.message(error), error: true) }
        }
        networkRun += 1
    }

    // MARK: - Delete

    /// Delete the repository from this device (after the view's user-presence
    /// check). True when it is gone and the screen should close.
    public func deleteRepository() async -> Bool {
        await guarded("Deleting repository") { try await self.store.delete(self.record.name) }
    }
}

/// Code editor for one worktree file with an explicit Save.
@MainActor
@Observable
public final class LocalFileEditorModel {
    public let workspace: GitWorkspace
    public let path: String

    public var text = ""
    public private(set) var content: LocalFileContent?
    public private(set) var error: String?
    public private(set) var saved = ""
    public private(set) var busy = false
    /// Whether anything was saved or deleted (the repository screen refreshes).
    public private(set) var changed = false
    public var notice: LocalNotice?

    public init(workspace: GitWorkspace, path: String) {
        self.workspace = workspace
        self.path = path
    }

    public var fileName: String { path.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? path }
    public var dirty: Bool { text != saved }
    public var canSave: Bool { (content?.isEditable ?? false) && dirty && !busy }

    /// The non-editable explanation, when the file cannot be edited.
    public var blockedTitle: String? {
        guard let content, !content.isEditable else { return nil }
        return content.isBinary ? "Binary file" : "File too large"
    }

    public var blockedMessage: String {
        guard let content else { return "" }
        let size = LocalRepoText.humanSize(content.bytes)
        return content.isBinary ? "\(size) · not editable on this device." : "\(size) exceeds the in-app editor limit."
    }

    public func load() async {
        do {
            let loaded = try await workspace.readFile(path)
            content = loaded
            saved = loaded.text
            text = loaded.text
            error = nil
        } catch {
            self.error = LocalRepoScreenModel.message(error)
        }
    }

    public func save() async {
        busy = true
        defer { busy = false }
        let current = text
        do {
            try await workspace.writeFile(path, current)
            saved = current
            changed = true
            notice = LocalNotice("Saved")
        } catch {
            notice = LocalNotice(LocalRepoScreenModel.message(error), isError: true)
        }
    }

    /// Delete the file (after the view's confirmation). True when the editor
    /// should close.
    public func delete() async -> Bool {
        busy = true
        do {
            try await workspace.deleteFile(path)
            changed = true
            return true
        } catch {
            busy = false
            notice = LocalNotice(LocalRepoScreenModel.message(error), isError: true)
            return false
        }
    }

    public static let deleteMessageSuffix = " is removed from the working tree. Stage the deletion to record it in the next commit."
}
