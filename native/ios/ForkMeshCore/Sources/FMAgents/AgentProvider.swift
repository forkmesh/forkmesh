// The provider seam the on-device coding agent drives: one turn in, one
// normalized `AgentTurn` out. Anthropic and OpenAI speak their documented
// wire formats over FMNet; `GemmaAgentProvider` (GemmaAgentProvider.swift)
// runs the model on this phone instead and keeps OpenAI-shaped history.

import FMApi
import FMBase
import FMNet
import Foundation

/// One call the model wants made.
public struct AgentToolCall: Equatable, Sendable {
    public let id: String
    public let name: String
    public let arguments: JSONObject

    public init(id: String, name: String, arguments: JSONObject) {
        self.id = id
        self.name = name
        self.arguments = arguments
    }
}

/// One model turn, normalized across providers.
public struct AgentTurn: Equatable, Sendable {
    /// Visible assistant text for this turn (may be empty when it only called
    /// tools).
    public let text: String
    public let toolCalls: [AgentToolCall]

    /// The provider's own assistant message, kept verbatim so it can be
    /// replayed in history. For Anthropic this preserves `thinking` blocks,
    /// which must go back unchanged on the same model.
    public let raw: JSONValue

    public let inputTokens: Int
    public let outputTokens: Int

    /// Set when the model declined the request rather than answering. Callers
    /// must check this before treating `text` as an answer.
    public let refusal: String?

    public init(
        text: String,
        toolCalls: [AgentToolCall],
        raw: JSONValue = .object([:]),
        inputTokens: Int = 0,
        outputTokens: Int = 0,
        refusal: String? = nil
    ) {
        self.text = text
        self.toolCalls = toolCalls
        self.raw = raw
        self.inputTokens = inputTokens
        self.outputTokens = outputTokens
        self.refusal = refusal
    }

    public var wantsTools: Bool { !toolCalls.isEmpty }
}

public struct AgentProviderException: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String
    public let statusCode: Int?
    /// Overrides the status-code rule. The on-device provider sets it to
    /// false: a model that is not downloaded will not appear on a retry.
    let retryable: Bool?

    public init(_ message: String, statusCode: Int? = nil, retryable: Bool? = nil) {
        self.message = message
        self.statusCode = statusCode
        self.retryable = retryable
    }

    /// Whether another attempt could plausibly succeed.
    public var isRetryable: Bool {
        if let retryable { return retryable }
        guard let code = statusCode else { return true }
        return code == 408 || code == 409 || code == 429 || (500..<600).contains(code)
    }

    public var description: String { message }
}

/// A chat provider the agent loop can drive.
///
/// The loop never sees a wire format: it hands over the running history plus
/// the tool catalogue and gets an `AgentTurn` back. Adding a provider means
/// implementing this one method. History entries are the provider's own
/// message objects.
@MainActor
public protocol AgentProvider: AnyObject {
    var id: String { get }
    var model: String { get }

    func send(systemPrompt: String, history: [JSONValue], tools: [AgentTool], client: HTTPClient) async throws -> AgentTurn

    /// Append an assistant turn to `history`.
    func appendAssistant(_ history: inout [JSONValue], _ turn: AgentTurn)

    /// Append the results of a turn's tool calls to `history`.
    func appendToolResults(_ history: inout [JSONValue], _ results: [AgentToolResult])

    /// Append a plain user message to `history`.
    func appendUser(_ history: inout [JSONValue], _ text: String)

    func appendUser(_ history: inout [JSONValue], _ text: String, images: [PromptAttachment])
}

extension AgentProvider {
    public func appendUser(_ history: inout [JSONValue], _ text: String, images: [PromptAttachment]) {
        appendUser(&history, text)
    }
}

/// How long one model call may take. A long answer from a large model takes
/// minutes, so this is only a backstop.
let agentProviderTimeout: TimeInterval = 600

