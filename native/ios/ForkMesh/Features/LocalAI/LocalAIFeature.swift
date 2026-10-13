// Wiring for the Local AI feature: the MLX engine, the model manager and the
// mesh-node request handler, created once and registered in `Services`. New
// for the native app; see docs/FMLocalAI.md.

import FMApi
import FMAssistant
import FMBase
import FMLocalAI
import FMNet
import Foundation
import SwiftUI
import UIKit
#if canImport(os)
import os
#endif

/// What the feature creates at startup.
struct LocalAIServices {
    let engine: LocalChatEngine
    let manager: LocalModelManager
    /// The phone as a mesh agent node: hand it to a transport (the local
    /// control channel) to answer `ai.info` / `ai.chat` from paired peers.
    let inference: LocalInferenceService
}

enum LocalAIFeature {
    /// Builds the engine, the store under Application Support/LocalModels and
    /// the manager over `store` (`localai/model`, `localai/enabled`).
    @MainActor
    static func makeServices(store: KeyValueStore, http: HTTPClient? = nil) -> LocalAIServices {
        let engine = MLXGemmaEngine()
        let fallbackRoot = FileManager.default.temporaryDirectory.appendingPathComponent("LocalModels", isDirectory: true)
        let root: URL = (try? LocalModelStore.defaultRoot()) ?? fallbackRoot
        let modelStore = LocalModelStore(root: root)
        let client: HTTPClient = http ?? URLSessionHTTPClient()
        let downloader = HuggingFaceSnapshotDownloader(http: client, store: modelStore)
        let manager = LocalModelManager(
            engine: engine,
            store: modelStore,
            downloader: downloader,
            prefs: store,
            device: currentDevice()
        )
        let inference = manager.makeInferenceService(deviceName: UIDevice.current.name)
        return LocalAIServices(engine: engine, manager: manager, inference: inference)
    }

    /// `makeServices`, registered: `LocalModelManager`, `LocalChatEngine` (as
    /// the protocol) and `LocalInferenceService`. AppServices calls this after
    /// Code and before Agents.
    @MainActor
    @discardableResult
    static func register(store: KeyValueStore, http: HTTPClient? = nil) -> LocalAIServices {
        let services = makeServices(store: store, http: http)
        Services.put(services.manager)
        Services.put(services.engine, as: LocalChatEngine.self)
        Services.put(services.inference)
        return services
    }

    /// The registered manager, registering one over UserDefaults if the shell
    /// has not (previews, a partial build).
    @MainActor
    static func manager() -> LocalModelManager {
        if let existing = Services.get(LocalModelManager.self) {
            return existing
        }
        let prefs: KeyValueStore = Services.get(KeyValueStore.self) ?? UserDefaultsStore()
        return register(store: prefs).manager
    }

    @MainActor
    static func toolbox() -> ForkMeshToolbox? {
        if let existing = Services.get(ForkMeshToolbox.self) {
            return existing
        }
        guard let api = Services.get(ApiService.self) else { return nil }
        let toolbox = ForkMeshToolbox(sources: .live(api: api))
        Services.put(toolbox)
        return toolbox
    }

    /// Installed memory, what iOS still lets this process allocate, and
    /// whether this is the Simulator.
    static func currentDevice() -> DeviceProfile {
        var available: UInt64?
        #if os(iOS)
        let free: Int = os_proc_available_memory()
        if free > 0 {
            available = UInt64(free)
        }
        #endif
        return DeviceProfile.current(availableMemory: available)
    }

    /// Settings → On-device AI.
    @MainActor
    static func settingsRoot() -> LocalAISettingsView {
        LocalAISettingsView()
    }

    /// Agents → On-device chat.
    @MainActor
    static func chatRoot() -> OnDeviceChatView {
        OnDeviceChatView()
    }
}
