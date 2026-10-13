// The GitRepository class; the request types are in GitRepositoryTypes.swift
// and the remote half in RemoteOperations.swift.
//
// Local operations are synchronous (blocking file I/O, as in the Kotlin
// port); fetch, push and clone are `async`. Not thread-safe: one caller at a
// time.

import Foundation

public final class GitRepository: @unchecked Sendable {
    public let worktree: String
    public let gitDirectory: String
    public let limits: GitResourceLimits
    public let detector: GitSha1CollisionDetector?

    private let objects: LooseObjectStore
    private let index: GitIndexStore
    public let packedObjects: GitPackObjectStore

    private init(_ worktree: String, _ gitDirectory: String, _ limits: GitResourceLimits, _ detector: GitSha1CollisionDetector?) {
        self.worktree = worktree
        self.gitDirectory = gitDirectory
        self.limits = limits
        self.detector = detector
        objects = LooseObjectStore(gitDirectory, limits, detector: detector)
        index = GitIndexStore(gitDirectory, limits)
        packedObjects = GitPackObjectStore(gitDirectory, limits, detector: detector)
    }

    // MARK: - Init and open

    public static func initialize(_ request: GitRepositoryInit) throws -> GitRepository {
        try EngineFS.createDirectories(request.worktree)
        let gitDirectory = EngineFS.join(request.worktree, ".git")
        if EngineFS.directoryExists(gitDirectory) {
            throw GitException(.invalidRepository, "repository already exists")
        }
        try EngineFS.createDirectories(EngineFS.join(gitDirectory, "objects"))
        try EngineFS.createDirectories(EngineFS.join(EngineFS.join(gitDirectory, "refs"), "heads"))
        try EngineFS.createDirectories(EngineFS.join(EngineFS.join(gitDirectory, "refs"), "tags"))
        try EngineFS.createDirectories(EngineFS.join(EngineFS.join(gitDirectory, "refs"), "remotes"))
        let initialBranch: String = request.initialBranch?.value ?? "main"
        try EngineFS.writeStringFlushed(EngineFS.join(gitDirectory, "HEAD"), "ref: refs/heads/\(initialBranch)\n")
        try EngineFS.writeStringFlushed(
            EngineFS.join(gitDirectory, "config"),
            "[core]\n\trepositoryformatversion = 0\n\tbare = false\n"
        )
        return try open(GitRepositoryOpen(request.worktree, limits: request.limits, detector: request.detector))
    }

    public static func open(_ request: GitRepositoryOpen) throws -> GitRepository {
        let gitDirectory = EngineFS.join(request.worktree, ".git")
        if !EngineFS.directoryExists(gitDirectory) {
            throw GitException(.invalidRepository, "missing .git directory")
        }
        let config = EngineFS.join(gitDirectory, "config")
        if !EngineFS.fileExists(config) {
            throw GitException(.invalidRepository, "missing config")
        }
        _ = try RepositoryConfig.load(config)
        try GitOperationJournal.recover(gitDirectory)
        try GitObjectQuarantine.recover(gitDirectory)
        _ = try recoverStaleLocks(in: gitDirectory)
        let head = EngineFS.join(gitDirectory, "HEAD")
        if !EngineFS.fileExists(head) {
            throw GitException(.invalidRepository, "HEAD is invalid")
        }
        let headValue = try EngineFS.readString(head).engTrim()
        do {
            if headValue.engHasPrefix("ref: ") {
                _ = try GitRefName.branch(headValue.engSubstring(5))
            } else {
                _ = try GitObjectId.parseSha1(headValue)
            }
        } catch is GitException {
            throw GitException(.invalidRepository, "HEAD is invalid")
        }
        return GitRepository(request.worktree, gitDirectory, request.limits ?? GitResourceLimits.standard, request.detector)
    }

    // MARK: - Stale locks

    /// Removes leftover `.lock` files older than `olderThan` that a killed
    /// process could not clean up, so they stop surfacing as `refLocked`.
    ///
    /// Only known engine lock locations are touched: `HEAD.lock`,
    /// `index.lock`, `config.lock`, `FETCH_HEAD.lock`, `packed-refs.lock`,
    /// and `*.lock` under `refs/`. Locks younger than `olderThan` are left
    /// alone because a live operation may still hold them. Runs automatically
    /// on `open`; returns the number of locks removed.
    public func recoverStaleLocks(olderThan: TimeInterval = 15 * 60) throws -> Int {
        try Self.recoverStaleLocks(in: gitDirectory, olderThan: olderThan)
    }

