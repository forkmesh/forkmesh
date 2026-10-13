// FMCode's on-device Git (`GitWorkspace`) as the coding agent's
// `AgentWorkspace`: the one place the two type families meet.

import FMCode
import Foundation

@MainActor
public final class GitAgentWorkspace: AgentWorkspace {
    /// Shared with the repository screen, which owns (and disposes) it.
    public let workspace: GitWorkspace

    public init(_ workspace: GitWorkspace) {
        self.workspace = workspace
    }

    public var worktree: URL { workspace.worktree }

    public func listFiles(_ directory: String) async throws -> [AgentFileEntry] {
        try await mapped {
            try await workspace.listFiles(directory).map { entry in
                AgentFileEntry(
                    name: entry.name,
                    path: entry.path,
                    kind: AgentFileKind(rawValue: entry.kind.rawValue) ?? .file,
                    size: entry.size,
                    tracked: entry.tracked
                )
            }
        }
    }

    public func readFile(_ path: String) async throws -> AgentFileContent {
        try await mapped {
            let file = try await workspace.readFile(path)
            return AgentFileContent(path: file.path, bytes: file.bytes, text: file.text, isBinary: file.isBinary, isTooLarge: file.isTooLarge)
        }
    }

    public func writeFile(_ path: String, _ text: String) async throws {
        try await mapped { try await workspace.writeFile(path, text) }
    }

    public func status() async throws -> AgentWorktreeStatus {
        try await mapped {
            let status = try await workspace.status()
            func changes(_ list: [LocalChange]) -> [AgentChange] {
                list.map { AgentChange(path: $0.path, kind: AgentChangeKind(rawValue: $0.kind.rawValue) ?? .modified) }
            }
            return AgentWorktreeStatus(
                staged: changes(status.staged),
                unstaged: changes(status.unstaged),
                untracked: changes(status.untracked)
            )
        }
    }

    public func stageAll() async throws {
        try await mapped { try await workspace.stageAll() }
    }

    public func commit(message: String, author: AgentAuthor) async throws -> String {
        try await mapped { try await workspace.commit(message: message, author: GitAuthor(name: author.name, email: author.email)) }
    }

    public func branches() async throws -> [AgentBranch] {
        try await mapped { try await workspace.branches().map { AgentBranch(name: $0.name, isCurrent: $0.isCurrent) } }
    }

    public func createBranch(_ name: String, checkout: Bool) async throws {
        try await mapped { try await workspace.createBranch(name, checkout: checkout) }
    }

    public func checkout(_ name: String) async throws {
        try await mapped { try await workspace.checkout(name) }
    }

    /// `GitWorkspaceException` carries the sentence the model should read.
    private func mapped<T>(_ body: () async throws -> T) async throws -> T {
        do {
            return try await body()
        } catch let error as GitWorkspaceException {
            throw AgentWorkspaceError(error.message)
        }
    }
}

extension AgentAuthor {
    /// The commit identity FMCode builds from the account
    /// (`GitAuthor.fromAccount`).
    public init(_ author: GitAuthor) {
        self.init(name: author.name, email: author.email)
    }

    /// `GitAuthor.fromAccount(nodeName:email:)`, as the agent's author.
    public static func fromAccount(nodeName: String, email: String = "") -> AgentAuthor {
        AgentAuthor(GitAuthor.fromAccount(nodeName: nodeName, email: email))
    }
}
