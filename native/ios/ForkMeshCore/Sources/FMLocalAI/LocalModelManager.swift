// The Local AI settings screen's model, and the one owner of "which model is
// on disk / in memory". New for the native app.

import FMBase
import FMNet
import Foundation
import Observation

public enum LocalModelState: Sendable, Equatable {
    case notDownloaded
    /// `progress` 0...1 over the whole snapshot; `bytes` on disk so far.
    case downloading(progress: Double, bytes: Int64)
    case downloaded
    case loading
    case ready
    case failed(String)

    public var isDownloading: Bool {
        if case .downloading = self { return true }
        return false
    }

    public var isOnDisk: Bool {
        switch self {
        case .downloaded, .loading, .ready: return true
        default: return false
        }
    }
}

/// Downloads, deletes, loads and unloads the catalog models, and persists the
/// user's choices:
///
/// - `localai/model` (String): the selected catalog id;
/// - `localai/enabled` (Bool): the "On-device AI" switch. Off unloads the
///   model, and nothing (chat, agents, peers) may start it.
///
/// One model is in memory at a time. Every other feature reaches the model
/// through `ensureReady()` and `engine`, never by loading on its own.
@MainActor
@Observable
public final class LocalModelManager {
    public static let modelKey = "localai/model"
    public static let enabledKey = "localai/enabled"

    @ObservationIgnored public let engine: LocalChatEngine
    @ObservationIgnored public let store: LocalModelStore
    @ObservationIgnored private let downloader: HuggingFaceSnapshotDownloader
    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private var downloads: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var loadTask: Task<Void, Never>?

    public let catalog: [LocalAIModel]
    public private(set) var device: DeviceProfile
    public private(set) var states: [String: LocalModelState] = [:]
    /// Latest progress of a running download, with the file it is on.
    public private(set) var downloadProgress: [String: SnapshotProgress] = [:]
    /// Bytes each model takes on disk (partial downloads included).
    public private(set) var diskUsage: [String: Int64] = [:]
    /// Bytes of an unfinished download a new one would resume from.
    public private(set) var resumableBytes: [String: Int64] = [:]
    public private(set) var availableDiskBytes: Int64?
    /// Models whose snapshot is complete on disk, whatever `states` says (a
    /// model that failed to load is still downloaded).
    public private(set) var downloadedIds: Set<String> = []
    public private(set) var isEnabled: Bool
    public private(set) var selectedModelId: String
    public private(set) var loadedModelId: String?

    public init(
        engine: LocalChatEngine,
        store: LocalModelStore,
        downloader: HuggingFaceSnapshotDownloader,
        prefs: KeyValueStore,
        device: DeviceProfile,
        catalog: [LocalAIModel] = LocalModelCatalog.all
    ) {
        self.engine = engine
        self.store = store
        self.downloader = downloader
        self.prefs = prefs
        self.device = device
        self.catalog = catalog.isEmpty ? LocalModelCatalog.all : catalog
        isEnabled = prefs.bool(forKey: LocalModelManager.enabledKey) ?? false
        let stored = prefs.string(forKey: LocalModelManager.modelKey) ?? ""
        let models = self.catalog
        if models.contains(where: { $0.id == stored }) {
            selectedModelId = stored
        } else {
            let recommended = LocalModelCatalog.recommendedModel(for: device, in: models)
            selectedModelId = (recommended ?? models.first { $0.id == LocalModelCatalog.defaultModel.id } ?? models[0]).id
        }
        refresh()
    }

    // MARK: - Reading

    public var selectedModel: LocalAIModel {
        catalog.first { $0.id == selectedModelId } ?? catalog[0]
    }

    public var loadedModel: LocalAIModel? {
        guard let id = loadedModelId else { return nil }
        return catalog.first { $0.id == id }
    }

    public var engineAvailability: LocalEngineAvailability { engine.availability }

    public func state(for model: LocalAIModel) -> LocalModelState {
        states[model.id] ?? .notDownloaded
    }

