// The worker-side half of GitWorkspace. Everything here runs on the runner's
// detached task, never on the main actor, and only receives and returns plain
// values.

import FMBase
import FMGit
import Foundation

enum WorkspaceOps {
    /// Bounded walk used by `syncState`; beyond it the counts are reported as
    /// unknown rather than paid for.
    static let maxSyncWalk = 500

    /// Engine default caps reused for imports (never raised).
    static let maxImportFiles = 100_000
    static let maxImportBytes = 512 * 1024 * 1024
    static let maxImportFileBytes = GitResourceLimits.defaultMaxObjectBytes
    static let maxZipBytes = 256 * 1024 * 1024
    static let maxTreeDepth = 64

    static func open(_ path: String) throws -> GitRepository {
        try GitRepository.open(GitRepositoryOpen(path, limits: try GitWorkspace.deviceLimits(), detector: GitSha1DCDetector()))
    }

    // MARK: - Init and imports

    static func initRequest(_ path: String, branch: String?) throws -> GitRepositoryInit {
        let initial: GitBranchName? = try branch.map { (name: String) -> GitBranchName in try GitBranchName(name) }
        return GitRepositoryInit(path, initialBranch: initial)
    }

    static func initialize(
        _ path: String, files: [String: String], message: String?, author: GitAuthor?, now: Date, branch: String? = nil
    ) throws -> String? {
        let repo = try GitRepository.initialize(initRequest(path, branch: branch))
        if files.isEmpty { return nil }
        let tree = GitWorktree(repo.worktree)
        var paths: [GitWorktreePath] = []
        for name in files.keys.sorted() {
            let path = try GitWorktreePath(name)
            try tree.writeFile(path, Array(files[name]!.utf8))
            paths.append(path)
        }
        try repo.add(paths)
        guard let message, let author else { return nil }
        let identity = try author.toIdentity(now)
        let id = try repo.commit(GitCommitRequest(message: message, author: identity, committer: identity))
        return id.hex
    }

    static func importDirectory(
        _ path: String, source: String, author: GitAuthor, message: String, now: Date, token: GitCancellationToken,
        branch: String? = nil
    ) throws -> LocalImportResult {
        if EngineFS.entityType(source, followLinks: false) != .directory {
            throw GitWorkerException("The selected folder could not be read.")
        }
        let repo = try GitRepository.initialize(initRequest(path, branch: branch))
        let tree = GitWorktree(repo.worktree)
        var imported = 0
        var skipped = 0
        var totalBytes = 0
        var roots = Set<String>()

        func walk(_ directory: String, _ prefix: [String]) throws {
            try token.throwIfCancelled()
            if prefix.count > maxTreeDepth {
                throw GitException(.resourceLimitExceeded, "import tree is too deep")
            }
            for name in try EngineFS.listNames(directory) {
                if prefix.isEmpty && name == ".git" {
                    // Never adopt foreign repository metadata (hooks, config, objects).
                    skipped += 1
                    continue
                }
                let entity = EngineFS.join(directory, name)
                let type = EngineFS.entityType(entity, followLinks: false)
                let components = prefix + [name]
                let worktreePath: GitWorktreePath
                do {
                    worktreePath = try GitWorktreePath(components.joined(separator: "/"))
                } catch is GitException {
                    skipped += 1
                    continue
                }
                if type == .directory {
                    try walk(entity, components)
                } else if type == .file {
                    let size = fileSize(entity)
                    if size > maxImportFileBytes {
                        throw GitException(.resourceLimitExceeded, "a file exceeds the maximum object size")
                    }
                    totalBytes += size
                    imported += 1
                    if imported > maxImportFiles || totalBytes > maxImportBytes {
                        throw GitException(.resourceLimitExceeded, "import exceeds file or byte limits")
                    }
                    try tree.writeFile(worktreePath, try EngineFS.readBytes(entity))
                    roots.insert(components[0])
                } else {
                    // Symlinks and special files are never copied from outside the app.
                    skipped += 1
                }
            }
        }

        try walk(source, [])
        let commitId = try commitImport(repo, roots: roots, author: author, message: message, now: now, imported: imported)
        return LocalImportResult(importedFiles: imported, skippedEntries: skipped, commitId: commitId)
    }

