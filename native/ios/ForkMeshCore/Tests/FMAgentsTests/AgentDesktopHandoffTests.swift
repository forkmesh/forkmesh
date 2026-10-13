import FMBase
import FMChat
import FMCode
import FMNet
import XCTest
@testable import FMAgents

@MainActor
final class FakeHandoffDesktop: AgentHandoffDesktop {
    var calls: [(owner: String, name: String, branch: String, base: String, task: String, title: String)] = []
    var failure: Error?

    var desktopName: String { "studio" }

    func handoff(owner: String, name: String, branch: String, base: String, task: String, title: String) async throws -> Int {
        if let failure { throw failure }
        calls.append((owner, name, branch, base, task, title))
        return 42
    }
}

@MainActor
final class AgentDesktopHandoffTests: XCTestCase {
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

    private func screen(
        script: [AgentTurn],
        publishedAs: String? = "alice/repo",
        desktop: AgentHandoffDesktop,
        pushed: Box<[String]>
    ) async throws -> (AgentScreenModel, GitWorkspace) {
        let git = GitWorkspace(worktree: temp.url.appendingPathComponent("repo", isDirectory: true))
        workspaces.append(git)
        try await git.initialize(files: ["a.txt": "one\n"], commitMessage: "base\n", author: GitAuthor(name: testAuthor.name, email: testAuthor.email))
        let record = LocalRepoRecord(name: "repo", path: git.worktree.path, createdAt: Date(), publishedAs: publishedAs)
        let service = PullRequestService(
            pushBranch: { _, _, _, branch in
                pushed.value.append(branch)
                return LocalPushResult(refs: [LocalPushRef(remoteRef: "refs/heads/\(branch)", newId: String(repeating: "a", count: 40), outcome: "updated")], packBytes: 1, objects: 1)
            },
            submitPull: { _, _, _, _, _, _ in })
        let store = AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient())
        store.write(AgentCredentials(kind: .anthropic, apiKey: "sk-ant-test-key-000000000000", model: "claude-opus-5")) // forkmesh-secret-scan:ignore-line
        let model = AgentScreenModel(
            repoName: "repo",
            workspace: GitAgentWorkspace(git),
            gitWorkspace: git,
            record: record,
            pullRequests: service,
            sessions: AgentSessionService(prefs: InMemoryKeyValueStore()),
            store: store,
            author: testAuthor,
            localModels: { nil },
            agentFactory: { toolset, _ in CodingAgent(provider: ScriptedProvider(script), toolset: toolset) },
            desktop: desktop
        )
        model.load()
        return (model, git)
    }

    func testAFinishedRunMovesToTheDesktopWithItsUncommittedWork() async throws {
        let desktop = FakeHandoffDesktop()
        let pushed = Box<[String]>([])
        let (model, git) = try await screen(script: [
            toolTurn("write_file", ["path": "a.txt", "content": "two\n"], id: "1"),
            AgentTurn(text: "Half way there.", toolCalls: []),
        ], desktop: desktop, pushed: pushed)
        XCTAssertFalse(model.canMoveToDesktop)
        model.draft = "change a.txt to two"
        _ = await model.send()
        await model.handle?.waitUntilDone()
        XCTAssertTrue(model.canMoveToDesktop)

        let move = await model.moveToDesktop()
        let moved = try XCTUnwrap(move, model.error ?? "")
        let branch = try XCTUnwrap(model.handle?.branch)
        XCTAssertEqual(moved, AgentDesktopMove(agentId: 42, desktopName: "studio", branch: branch))
        XCTAssertEqual(moved.message, "Continuing on studio as agent #42, on \(branch).")
        XCTAssertEqual(pushed.value, [branch])
        let call = try XCTUnwrap(desktop.calls.first)
        XCTAssertEqual(call.owner, "alice")
        XCTAssertEqual(call.name, "repo")
        XCTAssertEqual(call.branch, branch)
        XCTAssertEqual(call.base, "main")
        XCTAssertEqual(call.title, "change a.txt to two")
        XCTAssertTrue(call.task.contains("branch \(branch) (started from main)"), call.task)
        XCTAssertTrue(call.task.contains("Task:\nchange a.txt to two"), call.task)
        XCTAssertTrue(call.task.contains("Tool: write_file a.txt"), call.task)
        XCTAssertTrue(call.task.contains("Agent: Half way there."), call.task)
        XCTAssertFalse(call.task.contains("Me: change a.txt to two"), call.task)

        let status = try await git.status()
        XCTAssertTrue(status.isClean)
        let branches = try await git.branches()
        XCTAssertEqual(branches.first(where: \.isCurrent)?.name, "main")
        XCTAssertFalse(model.canMoveToDesktop)
    }

    func testAnOldDesktopKeepsTheAgentOnThePhoneAndSaysToUpdate() async throws {
        let desktop = FakeHandoffDesktop()
        desktop.failure = LocalControlException("unsupported")
        let pushed = Box<[String]>([])
        let (model, _) = try await screen(script: [AgentTurn(text: "Done.", toolCalls: [])], desktop: desktop, pushed: pushed)
        model.draft = "anything"
        _ = await model.send()
        await model.handle?.waitUntilDone()
        let move = await model.moveToDesktop()
        XCTAssertNil(move)
        XCTAssertEqual(model.error, "Update ForkMesh on the desktop to move agents to it.")
        XCTAssertTrue(model.canMoveToDesktop)
    }

    func testARepositoryOnlyOnThisPhoneCannotMove() async throws {
        let desktop = FakeHandoffDesktop()
        let pushed = Box<[String]>([])
        let (model, _) = try await screen(script: [AgentTurn(text: "Done.", toolCalls: [])], publishedAs: nil, desktop: desktop, pushed: pushed)
        model.draft = "anything"
        _ = await model.send()
        await model.handle?.waitUntilDone()
        XCTAssertFalse(model.canMoveToDesktop)
        let move = await model.moveToDesktop()
        XCTAssertNil(move)
        XCTAssertTrue(pushed.value.isEmpty)
    }

    func testALongConversationKeepsItsNewestStepsWithinTheDesktopLimit() async {
        let events: [AgentEvent] = [.userMessage("fix it")] + (0..<200).map { .assistantMessage("step \($0) " + String(repeating: "x", count: 200)) }
        let task = AgentDesktopHandoff.task(prompt: "fix it", events: events, branch: "agent/abc-fix", base: "main")
        XCTAssertLessThanOrEqual(task.utf16.count, AgentDesktopHandoff.maxTaskUnits)
        XCTAssertTrue(task.contains("… earlier steps omitted"))
        XCTAssertTrue(task.contains("Agent: step 199 "))
        XCTAssertFalse(task.contains("Agent: step 0 "))
    }

    func testAFollowUpInstructionIsKeptAndTheTitleIsTheFirstLine() async {
        let events: [AgentEvent] = [.userMessage("fix it"), .assistantMessage("ok"), .userMessage("also the tests")]
        let task = AgentDesktopHandoff.task(prompt: "fix it", events: events, branch: "agent/abc-fix", base: "main")
        XCTAssertTrue(task.hasSuffix("Agent: ok\nMe: also the tests"), task)
        XCTAssertEqual(AgentDesktopHandoff.title(prompt: "Fix login\nmore detail"), "Fix login")
    }
}