    private static func recoverStaleLocks(in gitDirectory: String, olderThan: TimeInterval = 15 * 60) throws -> Int {
        if olderThan < 60 {
            throw GitArgumentError("olderThan", "must be at least one minute")
        }
        let cutoff = Date().addingTimeInterval(-olderThan)
        var removed = 0
        func removeIfStale(_ lock: String) {
            guard EngineFS.entityType(lock) == .file, let modified = EngineFS.modified(lock),
                  modified < cutoff
            else { return }
            do {
                try EngineFS.deleteFile(lock)
                removed += 1
            } catch {
                // Another process may have completed or removed it concurrently.
            }
        }
        for name in ["HEAD.lock", "index.lock", "config.lock", "FETCH_HEAD.lock", "packed-refs.lock"] {
            removeIfStale(EngineFS.join(gitDirectory, name))
        }
        let refs = EngineFS.join(gitDirectory, "refs")
        if EngineFS.directoryExists(refs) {
            var visited = 0
            try EngineFS.walk(refs) { path, type in
                visited += 1
                if visited > GitResourceLimits.hardMaxRefCount {
                    throw GitException(.resourceLimitExceeded, "refs directory exceeds the configured limit")
                }
                if type == .file && path.engHasSuffix(".lock") { removeIfStale(path) }
            }
        }
        return removed
    }

    // MARK: - Refs and history

    /// Lists local branches (`refs/heads/*`) from loose and packed refs, sorted by name.
    public func listBranches() throws -> [GitRefEntry] { try listRefs("refs/heads/") }

    /// Lists tags (`refs/tags/*`) from loose and packed refs, sorted by name.
    public func listTags() throws -> [GitRefEntry] { try listRefs("refs/tags/") }

    private func listRefs(_ prefix: String) throws -> [GitRefEntry] {
        let refs = try RefStore(gitDirectory).listDirect(prefix, limits)
        let names = refs.keys.sorted { $0.engCompare($1) < 0 }
        return try names.map { GitRefEntry(try GitRefName.parse($0), refs[$0]!) }
    }

    /// Walks history newest-first (committer date order) from `request.ref`
    /// or `HEAD`, returning at most `request.limit` commits.
    ///
    /// An unborn `HEAD` yields an empty list. The walk is bounded by
    /// `limits.maxCommitWalk` in addition to the request limit.
    public func log(_ request: GitLogRequest, cancellation: GitCancellationToken? = nil) throws -> [GitLogEntry] {
        let start: GitObjectId?
        if let ref = request.ref {
            start = try RefStore(gitDirectory).readDirect(try GitRefName.parse(ref))
            if start == nil {
                throw GitException(.invalidRef, "ref does not exist")
            }
        } else {
            start = try readHeadObjectId()
        }
        guard let start else { return [] }
        let shallow = try shallowCommits()
        var seen = Set<GitObjectId>()
        var pending = [GitLogEntry]()
        var output = [GitLogEntry]()

        func commitTime(_ commit: GitCommit) -> Int {
            commit.committer?.timestampSeconds ?? commit.author?.timestampSeconds ?? 0
        }

        func enqueue(_ id: GitObjectId) throws {
            try cancellation?.throwIfCancelled()
            if !seen.insert(id).inserted { return }
            if seen.count > limits.maxCommitWalk {
                throw GitException(.resourceLimitExceeded, "log walk exceeded the configured limit")
            }
            guard let commit = try readCommitOrTagTarget(id) else {
                throw GitException(.invalidObject, "log start is not a commit")
            }
            pending.append(GitLogEntry(id, commit))
        }

        try enqueue(start)
        while !pending.isEmpty && output.count < request.limit {
            try cancellation?.throwIfCancelled()
            // Newest committer date first, matching `git log --date-order`.
            pending = stableSorted(pending) { commitTime($1.commit) < commitTime($0.commit) }
            let next = pending.removeFirst()
            output.append(next)
            for parent in walkParents(next.id, next.commit, shallow) { try enqueue(parent) }
        }
        return output
    }