    static func importZip(
        _ path: String, zipPath: String, author: GitAuthor, message: String, now: Date, token: GitCancellationToken,
        branch: String? = nil
    ) throws -> LocalImportResult {
        if EngineFS.entityType(zipPath, followLinks: false) != .file {
            throw GitWorkerException("The selected archive could not be read.")
        }
        if fileSize(zipPath) > maxZipBytes {
            throw GitException(.resourceLimitExceeded, "zip archive is too large")
        }
        let archive: LocalZipArchive
        do {
            archive = try LocalZipArchive(bytes: try EngineFS.readBytes(zipPath))
        } catch {
            throw GitWorkerException("The archive is not a valid zip file.", String(describing: error))
        }
        // Collect candidate files first so limits apply before any write.
        var candidates: [LocalZipArchive.Entry] = []
        var declaredBytes = 0
        for file in archive.entries {
            if !file.isFile || file.isSymbolicLink { continue }
            let name = file.name
            if name.utf8.starts(with: "__MACOSX/".utf8) || name.utf8.reversed().starts(with: "/.DS_Store".utf8.reversed())
                || name == ".DS_Store" {
                continue
            }
            if file.size > maxImportFileBytes {
                throw GitException(.resourceLimitExceeded, "a file exceeds the maximum object size")
            }
            declaredBytes += file.size
            candidates.append(file)
            if candidates.count > maxImportFiles || declaredBytes > maxImportBytes {
                throw GitException(.resourceLimitExceeded, "import exceeds file or byte limits")
            }
        }
        // Strip one shared top-level folder ("project/…") like Finder zips.
        var sharedRoot: String?
        var allShareRoot = !candidates.isEmpty
        for file in candidates {
            // Byte-wise: "/" followed by a combining mark is one Character.
            let bytes = Array(file.name.utf8)
            guard let slash = bytes.firstIndex(of: 0x2f), slash > 0 else {
                allShareRoot = false
                break
            }
            let root = String(decoding: bytes[0..<slash], as: UTF8.self)
            if sharedRoot == nil {
                sharedRoot = root
            } else if sharedRoot != root {
                allShareRoot = false
                break
            }
        }
        let strip = allShareRoot && sharedRoot != nil ? "\(sharedRoot!)/" : ""

        let repo = try GitRepository.initialize(initRequest(path, branch: branch))
        let tree = GitWorktree(repo.worktree)
        var imported = 0
        var skipped = 0
        var totalBytes = 0
        var roots = Set<String>()
        for file in candidates {
            try token.throwIfCancelled()
            var name = file.name
            if !strip.isEmpty && name.utf8.starts(with: strip.utf8) {
                name = String(decoding: Array(name.utf8).dropFirst(strip.utf8.count), as: UTF8.self)
            }
            while name.utf8.starts(with: "./".utf8) {
                name = String(decoding: Array(name.utf8).dropFirst(2), as: UTF8.self)
            }
            let worktreePath: GitWorktreePath
            do {
                worktreePath = try GitWorktreePath(name)
            } catch is GitException {
                skipped += 1
                continue
            }
            if worktreePath.components[0] == ".git" || worktreePath.components.count > maxTreeDepth {
                skipped += 1
                continue
            }
            let bytes: [UInt8]
            do {
                bytes = try archive.content(of: file, maximumBytes: maxImportFileBytes)
            } catch let error as GitException {
                throw error
            } catch {
                throw GitWorkerException("The archive is not a valid zip file.", String(describing: error))
            }
            totalBytes += bytes.count
            if bytes.count > maxImportFileBytes || totalBytes > maxImportBytes {
                throw GitException(.resourceLimitExceeded, "import exceeds file or byte limits")
            }
            try tree.writeFile(worktreePath, bytes)
            imported += 1
            roots.insert(worktreePath.components[0])
        }
        let commitId = try commitImport(repo, roots: roots, author: author, message: message, now: now, imported: imported)
        return LocalImportResult(importedFiles: imported, skippedEntries: skipped, commitId: commitId)
    }

