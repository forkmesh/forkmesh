// The phone as a mesh agent node: a transport-free request handler that runs a
// paired peer's chat request on the on-device model. A transport (the local
// control channel or the mesh) decodes a
// frame, calls this, and sends the JSON back; nothing here opens a socket.

import FMBase
import Foundation

/// The wire shapes are OpenAI Chat Completions', the lingua franca every
/// ForkMesh runtime already speaks (the desktop's local agents, the coding
/// agent's OpenAiProvider):
///
/// Request (`ai.chat`):
///
///     {"messages": [{"role": "system"|"user"|"assistant"|"tool", "content": "...",
///                    "tool_calls": [{"id", "type": "function",
///                                    "function": {"name", "arguments": "<JSON string>"}}],
///                    "tool_call_id": "...", "name": "..."}],
///      "tools": [{"type": "function", "function": {"name", "description", "parameters"}}],
///      "max_tokens": 512, "temperature": 0.7, "top_p": 0.95, "top_k": 64}
///
/// `tools` may also use the bare `{name, description, parameters}` or
/// Anthropic's `input_schema`; `content` may be a list of `{type: "text"}` parts.
///
/// Reply:
///
///     {"ok": true, "model": "gemma-4-e2b-it-4bit",
///      "message": {"role": "assistant", "content": "...", "tool_calls": [...]},
///      "finish_reason": "stop"|"length"|"tool_calls"|"cancelled",
///      "usage": {"prompt_tokens": 12, "completion_tokens": 40},
///      "tokens_per_second": 38.5}
///
///     {"ok": false, "error": "bad_request"|"model_unavailable"|"busy"|"generation_failed",
///      "message": "..."}
///
/// Streaming (`stream`) sends `{"event": "ai.delta", "text"}` and
/// `{"event": "ai.tool_call", "tool_call"}` as they come, then exactly one
/// `{"event": "ai.done", ...reply}` or `{"event": "ai.error", "error", "message"}`.
public actor LocalInferenceService {
    public static let infoMethod = "ai.info"
    public static let chatMethod = "ai.chat"
    public static let deltaEvent = "ai.delta"
    public static let toolCallEvent = "ai.tool_call"
    public static let doneEvent = "ai.done"
    public static let errorEvent = "ai.error"

    public static let defaultMaxTokens = 512

    private let engine: LocalChatEngine
    private let resolveModel: @Sendable () async -> LocalAIModel?
    private let deviceName: String
    /// Upper bound on `max_tokens`, whatever a peer asks for.
    public let maxTokensCap: Int
    /// Upper bound on the request's message count.
    public let maxMessages: Int
    private var busy = false

    /// `resolveModel` returns the model to run, loaded (loading it if needed),
    /// or nil when the phone does not offer one; it is where the owner's
    /// policy lives (`LocalModelManager.modelForPeers`: the switch is on and
    /// the model is downloaded).
    public init(
        engine: LocalChatEngine,
        deviceName: String = "",
        maxTokensCap: Int = 2048,
        maxMessages: Int = 64,
        resolveModel: @escaping @Sendable () async -> LocalAIModel?
    ) {
        self.engine = engine
        self.deviceName = deviceName
        self.maxTokensCap = max(maxTokensCap, 1)
        self.maxMessages = max(maxMessages, 1)
        self.resolveModel = resolveModel
    }

    // MARK: - Requests

    /// A parsed `ai.chat` request.
    public struct Request: Sendable, Equatable {
        public var messages: [LocalChatMessage]
        public var tools: [LocalToolSpec]
        public var parameters: LocalGenerationParameters
    }

    public struct RequestError: Error, Equatable, Sendable {
        public let message: String
    }

    /// Validates and converts a request body.
    public nonisolated func parse(_ body: JSONObject) throws -> Request {
        guard let rawMessages = body["messages"]?.arrayValue, !rawMessages.isEmpty else {
            throw RequestError(message: "messages must be a non-empty list")
        }
        guard rawMessages.count <= maxMessages else {
            throw RequestError(message: "at most \(maxMessages) messages")
        }
        var messages: [LocalChatMessage] = []
        for (index, raw) in rawMessages.enumerated() {
            guard let object = raw.objectValue else { throw RequestError(message: "messages[\(index)] is not an object") }
            do {
                messages.append(try LocalChatMessage(openAI: object))
            } catch let error as LocalChatFormatError {
                throw RequestError(message: "messages[\(index)]: \(error.message)")
            }
        }

        var tools: [LocalToolSpec] = []
        for (index, raw) in (body["tools"]?.arrayValue ?? []).enumerated() {
            guard let object = raw.objectValue else { throw RequestError(message: "tools[\(index)] is not an object") }
            let function = object["function"]?.objectValue ?? object
            let name = function.str("name")
            guard !name.isEmpty else { throw RequestError(message: "tools[\(index)] has no name") }
            let schema = function["parameters"]?.objectValue ?? function["input_schema"]?.objectValue
                ?? ["type": "object", "properties": .object(JSONObject())]
            tools.append(LocalToolSpec(name: name, description: function.str("description"), parameters: schema))
        }

        var parameters = LocalGenerationParameters.chat
        parameters.maxTokens = min(max(body.int("max_tokens", LocalInferenceService.defaultMaxTokens), 1), maxTokensCap)
        if let temperature = body["temperature"]?.doubleValue { parameters.temperature = min(max(temperature, 0), 2) }
        if let topP = body["top_p"]?.doubleValue { parameters.topP = min(max(topP, 0), 1) }
        if let topK = body["top_k"]?.intValue { parameters.topK = max(topK, 0) }
        return Request(messages: messages, tools: tools, parameters: parameters)
    }

    // MARK: - Handling

    /// `ai.info`: what this phone offers, without loading anything.
    public func info() async -> JSONObject {
        var out = JSONObject()
        out["ok"] = true
        out["available"] = .bool(engine.availability.isAvailable)
        if let reason = engine.availability.reason { out["reason"] = .string(reason) }
        let loaded = await engine.loadedModelId()
        out["model"] = loaded.map { JSONValue.string($0) } ?? .null
        if let model = loaded.flatMap(LocalModelCatalog.model(id:)) {
            out["model_name"] = .string(model.displayName)
            out["context_length"] = .int(Int64(model.contextLength))
        }
        out["max_tokens"] = .int(Int64(maxTokensCap))
        out["busy"] = .bool(busy)
        if !deviceName.isEmpty { out["device"] = .string(deviceName) }
        return out
    }

    /// `ai.chat`, answered in one reply.
    public func respond(to body: JSONObject) async -> JSONObject {
        var final = JSONObject()
        for await event in run(body) {
            if event.str("event") == LocalInferenceService.doneEvent || event.str("event") == LocalInferenceService.errorEvent {
                final = event
            }
        }
        final["event"] = nil
        return final
    }

    /// `ai.chat`, streamed. Dropping the stream stops generation.
    public nonisolated func stream(_ body: JSONObject) -> AsyncStream<JSONObject> {
        AsyncStream { continuation in
            let task = Task {
                for await event in await self.run(body) {
                    continuation.yield(event)
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    private func run(_ body: JSONObject) -> AsyncStream<JSONObject> {
        let request: Request
        do {
            request = try parse(body)
        } catch let error as RequestError {
            return LocalInferenceService.single(LocalInferenceService.error("bad_request", error.message))
        } catch {
            return LocalInferenceService.single(LocalInferenceService.error("bad_request", "\(error)"))
        }
        if busy {
            return LocalInferenceService.single(LocalInferenceService.error("busy", "The phone is already answering a request."))
        }
        if let reason = engine.availability.reason {
            return LocalInferenceService.single(LocalInferenceService.error("model_unavailable", reason))
        }
        busy = true
        return AsyncStream { continuation in
            let task = Task {
                await self.generate(request, into: continuation)
                self.release()
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    private func release() {
        busy = false
    }

    private func generate(_ request: Request, into continuation: AsyncStream<JSONObject>.Continuation) async {
        guard let model = await resolveModel() else {
            continuation.yield(LocalInferenceService.error(
                "model_unavailable", "On-device AI is off or no model is downloaded on this phone."
            ))
            return
        }
        var text = ""
        var calls: [LocalToolCall] = []
        var stats: LocalGenerationStats?
        do {
            let events = engine.generate(messages: request.messages, tools: request.tools, parameters: request.parameters)
            for try await event in events {
                switch event {
                case .text(let chunk):
                    text += chunk
                    continuation.yield(["event": .string(LocalInferenceService.deltaEvent), "text": .string(chunk)])
                case .toolCall(let call):
                    calls.append(call)
                    continuation.yield([
                        "event": .string(LocalInferenceService.toolCallEvent),
                        "tool_call": .object(LocalInferenceService.wire(call)),
                    ])
                case .finished(let final):
                    stats = final
                }
            }
        } catch is CancellationError {
            stats = stats ?? LocalGenerationStats(promptTokens: 0, generatedTokens: 0, tokensPerSecond: 0, stopReason: .cancelled)
        } catch {
            continuation.yield(LocalInferenceService.error("generation_failed", LocalModelManager.describe(error)))
            return
        }
        if Task.isCancelled, stats == nil {
            stats = LocalGenerationStats(promptTokens: 0, generatedTokens: 0, tokensPerSecond: 0, stopReason: .cancelled)
        }
        var done = LocalInferenceService.reply(model: model, text: text, calls: calls, stats: stats)
        done["event"] = .string(LocalInferenceService.doneEvent)
        continuation.yield(done)
    }

    // MARK: - Wire shapes

    public static func wire(_ call: LocalToolCall) -> JSONObject {
        call.openAIJSON
    }

    static func reply(model: LocalAIModel, text: String, calls: [LocalToolCall], stats: LocalGenerationStats?) -> JSONObject {
        var message: JSONObject = ["role": "assistant", "content": .string(text)]
        if !calls.isEmpty { message["tool_calls"] = .array(calls.map { .object(wire($0)) }) }
        let finish: String
        switch stats?.stopReason ?? .stop {
        case .stop: finish = calls.isEmpty ? "stop" : "tool_calls"
        case .length: finish = "length"
        case .cancelled: finish = "cancelled"
        }
        var out: JSONObject = [
            "ok": true,
            "model": .string(model.id),
            "message": .object(message),
            "finish_reason": .string(finish),
        ]
        out["usage"] = .object([
            "prompt_tokens": .int(Int64(stats?.promptTokens ?? 0)),
            "completion_tokens": .int(Int64(stats?.generatedTokens ?? 0)),
        ])
        let rate = stats?.tokensPerSecond ?? 0
        out["tokens_per_second"] = .double(rate.isFinite ? (rate * 10).rounded() / 10 : 0)
        return out
    }

    static func error(_ code: String, _ message: String) -> JSONObject {
        ["ok": false, "event": .string(errorEvent), "error": .string(code), "message": .string(message)]
    }

    private static func single(_ event: JSONObject) -> AsyncStream<JSONObject> {
        AsyncStream { continuation in
            continuation.yield(event)
            continuation.finish()
        }
    }
}
