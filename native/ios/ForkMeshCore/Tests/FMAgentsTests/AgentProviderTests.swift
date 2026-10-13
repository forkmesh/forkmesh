import FMBase
import FMNet
import XCTest
@testable import FMAgents

@MainActor
final class AgentProviderTests: XCTestCase {
    private func tools() -> [AgentTool] { AgentToolset(MemoryWorkspace(files: ["a.txt": "x\n"])).tools }

    func testAnthropicSendsToolsAndReturnsToolCalls() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: [
            "content": [
                ["type": "text", "text": "Reading it now."],
                ["type": "tool_use", "id": "toolu_1", "name": "read_file", "input": ["path": "a.txt"]],
            ],
            "stop_reason": "tool_use",
            "usage": ["input_tokens": 11, "output_tokens": 7],
        ])

        let provider = AnthropicProvider(apiKey: "sk-test", model: "claude-opus-5")
        var history: [JSONValue] = []
        provider.appendUser(&history, "read a.txt")

        let turn = try await provider.send(systemPrompt: "sys", history: history, tools: tools(), client: http)

        let request = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(request.url.absoluteString, "https://api.anthropic.com/v1/messages")
        XCTAssertEqual(request.headers["x-api-key"], "sk-test")
        XCTAssertEqual(request.headers["anthropic-version"], "2023-06-01")
        XCTAssertEqual(request.headers["anthropic-beta"], "server-side-fallback-2026-07-01")

        XCTAssertEqual(turn.text, "Reading it now.")
        XCTAssertEqual(turn.toolCalls.map(\.name), ["read_file"])
        XCTAssertEqual(turn.toolCalls.first?.arguments["path"], "a.txt")
        XCTAssertEqual(turn.inputTokens, 11)
        XCTAssertEqual(turn.outputTokens, 7)

        // Tools go over as name/description/input_schema.
        let sent = JSON.parseObject(request.bodyText)
        XCTAssertNotNil(sent["tools"]?[0]?["input_schema"])
        XCTAssertEqual(sent["model"], "claude-opus-5")
        XCTAssertEqual(sent["max_tokens"], 16000)
        XCTAssertEqual(sent["system"], "sys")
        // Opus 5 gets the server-side refusal fallback by default.
        XCTAssertEqual(sent["fallbacks"], "default")

        // All results for a turn go back in ONE user message.
        provider.appendAssistant(&history, turn)
        XCTAssertEqual(history.last?["role"], "assistant")
        XCTAssertEqual(history.last?["content"]?.arrayValue?.count, 2)
        provider.appendToolResults(&history, [
            AgentToolResult(callId: "toolu_1", name: "read_file", output: "x"),
            AgentToolResult(callId: "toolu_2", name: "grep", output: "boom", isError: true),
        ])
        let last = try XCTUnwrap(history.last)
        XCTAssertEqual(last["role"], "user")
        XCTAssertEqual(last["content"]?.arrayValue?.count, 2)
        XCTAssertEqual(last["content"]?[1]?["is_error"], true)
        XCTAssertNil(last["content"]?[0]?["is_error"])
        XCTAssertEqual(last["content"]?[0]?["tool_use_id"], "toolu_1")
    }

    func testAnthropicLeavesTheFallbackOffForOtherModels() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["content": [], "stop_reason": "end_turn"])
        _ = try await AnthropicProvider(apiKey: "k", model: "claude-haiku-4-5").send(systemPrompt: "s", history: [], tools: [], client: http)
        let request = try XCTUnwrap(http.lastRequest)
        XCTAssertNil(request.headers["anthropic-beta"])
        XCTAssertNil(JSON.parseObject(request.bodyText)["fallbacks"])
    }

    func testAnthropicSurfacesARefusalRatherThanAnEmptyAnswer() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: [
            "content": [],
            "stop_reason": "refusal",
            "stop_details": ["type": "refusal", "category": "cyber", "explanation": "Declined for safety."],
        ])
        let turn = try await AnthropicProvider(apiKey: "k").send(systemPrompt: "s", history: [], tools: [], client: http)
        XCTAssertEqual(turn.refusal, "Declined for safety.")

        XCTAssertEqual(AnthropicProvider.refusalMessage(["category": "cyber"]), "The model declined this request (cyber).")
        XCTAssertEqual(AnthropicProvider.refusalMessage(nil), "The model declined this request.")
    }

    func testAnthropicTurnsAnHTTPErrorIntoATypedException() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 401, json: ["error": ["message": "invalid api key"]])
        do {
            _ = try await AnthropicProvider(apiKey: "bad").send(systemPrompt: "s", history: [], tools: [], client: http)
            XCTFail("expected an error")
        } catch let error as AgentProviderException {
            XCTAssertEqual(error.statusCode, 401)
            XCTAssertEqual(error.message, "Claude API error 401: invalid api key")
            // A 401 is a bug in the request; retrying just burns a call.
            XCTAssertFalse(error.isRetryable)
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    func testAnErrorWithoutABodyUsesTheReasonPhrase() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 503, text: "upstream down")
        do {
            _ = try await OpenAiProvider(apiKey: "k").send(systemPrompt: "s", history: [], tools: [], client: http)
            XCTFail("expected an error")
        } catch let error as AgentProviderException {
            XCTAssertEqual(error.message, "OpenAI API error 503: Service Unavailable")
            XCTAssertTrue(error.isRetryable)
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    func testRetryableStatuses() async {
        XCTAssertTrue(AgentProviderException("x").isRetryable)
        for code in [408, 409, 429, 500, 529] {
            XCTAssertTrue(AgentProviderException("x", statusCode: code).isRetryable, "\(code)")
        }
        for code in [400, 401, 403, 404, 422] {
            XCTAssertFalse(AgentProviderException("x", statusCode: code).isRetryable, "\(code)")
        }
        XCTAssertFalse(AgentProviderException("x", retryable: false).isRetryable)
    }

    func testOpenAIParsesJSONStringToolArgumentsAndKeysResultsById() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: [
            "choices": [
                [
                    "message": [
                        "role": "assistant",
                        "content": nil,
                        "tool_calls": [
                            [
                                "id": "call_1",
                                "function": [
                                    "name": "replace_text",
                                    // Arguments arrive as a JSON *string*.
                                    "arguments": #"{"path":"a.txt","old":"one","new":"two"}"#,
                                ],
                            ],
                        ],
                    ],
                ],
            ],
            "usage": ["prompt_tokens": 3, "completion_tokens": 4],
        ])

        let provider = OpenAiProvider(apiKey: "sk-o")
        let turn = try await provider.send(systemPrompt: "sys", history: [], tools: tools(), client: http)

        let request = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(request.headers["authorization"], "Bearer sk-o")
        XCTAssertEqual(turn.toolCalls.map(\.name), ["replace_text"])
        XCTAssertEqual(turn.toolCalls.first?.arguments["old"], "one")
        XCTAssertEqual(turn.text, "")
        XCTAssertEqual(turn.inputTokens, 3)
        XCTAssertEqual(turn.outputTokens, 4)
        // The raw message is kept verbatim for history.
        XCTAssertEqual(turn.raw["content"], .null)

        // Tools go over in the function-wrapper shape.
        let sent = JSON.parseObject(request.bodyText)
        XCTAssertEqual(sent["tools"]?[0]?["type"], "function")
        XCTAssertNotNil(sent["tools"]?[0]?["function"]?["parameters"])
        XCTAssertEqual(sent["max_completion_tokens"], 16000)
        // The system prompt is a message, not a top-level field.
        XCTAssertEqual(sent["messages"]?[0]?["role"], "system")

        var history: [JSONValue] = []
        provider.appendToolResults(&history, [AgentToolResult(callId: "call_1", name: "replace_text", output: "ok")])
        let result = try XCTUnwrap(history.first)
        XCTAssertEqual(history.count, 1)
        XCTAssertEqual(result["role"], "tool")
        XCTAssertEqual(result["tool_call_id"], "call_1")
    }

    func testOpenAIBadArgumentsBecomeAnEmptyObjectAndARefusalIsSurfaced() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: [
            "choices": [["message": [
                "role": "assistant",
                "content": "",
                "refusal": "I can't help with that.",
                "tool_calls": [["id": "c", "function": ["name": "grep", "arguments": "{not json"]]],
            ]]],
        ])
        let turn = try await OpenAiProvider(apiKey: "k").send(systemPrompt: "s", history: [], tools: [], client: http)
        XCTAssertEqual(turn.toolCalls.first?.arguments, [:])
        XCTAssertEqual(turn.refusal, "I can't help with that.")
    }

    func testATransportFailureInTheLoopIsReportedAsUnreachable() async {
        let http = FakeHTTPClient()
        http.enqueue(error: HTTPError.transport("offline"))
        http.enqueue(error: HTTPError.transport("offline"))
        let agent = CodingAgent(
            provider: AnthropicProvider(apiKey: "k"),
            toolset: AgentToolset(MemoryWorkspace(files: [:])),
            client: http,
            sleep: { _ in }
        )
        let events = await agent.runToEnd("hi")
        XCTAssertEqual(events.compactMap(\.done).map(\.reason), [.error])
        XCTAssertEqual(events.compactMap(\.noticeText), ["Could not reach the model: offline"])
        XCTAssertEqual(http.requests.count, 2, "one retry")
    }
}
