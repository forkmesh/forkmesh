// The on-device model runtime: Gemma 4 through MLX (ml-explore/mlx-swift-lm,
// MLXLLM + MLXLMCommon) with swift-transformers' tokenizer. The one sanctioned
// third-party dependency of the app (native/ios/README.md, "Dependency
// policy"). FMLocalAI (docs/FMLocalAI.md) owns everything but this file.
//
// Written against mlx-swift-lm 3.31.4 and swift-transformers 1.3.4. Every MLX
// line is inside `#if canImport(MLXLLM)`, so the app still builds (with an
// engine that reports itself unavailable) if the packages are removed.

import FMBase
import FMLocalAI
import Foundation
import os
#if canImport(UIKit)
import UIKit
#endif

#if canImport(MLXLLM) && canImport(MLXLMCommon) && canImport(Tokenizers)
import MLXLLM
import MLXLMCommon
import Tokenizers
#if canImport(MLX)
import MLX
#endif

/// swift-transformers' tokenizer seen through MLXLMCommon's protocol: what the
/// `#adaptHuggingFaceTokenizer` macro of MLXHuggingFace generates, written out
/// so the app builds no macro target.
struct TransformersTokenizerBridge: MLXLMCommon.Tokenizer {
    private let upstream: any Tokenizers.Tokenizer

    init(_ upstream: any Tokenizers.Tokenizer) {
        self.upstream = upstream
    }

    func encode(text: String, addSpecialTokens: Bool) -> [Int] {
        upstream.encode(text: text, addSpecialTokens: addSpecialTokens)
    }

    // swift-transformers names it `decode(tokens:)`.
    func decode(tokenIds: [Int], skipSpecialTokens: Bool) -> String {
        upstream.decode(tokens: tokenIds, skipSpecialTokens: skipSpecialTokens)
    }

    func convertTokenToId(_ token: String) -> Int? {
        upstream.convertTokenToId(token)
    }

    func convertIdToToken(_ id: Int) -> String? {
        upstream.convertIdToToken(id)
    }

    var bosToken: String? { upstream.bosToken }
    var eosToken: String? { upstream.eosToken }
    var unknownToken: String? { upstream.unknownToken }

    func applyChatTemplate(
        messages: [[String: any Sendable]],
        tools: [[String: any Sendable]]?,
        additionalContext: [String: any Sendable]?
    ) throws -> [Int] {
        do {
            return try upstream.applyChatTemplate(messages: messages, tools: tools, additionalContext: additionalContext)
        } catch Tokenizers.TokenizerError.missingChatTemplate {
            throw MLXLMCommon.TokenizerError.missingChatTemplate
        }
    }
}

/// Loads tokenizer.json, tokenizer_config.json and chat_template.jinja from the
/// snapshot directory (`#huggingFaceTokenizerLoader()`, written out).
struct TransformersTokenizerLoader: MLXLMCommon.TokenizerLoader {
    func load(from directory: URL) async throws -> any MLXLMCommon.Tokenizer {
        let upstream = try await Tokenizers.AutoTokenizer.from(modelFolder: directory)
        return TransformersTokenizerBridge(upstream)
    }
}

/// The weights are already on disk (FMLocalAI downloads them); a directory
/// configuration never asks a downloader for anything.
struct LocalDirectoryOnlyDownloader: MLXLMCommon.Downloader {
    struct NotAvailable: LocalizedError {
        var errorDescription: String? { "Models are downloaded by the app, not by MLX." }
    }

    func download(
        id: String,
        revision: String?,
        matching patterns: [String],
        useLatest: Bool,
        progressHandler: @Sendable @escaping (Progress) -> Void
    ) async throws -> URL {
        throw NotAvailable()
    }
}

final class MLXGemmaEngine: LocalChatEngine, @unchecked Sendable {
    /// MLX keeps freed GPU buffers for reuse up to this many bytes. Its default
    /// is the whole memory limit, which on a phone holds gigabytes the app no
    /// longer uses until iOS ends it.
    static let bufferCacheLimit = 64 * 1024 * 1024
    static let prefillStepSize = 256
    static let minimumFreeBytesToGenerate = 400 * 1024 * 1024

