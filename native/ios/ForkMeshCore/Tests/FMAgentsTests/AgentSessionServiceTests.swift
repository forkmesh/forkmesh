import FMAccount
import FMBase
import FMNet
import XCTest
@testable import FMAgents

@MainActor
final class AgentSessionServiceTests: XCTestCase {
    private func agentFrom(_ provider: ScriptedProvider) -> @MainActor (AgentToolset) -> CodingAgent {
        { toolset in CodingAgent(provider: provider, toolset: toolset) }
    }

    private func current(_ workspace: MemoryWorkspace) async throws -> String {
        try await workspace.currentBranch()
    }

    private func seeded(_ records: [JSONValue]) -> InMemoryKeyValueStore {
        InMemoryKeyValueStore([AgentSessionService.recordsKey: .string(JSON.encode(.array(records)))])
    }

    private func record(_ id: String, branch: String, base: String = "main", daysAgo: Double) -> JSONValue {
        [
            "sessionId": .string(id),
            "repoName": "repo",
            "branch": .string(branch),
            "baseBranch": .string(base),
            "startedAt": .string(DartDateFormat.iso8601(Date().addingTimeInterval(-daysAgo * 24 * 3600))),
        ]
    }

    // MARK: - Branch isolation

    func testARunHappensOnItsOwnBranchAndRestoresTheBaseAfter() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let base = try await current(workspace)

