import FMBase
import FMNet
import XCTest
@testable import FMLocalAI

@MainActor
final class OnDeviceChatModelTests: XCTestCase {
    private var temp: TempDirectory!

    override func setUp() async throws {
        temp = TempDirectory()
    }

    override func tearDown() async throws {
        temp.cleanUp()
    }

    /// A manager with `tinyModel` downloaded and the switch on.
    private func readyManager(_ engine: FakeLocalChatEngine) async throws -> LocalModelManager {
        let store = LocalModelStore(root: temp.url)
        let directory = store.directory(for: tinyModel)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try Data("{}".utf8).write(to: directory.appendingPathComponent("config.json"))
        try store.writeManifest(
            SnapshotManifest(repo: tinyModel.repo, revision: tinyModel.revision, files: [HubFile(path: "config.json", size: 2)], completedAtMs: 0),
            for: tinyModel
        )
        let downloader = HuggingFaceSnapshotDownloader(http: FakeHTTPClient(), files: FakeFileDownloader(), store: store)
        let manager = LocalModelManager(
            engine: engine, store: store, downloader: downloader, prefs: InMemoryKeyValueStore(), device: bigDevice, catalog: [tinyModel]
        )
        manager.setEnabled(true)
        return manager
    }

    func testStreamsAReplyAndShowsSpeed() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        engine.enqueueReply(["Hel", "lo", "!"], promptTokens: 21, tokensPerSecond: 38.5)
        var clock = Date(timeIntervalSince1970: 0)
        let chat = OnDeviceChatModel(manager: manager, now: {
            clock = clock.addingTimeInterval(0.2)
            return clock
        })
        XCTAssertEqual(chat.statusLine, "Tiny")
        XCTAssertFalse(chat.canSend)
        chat.draft = "  hi there  "
        XCTAssertTrue(chat.canSend)

        chat.send()
        XCTAssertTrue(chat.isGenerating)
        XCTAssertEqual(chat.draft, "")
        await chat.waitForReply()

        XCTAssertFalse(chat.isGenerating)
        XCTAssertEqual(chat.entries.map(\.role), [.user, .assistant])
        XCTAssertEqual(chat.entries.map(\.text), ["hi there", "Hello!"])
        XCTAssertFalse(chat.entries[1].isStreaming)
        XCTAssertEqual(chat.lastStats?.promptTokens, 21)
        XCTAssertEqual(chat.statusLine, "Tiny · 38.5 tok/s")
        XCTAssertEqual(manager.loadedModelId, tinyModel.id, "the first send loads the model")

        let sent = try XCTUnwrap(engine.requests.last)
        XCTAssertEqual(sent.messages, [.system(OnDeviceChatModel.defaultSystemPrompt), .user("hi there")])
        XCTAssertEqual(sent.tools, [])
        XCTAssertEqual(sent.parameters, .chat)

