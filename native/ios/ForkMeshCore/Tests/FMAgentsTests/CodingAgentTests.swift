// The coding agent's tools and loop; the wire formats are in AgentProviderTests.

import FMBase
import XCTest
@testable import FMAgents

@MainActor
final class AgentToolsTests: XCTestCase {
    func testReadFileReturnsNumberedLinesAnEditCanReferTo() async {
        let tools = AgentToolset(MemoryWorkspace(files: ["a.txt": "alpha\nbeta\n"]))
        let result = await tools.invoke("1", "read_file", ["path": "a.txt"])
        XCTAssertFalse(result.isError)
        XCTAssertTrue(result.output.contains("1  alpha"))
        XCTAssertTrue(result.output.contains("2  beta"))
        XCTAssertEqual(result.output, "1  alpha\n2  beta\n")
    }

    func testLineNumbersArePaddedToTheWidestNumber() async {
        let text = (1...12).map { "line \($0)" }.joined(separator: "\n")
        let tools = AgentToolset(MemoryWorkspace(files: ["a.txt": text]))
        let result = await tools.invoke("1", "read_file", ["path": "a.txt"])
        XCTAssertTrue(result.output.hasPrefix(" 1  line 1\n"))
        XCTAssertTrue(result.output.contains("12  line 12\n"))
    }

    func testWriteFileThenReadFileRoundTrips() async {
        let tools = AgentToolset(MemoryWorkspace(files: ["a.txt": "x\n"]))
        let wrote = await tools.invoke("1", "write_file", ["path": "lib/new.dart", "content": "void main() {}\n"])
        XCTAssertFalse(wrote.isError)
        XCTAssertEqual(wrote.output, "Wrote lib/new.dart (1 lines).")
        let read = await tools.invoke("2", "read_file", ["path": "lib/new.dart"])
        XCTAssertTrue(read.output.contains("void main() {}"))
    }

