// Gemma 4 on this iPhone as an agent provider. See docs/FMLocalAI.md, "Writing GemmaAgentProvider".
//
// History stays OpenAI Chat Completions shaped, exactly like OpenAiProvider,
// so the loop and the transcript do not change; the model itself is reached
// through FMLocalAI's `LocalModelManager` (switch, download, load) and its
// `LocalChatEngine` (MLX in the app target, a fake in tests).

import FMBase
import FMLocalAI
import FMNet
import Foundation

@MainActor
public final class GemmaAgentProvider: AgentProvider {
    /// A phone-sized model has a small effective context: keep less history
    /// than the cloud providers (60) and cut tool output shorter (24 000).
    nonisolated public static let maxHistoryMessages = 20
    nonisolated public static let maxToolOutputChars = 8000

    private let explicitManager: LocalModelManager?
    public let parameters: LocalGenerationParameters

    /// `manager` nil resolves the app's through `Services` at each turn, so a
    /// provider built before the Local AI feature registered still works.
    public init(manager: LocalModelManager? = nil, parameters: LocalGenerationParameters = .agent) {
        self.explicitManager = manager
        self.parameters = parameters
    }

    /// The manager this provider runs on, or nil when the build has none.
    public var manager: LocalModelManager? {
        explicitManager ?? Services.get(LocalModelManager.self)
    }

    public var id: String { "on-device" }

    public var model: String { manager?.selectedModel.id ?? LocalModelCatalog.defaultModel.id }

    /// Nothing goes over the network: `client` is ignored. Cancel the task to
    /// stop generation.
    public func send(systemPrompt: String, history: [JSONValue], tools: [AgentTool], client: HTTPClient) async throws -> AgentTurn {
        guard let manager else {
            throw AgentProviderException("On-device AI is not available in this build.", retryable: false)
        }
        do {
            // Loads the selected model if it is not in memory yet. Throws when
            // the switch is off or the model is not downloaded; a retry would
            // not change either, so none of these are retryable.
            try await manager.ensureReady()
            var messages: [LocalChatMessage] = [.system(systemPrompt)]
            for item in history {
                messages.append(try LocalChatMessage(openAI: item.objectOrEmpty))
            }
            let specs = tools.map { LocalToolSpec(name: $0.name, description: $0.description, parameters: $0.schema) }
            let turn = try await manager.engine.complete(messages: messages, tools: specs, parameters: parameters)
            return AgentTurn(
                text: turn.text,
                toolCalls: turn.toolCalls.map { AgentToolCall(id: $0.id, name: $0.name, arguments: $0.arguments) },
                raw: .object(turn.message.openAIJSON),
                inputTokens: turn.inputTokens,
                outputTokens: turn.outputTokens,
                refusal: nil
            )
        } catch let error as AgentProviderException {
            throw error
        } catch let error as LocalChatFormatError {
            throw AgentProviderException("The conversation could not be handed to the model: \(error.message)", retryable: false)
        } catch {
            throw AgentProviderException(LocalModelManager.describe(error), retryable: false)
        }
    }

    public func appendAssistant(_ history: inout [JSONValue], _ turn: AgentTurn) {
        history.append(turn.raw)
    }

    public func appendToolResults(_ history: inout [JSONValue], _ results: [AgentToolResult]) {
        for result in results {
            history.append(.object(LocalChatMessage.tool(result.output, callId: result.callId, name: result.name).openAIJSON))
        }
    }

    public func appendUser(_ history: inout [JSONValue], _ text: String) {
        history.append(.object(LocalChatMessage.user(text).openAIJSON))
    }
}

/// Where the on-device model stands, as the agent screens show it: whether a
/// session can run on it now, and if not, the one sentence that says why.
public enum OnDeviceAgentReadiness: Equatable, Sendable {
    case ready(modelName: String)
    case notReady(reason: String)
    case unavailable(reason: String)

    public var isReady: Bool {
        if case .ready = self { return true }
        return false
    }

    public var message: String {
        switch self {
        case .ready(let name): return "\(name) is downloaded and runs on this iPhone."
        case .notReady(let reason), .unavailable(let reason): return reason
        }
    }

    /// Nothing the person can change here (no manager, or MLX cannot run on
    /// this device): settings would not help.
    public var canBeFixedInSettings: Bool {
        if case .notReady = self { return true }
        return false
    }

    @MainActor
    public static func of(_ manager: LocalModelManager?) -> OnDeviceAgentReadiness {
        guard let manager else { return .unavailable(reason: "On-device AI is not available in this build.") }
        if let reason = manager.engineAvailability.reason { return .unavailable(reason: reason) }
        let model = manager.selectedModel
        if !manager.isEnabled {
            return .notReady(reason: "On-device AI is off. Turn it on and download \(model.displayName) to run agents on this iPhone.")
        }
        if !manager.isDownloaded(model) {
            if case .downloading = manager.state(for: model) {
                return .notReady(reason: "\(model.displayName) is still downloading.")
            }
            return .notReady(reason: "Download \(model.displayName) to run agents on this iPhone.")
        }
        return .ready(modelName: model.displayName)
    }
}