        // History goes back with the next prompt.
        engine.enqueueReply(["Sure."])
        chat.draft = "again"
        chat.send()
        await chat.waitForReply()
        XCTAssertEqual(engine.requests.last?.messages.map(\.content), [OnDeviceChatModel.defaultSystemPrompt, "hi there", "Hello!", "again"])
    }

    func testStopKeepsWhatStreamed() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        engine.enqueue([.event(.text("partial")), .hang])
        let chat = OnDeviceChatModel(manager: manager)
        chat.draft = "go"
        chat.send()
        while chat.entries.last?.text != "partial" { try await Task.sleep(nanoseconds: 1_000_000) }
        chat.stop()
        await chat.waitForReply()
        XCTAssertFalse(chat.isGenerating)
        XCTAssertEqual(chat.entries.last?.text, "partial")
        XCTAssertNil(chat.errorMessage)

        chat.clear()
        XCTAssertTrue(chat.entries.isEmpty)
        XCTAssertNil(chat.lastStats)
    }

    func testErrorsBecomeNotesNotHistory() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        engine.enqueue([.fail(.generationFailed("out of memory"))])
        let chat = OnDeviceChatModel(manager: manager)
        chat.draft = "hi"
        chat.send()
        await chat.waitForReply()
        XCTAssertEqual(chat.errorMessage, "The model stopped with an error: out of memory")
        XCTAssertEqual(chat.entries.last?.isError, true)

        engine.enqueueReply(["ok"])
        chat.draft = "again"
        chat.send()
        await chat.waitForReply()
        XCTAssertEqual(engine.requests.last?.messages.map(\.content), [OnDeviceChatModel.defaultSystemPrompt, "hi", "again"])
    }

    func testSwitchOffExplainsItself() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        manager.setEnabled(false)
        let chat = OnDeviceChatModel(manager: manager)
        XCTAssertEqual(chat.statusLine, "On-device AI is off")
        chat.draft = "hi"
        XCTAssertFalse(chat.canSend)
        chat.send()
        await chat.waitForReply()
        XCTAssertEqual(chat.errorMessage, LocalModelError.disabled.description)
        XCTAssertTrue(engine.requests.isEmpty)
    }

    func testHistoryIsBounded() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let chat = OnDeviceChatModel(manager: manager, maxHistoryMessages: 3)
        for index in 0..<3 {
            engine.enqueueReply(["r\(index)"])
            chat.draft = "q\(index)"
            chat.send()
            await chat.waitForReply()
        }
        // q2 plus up to three earlier messages, starting at a user turn.
        XCTAssertEqual(engine.requests.last?.messages.map(\.content), [OnDeviceChatModel.defaultSystemPrompt, "q1", "r1", "q2"])
    }

    func testToolboxAnswersFromForkMeshData() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        let call = LocalToolCall(id: "call_1", name: "list_tasks", arguments: ["status": "open"])
        engine.enqueue([.event(.toolCall(call)), .event(.finished(LocalGenerationStats(promptTokens: 300, generatedTokens: 9, tokensPerSecond: 30)))])
        engine.enqueueReply(["You have ", "2 tasks."])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "List my tasks"
        chat.send()
        await chat.waitForReply()

        XCTAssertEqual(toolbox.calls, [call])
        XCTAssertEqual(chat.entries.map(\.role), [.user, .assistant, .tool, .assistant])
        XCTAssertTrue(chat.entries[1].isToolRequest)
        XCTAssertEqual(chat.entries[2].text, "1. Ship it\n2. Test it")
        XCTAssertEqual(chat.entries[2].toolName, "list_tasks")
        XCTAssertEqual(chat.entries[3].text, "You have 2 tasks.")
        XCTAssertNil(chat.activeTool)

        let first = try XCTUnwrap(engine.requests.first)
        XCTAssertEqual(first.tools, toolbox.tools)
        XCTAssertEqual(first.messages.first?.role, .system)
        XCTAssertTrue(first.messages[0].content.hasPrefix(OnDeviceChatModel.toolboxSystemPrompt))
        XCTAssertTrue(first.messages[0].content.contains("Signed in as ada"))
        let second = try XCTUnwrap(engine.requests.last)
        XCTAssertEqual(second.messages.map(\.role), [.system, .user, .assistant, .tool])
        XCTAssertEqual(second.messages[2].toolCalls, [call])
        XCTAssertEqual(second.messages[3].toolCallId, "call_1")
        XCTAssertEqual(second.messages[3].name, "list_tasks")

        engine.enqueueReply(["Done."])
        chat.draft = "thanks"
        chat.send()
        await chat.waitForReply()
        XCTAssertEqual(engine.requests.last?.messages.map(\.role), [.system, .user, .assistant, .tool, .assistant, .user])
    }

    func testToolRoundsAreBounded() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        for index in 0..<OnDeviceChatModel.maxToolRounds {
            engine.enqueue([.event(.toolCall(LocalToolCall(id: "call_\(index)", name: "list_tasks", arguments: [:])))])
        }
        engine.enqueueReply(["Here."])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "tasks?"
        chat.send()
        await chat.waitForReply()
        XCTAssertEqual(toolbox.calls.count, OnDeviceChatModel.maxToolRounds)
        XCTAssertEqual(engine.requests.last?.tools, [])
        XCTAssertEqual(chat.entries.last?.text, "Here.")
    }

    func testLongToolOutputIsClipped() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        toolbox.output = String(repeating: "x", count: OnDeviceChatModel.maxToolOutputChars + 50)
        engine.enqueue([.event(.toolCall(LocalToolCall(id: "c", name: "read_note", arguments: [:])))])
        engine.enqueueReply(["ok"])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "note?"
        chat.send()
        await chat.waitForReply()
        let tool = try XCTUnwrap(chat.entries.first { $0.role == .tool })
        XCTAssertTrue(tool.text.hasSuffix("(50 more characters cut)"))
    }

    func testEarlierToolOutputIsClippedHarderInHistory() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        toolbox.output = String(repeating: "x", count: OnDeviceChatModel.maxHistoryToolOutputChars + 70)
        engine.enqueue([.event(.toolCall(LocalToolCall(id: "c", name: "read_note", arguments: [:])))])
        engine.enqueueReply(["ok"])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "note?"
        chat.send()
        await chat.waitForReply()
        let sameSend = try XCTUnwrap(engine.requests.last?.messages.first { $0.role == .tool })
        XCTAssertEqual(sameSend.content.count, OnDeviceChatModel.maxHistoryToolOutputChars + 70)

        engine.enqueueReply(["fine"])
        chat.draft = "and now?"
        chat.send()
        await chat.waitForReply()
        let later = try XCTUnwrap(engine.requests.last?.messages.first { $0.role == .tool })
        XCTAssertTrue(later.content.hasSuffix("(70 more characters cut)"))
    }

    func testApprovedToolRuns() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        toolbox.needsApproval = ["delete_crm_contact"]
        toolbox.output = "Deleted."
        engine.enqueue([.event(.toolCall(LocalToolCall(id: "c", name: "delete_crm_contact", arguments: ["id": "a"])))])
        engine.enqueueReply(["Gone."])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "delete ada"
        chat.send()
        while chat.pendingApproval == nil { try await Task.sleep(nanoseconds: 1_000_000) }
        XCTAssertEqual(chat.pendingApproval?.summary, "Delete Ada Lovelace from the CRM")
        XCTAssertEqual(chat.statusLine, "Waiting for your approval")
        XCTAssertTrue(toolbox.calls.isEmpty, "nothing runs before the user answers")
        chat.resolveApproval(true)
        await chat.waitForReply()
        XCTAssertEqual(toolbox.calls.count, 1)
        XCTAssertNil(chat.pendingApproval)
        XCTAssertEqual(chat.entries.last?.text, "Gone.")
    }

    func testDeclinedToolTellsTheModel() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        toolbox.needsApproval = ["delete_crm_contact"]
        engine.enqueue([.event(.toolCall(LocalToolCall(id: "c", name: "delete_crm_contact", arguments: [:])))])
        engine.enqueueReply(["Kept."])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "delete ada"
        chat.send()
        while chat.pendingApproval == nil { try await Task.sleep(nanoseconds: 1_000_000) }
        chat.resolveApproval(false)
        await chat.waitForReply()
        XCTAssertTrue(toolbox.calls.isEmpty)
        let tool = try XCTUnwrap(chat.entries.first { $0.role == .tool })
        XCTAssertTrue(tool.text.hasPrefix("The user declined this"), tool.text)
        XCTAssertEqual(engine.requests.last?.messages.last?.content, tool.text)
    }

    func testStopWhileWaitingForApprovalEndsTheReply() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        toolbox.needsApproval = ["stop_agent"]
        engine.enqueue([.event(.toolCall(LocalToolCall(id: "c", name: "stop_agent", arguments: [:])))])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "stop it"
        chat.send()
        while chat.pendingApproval == nil { try await Task.sleep(nanoseconds: 1_000_000) }
        chat.stop()
        await chat.waitForReply()
        XCTAssertFalse(chat.isGenerating)
        XCTAssertNil(chat.pendingApproval)
        XCTAssertTrue(toolbox.calls.isEmpty)
    }

    func testEmailDraftsRideOnTheToolEntry() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try await readyManager(engine)
        let toolbox = FakeToolbox()
        toolbox.email = OnDeviceEmailDraft(to: "ada@example.com", recipientName: "Ada", subject: "Hello & welcome", body: "Hi Ada,\nThanks.")
        engine.enqueue([.event(.toolCall(LocalToolCall(id: "c", name: "draft_email", arguments: [:])))])
        engine.enqueueReply(["Drafted."])
        let chat = OnDeviceChatModel(manager: manager, toolbox: toolbox)
        chat.draft = "email ada"
        chat.send()
        await chat.waitForReply()
        let tool = try XCTUnwrap(chat.entries.first { $0.role == .tool })
        XCTAssertEqual(tool.email?.to, "ada@example.com")
        let url = try XCTUnwrap(tool.email?.mailtoURL?.absoluteString)
        XCTAssertTrue(url.hasPrefix("mailto:ada@example.com?subject=Hello%20%26%20welcome&body=Hi%20Ada,%0AThanks."), url)
    }

    func testToolLabels() {
        XCTAssertEqual(OnDeviceChatTools.label(for: "list_tasks"), "List tasks")
        XCTAssertEqual(OnDeviceChatTools.label(for: "search"), "Search")
    }
}

@MainActor
private final class FakeToolbox: OnDeviceChatToolbox {
    var calls: [LocalToolCall] = []
    var output = "1. Ship it\n2. Test it"

    let tools = [LocalToolSpec(name: "list_tasks", description: "The user's tasks", parameters: ["type": "object"])]

    func briefing() async -> String { "Signed in as ada." }

    var needsApproval: Set<String> = []
    var email: OnDeviceEmailDraft?

    func approval(for call: LocalToolCall) async -> String? {
        needsApproval.contains(call.name) ? "Delete Ada Lovelace from the CRM" : nil
    }

    func invoke(_ call: LocalToolCall) async -> OnDeviceToolResult {
        calls.append(call)
        return OnDeviceToolResult(output, email: email)
    }
}