    func testReplaceTextEditsAUniqueSnippet() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "keep\nchange me\nkeep\n"])
        let tools = AgentToolset(workspace)
        let result = await tools.invoke("1", "replace_text", ["path": "a.txt", "old": "change me", "new": "changed"])
        XCTAssertFalse(result.isError)
        XCTAssertEqual(result.output, "Edited a.txt.")
        let file = try await workspace.readFile("a.txt")
        XCTAssertEqual(file.text, "keep\nchanged\nkeep\n")
    }

    func testReplaceTextExplainsAMissInsteadOfGuessing() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\ntwo\n"])
        let tools = AgentToolset(workspace)

        let absent = await tools.invoke("1", "replace_text", ["path": "a.txt", "old": "nowhere", "new": "x"])
        XCTAssertTrue(absent.output.contains("does not appear"))
        // An unmatched edit must not touch the file.
        let unchanged = try await workspace.readFile("a.txt")
        XCTAssertEqual(unchanged.text, "one\ntwo\n")

        try await workspace.writeFile("dup.txt", "same\nsame\n")
        let ambiguous = await tools.invoke("2", "replace_text", ["path": "dup.txt", "old": "same", "new": "x"])
        XCTAssertTrue(ambiguous.output.contains("appears 2 times"))
        let dup = try await workspace.readFile("dup.txt")
        XCTAssertEqual(dup.text, "same\nsame\n")

        let empty = await tools.invoke("3", "replace_text", ["path": "a.txt", "old": "", "new": "x"])
        XCTAssertTrue(empty.output.contains("Use write_file"))
    }

    func testGrepFindsMatchingLinesWithPathsAndReportsMisses() async {
        let tools = AgentToolset(MemoryWorkspace(files: [
            "a.dart": "final answer = 42;\n",
            "lib/b.dart": "void answer() {}\n",
        ]))

        let hit = await tools.invoke("1", "grep", ["pattern": "answer"])
        XCTAssertTrue(hit.output.contains("a.dart:1"))
        XCTAssertTrue(hit.output.contains("lib/b.dart:1: void answer() {}"))

        let miss = await tools.invoke("2", "grep", ["pattern": "zzz-none"])
        XCTAssertTrue(miss.output.contains("No matches"))
        XCTAssertEqual(miss.output, "No matches for \"zzz-none\" in 2 files.")

        // A bad regex is a message, not a crash.
        let bad = await tools.invoke("3", "grep", ["pattern": "("])
        XCTAssertTrue(bad.output.lowercased().contains("regular expression"))

        // A literal search escapes the pattern.
        let literal = await tools.invoke("4", "grep", ["pattern": "answer()", "regex": false])
        XCTAssertEqual(literal.output, "lib/b.dart:1: void answer() {}")
    }

    func testGrepStopsAtTheMatchCap() async {
        let lines = (0..<100).map { "hit \($0)" }.joined(separator: "\n")
        let tools = AgentToolset(MemoryWorkspace(files: ["many.txt": lines]))
        let result = await tools.invoke("1", "grep", ["pattern": "hit"])
        XCTAssertTrue(result.output.hasSuffix("… more matches were not listed; narrow the pattern or path."))
        XCTAssertEqual(result.output.components(separatedBy: "\n").count, AgentToolset.maxGrepMatches + 1)
    }

    func testListDirectoryPutsDirectoriesFirst() async {
        let tools = AgentToolset(MemoryWorkspace(files: ["b.txt": "bb", "a.txt": "a", "lib/x.dart": "x"]))
        let result = await tools.invoke("1", "list_directory", ["path": "."])
        XCTAssertEqual(result.output, "lib/\na.txt  (1 bytes)\nb.txt  (2 bytes)")
        let empty = await AgentToolset(MemoryWorkspace(files: [:])).invoke("2", "list_directory", [:])
        XCTAssertEqual(empty.output, "The repository is empty.")
    }

    func testGitStatusAndGitCommitReflectRealRepositoryState() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let tools = AgentToolset(workspace)
        tools.author = testAuthor

        let clean = await tools.invoke("1", "git_status", [:])
        XCTAssertEqual(clean.output, "Clean worktree, on main.")

        _ = await tools.invoke("2", "write_file", ["path": "a.txt", "content": "two\n"])
        let dirty = await tools.invoke("3", "git_status", [:])
        XCTAssertTrue(dirty.output.contains("a.txt"))
        XCTAssertEqual(dirty.output, "Worktree on main.\nModified:\n  modified a.txt")

        let committed = await tools.invoke("4", "git_commit", ["message": "Update a.txt"])
        XCTAssertFalse(committed.isError)
        XCTAssertTrue(committed.output.contains("Update a.txt"))
        XCTAssertTrue(committed.output.hasPrefix("Committed 00000000"))
        let after = await tools.invoke("5", "git_status", [:])
        XCTAssertTrue(after.output.contains("Clean"))

        // Committing a clean tree is refused, not an empty commit.
        let again = await tools.invoke("6", "git_commit", ["message": "noop"])
        XCTAssertTrue(again.output.contains("Nothing to commit"))
    }

    func testAnUnknownToolIsReportedWithTheRealCatalogue() async {
        let tools = AgentToolset(MemoryWorkspace(files: ["a.txt": "x\n"]))
        let result = await tools.invoke("1", "execute_command", ["command": "rm -rf /"])
        XCTAssertTrue(result.isError)
        XCTAssertTrue(result.output.contains("No tool named"))
        XCTAssertTrue(result.output.contains("read_file"))
        // The shell is genuinely absent, not merely undocumented.
        XCTAssertFalse(tools.tools.map(\.name).contains("execute_command"))
        XCTAssertEqual(tools.tools.map(\.name), ["read_file", "write_file", "replace_text", "list_directory", "grep", "git_status", "git_commit"])
    }

    func testReadOnlyModeRefusesWritesButStillAllowsReading() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let tools = AgentToolset(workspace, readOnly: true)

        let blocked = await tools.invoke("1", "write_file", ["path": "a.txt", "content": "two\n"])
        XCTAssertTrue(blocked.isError)
        XCTAssertTrue(blocked.output.contains("read-only"))
        let file = try await workspace.readFile("a.txt")
        XCTAssertEqual(file.text, "one\n")

        let allowed = await tools.invoke("2", "read_file", ["path": "a.txt"])
        XCTAssertFalse(allowed.isError)
    }

    func testAHugeToolOutputIsTruncatedRatherThanFloodingContext() async {
        let workspace = MemoryWorkspace(files: ["big.txt": String(repeating: "y\n", count: 40000)])
        let tools = AgentToolset(workspace)
        let result = await tools.invoke("1", "read_file", ["path": "big.txt"])
        XCTAssertLessThan(result.output.utf16.count, AgentToolset.maxToolOutputChars + 200)
        XCTAssertTrue(result.output.contains("truncated"))

        tools.outputLimit = 100
        let short = await tools.invoke("2", "read_file", ["path": "big.txt"])
        XCTAssertTrue(short.output.hasSuffix("… output truncated at 100 characters."))
    }

    func testWorkspaceAndArgumentFailuresBecomeReadableResults() async {
        let tools = AgentToolset(MemoryWorkspace(files: [:]))
        let missing = await tools.invoke("1", "read_file", ["path": "nope.txt"])
        XCTAssertTrue(missing.isError)
        XCTAssertEqual(missing.output, "read_file failed: nope.txt does not exist.")

        let escape = await tools.invoke("2", "write_file", ["path": ".git/config", "content": "x"])
        XCTAssertEqual(escape.output, "write_file failed: .git/config is inside .git.")

        let badArgs = await tools.invoke("3", "read_file", ["path": 42])
        XCTAssertEqual(badArgs.output, "read_file failed: Invalid argument(s): \"path\" is required and must be a string.")
    }

    func testSchemasArePlainJSONSchema() async {
        let tools = AgentToolset(MemoryWorkspace(files: [:]))
        let read = tools.tools[0].schema
        XCTAssertEqual(JSON.encode(.object(read)), #"{"type":"object","properties":{"path":{"type":"string","description":"Repository-relative path, e.g. lib/main.dart"}},"required":["path"]}"#)
        XCTAssertEqual(JSON.encode(.object(tools.tools[5].schema)), #"{"type":"object","properties":{}}"#)
    }

    func testLineSplittingMatchesDartLineSplitter() async {
        XCTAssertEqual(AgentText.splitLines("a\nb\n"), ["a", "b"])
        XCTAssertEqual(AgentText.splitLines("a\r\nb\rc"), ["a", "b", "c"])
        XCTAssertEqual(AgentText.splitLines(""), [])
        XCTAssertEqual(AgentText.splitLines("\n\n"), ["", ""])
    }
}

@MainActor
final class CodingAgentTests: XCTestCase {
    func testRunsAToolFeedsTheResultBackThenFinishes() async {
        let workspace = MemoryWorkspace(files: ["a.txt": "hello\n"])
        let provider = ScriptedProvider([
            toolTurn("read_file", ["path": "a.txt"]),
            AgentTurn(text: "The file says hello.", toolCalls: [], inputTokens: 10, outputTokens: 5),
        ])
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(workspace))

        let events = await agent.runToEnd("What does a.txt say?")

        XCTAssertEqual(events.compactMap(\.userText), ["What does a.txt say?"])
        XCTAssertEqual(events.compactMap(\.toolCall).map(\.name), ["read_file"])
        XCTAssertTrue(events.compactMap(\.toolResult).first?.output.contains("hello") ?? false)
        XCTAssertEqual(events.compactMap(\.assistantText).last, "The file says hello.")

        let done = events.compactMap(\.done)
        XCTAssertEqual(done.count, 1)
        XCTAssertEqual(done.first?.reason, .completed)
        XCTAssertEqual(done.first?.turns, 2)
        XCTAssertEqual(done.first?.inputTokens, 10)
        // The tool result really was sent back to the model.
        XCTAssertTrue(JSON.encode(.array(provider.sentHistories.last ?? [])).contains("hello"))
    }

    func testStopsAtTheTurnLimitAndSaysSo() async {
        let workspace = MemoryWorkspace(files: ["a.txt": "x\n"])
        // A model that never stops asking for tools.
        let provider = ScriptedProvider((0..<10).map { toolTurn("git_status", id: "t\($0)") })
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(workspace), maxTurns: 3)

        let events = await agent.runToEnd("loop forever")
        let done = events.compactMap(\.done)
        XCTAssertEqual(done.count, 1)
        XCTAssertEqual(done.first?.reason, .turnLimit)
        XCTAssertEqual(done.first?.turns, 3)
        XCTAssertTrue(events.compactMap(\.noticeText).last?.contains("after 3 turns") ?? false)
    }

    func testARefusalEndsTheRunInsteadOfContinuingOnNothing() async {
        let provider = ScriptedProvider([
            AgentTurn(text: "", toolCalls: [], refusal: "The model declined this request (cyber)."),
        ])
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(MemoryWorkspace(files: ["a.txt": "x\n"])))

        let events = await agent.runToEnd("do something disallowed")
        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.refused])
        XCTAssertEqual(events.compactMap(\.noticeText).count, 1)
        XCTAssertTrue(events.compactMap(\.noticeText).first?.contains("declined") ?? false)
    }

    func testCancellingStillAnswersEveryToolCallItStarted() async {
        let provider = ScriptedProvider([
            AgentTurn(text: "", toolCalls: [
                AgentToolCall(id: "a", name: "git_status", arguments: [:]),
                AgentToolCall(id: "b", name: "git_status", arguments: [:]),
            ]),
        ])
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(MemoryWorkspace(files: ["a.txt": "x\n"])))

        var events: [AgentEvent] = []
        await agent.run("status please") { event in
            events.append(event)
            // Cancel as soon as the first call is announced.
            if event.toolCall != nil { agent.cancel() }
        }

        // Every tool_use must have a matching result, or the next request
        // would be a malformed conversation.
        XCTAssertEqual(events.compactMap(\.toolCall).count, 2)
        let results = events.compactMap(\.toolResult)
        XCTAssertEqual(results.count, 2)
        XCTAssertEqual(results.map(\.output), Array(repeating: "Cancelled by the user before this ran.", count: 2))
        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.cancelled])
        XCTAssertEqual(events.compactMap(\.noticeText).last, "Stopped. The work so far is kept.")
    }

    func testSteeringIsDeliveredAtTheNextTurnBoundary() async {
        let provider = ScriptedProvider([
            toolTurn("git_status"),
            AgentTurn(text: "first done", toolCalls: []),
            AgentTurn(text: "second done", toolCalls: []),
        ])
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(MemoryWorkspace(files: ["a.txt": "x\n"])))

        agent.steer("also mention the branch")
        let events = await agent.runToEnd("check status")

        XCTAssertEqual(events.compactMap(\.userText), ["check status", "also mention the branch"])
        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.completed])
    }

    func testSteeringAfterTheLastAnswerContinuesTheSession() async {
        let provider = ScriptedProvider([
            AgentTurn(text: "first done", toolCalls: []),
            AgentTurn(text: "second done", toolCalls: []),
        ])
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(MemoryWorkspace(files: [:])))
        var events: [AgentEvent] = []
        var steered = false
        await agent.run("one") { event in
            events.append(event)
            if event.assistantText == "first done" && !steered {
                steered = true
                agent.steer("  two  ")
            }
        }
        XCTAssertEqual(events.compactMap(\.userText), ["one", "two"])
        XCTAssertEqual(events.compactMap(\.assistantText), ["first done", "second done"])
        XCTAssertEqual(events.compactMap(\.done).first?.turns, 2)
    }

    func testATransientFailureIsRetriedOnceAndABadRequestIsNot() async {
        let provider = ScriptedProvider([AgentTurn(text: "ok", toolCalls: [])])
        provider.failures = [AgentProviderException("Claude API error 529: Overloaded", statusCode: 529)]
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(MemoryWorkspace(files: [:])), sleep: { _ in })
        let events = await agent.runToEnd("hi")
        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.completed])
        XCTAssertEqual(provider.sentHistories.count, 2)

        let bad = ScriptedProvider([])
        bad.failures = [AgentProviderException("Claude API error 400: bad", statusCode: 400)]
        let second = CodingAgent(provider: bad, toolset: AgentToolset(MemoryWorkspace(files: [:])), sleep: { _ in })
        let failed = await second.runToEnd("hi")
        XCTAssertEqual(failed.compactMap(\.done).map(\.reason), [.error])
        XCTAssertEqual(failed.compactMap(\.noticeText), ["Claude API error 400: bad"])
        XCTAssertEqual(bad.sentHistories.count, 1)
    }

    func testHistoryIsTrimmedButKeepsTheTask() async {
        let provider = ScriptedProvider((0..<6).map { toolTurn("git_status", id: "t\($0)") })
        let agent = CodingAgent(provider: provider, toolset: AgentToolset(MemoryWorkspace(files: [:])), maxHistoryMessages: 4)
        _ = await agent.runToEnd("the task")
        for sent in provider.sentHistories {
            XCTAssertLessThanOrEqual(sent.count, 4)
            XCTAssertEqual(sent.first?["content"]?.stringValue, "the task")
        }
    }

    func testEndToEndTheAgentEditsARealFileAndCommitsIt() async throws {
        let workspace = MemoryWorkspace(files: ["greet.txt": "hello world\n"])
        let provider = ScriptedProvider([
            toolTurn("read_file", ["path": "greet.txt"], id: "1"),
            toolTurn("replace_text", ["path": "greet.txt", "old": "hello world", "new": "hello ForkMesh"], id: "2"),
            toolTurn("git_commit", ["message": "Greet ForkMesh"], id: "3"),
            AgentTurn(text: "Done.", toolCalls: []),
        ])
        let toolset = AgentToolset(workspace)
        toolset.author = testAuthor
        let agent = CodingAgent(provider: provider, toolset: toolset)

        let events = await agent.runToEnd("say ForkMesh instead of world")

        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.completed])
        let file = try await workspace.readFile("greet.txt")
        XCTAssertEqual(file.text, "hello ForkMesh\n")
        let status = try await workspace.status()
        XCTAssertTrue(status.isClean)
        XCTAssertEqual(workspace.log.first?.message.dartTrimmed, "Greet ForkMesh")
    }

    func testTheSystemPromptStatesTheTwoConstraints() async {
        let agent = CodingAgent(provider: ScriptedProvider([]), toolset: AgentToolset(MemoryWorkspace(files: [:])))
        XCTAssertTrue(agent.systemPrompt.contains("You have no shell."))
        XCTAssertTrue(agent.systemPrompt.hasSuffix("Commit with git_commit when the change is complete."))
    }
}
