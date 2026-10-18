import FMAgents
import FMApi
import FMAssistant
import FMBase
import FMControl
import FMLocalAI
import FMNet
import Foundation

@MainActor
final class GenieShellBridge: GenieActions {
    let context: @MainActor () -> PaletteContext
    let onCommand: @MainActor (PaletteCommand) -> Void
    let onStart: @MainActor (String, String?) async -> String

    init(
        context: @escaping @MainActor () -> PaletteContext,
        onCommand: @escaping @MainActor (PaletteCommand) -> Void,
        onStart: @escaping @MainActor (String, String?) async -> String
    ) {
        self.context = context
        self.onCommand = onCommand
        self.onStart = onStart
    }

    var me: String { AppNavigator.shared?.me ?? "" }

    var paletteContext: PaletteContext { context() }

    func run(_ command: PaletteCommand) {
        onCommand(command)
    }

    func startAgent(prompt: String, repository: String?) async -> String {
        await onStart(prompt, repository)
    }

    var canTurnPages: Bool { GeniePageTurner.shared.active }

    func turnPage(_ turn: GeniePageTurn) -> String? {
        GeniePageTurner.shared.turn(turn)
    }
}

@MainActor
enum GenieFeature {
    static func model() -> GenieModel? {
        let manager: LocalModelManager = LocalAIFeature.manager()
        let gemma: String = manager.selectedModel.displayName
        #if canImport(FoundationModels)
        if #available(iOS 26.0, *) {
            let apple: AppleGenieEngine = AppleGenieEngine()
            if apple.availability.isAvailable {
                guard manager.isUsable else {
                    return GenieModel(name: "Apple Intelligence, on this iPhone", engine: apple)
                }
                let duo: GenieDuoEngine = GenieDuoEngine(quick: apple, deep: manager.engine) {
                    _ = try await manager.ensureReady()
                }
                return GenieModel(name: "Apple Intelligence + \(gemma), on this iPhone", engine: duo)
            }
        }
        #endif
        guard manager.isUsable else { return nil }
        return GenieModel(name: "\(gemma), on this iPhone", engine: manager.engine) {
            _ = try await manager.ensureReady()
        }
    }

    static func outreachWriters() -> [GenieOutreachWriter] {
        var writers: [GenieOutreachWriter] = []
        if let cloud: GenieOutreachWriter = outreachCloudWriter() { writers.append(cloud) }
        if let local: GenieOutreachWriter = GenieOutreachWriter.local(model()) { writers.append(local) }
        return writers
    }

    static func outreachCloudWriter() -> GenieOutreachWriter? {
        guard let saved: AgentCredentials = Services.get(AgentCredentialStore.self)?.read(), saved.kind != .onDevice else { return nil }
        let best: String = saved.kind == .anthropic ? AnthropicProvider.defaultModel : "gpt-5.3"
        let chosen: AgentCredentials = AgentCredentials(kind: saved.kind, apiKey: saved.apiKey, model: best)
        return GenieOutreachWriter(name: chosen.modelLabel, timeout: GenieOutreach.cloudTimeout) {
            (messages: [LocalChatMessage]) async throws -> String in
            let provider: AgentProvider = chosen.build()
            let system: String = messages.filter { $0.role == .system }.map(\.content).joined(separator: "\n\n")
            var history: [JSONValue] = []
            for message in messages where message.role == .user { provider.appendUser(&history, message.content) }
            let client: HTTPClient = Services.get(HTTPClient.self) ?? URLSessionHTTPClient()
            return try await provider.send(systemPrompt: system, history: history, tools: [], client: client).text
        }
    }

    static func makeSession(
        bridge: GenieShellBridge,
        settings: @escaping @MainActor () -> String,
        launch: @escaping @MainActor (GenieHandoff) async -> String
    ) -> GenieSession {
        let toolbox: GenieToolbox = GenieToolbox(data: LocalAIFeature.toolbox(), actions: bridge)
        toolbox.vitals = { await GenieHub.shared.freshVitals() }
        toolbox.tricks = { (wanted: GenieTrick?) in await GenieHandCoach.shared.playTrick(wanted) }
        let memory: GenieMemory = GenieMemory(store: Services.get(KeyValueStore.self))
        let brain: GenieBrain = GenieBrain(toolbox: toolbox, memory: memory, model: model())
        return GenieSession(brain: brain, voice: GenieSpeechVoice(), settings: settings, launch: launch)
    }

    static func settingsLine(_ launcher: QuickAgentLauncher) -> String {
        var parts: [String] = [
            "model \(launcher.model.label) (\(agentProviderLabel(launcher.model.provider)))",
            "mode \(launcher.mode.label)",
            launcher.contextLine,
        ]
        if launcher.sendsYolo { parts.append("YOLO on") }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    static func outcomeLine(_ outcome: QuickLaunchOutcome) -> String {
        switch outcome {
        case .ignored:
            return "There was nothing to send to an agent."
        case .queued(let repo, _, _):
            return "An agent is working on it in \(repo.owner)/\(repo.name). You'll find it under Agents."
        case .queuedWithoutSession(let message):
            return "An agent is on it. \(message)"
        case .device:
            return "The agent runs on this iPhone."
        case .desktop(let name, _, _):
            return "An agent is working on it on \(name.isEmpty ? "your desktop" : name)."
        case .needsFix(_, let message):
            return "The agent did not start. \(message)"
        case .failed(let message):
            return "The agent did not start. \(message)"
        case .done(let message):
            return message
        }
    }
}