    static func commitImport(
        _ repo: GitRepository, roots: Set<String>, author: GitAuthor, message: String, now: Date, imported: Int
    ) throws -> String {
        if imported == 0 {
            throw GitWorkerException("Nothing importable was found.")
        }
        try repo.add(try roots.sorted().map { try GitWorktreePath($0) })
        let identity = try author.toIdentity(now)
        let id = try repo.commit(GitCommitRequest(message: message, author: identity, committer: identity))
        return id.hex
    }

    // MARK: - Files

    static func listFiles(_ path: String, _ target: String) throws -> [LocalFileEntry] {
        let repo = try open(path)
        let index = try indexPaths(repo)
        let prefixComponents = target.isEmpty ? [] : try GitWorktreePath(target).components
        var directory = repo.worktree
        for component in prefixComponents {
            let next = EngineFS.join(directory, component)
            if EngineFS.entityType(next, followLinks: false) != .directory {
                throw GitException(.pathEscapesWorktree, "directory is missing or is a symlink")
            }
            directory = next
        }
        var entries: [LocalFileEntry] = []
        for name in try EngineFS.listNames(directory) {
            if prefixComponents.isEmpty && name == ".git" { continue }
            let relative = (prefixComponents + [name]).joined(separator: "/")
            let entity = EngineFS.join(directory, name)
            let kind: LocalFileKind
            switch EngineFS.entityType(entity, followLinks: false) {
            case .directory: kind = .directory
            case .link: kind = .symlink
            default: kind = .file
            }
            let size = kind == .file ? fileSize(entity) : 0
            let tracked = kind == .directory
                ? index.contains { $0.utf8.starts(with: "\(relative)/".utf8) }
                : index.contains(relative)
            entries.append(LocalFileEntry(name: name, path: relative, kind: kind, size: size, tracked: tracked))
        }
        entries.sort { a, b in
            if a.isDirectory != b.isDirectory { return a.isDirectory }
            // Lower-cased strings in UTF-16 code unit order.
            return a.name.lowercased().utf16.lexicographicallyPrecedes(b.name.lowercased().utf16)
        }
        return entries
    }

    static func readFile(_ path: String, _ target: String) throws -> LocalFileContent {
        let repo = try open(path)
        let bytes = try GitWorktree(repo.worktree).readFile(try GitWorktreePath(target))
        if bytes.count > LocalFileContent.maxEditableBytes {
            return LocalFileContent(path: target, bytes: bytes.count, text: "", isBinary: false, isTooLarge: true)
        }
        let probe = bytes.count > 8000 ? Array(bytes[0..<8000]) : bytes
        if probe.contains(0) {
            return LocalFileContent(path: target, bytes: bytes.count, text: "", isBinary: true, isTooLarge: false)
        }
        guard let text = strictUtf8(bytes) else {
            return LocalFileContent(path: target, bytes: bytes.count, text: "", isBinary: true, isTooLarge: false)
        }
        return LocalFileContent(path: target, bytes: bytes.count, text: text, isBinary: false, isTooLarge: false)
    }

    static func writeFile(_ path: String, _ target: String, _ text: String) throws {
        let repo = try open(path)
        let file = try GitWorktreePath(target)
        if file.components[0] == ".git" {
            throw GitException(.pathEscapesWorktree, "cannot write repository metadata")
        }
        let bytes = Array(text.utf8)
        if bytes.count > repo.limits.maxObjectBytes {
            throw GitException(.resourceLimitExceeded, "file exceeds the maximum object size")
        }
        try GitWorktree(repo.worktree).writeFile(file, bytes)
    }

    static func deleteFile(_ path: String, _ target: String) throws {
        let repo = try open(path)
        let file = try GitWorktreePath(target)
        if file.components[0] == ".git" {
            throw GitException(.pathEscapesWorktree, "cannot delete repository metadata")
        }
        try GitWorktree(repo.worktree).deleteFile(file)
    }

    // MARK: - Status, staging, commit

    static func status(_ path: String, token: GitCancellationToken) throws -> LocalStatus {
        try status(try open(path), token: token)
    }

