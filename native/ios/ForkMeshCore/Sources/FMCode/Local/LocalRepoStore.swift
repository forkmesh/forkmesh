import FMAccount
import FMApi
import FMBase
import FMGit
import Foundation
import Observation

/// Where a local repository came from.
public enum LocalRepoOrigin: String, Sendable {
    /// Created or imported on this device.
    case authored
    /// Cloned from the mesh.
    case cloned
}

/// One repository stored on this device.
public struct LocalRepoRecord: Equatable, Hashable, Sendable, Identifiable {
    /// Slug; also the directory name under the store root.
    public let name: String

    /// Absolute worktree path.
    ///
    /// Always `<store root>/<name>` — see `LocalRepoStore.directoryFor`, which
    /// is the only thing that ever creates one. It is persisted for
    /// convenience and for older builds to read, but it is a **cache of where
    /// the worktree was**, not the authority on where it is: see `rebasedOn`.
    public let path: String
    public let description: String
    public let createdAt: Date
    public let branch: String
    public let lastCommitSubject: String
    public let lastCommitAt: Date?

    /// `owner/name` on the network once published or cloned, else nil.
    public let publishedAs: String?

    /// Whether this worktree was authored here or cloned from the mesh.
    public let origin: LocalRepoOrigin

    public init(
        name: String,
        path: String,
        createdAt: Date,
        description: String = "",
        branch: String = "main",
        lastCommitSubject: String = "",
        lastCommitAt: Date? = nil,
        publishedAs: String? = nil,
        origin: LocalRepoOrigin = .authored
    ) {
        self.name = name
        self.path = path
        self.createdAt = createdAt
        self.description = description
        self.branch = branch
        self.lastCommitSubject = lastCommitSubject
        self.lastCommitAt = lastCommitAt
        self.publishedAs = publishedAs
        self.origin = origin
    }

    public var id: String { name }
    public var worktree: URL { URL(fileURLWithPath: path, isDirectory: true) }
    public var isPublished: Bool { !(publishedAs ?? "").isEmpty }
    public var isCloned: Bool { origin == .cloned }

    public var meshRoute: (owner: String, repo: String)? {
        let route = (publishedAs ?? "").dartTrimmed
        guard let slash = route.firstIndex(of: "/"), slash != route.startIndex,
              route.index(after: slash) != route.endIndex
        else { return nil }
        return (String(route[..<slash]), String(route[route.index(after: slash)...]))
    }

    /// One-word sync state for the unified repository list.
    public var syncLabel: String { !isPublished ? "Local only" : isCloned ? "Cloned" : "Published" }

    /// This record with its worktree resolved under `root`.
    ///
    /// The stored `path` is absolute, and on iOS an app's container UUID
    /// changes on reinstall and on some upgrades. The files move with the
    /// container, so the worktree is still there — but every path recorded
    /// before the move names a directory that no longer exists. Since the
    /// layout is always `<root>/<name>`, the current path is derivable, and
    /// deriving it is what keeps a container move from looking like the
    /// repository was deleted.
    public func rebasedOn(_ root: URL) -> LocalRepoRecord {
        let expected = "\(root.path)/\(name)"
        if expected == path { return self }
        return LocalRepoRecord(
            name: name, path: expected, createdAt: createdAt, description: description, branch: branch,
            lastCommitSubject: lastCommitSubject, lastCommitAt: lastCommitAt, publishedAs: publishedAs, origin: origin)
    }

    public func copyWith(
        description: String? = nil,
        branch: String? = nil,
        lastCommitSubject: String? = nil,
        lastCommitAt: Date? = nil,
        clearLastCommit: Bool = false,
        publishedAs: String? = nil,
        clearPublishedAs: Bool = false,
        origin: LocalRepoOrigin? = nil
    ) -> LocalRepoRecord {
        LocalRepoRecord(
            name: name,
            path: path,
            createdAt: createdAt,
            description: description ?? self.description,
            branch: branch ?? self.branch,
            lastCommitSubject: lastCommitSubject ?? self.lastCommitSubject,
            lastCommitAt: clearLastCommit ? nil : (lastCommitAt ?? self.lastCommitAt),
            publishedAs: clearPublishedAs ? nil : (publishedAs ?? self.publishedAs),
            origin: origin ?? self.origin)
    }

