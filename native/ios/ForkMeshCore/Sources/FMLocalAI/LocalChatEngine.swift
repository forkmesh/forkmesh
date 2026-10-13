// The seam between the core and the model runtime. The app target implements
// `LocalChatEngine` with MLX (ForkMesh/Features/LocalAI/MLXGemmaEngine.swift);
// tests and Linux use `FakeLocalChatEngine`.
//
// Shapes follow the agent loop in FMAgents/AgentProvider.swift
// (AgentTurn, AgentToolCall) so a Gemma provider for the on-device coding agent
// is a thin adapter; see docs/FMLocalAI.md.

import FMBase
import Foundation

public enum LocalChatRole: String, Sendable, Equatable {
    case system
    case user
    case assistant
    case tool
}

/// A tool the model may call. `parameters` is plain JSON Schema, the same
/// object `AgentTool.schema` holds.
public struct LocalToolSpec: Sendable, Equatable {
    public var name: String
    public var description: String
    public var parameters: JSONObject

    public init(name: String, description: String, parameters: JSONObject) {
        self.name = name
        self.description = description
        self.parameters = parameters
    }
}

/// A call the model asked for (the shape of `AgentToolCall`).
public struct LocalToolCall: Sendable, Equatable {
    public var id: String
    public var name: String
    public var arguments: JSONObject

    public init(id: String, name: String, arguments: JSONObject) {
        self.id = id
        self.name = name
        self.arguments = arguments
    }

    /// The arguments as a JSON string, the OpenAI wire shape.
    public var argumentsJSON: String { JSON.encode(arguments) }

    /// A fresh id for a call the runtime reported without one.
    public static func makeId() -> String {
        "call_" + UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
    }
}

/// One message of a conversation, in OpenAI Chat Completions terms, which is
/// what Gemma 4's chat template reads (assistant `tool_calls`, then one `tool`
/// message per result).
public struct LocalChatMessage: Sendable, Equatable {
    public var role: LocalChatRole
    public var content: String
    /// Assistant messages: the calls this turn made.
    public var toolCalls: [LocalToolCall]
    /// Tool messages: which call this answers.
    public var toolCallId: String?
    /// Tool messages: the tool's name.
    public var name: String?

    public init(
        role: LocalChatRole,
        content: String,
        toolCalls: [LocalToolCall] = [],
        toolCallId: String? = nil,
        name: String? = nil
    ) {
        self.role = role
        self.content = content
        self.toolCalls = toolCalls
        self.toolCallId = toolCallId
        self.name = name
    }

    public static func system(_ text: String) -> LocalChatMessage { LocalChatMessage(role: .system, content: text) }
    public static func user(_ text: String) -> LocalChatMessage { LocalChatMessage(role: .user, content: text) }
    public static func assistant(_ text: String, toolCalls: [LocalToolCall] = []) -> LocalChatMessage {
        LocalChatMessage(role: .assistant, content: text, toolCalls: toolCalls)
    }
    public static func tool(_ output: String, callId: String, name: String) -> LocalChatMessage {
        LocalChatMessage(role: .tool, content: output, toolCallId: callId, name: name)
    }
}

/// A message or tool call that is not valid OpenAI Chat Completions JSON.
public struct LocalChatFormatError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { message }
}

extension LocalChatMessage {
    /// From an OpenAI Chat Completions message (what the coding agent's
    /// OpenAiProvider keeps in history, and what peers send): `content` may be
    /// a string, null, or `{type: "text"}` parts; `tool_calls[].function.
    /// arguments` a JSON string or an object.
    public init(openAI object: JSONObject) throws {
        guard let role = LocalChatRole(rawValue: object.str("role")) else {
            throw LocalChatFormatError("role must be system, user, assistant or tool")
        }
        var calls: [LocalToolCall] = []
        for (index, call) in object.maps("tool_calls").enumerated() {
            do {
                calls.append(try LocalToolCall(openAI: call))
            } catch let error as LocalChatFormatError {
                throw LocalChatFormatError("tool_calls[\(index)]: \(error.message)")
            }
        }
        self.init(
            role: role,
            content: LocalChatMessage.text(of: object["content"]),
            toolCalls: calls,
            toolCallId: object.optStr("tool_call_id"),
            name: object.optStr("name")
        )
    }

    /// The OpenAI Chat Completions JSON of this message (arguments as a JSON
    /// string, as OpenAI sends them).
    public var openAIJSON: JSONObject {
        var out: JSONObject = ["role": .string(role.rawValue), "content": .string(content)]
        if !toolCalls.isEmpty { out["tool_calls"] = .array(toolCalls.map { .object($0.openAIJSON) }) }
        if let toolCallId { out["tool_call_id"] = .string(toolCallId) }
        if let name { out["name"] = .string(name) }
        return out
    }