    static func status(_ repo: GitRepository, token: GitCancellationToken?) throws -> LocalStatus {
        let status = try repo.status(cancellation: token)
        func kindOf(_ kind: GitChangeKind) -> LocalChangeKind {
            switch kind {
            case .added: return .added
            case .modified: return .modified
            case .deleted: return .deleted
            case .untracked: return .untracked
            }
        }
        let staged = status.staged.map { LocalChange(path: $0.path.value, kind: kindOf($0.kind), staged: true) }
        var unstaged: [LocalChange] = []
        var untracked: [LocalChange] = []
        for entry in status.worktree {
            let change = LocalChange(path: entry.path.value, kind: kindOf(entry.kind), staged: false)
            if entry.kind == .untracked {
                untracked.append(change)
            } else {
                unstaged.append(change)
            }
        }
        return LocalStatus(staged: staged, unstaged: unstaged, untracked: untracked)
    }

    static func stage(_ path: String, _ targets: [String]) throws {
        try stagePaths(try open(path), targets)
    }

    static func stageAll(_ path: String, token: GitCancellationToken) throws {
        let repo = try open(path)
        let current = try status(repo, token: token)
        try stagePaths(repo, current.unstaged.map(\.path) + current.untracked.map(\.path))
    }

    static func stagePaths(_ repo: GitRepository, _ targets: [String]) throws {
        var adds: [GitWorktreePath] = []
        var removes: [GitWorktreePath] = []
        let worktree = GitWorktree(repo.worktree)
        for target in targets {
            let path = try GitWorktreePath(target)
            if try worktree.typeOf(path) == .notFound {
                removes.append(path)
            } else {
                adds.append(path)
            }
        }
        if !removes.isEmpty { try repo.remove(removes) }
        if !adds.isEmpty { try repo.add(adds) }
    }

    static func unstage(_ path: String, _ targets: Set<String>, token: GitCancellationToken) throws {
        try unstagePaths(try open(path), targets, token: token)
    }

    static func unstageAll(_ path: String, token: GitCancellationToken) throws {
        let repo = try open(path)
        let current = try status(repo, token: token)
        try unstagePaths(repo, Set(current.staged.map(\.path)), token: token)
    }

    /// Reset index entries for `targets` to their HEAD state (or drop them
    /// when HEAD has no such path) through the engine's journaled `resetPaths`.
    static func unstagePaths(_ repo: GitRepository, _ targets: Set<String>, token: GitCancellationToken) throws {
        if targets.isEmpty { return }
        try repo.resetPaths(try targets.sorted().map { try GitWorktreePath($0) }, cancellation: token)
    }

    static func discard(_ path: String, _ targets: [String]) throws {
        let repo = try open(path)
        let index = try GitIndexStore(repo.gitDirectory, repo.limits).read()
        var entries: [String: GitIndexEntry] = [:]
        for entry in index.entries { entries[String(decoding: entry.path, as: UTF8.self)] = entry }
        let tree = GitWorktree(repo.worktree)
        for target in targets {
            let file = try GitWorktreePath(target)
            if file.components[0] == ".git" { continue }
            guard let entry = entries[target] else {
                try tree.deleteFile(file)
                continue
            }
            let bytes = try repo.readBlob(entry.objectId)
            if entry.mode == .symlink {
                try tree.writeSymlink(file, String(decoding: bytes, as: UTF8.self))
            } else {
                try tree.writeFile(file, bytes)
            }
        }
    }

    static func commit(_ path: String, message raw: String, author: GitAuthor, now: Date, token: GitCancellationToken) throws -> String {
        let repo = try open(path)
        let message = raw.dartTrimmed
        if message.isEmpty {
            throw GitWorkerException("Enter a commit message.")
        }
        if try repo.status(cancellation: token).staged.isEmpty {
            throw GitWorkerException("Nothing is staged. Stage changes first.")
        }
        let identity = try author.toIdentity(now)
        let id = try repo.commit(
            GitCommitRequest(
                message: message.hasSuffix("\n") ? message : "\(message)\n",
                author: identity,
                committer: identity),
            cancellation: token)
        return id.hex
    }

    // MARK: - Branches and history

    static func currentBranch(_ repo: GitRepository) -> String? {
        guard let head = try? repo.readHead() else { return nil } // detached
        let prefix = "refs/heads/"
        guard head.target.utf8.starts(with: prefix.utf8) else { return nil }
        return String(decoding: Array(head.target.utf8).dropFirst(prefix.utf8.count), as: UTF8.self)
    }

