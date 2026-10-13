// The slice of FMCode's LocalRepoStore (LocalRepoStore, LocalRepoRecord) and GitWorkspace
// (GitWorkspace, GitAuthor, LocalFileEntry, LocalFileContent) that the node-sync applier uses, as
// protocols. FMChat cannot depend on FMCode, where the on-device Git port and the repository store
// live, so FMCode provides the adapter (FMCode/Local/LocalNodeSyncAdapter.swift):
//
//   LocalRepoStore -> LocalNodeSyncRepositories (NodeSyncRepositorySource)
//   GitWorkspace   -> LocalNodeSyncWorkspace    (NodeSyncWorkspace)
//
// Adapter contract (what the applier relies on):
// - `readFile` THROWS for a path that does not exist (the applier treats any throw as "absent").
//   A file that exists but is binary or too large is returned with `isBinary`/`isTooLarge` set.
// - `listFiles` lists the direct children of a repository-relative directory; it may throw or
//   return [] for a missing one.
// - `writeFile` creates parent directories.
// - `stage` stages exactly the given paths, including deletions of paths no longer on disk.
// - `commit` throws `NodeSyncWorkspaceError.nothingStaged` (or any error whose text contains
//   "Nothing is staged", the GitWorkerException message) when the index equals HEAD; the
//   applier counts that as success, as the desktop does.
// - Errors should describe themselves (`CustomStringConvertible`): their text is shown in the
//   node activity card.

import FMBase
import Foundation

/// FMCode's `GitAuthor`.
public struct NodeSyncAuthor: Equatable, Hashable, Sendable {
    public let name: String
    public let email: String

    public init(name: String, email: String) {
        self.name = name
        self.email = email
    }
}

/// The fields of FMCode's `LocalRepoRecord` the applier reads.
public struct NodeSyncRepoRecord: Equatable, Sendable {
    /// Slug; also the directory name under the store root.
    public let name: String
    /// Absolute worktree path (the workspace cache key).
    public let path: String
    /// `owner/name` on the network once published or cloned, else nil. The only thing an inbox
    /// item is matched on.
    public let publishedAs: String?

    public init(name: String, path: String, publishedAs: String?) {
        self.name = name
        self.path = path
        self.publishedAs = publishedAs
    }
}

/// FMCode's `LocalFileContent` (the fields the applier reads).
public struct NodeSyncFileContent: Equatable, Sendable {
    public let path: String
    /// UTF-8 text when `isBinary` and `isTooLarge` are both false.
    public let text: String
    public let isBinary: Bool
    public let isTooLarge: Bool

    public init(path: String, text: String, isBinary: Bool = false, isTooLarge: Bool = false) {
        self.path = path
        self.text = text
        self.isBinary = isBinary
        self.isTooLarge = isTooLarge
    }
}

/// FMCode's `LocalFileEntry` (the fields the applier reads).
public struct NodeSyncFileEntry: Equatable, Sendable {
    public let name: String
    /// Repository-relative path with `/` separators.
    public let path: String
    public let isDirectory: Bool

    public init(name: String, path: String, isDirectory: Bool) {
        self.name = name
        self.path = path
        self.isDirectory = isDirectory
    }
}

/// FMCode's `LocalRepoSummary` (the field the applier reads).
public struct NodeSyncWorkspaceSummary: Equatable, Sendable {
    /// The checked-out branch.
    public let branch: String

    public init(branch: String) { self.branch = branch }
}

/// Failures a workspace adapter may throw.
public enum NodeSyncWorkspaceError: Error, Equatable, CustomStringConvertible, Sendable {
    /// Nothing differs between the index and HEAD. Counted as success by the applier.
    case nothingStaged
    case notFound(String)
    case failed(String)

    public var description: String {
        switch self {
        case .nothingStaged: return "Nothing is staged. Stage changes first."
        case .notFound(let path): return "\(path) was not found"
        case .failed(let message): return message
        }
    }
}

/// One repository's worktree (FMCode's `GitWorkspace`).
@MainActor
public protocol NodeSyncWorkspace: AnyObject {
    func summary() async throws -> NodeSyncWorkspaceSummary
    /// Throws when `path` does not exist.
    func readFile(_ path: String) async throws -> NodeSyncFileContent
    func writeFile(_ path: String, _ text: String) async throws
    func deleteFile(_ path: String) async throws
    func listFiles(_ directory: String) async throws -> [NodeSyncFileEntry]
    func stage(_ paths: [String]) async throws
    /// Commits the index; returns the new commit id.
    func commit(message: String, author: NodeSyncAuthor) async throws -> String
    func dispose()
}

