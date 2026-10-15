import AppIntents
import FMApi
import FMSurfaces
import Foundation
import SwiftUI

enum SurfaceAppIntentWait {
    static let open: Double = 6
    static let action: Double = 8
}

struct StartAgentIntent: AppIntent {
    static let title: LocalizedStringResource = "Start an agent"
    static let description = IntentDescription("Opens the ForkMesh composer, ready to start an agent.")
    static let openAppWhenRun: Bool = true

    @Parameter(title: "Repository")
    var repository: RepositoryEntity?

    @Parameter(title: "Prompt")
    var prompt: String?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        let text = prompt?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let target = ComposeTarget(repo: repository?.repo, text: text.isEmpty ? nil : String(text.prefix(4_000)))
        try await SurfaceCommandCenter.shared.perform(.open(.compose(target)), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct OpenInboxIntent: AppIntent {
    static let title: LocalizedStringResource = "Open inbox"
    static let description = IntentDescription("Opens your ForkMesh inbox.")
    static let openAppWhenRun: Bool = true

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        try await SurfaceCommandCenter.shared.perform(.open(.inbox(.all)), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct OpenApprovalsIntent: AppIntent {
    static let title: LocalizedStringResource = "Open agent approvals"
    static let description = IntentDescription("Opens the requests your agents are waiting on.")
    static let openAppWhenRun: Bool = true

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        try await SurfaceCommandCenter.shared.perform(.open(.inbox(.approvals)), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct OpenRepositoryIntent: AppIntent {
    static let title: LocalizedStringResource = "Open repository"
    static let openAppWhenRun: Bool = true

    @Parameter(title: "Repository")
    var repository: RepositoryEntity

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        guard let repo = repository.repo else { return .result() }
        try await SurfaceCommandCenter.shared.perform(.open(.repo(repo, section: nil)), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct OpenIssueIntent: AppIntent {
    static let title: LocalizedStringResource = "Open issue"
    static let openAppWhenRun: Bool = true

    @Parameter(title: "Issue")
    var issue: IssueEntity

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        guard let route = issue.route else { return .result() }
        try await SurfaceCommandCenter.shared.perform(.open(route), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct OpenNoteIntent: AppIntent {
    static let title: LocalizedStringResource = "Open note"
    static let openAppWhenRun: Bool = true

    @Parameter(title: "Note")
    var note: NoteEntity

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        try await SurfaceCommandCenter.shared.perform(.open(.note(id: note.id)), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct OpenAgentRunIntent: AppIntent {
    static let title: LocalizedStringResource = "Open agent run"
    static let openAppWhenRun: Bool = true

    @Parameter(title: "Agent run")
    var run: AgentRunEntity

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        try await SurfaceCommandCenter.shared.perform(.open(.agent(id: run.id)), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct StartTimerIntent: AppIntent {
    static let title: LocalizedStringResource = "Start a timer"
    static let description = IntentDescription("Starts a ForkMesh countdown.")

    @Parameter(title: "Minutes", default: 25)
    var minutes: Int

    @Parameter(title: "Label")
    var label: String?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let clamped = min(max(minutes, 1), 24 * 60)
        let name = label?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        try await SurfaceCommandCenter.shared.perform(
            .startTimer(seconds: clamped * 60, label: name), waitSeconds: SurfaceAppIntentWait.action)
        let unit = clamped == 1 ? "minute" : "minutes"
        return .result(dialog: IntentDialog(stringLiteral: "Timer set for \(clamped) \(unit)."))
    }
}

struct AgentStatusIntent: AppIntent {
    static let title: LocalizedStringResource = "Agent status"
    static let description = IntentDescription("Says what your ForkMesh agents, or one agent, are doing.")

    @Parameter(title: "Agent")
    var run: AgentRunEntity?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog & ShowsSnippetView {
        if let run {
            let row = try await AgentVoiceSource.row(run)
            let candidate = AgentVoiceSource.candidate(row)
            var changes = ""
            if candidate.number > 0, !row.isActive, let client = AgentOpsFeature.client,
               let diff = await AgentVoiceSource.changes(client, number: candidate.number) {
                changes = AgentVoice.changes(
                    files: diff.files.count, added: diff.files.reduce(0) { $0 + $1.added },
                    removed: diff.files.reduce(0) { $0 + $1.removed }, merged: diff.merged, base: diff.base)
            }
            let sentence = changes.isEmpty ? AgentVoice.status(candidate) : "\(AgentVoice.status(candidate)) \(changes)"
            return .result(
                dialog: IntentDialog(stringLiteral: sentence),
                view: AnyView(AgentRunSnippet(candidate: candidate, changes: changes)))
        }
        let surface = SurfaceEntitySource.agents()
        return .result(
            dialog: IntentDialog(stringLiteral: AgentStatusIntent.sentence(surface)),
            view: AnyView(AgentStatusSnippet(surface: surface)))
    }

    static func sentence(_ surface: AgentsSurface) -> String {
        if surface.activeCount == 0 { return "No ForkMesh agents are running." }
        var text = "Your agents: \(surface.summaryLine)."
        if let first = surface.active.first {
            text += " Latest: \(first.title), \(first.state.label.lowercased())."
        }
        return text
    }
}

struct AgentStatusSnippet: View {
    let surface: AgentsSurface

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(surface.summaryLine)
                .font(.headline)
            ForEach(surface.runs.prefix(4)) { (run: AgentRunCard) in
                HStack(spacing: 8) {
                    Image(systemName: run.state.systemImage)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(run.title)
                            .font(.subheadline.weight(.semibold))
                            .lineLimit(1)
                        Text("\(run.state.label) · \(run.repo)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }
            }
        }
        .padding()
    }
}

struct ForkMeshShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: MergeAgentIntent(),
            phrases: [
                "Merge \(\.$run) in \(.applicationName)",
                "Merge \(\.$run) for \(.applicationName)",
                "Merge \(\.$run) on \(.applicationName)",
                "Merge \(\.$run) with \(.applicationName)",
                "\(.applicationName) merge \(\.$run)",
                "Merge and clean up \(\.$run) in \(.applicationName)",
                "Merge a \(.applicationName) agent",
                "Merge an agent in \(.applicationName)",
                "Merge an agent for \(.applicationName)",
            ],
            shortTitle: "Merge agent",
            systemImageName: "arrow.triangle.merge")
        AppShortcut(
            intent: StopAgentIntent(),
            phrases: [
                "Stop \(\.$run) in \(.applicationName)",
                "Stop \(\.$run) for \(.applicationName)",
                "Cancel \(\.$run) in \(.applicationName)",
                "\(.applicationName) stop \(\.$run)",
                "Stop a \(.applicationName) agent",
                "Stop an agent in \(.applicationName)",
            ],
            shortTitle: "Stop agent",
            systemImageName: "stop.circle")
        AppShortcut(
            intent: ContinueAgentIntent(),
            phrases: [
                "Continue \(\.$run) in \(.applicationName)",
                "Continue \(\.$run) for \(.applicationName)",
                "Resume \(\.$run) in \(.applicationName)",
                "\(.applicationName) continue \(\.$run)",
                "Continue a \(.applicationName) agent",
                "Keep going with a \(.applicationName) agent",
            ],
            shortTitle: "Continue agent",
            systemImageName: "play.circle")
        AppShortcut(
            intent: TellAgentIntent(),
            phrases: [
                "Tell \(\.$run) in \(.applicationName)",
                "Message \(\.$run) in \(.applicationName)",
                "Send a message to \(\.$run) in \(.applicationName)",
                "\(.applicationName) tell \(\.$run)",
                "Tell a \(.applicationName) agent",
                "Message a \(.applicationName) agent",
            ],
            shortTitle: "Tell agent",
            systemImageName: "text.bubble")
        AppShortcut(
            intent: StartAgentIntent(),
            phrases: [
                "Start a new agent in \(.applicationName)",
                "Start a new agent with \(.applicationName)",
                "Start a new agent on \(.applicationName)",
                "Start a new \(.applicationName) agent",
                "Start an agent in \(.applicationName)",
                "Start a \(.applicationName) agent",
                "Start a \(.applicationName) agent on \(\.$repository)",
                "Start a new agent on \(\.$repository) in \(.applicationName)",
                "New agent in \(.applicationName)",
                "New \(.applicationName) agent",
                "New \(.applicationName) agent in \(\.$repository)",
                "Create a new agent in \(.applicationName)",
                "\(.applicationName) new agent",
                "\(.applicationName) start a new agent",
            ],
            shortTitle: "New agent",
            systemImageName: "sparkles")
        AppShortcut(
            intent: AgentStatusIntent(),
            phrases: [
                "What are my \(.applicationName) agents doing",
                "\(.applicationName) agent status",
                "How is \(\.$run) doing in \(.applicationName)",
                "What is \(\.$run) doing in \(.applicationName)",
                "Check \(\.$run) in \(.applicationName)",
                "What did \(\.$run) change in \(.applicationName)",
                "\(.applicationName) status of \(\.$run)",
            ],
            shortTitle: "Agent status",
            systemImageName: "cpu")
        AppShortcut(
            intent: AnswerAgentRequestIntent(),
            phrases: [
                "\(\.$decision) \(.applicationName) request",
                "\(\.$decision) the \(.applicationName) request",
                "\(\.$decision) my \(.applicationName) agent's request",
            ],
            shortTitle: "Answer request",
            systemImageName: "hand.raised")
        AppShortcut(
            intent: OpenScreenIntent(),
            phrases: [
                "Open \(\.$screen) in \(.applicationName)",
                "Open my \(.applicationName) \(\.$screen)",
                "Show \(.applicationName) \(\.$screen)",
                "Show my \(.applicationName) \(\.$screen)",
                "Go to \(\.$screen) in \(.applicationName)",
                "Open \(\.$repository) in \(.applicationName)",
            ],
            shortTitle: "Open screen",
            systemImageName: "square.grid.2x2")
        AppShortcut(
            intent: ListeningIntent(),
            phrases: [
                "\(\.$change) listening in \(.applicationName)",
                "\(\.$change) listening on \(.applicationName)",
                "\(\.$change) listening with \(.applicationName)",
                "\(\.$change) \(.applicationName) listening",
                "\(.applicationName) \(\.$change) listening",
                "\(\.$change) the \(.applicationName) microphone",
                "\(\.$change) speech recognition in \(.applicationName)",
                "Turn the \(.applicationName) Genie \(\.$change)",
                "Toggle listening in \(.applicationName)",
                "Toggle \(.applicationName) listening",
            ],
            shortTitle: "Listening",
            systemImageName: "ear")
        AppShortcut(
            intent: DeleteAgentIntent(),
            phrases: [
                "Delete \(\.$run) in \(.applicationName)",
                "Delete \(\.$run) from \(.applicationName)",
                "Remove \(\.$run) from \(.applicationName)",
                "\(.applicationName) delete \(\.$run)",
                "Delete \(\.$scope) in \(.applicationName)",
                "Delete \(\.$scope) from \(.applicationName)",
                "Delete \(\.$scope) on \(.applicationName)",
                "Remove \(\.$scope) from \(.applicationName)",
                "\(.applicationName) delete \(\.$scope)",
                "Delete a \(.applicationName) agent",
            ],
            shortTitle: "Delete agent",
            systemImageName: "trash")
    }
}