    static func branches(_ repo: GitRepository) throws -> [LocalBranch] {
        let current = currentBranch(repo)
        // Engine enumeration covers loose and packed refs, sorted by name.
        var branches = try repo.listBranches().map {
            LocalBranch(name: $0.shortName, isCurrent: $0.shortName == current, tipId: $0.objectId.hex)
        }
        if let current, !branches.contains(where: { $0.name == current }) {
            // Unborn branch: HEAD points at a ref that has no commits yet.
            branches.insert(LocalBranch(name: current, isCurrent: true, tipId: nil), at: 0)
        }
        return branches
    }

    static func createBranch(_ path: String, _ name: String, checkout: Bool, token: GitCancellationToken) throws {
        let repo = try open(path)
        let branch = try GitBranchName(name)
        try repo.createBranch(branch)
        if checkout {
            try repo.checkout(.branch(branch), cancellation: token)
        }
    }

    static func checkout(_ path: String, _ name: String, token: GitCancellationToken) throws {
        let repo = try open(path)
        try repo.checkout(.branch(try GitBranchName(name)), cancellation: token)
    }

    static func log(_ path: String, limit: Int, token: GitCancellationToken) throws -> [LocalCommit] {
        let repo = try open(path)
        // The engine walks newest-first in committer date order and applies
        // its own commit-walk bound on top of the requested limit.
        return try repo.log(GitLogRequest(limit: limit), cancellation: token).map {
            toLocalCommit($0.id.hex, $0.commit)
        }
    }

    static func summary(_ path: String) throws -> LocalRepoSummary {
        let repo = try open(path)
        let branch = currentBranch(repo) ?? "HEAD"
        guard let head = try repo.readHeadObjectId() else {
            return LocalRepoSummary(branch: branch, headId: nil, lastCommitSubject: "", lastCommitAt: nil)
        }
        let commit = toLocalCommit(head.hex, try repo.readCommit(head))
        return LocalRepoSummary(branch: branch, headId: head.hex, lastCommitSubject: commit.subject, lastCommitAt: commit.time)
    }

