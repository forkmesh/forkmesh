import FMAccount
import FMApi
import FMBase
import FMGit
import Foundation

public struct CodeInsightPrepared: Equatable, Sendable {
    public let worktree: URL
    public let tipRef: String?
    public let readsWorkingFiles: Bool
    public let name: String
    public let description: String
    public let sourceLabel: String
    public let warning: String

    public init(worktree: URL, tipRef: String?, readsWorkingFiles: Bool, name: String, description: String, sourceLabel: String, warning: String = "") {
        self.worktree = worktree
        self.tipRef = tipRef
        self.readsWorkingFiles = readsWorkingFiles
        self.name = name
        self.description = description
        self.sourceLabel = sourceLabel
        self.warning = warning
    }
}

public struct CodeInsightCopies: Sendable {
    public let root: URL

    public init(root: URL) {
        self.root = root.standardizedFileURL
    }

    public static func defaultRoot() -> URL {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        return caches.appendingPathComponent("CodeInsight", isDirectory: true)
    }

    public func directory(owner: String, name: String) -> URL? {
        guard CodeInsightLabels.copyFolder(owner: owner, name: name) != nil else { return nil }
        return root.appendingPathComponent(owner, isDirectory: true).appendingPathComponent(name, isDirectory: true)
    }

    public func exists(owner: String, name: String) -> Bool {
        guard let directory = directory(owner: owner, name: name) else { return false }
        return EngineFS.directoryExists(directory.appendingPathComponent(".git").path)
    }

    func metadataFile(owner: String, name: String) -> URL? {
        directory(owner: owner, name: name).map { $0.deletingLastPathComponent().appendingPathComponent("\(name).depth") }
    }

    public func fetchedDepth(owner: String, name: String) -> Int {
        guard let file = metadataFile(owner: owner, name: name),
              let data = FileManager.default.contents(atPath: file.path)
        else { return 0 }
        return Int(String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
    }

    public func recordDepth(_ depth: Int, owner: String, name: String) {
        guard let file = metadataFile(owner: owner, name: name) else { return }
        try? Data("\(depth)\n".utf8).write(to: file, options: .atomic)
    }

    public func stagingDirectory(owner: String, name: String) -> URL? {
        guard let directory = directory(owner: owner, name: name) else { return nil }
        return directory.deletingLastPathComponent().appendingPathComponent(".\(name).partial-\(UUID().uuidString.lowercased())", isDirectory: true)
    }

    public static func tipRef(branch: String?) -> String? {
        guard let branch, !branch.isEmpty, branch != "HEAD" else { return nil }
        return "refs/remotes/origin/\(branch)"
    }

    public static func deepen(fetched: Int, wanted: CodeInsightHistory, shallow: Bool) -> GitDeepen? {
        guard shallow, wanted != .filesOnly else { return nil }
        let want = wanted.cloneDepth
        let have = max(fetched, 1)
        return want > have ? .relative(want - have) : nil
    }
}

public struct CodeInsightCopyFailure: Error, Equatable, Sendable, CustomStringConvertible, LocalizedError {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
    public var errorDescription: String? { message }
}

@MainActor
public final class CodeInsightFetcher {
    public let copies: CodeInsightCopies
    let store: LocalRepoStore?
    let publisher: PublishService?
    let session: @MainActor () -> AuthSession?
    let mirrorLookup: @MainActor (String, String) async throws -> [RepoMirror]
    var workspaceFactory: ((URL) -> GitWorkspace)?
    private var active: GitWorkspace?

    public init(
        copies: CodeInsightCopies = CodeInsightCopies(root: CodeInsightCopies.defaultRoot()),
        store: LocalRepoStore?,
        publisher: PublishService?,
        session: @escaping @MainActor () -> AuthSession?,
        mirrors: (@MainActor (String, String) async throws -> [RepoMirror])? = nil
    ) {
        self.copies = copies
        self.store = store
        self.publisher = publisher
        self.session = session
        if let mirrors {
            mirrorLookup = mirrors
        } else if let api = publisher?.api {
            mirrorLookup = { owner, name in try await api.freshMirrorSet(owner, name).mirrors }
        } else {
            mirrorLookup = { _, _ in [] }
        }
    }