    /// Device fit, with the runtime's own availability folded in.
    public func fit(for model: LocalAIModel) -> LocalModelFit {
        if let reason = engine.availability.reason { return .unsupported(reason: reason) }
        return device.fit(for: model)
    }

    public var recommendedModel: LocalAIModel? {
        if engine.availability.reason != nil { return nil }
        return LocalModelCatalog.recommendedModel(for: device, in: catalog)
    }

    public func canDownload(_ model: LocalAIModel) -> Bool {
        fit(for: model).canRun && !isDownloaded(model) && !state(for: model).isDownloading
    }

    public func isDownloaded(_ model: LocalAIModel) -> Bool {
        downloadedIds.contains(model.id)
    }

    public var totalDiskUsage: Int64 { diskUsage.values.reduce(0, +) }

    /// Whether chat and agents can use the model now, or after a load.
    public var isUsable: Bool {
        isEnabled && engine.availability.isAvailable && isDownloaded(selectedModel)
    }

    // MARK: - Choices

    public func setEnabled(_ enabled: Bool) {
        isEnabled = enabled
        prefs.set(enabled, forKey: LocalModelManager.enabledKey)
        if !enabled, loadedModelId != nil {
            Task { await self.unload() }
        }
    }

    public func select(_ model: LocalAIModel) {
        guard catalog.contains(model) else { return }
        selectedModelId = model.id
        prefs.set(model.id, forKey: LocalModelManager.modelKey)
    }

    /// New memory figures (the app passes `os_proc_available_memory()`).
    public func updateDevice(_ profile: DeviceProfile) {
        device = profile
    }

    // MARK: - Disk

    /// Re-reads what is on disk. Running downloads and loads keep their state.
    public func refresh() {
        for model in catalog {
            diskUsage[model.id] = store.diskUsage(for: model)
            resumableBytes[model.id] = store.partialBytes(for: model)
            let complete = store.isComplete(model)
            if complete {
                downloadedIds.insert(model.id)
            } else {
                downloadedIds.remove(model.id)
            }
            let current = states[model.id]
            if let current, current.isDownloading || current == .loading { continue }
            if loadedModelId == model.id {
                states[model.id] = .ready
            } else if complete {
                states[model.id] = .downloaded
            } else {
                states[model.id] = .notDownloaded
            }
        }
        availableDiskBytes = store.availableCapacity()
    }

    /// Starts (or resumes) a download. Progress lands in `states` and
    /// `downloadProgress`.
    public func download(_ model: LocalAIModel) {
        guard downloads[model.id] == nil, !isDownloaded(model) else { return }
        let fit = fit(for: model)
        guard fit.canRun else {
            states[model.id] = .failed(fit.message ?? "\(model.displayName) cannot run on this device.")
            return
        }
        let resumable = store.partialBytes(for: model)
        let startFraction = model.approxDownloadBytes > 0 ? Double(resumable) / Double(model.approxDownloadBytes) : 0
        states[model.id] = .downloading(progress: min(startFraction, 1), bytes: resumable)
        let id = model.id
        let downloader = self.downloader
        // The manager lives as long as the app; holding it until the
        // download ends is fine.
        downloads[id] = Task {
            do {
                try await downloader.download(model) { progress in
                    Task { @MainActor in self.apply(progress, to: id) }
                }
                self.finishDownload(id, error: nil)
            } catch {
                // Whatever a cancelled transfer throws, it was a cancel.
                self.finishDownload(id, error: Task.isCancelled ? CancellationError() : error)
            }
        }
    }

    public func cancelDownload(_ model: LocalAIModel) {
        downloads[model.id]?.cancel()
    }

    /// Waits for a running download to end (tests, and the settings screen's
    /// "done" state).
    public func waitForDownload(_ model: LocalAIModel) async {
        await downloads[model.id]?.value
    }

