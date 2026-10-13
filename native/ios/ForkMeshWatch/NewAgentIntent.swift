import AppIntents
import FMWatchWire
import Foundation

struct WatchIntentError: Error, CustomLocalizedStringResourceConvertible {
    let message: String

    init(_ message: String) {
        self.message = message
    }

    var localizedStringResource: LocalizedStringResource { LocalizedStringResource(stringLiteral: message) }
}

struct NewAgentIntent: AppIntent {
    static let title: LocalizedStringResource = "Start an agent"
    static let description = IntentDescription("Starts a ForkMesh agent on what you say, through your phone.")
    static let openAppWhenRun: Bool = true

    static let reachWaitSeconds: Double = 6

    @Parameter(title: "Prompt", requestValueDialog: "What should the agent do?")
    var prompt: String

    @Parameter(title: "Repository")
    var repository: String?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let text = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { throw $prompt.needsValueError("What should the agent do?") }
        if let problem = WatchCommand.promptProblem(text) { throw WatchIntentError(problem) }
        let repo = repository?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let place = repo.isEmpty ? "" : " on \(repo)"
        try await requestConfirmation(
            result: .result(dialog: IntentDialog(stringLiteral: "Start an agent\(place) to: \(text)?")))
        let store = WatchStore.shared
        guard await store.connected(within: NewAgentIntent.reachWaitSeconds) else {
            throw WatchIntentError("Your iPhone is out of reach. Bring it closer and try again.")
        }
        let reply = await store.send(.start(text: text, repo: repo.isEmpty ? nil : repo))
        store.feedback.acknowledge(reply)
        guard reply.ok else {
            throw WatchIntentError(reply.message.isEmpty ? "The agent did not start." : reply.message)
        }
        return .result(dialog: IntentDialog(stringLiteral: reply.message.isEmpty ? "The agent started." : reply.message))
    }
}

struct ForkMeshWatchShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: NewAgentIntent(),
            phrases: [
                "Start a new agent in \(.applicationName)",
                "Start a new agent with \(.applicationName)",
                "Start a new agent on \(.applicationName)",
                "Start a new \(.applicationName) agent",
                "Start an agent in \(.applicationName)",
                "Start a \(.applicationName) agent",
                "New agent in \(.applicationName)",
                "New \(.applicationName) agent",
                "Create a new agent in \(.applicationName)",
                "Create an agent in \(.applicationName)",
                "\(.applicationName) new agent",
                "\(.applicationName) start a new agent",
            ],
            shortTitle: "New agent",
            systemImageName: "sparkles")
    }
}
