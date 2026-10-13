// The adapter FMChat's node-sync applier needs over the on-device Git port
// (FMChat/NodeSync/NodeSyncWorkspace.swift has the contract). FMChat cannot
// see FMCode, so these two small types stand between the applier and
// LocalRepoStore and GitWorkspace:
//
//   LocalRepoStore -> LocalNodeSyncRepositories (NodeSyncRepositorySource)
//   GitWorkspace   -> LocalNodeSyncWorkspace    (NodeSyncWorkspace)
//
// How the contract is met:
// - `readFile` throws for a missing path: GitWorktree.readFile does, and so
//   does a missing parent directory.
// - `writeFile` creates parent directories (GitWorktree.writeFile).
// - `stage` stages exactly the given paths; a path gone from disk is staged
//   as a deletion (GitWorkspaceOperations.stagePaths).
// - `commit` with nothing staged throws GitWorkerException "Nothing is
//   staged. Stage changes first.", which the applier counts as success.
// - GitException and GitWorkerException describe themselves.

import FMBase
import FMChat
import FMGit
import Foundation

/// One repository's worktree for the applier.
@MainActor
public final class LocalNodeSyncWorkspace: NodeSyncWorkspace {
    public let workspace: GitWorkspace

    public init(_ workspace: GitWorkspace) {
        self.workspace = workspace
    }

    public func summary() async throws -> NodeSyncWorkspaceSummary {
        NodeSyncWorkspaceSummary(branch: try await workspace.summary().branch)
    }

    public func readFile(_ path: String) async throws -> NodeSyncFileContent {
        let content = try await workspace.readFile(path)
        return NodeSyncFileContent(
            path: content.path, text: content.text, isBinary: content.isBinary, isTooLarge: content.isTooLarge)
    }

    public func writeFile(_ path: String, _ text: String) async throws {
        try await workspace.writeFile(path, text)
    }

    public func deleteFile(_ path: String) async throws {
        try await workspace.deleteFile(path)
    }

    public func listFiles(_ directory: String) async throws -> [NodeSyncFileEntry] {
        try await workspace.listFiles(directory).map {
            NodeSyncFileEntry(name: $0.name, path: $0.path, isDirectory: $0.kind == .directory)
        }
    }

    public func stage(_ paths: [String]) async throws {
        try await workspace.stage(paths)
    }

    public func commit(message: String, author: NodeSyncAuthor) async throws -> String {
        try await workspace.commit(message: message, author: GitAuthor(name: author.name, email: author.email))
    }

    public func dispose() {
        workspace.dispose()
    }
}

/// The on-device repositories for the applier.
@MainActor
public final class LocalNodeSyncRepositories: NodeSyncRepositorySource {
    public let store: LocalRepoStore

    public init(store: LocalRepoStore) {
        self.store = store
    }

    public func ensureLoaded() async {
        await store.ensureLoaded()
    }

    public var records: [NodeSyncRepoRecord] {
        store.records.map { NodeSyncRepoRecord(name: $0.name, path: $0.path, publishedAs: $0.publishedAs) }
    }

    public func workspace(for record: NodeSyncRepoRecord) -> NodeSyncWorkspace {
        if let local = store.records.first(where: { $0.name == record.name && $0.path == record.path }) {
            return LocalNodeSyncWorkspace(store.workspace(local))
        }
        return LocalNodeSyncWorkspace(store.makeWorkspace(URL(fileURLWithPath: record.path, isDirectory: true)))
    }

    public func refresh(_ name: String) async throws {
        _ = try await store.refresh(name)
    }
}

extension LocalNodeSyncApplier {
    /// The applier over `store`, committing as the signed-in account, with its
    /// applied ledger in `scope`'s namespace. Call `installOn(_:)` to let the
    /// node acknowledge items.
    public static func forLocalRepos(
        _ store: LocalRepoStore,
        prefs: KeyValueStore,
        scope: LocalRepoScope = LegacyLocalRepoScope(),
        account: @escaping @MainActor () -> (nodeName: String, email: String)
    ) -> LocalNodeSyncApplier {
        LocalNodeSyncApplier(
            repos: LocalNodeSyncRepositories(store: store),
            author: {
                let who = account()
                let author = LocalRepoText.overriding(GitAuthor.fromAccount(nodeName: who.nodeName, email: who.email), prefs: prefs)
                return NodeSyncAuthor(name: author.name, email: author.email)
            },
            prefs: prefs,
            ledgerKey: scope.key(LocalNodeSyncApplier.appliedPrefsKey))
    }
}
