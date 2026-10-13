// The agent's tools and sessions against a real repository, on FMCode's GitWorkspace
// through GitAgentWorkspace.

import FMBase
import FMCode
import XCTest
@testable import FMAgents

@MainActor
final class GitAgentWorkspaceTests: XCTestCase {
    private var temp: TempDirectory!
    private var workspaces: [GitWorkspace] = []

    override func setUp() async throws {
        temp = TempDirectory()
    }

    override func tearDown() async throws {
        for workspace in workspaces { workspace.dispose() }
        workspaces.removeAll()
        temp.cleanUp()
    }

    private func newRepo(_ files: [String: String]) async throws -> GitAgentWorkspace {
        let workspace = GitWorkspace(worktree: temp.url.appendingPathComponent("repo", isDirectory: true))
        workspaces.append(workspace)
        try await workspace.initialize(files: files, commitMessage: "Initial commit\n", author: GitAuthor(name: testAuthor.name, email: testAuthor.email))
        return GitAgentWorkspace(workspace)
    }

    func testToolsReadWriteGrepAndCommitARealRepository() async throws {
        let workspace = try await newRepo(["a.dart": "final answer = 42;\n", "lib/b.dart": "void answer() {}\n"])
        let tools = AgentToolset(workspace)
        tools.author = testAuthor

        let read = await tools.invoke("1", "read_file", ["path": "a.dart"])
        XCTAssertEqual(read.output, "1  final answer = 42;\n")

        let hit = await tools.invoke("2", "grep", ["pattern": "answer"])
        XCTAssertTrue(hit.output.contains("a.dart:1"))
        XCTAssertTrue(hit.output.contains("lib/b.dart:1"))

        let listed = await tools.invoke("3", "list_directory", [:])
        XCTAssertTrue(listed.output.hasPrefix("lib/\n"))

        let clean = await tools.invoke("4", "git_status", [:])
        XCTAssertTrue(clean.output.contains("Clean"))

        _ = await tools.invoke("5", "replace_text", ["path": "a.dart", "old": "42", "new": "43"])
        let dirty = await tools.invoke("6", "git_status", [:])
        XCTAssertTrue(dirty.output.contains("a.dart"))

        let committed = await tools.invoke("7", "git_commit", ["message": "Bump the answer"])
        XCTAssertFalse(committed.isError)
        XCTAssertTrue(committed.output.hasSuffix(": Bump the answer"))
        let after = await tools.invoke("8", "git_status", [:])
        XCTAssertTrue(after.output.contains("Clean"))
        let again = await tools.invoke("9", "git_commit", ["message": "noop"])
        XCTAssertTrue(again.output.contains("Nothing to commit"))

        let log = try await workspace.workspace.log(limit: 5)
        XCTAssertEqual(log.first?.message.dartTrimmed, "Bump the answer")
        XCTAssertEqual(log.first?.authorEmail, testAuthor.email)
    }

    func testThePathPolicyIsReportedToTheModel() async throws {
        let workspace = try await newRepo(["a.txt": "x\n"])
        let tools = AgentToolset(workspace)
        let escape = await tools.invoke("1", "write_file", ["path": "../outside.txt", "content": "x"])
        XCTAssertTrue(escape.isError)
        XCTAssertTrue(escape.output.hasPrefix("write_file failed: "))
    }

    func testARunHappensOnItsOwnBranchAndRestoresTheBaseAfter() async throws {
        let workspace = try await newRepo(["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let base = try await workspace.currentBranch()
        let provider = ScriptedProvider([
            toolTurn("write_file", ["path": "a.txt", "content": "two\n"], id: "1"),
            toolTurn("git_commit", ["message": "Update a.txt"], id: "2"),
            AgentTurn(text: "Done.", toolCalls: []),
        ])
        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "update a.txt"))
        let handle = try await service.start(
            task: task,
            workspace: workspace,
            buildAgent: { toolset in CodingAgent(provider: provider, toolset: toolset) },
            author: testAuthor
        )
        await handle.waitUntilDone()

        XCTAssertEqual(handle.lastDone?.reason, .completed)
        let current = try await workspace.currentBranch()
        XCTAssertEqual(current, base)
        let onBase = try await workspace.readFile("a.txt")
        XCTAssertEqual(onBase.text, "one\n")
        try await workspace.checkout(handle.branch)
        let onAgent = try await workspace.readFile("a.txt")
        XCTAssertEqual(onAgent.text, "two\n")
    }

    func testADirtyRepositoryRefusesToStart() async throws {
        let workspace = try await newRepo(["a.txt": "one\n"])
        try await workspace.writeFile("a.txt", "edited by hand\n")
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "do something"))
        do {
            _ = try await service.start(
                task: task,
                workspace: workspace,
                buildAgent: { toolset in CodingAgent(provider: ScriptedProvider([]), toolset: toolset) },
                author: testAuthor
            )
            XCTFail("expected a refusal")
        } catch let error as AgentStartException {
            XCTAssertEqual(error.refusal, .dirtyWorktree)
        }
        let branches = try await workspace.branches()
        XCTAssertFalse(branches.contains { $0.name.hasPrefix("agent/") })
    }
}
