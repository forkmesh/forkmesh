// The GitWorkspace facade; the worker-side operations are in
// GitWorkspaceOperations.swift.
//
// Every engine call runs off the main actor through `GitOperationRunner`;
// operations are serialized per workspace, and `cancel()` stops the current
// one.

import FMAccount
import FMBase
import FMGit
import Foundation

/// Per-repository facade over the owned Git engine.
///
/// Share one workspace between every surface that works on the same
/// repository (the repository screen and the coding agent do): operations
/// serialize on its runner, so they cannot race the worktree.
public class GitWorkspace: @unchecked Sendable {
    /// The worktree directory.
    public let worktree: URL
    let runner: GitOperationRunner

    /// Builds the inner HTTP transport for a remote operation; nil uses the
    /// engine's `GitNetHttpTransport` over `URLSessionHTTPClient`. Tests
    /// substitute a fake smart-HTTP server here.
    var transportFactory: (@Sendable () -> GitHttpTransport)?

    /// Hard bound on commits returned by `log`.
    public static let maxLogEntries = 500

    /// Largest pack this device will accept from a clone.
    public static let maxCloneBytes = GitResourceLimits.hardMaxCompressedPackBytes

    public static let meshCloneDepth = 1

    public static let cloneDeadline: TimeInterval = 20 * 60

    public static func deviceLimits(maxPackBytes: Int = maxCloneBytes) throws -> GitResourceLimits {
        try GitResourceLimits(
            maxObjectBytes: GitResourceLimits.hardMaxObjectBytes,
            maxInflatedBytes: GitResourceLimits.hardMaxInflatedBytes,
            maxCompressedPackBytes: maxPackBytes,
            connectTimeout: GitResourceLimits.hardConnectTimeout,
            idleReadTimeout: GitResourceLimits.hardIdleReadTimeout,
            operationDeadline: cloneDeadline)
    }

    public init(worktree: URL, performanceMonitor: PerformanceMonitorService? = nil, runner: GitOperationRunner? = nil) {
        self.worktree = worktree
        _ = GitNetHttpTransport.registeredAsDefault
        if let runner {
            self.runner = runner
        } else {
            let repoName = worktree.pathComponents.last(where: { !$0.isEmpty && $0 != "/" }) ?? ""
            self.runner = GitOperationRunner(onDuration: { name, elapsed, failed in
                guard let monitor = performanceMonitor else { return }
                var details: JSONObject = ["repo": .string(repoName)]
                if failed { details["failed"] = .bool(true) }
                monitor.recordSlowEvent(kind: .operation, name: "git.\(name)", duration: elapsed, details: details)
            })
        }
    }

    /// Absolute worktree path.
    public var path: String { worktree.path }

    /// Whether an operation is in flight.
    public var isBusy: Bool { runner.isBusy }

    /// Abort the in-flight operation (it fails with `cancelled`).
    public func cancel() { runner.cancel() }

    /// Cancel any in-flight work and release the worker.
    public func dispose() { runner.dispose() }

    // MARK: - Local operations

    /// `git init` plus optional seed `files` and an initial commit when
    /// `commitMessage` is non-nil.
    @discardableResult
    public func initialize(
        files: [String: String] = [:],
        commitMessage: String? = nil,
        author: GitAuthor? = nil,
        branch: String? = nil
    ) async throws -> String? {
        let path = self.path
        let now = Date()
        return try await run("init") { _ in
            try WorkspaceOps.initialize(path, files: files, message: commitMessage, author: author, now: now, branch: branch)
        }
    }

    /// Copy `source` into the worktree, then init + stage everything + commit.
    public func importFromDirectory(
        _ source: URL, author: GitAuthor, message: String = "Import", branch: String? = nil
    ) async throws -> LocalImportResult {
        let path = self.path
        let now = Date()
        return try await run("import-directory") { token in
            try WorkspaceOps.importDirectory(
                path, source: source.path, author: author, message: message, now: now, token: token, branch: branch)
        }
    }

