// `.onDevice` is Gemma 4 on this iPhone (GemmaAgentProvider). With MLX a
// phone can host a local model usefully, and it needs no key: what it needs is
// the model downloaded, which FMLocalAI's manager tracks.

import FMBase
import FMLocalAI
import FMNet
import Foundation

/// Which provider a session talks to. `rawValue` is the name stored in the
/// keychain entry.
public enum AgentProviderKind: String, CaseIterable, Sendable {
    case anthropic
    case openai
    case onDevice

    public var id: String {
        switch self {
        case .anthropic: return "claude-api"
        case .openai: return "openai"
        case .onDevice: return "on-device"
        }
    }

    public var label: String {
        switch self {
        case .anthropic: return "Claude"
        case .openai: return "OpenAI"
        case .onDevice: return "Gemma 4"
        }
    }

    /// The longer name the provider picker shows.
    public var pickerLabel: String {
        switch self {
        case .onDevice: return "Gemma 4 (on this iPhone)"
        default: return label
        }
    }

    /// Whether the provider needs an API key at all.
    public var needsKey: Bool { self != .onDevice }

    /// Where a user gets a key, shown next to the key field.
    public var consoleUrl: String {
        switch self {
        case .anthropic: return "https://console.anthropic.com/settings/keys"
        case .openai: return "https://platform.openai.com/api-keys"
        case .onDevice: return ""
        }
    }

    /// What a valid key looks like, so an obvious paste error is caught before
    /// a request is spent on it. The on-device model takes none.
    public func looksLikeKey(_ value: String) -> Bool {
        let trimmed = value.dartTrimmed
        switch self {
        case .anthropic: return trimmed.hasPrefix("sk-ant-") && trimmed.utf16.count > 20
        case .openai: return trimmed.hasPrefix("sk-") && trimmed.utf16.count > 20
        case .onDevice: return true
        }
    }

    /// The prefix a key starts with, for the paste-error message.
    public var keyPrefix: String {
        switch self {
        case .anthropic: return "\"sk-ant-\""
        case .openai, .onDevice: return "\"sk-\""
        }
    }

    /// Models offered for this provider. The first entry is the default.
    public var models: [AgentModelOption] {
        switch self {
        case .anthropic:
            return [
                AgentModelOption("claude-opus-5-5", "Claude Opus 5.5", "Most capable"),
                AgentModelOption("claude-opus-5", "Claude Opus 5", "Previous Opus"),
                AgentModelOption("claude-sonnet-5", "Claude Sonnet 5", "Balanced"),
                AgentModelOption("claude-haiku-4-5", "Claude Haiku 4.5", "Fastest and cheapest"),
            ]
        case .openai:
            return [
                AgentModelOption("gpt-5.3-codex", "GPT-5.3 Codex", "Coding"),
                AgentModelOption("gpt-5.3", "GPT-5.3", "General"),
            ]
        case .onDevice:
            // The Local AI catalog, default first: which one runs is the
            // manager's selection (`localai/model`), shared with on-device chat.
            let catalog = LocalModelCatalog.all
            let ordered = catalog.filter { $0.id == LocalModelCatalog.defaultModel.id }
                + catalog.filter { $0.id != LocalModelCatalog.defaultModel.id }
            return ordered.map { AgentModelOption($0.id, $0.displayName, "On this iPhone · \(LocalAIFormat.bytes($0.approxDownloadBytes))") }
        }
    }
}

public struct AgentModelOption: Equatable, Sendable {
    public let id: String
    public let label: String
    public let note: String

    public init(_ id: String, _ label: String, _ note: String) {
        self.id = id
        self.label = label
        self.note = note
    }
}

/// The provider, key, and model a session runs with.
public struct AgentCredentials: Equatable, Sendable {
    public let kind: AgentProviderKind
    public let apiKey: String
    public let model: String

    public init(kind: AgentProviderKind, apiKey: String, model: String) {
        self.kind = kind
        self.apiKey = apiKey
        self.model = model
    }

    /// The on-device model: no key, and the model id is the Local AI one.
    public static func onDevice(model: String = LocalModelCatalog.defaultModel.id) -> AgentCredentials {
        AgentCredentials(kind: .onDevice, apiKey: "", model: model)
    }

    public var isUsable: Bool {
        (!kind.needsKey || !apiKey.dartTrimmed.isEmpty) && !model.dartTrimmed.isEmpty
    }

    /// Never put the key itself in a log line or a transcript.
    public var maskedKey: String {
        let trimmed = apiKey.dartTrimmed
        let units = Array(trimmed.utf16)
        if units.count <= 10 { return "••••" }
        let head = String(decoding: units[0..<6], as: UTF16.self)
        let tail = String(decoding: units[(units.count - 4)...], as: UTF16.self)
        return "\(head)…\(tail)"
    }