    private func apply(_ progress: SnapshotProgress, to id: String) {
        guard states[id]?.isDownloading == true else { return }
        downloadProgress[id] = progress
        states[id] = .downloading(progress: progress.fraction, bytes: progress.completedBytes)
    }

    private func finishDownload(_ id: String, error: Error?) {
        downloads[id] = nil
        downloadProgress[id] = nil
        states[id] = nil
        refresh()
        guard let error else { return }
        if error is CancellationError || (error as? HTTPError) == .cancelled {
            return
        }
        states[id] = .failed(LocalModelManager.describe(error))
    }

    /// Removes the model from disk (unloading it first).
    public func delete(_ model: LocalAIModel) async {
        if let running = downloads[model.id] {
            running.cancel()
            await running.value
        }
        if loadedModelId == model.id {
            await unload()
        }
        do {
            try store.delete(model)
            states[model.id] = nil
            refresh()
        } catch {
            refresh()
            states[model.id] = .failed(LocalModelManager.describe(error))
        }
    }

    // MARK: - Memory

    /// Loads `model`, unloading any other first. Failures land in `states`.
    public func load(_ model: LocalAIModel) async {
        if let running = loadTask {
            await running.value
        }
        if loadedModelId == model.id { return }
        let task = Task { await self.performLoad(model) }
        loadTask = task
        await task.value
        if loadTask == task { loadTask = nil }
    }

    private func performLoad(_ model: LocalAIModel) async {
        if let reason = engine.availability.reason {
            states[model.id] = .failed(reason)
            return
        }
        let fit = device.fit(for: model)
        if case .tooLarge(let reason) = fit {
            states[model.id] = .failed(reason)
            return
        }
        guard store.isComplete(model) else {
            states[model.id] = .failed(LocalModelError.notDownloaded(model.displayName).description)
            return
        }
        if let previous = loadedModelId {
            await engine.unload()
            loadedModelId = nil
            states[previous] = nil
        }
        states[model.id] = .loading
        var failure: String?
        do {
            try await engine.load(modelDirectory: store.directory(for: model), model: model)
            loadedModelId = model.id
        } catch {
            loadedModelId = nil
            failure = LocalModelManager.describe(error)
        }
        states[model.id] = nil
        refresh()
        if let failure { states[model.id] = .failed(failure) }
    }

    public func unload() async {
        await engine.unload()
        let previous = loadedModelId
        loadedModelId = nil
        if let previous { states[previous] = nil }
        refresh()
    }

    /// The selected model, loaded: what chat, agents and peers call before
    /// generating. Throws when the switch is off, the runtime cannot run
    /// here, or the model is not downloaded or does not load.
    @discardableResult
    public func ensureReady() async throws -> LocalAIModel {
        guard isEnabled else { throw LocalModelError.disabled }
        if let reason = engine.availability.reason { throw LocalModelError.engineUnavailable(reason) }
        let model = selectedModel
        if loadedModelId == model.id { return model }
        guard store.isComplete(model) else { throw LocalModelError.notDownloaded(model.displayName) }
        await load(model)
        guard loadedModelId == model.id else {
            if case .failed(let message) = state(for: model) {
                throw LocalEngineError.loadFailed(message)
            }
            throw LocalEngineError.noModelLoaded
        }
        return model
    }

    /// For `LocalInferenceService`: the model a paired peer may use, loading
    /// it if needed, or nil when the switch is off or nothing can load.
    public func modelForPeers() async -> LocalAIModel? {
        try? await ensureReady()
    }

    /// The mesh-node handler over this manager's engine and model.
    public func makeInferenceService(deviceName: String = "") -> LocalInferenceService {
        LocalInferenceService(engine: engine, deviceName: deviceName) { [weak self] in
            await self?.modelForPeers()
        }
    }

    /// A user-facing sentence for any error this module throws.
    public nonisolated static func describe(_ error: Error) -> String {
        if let error = error as? LocalModelError { return error.description }
        if let error = error as? LocalEngineError { return error.description }
        return error.localizedDescription
    }
}