    static func text(of content: JSONValue?) -> String {
        guard let content else { return "" }
        if let string = content.stringValue { return string }
        if let parts = content.arrayValue {
            return parts.compactMap { part -> String? in
                guard let object = part.objectValue, object.str("type") == "text" else { return part.stringValue }
                return object.str("text")
            }.joined()
        }
        return content.isNull ? "" : content.text
    }
}

extension LocalToolCall {
    /// `{id, type: "function", function: {name, arguments}}`, arguments a JSON
    /// string or an object; never string-matched. A missing id gets a fresh one.
    public init(openAI call: JSONObject) throws {
        let function = call["function"]?.objectValue ?? call
        let name = function.str("name")
        guard !name.isEmpty else { throw LocalChatFormatError("no function name") }
        var arguments = JSONObject()
        if let object = function["arguments"]?.objectValue {
            arguments = object
        } else if let string = function["arguments"]?.stringValue, !string.isEmpty {
            arguments = JSON.parseObject(string)
        }
        let id = call.str("id")
        self.init(id: id.isEmpty ? LocalToolCall.makeId() : id, name: name, arguments: arguments)
    }

    public var openAIJSON: JSONObject {
        [
            "id": .string(id),
            "type": "function",
            "function": .object(["name": .string(name), "arguments": .string(argumentsJSON)]),
        ]
    }
}

public struct LocalGenerationParameters: Sendable, Equatable {
    public var maxTokens: Int
    /// Gemma's own sampling defaults (generation_config.json): 1.0 / 0.95 / 64.
    public var temperature: Double
    public var topP: Double
    public var topK: Int
    public var seed: UInt64?

    public init(maxTokens: Int = 1024, temperature: Double = 1.0, topP: Double = 0.95, topK: Int = 64, seed: UInt64? = nil) {
        self.maxTokens = maxTokens
        self.temperature = temperature
        self.topP = topP
        self.topK = topK
        self.seed = seed
    }

    public static let chat = LocalGenerationParameters()
    /// Tool use wants determinism more than variety.
    public static let agent = LocalGenerationParameters(maxTokens: 2048, temperature: 0.3, topP: 0.95, topK: 64)
}

public enum LocalStopReason: String, Sendable, Equatable {
    /// End of turn.
    case stop
    /// Hit `maxTokens`.
    case length
    case cancelled
}

public struct LocalGenerationStats: Sendable, Equatable {
    public var promptTokens: Int
    public var generatedTokens: Int
    /// Decode speed, generated tokens / generation time.
    public var tokensPerSecond: Double
    /// Prompt processing (prefill) time, seconds.
    public var promptSeconds: Double
    public var generateSeconds: Double
    public var stopReason: LocalStopReason

    public init(
        promptTokens: Int,
        generatedTokens: Int,
        tokensPerSecond: Double,
        promptSeconds: Double = 0,
        generateSeconds: Double = 0,
        stopReason: LocalStopReason = .stop
    ) {
        self.promptTokens = promptTokens
        self.generatedTokens = generatedTokens
        self.tokensPerSecond = tokensPerSecond
        self.promptSeconds = promptSeconds
        self.generateSeconds = generateSeconds
        self.stopReason = stopReason
    }
}

/// What a generation streams. `finished` comes last, once.
public enum LocalChatEvent: Sendable, Equatable {
    case text(String)
    case toolCall(LocalToolCall)
    case finished(LocalGenerationStats)
}

public enum LocalEngineAvailability: Sendable, Equatable {
    case available
    case unavailable(reason: String)

    public var isAvailable: Bool { self == .available }

    public var reason: String? {
        if case .unavailable(let reason) = self { return reason }
        return nil
    }
}

/// The on-device model runtime.
///
/// One model is loaded at a time; `load` replaces whatever was loaded.
/// Generations are not concurrent: callers (the view models, the inference
/// service, an agent) run one at a time. Cancelling the task that iterates a
/// `generate` stream, or dropping the stream, stops generation.
public protocol LocalChatEngine: AnyObject, Sendable {
    /// Whether this build and device can run models at all. Constant for the
    /// process (Simulator, runtime missing from the build).
    var availability: LocalEngineAvailability { get }

    /// Loads the snapshot in `modelDirectory` (config, weights, tokenizer,
    /// chat template). Throws when the files do not load.
    func load(modelDirectory: URL, model: LocalAIModel) async throws

    /// Releases the weights and the runtime's buffer cache.
    func unload() async

    /// The catalog id of the loaded model, nil when none is.
    func loadedModelId() async -> String?

    /// Streams one assistant turn. Throws (through the stream) when no model
    /// is loaded or generation fails.
    func generate(
        messages: [LocalChatMessage],
        tools: [LocalToolSpec],
        parameters: LocalGenerationParameters
    ) -> AsyncThrowingStream<LocalChatEvent, Error>
}