/// The on-device repositories (FMCode's `LocalRepoStore`).
@MainActor
public protocol NodeSyncRepositorySource: AnyObject {
    func ensureLoaded() async
    var records: [NodeSyncRepoRecord] { get }
    func workspace(for record: NodeSyncRepoRecord) -> NodeSyncWorkspace
    /// Re-reads one repository's summary after a commit (the store's index).
    func refresh(_ name: String) async throws
}

// MARK: - In memory

/// A `NodeSyncWorkspace` over a dictionary of files with a one-branch commit log: the applier's
/// tests, and previews. Staging snapshots the given paths; a commit whose index equals HEAD
/// throws `nothingStaged`, as the Git engine does.
@MainActor
public final class InMemoryNodeSyncWorkspace: NodeSyncWorkspace {
    public struct Commit: Equatable, Sendable {
        public let id: String
        public let message: String
        public let author: NodeSyncAuthor
    }

    /// Worktree files by repository-relative path.
    public var files: [String: String]
    /// Paths whose content reads back as binary.
    public var binaryPaths: Set<String> = []
    public var branch: String
    /// Newest first (like `git log`).
    public private(set) var commits: [Commit] = []
    public private(set) var disposed = false
    /// When set, the next `commit` throws it.
    public var commitError: Error?

    private var index: [String: String]
    private var head: [String: String]

    public init(files: [String: String] = [:], branch: String = "main") {
        self.files = files
        self.branch = branch
        self.index = files
        self.head = files
    }

    public func summary() async throws -> NodeSyncWorkspaceSummary { NodeSyncWorkspaceSummary(branch: branch) }

    public func readFile(_ path: String) async throws -> NodeSyncFileContent {
        guard let text = files[path] else { throw NodeSyncWorkspaceError.notFound(path) }
        if binaryPaths.contains(path) { return NodeSyncFileContent(path: path, text: "", isBinary: true) }
        return NodeSyncFileContent(path: path, text: text)
    }

    public func writeFile(_ path: String, _ text: String) async throws { files[path] = text }

    public func deleteFile(_ path: String) async throws {
        guard files.removeValue(forKey: path) != nil else { throw NodeSyncWorkspaceError.notFound(path) }
    }

    public func listFiles(_ directory: String) async throws -> [NodeSyncFileEntry] {
        let prefix = directory.isEmpty ? "" : directory + "/"
        var seen: [String: Bool] = [:]
        for path in files.keys where path.hasPrefix(prefix) {
            let rest = path.dropFirst(prefix.count)
            if let slash = rest.firstIndex(of: "/") {
                seen[String(rest[..<slash])] = true
            } else if seen[String(rest)] == nil {
                seen[String(rest)] = false
            }
        }
        if seen.isEmpty { throw NodeSyncWorkspaceError.notFound(directory) }
        return seen.keys.sorted().map { NodeSyncFileEntry(name: $0, path: prefix + $0, isDirectory: seen[$0] ?? false) }
    }

    public func stage(_ paths: [String]) async throws {
        for path in paths { index[path] = files[path] }
    }

    public func commit(message: String, author: NodeSyncAuthor) async throws -> String {
        if let commitError {
            self.commitError = nil
            throw commitError
        }
        if index == head { throw NodeSyncWorkspaceError.nothingStaged }
        head = index
        let id = String(format: "%040x", commits.count + 1)
        commits.insert(Commit(id: id, message: message, author: author), at: 0)
        return id
    }

    public func dispose() { disposed = true }
}

/// A `NodeSyncRepositorySource` over `InMemoryNodeSyncWorkspace`s.
@MainActor
public final class InMemoryNodeSyncRepositorySource: NodeSyncRepositorySource {
    public private(set) var records: [NodeSyncRepoRecord] = []
    public private(set) var workspaces: [String: InMemoryNodeSyncWorkspace] = [:]
    /// Names passed to `refresh`, oldest first.
    public private(set) var refreshed: [String] = []
    public var refreshError: Error?

    public init() {}

    /// Adds a repository (`LocalRepoStore.create` + `markPublished`); returns its workspace.
    @discardableResult
    public func add(name: String, publishedAs: String? = nil, branch: String = "main", files: [String: String] = [:])
        -> InMemoryNodeSyncWorkspace
    {
        let record = NodeSyncRepoRecord(name: name, path: "/repos/\(name)", publishedAs: publishedAs)
        records.removeAll { $0.name == name }
        records.append(record)
        let workspace = InMemoryNodeSyncWorkspace(files: files, branch: branch)
        workspaces[record.path] = workspace
        return workspace
    }

    public func ensureLoaded() async {}

    public func workspace(for record: NodeSyncRepoRecord) -> NodeSyncWorkspace {
        if let existing = workspaces[record.path] { return existing }
        let created = InMemoryNodeSyncWorkspace()
        workspaces[record.path] = created
        return created
    }

    public func refresh(_ name: String) async throws {
        refreshed.append(name)
        if let refreshError { throw refreshError }
    }
}
