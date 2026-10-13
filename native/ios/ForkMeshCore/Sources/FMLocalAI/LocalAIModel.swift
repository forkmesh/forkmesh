// The on-device model catalog and device fit.

import Foundation

/// One model the phone can download and run.
///
/// Sizes were read from the Hugging Face API and the safetensors headers at
/// the pinned revision. The repository ships the multimodal checkpoint (vision
/// and audio towers included), so the download is larger than what stays in
/// memory: MLXLLM's Gemma 4 loader drops the towers and keeps the language
/// model, whose per-layer embeddings are most of it.
public struct LocalAIModel: Sendable, Equatable, Hashable, Identifiable {
    /// Stable key, stored under `localai/model`.
    public let id: String
    /// Hugging Face repository, "org/name".
    public let repo: String
    /// Commit the files are downloaded at. A commit rather than "main", so what
    /// runs is what was measured, and a partial download never mixes revisions.
    public let revision: String
    public let displayName: String
    public let summary: String
    /// Bytes the snapshot patterns select (weights, JSON, chat template).
    public let approxDownloadBytes: Int64
    /// Resident size of the loaded weights, before the KV cache.
    public let approxMemoryBytes: Int64
    /// `ProcessInfo.physicalMemory` below this: the model is not offered.
    public let minimumPhysicalMemory: UInt64
    /// At or above this the model is recommended; between the two it runs but
    /// iOS may end the app under memory pressure.
    public let recommendedPhysicalMemory: UInt64
    /// The model's context window (`max_position_embeddings`).
    public let contextLength: Int
    /// Which repository files the download takes: what MLXLMCommon's own
    /// downloader asks for (`modelDownloadPatterns`).
    public let filePatterns: [String]

    public init(
        id: String,
        repo: String,
        revision: String,
        displayName: String,
        summary: String,
        approxDownloadBytes: Int64,
        approxMemoryBytes: Int64,
        minimumPhysicalMemory: UInt64,
        recommendedPhysicalMemory: UInt64,
        contextLength: Int,
        filePatterns: [String] = LocalAIModel.defaultFilePatterns
    ) {
        self.id = id
        self.repo = repo
        self.revision = revision
        self.displayName = displayName
        self.summary = summary
        self.approxDownloadBytes = approxDownloadBytes
        self.approxMemoryBytes = approxMemoryBytes
        self.minimumPhysicalMemory = minimumPhysicalMemory
        self.recommendedPhysicalMemory = recommendedPhysicalMemory
        self.contextLength = contextLength
        self.filePatterns = filePatterns
    }

    public static let defaultFilePatterns = ["*.safetensors", "*.json", "*.jinja"]
}

public enum LocalModelCatalog {
    /// Gemma 4 E2B, instruction tuned, 4-bit. The default: the smallest Gemma 4
    /// and the one MLXLLM registers as `LLMRegistry.gemma4_e2b_it_4bit`.
    public static let gemma4E2B = LocalAIModel(
        id: "gemma-4-e2b-it-4bit",
        repo: "mlx-community/gemma-4-e2b-it-4bit",
        revision: "238767527555cb75a05732a84dff5d6ba0dd6809",
        displayName: "Gemma 4 E2B",
        summary: "Google Gemma 4, 2B effective parameters, instruction tuned, 4-bit.",
        approxDownloadBytes: 3_583_086_498,
        approxMemoryBytes: 2_610_000_000,
        minimumPhysicalMemory: 5_500_000_000,
        recommendedPhysicalMemory: 7_500_000_000,
        contextLength: 131_072
    )

    /// Gemma 4 E4B, instruction tuned, 4-bit: better answers, twice the memory.
    public static let gemma4E4B = LocalAIModel(
        id: "gemma-4-e4b-it-4bit",
        repo: "mlx-community/gemma-4-e4b-it-4bit",
        revision: "475b9088d29754a3379866cf5aeb6b41acd313c2",
        displayName: "Gemma 4 E4B",
        summary: "Google Gemma 4, 4B effective parameters, instruction tuned, 4-bit.",
        approxDownloadBytes: 5_179_239_349,
        approxMemoryBytes: 4_200_000_000,
        minimumPhysicalMemory: 7_500_000_000,
        recommendedPhysicalMemory: 11_000_000_000,
        contextLength: 131_072
    )

    public static let all: [LocalAIModel] = [gemma4E2B, gemma4E4B]
    public static let defaultModel: LocalAIModel = gemma4E2B

    public static func model(id: String) -> LocalAIModel? {
        all.first { $0.id == id }
    }

    /// The largest model this device runs comfortably, else the largest it
    /// runs at all, else nil.
    public static func recommendedModel(for device: DeviceProfile, in models: [LocalAIModel] = all) -> LocalAIModel? {
        let bySize = models.sorted { $0.approxMemoryBytes > $1.approxMemoryBytes }
        if let best = bySize.first(where: { device.fit(for: $0) == .recommended }) { return best }
        return bySize.first { device.fit(for: $0).canRun }
    }
}