    public func toJSON() -> JSONObject {
        var json: JSONObject = [:]
        json["name"] = .string(name)
        json["path"] = .string(path)
        json["description"] = .string(description)
        json["createdAt"] = .string(DartISO.utcString(createdAt))
        json["branch"] = .string(branch)
        json["lastCommit"] = .string(lastCommitSubject)
        json["lastCommitAt"] = lastCommitAt.map { .string(DartISO.utcString($0)) } ?? .null
        json["publishedAs"] = publishedAs.map { .string($0) } ?? .null
        json["origin"] = .string(origin.rawValue)
        return json
    }

    public static func fromJSON(_ json: JSONObject) -> LocalRepoRecord? {
        guard let name = json["name"]?.stringValue, let path = json["path"]?.stringValue else { return nil }
        if !LocalRepoStore.isValidName(name) { return nil }
        func text(_ key: String, _ fallback: String) -> String {
            guard let value = json[key], !value.isNull else { return fallback }
            return DartCore.toString(value)
        }
        func date(_ key: String) -> Date? {
            DartCore.dateTimeTryParseMs(text(key, "")).map { Date(epochMillis: Int64($0)) }
        }
        return LocalRepoRecord(
            name: name,
            path: path,
            createdAt: date("createdAt") ?? Date(timeIntervalSince1970: 0),
            description: text("description", ""),
            branch: text("branch", "main"),
            lastCommitSubject: text("lastCommit", ""),
            lastCommitAt: date("lastCommitAt"),
            publishedAs: json["publishedAs"]?.stringValue,
            origin: json["origin"]?.stringValue == LocalRepoOrigin.cloned.rawValue ? .cloned : .authored)
    }
}

/// Thrown for invalid names, duplicates, and unsafe paths.
public struct LocalRepoRelocation: Equatable, Sendable {
    public let moved: [String]
    public let leftBehind: [String]

    public init(moved: [String], leftBehind: [String]) {
        self.moved = moved
        self.leftBehind = leftBehind
    }
}

public struct LocalRepoStoreException: Error, Equatable, Sendable, CustomStringConvertible, LocalizedError {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
    public var errorDescription: String? { message }
}

/// App-private root for on-device repositories plus a small JSON index.
@MainActor
@Observable
public final class LocalRepoStore {
    nonisolated public static let indexFileName = "index.json"
    nonisolated public static let indexVersion = 1

    public private(set) var root: URL
    @ObservationIgnored let performanceMonitor: PerformanceMonitorService?
    @ObservationIgnored let now: () -> Date

    /// Records as last loaded, most recent first; call `ensureLoaded` or `list` first.
    public private(set) var records: [LocalRepoRecord] = []
    public private(set) var isLoaded = false
    /// Why the last `ensureLoaded()` failed, or nil.
    public private(set) var loadError: String?

    /// Bumped on every change, for observers outside SwiftUI.
    public private(set) var revision = 0

    private var activeWorkspace: GitWorkspace?
    @ObservationIgnored private var loading: Task<Void, Error>?
    @ObservationIgnored private var listeners: [UUID: @MainActor () -> Void] = [:]

    /// Builds the workspace for a create/import/clone; tests substitute one
    /// with a fake transport.
    @ObservationIgnored var workspaceFactory: ((URL) -> GitWorkspace)?

    public init(root: URL, performanceMonitor: PerformanceMonitorService? = nil, now: @escaping () -> Date = { Date() }) {
        self.root = root.standardizedFileURL
        self.performanceMonitor = performanceMonitor
        self.now = now
    }

    /// `<base>/forkmesh/repos`.
    nonisolated public static func defaultRoot(in base: URL) -> URL {
        base.appendingPathComponent("forkmesh", isDirectory: true).appendingPathComponent("repos", isDirectory: true)
    }