    private let lock = NSLock()
    private var container: MLXLMCommon.ModelContainer?
    private var modelId: String?
    private var running: [UUID: Task<Void, Never>] = [:]
    private var observers: [NSObjectProtocol] = []

    let availability: LocalEngineAvailability

    init() {
        #if targetEnvironment(simulator)
        availability = .unavailable(
            reason: "On-device models need a real iPhone or iPad: MLX runs on the GPU through Metal, which the Simulator does not provide."
        )
        #else
        availability = .available
        #endif
        #if canImport(UIKit)
        // iOS ends an app that submits GPU work from the background: stop
        // generating the moment the app leaves the screen.
        let stops: [Notification.Name] = [
            UIApplication.willResignActiveNotification,
            UIApplication.didEnterBackgroundNotification,
        ]
        for name in stops {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: nil) { [weak self] _ in
                self?.cancelAll()
            })
        }
        observers.append(NotificationCenter.default.addObserver(
            forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: nil
        ) { [weak self] _ in
            self?.cancelAll()
            #if canImport(MLX)
            Memory.clearCache()
            #endif
        })
        #endif
    }

    deinit {
        for observer in observers {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    static func freeBytes() -> Int? {
        #if os(iOS)
        let free: Int = os_proc_available_memory()
        return free > 0 ? free : nil
        #else
        return nil
        #endif
    }

    func load(modelDirectory: URL, model: LocalAIModel) async throws {
        if let reason = availability.reason {
            throw LocalEngineError.unavailable(reason)
        }
        await unload()
        #if canImport(MLX)
        Memory.cacheLimit = MLXGemmaEngine.bufferCacheLimit
        #endif
        // Gemma 4 ends a turn with <turn|>; MLXLLM's own registry entry for
        // these repositories adds it the same way. Tool calls use the Gemma 4
        // syntax (<|tool_call>call:name{...}<tool_call|>).
        let configuration = MLXLMCommon.ModelConfiguration(
            directory: modelDirectory,
            extraEOSTokens: ["<turn|>"],
            toolCallFormat: .gemma4
        )
        do {
            let loaded = try await LLMModelFactory.shared.loadContainer(
                from: LocalDirectoryOnlyDownloader(),
                using: TransformersTokenizerLoader(),
                configuration: configuration
            )
            lock.withLock {
                container = loaded
                modelId = model.id
            }
        } catch {
            #if canImport(MLX)
            Memory.clearCache()
            #endif
            throw LocalEngineError.loadFailed(error.localizedDescription)
        }
    }

    func unload() async {
        cancelAll()
        lock.withLock {
            container = nil
            modelId = nil
        }
        #if canImport(MLX)
        Memory.clearCache()
        #endif
    }

    func loadedModelId() async -> String? {
        lock.withLock { modelId }
    }

    func generate(
        messages: [LocalChatMessage],
        tools: [LocalToolSpec],
        parameters: LocalGenerationParameters
    ) -> AsyncThrowingStream<LocalChatEvent, Error> {
        let loaded: MLXLMCommon.ModelContainer? = lock.withLock { container }
        let chat: [[String: any Sendable]] = LocalChatTemplate.messages(messages)
        let toolSpecs: [[String: any Sendable]]? = tools.isEmpty ? nil : LocalChatTemplate.tools(tools)
        var tuned = MLXLMCommon.GenerateParameters(
            maxTokens: parameters.maxTokens,
            temperature: Float(parameters.temperature),
            topP: Float(parameters.topP),
            topK: parameters.topK,
            seed: parameters.seed
        )
        tuned.prefillStepSize = MLXGemmaEngine.prefillStepSize
        let generateParameters = tuned
        let id = UUID()

        return AsyncThrowingStream { continuation in
            let task = Task {
                defer { self.finished(id) }
                guard let loaded else {
                    continuation.finish(throwing: LocalEngineError.noModelLoaded)
                    return
                }
                if let free = MLXGemmaEngine.freeBytes(), free < MLXGemmaEngine.minimumFreeBytesToGenerate {
                    #if canImport(MLX)
                    Memory.clearCache()
                    #endif
                    let left = ByteCountFormatter.string(fromByteCount: Int64(free), countStyle: .memory)
                    continuation.finish(throwing: LocalEngineError.generationFailed(
                        "iOS left only \(left) for ForkMesh, too little to run the model safely. Close other apps or start a new chat."))
                    return
                }
                do {
                    let stream: AsyncStream<MLXLMCommon.Generation> = try await loaded.perform { (context: MLXLMCommon.ModelContext) -> AsyncStream<MLXLMCommon.Generation> in
                        let input = MLXLMCommon.UserInput(messages: chat, tools: toolSpecs)
                        let prepared = try await context.processor.prepare(input: input)
                        return try MLXLMCommon.generate(
                            input: prepared, parameters: generateParameters, context: context, tools: toolSpecs
                        )
                    }
                    for await item in stream {
                        if Task.isCancelled { break }
                        if let text = item.chunk {
                            continuation.yield(.text(text))
                        } else if let call = item.toolCall {
                            continuation.yield(.toolCall(MLXGemmaEngine.convert(call)))
                        } else if let info = item.info {
                            continuation.yield(.finished(MLXGemmaEngine.stats(info)))
                        }
                    }
                    continuation.finish()
                } catch is CancellationError {
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: LocalEngineError.generationFailed(error.localizedDescription))
                }
            }
            self.started(id, task)
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    private func started(_ id: UUID, _ task: Task<Void, Never>) {
        lock.withLock { running[id] = task }
    }

    private func finished(_ id: UUID) {
        _ = lock.withLock { running.removeValue(forKey: id) }
    }

    private func cancelAll() {
        let tasks: [Task<Void, Never>] = lock.withLock { Array(running.values) }
        for task in tasks {
            task.cancel()
        }
    }

    /// MLX's tool call (arguments as its own JSONValue) as FMLocalAI's.
    static func convert(_ call: MLXLMCommon.ToolCall) -> LocalToolCall {
        var arguments = JSONObject()
        if let data = try? JSONEncoder().encode(call.function.arguments),
           let parsed = try? JSON.parse(data),
           let object = parsed.objectValue {
            arguments = object
        }
        let id = call.id ?? ""
        return LocalToolCall(id: id.isEmpty ? LocalToolCall.makeId() : id, name: call.function.name, arguments: arguments)
    }

    static func stats(_ info: MLXLMCommon.GenerateCompletionInfo) -> LocalGenerationStats {
        var reason = LocalStopReason.stop
        if case .length = info.stopReason { reason = .length }
        if case .cancelled = info.stopReason { reason = .cancelled }
        let rate = info.tokensPerSecond
        return LocalGenerationStats(
            promptTokens: info.promptTokenCount,
            generatedTokens: info.generationTokenCount,
            tokensPerSecond: rate.isFinite ? rate : 0,
            promptSeconds: info.promptTime,
            generateSeconds: info.generateTime,
            stopReason: reason
        )
    }
}

#else

/// This build has no MLX: the settings screen says so and nothing loads.
final class MLXGemmaEngine: LocalChatEngine, @unchecked Sendable {
    let availability = LocalEngineAvailability.unavailable(
        reason: "This build of ForkMesh does not include the on-device model runtime."
    )

    init() {}

    func load(modelDirectory: URL, model: LocalAIModel) async throws {
        throw LocalEngineError.unavailable(availability.reason ?? "")
    }

    func unload() async {}

    func loadedModelId() async -> String? { nil }

    func generate(
        messages: [LocalChatMessage],
        tools: [LocalToolSpec],
        parameters: LocalGenerationParameters
    ) -> AsyncThrowingStream<LocalChatEvent, Error> {
        let reason = availability.reason ?? ""
        return AsyncThrowingStream { continuation in
            continuation.finish(throwing: LocalEngineError.unavailable(reason))
        }
    }
}

#endif
