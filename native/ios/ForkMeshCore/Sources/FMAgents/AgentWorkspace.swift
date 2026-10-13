// The part of the Git workspace the coding agent uses, as a protocol.
//
// The tools and the session service talk to this protocol instead of
// `GitWorkspace`, so the loop is tested without a Git engine, and FMCode's
// `GitWorkspace` reaches it through one adapter (`GitAgentWorkspace`,
// GitAgentWorkspace.swift). The value types mirror FMCode's field for field;
// they carry an `Agent` prefix so a view that
// imports FMCode and FMAgents never sees two `LocalFileContent`s.

import Foundation

/// Commit identity.
public struct AgentAuthor: Equatable, Sendable {
    public let name: String
    public let email: String

    public init(name: String, email: String) {
        self.name = name
        self.email = email
    }

    /// What commits carry until the signed-in account is known.
    public static let placeholder = AgentAuthor(name: "forkmesh", email: "forkmesh@mobile.forkmesh")
}

public enum AgentFileKind: String, Sendable {
    case file, directory, symlink
}

/// One directory entry.
public struct AgentFileEntry: Equatable, Sendable {
    public let name: String
    /// Repository-relative path.
    public let path: String
    public let kind: AgentFileKind
    public let size: Int
    /// Whether the path is in the index.
    public let tracked: Bool

    public init(name: String, path: String, kind: AgentFileKind, size: Int = 0, tracked: Bool = true) {
        self.name = name
        self.path = path
        self.kind = kind
        self.size = size
        self.tracked = tracked
    }

    public var isDirectory: Bool { kind == .directory }
}

/// A file read for editing.
public struct AgentFileContent: Equatable, Sendable {
    public let path: String
    public let bytes: Int
    /// The text, or "" when binary or too large.
    public let text: String
    public let isBinary: Bool
    public let isTooLarge: Bool

    public init(path: String, bytes: Int, text: String, isBinary: Bool = false, isTooLarge: Bool = false) {
        self.path = path
        self.bytes = bytes
        self.text = text
        self.isBinary = isBinary
        self.isTooLarge = isTooLarge
    }

    public var isEditable: Bool { !isBinary && !isTooLarge }
}

/// `rawValue` is the name the status tool prints.
public enum AgentChangeKind: String, Sendable {
    case added, modified, deleted, untracked
}

public struct AgentChange: Equatable, Sendable {
    public let path: String
    public let kind: AgentChangeKind

    public init(path: String, kind: AgentChangeKind) {
        self.path = path
        self.kind = kind
    }
}

public struct AgentWorktreeStatus: Equatable, Sendable {
    public let staged: [AgentChange]
    public let unstaged: [AgentChange]
    public let untracked: [AgentChange]

    public init(staged: [AgentChange] = [], unstaged: [AgentChange] = [], untracked: [AgentChange] = []) {
        self.staged = staged
        self.unstaged = unstaged
        self.untracked = untracked
    }

    public var isClean: Bool { staged.isEmpty && unstaged.isEmpty && untracked.isEmpty }
    public var changeCount: Int { staged.count + unstaged.count + untracked.count }
}

public struct AgentBranch: Equatable, Sendable {
    public let name: String
    public let isCurrent: Bool

    public init(name: String, isCurrent: Bool) {
        self.name = name
        self.isCurrent = isCurrent
    }
}

/// A workspace failure with a message fit for the model: the tools report it as
/// "<tool> failed: <message>".
public struct AgentWorkspaceError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { message }
}

/// The on-device repository an agent session works in. Paths are
/// repository-relative; the implementation enforces the path policy (no
/// escaping the worktree, no writing inside `.git`).
@MainActor
public protocol AgentWorkspace: AnyObject {
    /// Direct children of `directory` ("" is the root).
    func listFiles(_ directory: String) async throws -> [AgentFileEntry]
    func readFile(_ path: String) async throws -> AgentFileContent
    func writeFile(_ path: String, _ text: String) async throws
    func status() async throws -> AgentWorktreeStatus
    func stageAll() async throws
    /// Commits the index; returns the new commit id.
    func commit(message: String, author: AgentAuthor) async throws -> String
    func branches() async throws -> [AgentBranch]
    func createBranch(_ name: String, checkout: Bool) async throws
    func checkout(_ name: String) async throws
}

extension AgentWorkspace {
    /// The checked-out branch, "" when HEAD is unborn or detached.
    public func currentBranch() async throws -> String {
        try await branches().filter(\.isCurrent).map(\.name).joined()
    }
}