    nonisolated public static func root(for scope: LocalRepoScope, documents: URL) -> URL {
        defaultRoot(in: scope.directory(documents))
    }

    /// True while `create`, `importDirectory`, `importZip` or `cloneFromMesh` is running.
    public var isBusy: Bool { activeWorkspace != nil }

    /// Cancel the in-flight create/import/clone, if any.
    public func cancelActive() { activeWorkspace?.cancel() }

    // MARK: - Listeners

    /// Called after every change.
    @discardableResult
    public func addListener(_ listener: @escaping @MainActor () -> Void) -> UUID {
        let id = UUID()
        listeners[id] = listener
        return id
    }

    public func removeListener(_ id: UUID) { listeners[id] = nil }

    private func notifyListeners() {
        revision += 1
        for listener in listeners.values { listener() }
    }

    // MARK: - Names

    nonisolated public static func isValidName(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        guard (1...100).contains(bytes.count) else { return false }
        let ok = bytes.allSatisfy { byte in
            (0x41...0x5a).contains(byte) || (0x61...0x7a).contains(byte) || (0x30...0x39).contains(byte)
                || byte == 0x2e || byte == 0x5f || byte == 0x2d
        }
        return ok && name != "." && name != ".."
    }

    /// Explain why `name` is rejected, or nil when it is acceptable.
    nonisolated public static func validateName(_ name: String) -> String? {
        if name.isEmpty { return "Enter a name." }
        if name.utf16.count > 100 { return "Keep the name under 100 characters." }
        if !isValidName(name) { return "Use letters, digits, dots, dashes, or underscores only." }
        return nil
    }

    /// Resolve and validate the worktree directory for `name`.
    public func directoryFor(_ name: String) throws -> URL {
        if !LocalRepoStore.isValidName(name) {
            throw LocalRepoStoreException("Repository name is not valid.")
        }
        try ensureSafeRoot()
        let directory = root.appendingPathComponent(name, isDirectory: true)
        if EngineFS.entityType(directory.path, followLinks: false) == .link {
            throw LocalRepoStoreException("Repository path is a symlink and was refused.")
        }
        return directory
    }

    private func ensureSafeRoot() throws {
        try LocalRepoStore.ensureSafeDirectory(root)
    }