    /// The label of the chosen model ("Claude Opus 5", "Gemma 4 E2B"), or the
    /// raw id for one the picker does not list.
    public var modelLabel: String {
        kind.models.first { $0.id == model }?.label ?? model
    }

    /// The provider. For `.onDevice`, `localModels` (default: the app's, from
    /// `Services`) supplies the model; its selection follows `model` when that
    /// names a catalog model.
    @MainActor
    public func build(localModels: LocalModelManager? = nil) -> AgentProvider {
        switch kind {
        case .anthropic:
            return AnthropicProvider(apiKey: apiKey, model: model)
        case .openai:
            return OpenAiProvider(apiKey: apiKey, model: model)
        case .onDevice:
            let manager = localModels ?? Services.get(LocalModelManager.self)
            if let manager, manager.selectedModelId != model,
               let chosen = manager.catalog.first(where: { $0.id == model }) {
                manager.select(chosen)
            }
            return GemmaAgentProvider(manager: manager)
        }
    }

    /// The loop for this provider: the on-device model gets the shorter
    /// history and tool-output bounds its context can hold.
    @MainActor
    public func makeAgent(toolset: AgentToolset, localModels: LocalModelManager? = nil, client: HTTPClient? = nil) -> CodingAgent {
        let provider = build(localModels: localModels)
        if kind == .onDevice {
            toolset.outputLimit = GemmaAgentProvider.maxToolOutputChars
            return CodingAgent(
                provider: provider,
                toolset: toolset,
                maxHistoryMessages: GemmaAgentProvider.maxHistoryMessages,
                client: client
            )
        }
        return CodingAgent(provider: provider, toolset: toolset, client: client)
    }
}

/// Stores the agent's API credentials in the platform keychain.
///
/// The key is a bearer credential for the user's own paid account, so it lives
/// beside the node identity in `SecureStore` rather than in preferences, and
/// it is never sent anywhere except the provider it belongs to.
@MainActor
public final class AgentCredentialStore {
    public static let storageKey = "agent/credentials-v1"

    private let store: SecureStore
    private let http: HTTPClient
    private let localModels: LocalModelManager?

    public init(secureStore: SecureStore, http: HTTPClient? = nil, localModels: LocalModelManager? = nil) {
        self.store = secureStore
        self.http = http ?? URLSessionHTTPClient()
        self.localModels = localModels
    }

    /// The stored credentials, or nil when the user has not configured a
    /// provider yet, which is what puts the app into its "set up agent mode"
    /// state rather than failing at the first prompt.
    public func read() -> AgentCredentials? {
        guard let raw = store.read(Self.storageKey), !raw.isEmpty else { return nil }
        guard let data = (try? JSON.parse(raw))?.objectValue else { return nil }
        let kind = AgentProviderKind(rawValue: data["kind"]?.stringValue ?? "") ?? .anthropic
        let storedModel = data["model"].map(lenientText) ?? ""
        let credentials = AgentCredentials(
            kind: kind,
            apiKey: data["apiKey"].map(lenientText) ?? "",
            model: storedModel.isEmpty ? (kind.models.first?.id ?? "") : storedModel
        )
        return credentials.isUsable ? credentials : nil
    }

    public func write(_ credentials: AgentCredentials) {
        let payload: JSONObject = [
            "kind": .string(credentials.kind.rawValue),
            "apiKey": .string(credentials.apiKey.dartTrimmed),
            "model": .string(credentials.model),
        ]
        store.write(Self.storageKey, JSON.encode(.object(payload)))
    }

    public func clear() {
        store.delete(Self.storageKey)
    }

    /// Spend one cheap request to prove the key works, so a typo surfaces
    /// during setup rather than halfway through a coding task. For the
    /// on-device model it checks the model is ready instead (switch on,
    /// downloaded) without loading it.
    ///
    /// Returns nil when the key is good, or a message to show the user.
    public func verify(_ credentials: AgentCredentials, client: HTTPClient? = nil) async -> String? {
        if credentials.kind == .onDevice {
            let readiness = OnDeviceAgentReadiness.of(localModels ?? Services.get(LocalModelManager.self))
            return readiness.isReady ? nil : readiness.message
        }
        let transport = client ?? http
        do {
            _ = try await credentials.build(localModels: localModels).send(
                systemPrompt: "Reply with the single word: ok",
                history: [.object(["role": "user", "content": "ok"])],
                tools: [],
                client: transport
            )
            return nil
        } catch let error as AgentProviderException {
            if error.statusCode == 401 || error.statusCode == 403 {
                return "\(credentials.kind.label) rejected this key. Check that you copied all of it and that it is still active."
            }
            if error.statusCode == 429 {
                return "The key is valid but the account is rate limited right now."
            }
            return error.message
        } catch {
            return "Could not reach \(credentials.kind.label): \(error)"
        }
    }
}