    public func cancel() { active?.cancel() }

    public func prepare(
        _ target: CodeInsightTarget,
        history: CodeInsightHistory,
        progress: @escaping @MainActor (String) -> Void
    ) async throws -> CodeInsightPrepared {
        switch target {
        case let .local(name, _):
            guard let store else { throw CodeInsightCopyFailure("Local repositories are not available in this build.") }
            guard let record = try await store.find(name) else {
                throw CodeInsightCopyFailure("\(name) is no longer on this phone.")
            }
            return CodeInsightPrepared(
                worktree: record.worktree, tipRef: nil, readsWorkingFiles: true, name: record.name,
                description: record.path, sourceLabel: CodeInsightLabels.onThisPhone)
        case let .account(owner, name):
            return try await prepareCopy(owner: owner, name: name, history: history, progress: progress)
        }
    }

    func workspace(_ directory: URL) -> GitWorkspace {
        workspaceFactory?(directory) ?? GitWorkspace(worktree: directory)
    }

    func prepareCopy(
        owner: String, name: String, history: CodeInsightHistory, progress: @escaping @MainActor (String) -> Void
    ) async throws -> CodeInsightPrepared {
        let slug = "\(owner)/\(name)"
        guard let directory = copies.directory(owner: owner, name: name) else {
            throw CodeInsightCopyFailure("\(slug) cannot be copied to this phone: its name has unusual characters.")
        }
        guard let publisher else { throw CodeInsightCopyFailure("Repositories cannot be copied to this phone in this build.") }
        let label = CodeInsightLabels.sourceLabel(.account(owner: owner, name: name), history)
        if copies.exists(owner: owner, name: name) {
            var warning = ""
            do {
                try await update(directory, owner: owner, name: name, history: history, publisher: publisher, progress: progress)
            } catch is CancellationError {
                throw CancellationError()
            } catch let error as GitWorkspaceException where error.code == .cancelled {
                throw CancellationError()
            } catch {
                warning = "Could not update the analysis copy, so this shows the copy already on the phone. \(LocalRepoScreenModel.message(error))"
            }
            let branch = try? GitInsightSource.open(worktree: directory, name: name, description: slug, readsWorkingFiles: false).branch
            return CodeInsightPrepared(
                worktree: directory, tipRef: CodeInsightCopies.tipRef(branch: branch), readsWorkingFiles: false,
                name: name, description: slug, sourceLabel: label, warning: warning)
        }
        let branch = try await cloneCopy(directory, owner: owner, name: name, history: history, publisher: publisher, progress: progress)
        return CodeInsightPrepared(
            worktree: directory, tipRef: CodeInsightCopies.tipRef(branch: branch), readsWorkingFiles: false,
            name: name, description: slug, sourceLabel: label)
    }

    private func update(
        _ directory: URL, owner: String, name: String, history: CodeInsightHistory, publisher: PublishService,
        progress: @escaping @MainActor (String) -> Void
    ) async throws {
        progress("Updating the analysis copy of \(owner)/\(name)…")
        let workspace = self.workspace(directory)
        active = workspace
        defer {
            if active === workspace { active = nil }
            workspace.dispose()
        }
        let fetched = copies.fetchedDepth(owner: owner, name: name)
        let shallow = (try? GitRepository.open(GitRepositoryOpen(directory.path)).isShallow) ?? false
        let deepen = CodeInsightCopies.deepen(fetched: fetched, wanted: history, shallow: shallow)
        let onProgress = progressRelay(progress, source: "\(owner)/\(name)")
        _ = try await publisher.withMirrorFailover(owner: owner, repo: name, action: "fetch") { credentials, route in
            try await workspace.insightFetch(credentials: credentials, route: route, deepen: deepen, onProgress: onProgress)
        }
        if deepen != nil || !shallow {
            copies.recordDepth(max(fetched, history.cloneDepth), owner: owner, name: name)
        }
    }