/// Anthropic Messages API (`POST /v1/messages`).
///
/// Raw HTTPS rather than an SDK, following the documented wire format.
@MainActor
public final class AnthropicProvider: AgentProvider {
    /// Anthropic's most capable general model. The picker may choose another;
    /// this is only what a session starts with.
    nonisolated public static let defaultModel = "claude-opus-5-5"
    nonisolated public static let apiVersion = "2023-06-01"

    /// Models where a safety decline can be rescued by the server re-running
    /// the request on a fallback model inside the same call. Without this a
    /// decline just ends the turn.
    nonisolated static let serverSideFallbackBeta = "server-side-fallback-2026-07-01"
    nonisolated static let fallbackModels: Set<String> = ["claude-opus-5", "claude-fable-5"]

    public let apiKey: String
    public let model: String
    public let maxTokens: Int
    public let baseUrl: String

    public init(
        apiKey: String,
        model: String = AnthropicProvider.defaultModel,
        maxTokens: Int = 16000,
        baseUrl: String = "https://api.anthropic.com/v1/messages"
    ) {
        self.apiKey = apiKey
        self.model = model
        self.maxTokens = maxTokens
        self.baseUrl = baseUrl
    }

    public var id: String { "claude-api" }

    public func send(systemPrompt: String, history: [JSONValue], tools: [AgentTool], client: HTTPClient) async throws -> AgentTurn {
        let wantsFallback = AnthropicProvider.fallbackModels.contains(model)
        var body: JSONObject = [
            "model": .string(model),
            "max_tokens": .int(Int64(maxTokens)),
            "system": .string(systemPrompt),
            "messages": .array(history),
            "tools": .array(tools.map { tool in
                .object([
                    "name": .string(tool.name),
                    "description": .string(tool.description),
                    "input_schema": .object(tool.schema),
                ])
            }),
        ]
        // Thinking is deliberately not configured: current models run
        // adaptive thinking by default, and `budget_tokens` is rejected
        // outright by the Opus 5 / Sonnet 5 family. Omitting the field is the
        // one setting that is valid on every model.
        if wantsFallback { body["fallbacks"] = "default" }

        var headers: HTTPHeaders = [
            "content-type": "application/json",
            "x-api-key": apiKey,
            "anthropic-version": AnthropicProvider.apiVersion,
        ]
        if wantsFallback { headers["anthropic-beta"] = AnthropicProvider.serverSideFallbackBeta }

        let response = try await client.send(try agentPost(baseUrl, headers: headers, body: body))
        let decoded = try agentDecode(response, label: "Claude API")
        let blocks = decoded["content"]?.arrayValue ?? []

        var text = ""
        var calls: [AgentToolCall] = []
        for block in blocks {
            guard let block = block.objectValue else { continue }
            switch block["type"]?.stringValue {
            case "text":
                text += block["text"].map(lenientText) ?? ""
            case "tool_use":
                calls.append(AgentToolCall(
                    id: block["id"].map(lenientText) ?? "",
                    name: block["name"].map(lenientText) ?? "",
                    arguments: block["input"]?.objectValue ?? [:]
                ))
            default:
                break
            }
        }

        let usage = decoded["usage"]?.objectValue
        return AgentTurn(
            text: text,
            toolCalls: calls,
            raw: .object(["role": "assistant", "content": .array(blocks)]),
            inputTokens: usage.map { agentInt($0["input_tokens"]) } ?? 0,
            outputTokens: usage.map { agentInt($0["output_tokens"]) } ?? 0,
            refusal: decoded["stop_reason"] == .string("refusal")
                ? AnthropicProvider.refusalMessage(decoded["stop_details"])
                : nil
        )
    }

    static func refusalMessage(_ details: JSONValue?) -> String {
        if let details = details?.objectValue {
            let explanation = (details["explanation"].map(lenientText) ?? "").dartTrimmed
            if !explanation.isEmpty { return explanation }
            let category = (details["category"].map(lenientText) ?? "").dartTrimmed
            if !category.isEmpty { return "The model declined this request (\(category))." }
        }
        return "The model declined this request."
    }