    /// Extract a zip archive at `zipPath` into the worktree, then init + stage
    /// everything + commit. A single shared top-level folder is stripped.
    public func importFromZip(
        _ zipPath: String, author: GitAuthor, message: String = "Import", branch: String? = nil
    ) async throws -> LocalImportResult {
        let path = self.path
        let now = Date()
        return try await run("import-zip") { token in
            try WorkspaceOps.importZip(
                path, zipPath: zipPath, author: author, message: message, now: now, token: token, branch: branch)
        }
    }

    /// List the worktree at `directory` ("" for the root), directories first.
    public func listFiles(_ directory: String = "") async throws -> [LocalFileEntry] {
        let path = self.path
        return try await run("list-files") { _ in try WorkspaceOps.listFiles(path, directory) }
    }

    public func readFile(_ file: String) async throws -> LocalFileContent {
        let path = self.path
        return try await run("read-file") { _ in try WorkspaceOps.readFile(path, file) }
    }

    public func writeFile(_ file: String, _ text: String) async throws {
        let path = self.path
        try await run("write-file") { _ in try WorkspaceOps.writeFile(path, file, text) }
    }

    public func deleteFile(_ file: String) async throws {
        let path = self.path
        try await run("delete-file") { _ in try WorkspaceOps.deleteFile(path, file) }
    }

    public func status() async throws -> LocalStatus {
        let path = self.path
        return try await run("status") { token in try WorkspaceOps.status(path, token: token) }
    }

    public func stage(_ paths: [String]) async throws {
        let path = self.path
        try await run("stage") { _ in try WorkspaceOps.stage(path, paths) }
    }

    public func stageAll() async throws {
        let path = self.path
        try await run("stage-all") { token in try WorkspaceOps.stageAll(path, token: token) }
    }

    public func unstage(_ paths: [String]) async throws {
        let path = self.path
        try await run("unstage") { token in try WorkspaceOps.unstage(path, Set(paths), token: token) }
    }

    public func unstageAll() async throws {
        let path = self.path
        try await run("unstage-all") { token in try WorkspaceOps.unstageAll(path, token: token) }
    }

    /// Restore worktree `paths` from the index (drops unstaged edits; removes
    /// untracked files).
    public func discard(_ paths: [String]) async throws {
        let path = self.path
        try await run("discard") { _ in try WorkspaceOps.discard(path, paths) }
    }

    /// Commit the index. Returns the new commit id (hex).
    public func commit(message: String, author: GitAuthor) async throws -> String {
        let path = self.path
        let now = Date()
        return try await run("commit") { token in
            try WorkspaceOps.commit(path, message: message, author: author, now: now, token: token)
        }
    }

    public func branches() async throws -> [LocalBranch] {
        let path = self.path
        return try await run("branches") { _ in try WorkspaceOps.branches(try WorkspaceOps.open(path)) }
    }

    /// Create `name` at HEAD; optionally check it out. Switching to the new
    /// branch keeps local edits because it points at the same commit.
    public func createBranch(_ name: String, checkout: Bool = false) async throws {
        let path = self.path
        try await run("create-branch") { token in
            try WorkspaceOps.createBranch(path, name, checkout: checkout, token: token)
        }
    }

    public func checkout(_ name: String) async throws {
        let path = self.path
        try await run("checkout") { token in try WorkspaceOps.checkout(path, name, token: token) }
    }

    /// Walk history from HEAD, newest first, bounded by `limit`.
    public func log(limit: Int = 200) async throws -> [LocalCommit] {
        let path = self.path
        let bounded = min(max(limit, 1), GitWorkspace.maxLogEntries)
        return try await run("log") { token in try WorkspaceOps.log(path, limit: bounded, token: token) }
    }

    public func summary() async throws -> LocalRepoSummary {
        let path = self.path
        return try await run("summary") { _ in try WorkspaceOps.summary(path) }
    }

    // MARK: - Network