    nonisolated static func ensureSafeDirectory(_ directory: URL) throws {
        // Never follow a symlinked store root; create it if missing.
        var current = directory.path
        var missing: [String] = []
        while true {
            let type = EngineFS.entityType(current, followLinks: false)
            if type == .directory { break }
            if type == .link {
                throw LocalRepoStoreException("Local repository root is a symlink and was refused.")
            }
            if type != .notFound {
                throw LocalRepoStoreException("Local repository root is not a directory.")
            }
            missing.append(current)
            let parent = (current as NSString).deletingLastPathComponent
            if parent == current || parent.isEmpty { break }
            current = parent
        }
        for directory in missing.reversed() {
            do {
                try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: false)
            } catch {
                if EngineFS.entityType(directory, followLinks: false) != .directory { throw error }
            }
        }
    }

    // MARK: - Loading

    /// Load the index once. Never throws: a failure is
    /// kept in `loadError` and the records stay empty; `loadIfNeeded()` is
    /// the throwing form.
    public func ensureLoaded() async {
        do {
            try await loadIfNeeded()
            loadError = nil
        } catch {
            loadError = (error as? LocalRepoStoreException)?.message ?? String(describing: error)
        }
    }

    /// `ensureLoaded` that throws the load failure (`LocalRepoStoreException`
    /// for an unsafe root).
    public func loadIfNeeded() async throws {
        if isLoaded { return }
        if let loading {
            try await loading.value
            return
        }
        let task = Task { @MainActor in try self.load() }
        loading = task
        defer { loading = nil }
        try await task.value
    }

    private func load() throws {
        try ensureSafeRoot()
        let loaded = LocalRepoStore.readIndex(in: root)
        // Resolve every worktree under the *current* root before judging
        // whether it vanished. The recorded path is only where the worktree
        // used to be, and an app container that moved (see `rebasedOn`) makes
        // every one of them stale at once. Checking the stale path instead
        // would find nothing on disk, drop every record as vanished, and then
        // persist that verdict — turning a directory the OS merely relocated
        // into an index wiped of repositories that are still sitting there.
        var changed = false
        var alive: [LocalRepoRecord] = []
        for record in loaded {
            let rebased = record.rebasedOn(root)
            if rebased.path != record.path { changed = true }
            // A worktree replaced by a symlink is refused here exactly as
            // `directoryFor` refuses one.
            if EngineFS.entityType(rebased.path, followLinks: false) == .directory {
                alive.append(rebased)
            } else {
                // Genuinely gone — cleared by the OS, or deleted outside the app.
                changed = true
            }
        }
        if adoptOrphans(&alive) { changed = true }
        records = LocalRepoStore.sortedByRecency(alive)
        isLoaded = true
        // Rewrite so the index heals itself: the next load reads paths that
        // are already correct instead of rebasing them again.
        if changed { try persist() }
        notifyListeners()
    }

    /// Add worktrees sitting under `root` that the index does not list, and
    /// report whether any were found.
    ///
    /// The index and the filesystem are two records of the same thing, and
    /// only one of them holds the actual work. When they disagree, the
    /// directories win: a repository whose index entry was lost is otherwise
    /// invisible and unreachable forever — and worse, `reserve` refuses to
    /// create a repository whose folder already exists, so the name cannot
    /// even be used again.
    private func adoptOrphans(_ known: inout [LocalRepoRecord]) -> Bool {
        var named = Set(known.map(\.name))
        var adopted = false
        guard let names = try? EngineFS.listNames(root.path) else {
            return false // An unreadable root is not a reason to fail the load.
        }
        for name in names {
            let path = "\(root.path)/\(name)"
            if EngineFS.entityType(path, followLinks: false) != .directory { continue }
            if !LocalRepoStore.isValidName(name) || named.contains(name) { continue }
            // A worktree, not just any folder: `<root>/<name>/.git`.
            if EngineFS.entityType("\(path)/.git", followLinks: false) != .directory { continue }
            let attributes = try? FileManager.default.attributesOfItem(atPath: path)
            let createdAt = (attributes?[.modificationDate] as? Date) ?? Date(timeIntervalSince1970: 0)
            // Deliberately metadata-light: branch, last commit and description
            // fill in from `updateSummary` the first time the repository is
            // opened.
            known.append(LocalRepoRecord(name: name, path: path, createdAt: createdAt))
            named.insert(name)
            adopted = true
        }
        return adopted
    }

    static func sortedByRecency(_ records: [LocalRepoRecord]) -> [LocalRepoRecord] {
        records.enumerated().sorted { a, b in
            let at = a.element.lastCommitAt ?? a.element.createdAt
            let bt = b.element.lastCommitAt ?? b.element.createdAt
            if at != bt { return at > bt }
            return a.offset < b.offset
        }.map(\.element)
    }

    private func persist() throws {
        try LocalRepoStore.writeIndex(records, in: root)
    }

    nonisolated static func readIndex(in directory: URL) -> [LocalRepoRecord] {
        let file = "\(directory.path)/\(indexFileName)"
        guard EngineFS.entityType(file, followLinks: false) == .file,
              let data = FileManager.default.contents(atPath: file),
              let json = try? JSON.parse(data),
              let repos = json.objectValue?["repos"]?.arrayValue
        else { return [] }
        return repos.compactMap { $0.objectValue.flatMap(LocalRepoRecord.fromJSON) }
    }

    nonisolated static func writeIndex(_ records: [LocalRepoRecord], in directory: URL) throws {
        try ensureSafeDirectory(directory)
        let payload: JSONObject = [
            "version": .int(Int64(indexVersion)),
            "repos": .array(records.map { .object($0.toJSON()) }),
        ]
        let text = JSON.encodePretty(.object(payload), indent: "  ")
        let file = "\(directory.path)/\(indexFileName)"
        let tmp = file + ".tmp"
        try Data(text.utf8).write(to: URL(fileURLWithPath: tmp))
        try EngineFS.rename(tmp, file)
    }

    public func relocate(to newRoot: URL) async throws -> LocalRepoRelocation {
        if isBusy {
            throw LocalRepoStoreException("Wait for the current repository task to finish, then try again.")
        }
        let target = newRoot.standardizedFileURL
        if target.path == root.path { return LocalRepoRelocation(moved: [], leftBehind: []) }
        if target.path.hasPrefix(root.path + "/") {
            throw LocalRepoStoreException("Pick a folder outside the one the repositories are in now.")
        }
        try await loadIfNeeded()
        var arrived = LocalRepoStore.readIndex(in: target).map { $0.rebasedOn(target) }
        try LocalRepoStore.writeIndex(arrived, in: target)
        var moved: [String] = []
        var stayed: [LocalRepoRecord] = []
        for record in records {
            let from = root.appendingPathComponent(record.name, isDirectory: true)
            let to = target.appendingPathComponent(record.name, isDirectory: true)
            guard EngineFS.entityType(from.path, followLinks: false) == .directory,
                  EngineFS.entityType(to.path, followLinks: false) == .notFound,
                  (try? FileManager.default.moveItem(at: from, to: to)) != nil
            else {
                stayed.append(record)
                continue
            }
            arrived.removeAll { $0.name == record.name }
            arrived.append(record.rebasedOn(target))
            moved.append(record.name)
        }
        try LocalRepoStore.writeIndex(LocalRepoStore.sortedByRecency(arrived), in: target)
        try? LocalRepoStore.writeIndex(stayed, in: root)
        root = target
        records = []
        isLoaded = false
        try await loadIfNeeded()
        return LocalRepoRelocation(moved: moved, leftBehind: stayed.map(\.name))
    }

    /// Reload from disk and return every record (most recent first).
    public func list() async throws -> [LocalRepoRecord] {
        isLoaded = false
        try await loadIfNeeded()
        return records
    }

    nonisolated public static func alreadyOnDevice(_ identity: String, as name: String) -> String {
        "\(identity) is already on this device as \(name). Open it and sync instead."
    }

    public func record(meshIdentity: String) -> LocalRepoRecord? {
        let wanted = meshIdentity.dartTrimmed.lowercased()
        if wanted.isEmpty { return nil }
        return records.first { ($0.publishedAs ?? "").dartTrimmed.lowercased() == wanted }
    }

    public func record(for repository: Repository) -> LocalRepoRecord? {
        record(meshIdentity: repository.fullName) ?? record(meshIdentity: "\(repository.owner)/\(repository.name)")
    }

    public func sync(
        _ record: LocalRepoRecord,
        publisher: PublishService,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalPullResult {
        guard let route = record.meshRoute else {
            throw LocalRepoStoreException("\(record.name) is only on this device. Publish it before syncing.")
        }
        let workspace = makeWorkspace(record.worktree)
        activeWorkspace = workspace
        defer {
            if activeWorkspace === workspace { activeWorkspace = nil }
            workspace.dispose()
        }
        let branch = try await workspace.summary().branch
        if branch == "HEAD" {
            throw LocalRepoStoreException("Switch \(record.name) to a branch before syncing.")
        }
        let result = try await publisher.pull(
            workspace: workspace, owner: route.owner, repo: route.repo, branch: branch, onProgress: onProgress)
        if let summary = try? await workspace.summary() {
            _ = try? await updateSummary(record.name, summary)
        }
        return result
    }

    public func find(_ name: String) async throws -> LocalRepoRecord? {
        try await loadIfNeeded()
        return records.first { $0.name == name }
    }

    /// Workspace bound to `record`'s worktree. The caller owns it: `dispose()`
    /// it when done.
    public func workspace(_ record: LocalRepoRecord) -> GitWorkspace {
        GitWorkspace(worktree: record.worktree, performanceMonitor: performanceMonitor)
    }

    func makeWorkspace(_ directory: URL) -> GitWorkspace {
        workspaceFactory?(directory) ?? GitWorkspace(worktree: directory, performanceMonitor: performanceMonitor)
    }

    // MARK: - Create, import, clone

    /// Create an empty repository seeded with a README and an initial commit.
    public func create(
        name: String, author: GitAuthor, description: String = "", branch: String? = nil
    ) async throws -> LocalRepoRecord {
        try await withNewWorkspace(name) { directory, workspace in
            var readme = "# \(name)\n"
            let trimmed = description.dartTrimmed
            if !trimmed.isEmpty { readme += "\n\(trimmed)\n" }
            try await workspace.initialize(
                files: ["README.md": readme], commitMessage: "Initial commit\n", author: author, branch: branch)
            return try await self.register(name, directory, description, workspace)
        }
    }

    /// Import a folder chosen by the user into a new repository.
    public func importDirectory(
        name: String, source: URL, author: GitAuthor, description: String = "", branch: String? = nil
    ) async throws -> LocalRepoRecord {
        try await withNewWorkspace(name) { directory, workspace in
            _ = try await workspace.importFromDirectory(source, author: author, message: "Import from Files\n", branch: branch)
            return try await self.register(name, directory, description, workspace)
        }
    }

    /// Import a zip archive chosen by the user into a new repository.
    public func importZip(
        name: String, zipPath: String, author: GitAuthor, description: String = "", branch: String? = nil
    ) async throws -> LocalRepoRecord {
        try await withNewWorkspace(name) { directory, workspace in
            _ = try await workspace.importFromZip(zipPath, author: author, message: "Import from Files\n", branch: branch)
            return try await self.register(name, directory, description, workspace)
        }
    }

    /// Clone a mesh repository into a new on-device worktree.
    ///
    /// Refuses a pack larger than `maxPackBytes` so a huge repository cannot
    /// fill the device; a failed clone leaves nothing behind. `onProgress` is
    /// called off the main actor.
    public func cloneFromMesh(
        name: String,
        credentials: ForkMeshRemoteCredentials,
        description: String = "",
        branch: String? = nil,
        depth: Int? = nil,
        maxPackBytes: Int = GitWorkspace.maxCloneBytes,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalRepoRecord {
        try await loadIfNeeded()
        let identity = "\(credentials.owner)/\(credentials.repo)"
        if let existing = record(meshIdentity: identity) {
            throw LocalRepoStoreException(LocalRepoStore.alreadyOnDevice(identity, as: existing.name))
        }
        return try await withNewWorkspace(name) { directory, workspace in
            _ = try await workspace.clone(
                credentials: credentials, branch: branch, depth: depth, maxPackBytes: maxPackBytes, onProgress: onProgress)
            let record = try await self.register(name, directory, description, workspace)
            let publishedAs = "\(credentials.owner)/\(credentials.repo)"
            try await self.mark(name) { $0.copyWith(publishedAs: publishedAs, origin: .cloned) }
            return record.copyWith(publishedAs: publishedAs, origin: .cloned)
        }
    }

    private func withNewWorkspace(
        _ name: String,
        _ body: (URL, GitWorkspace) async throws -> LocalRepoRecord
    ) async throws -> LocalRepoRecord {
        let directory = try await reserve(name)
        let workspace = makeWorkspace(directory)
        activeWorkspace = workspace
        defer {
            activeWorkspace = nil
            workspace.dispose()
        }
        do {
            return try await body(directory, workspace)
        } catch {
            cleanupFailed(directory)
            throw error
        }
    }

    // MARK: - Updates

    /// Re-read branch + last commit for `name` and persist the index.
    public func refresh(_ name: String) async throws -> LocalRepoRecord? {
        guard let record = try await find(name) else { return nil }
        let workspace = self.workspace(record)
        defer { workspace.dispose() }
        return try await updateSummary(name, try await workspace.summary())
    }

    /// Persist an already-computed `summary` for `name` (no engine call), so a
    /// screen that owns the repository's workspace can keep the index fresh
    /// without a second workspace racing the first.
    @discardableResult
    public func updateSummary(_ name: String, _ summary: LocalRepoSummary) async throws -> LocalRepoRecord? {
        guard let record = try await find(name) else { return nil }
        let updated = record.copyWith(
            branch: summary.branch,
            lastCommitSubject: summary.lastCommitSubject,
            lastCommitAt: summary.lastCommitAt,
            clearLastCommit: summary.lastCommitAt == nil)
        records = LocalRepoStore.sortedByRecency(records.map { $0.name == name ? updated : $0 })
        try persist()
        notifyListeners()
        return updated
    }

    /// Record the network identity once a repository has been published.
    public func markPublished(_ name: String, _ publishedAs: String) async throws {
        try await mark(name) { $0.copyWith(publishedAs: publishedAs) }
    }

    private func mark(_ name: String, _ update: (LocalRepoRecord) -> LocalRepoRecord) async throws {
        try await loadIfNeeded()
        records = records.map { $0.name == name ? update($0) : $0 }
        try persist()
        notifyListeners()
    }

    /// Permanently delete the worktree and its index entry.
    public func delete(_ name: String) async throws {
        try await loadIfNeeded()
        let directory = try directoryFor(name)
        if EngineFS.entityType(directory.path, followLinks: false) == .directory {
            try FileManager.default.removeItem(at: directory)
        }
        records = records.filter { $0.name != name }
        try persist()
        notifyListeners()
    }

    private func reserve(_ name: String) async throws -> URL {
        if let problem = LocalRepoStore.validateName(name) { throw LocalRepoStoreException(problem) }
        try await loadIfNeeded()
        if records.contains(where: { $0.name == name }) {
            throw LocalRepoStoreException("A local repository with that name already exists.")
        }
        let directory = try directoryFor(name)
        if EngineFS.entityType(directory.path, followLinks: false) != .notFound {
            throw LocalRepoStoreException("A folder with that name already exists on this device.")
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        return directory
    }

    private func register(_ name: String, _ directory: URL, _ description: String, _ workspace: GitWorkspace) async throws -> LocalRepoRecord {
        let summary = try await workspace.summary()
        let record = LocalRepoRecord(
            name: name,
            path: directory.path,
            createdAt: now(),
            description: description.dartTrimmed,
            branch: summary.branch,
            lastCommitSubject: summary.lastCommitSubject,
            lastCommitAt: summary.lastCommitAt)
        records = LocalRepoStore.sortedByRecency([record] + records)
        try persist()
        notifyListeners()
        return record
    }

    private func cleanupFailed(_ directory: URL) {
        // Best effort: a half-created folder is excluded from the index anyway.
        if EngineFS.entityType(directory.path, followLinks: false) == .directory {
            try? FileManager.default.removeItem(at: directory)
        }
    }
}

/// UTC ISO 8601 for a millisecond-precision instant:
/// `yyyy-MM-ddTHH:mm:ss.mmmZ`.
enum DartISO {
    static func utcString(_ date: Date) -> String {
        let ms = date.epochMillis
        var seconds = ms / 1000
        var millis = ms % 1000
        if millis < 0 {
            millis += 1000
            seconds -= 1
        }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        let parts = calendar.dateComponents(
            [.year, .month, .day, .hour, .minute, .second], from: Date(timeIntervalSince1970: TimeInterval(seconds)))
        func pad(_ value: Int?, _ width: Int) -> String {
            let text = String(abs(value ?? 0))
            return String(repeating: "0", count: max(0, width - text.count)) + text
        }
        let year = parts.year ?? 1970
        let yearText = (0...9999).contains(year) ? pad(year, 4) : (year < 0 ? "-" : "+") + pad(year, 6)
        return "\(yearText)-\(pad(parts.month, 2))-\(pad(parts.day, 2))T\(pad(parts.hour, 2)):\(pad(parts.minute, 2)):\(pad(parts.second, 2)).\(pad(Int(millis), 3))Z"
    }
}