    public func appendAssistant(_ history: inout [JSONValue], _ turn: AgentTurn) {
        history.append(turn.raw)
    }

    public func appendToolResults(_ history: inout [JSONValue], _ results: [AgentToolResult]) {
        // Every result for a turn goes back in ONE user message. Splitting
        // them across messages teaches the model to stop calling tools in
        // parallel.
        let content: [JSONValue] = results.map { result in
            var block: JSONObject = [
                "type": "tool_result",
                "tool_use_id": .string(result.callId),
                "content": .string(result.output),
            ]
            if result.isError { block["is_error"] = true }
            return .object(block)
        }
        history.append(.object(["role": "user", "content": .array(content)]))
    }

    public func appendUser(_ history: inout [JSONValue], _ text: String) {
        history.append(.object(["role": "user", "content": .string(text)]))
    }

    public func appendUser(_ history: inout [JSONValue], _ text: String, images: [PromptAttachment]) {
        if images.isEmpty { return appendUser(&history, text) }
        var content: [JSONValue] = images.map { (image: PromptAttachment) -> JSONValue in
            .object([
                "type": "image",
                "source": .object([
                    "type": "base64",
                    "media_type": .string(image.mediaType),
                    "data": .string(image.base64Data),
                ]),
            ])
        }
        content.append(.object(["type": "text", "text": .string(text)]))
        history.append(.object(["role": "user", "content": .array(content)]))
    }
}

/// OpenAI Chat Completions (`POST /v1/chat/completions`).
///
/// Kept deliberately close to `AnthropicProvider` so the loop stays
/// identical; only the request/response shapes differ.
@MainActor
public final class OpenAiProvider: AgentProvider {
    nonisolated public static let defaultModel = "gpt-5.3-codex"

    public let apiKey: String
    public let model: String
    public let maxTokens: Int
    public let baseUrl: String

    public init(
        apiKey: String,
        model: String = OpenAiProvider.defaultModel,
        maxTokens: Int = 16000,
        baseUrl: String = "https://api.openai.com/v1/chat/completions"
    ) {
        self.apiKey = apiKey
        self.model = model
        self.maxTokens = maxTokens
        self.baseUrl = baseUrl
    }

    public var id: String { "openai" }

    public func send(systemPrompt: String, history: [JSONValue], tools: [AgentTool], client: HTTPClient) async throws -> AgentTurn {
        let body: JSONObject = [
            "model": .string(model),
            "max_completion_tokens": .int(Int64(maxTokens)),
            "messages": .array([.object(["role": "system", "content": .string(systemPrompt)])] + history),
            "tools": .array(tools.map { tool in
                .object([
                    "type": "function",
                    "function": .object([
                        "name": .string(tool.name),
                        "description": .string(tool.description),
                        "parameters": .object(tool.schema),
                    ]),
                ])
            }),
        ]
        let headers: HTTPHeaders = [
            "content-type": "application/json",
            "authorization": "Bearer \(apiKey)",
        ]

        let response = try await client.send(try agentPost(baseUrl, headers: headers, body: body))
        let decoded = try agentDecode(response, label: "OpenAI API")
        let message = decoded["choices"]?.arrayValue?.first?["message"]?.objectValue

        var calls: [AgentToolCall] = []
        if let message, let toolCalls = message["tool_calls"]?.arrayValue {
            for call in toolCalls {
                guard let call = call.objectValue, let function = call["function"]?.objectValue else { continue }
                // Arguments arrive as a JSON *string*; never string-match it.
                let rawArguments = function["arguments"].flatMap { $0.isNull ? nil : lenientText($0) } ?? "{}"
                let arguments = (try? JSON.parse(rawArguments))?.objectValue ?? [:]
                calls.append(AgentToolCall(
                    id: call["id"].map(lenientText) ?? "",
                    name: function["name"].map(lenientText) ?? "",
                    arguments: arguments
                ))
            }
        }

        let usage = decoded["usage"]?.objectValue
        return AgentTurn(
            text: message.map { $0["content"].map(lenientText) ?? "" } ?? "",
            toolCalls: calls,
            raw: .object(message ?? ["role": "assistant", "content": ""]),
            inputTokens: usage.map { agentInt($0["prompt_tokens"]) } ?? 0,
            outputTokens: usage.map { agentInt($0["completion_tokens"]) } ?? 0,
            refusal: message?["refusal"]?.stringValue
        )
    }