    static func toLocalCommit(_ id: String, _ commit: GitCommit) -> LocalCommit {
        let message = String(decoding: commit.message, as: UTF8.self)
        let firstLine = message.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init) ?? ""
        // Typed engine identities; foreign history with non-canonical headers
        // falls back to the raw header text and the epoch.
        let author = commit.author
        let committer = commit.committer
        return LocalCommit(
            id: id,
            parents: commit.parents.map(\.hex),
            subject: firstLine.dartTrimmed,
            message: message,
            authorName: author?.name ?? commit.headers["author"]?.first ?? "",
            authorEmail: author?.email ?? "",
            time: committer?.timeUtc ?? author?.timeUtc ?? Date(timeIntervalSince1970: 0))
    }

    static func indexPaths(_ repo: GitRepository) throws -> Set<String> {
        let index = try GitIndexStore(repo.gitDirectory, repo.limits).read()
        return Set(index.entries.map { String(decoding: $0.path, as: UTF8.self) })
    }

    // MARK: - Network

    static func pointRemote(_ repo: GitRepository, _ remote: String, _ url: String) throws {
        let existing = try repo.readConfig().remotes[remote]?.fetch
        try repo.setRemote(remote, try GitRemoteUrl.parse(url), fetch: existing)
    }

    static func push(
        _ path: String, credentials: ForkMeshRemoteCredentials, remote: String, branch: String,
        inner: GitHttpTransport?, token: GitCancellationToken, progress: GitProgressListener?
    ) async throws -> LocalPushResult {
        do {
            return try await pushOnce(path, credentials, remote, branch, inner, token, progress)
        } catch let error as GitException where error.code == .nonFastForward {
            let repo = try open(path, limits: try GitWorkspace.deviceLimits())
            if !repo.isShallow { throw error }
            try await ShallowPushProof.prove(
                repo, branch: branch, remote: remote,
                fetch: { refspec, deepen in
                    _ = try await fetchInto(
                        repo, credentials, remote, refspec: refspec, deepen: deepen, route: nil,
                        inner: inner, token: token, progress: progress)
                },
                token: token)
            return try await pushOnce(path, credentials, remote, branch, inner, token, progress)
        }
    }

    private static func pushOnce(
        _ path: String, _ credentials: ForkMeshRemoteCredentials, _ remote: String, _ branch: String,
        _ inner: GitHttpTransport?, _ token: GitCancellationToken, _ progress: GitProgressListener?
    ) async throws -> LocalPushResult {
        let repo = try open(path)
        try pointRemote(repo, remote, credentials.remoteUrl)
        let result = try await repo.push(
            GitPushRequest(
                remote: remote,
                refspecs: [try GitPushRefspec(localBranch: branch)],
                access: forkMeshRemoteAccess(credentials, inner: inner),
                cancellation: token),
            progress: progress)
        return LocalPushResult(
            refs: result.refs.map {
                LocalPushRef(remoteRef: $0.remoteRef, newId: $0.newId.hex, outcome: $0.outcome.rawValue, message: $0.message ?? "")
            },
            packBytes: result.packBytes,
            objects: result.objects)
    }

    static func open(_ path: String, limits: GitResourceLimits) throws -> GitRepository {
        try GitRepository.open(GitRepositoryOpen(path, limits: limits, detector: GitSha1DCDetector()))
    }

    static func fetchInto(
        _ repo: GitRepository, _ credentials: ForkMeshRemoteCredentials, _ remote: String,
        refspec: GitFetchRefspec?, deepen: GitDeepen?, route: GitRemoteRoute?,
        inner: GitHttpTransport?, token: GitCancellationToken, progress: GitProgressListener?
    ) async throws -> GitFetchResult {
        let home = credentials.origin.isEmpty ? credentials.remoteUrl : credentials.relayUrl
        try pointRemote(repo, remote, home)
        let override = credentials.remoteUrl == home ? nil : try GitRemoteUrl.parse(credentials.remoteUrl)
        return try await repo.fetch(
            GitFetchRequest(
                remote: remote, url: override, refspec: refspec, deepen: deepen,
                access: forkMeshRemoteAccess(credentials, inner: inner, route: route), cancellation: token),
            progress: progress)
    }

    static func fetch(
        _ path: String, credentials: ForkMeshRemoteCredentials, remote: String, route: GitRemoteRoute? = nil,
        inner: GitHttpTransport?, token: GitCancellationToken, progress: GitProgressListener?
    ) async throws -> LocalFetchResult {
        let repo = try open(path)
        let route = route ?? GitRemoteRoute()
        let result = try await fetchInto(
            repo, credentials, remote, refspec: nil, deepen: nil, route: route,
            inner: inner, token: token, progress: progress)
        return localFetchResult(result, credentials: credentials, route: route)
    }

    static func localFetchResult(
        _ result: GitFetchResult, credentials: ForkMeshRemoteCredentials, route: GitRemoteRoute
    ) -> LocalFetchResult {
        let servedBy = route.mirrorHost ?? (credentials.endpointUrl.isEmpty ? nil : credentials.host)
        return LocalFetchResult(
            updatedRefs: result.updates.filter(\.isChanged).map(\.ref),
            packBytes: result.packBytes,
            objects: result.objects,
            remoteHead: result.remoteHead,
            servedBy: servedBy)
    }

    static func pull(
        _ path: String, credentials: ForkMeshRemoteCredentials, remote: String, branch: String, route: GitRemoteRoute? = nil,
        inner: GitHttpTransport?, token: GitCancellationToken, progress: GitProgressListener?
    ) async throws -> LocalPullResult {
        let repo = try open(path)
        let route = route ?? GitRemoteRoute()
        let tracking = "refs/remotes/\(remote)/\(branch)"
        let refspec = try GitFetchRefspec.parse("+refs/heads/\(branch):\(tracking)")
        let fetched = try await fetchInto(
            repo, credentials, remote, refspec: refspec, deepen: nil, route: route,
            inner: inner, token: token, progress: progress)
        let fetch = localFetchResult(fetched, credentials: credentials, route: route)
        let store = RefStore(repo.gitDirectory)
        let branchName = try GitBranchName(branch)
        guard let remoteTip = try store.readDirect(try GitRefName.parse(tracking)) else {
            return LocalPullResult(branch: branch, fetch: fetch, outcome: .noRemoteBranch)
        }
        guard let localTip = try store.readDirect(try branchName.ref()) else {
            throw GitException(.invalidRef, "local branch \(branch) does not exist")
        }
        if localTip != remoteTip, try repo.ancestry(remoteTip, localTip, token) == .ancestor {
            return LocalPullResult(branch: branch, fetch: fetch, outcome: .ahead)
        }
        let forwarded: GitFastForwardOutcome
        do {
            forwarded = try repo.fastForward(branchName, to: remoteTip, cancellation: token) { written, total in
                progress?(GitProgress(.checkingOut, objects: written, total: total))
            }
        } catch let error as GitException where error.code == .operationInterrupted {
            throw GitWorkspaceException(
                "Commit or discard your changes before syncing \(branch).", code: .operationInterrupted, detail: error.message)
        }
        let outcome: LocalPullOutcome
        switch forwarded {
        case .upToDate: outcome = .upToDate
        case .fastForwarded: outcome = .fastForwarded(from: localTip.hex, to: remoteTip.hex)
        case .diverged: outcome = .diverged
        case .undetermined: outcome = .undetermined
        }
        return LocalPullResult(branch: branch, fetch: fetch, outcome: outcome)
    }

    static func clone(
        _ path: String, credentials: ForkMeshRemoteCredentials, branch: String?, depth: Int? = nil, maxPackBytes: Int,
        inner: GitHttpTransport?, token: GitCancellationToken, progress: GitProgressListener?
    ) async throws -> LocalCloneResult {
        let result = try await GitRepository.clone(
            GitCloneRequest(
                url: try GitRemoteUrl.parse(credentials.remoteUrl),
                worktree: path,
                branch: branch,
                depth: depth,
                singleBranch: depth != nil,
                access: forkMeshRemoteAccess(credentials, inner: inner),
                limits: try GitWorkspace.deviceLimits(maxPackBytes: maxPackBytes),
                detector: GitSha1DCDetector(),
                cancellation: token),
            progress: progress)
        if credentials.remoteUrl != credentials.relayUrl, !credentials.origin.isEmpty {
            try pointRemote(result.repository, "origin", credentials.relayUrl)
        }
        return LocalCloneResult(branch: result.checkedOutBranch, packBytes: result.fetch.packBytes, objects: result.fetch.objects)
    }

    static func syncState(_ path: String, remote: String, branch: String, token: GitCancellationToken) throws -> LocalSyncState {
        let repo = try open(path)
        if branch.isEmpty { return .unknown(branch) }
        let store = RefStore(repo.gitDirectory)
        let localRef = "refs/heads/\(branch)"
        let remoteRef = "refs/remotes/\(remote)/\(branch)"
        let local = try store.readDirect(try GitRefName.parse(localRef))
        let remoteTip = try store.readDirect(try GitRefName.parse(remoteRef))
        guard let local, let remoteTip else {
            return LocalSyncState(branch: branch, localTip: local?.hex, remoteTip: remoteTip?.hex, ahead: nil, behind: nil)
        }
        if local == remoteTip {
            return LocalSyncState(branch: branch, localTip: local.hex, remoteTip: remoteTip.hex, ahead: 0, behind: 0)
        }
        return LocalSyncState(
            branch: branch,
            localTip: local.hex,
            remoteTip: remoteTip.hex,
            ahead: try distance(repo, from: localRef, to: remoteTip.hex, token: token),
            behind: try distance(repo, from: remoteRef, to: local.hex, token: token))
    }

    /// Commits between `from`'s tip and `target`, or nil when `target` is not
    /// reachable within `maxSyncWalk`.
    static func distance(_ repo: GitRepository, from: String, to target: String, token: GitCancellationToken) throws -> Int? {
        let entries = try repo.log(GitLogRequest(ref: from, limit: maxSyncWalk), cancellation: token)
        for (index, entry) in entries.enumerated() where entry.id.hex == target {
            return index
        }
        return nil
    }

    // MARK: - Helpers

    static func fileSize(_ path: String) -> Int {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return (attributes?[.size] as? NSNumber)?.intValue ?? 0
    }

    /// Strict UTF-8 decoding: nil on malformed input.
    static func strictUtf8(_ bytes: [UInt8]) -> String? {
        try? Utf8.decode(bytes)
    }
}
