import FMBase
import XCTest
@testable import FMLocalAI

final class LocalInferenceServiceTests: XCTestCase {
    private func loadedEngine() async throws -> FakeLocalChatEngine {
        let engine = FakeLocalChatEngine()
        try await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: LocalModelCatalog.gemma4E2B)
        return engine
    }

    private func service(_ engine: FakeLocalChatEngine, model: LocalAIModel? = LocalModelCatalog.gemma4E2B) -> LocalInferenceService {
        LocalInferenceService(engine: engine, deviceName: "Pat's iPhone", maxTokensCap: 1000) { model }
    }

    func testParsesOpenAIRequests() throws {
        let svc = LocalInferenceService(engine: FakeLocalChatEngine(), maxTokensCap: 1000) { nil }
        let request = try svc.parse([
            "messages": [
                ["role": "system", "content": "Be brief."],
                ["role": "user", "content": [["type": "text", "text": "Read "], ["type": "text", "text": "main"]]],
                ["role": "assistant", "content": .null, "tool_calls": [
                    ["id": "c1", "type": "function", "function": ["name": "read_file", "arguments": #"{"path":"a.dart"}"#]],
                ]],
                ["role": "tool", "tool_call_id": "c1", "name": "read_file", "content": "1: x"],
            ],
            "tools": [
                ["type": "function", "function": ["name": "read_file", "description": "Read.", "parameters": ["type": "object"]]],
                ["name": "grep", "input_schema": ["type": "object", "required": ["pattern"]]],
            ],
            "max_tokens": 5000,
            "temperature": 0.2,
            "top_k": 20,
        ])
        XCTAssertEqual(request.messages, [
            .system("Be brief."),
            .user("Read main"),
            .assistant("", toolCalls: [LocalToolCall(id: "c1", name: "read_file", arguments: ["path": "a.dart"])]),
            .tool("1: x", callId: "c1", name: "read_file"),
        ])
        XCTAssertEqual(request.tools.map(\.name), ["read_file", "grep"])
        XCTAssertEqual(request.tools[1].parameters, ["type": "object", "required": ["pattern"]])
        XCTAssertEqual(request.parameters.maxTokens, 1000, "capped")
        XCTAssertEqual(request.parameters.temperature, 0.2)
        XCTAssertEqual(request.parameters.topK, 20)
        XCTAssertEqual(try svc.parse(["messages": [["role": "user", "content": "x"]]]).parameters.maxTokens, 512)

        XCTAssertThrowsError(try svc.parse([:]))
        XCTAssertThrowsError(try svc.parse(["messages": [["role": "wizard", "content": "x"]]]))
        XCTAssertThrowsError(try svc.parse(["messages": ["just text"]]))
        XCTAssertThrowsError(try svc.parse(["messages": [["role": "user", "content": "x"]], "tools": [["description": "no name"]]]))
    }

    func testRespondReturnsAnOpenAIShapedReply() async throws {
        let engine = try await loadedEngine()
        engine.enqueue([
            .event(.text("Reading.")),
            .event(.toolCall(LocalToolCall(id: "call_7", name: "read_file", arguments: ["path": "a.dart"]))),
            .event(.finished(LocalGenerationStats(promptTokens: 40, generatedTokens: 9, tokensPerSecond: 37.26))),
        ])
        let reply = await service(engine).respond(to: [
            "messages": [["role": "user", "content": "read a.dart"]],
            "tools": [["type": "function", "function": ["name": "read_file", "parameters": ["type": "object"]]]],
        ])
        XCTAssertEqual(JSON.encode(reply), JSON.encode(.object([
            "ok": true,
            "model": "gemma-4-e2b-it-4bit",
            "message": [
                "role": "assistant",
                "content": "Reading.",
                "tool_calls": [["id": "call_7", "type": "function", "function": ["name": "read_file", "arguments": #"{"path":"a.dart"}"#]]],
            ],
            "finish_reason": "tool_calls",
            "usage": ["prompt_tokens": 40, "completion_tokens": 9],
            "tokens_per_second": 37.3,
        ])))
        XCTAssertEqual(engine.requests.last?.tools.map(\.name), ["read_file"])
    }

    func testStreamEmitsDeltasThenDone() async throws {
        let engine = try await loadedEngine()
        engine.enqueueReply(["a", "b"], promptTokens: 3, tokensPerSecond: 50)
        var events: [JSONObject] = []
        for await event in service(engine).stream(["messages": [["role": "user", "content": "hi"]]]) {
            events.append(event)
        }
        XCTAssertEqual(events.map { $0.str("event") }, ["ai.delta", "ai.delta", "ai.done"])
        XCTAssertEqual(events[0].str("text"), "a")
        XCTAssertEqual(events[2].obj("message").str("content"), "ab")
        XCTAssertEqual(events[2].str("finish_reason"), "stop")
    }

    func testErrors() async throws {
        let engine = try await loadedEngine()
        let bad = await service(engine).respond(to: ["messages": []])
        XCTAssertEqual(bad.bool("ok"), false)
        XCTAssertEqual(bad.str("error"), "bad_request")
        XCTAssertNil(bad["event"])

        let off = await service(engine, model: nil).respond(to: ["messages": [["role": "user", "content": "hi"]]])
        XCTAssertEqual(off.str("error"), "model_unavailable")

        let noGPU = FakeLocalChatEngine(availability: .unavailable(reason: "Simulator."))
        let unavailable = await LocalInferenceService(engine: noGPU) { nil }.respond(to: ["messages": [["role": "user", "content": "hi"]]])
        XCTAssertEqual(unavailable.str("error"), "model_unavailable")
        XCTAssertEqual(unavailable.str("message"), "Simulator.")

        engine.enqueue([.fail(.generationFailed("boom"))])
        let failed = await service(engine).respond(to: ["messages": [["role": "user", "content": "hi"]]])
        XCTAssertEqual(failed.str("error"), "generation_failed")
    }

    func testOneRequestAtATime() async throws {
        let engine = try await loadedEngine()
        engine.enqueue([.event(.text("slow")), .hang])
        let svc = service(engine)
        let firstDelta = Expectation()
        let consumer = Task {
            for await event in svc.stream(["messages": [["role": "user", "content": "one"]]]) {
                if event.str("event") == "ai.delta" { firstDelta.fulfil() }
            }
        }
        while !firstDelta.done { try await Task.sleep(nanoseconds: 1_000_000) }

        let second = await svc.respond(to: ["messages": [["role": "user", "content": "two"]]])
        XCTAssertEqual(second.str("error"), "busy")
        let info = await svc.info()
        XCTAssertEqual(info.bool("busy"), true)
        XCTAssertEqual(info.str("model"), "gemma-4-e2b-it-4bit")
        XCTAssertEqual(info.str("model_name"), "Gemma 4 E2B")
        XCTAssertEqual(info.str("device"), "Pat's iPhone")
        XCTAssertEqual(info.int("max_tokens"), 1000)

        // Dropping the stream stops the generation and frees the service.
        consumer.cancel()
        await consumer.value
        var freed = false
        for _ in 0..<500 {
            if await svc.info().bool("busy") == false {
                freed = true
                break
            }
            try await Task.sleep(nanoseconds: 2_000_000)
        }
        XCTAssertTrue(freed)
    }
}

final class Expectation: @unchecked Sendable {
    private let lock = NSLock()
    private var fulfilled = false

    func fulfil() {
        lock.lock()
        fulfilled = true
        lock.unlock()
    }

    var done: Bool {
        lock.lock()
        defer { lock.unlock() }
        return fulfilled
    }
}