    /// Restores the index entries for `paths` to their `HEAD` state (removing
    /// entries absent at `HEAD`), leaving the worktree untouched: the
    /// `git reset -- <path>` unstage operation.
    public func resetPaths(_ paths: [GitWorktreePath], cancellation: GitCancellationToken? = nil) throws {
        let journal = try GitOperationJournal.begin(gitDirectory, "reset")
        defer { try? journal.complete() }
        let headEntries = try self.headEntries()
        var entries = try indexEntriesByPath()
        for path in paths {
            try cancellation?.throwIfCancelled()
            if let headEntry = headEntries[path.value] {
                entries[path.value] = headEntry
            } else {
                entries[path.value] = nil
            }
        }
        try index.write(GitIndexV2(Array(entries.values)))
    }

    // MARK: - Objects

    @discardableResult
    public func writeBlob(_ bytes: [UInt8]) throws -> GitObjectId { try objects.write(.blob, bytes) }

    public func readBlob(_ id: GitObjectId) throws -> [UInt8] {
        let object = try readObject(id)
        if object.type != .blob {
            throw GitException(.invalidObject, "expected blob object")
        }
        return object.body
    }

    public func readHead() throws -> GitSymbolicRef {
        let value = try EngineFS.readString(headFile).engTrim()
        if !value.engHasPrefix("ref: ") {
            throw GitException(.invalidRef, "HEAD is detached")
        }
        let target = value.engSubstring(5)
        _ = try GitRefName.branch(target)
        return GitSymbolicRef(target)
    }

    public func readHeadObjectId() throws -> GitObjectId? {
        switch try headTarget() {
        case .direct(let id): return id
        case .ref(let ref): return try RefStore(gitDirectory).readDirect(ref)
        }
    }

    public func readTree(_ id: GitObjectId) throws -> GitTree {
        let object = try readObject(id)
        if object.type != .tree {
            throw GitException(.invalidObject, "expected tree object")
        }
        return try GitTree.parse(object.body)
    }

    public func readCommit(_ id: GitObjectId) throws -> GitCommit {
        let object = try readObject(id)
        if object.type != .commit {
            throw GitException(.invalidObject, "expected commit object")
        }
        return try GitCommit.parse(object.body)
    }

    public func readObject(_ id: GitObjectId) throws -> GitObjectFrame {
        do {
            return try objects.read(id)
        } catch let error as GitException {
            if error.code != .invalidObject { throw error }
            return try packedObjects.read(id)
        }
    }

    /// Writes a non-delta pack of every object reachable from `roots`.
    ///
    /// Objects in `exclude` (and anything only reachable through them) are
    /// omitted, which is how push sends only what the remote lacks.
    public func writePack(
        _ roots: [GitObjectId],
        exclude: Set<GitObjectId> = [],
        cancellation: GitCancellationToken? = nil
    ) throws -> GitWrittenPack {
        var visited = exclude
        var output = [GitPackWriteObject]()
        let shallow = try shallowCommits()
        for root in roots {
            try collectPackObject(root, shallow, &visited, &output, cancellation)
        }
        return try GitPackWriter.write(output, cancellation: cancellation)
    }

    /// A pre-order walk, with an explicit stack of child
    /// lists so a long history cannot overflow the call stack.
    private func collectPackObject(
        _ root: GitObjectId,
        _ shallow: Set<GitObjectId>,
        _ visited: inout Set<GitObjectId>,
        _ output: inout [GitPackWriteObject],
        _ cancellation: GitCancellationToken?
    ) throws {
        var stack = [(children: [GitObjectId], next: Int)]()
        if let children = try visitPackObject(root, shallow, &visited, &output, cancellation) {
            stack.append((children, 0))
        }
        while !stack.isEmpty {
            let top = stack.count - 1
            if stack[top].next >= stack[top].children.count {
                stack.removeLast()
                continue
            }
            let child = stack[top].children[stack[top].next]
            stack[top].next += 1
            if let children = try visitPackObject(child, shallow, &visited, &output, cancellation) {
                stack.append((children, 0))
            }
        }
    }

    private func visitPackObject(
        _ id: GitObjectId,
        _ shallow: Set<GitObjectId>,
        _ visited: inout Set<GitObjectId>,
        _ output: inout [GitPackWriteObject],
        _ cancellation: GitCancellationToken?
    ) throws -> [GitObjectId]? {
        try cancellation?.throwIfCancelled()
        if !visited.insert(id).inserted { return nil }
        if output.count >= limits.maxCommitWalk * 8 {
            throw GitException(.resourceLimitExceeded, "pack object walk exceeded the configured limit")
        }
        let object = try readObject(id)
        output.append(try GitPackWriteObject(object.type, object.body))
        switch object.type {
        case .blob:
            return nil
        case .tree:
            return try GitTree.parse(object.body).entries.map(\.objectId)
        case .commit:
            let commit = try GitCommit.parse(object.body)
            return [commit.tree] + walkParents(id, commit, shallow)
        case .tag:
            return [try GitAnnotatedTag.parse(object.body).target]
        }
    }

