// The models a queued (node or mirror) agent run can ask for, the composer chip's short
// names, and the remembered choice.

import FMBase
import Foundation

/// A model a queued agent run can ask for.
///
/// The relay takes the website's short aliases ("opus"), not model ids, and
/// resolves them per runtime; `id` is what the alias means today, kept so a
/// session that reports the resolved id can be labelled the same way.
public struct AgentModelChoice: Equatable, Hashable, Sendable, Identifiable {
    /// `claude-code`, `codex` or `local-llama` (Gemma on the node's Ollama).
    public let provider: String
    public let alias: String
    public let modelId: String
    /// The short name shown on the composer's model chip.
    public let label: String
    public let note: String

    public var id: String { "\(provider)/\(alias)" }

    public init(provider: String, alias: String, id: String, label: String, note: String = "") {
        self.provider = provider
        self.alias = alias
        self.modelId = id
        self.label = label
        self.note = note
    }

    /// The launch request's `provider`: a Claude model keeps the automatic
    /// runtime choice (Claude Code first, falling back to whatever the node
    /// has); any other model names its own runtime.
    public var launchProvider: String { provider == "claude-code" ? "auto" : provider }

    public var runsOnDevice: Bool { provider == AgentProviderKind.onDevice.id }
}

public enum AgentModelCatalog {
    /// The default queued-run model: Opus 5.5 through Claude Code.
    public static let defaultNodeModel = AgentModelChoice(
        provider: "claude-code", alias: "opus", id: "claude-opus-5-5", label: "Opus 5.5",
        note: "Default · most capable Opus")

    /// Models offered for queued runs. The first per provider is the default.
    public static let nodeModels: [AgentModelChoice] = [
        defaultNodeModel,
        AgentModelChoice(provider: "claude-code", alias: "opus-5", id: "claude-opus-5", label: "Opus 5", note: "Previous Opus"),
        AgentModelChoice(provider: "claude-code", alias: "sonnet", id: "claude-sonnet-5", label: "Sonnet 5", note: "Balanced"),
        AgentModelChoice(provider: "claude-code", alias: "haiku", id: "claude-haiku-4-5", label: "Haiku 4.5", note: "Fastest"),
        AgentModelChoice(provider: "claude-code", alias: "fable", id: "claude-fable-5", label: "Fable 5", note: "Long, hard tasks"),
        AgentModelChoice(provider: "codex", alias: "terra", id: "gpt-5.6-terra", label: "GPT-5.6 Terra"),
        AgentModelChoice(provider: "codex", alias: "sol", id: "gpt-5.6-sol", label: "GPT-5.6 Sol"),
        AgentModelChoice(provider: "codex", alias: "luna", id: "gpt-5.6-luna", label: "GPT-5.6 Luna"),
        AgentModelChoice(provider: "codex", alias: "spark", id: "gpt-5.3-codex-spark", label: "Codex Spark"),
        AgentModelChoice(provider: "local-llama", alias: "gemma4:e4b", id: "gemma4:e4b", label: "Gemma 4 E4B", note: "Gemma on the node's Ollama"),
        AgentModelChoice(provider: "local-llama", alias: "gemma4:e2b", id: "gemma4:e2b", label: "Gemma 4 E2B", note: "Gemma on the node's Ollama"),
        AgentModelChoice(provider: "local-llama", alias: "gemma4:12b", id: "gemma4:12b", label: "Gemma 4 12B", note: "Gemma on the node's Ollama"),
        AgentModelChoice(provider: "local-llama", alias: "gemma4:26b", id: "gemma4:26b", label: "Gemma 4 26B", note: "Gemma on the node's Ollama"),
        AgentModelChoice(provider: "local-llama", alias: "gemma4:31b", id: "gemma4:31b", label: "Gemma 4 31B", note: "Gemma on the node's Ollama"),
    ]

    public static let onDeviceGemma = AgentModelChoice(
        provider: AgentProviderKind.onDevice.id, alias: "on-device-gemma", id: "on-device-gemma",
        label: "Gemma 4 · iPhone", note: "On this iPhone · no network or key")

    public static var allModels: [AgentModelChoice] { nodeModels + [onDeviceGemma] }

    public static let handoffProviders = ["claude-code", "codex"]

    public static func followUpChoices(provider: String) -> [AgentModelChoice] {
        guard handoffProviders.contains(provider) else { return [] }
        let others = nodeModels.filter { $0.provider != provider && handoffProviders.contains($0.provider) }
        return nodeModels.filter { $0.provider == provider } + others
    }

    public static func runtimeLabel(_ provider: String) -> String {
        switch provider {
        case "claude-code": return "Claude Code"
        case "codex": return "Codex"
        default: return provider
        }
    }

    /// Short, human names for model ids and aliases the app may be shown,
    /// including the on-device API models.
    static let shortLabels: [String: String] = [
        "claude-opus-5-5": "Opus 5.5",
        "claude-opus-5": "Opus 5",
        "claude-sonnet-5": "Sonnet 5",
        "claude-haiku-4-5": "Haiku 4.5",
        "claude-fable-5": "Fable 5",
        "claude-fable-5-1": "Fable 5.1",
        "claude-opus-4-8": "Opus 4.8",
        "claude-sonnet-4-6": "Sonnet 4.6",
        "gpt-5.3-codex": "GPT-5.3 Codex",
        "gpt-5.3": "GPT-5.3",
    ]

    /// The chip label for a model id or alias; unknown names are shown as given.
    public static func shortLabel(_ idOrAlias: String) -> String {
        let key = idOrAlias.dartTrimmed
        if key.isEmpty { return "" }
        let lower = key.lowercased()
        if let known = shortLabels[lower] { return known }
        if let choice = nodeModels.first(where: { $0.alias == lower || $0.modelId == lower }) { return choice.label }
        return key
    }

    /// The choices to offer, narrowed to what the relay says it accepts when
    /// it said anything (`models: {provider: [alias...]}` in the launch
    /// options). An older relay that does not know an alias simply does not
    /// offer it.
    public static func choices(offered: [String: [String]]) -> [AgentModelChoice] {
        if offered.isEmpty { return nodeModels }
        let narrowed = nodeModels.filter { offered[$0.provider]?.contains($0.alias) ?? false }
        return narrowed.isEmpty ? nodeModels : narrowed
    }
}

public enum NodeModelPreference {
    public static let key = "agent/node-model-alias"

    public static func read(_ store: KeyValueStore?) -> AgentModelChoice {
        let alias = store?.string(forKey: key) ?? ""
        return AgentModelCatalog.allModels.first { $0.alias == alias } ?? AgentModelCatalog.defaultNodeModel
    }

    public static func write(_ choice: AgentModelChoice, to store: KeyValueStore?) {
        store?.set(choice.alias, forKey: key)
    }
}