public enum LocalEngineError: Error, Equatable, Sendable, CustomStringConvertible {
    case noModelLoaded
    case unavailable(String)
    case loadFailed(String)
    case generationFailed(String)

    public var description: String {
        switch self {
        case .noModelLoaded: return "No on-device model is loaded."
        case .unavailable(let reason): return reason
        case .loadFailed(let detail): return "The model did not load: \(detail)"
        case .generationFailed(let detail): return "The model stopped with an error: \(detail)"
        }
    }
}

/// One whole assistant turn, collected from a stream: the shape of
/// `AgentTurn` (text, toolCalls, token counts).
public struct LocalChatTurn: Sendable, Equatable {
    public var text: String
    public var toolCalls: [LocalToolCall]
    public var stats: LocalGenerationStats?

    public init(text: String, toolCalls: [LocalToolCall], stats: LocalGenerationStats?) {
        self.text = text
        self.toolCalls = toolCalls
        self.stats = stats
    }

    public var wantsTools: Bool { !toolCalls.isEmpty }
    public var inputTokens: Int { stats?.promptTokens ?? 0 }
    public var outputTokens: Int { stats?.generatedTokens ?? 0 }

    /// The assistant message to append to history for this turn.
    public var message: LocalChatMessage { .assistant(text, toolCalls: toolCalls) }
}

extension LocalChatEngine {
    /// Runs one turn to completion (what an agent provider's `send` does).
    public func complete(
        messages: [LocalChatMessage],
        tools: [LocalToolSpec] = [],
        parameters: LocalGenerationParameters = .agent
    ) async throws -> LocalChatTurn {
        var text = ""
        var calls: [LocalToolCall] = []
        var stats: LocalGenerationStats?
        for try await event in generate(messages: messages, tools: tools, parameters: parameters) {
            switch event {
            case .text(let chunk): text += chunk
            case .toolCall(let call): calls.append(call)
            case .finished(let final): stats = final
            }
        }
        return LocalChatTurn(text: text, toolCalls: calls, stats: stats)
    }
}

// MARK: - Chat template shapes

/// The `[String: any Sendable]` values MLX's `UserInput(messages:tools:)`
/// hands to the Jinja chat template. Built here, where Linux tests them, so
/// the app target only passes them through.
public enum LocalChatTemplate {
    /// OpenAI-style message dictionaries, which Gemma 4's template reads:
    /// `{role, content}`, assistant `tool_calls: [{id, type, function: {name,
    /// arguments}}]` with arguments as an object, and `{role: "tool",
    /// tool_call_id, name, content}` for results.
    public static func messages(_ messages: [LocalChatMessage]) -> [[String: any Sendable]] {
        messages.map(message)
    }

    public static func message(_ message: LocalChatMessage) -> [String: any Sendable] {
        var out: [String: any Sendable] = [
            "role": message.role.rawValue,
            "content": message.content,
        ]
        if !message.toolCalls.isEmpty {
            out["tool_calls"] = message.toolCalls.map { call -> [String: any Sendable] in
                [
                    "id": call.id,
                    "type": "function",
                    "function": [
                        "name": call.name,
                        "arguments": dictionary(call.arguments),
                    ] as [String: any Sendable],
                ]
            }
        }
        if let id = message.toolCallId { out["tool_call_id"] = id }
        if let name = message.name { out["name"] = name }
        return out
    }

    /// `{type: "function", function: {name, description, parameters}}`, the
    /// tool shape Gemma 4's template (and MLXLMCommon's `ToolSpec`) expects.
    public static func tools(_ tools: [LocalToolSpec]) -> [[String: any Sendable]] {
        tools.map { tool -> [String: any Sendable] in
            [
                "type": "function",
                "function": [
                    "name": tool.name,
                    "description": tool.description,
                    "parameters": dictionary(tool.parameters),
                ] as [String: any Sendable],
            ]
        }
    }

    /// JSON as Swift values: String, Int, Double, Bool, arrays, dictionaries.
    /// Object keys holding null are dropped (the template tests for presence);
    /// a null inside an array becomes a nil Optional, which Jinja reads as none.
    public static func dictionary(_ object: JSONObject) -> [String: any Sendable] {
        var out: [String: any Sendable] = [:]
        for (key, value) in object where !value.isNull {
            out[key] = sendable(value)
        }
        return out
    }

    public static func sendable(_ value: JSONValue) -> any Sendable {
        switch value {
        case .null: return Optional<String>.none as any Sendable
        case .bool(let bool): return bool
        case .int(let int): return Int(truncatingIfNeeded: int)
        case .double(let double): return double
        case .string(let string): return string
        case .array(let items): return items.map { sendable($0) }
        case .object(let object): return dictionary(object)
        }
    }
}