    /// True when `id` exists as a loose or packed object.
    public func hasObject(_ id: GitObjectId) throws -> Bool {
        if EngineFS.fileExists(objects.fileFor(id)) { return true }
        return try packedObjects.contains(id)
    }

    /// Reads `id` as a commit, following annotated tags to their commit.
    /// Returns nil for trees and blobs.
    public func readCommitOrTagTarget(_ id: GitObjectId) throws -> GitCommit? {
        var current = id
        for _ in 0..<8 {
            let object = try readObject(current)
            switch object.type {
            case .commit:
                return try GitCommit.parse(object.body)
            case .tag:
                current = try GitAnnotatedTag.parse(object.body).target
            case .tree, .blob:
                return nil
            }
        }
        throw GitException(.invalidObject, "tag chain is too deep")
    }

    /// True when `ancestor` is reachable from `descendant` through parent
    /// links (or equal to it), walking at most `limits.maxCommitWalk` commits.
    public func isAncestor(_ ancestor: GitObjectId, _ descendant: GitObjectId, _ cancellation: GitCancellationToken?) throws -> Bool {
        if ancestor == descendant { return true }
        let shallow = try shallowCommits()
        var visited = Set<GitObjectId>()
        var queue = [descendant]
        while let id = queue.popLast() {
            try cancellation?.throwIfCancelled()
            if !visited.insert(id).inserted { continue }
            if visited.count > limits.maxCommitWalk {
                throw GitException(.resourceLimitExceeded, "ancestry walk exceeded the configured limit")
            }
            guard let commit = try readCommitOrTagTarget(id) else { continue }
            for parent in walkParents(id, commit, shallow) {
                if parent == ancestor { return true }
                queue.append(parent)
            }
        }
        return false
    }

    /// Every commit reachable from `tips`, bounded by `limits.maxCommitWalk`.
    public func reachableCommits(_ tips: [GitObjectId], _ cancellation: GitCancellationToken?) throws -> Set<GitObjectId> {
        let shallow = try shallowCommits()
        var visited = Set<GitObjectId>()
        var queue = tips
        while let id = queue.popLast() {
            try cancellation?.throwIfCancelled()
            if !visited.insert(id).inserted { continue }
            if visited.count > limits.maxCommitWalk {
                throw GitException(.resourceLimitExceeded, "reachability walk exceeded the configured limit")
            }
            guard let commit = try readCommitOrTagTarget(id) else { continue }
            queue.append(contentsOf: walkParents(id, commit, shallow))
        }
        return visited
    }

    /// Adds `tree` and every tree and blob below it to `output`.
    public func collectTreeClosure(_ tree: GitObjectId, _ output: inout Set<GitObjectId>, _ cancellation: GitCancellationToken?) throws {
        var stack = [tree]
        while let id = stack.popLast() {
            try cancellation?.throwIfCancelled()
            if !output.insert(id).inserted { continue }
            if output.count > limits.maxCommitWalk * 8 {
                throw GitException(.resourceLimitExceeded, "tree walk exceeded the configured limit")
            }
            let object = try readObject(id)
            if object.type != .tree { continue }
            for entry in try GitTree.parse(object.body).entries { stack.append(entry.objectId) }
        }
    }

    // MARK: - Config

    private var configFile: String { EngineFS.join(gitDirectory, "config") }

    private var headFile: String { EngineFS.join(gitDirectory, "HEAD") }

    /// Reads the validated repository configuration.
    public func readConfig() throws -> RepositoryConfig { try RepositoryConfig.load(configFile) }

    /// Records `remote.<name>.url` and `remote.<name>.fetch`.
    public func setRemote(_ name: String, _ url: GitRemoteUrl, fetch: GitFetchRefspec? = nil) throws {
        try RepositoryConfig.writeRemote(configFile, name, url: url, fetch: try fetch ?? GitFetchRefspec.standard(name))
    }

    /// Records `branch.<name>.remote` and `branch.<name>.merge`.
    public func setBranchUpstream(_ branch: GitBranchName, remote: String, merge: GitRefName) throws {
        try RepositoryConfig.writeBranchUpstream(configFile, branch.value, remote: remote, merge: merge)
    }