/// What the app knows about the hardware, for device fit.
public struct DeviceProfile: Sendable, Equatable {
    /// `ProcessInfo.processInfo.physicalMemory`.
    public var physicalMemory: UInt64
    /// `os_proc_available_memory()` on iOS: what this process may still
    /// allocate before iOS ends it. nil where the OS does not say.
    public var availableMemory: UInt64?
    /// MLX needs Metal on a real GPU; the Simulator has none it can use.
    public var isSimulator: Bool

    public init(physicalMemory: UInt64, availableMemory: UInt64? = nil, isSimulator: Bool = false) {
        self.physicalMemory = physicalMemory
        self.availableMemory = availableMemory
        self.isSimulator = isSimulator
    }

    /// This process. The app target passes `os_proc_available_memory()`,
    /// which the core cannot call on Linux.
    public static func current(availableMemory: UInt64? = nil) -> DeviceProfile {
        #if targetEnvironment(simulator)
        let simulator = true
        #else
        let simulator = false
        #endif
        return DeviceProfile(
            physicalMemory: ProcessInfo.processInfo.physicalMemory,
            availableMemory: availableMemory,
            isSimulator: simulator
        )
    }

    /// Headroom kept free beyond the weights: the KV cache, activations, MLX's
    /// buffer cache, and the rest of the app.
    public static let workingHeadroomBytes: Int64 = 700_000_000

    public func fit(for model: LocalAIModel) -> LocalModelFit {
        if isSimulator {
            return .unsupported(reason: "On-device models need a real iPhone or iPad: MLX runs on the GPU, which the Simulator does not provide.")
        }
        if physicalMemory < model.minimumPhysicalMemory {
            return .tooLarge(
                reason: "\(model.displayName) needs a device with at least \(LocalAIFormat.memory(model.minimumPhysicalMemory)) of memory; this one has \(LocalAIFormat.memory(physicalMemory))."
            )
        }
        if let available = availableMemory,
           Int64(clamping: available) < model.approxMemoryBytes + DeviceProfile.workingHeadroomBytes {
            return .tight(
                note: "Only \(LocalAIFormat.bytes(Int64(clamping: available))) is free for the app right now; \(model.displayName) needs about \(LocalAIFormat.bytes(model.approxMemoryBytes + DeviceProfile.workingHeadroomBytes)). Close other apps first."
            )
        }
        if physicalMemory < model.recommendedPhysicalMemory {
            return .tight(note: "Runs on this device, but iOS may close the app when memory is short.")
        }
        return .recommended
    }
}

public enum LocalModelFit: Sendable, Equatable {
    case recommended
    /// Runs, with a caveat worth showing.
    case tight(note: String)
    /// Not enough memory to load it at all.
    case tooLarge(reason: String)
    /// The runtime cannot run here (Simulator, no MLX in the build).
    case unsupported(reason: String)

    public var canRun: Bool {
        switch self {
        case .recommended, .tight: return true
        case .tooLarge, .unsupported: return false
        }
    }

    /// The caveat or the reason, nil when recommended.
    public var message: String? {
        switch self {
        case .recommended: return nil
        case .tight(let note): return note
        case .tooLarge(let reason), .unsupported(let reason): return reason
        }
    }
}

/// Byte and rate formatting for the Local AI screens. Decimal units, as the
/// iOS storage screen uses.
public enum LocalAIFormat {
    public static func bytes(_ count: Int64) -> String {
        let value = Double(max(count, 0))
        if value >= 1_000_000_000 { return String(format: "%.1f GB", value / 1_000_000_000) }
        if value >= 1_000_000 { return String(format: "%.0f MB", value / 1_000_000) }
        if value >= 1_000 { return String(format: "%.0f KB", value / 1_000) }
        return "\(Int64(value)) B"
    }

    /// Installed RAM, rounded to the whole gigabyte the device is sold with
    /// (a 6 GB iPhone reports about 6.0e9 bytes, an 8 GB one about 8.0e9).
    public static func memory(_ count: UInt64) -> String {
        let gigabytes = (Double(count) / 1_000_000_000).rounded()
        return "\(Int(max(gigabytes, 1))) GB"
    }

    public static func tokensPerSecond(_ rate: Double) -> String {
        guard rate.isFinite, rate > 0 else { return "– tok/s" }
        return String(format: "%.1f tok/s", rate)
    }

    public static func percent(_ fraction: Double) -> String {
        guard fraction.isFinite else { return "0%" }
        return "\(Int((min(max(fraction, 0), 1) * 100).rounded(.down)))%"
    }
}
