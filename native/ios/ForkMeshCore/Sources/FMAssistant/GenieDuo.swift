import FMBase
import FMLocalAI
import Foundation

public enum GenieVoiceModel: String, Equatable, Sendable {
    case quick
    case deep
}

public final class GenieDuoEngine: LocalChatEngine, @unchecked Sendable {
    public nonisolated static let quickResultChars: Int = 1200

    public let quick: LocalChatEngine
    public let deep: LocalChatEngine
    private let warmDeep: @MainActor @Sendable () async throws -> Void
    private let lock: NSLock = NSLock()
    private var answered: [GenieVoiceModel] = []

    public init(quick: LocalChatEngine, deep: LocalChatEngine, warmDeep: @escaping @MainActor @Sendable () async throws -> Void = {}) {
        self.quick = quick
        self.deep = deep
        self.warmDeep = warmDeep
    }

    public var availability: LocalEngineAvailability {
        if quick.availability.isAvailable || deep.availability.isAvailable { return .available }
        return quick.availability
    }

    public var voices: [GenieVoiceModel] {
        lock.lock()
        defer { lock.unlock() }
        return answered
    }

    public var lastVoice: GenieVoiceModel? { voices.last }

    public func load(modelDirectory: URL, model: LocalAIModel) async throws {
        try await deep.load(modelDirectory: modelDirectory, model: model)
    }

    public func unload() async {
        await deep.unload()
    }

    public func loadedModelId() async -> String? {
        if let id = await deep.loadedModelId() { return id }
        return await quick.loadedModelId()
    }

    public static func needsDeepReply(_ messages: [LocalChatMessage], _ turn: LocalChatTurn) -> Bool {
        guard turn.toolCalls.isEmpty else { return false }
        if turn.text.dartTrimmed.isEmpty { return true }
        return messages.contains { $0.role == .tool && $0.content.count > quickResultChars }
    }

    public func generate(
        messages: [LocalChatMessage],
        tools: [LocalToolSpec],
        parameters: LocalGenerationParameters
    ) -> AsyncThrowingStream<LocalChatEvent, Error> {
        AsyncThrowingStream { (continuation: AsyncThrowingStream<LocalChatEvent, Error>.Continuation) in
            let task: Task<Void, Never> = Task {
                do {
                    let (turn, voice) = try await self.turn(messages: messages, tools: tools, parameters: parameters)
                    self.note(voice)
                    if !turn.text.isEmpty { continuation.yield(.text(turn.text)) }
                    for call in turn.toolCalls { continuation.yield(.toolCall(call)) }
                    continuation.yield(.finished(turn.stats ?? LocalGenerationStats(promptTokens: 0, generatedTokens: 0, tokensPerSecond: 0)))
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: Task.isCancelled ? CancellationError() : error)
                }
            }
            continuation.onTermination = { (_: AsyncThrowingStream<LocalChatEvent, Error>.Continuation.Termination) in
                task.cancel()
            }
        }
    }

    private func turn(
        messages: [LocalChatMessage], tools: [LocalToolSpec], parameters: LocalGenerationParameters
    ) async throws -> (LocalChatTurn, GenieVoiceModel) {
        let hasDeep: Bool = deep.availability.isAvailable
        guard quick.availability.isAvailable else {
            return (try await deepTurn(messages: messages, tools: tools, parameters: parameters), .deep)
        }
        let first: LocalChatTurn
        do {
            first = try await quick.complete(messages: messages, tools: tools, parameters: parameters)
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            guard hasDeep else { throw error }
            return (try await deepTurn(messages: messages, tools: tools, parameters: parameters), .deep)
        }
        try Task.checkCancellation()
        guard hasDeep, GenieDuoEngine.needsDeepReply(messages, first) else { return (first, .quick) }
        if let second = try? await deepTurn(messages: messages, tools: tools, parameters: parameters),
            !second.text.dartTrimmed.isEmpty || !second.toolCalls.isEmpty
        {
            return (second, .deep)
        }
        try Task.checkCancellation()
        return (first, .quick)
    }

    private func deepTurn(
        messages: [LocalChatMessage], tools: [LocalToolSpec], parameters: LocalGenerationParameters
    ) async throws -> LocalChatTurn {
        try await warmDeep()
        return try await deep.complete(messages: messages, tools: tools, parameters: parameters)
    }

    private func note(_ voice: GenieVoiceModel) {
        lock.lock()
        answered.append(voice)
        if answered.count > 20 { answered.removeFirst(answered.count - 20) }
        lock.unlock()
    }
}