        let provider = ScriptedProvider([
            toolTurn("write_file", ["path": "a.txt", "content": "two\n"], id: "1"),
            toolTurn("git_commit", ["message": "Update a.txt"], id: "2"),
            AgentTurn(text: "Done.", toolCalls: []),
        ])

        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "update a.txt"))
        let handle = try await service.start(task: task, workspace: workspace, buildAgent: agentFrom(provider), author: testAuthor)
        XCTAssertTrue(handle.branch.hasPrefix("agent/"))
        XCTAssertEqual(handle.baseBranch, "main")
        XCTAssertTrue(service.activeRun === handle)
        await handle.waitUntilDone()
        let events = handle.events

        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.completed])
        XCTAssertEqual(events.first?.noticeText, "Working on branch \(handle.branch).")
        XCTAssertEqual(events.last?.noticeText, "Back on main. The work is on \(handle.branch).")

        // The edit landed on the agent branch, and the user is back on base.
        let after = try await current(workspace)
        XCTAssertEqual(after, base)
        let branches = try await workspace.branches()
        let agentBranch = try XCTUnwrap(branches.first { $0.name.hasPrefix("agent/") })

        // Base is untouched; the change lives on the agent branch.
        let onBase = try await workspace.readFile("a.txt")
        XCTAssertEqual(onBase.text, "one\n")
        try await workspace.checkout(agentBranch.name)
        let onAgent = try await workspace.readFile("a.txt")
        XCTAssertEqual(onAgent.text, "two\n")

        XCTAssertEqual(service.queue.first?.state, .done)
        XCTAssertNil(service.activeRun)
        XCTAssertTrue(service.lastRun === handle)
        XCTAssertFalse(service.isRunning)
        XCTAssertTrue(service.records.first?.isFinished ?? false)
    }

    func testADirtyWorktreeRefusesToStartAndExplainsWhy() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        try await workspace.writeFile("a.txt", "edited by hand\n")
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "do something"))

        do {
            _ = try await service.start(task: task, workspace: workspace, buildAgent: agentFrom(ScriptedProvider([])), author: testAuthor)
            XCTFail("expected a refusal")
        } catch let error as AgentStartException {
            XCTAssertEqual(error.refusal, .dirtyWorktree)
            XCTAssertTrue(error.message.contains("uncommitted"))
            XCTAssertTrue(error.message.hasPrefix("This repository has 1 uncommitted change."))
        }

        // The user's edit is untouched and no branch was taken.
        let file = try await workspace.readFile("a.txt")
        XCTAssertEqual(file.text, "edited by hand\n")
        let branches = try await workspace.branches()
        XCTAssertTrue(branches.filter { $0.name.hasPrefix("agent/") }.isEmpty)
    }

    func testARunThatStopsMidEditLeavesTheWorkAndSaysWhereItIs() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())

        // Writes a file but never commits, then finishes.
        let provider = ScriptedProvider([
            toolTurn("write_file", ["path": "a.txt", "content": "half done\n"], id: "1"),
            AgentTurn(text: "I stopped early.", toolCalls: []),
        ])

        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "half a job"))
        let handle = try await service.start(task: task, workspace: workspace, buildAgent: agentFrom(provider), author: testAuthor)
        await handle.waitUntilDone()

        let notices = handle.events.compactMap(\.noticeText)
        XCTAssertTrue(notices.last?.contains("uncommitted changes") ?? false)
        // Still on the agent branch: switching away would strand the work.
        let branch = try await current(workspace)
        XCTAssertTrue(branch.hasPrefix("agent/"))
        let file = try await workspace.readFile("a.txt")
        XCTAssertEqual(file.text, "half done\n")
    }

    func testASecondSessionIsRefusedWhileOneIsRunning() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let provider = ScriptedProvider([AgentTurn(text: "done", toolCalls: [])])

        let first = try XCTUnwrap(service.submit(repoName: "repo", prompt: "first"))
        let firstHandle = try await service.start(task: first, workspace: workspace, buildAgent: agentFrom(provider), author: testAuthor)

        // The run (and its cleanup) is still in flight: a second start must be
        // refused rather than racing the same worktree.
        XCTAssertFalse(firstHandle.isComplete)
        let second = AgentTask(id: "second", repoName: "repo", prompt: "second")
        do {
            _ = try await service.start(task: second, workspace: workspace, buildAgent: agentFrom(provider), author: testAuthor)
            XCTFail("expected a refusal")
        } catch let error as AgentStartException {
            XCTAssertEqual(error.refusal, .alreadyRunning)
        }
        await firstHandle.waitUntilDone()
    }

    func testAProviderErrorMarksTheTaskFailedAndStillRestoresTheBase() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let provider = ScriptedProvider([])
        provider.failures = [AgentProviderException("Claude API error 401: invalid api key", statusCode: 401)]

        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "anything"))
        let handle = try await service.start(task: task, workspace: workspace, buildAgent: agentFrom(provider), author: testAuthor)
        await handle.waitUntilDone()

        XCTAssertEqual(handle.lastDone?.reason, .error)
        XCTAssertEqual(service.queue.first?.state, .failed)
        let branch = try await current(workspace)
        XCTAssertEqual(branch, "main")
    }

    func testCancelActiveStopsTheRunAtASafePoint() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let provider = ScriptedProvider((0..<10).map { toolTurn("git_status", id: "t\($0)") })
        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "loop"))
        let cancelled = Box(false)
        service.addListener {
            MainActor.assumeIsolated {
                if !cancelled.value, service.lastRun?.events.contains(where: { $0.toolCall != nil }) ?? false {
                    cancelled.value = true
                    service.cancelActive()
                }
            }
        }
        let handle = try await service.start(task: task, workspace: workspace, buildAgent: agentFrom(provider), author: testAuthor)
        await handle.waitUntilDone()
        XCTAssertEqual(handle.lastDone?.reason, .cancelled)
        XCTAssertEqual(service.queue.first?.state, .done)
    }

    // MARK: - Task queue

    func testHighPriorityRunsBeforeNormalOldestFirstWithinAPriority() async {
        let clock = FixedClock(epochMillis: 1_000)
        let service = AgentSessionService(prefs: InMemoryKeyValueStore(), clock: clock)
        service.submit(repoName: "r", prompt: "normal one", id: "n1")
        clock.advance(by: 1)
        service.submit(repoName: "r", prompt: "normal two", id: "n2")
        clock.advance(by: 1)
        service.submit(repoName: "r", prompt: "urgent", priority: .high, id: "h1")

        XCTAssertEqual(service.nextTask?.id, "h1")
        service.removeTask("h1")
        XCTAssertEqual(service.nextTask?.id, "n1")
        service.removeTask("n1")
        XCTAssertEqual(service.nextTask?.id, "n2")
        service.removeTask("n2")
        XCTAssertNil(service.nextTask)
    }

    func testAMessageDuringARunSteersInsteadOfQueueingBehindIt() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let provider = ScriptedProvider([
            toolTurn("git_status", id: "1"),
            AgentTurn(text: "first answer", toolCalls: []),
            AgentTurn(text: "second answer", toolCalls: []),
        ])

        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "check status"))
        let steered = Box(false)
        let queuedWhileRunning = Box<AgentTask?>(nil)
        service.addListener {
            MainActor.assumeIsolated {
                // Steer the moment the tool call shows up in the transcript.
                guard !steered.value, let handle = service.lastRun else { return }
                if handle.events.contains(where: { $0.toolCall != nil }) {
                    steered.value = true
                    queuedWhileRunning.value = service.submit(repoName: "repo", prompt: "also check the branch")
                }
            }
        }
        let handle = try await service.start(task: task, workspace: workspace, buildAgent: agentFrom(provider), author: testAuthor)
        await handle.waitUntilDone()

        XCTAssertTrue(steered.value)
        XCTAssertNil(queuedWhileRunning.value, "should steer the running session")
        XCTAssertTrue(handle.events.compactMap(\.userText).contains("also check the branch"))
        // Nothing extra was left sitting in the queue.
        XCTAssertTrue(service.queue.filter { $0.state == .queued }.isEmpty)
    }

    func testAMessageForADifferentRepositoryStillQueues() async {
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        XCTAssertNotNil(service.submit(repoName: "other", prompt: "do a thing"))
        XCTAssertEqual(service.queue.count, 1)
        XCTAssertNil(service.submit(repoName: "other", prompt: "   "))
        XCTAssertEqual(service.queue.count, 1)
    }

    // MARK: - Stale sessions

    func testAnUnfinishedSessionPastTheWindowIsReleased() async {
        // Interrupted three weeks ago and never finished.
        let prefs = seeded([record("old", branch: "agent/old-thing", daysAgo: 21)])
        let service = AgentSessionService(prefs: prefs)
        service.load()
        XCTAssertEqual(service.records.count, 1)

        let released = await service.sweepStaleRecords()
        XCTAssertEqual(released.map(\.branch), ["agent/old-thing"])
        XCTAssertTrue(service.records.isEmpty)

        // The release is persisted, so it does not resurrect on next launch.
        let reloaded = AgentSessionService(prefs: prefs)
        reloaded.load()
        XCTAssertTrue(reloaded.records.isEmpty)
    }

    func testARecentUnfinishedSessionIsLeftAlone() async {
        let service = AgentSessionService(prefs: seeded([record("recent", branch: "agent/recent", daysAgo: 2)]))
        service.load()
        let released = await service.sweepStaleRecords()
        XCTAssertTrue(released.isEmpty)
        XCTAssertEqual(service.records.count, 1)
    }

    func testAStaleSessionParkedOnItsBranchIsReturnedToBase() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let base = try await current(workspace)
        try await workspace.createBranch("agent/stuck", checkout: true)
        let parked = try await current(workspace)
        XCTAssertEqual(parked, "agent/stuck")

        let service = AgentSessionService(prefs: seeded([record("stuck", branch: "agent/stuck", base: base, daysAgo: 30)]))
        service.load()

        await service.sweepStaleRecords(workspace: workspace, repoName: "repo")

        // The repository is usable again, and the work is still on the branch.
        let restored = try await current(workspace)
        XCTAssertEqual(restored, base)
        let branches = try await workspace.branches()
        XCTAssertTrue(branches.contains { $0.name == "agent/stuck" })
    }

    func testStartingARunSweepsStaleRecordsFirst() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: seeded([record("ancient", branch: "agent/ancient", daysAgo: 40)]))
        service.load()

        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "new work"))
        let handle = try await service.start(
            task: task,
            workspace: workspace,
            buildAgent: agentFrom(ScriptedProvider([AgentTurn(text: "done", toolCalls: [])])),
            author: testAuthor
        )
        await handle.waitUntilDone()

        // The ancient record is gone; only this run's finished record remains.
        XCTAssertFalse(service.records.contains { $0.sessionId == "ancient" })
        XCTAssertEqual(service.records.count, 1)
        XCTAssertTrue(service.records.first?.isFinished ?? false)
    }

    func testRecordsRoundTripInTheDartShape() async throws {
        let started = Date(epochMillis: 1_700_000_000_123)
        let record = AgentBranchRecord(sessionId: "s", repoName: "r", branch: "agent/x", baseBranch: "main", startedAt: started)
        XCTAssertEqual(record.json.keys, ["sessionId", "repoName", "branch", "baseBranch", "startedAt"])
        let back = try XCTUnwrap(AgentBranchRecord(json: .object(record.json)))
        XCTAssertEqual(back, record)
        XCTAssertNil(AgentBranchRecord(json: ["branch": "b"]), "no start date")
        XCTAssertNil(AgentBranchRecord(json: ["startedAt": "2026-01-01T00:00:00.000"]), "no branch")
        XCTAssertEqual(AgentBranchRecord(json: ["startedAt": "2026-01-01T00:00:00.000", "branch": "b"])?.baseBranch, "main")
        XCTAssertTrue(AgentSessionService.decodeRecords("not json").isEmpty)
    }

    // MARK: - Branch names

    func testBranchNamesAreReadableScopedAndBounded() async {
        let name = AgentSessionService.agentBranchName("task-1712345", "Please fix the login redirect on Android")
        XCTAssertTrue(name.hasPrefix("agent/"))
        XCTAssertTrue(name.contains("fix"))
        XCTAssertTrue(name.contains("login"))
        // Filler words are dropped and the name stays short.
        XCTAssertFalse(name.contains("please"))
        XCTAssertLessThan(name.count, 50)
        XCTAssertEqual(name, "agent/712345-fix-login-redirect-android")
    }

    func testAPromptlessTaskStillProducesAValidBranch() async {
        XCTAssertEqual(AgentSessionService.agentBranchName("abc", "?????"), "agent/abc-task")
        XCTAssertEqual(
            AgentSessionService.agentBranchName("t", "Refactor everything: extraordinarily-long-identifiers everywhere now"),
            "agent/t-refactor-everything-extraordinar"
        )
    }
}