    public func appendAssistant(_ history: inout [JSONValue], _ turn: AgentTurn) {
        history.append(turn.raw)
    }

    public func appendToolResults(_ history: inout [JSONValue], _ results: [AgentToolResult]) {
        // OpenAI takes one message per result, each keyed by tool_call_id.
        for result in results {
            history.append(.object([
                "role": "tool",
                "tool_call_id": .string(result.callId),
                "content": .string(result.output),
            ]))
        }
    }

    public func appendUser(_ history: inout [JSONValue], _ text: String) {
        history.append(.object(["role": "user", "content": .string(text)]))
    }

    public func appendUser(_ history: inout [JSONValue], _ text: String, images: [PromptAttachment]) {
        if images.isEmpty { return appendUser(&history, text) }
        var content: [JSONValue] = [.object(["type": "text", "text": .string(text)])]
        for image in images {
            content.append(.object([
                "type": "image_url",
                "image_url": .object(["url": .string("data:\(image.mediaType);base64,\(image.base64Data)")]),
            ]))
        }
        history.append(.object(["role": "user", "content": .array(content)]))
    }
}

// MARK: - Shared wire helpers

/// Null and missing are "", anything else its text.
func lenientText(_ value: JSONValue) -> String { value.isNull ? "" : value.text }

/// A number truncated, anything else 0.
func agentInt(_ value: JSONValue?) -> Int { value?.intValue ?? 0 }

func agentPost(_ url: String, headers: HTTPHeaders, body: JSONObject) throws -> HTTPRequest {
    guard let target = URL(string: url) else {
        throw AgentProviderException("Invalid model endpoint: \(url)", retryable: false)
    }
    return HTTPRequest(
        method: "POST",
        url: target,
        headers: headers,
        body: JSON.encodeData(.object(body)),
        timeout: agentProviderTimeout
    )
}

/// The body as a JSON object; a non-2xx status becomes an
/// `AgentProviderException` carrying the provider's own message.
func agentDecode(_ response: HTTPResponse, label: String) throws -> JSONObject {
    let decoded = (try? JSON.parse(response.body))?.objectValue ?? [:]
    if !response.ok {
        let reason = httpReasonPhrase(response.status)
        let message: String
        if let error = decoded["error"]?.objectValue {
            message = error["message"].flatMap { $0.isNull ? nil : $0.text } ?? reason ?? "null"
        } else {
            message = reason ?? "request failed"
        }
        throw AgentProviderException("\(label) error \(response.status): \(message)", statusCode: response.status)
    }
    return decoded
}

/// The standard reason phrase for a status, as a server's status line carries
/// it.
func httpReasonPhrase(_ status: Int) -> String? {
    switch status {
    case 400: return "Bad Request"
    case 401: return "Unauthorized"
    case 402: return "Payment Required"
    case 403: return "Forbidden"
    case 404: return "Not Found"
    case 405: return "Method Not Allowed"
    case 408: return "Request Timeout"
    case 409: return "Conflict"
    case 413: return "Payload Too Large"
    case 422: return "Unprocessable Entity"
    case 429: return "Too Many Requests"
    case 500: return "Internal Server Error"
    case 502: return "Bad Gateway"
    case 503: return "Service Unavailable"
    case 504: return "Gateway Timeout"
    case 529: return "Overloaded"
    default: return nil
    }
}
