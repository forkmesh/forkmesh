// A scripted `LocalChatEngine` for tests (this module's and whoever builds on
// it, such as a Gemma agent provider in FMAgents) and for Linux, where there
// is no MLX.

import FMBase
import Foundation

public final class FakeLocalChatEngine: LocalChatEngine, @unchecked Sendable {
    /// One `generate` call as it arrived.
    public struct Request: Sendable, Equatable {
        public let messages: [LocalChatMessage]
        public let tools: [LocalToolSpec]
        public let parameters: LocalGenerationParameters
    }

    public enum Step: Sendable {
        case event(LocalChatEvent)
        case fail(LocalEngineError)
        /// Waits until the consumer stops iterating (a stop button).
        case hang
    }

    private let lock = NSLock()
    private var scripts: [[Step]] = []
    private var recorded: [Request] = []
    private var loaded: String?
    private var loadError: LocalEngineError?
    private var loads: [URL] = []
    private var unloads = 0

    public let availability: LocalEngineAvailability

    public init(availability: LocalEngineAvailability = .available) {
        self.availability = availability
    }

    /// Queues the events of the next `generate` call.
    public func enqueue(_ steps: [Step]) {
        lock.lock()
        scripts.append(steps)
        lock.unlock()
    }

    /// Queues a plain reply: `chunks`, then `finished` with these counts.
    public func enqueueReply(_ chunks: [String], promptTokens: Int = 10, tokensPerSecond: Double = 40) {
        var steps: [Step] = chunks.map { .event(.text($0)) }
        steps.append(.event(.finished(LocalGenerationStats(
            promptTokens: promptTokens, generatedTokens: chunks.count, tokensPerSecond: tokensPerSecond
        ))))
        enqueue(steps)
    }

    public func failNextLoad(_ error: LocalEngineError?) {
        lock.lock()
        loadError = error
        lock.unlock()
    }

    public var requests: [Request] {
        lock.lock()
        defer { lock.unlock() }
        return recorded
    }

    public var loadedDirectories: [URL] {
        lock.lock()
        defer { lock.unlock() }
        return loads
    }

    public var unloadCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return unloads
    }

    public func load(modelDirectory: URL, model: LocalAIModel) async throws {
        let error: LocalEngineError? = lock.withLock {
            let error = loadError
            loadError = nil
            loads.append(modelDirectory)
            if error == nil { loaded = model.id }
            return error
        }
        if let unavailable = availability.reason { throw LocalEngineError.unavailable(unavailable) }
        if let error { throw error }
    }

    public func unload() async {
        lock.withLock {
            loaded = nil
            unloads += 1
        }
    }

    public func loadedModelId() async -> String? {
        lock.withLock { loaded }
    }

    public func generate(
        messages: [LocalChatMessage],
        tools: [LocalToolSpec],
        parameters: LocalGenerationParameters
    ) -> AsyncThrowingStream<LocalChatEvent, Error> {
        lock.lock()
        recorded.append(Request(messages: messages, tools: tools, parameters: parameters))
        let steps = scripts.isEmpty ? nil : scripts.removeFirst()
        let isLoaded = loaded != nil
        lock.unlock()

        return AsyncThrowingStream { continuation in
            let task = Task {
                guard isLoaded else {
                    continuation.finish(throwing: LocalEngineError.noModelLoaded)
                    return
                }
                for step in steps ?? [] {
                    if Task.isCancelled { break }
                    switch step {
                    case .event(let event):
                        continuation.yield(event)
                        await Task.yield()
                    case .fail(let error):
                        continuation.finish(throwing: error)
                        return
                    case .hang:
                        while !Task.isCancelled {
                            try? await Task.sleep(nanoseconds: 2_000_000)
                        }
                    }
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}
