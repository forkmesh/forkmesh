import FMBase
import XCTest
@testable import FMLocalAI

final class LocalChatEngineTests: XCTestCase {
    func testTemplateMessagesHaveTheOpenAIShapeGemmaReads() throws {
        let call = LocalToolCall(id: "call_1", name: "read_file", arguments: ["path": "lib/main.dart", "limit": 20, "all": true])
        let rendered = LocalChatTemplate.messages([
            .system("Be brief."),
            .user("Read main"),
            .assistant("", toolCalls: [call]),
            .tool("1: void main() {}", callId: "call_1", name: "read_file"),
        ])
        XCTAssertEqual(rendered.count, 4)
        XCTAssertEqual(rendered[0]["role"] as? String, "system")
        XCTAssertEqual(rendered[0]["content"] as? String, "Be brief.")
        XCTAssertNil(rendered[0]["tool_calls"])

        let calls = try XCTUnwrap(rendered[2]["tool_calls"] as? [[String: any Sendable]])
        XCTAssertEqual(calls.count, 1)
        XCTAssertEqual(calls[0]["id"] as? String, "call_1")
        XCTAssertEqual(calls[0]["type"] as? String, "function")
        let function = try XCTUnwrap(calls[0]["function"] as? [String: any Sendable])
        XCTAssertEqual(function["name"] as? String, "read_file")
        // Gemma's template iterates arguments as a mapping, not a JSON string.
        let arguments = try XCTUnwrap(function["arguments"] as? [String: any Sendable])
        XCTAssertEqual(arguments["path"] as? String, "lib/main.dart")
        XCTAssertEqual(arguments["limit"] as? Int, 20)
        XCTAssertEqual(arguments["all"] as? Bool, true)

        XCTAssertEqual(rendered[3]["role"] as? String, "tool")
        XCTAssertEqual(rendered[3]["tool_call_id"] as? String, "call_1")
        XCTAssertEqual(rendered[3]["name"] as? String, "read_file")
    }

    func testTemplateToolsAndJSONConversion() throws {
        let schema: JSONObject = [
            "type": "object",
            "properties": ["path": ["type": "string", "description": "Repository-relative path"]],
            "required": ["path"],
            "nullable": .null,
        ]
        let tools = LocalChatTemplate.tools([LocalToolSpec(name: "read_file", description: "Read a file.", parameters: schema)])
        XCTAssertEqual(tools.count, 1)
        XCTAssertEqual(tools[0]["type"] as? String, "function")
        let function = try XCTUnwrap(tools[0]["function"] as? [String: any Sendable])
        XCTAssertEqual(function["description"] as? String, "Read a file.")
        let parameters = try XCTUnwrap(function["parameters"] as? [String: any Sendable])
        XCTAssertEqual((parameters["required"] as? [any Sendable])?.compactMap { $0 as? String }, ["path"])
        XCTAssertNil(parameters["nullable"], "null keys are dropped")
        let properties = try XCTUnwrap(parameters["properties"] as? [String: any Sendable])
        XCTAssertNotNil(properties["path"] as? [String: any Sendable])

        XCTAssertEqual(LocalChatTemplate.sendable(.double(1.5)) as? Double, 1.5)
        let nullInArray = LocalChatTemplate.sendable([nil, 1]) as? [any Sendable]
        XCTAssertEqual(nullInArray?.count, 2)
        XCTAssertNil(nullInArray?[0] as? String)
    }

    func testToolCallArgumentsJSON() {
        let call = LocalToolCall(id: "c", name: "grep", arguments: ["pattern": "TODO", "max": 5])
        XCTAssertEqual(call.argumentsJSON, #"{"pattern":"TODO","max":5}"#)
        XCTAssertTrue(LocalToolCall.makeId().hasPrefix("call_"))
        XCTAssertNotEqual(LocalToolCall.makeId(), LocalToolCall.makeId())
    }

    func testCompleteCollectsATurn() async throws {
        let engine = FakeLocalChatEngine()
        try await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: tinyModel)
        let call = LocalToolCall(id: "call_9", name: "list_files", arguments: [:])
        engine.enqueue([
            .event(.text("Let me ")),
            .event(.text("look.")),
            .event(.toolCall(call)),
            .event(.finished(LocalGenerationStats(promptTokens: 30, generatedTokens: 12, tokensPerSecond: 41))),
        ])
        let tools = [LocalToolSpec(name: "list_files", description: "List.", parameters: ["type": "object"])]
        let turn = try await engine.complete(messages: [.user("hi")], tools: tools)
        XCTAssertEqual(turn.text, "Let me look.")
        XCTAssertEqual(turn.toolCalls, [call])
        XCTAssertTrue(turn.wantsTools)
        XCTAssertEqual(turn.inputTokens, 30)
        XCTAssertEqual(turn.outputTokens, 12)
        XCTAssertEqual(turn.message, .assistant("Let me look.", toolCalls: [call]))
        XCTAssertEqual(engine.requests.last?.tools, tools)
        XCTAssertEqual(engine.requests.last?.parameters, .agent)
    }

    func testFakeEngineThrowsWithoutAModelAndOnScriptedFailure() async throws {
        let engine = FakeLocalChatEngine()
        do {
            _ = try await engine.complete(messages: [.user("hi")])
            XCTFail("no model loaded")
        } catch let error as LocalEngineError {
            XCTAssertEqual(error, .noModelLoaded)
        }
        try await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: tinyModel)
        engine.enqueue([.event(.text("a")), .fail(.generationFailed("metal"))])
        do {
            _ = try await engine.complete(messages: [.user("hi")])
            XCTFail("scripted failure")
        } catch let error as LocalEngineError {
            XCTAssertEqual(error, .generationFailed("metal"))
        }
        await engine.unload()
        let loaded = await engine.loadedModelId()
        XCTAssertNil(loaded)
    }
}

final class LocalChatMessageOpenAITests: XCTestCase {
    func testRoundTripsOpenAIMessages() throws {
        let assistant = LocalChatMessage.assistant("Reading.", toolCalls: [
            LocalToolCall(id: "call_1", name: "read_file", arguments: ["path": "a.dart"]),
        ])
        XCTAssertEqual(JSON.encode(assistant.openAIJSON), #"{"role":"assistant","content":"Reading.","tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.dart\"}"}}]}"#)
        XCTAssertEqual(try LocalChatMessage(openAI: assistant.openAIJSON), assistant)

        let tool = LocalChatMessage.tool("1: x", callId: "call_1", name: "read_file")
        XCTAssertEqual(JSON.encode(tool.openAIJSON), #"{"role":"tool","content":"1: x","tool_call_id":"call_1","name":"read_file"}"#)
        XCTAssertEqual(try LocalChatMessage(openAI: tool.openAIJSON), tool)

        // Arguments as an object, content null, a call without an id.
        let loose = try LocalChatMessage(openAI: [
            "role": "assistant", "content": .null,
            "tool_calls": [["function": ["name": "grep", "arguments": ["pattern": "TODO"]]]],
        ])
        XCTAssertEqual(loose.content, "")
        XCTAssertEqual(loose.toolCalls.first?.arguments, ["pattern": "TODO"])
        XCTAssertTrue(loose.toolCalls.first?.id.hasPrefix("call_") ?? false)

        XCTAssertThrowsError(try LocalChatMessage(openAI: ["role": "robot"]))
        XCTAssertThrowsError(try LocalChatMessage(openAI: ["role": "assistant", "tool_calls": [["function": ["arguments": "{}"]]]])) { error in
            XCTAssertEqual((error as? LocalChatFormatError)?.message, "tool_calls[0]: no function name")
        }
    }
}
