// GemmaAgentProvider over FMLocalAI's FakeLocalChatEngine: the same loop the
// cloud providers drive, with the model on the phone.

import FMBase
import FMLocalAI
import FMNet
import XCTest
@testable import FMAgents

@MainActor
final class GemmaAgentProviderTests: XCTestCase {
    private var temp: TempDirectory!

    override func setUp() async throws {
        temp = TempDirectory()
    }

    override func tearDown() async throws {
        temp.cleanUp()
        Services.remove(LocalModelManager.self)
    }

    func testAToolCallRoundTripsThroughTheLoop() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try localModels(engine, in: temp)
        let workspace = MemoryWorkspace(files: ["a.txt": "hello\n"])
        engine.enqueue([
            .event(.text("Let me look.")),
            .event(.toolCall(LocalToolCall(id: "call_1", name: "read_file", arguments: ["path": "a.txt"]))),
            .event(.finished(LocalGenerationStats(promptTokens: 120, generatedTokens: 9, tokensPerSecond: 40))),
        ])
        engine.enqueueReply(["It says ", "hello."], promptTokens: 150)

        let credentials = AgentCredentials.onDevice(model: tinyModel.id)
        let toolset = AgentToolset(workspace)
        let agent = credentials.makeAgent(toolset: toolset, localModels: manager)
        XCTAssertEqual(agent.provider.id, "on-device")
        XCTAssertEqual(agent.provider.model, tinyModel.id)
        XCTAssertEqual(agent.maxHistoryMessages, GemmaAgentProvider.maxHistoryMessages)
        XCTAssertEqual(toolset.outputLimit, GemmaAgentProvider.maxToolOutputChars)