    // MARK: - Index and worktree

    public func add(_ paths: [GitWorktreePath], cancellation: GitCancellationToken? = nil) throws {
        let journal = try GitOperationJournal.begin(gitDirectory, "add")
        defer { try? journal.complete() }
        var entries = try indexEntriesByPath()
        for path in paths {
            try cancellation?.throwIfCancelled()
            if path.components[0].engSame(".git") {
                throw GitException(.pathEscapesWorktree, "cannot stage repository metadata")
            }
            try stagePath(path, &entries, cancellation)
        }
        try cancellation?.throwIfCancelled()
        try index.write(GitIndexV2(Array(entries.values)))
    }

    public func remove(_ paths: [GitWorktreePath]) throws {
        let journal = try GitOperationJournal.begin(gitDirectory, "remove")
        defer { try? journal.complete() }
        var entries = try indexEntriesByPath()
        for path in paths { entries[path.value] = nil }
        try index.write(GitIndexV2(Array(entries.values)))
    }

    public func status(cancellation: GitCancellationToken? = nil) throws -> GitStatus {
        let indexEntries = try indexEntriesByPath()
        let headEntries = try self.headEntries()
        var staged = [GitStatusEntry]()
        let allPaths = Set(indexEntries.keys).union(headEntries.keys).sorted { $0.engCompare($1) < 0 }
        for path in allPaths {
            try cancellation?.throwIfCancelled()
            let indexEntry = indexEntries[path]
            let headEntry = headEntries[path]
            if indexEntry == nil {
                staged.append(GitStatusEntry(try GitWorktreePath(path), .deleted))
            } else if headEntry == nil {
                staged.append(GitStatusEntry(try GitWorktreePath(path), .added))
            } else if let indexEntry, let headEntry,
                      indexEntry.objectId != headEntry.objectId || indexEntry.mode != headEntry.mode
            {
                staged.append(GitStatusEntry(try GitWorktreePath(path), .modified))
            }
        }
        var worktreeChanges = [GitStatusEntry]()
        for entry in indexEntries.values {
            try cancellation?.throwIfCancelled()
            if let change = try worktreeChange(entry) {
                worktreeChanges.append(GitStatusEntry(try GitWorktreePath(engUtf8Decode(entry.path)), change))
            }
        }
        try collectUntracked(worktree, [], indexEntries, &worktreeChanges, cancellation)
        worktreeChanges = stableSorted(worktreeChanges) { $0.path.value.engCompare($1.path.value) < 0 }
        return GitStatus(staged: staged, worktree: worktreeChanges)
    }

    public func commit(_ request: GitCommitRequest, cancellation: GitCancellationToken? = nil) throws -> GitObjectId {
        let journal = try GitOperationJournal.begin(gitDirectory, "commit")
        defer { try? journal.complete() }
        let current = try index.read()
        let tree = try writeTree(current.entries, cancellation)
        try cancellation?.throwIfCancelled()
        let parent = try readHeadObjectId()
        let body = encodeCommit(request, tree, parent)
        let commit = try objects.write(.commit, body)
        guard case .ref(let headRef) = try headTarget() else {
            throw GitException(.unsupportedRepositoryFormat, "committing from detached HEAD is unsupported")
        }
        let result = try RefStore(gitDirectory).update(headRef, expectedOld: parent, next: commit)
        if result != .updated {
            throw GitException(.nonFastForward, "HEAD changed while creating commit")
        }
        return commit
    }

    public func createBranch(_ branch: GitBranchName) throws {
        guard let head = try readHeadObjectId() else {
            throw GitException(.invalidRef, "cannot create a branch from an unborn HEAD")
        }
        let result = try RefStore(gitDirectory).update(try branch.ref(), expectedOld: nil, next: head)
        if result != .updated {
            throw GitException(.refLocked, "branch already exists")
        }
    }

    /// Materializes `branch` into an empty worktree after a clone fetch.
    ///
    /// Unlike `checkout`, there is no previous state to preserve: the index
    /// is empty and the worktree contains nothing but `.git`.
    public func initialCheckout(
        _ branch: GitBranchName,
        _ target: GitObjectId,
        cancellation: GitCancellationToken? = nil,
        progress: ((Int, Int) -> Void)? = nil
    ) throws {
        let journal = try GitOperationJournal.begin(gitDirectory, "checkout")
        defer { try? journal.complete() }
        let targetEntries = try entriesForCommit(target)
        try materializeCheckout([:], targetEntries, cancellation, progress: progress)
        try index.write(GitIndexV2(Array(targetEntries.values)))
        try writeHead("ref: \(try branch.ref().value)\n")
    }

