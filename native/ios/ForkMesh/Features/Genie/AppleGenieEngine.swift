#if canImport(FoundationModels)
import FMAssistant
import FMBase
import FMLocalAI
import Foundation
import FoundationModels

final class GenieCallBox: @unchecked Sendable {
    private let lock: NSLock = NSLock()
    private var held: LocalToolCall?

    func record(_ call: LocalToolCall) {
        lock.lock()
        if held == nil { held = call }
        lock.unlock()
    }

    var call: LocalToolCall? {
        lock.lock()
        defer { lock.unlock() }
        return held
    }
}

struct GenieToolPause: Error {}

@available(iOS 26.0, *)
struct GenieNativeTool: Tool {
    let name: String
    let description: String
    let parameters: GenerationSchema
    let box: GenieCallBox

    func call(arguments: GeneratedContent) async throws -> String {
        box.record(LocalToolCall(id: LocalToolCall.makeId(), name: name, arguments: JSON.parseObject(arguments.jsonString)))
        throw GenieToolPause()
    }
}

@available(iOS 26.0, *)
final class AppleGenieEngine: LocalChatEngine, @unchecked Sendable {
    static let modelId: String = "apple-foundation-model"
    static let maxResultChars: Int = 1200

    var availability: LocalEngineAvailability {
        switch SystemLanguageModel.default.availability {
        case .available:
            return .available
        case .unavailable(let reason):
            switch reason {
            case .deviceNotEligible:
                return .unavailable(reason: "This iPhone cannot run Apple Intelligence.")
            case .appleIntelligenceNotEnabled:
                return .unavailable(reason: "Apple Intelligence is turned off in Settings.")
            case .modelNotReady:
                return .unavailable(reason: "Apple Intelligence is still downloading its model.")
            @unknown default:
                return .unavailable(reason: "Apple Intelligence is not available.")
            }
        }
    }

    func load(modelDirectory: URL, model: LocalAIModel) async throws {}

    func unload() async {}

    func loadedModelId() async -> String? {
        availability.isAvailable ? AppleGenieEngine.modelId : nil
    }

    static func schema(_ spec: LocalToolSpec) -> GenerationSchema? {
        let properties: JSONObject = spec.parameters["properties"]?.objectValue ?? [:]
        let required: Set<String> = Set(spec.parameters.strings("required"))
        let fields: [DynamicGenerationSchema.Property] = properties.keys.sorted().map { (key: String) -> DynamicGenerationSchema.Property in
            let field: JSONObject = properties[key]?.objectValue ?? [:]
            let about: String? = field["description"]?.stringValue
            let values: [String] = field.strings("enum")
            let kind: DynamicGenerationSchema
            if !values.isEmpty {
                kind = DynamicGenerationSchema(name: spec.name + "_" + key, description: about, anyOf: values)
            } else {
                switch field["type"]?.stringValue ?? "string" {
                case "integer": kind = DynamicGenerationSchema(type: Int.self)
                case "boolean": kind = DynamicGenerationSchema(type: Bool.self)
                case "number": kind = DynamicGenerationSchema(type: Double.self)
                default: kind = DynamicGenerationSchema(type: String.self)
                }
            }
            return DynamicGenerationSchema.Property(name: key, description: about, schema: kind, isOptional: !required.contains(key))
        }
        let root: DynamicGenerationSchema = DynamicGenerationSchema(name: spec.name, description: spec.description, properties: fields)
        return try? GenerationSchema(root: root, dependencies: [])
    }

    func generate(
        messages: [LocalChatMessage],
        tools: [LocalToolSpec],
        parameters: LocalGenerationParameters
    ) -> AsyncThrowingStream<LocalChatEvent, Error> {
        let instructions: String = messages.filter { $0.role == .system }.map(\.content).joined(separator: "\n\n")
        let clipped: [LocalChatMessage] = messages.map { (message: LocalChatMessage) -> LocalChatMessage in
            guard message.role == .tool else { return message }
            var short: LocalChatMessage = message
            short.content = OnDeviceChatTools.clip(message.content, limit: AppleGenieEngine.maxResultChars)
            return short
        }
        let prompt: String = GenieToolText.prompt(clipped)
        let chosen: [LocalToolSpec] = GenieToolText.pick(tools, for: GenieToolText.lastRequest(messages))
        let options: GenerationOptions = GenerationOptions(
            temperature: parameters.temperature, maximumResponseTokens: parameters.maxTokens)
        return AsyncThrowingStream { (continuation: AsyncThrowingStream<LocalChatEvent, Error>.Continuation) in
            let task: Task<Void, Never> = Task {
                let started: Date = Date()
                let box: GenieCallBox = GenieCallBox()
                let native: [any Tool] = chosen.compactMap { (spec: LocalToolSpec) -> (any Tool)? in
                    AppleGenieEngine.schema(spec).map { GenieNativeTool(name: spec.name, description: spec.description, parameters: $0, box: box) }
                }
                let session: LanguageModelSession = LanguageModelSession(tools: native, instructions: instructions)
                var failure: Error?
                var said: String = ""
                do {
                    said = try await session.respond(to: prompt, options: options).content
                } catch {
                    failure = error
                }
                if Task.isCancelled {
                    continuation.finish(throwing: CancellationError())
                    return
                }
                if let call = box.call {
                    continuation.yield(.toolCall(call))
                } else if let failure = failure {
                    continuation.finish(throwing: failure)
                    return
                } else {
                    continuation.yield(.text(GenieToolText.reply(said)))
                }
                let seconds: Double = Date().timeIntervalSince(started)
                continuation.yield(.finished(LocalGenerationStats(
                    promptTokens: 0, generatedTokens: 0, tokensPerSecond: 0, generateSeconds: seconds)))
                continuation.finish()
            }
            continuation.onTermination = { (_: AsyncThrowingStream<LocalChatEvent, Error>.Continuation.Termination) in
                task.cancel()
            }
        }
    }
}
#endif