    private func cloneCopy(
        _ directory: URL, owner: String, name: String, history: CodeInsightHistory, publisher: PublishService,
        progress: @escaping @MainActor (String) -> Void
    ) async throws -> String? {
        let slug = "\(owner)/\(name)"
        progress("Finding mirrors of \(slug)…")
        let current = session()
        let relay = publisher.credentials(owner: owner, repo: name, signed: current != nil && !(current?.nodeName ?? "").isEmpty)
        var mirrors: [RepoMirror] = []
        var lookupProblem: String?
        do {
            mirrors = try await mirrorLookup(owner, name)
        } catch {
            lookupProblem = LocalRepoScreenModel.message(error)
        }
        let endpoints = CloneEndpoints.plan(owner: owner, name: name, mirrors: mirrors, relay: relay)
        try FileManager.default.createDirectory(at: directory.deletingLastPathComponent(), withIntermediateDirectories: true)
        let depth = history.cloneDepth
        let staged: (URL, String?) = try await CloneEndpoints.run(
            repository: slug, endpoints: endpoints, lookupProblem: lookupProblem, action: "copy",
            onAttempt: { endpoint, index in
                progress(CloneToDeviceModel.attemptLabel(endpoint, index: index, of: endpoints.count))
            }
        ) { endpoint in
            guard let staging = self.copies.stagingDirectory(owner: owner, name: name) else {
                throw CodeInsightCopyFailure("\(slug) cannot be copied to this phone.")
            }
            let workspace = self.workspace(staging)
            self.active = workspace
            defer {
                if self.active === workspace { self.active = nil }
                workspace.dispose()
            }
            let onProgress = self.progressRelay(progress, source: endpoint.label)
            do {
                let result = try await workspace.clone(credentials: endpoint.credentials(relay), depth: depth, onProgress: onProgress)
                return (staging, result.branch)
            } catch let error as GitWorkspaceException where endpoint.isRelay && error.code == .protocolUnsupported {
                guard let fresh = self.copies.stagingDirectory(owner: owner, name: name) else { throw error }
                let retry = self.workspace(fresh)
                defer { retry.dispose() }
                let result = try await retry.clone(credentials: endpoint.credentials(relay), onProgress: onProgress)
                return (fresh, result.branch)
            }
        }
        if EngineFS.entityType(directory.path, followLinks: false) != .notFound {
            throw CodeInsightCopyFailure("A folder for the analysis copy of \(slug) is already in the way.")
        }
        try FileManager.default.moveItem(at: staged.0, to: directory)
        copies.recordDepth(depth, owner: owner, name: name)
        return staged.1
    }

    private func progressRelay(_ progress: @escaping @MainActor (String) -> Void, source: String) -> @Sendable (GitTransferProgress) -> Void {
        { update in
            Task { @MainActor in progress(CloneToDeviceModel.progressLabel(update, source: source)) }
        }
    }
}

extension GitWorkspace {
    func insightFetch(
        credentials: ForkMeshRemoteCredentials,
        route: GitRemoteRoute,
        deepen: GitDeepen?,
        onProgress: (@Sendable (GitTransferProgress) -> Void)?
    ) async throws -> LocalFetchResult {
        let factory = transportFactory
        let listener: GitProgressListener? = onProgress.map { handler -> GitProgressListener in
            { @Sendable sample in
                handler(GitTransferProgress(
                    phase: sample.phase.rawValue, bytes: sample.bytes, objects: sample.objects, refs: sample.refs,
                    total: sample.total))
            }
        }
        return try await withRepository("insight-fetch") { repo, token in
            let result = try await WorkspaceOps.fetchInto(
                repo, credentials, "origin", refspec: nil, deepen: deepen, route: route,
                inner: factory?(), token: token, progress: listener)
            return WorkspaceOps.localFetchResult(result, credentials: credentials, route: route)
        }
    }
}