    public func checkout(_ request: GitCheckoutRequest, cancellation: GitCancellationToken? = nil) throws {
        let journal = try GitOperationJournal.begin(gitDirectory, "checkout")
        defer { try? journal.complete() }
        let branchRef = try request.branch.ref()
        guard let target = try RefStore(gitDirectory).readDirect(branchRef) else {
            throw GitException(.invalidRef, "branch does not exist")
        }
        // Switching to a branch that points at the current commit only moves
        // the HEAD symref: no file changes, so local edits may stay in place
        // (`git checkout -b` / `git switch` semantics).
        if try readHeadObjectId() == target {
            try writeHead("ref: \(branchRef.value)\n")
            return
        }
        let before = try status(cancellation: cancellation)
        if !before.isClean {
            throw GitException(.operationInterrupted, "cannot checkout with local changes")
        }
        let currentEntries = try headEntries()
        let targetEntries = try entriesForCommit(target)
        try materializeCheckout(currentEntries, targetEntries, cancellation, progress: nil)
        try cancellation?.throwIfCancelled()
        try index.write(GitIndexV2(Array(targetEntries.values)))
        try writeHead("ref: \(branchRef.value)\n")
    }

    public func fastForward(
        _ branch: GitBranchName,
        to target: GitObjectId,
        cancellation: GitCancellationToken? = nil,
        progress: ((Int, Int) -> Void)? = nil
    ) throws -> GitFastForwardOutcome {
        let journal = try GitOperationJournal.begin(gitDirectory, "checkout")
        defer { try? journal.complete() }
        let branchRef = try branch.ref()
        let store = RefStore(gitDirectory)
        guard let current = try store.readDirect(branchRef) else {
            throw GitException(.invalidRef, "branch does not exist")
        }
        if current == target { return .upToDate }
        if !(try hasObject(target)) {
            throw GitException(.invalidRef, "fast-forward target is not present")
        }
        switch try ancestry(current, target, cancellation) {
        case .ancestor:
            break
        case .notAncestor:
            return .diverged
        case .unknown:
            return .undetermined
        }
        let checkedOut = try readHead().target.sameText(branchRef.value)
        if checkedOut {
            let before = try status(cancellation: cancellation)
            if !before.isClean {
                throw GitException(.operationInterrupted, "cannot fast-forward with local changes")
            }
            let currentEntries = try headEntries()
            let targetEntries = try entriesForCommit(target)
            try materializeCheckout(currentEntries, targetEntries, cancellation, progress: progress)
            try cancellation?.throwIfCancelled()
            try index.write(GitIndexV2(Array(targetEntries.values)))
        }
        if try store.update(branchRef, expectedOld: current, next: target) != .updated {
            throw GitException(.refLocked, "branch changed during fast-forward")
        }
        return .fastForwarded
    }

    private func indexEntriesByPath() throws -> [String: GitIndexEntry] {
        var entries = [String: GitIndexEntry]()
        for entry in try index.read().entries { entries[engUtf8Decode(entry.path)] = entry }
        return entries
    }

    private func absolutePath(_ path: GitWorktreePath) -> String {
        EngineFS.join(worktree, path.components.joined(separator: EngineFS.separator))
    }

    private func stagePath(
        _ path: GitWorktreePath,
        _ entries: inout [String: GitIndexEntry],
        _ cancellation: GitCancellationToken?
    ) throws {
        try cancellation?.throwIfCancelled()
        let tree = GitWorktree(worktree)
        switch try tree.typeOf(path) {
        case .file:
            let bytes = try tree.readFile(path)
            let permissions = EngineFS.mode(absolutePath(path)) ?? 0
            let config = try RepositoryConfig.load(configFile)
            let mode: GitIndexMode = config.fileMode && permissions & 0o111 != 0 ? .executable : .regular
            entries[path.value] = try GitIndexEntry(
                path: path.bytes, objectId: try writeBlob(bytes), mode: mode, size: bytes.count
            )
        case .link:
            let target = try tree.readSymlink(path)
            let bytes = Array(target.utf8)
            entries[path.value] = try GitIndexEntry(
                path: path.bytes, objectId: try writeBlob(bytes), mode: .symlink, size: bytes.count
            )
        case .directory:
            // The Kotlin port's fix applies here too: stage each entry by its
            // own name so nested directories recurse as intended.
            for name in try EngineFS.listNames(absolutePath(path)) {
                try stagePath(try GitWorktreePath("\(path.value)/\(name)"), &entries, cancellation)
            }
        case .notFound:
            throw GitException(.invalidRepository, "worktree path is missing")
        default:
            throw GitException(.unsupportedRepositoryFormat, "worktree path has unsupported type")
        }
    }