    /// Point `remote` at the mesh endpoint and push `refs/heads/<branch>`.
    ///
    /// Never forces and never deletes: `GitPushRefspec` cannot express either,
    /// and a non-fast-forward is refused by the engine before bytes are sent.
    public func push(
        credentials: ForkMeshRemoteCredentials,
        branch: String,
        remote: String = "origin",
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalPushResult {
        let path = self.path
        let factory = transportFactory
        return try await runRemote("push", push: true, host: forkMeshRemoteHost(credentials.origin), onProgress: onProgress) { token, progress in
            try await WorkspaceOps.push(
                path, credentials: credentials, remote: remote, branch: branch,
                inner: factory?(), token: token, progress: progress)
        }
    }

    /// Fetch `refs/heads/*` into `refs/remotes/<remote>/*`. Local branches and
    /// the worktree are never touched.
    public func fetch(
        credentials: ForkMeshRemoteCredentials,
        remote: String = "origin",
        route: GitRemoteRoute? = nil,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalFetchResult {
        let path = self.path
        let factory = transportFactory
        return try await runRemote("fetch", host: credentials.host, onProgress: onProgress) { token, progress in
            try await WorkspaceOps.fetch(
                path, credentials: credentials, remote: remote, route: route,
                inner: factory?(), token: token, progress: progress)
        }
    }

    public func pull(
        credentials: ForkMeshRemoteCredentials,
        branch: String,
        remote: String = "origin",
        route: GitRemoteRoute? = nil,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalPullResult {
        let path = self.path
        let factory = transportFactory
        return try await runRemote("pull", host: credentials.host, onProgress: onProgress) { token, progress in
            try await WorkspaceOps.pull(
                path, credentials: credentials, remote: remote, branch: branch, route: route,
                inner: factory?(), token: token, progress: progress)
        }
    }

    /// Clone the mesh repository named by `credentials` into this worktree.
    ///
    /// `maxPackBytes` is the mobile size guard: a repository whose pack
    /// exceeds it fails with `resourceLimitExceeded` rather than filling the
    /// device.
    public func clone(
        credentials: ForkMeshRemoteCredentials,
        branch: String? = nil,
        depth: Int? = nil,
        maxPackBytes: Int = GitWorkspace.maxCloneBytes,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalCloneResult {
        let path = self.path
        let factory = transportFactory
        return try await runRemote("clone", host: credentials.host, onProgress: onProgress) { token, progress in
            try await WorkspaceOps.clone(
                path, credentials: credentials, branch: branch, depth: depth, maxPackBytes: maxPackBytes,
                inner: factory?(), token: token, progress: progress)
        }
    }

    /// Compare `refs/heads/<branch>` with `refs/remotes/<remote>/<branch>`
    /// using bounded history walks (no network).
    public func syncState(branch: String, remote: String = "origin") async throws -> LocalSyncState {
        let path = self.path
        return try await run("sync-state") { token in
            try WorkspaceOps.syncState(path, remote: remote, branch: branch, token: token)
        }
    }

    /// Run `body` against this worktree's engine repository on the
    /// workspace's runner (serialized with every other operation, off the
    /// main actor). For FMCode's own services (pull request preparation).
    func withRepository<T: Sendable>(
        _ name: String,
        _ body: @escaping @Sendable (GitRepository, GitCancellationToken) async throws -> T
    ) async throws -> T {
        let path = self.path
        do {
            return try await runner.run(name) { token in try await body(try WorkspaceOps.open(path), token) }
        } catch let error as GitException {
            throw GitWorkspaceException(GitWorkspace.describe(error), code: error.code, detail: error.message)
        } catch let error as GitWorkerException {
            throw GitWorkspaceException(error.message, detail: error.detail)
        }
    }

    // MARK: - Running

    private func runRemote<T: Sendable>(
        _ name: String,
        push: Bool = false,
        host: String = "",
        onProgress: (@Sendable (GitTransferProgress) -> Void)?,
        _ body: @escaping @Sendable (GitCancellationToken, GitProgressListener?) async throws -> T
    ) async throws -> T {
        let relay = ProgressRelay(onProgress)
        defer { relay.close() }
        do {
            return try await runner.run(name) { token in
                try await body(token, relay.listener)
            }
        } catch let error as GitException {
            let failure = describeRemoteFailure(error, push: push, host: host)
            throw GitWorkspaceException(failure.message, code: error.code, detail: failure.code)
        } catch let error as GitWorkerException {
            throw GitWorkspaceException(error.message, detail: error.detail)
        }
    }

    private func run<T: Sendable>(
        _ name: String,
        _ body: @escaping @Sendable (GitCancellationToken) throws -> T
    ) async throws -> T {
        do {
            return try await runner.run(name) { token in try body(token) }
        } catch let error as GitException {
            throw GitWorkspaceException(GitWorkspace.describe(error), code: error.code, detail: error.message)
        } catch let error as GitWorkerException {
            throw GitWorkspaceException(error.message, detail: error.detail)
        }
    }

    /// Map an engine or worker failure to a short user-facing sentence.
    public static func describe(_ error: Error) -> String {
        if let error = error as? GitWorkspaceException { return error.message }
        if let error = error as? GitException {
            switch error.code {
            case .cancelled:
                return "Operation cancelled."
            case .resourceLimitExceeded:
                return "This content exceeds the on-device Git limits."
            case .pathEscapesWorktree, .symlinkEscape:
                return "A path was rejected because it is not safe or portable."
            case .refLocked:
                return "The repository is busy with another operation. Try again."
            case .nonFastForward:
                return "The repository changed while committing. Try again."
            case .operationInterrupted:
                return error.message.contains("local changes")
                    ? "Commit or discard your changes before switching branches."
                    : "A previous operation was interrupted. Reopen the repository."
            case .invalidRef:
                if error.message.contains("does not exist") { return "That branch does not exist." }
                if error.message.contains("unborn") { return "Make a first commit before creating branches." }
                return "That branch name is not valid."
            case .invalidRepository:
                return error.message.contains("already exists")
                    ? "A repository already exists at this location."
                    : "This is not a usable Git repository."
            case .unsupportedRepositoryFormat:
                return "This repository uses a format the mobile engine does not support."
            case .indexCorrupt, .packCorrupt, .packChecksumMismatch, .objectHashMismatch,
                 .invalidObject, .invalidTree, .deltaDepthExceeded:
                return "Repository data is corrupt (\(error.code.wireName))."
            case .sha1CollisionDetected, .hashScreeningUnavailable, .hashImplementationFailure, .quarantineRejected:
                return "Object hashing failed (\(error.code.wireName))."
            case .networkUnavailable, .tlsRejected, .authenticationFailed, .protocolUnsupported, .remoteRejected:
                return "Network operation failed (\(error.code.wireName))."
            }
        }
        if let error = error as? GitWorkerException { return error.message }
        return String(describing: error)
    }
}

/// Delivers engine progress to the caller in order, and drops whatever is
/// still queued once the operation has finished.
private final class ProgressRelay: @unchecked Sendable {
    private let lock = NSLock()
    private var handler: (@Sendable (GitTransferProgress) -> Void)?

    init(_ handler: (@Sendable (GitTransferProgress) -> Void)?) {
        self.handler = handler
    }

    var listener: GitProgressListener? {
        guard currentHandler() != nil else { return nil }
        return { [self] sample in
            guard let handler = currentHandler() else { return }
            handler(GitTransferProgress(
                phase: sample.phase.rawValue, bytes: sample.bytes, objects: sample.objects, refs: sample.refs,
                total: sample.total))
        }
    }

    private func currentHandler() -> (@Sendable (GitTransferProgress) -> Void)? {
        lock.lock()
        defer { lock.unlock() }
        return handler
    }

    func close() {
        lock.lock()
        handler = nil
        lock.unlock()
    }
}