        let events = await agent.runToEnd("What does a.txt say?")

        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.completed])
        XCTAssertEqual(events.compactMap(\.done).first?.turns, 2)
        XCTAssertEqual(events.compactMap(\.done).first?.inputTokens, 270)
        XCTAssertEqual(events.compactMap(\.toolCall), [AgentToolCall(id: "call_1", name: "read_file", arguments: ["path": "a.txt"])])
        XCTAssertEqual(events.compactMap(\.toolResult).first?.output, "1  hello\n")
        XCTAssertEqual(events.compactMap(\.assistantText), ["Let me look.", "It says hello."])
        XCTAssertEqual(manager.loadedModelId, tinyModel.id, "the first turn loads the model")

        // The first request: system prompt, the task, the tool catalogue.
        let first = engine.requests[0]
        XCTAssertEqual(first.parameters, .agent)
        XCTAssertEqual(first.messages.map(\.role), [.system, .user])
        XCTAssertEqual(first.messages[0].content, agent.systemPrompt)
        XCTAssertEqual(first.tools.map(\.name), toolset.tools.map(\.name))
        XCTAssertEqual(first.tools[0].parameters, toolset.tools[0].schema)

        // The second carries the assistant's call and the tool's answer,
        // paired by id, the way Gemma 4's template reads them.
        let second = engine.requests[1]
        XCTAssertEqual(second.messages.map(\.role), [.system, .user, .assistant, .tool])
        XCTAssertEqual(second.messages[2].content, "Let me look.")
        XCTAssertEqual(second.messages[2].toolCalls.map(\.id), ["call_1"])
        XCTAssertEqual(second.messages[3].toolCallId, "call_1")
        XCTAssertEqual(second.messages[3].name, "read_file")
        XCTAssertEqual(second.messages[3].content, "1  hello\n")

        // History stays OpenAI-shaped.
        XCTAssertEqual(agent.history[1]["role"], "assistant")
        XCTAssertEqual(agent.history[1]["tool_calls"]?[0]?["function"]?["name"], "read_file")
        XCTAssertEqual(agent.history[2]["role"], "tool")
        XCTAssertEqual(agent.history[2]["tool_call_id"], "call_1")
    }

    func testAnEditAndCommitRunEntirelyOnTheDevice() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try localModels(engine, in: temp)
        let workspace = MemoryWorkspace(files: ["greet.txt": "hello world\n"])
        engine.enqueue([
            .event(.toolCall(LocalToolCall(id: "1", name: "replace_text", arguments: ["path": "greet.txt", "old": "world", "new": "ForkMesh"]))),
            .event(.toolCall(LocalToolCall(id: "2", name: "git_commit", arguments: ["message": "Greet ForkMesh"]))),
        ])
        engine.enqueueReply(["Done."])
        let agent = CodingAgent(provider: GemmaAgentProvider(manager: manager), toolset: AgentToolset(workspace))

        let events = await agent.runToEnd("say ForkMesh")

        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.completed])
        XCTAssertEqual(events.compactMap(\.toolResult).map(\.isError), [false, false])
        let file = try await workspace.readFile("greet.txt")
        XCTAssertEqual(file.text, "hello ForkMesh\n")
        XCTAssertEqual(workspace.log.first?.message, "Greet ForkMesh")
        // Both results of the turn go back as two tool messages.
        XCTAssertEqual(engine.requests[1].messages.map(\.role), [.system, .user, .assistant, .tool, .tool])
    }

    func testAModelThatIsNotDownloadedEndsTheRunWithoutARetry() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try localModels(engine, in: temp, downloaded: false)
        let agent = CodingAgent(
            provider: GemmaAgentProvider(manager: manager),
            toolset: AgentToolset(MemoryWorkspace(files: [:])),
            sleep: { _ in }
        )
        let events = await agent.runToEnd("hi")
        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.error])
        XCTAssertEqual(events.compactMap(\.noticeText), ["Tiny is not downloaded yet."])
        XCTAssertTrue(engine.requests.isEmpty)
    }

    func testTheSwitchOffIsExplained() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try localModels(engine, in: temp, enabled: false)
        do {
            _ = try await GemmaAgentProvider(manager: manager).send(systemPrompt: "s", history: [], tools: [], client: FakeHTTPClient())
            XCTFail("expected an error")
        } catch let error as AgentProviderException {
            XCTAssertEqual(error.message, LocalModelError.disabled.description)
            XCTAssertFalse(error.isRetryable)
            XCTAssertNil(error.statusCode)
        }
    }

    func testAGenerationFailureIsNotRetried() async throws {
        let engine = FakeLocalChatEngine()
        let manager = try localModels(engine, in: temp)
        engine.enqueue([.fail(.generationFailed("out of memory"))])
        engine.enqueueReply(["should not be used"])
        let agent = CodingAgent(provider: GemmaAgentProvider(manager: manager), toolset: AgentToolset(MemoryWorkspace(files: [:])), sleep: { _ in })
        let events = await agent.runToEnd("hi")
        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.error])
        XCTAssertEqual(events.compactMap(\.noticeText), ["The model stopped with an error: out of memory"])
        XCTAssertEqual(engine.requests.count, 1)
    }

    func testAnUnavailableRuntimeIsExplained() async throws {
        let engine = FakeLocalChatEngine(availability: .unavailable(reason: "On-device models do not run in the Simulator."))
        let manager = try localModels(engine, in: temp)
        XCTAssertEqual(OnDeviceAgentReadiness.of(manager), .unavailable(reason: "On-device models do not run in the Simulator."))
        do {
            _ = try await GemmaAgentProvider(manager: manager).send(systemPrompt: "s", history: [], tools: [], client: FakeHTTPClient())
            XCTFail("expected an error")
        } catch let error as AgentProviderException {
            XCTAssertEqual(error.message, "On-device models do not run in the Simulator.")
        }
    }

    func testWithoutAManagerTheProviderSaysSo() async {
        Services.remove(LocalModelManager.self)
        let provider = GemmaAgentProvider()
        XCTAssertNil(provider.manager)
        XCTAssertEqual(provider.model, LocalModelCatalog.defaultModel.id)
        do {
            _ = try await provider.send(systemPrompt: "s", history: [], tools: [], client: FakeHTTPClient())
            XCTFail("expected an error")
        } catch let error as AgentProviderException {
            XCTAssertEqual(error.message, "On-device AI is not available in this build.")
            XCTAssertFalse(error.isRetryable)
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    func testTheManagerIsResolvedFromServices() async throws {
        let manager = try localModels(FakeLocalChatEngine(), in: temp)
        Services.put(manager)
        XCTAssertTrue(GemmaAgentProvider().manager === manager)
        XCTAssertTrue((AgentCredentials.onDevice(model: tinyModel.id).build() as? GemmaAgentProvider)?.manager === manager)
    }

    func testReadinessFollowsTheManager() async throws {
        let engine = FakeLocalChatEngine()
        XCTAssertEqual(OnDeviceAgentReadiness.of(nil), .unavailable(reason: "On-device AI is not available in this build."))
        XCTAssertFalse(OnDeviceAgentReadiness.of(nil).canBeFixedInSettings)

        let off = try localModels(engine, in: temp, enabled: false)
        XCTAssertEqual(
            OnDeviceAgentReadiness.of(off),
            .notReady(reason: "On-device AI is off. Turn it on and download Tiny to run agents on this iPhone.")
        )
        XCTAssertTrue(OnDeviceAgentReadiness.of(off).canBeFixedInSettings)

        let missing = TempDirectory()
        defer { missing.cleanUp() }
        let notDownloaded = try localModels(engine, in: missing, downloaded: false)
        XCTAssertEqual(OnDeviceAgentReadiness.of(notDownloaded), .notReady(reason: "Download Tiny to run agents on this iPhone."))

        let ready = try localModels(engine, in: temp)
        XCTAssertTrue(OnDeviceAgentReadiness.of(ready).isReady)
        XCTAssertEqual(OnDeviceAgentReadiness.of(ready).message, "Tiny is downloaded and runs on this iPhone.")
    }
}