    private func worktreeChange(_ entry: GitIndexEntry) throws -> GitChangeKind? {
        let path = try GitWorktreePath(engUtf8Decode(entry.path))
        let tree = GitWorktree(worktree)
        let type = try tree.typeOf(path)
        if type == .notFound { return .deleted }
        let bytes: [UInt8]
        if type == .file {
            bytes = try tree.readFile(path)
        } else if type == .link {
            bytes = Array(try tree.readSymlink(path).utf8)
        } else {
            return .modified
        }
        let mode: GitIndexMode = type == .link ? .symlink : entry.mode == .executable ? .executable : .regular
        let id = GitObjectId.sha1Of(GitObjectFrame.encode(.blob, bytes))
        if id != entry.objectId || mode != entry.mode { return .modified }
        return nil
    }

    private func collectUntracked(
        _ directory: String,
        _ prefix: [String],
        _ tracked: [String: GitIndexEntry],
        _ output: inout [GitStatusEntry],
        _ cancellation: GitCancellationToken?
    ) throws {
        for name in try EngineFS.listNames(directory) {
            try cancellation?.throwIfCancelled()
            if prefix.isEmpty && name.engSame(".git") { continue }
            let components = prefix + [name]
            let path = try GitWorktreePath(components.joined(separator: "/"))
            let entity = EngineFS.join(directory, name)
            if EngineFS.entityType(entity) == .directory {
                try collectUntracked(entity, components, tracked, &output, cancellation)
            } else if tracked[path.value] == nil {
                output.append(GitStatusEntry(path, .untracked))
            }
        }
    }

    private func headEntries() throws -> [String: GitIndexEntry] {
        guard let head = try readHeadObjectId() else { return [:] }
        return try entriesForCommit(head)
    }

    private func entriesForCommit(_ commitId: GitObjectId) throws -> [String: GitIndexEntry] {
        let commit = try readCommit(commitId)
        var entries = [String: GitIndexEntry]()
        try collectTreeEntries(commit.tree, [], &entries)
        return entries
    }

    private func materializeCheckout(
        _ current: [String: GitIndexEntry],
        _ target: [String: GitIndexEntry],
        _ cancellation: GitCancellationToken?,
        progress: ((Int, Int) -> Void)?
    ) throws {
        let tree = GitWorktree(worktree)
        let removedPaths = current.keys.filter { target[$0] == nil }.sorted { $1.engCompare($0) < 0 }
        for path in removedPaths {
            try cancellation?.throwIfCancelled()
            let removed = try GitWorktreePath(path)
            try tree.deleteEntry(removed)
            tree.removeEmptyParents(removed)
        }
        let changedPaths = target.keys.filter { path in
            guard let before = current[path], let after = target[path] else { return true }
            return before.objectId != after.objectId || before.mode != after.mode
        }.sorted { $0.engCompare($1) < 0 }
        progress?(0, changedPaths.count)
        for (written, path) in changedPaths.enumerated() {
            try cancellation?.throwIfCancelled()
            let entry = target[path]!
            let destination = try GitWorktreePath(path)
            let bytes = try readBlob(entry.objectId)
            if let before = current[path], (before.mode == .symlink) != (entry.mode == .symlink) {
                try tree.deleteEntry(destination)
            }
            switch entry.mode {
            case .regular, .executable:
                try tree.writeFile(destination, bytes, flush: false)
            case .symlink:
                guard let targetText = engUtf8DecodeStrict(bytes) else {
                    throw GitException(.symlinkEscape, "symlink payload is not valid UTF-8")
                }
                try tree.writeSymlink(destination, targetText)
            }
            if let progress, (written + 1) % 64 == 0 || written + 1 == changedPaths.count {
                progress(written + 1, changedPaths.count)
            }
        }
    }

    private func writeHead(_ value: String) throws {
        let destination = headFile
        let lock = "\(destination).lock"
        if !EngineFS.createFileExclusive(lock) {
            throw GitException(.refLocked, "HEAD lock already exists")
        }
        defer {
            if EngineFS.fileExists(lock) { try? EngineFS.deleteFile(lock) }
        }
        try EngineFS.writeStringFlushed(lock, value)
        try EngineFS.rename(lock, destination)
    }

    private func collectTreeEntries(
        _ treeId: GitObjectId,
        _ prefix: [String],
        _ output: inout [String: GitIndexEntry]
    ) throws {
        let tree = try readTree(treeId)
        for entry in tree.entries {
            let name = engUtf8Decode(entry.name)
            if name.lowercased() == ".git" {
                throw GitException(.pathEscapesWorktree, "tree entry names repository metadata")
            }
            let components = prefix + [name]
            if entry.mode == .directory {
                try collectTreeEntries(entry.objectId, components, &output)
            } else {
                let path = try GitWorktreePath(components.joined(separator: "/"))
                let mode: GitIndexMode
                switch entry.mode {
                case .regular: mode = .regular
                case .executable: mode = .executable
                case .symlink: mode = .symlink
                case .directory: fatalError("handled above")
                }
                output[path.value] = try GitIndexEntry(path: path.bytes, objectId: entry.objectId, mode: mode)
            }
        }
    }

    private final class TreeNode {
        var directories = [String: TreeNode]()
        var files = [String: GitIndexEntry]()
    }

    private func writeTree(_ entries: [GitIndexEntry], _ cancellation: GitCancellationToken?) throws -> GitObjectId {
        let root = TreeNode()
        for entry in entries {
            try cancellation?.throwIfCancelled()
            let path = try GitWorktreePath(engUtf8Decode(entry.path))
            var node = root
            for component in path.components.dropLast() {
                if node.files[component] != nil {
                    throw GitException(.invalidTree, "file and directory paths collide")
                }
                if let next = node.directories[component] {
                    node = next
                } else {
                    let next = TreeNode()
                    node.directories[component] = next
                    node = next
                }
            }
            let name = path.components[path.components.count - 1]
            if node.directories[name] != nil || node.files[name] != nil {
                throw GitException(.invalidTree, "duplicate tree path")
            }
            node.files[name] = entry
        }
        return try writeTreeNode(root, cancellation)
    }

    private func writeTreeNode(_ node: TreeNode, _ cancellation: GitCancellationToken?) throws -> GitObjectId {
        try cancellation?.throwIfCancelled()
        var entries = [GitTreeEntry]()
        for (key, entry) in node.files {
            let name = Array(key.utf8)
            switch entry.mode {
            case .regular: entries.append(try GitTreeEntry.regular(name, entry.objectId))
            case .executable: entries.append(try GitTreeEntry.executable(name, entry.objectId))
            case .symlink: entries.append(try GitTreeEntry.symlink(name, entry.objectId))
            }
        }
        for (key, directory) in node.directories {
            entries.append(try GitTreeEntry.directory(Array(key.utf8), try writeTreeNode(directory, cancellation)))
        }
        return try objects.write(.tree, try GitTree(entries).encode())
    }

    private enum HeadTarget {
        case ref(GitRefName)
        case direct(GitObjectId)
    }

    private func headTarget() throws -> HeadTarget {
        let value = try EngineFS.readString(headFile).engTrim()
        if value.engHasPrefix("ref: ") {
            return .ref(try GitRefName.branch(value.engSubstring(5)))
        }
        return .direct(try GitObjectId.parseSha1(value))
    }

    private func encodeCommit(_ request: GitCommitRequest, _ tree: GitObjectId, _ parent: GitObjectId?) -> [UInt8] {
        var headers = "tree \(tree.hex)\n"
        if let parent { headers += "parent \(parent.hex)\n" }
        headers += "author \(request.author.gitText)\n"
        headers += "committer \(request.committer.gitText)\n"
        headers += "\n"
        return Array(headers.utf8) + Array(request.message.utf8)
    }
}

/// A stable sort (Swift's `sorted` is not guaranteed stable).
public func stableSorted<T>(_ values: [T], by areInIncreasingOrder: (T, T) -> Bool) -> [T] {
    values.enumerated().sorted { left, right in
        if areInIncreasingOrder(left.element, right.element) { return true }
        if areInIncreasingOrder(right.element, left.element) { return false }
        return left.offset < right.offset
    }.map(\.element)
}
