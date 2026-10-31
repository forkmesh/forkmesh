import AppIntents
import FMAgents
import FMApi
import FMControl
import FMSurfaces
import Foundation
import SwiftUI

struct AgentVoiceError: Error, CustomLocalizedStringResourceConvertible {
    let message: String

    init(_ message: String) {
        self.message = message
    }

    var localizedStringResource: LocalizedStringResource { LocalizedStringResource(stringLiteral: message) }
}

@MainActor
final class AgentVoiceGate {
    var done = false
}

@MainActor
enum AgentVoiceSource {
    static let suggestionLimit = 50
    static let readyWaitSteps = 32
    static let changesWaitSeconds: Double = 6
    private static var shortcutsKey = ""

    static func center() async throws -> AgentControlCenter {
        var waited = 0
        while SurfacesCoordinator.shared == nil, waited < readyWaitSteps {
            try? await Task.sleep(nanoseconds: 250_000_000)
            waited += 1
        }
        guard let services = AppServices.current else { throw AgentVoiceError(SurfaceCommandError.appNotReady.message) }
        guard !(services.auth.session?.nodeName ?? "").isEmpty else { throw AgentVoiceError("Sign in to ForkMesh first.") }
        guard let center = HostsFeature.agents else { throw AgentVoiceError("Pair this phone with your desktop first.") }
        return center
    }

    static func ops() async throws -> AgentOpsClient {
        _ = try await center()
        guard let client = AgentOpsFeature.client else { throw AgentVoiceError("Pair this phone with your desktop first.") }
        return client
    }

    static func approvals() async throws -> ApprovalsQueue {
        _ = try await center()
        guard let queue = AgentOpsFeature.approvals else { throw AgentVoiceError("Pair this phone with your desktop first.") }
        return queue
    }

    static func candidate(_ row: UnifiedAgentRow) -> AgentVoiceCandidate {
        AgentVoiceCandidate(
            id: row.id, number: row.isDesktop && !row.outside && !row.elsewhere ? row.localId : 0, title: row.title,
            repo: row.repoLabel, statusLabel: row.statusLabel, isActive: row.isActive,
            updatedAtMs: Int64(row.updatedAtMs))
    }

    static func candidate(_ card: AgentRunCard) -> AgentVoiceCandidate {
        AgentVoiceCandidate(
            id: card.id, number: AgentVoice.number(rowId: card.id) ?? 0, title: card.title, repo: card.repo,
            statusLabel: card.state.label, isActive: card.state.isActive, updatedAtMs: card.updatedAtMs)
    }

    static func candidates() async -> [AgentVoiceCandidate] {
        if let center = try? await center() {
            if center.rows.isEmpty { await center.refresh() }
            if !center.rows.isEmpty { return center.rows.map(candidate) }
        }
        return SurfaceEntitySource.agents().runs.map(candidate)
    }

    static func row(_ run: AgentRunEntity) async throws -> UnifiedAgentRow {
        let center = try await center()
        let id = AgentRouteDestination.rowId(run.id)
        if let row = center.rows.first(where: { $0.id == id }) { return row }
        await center.refresh()
        if let row = center.rows.first(where: { $0.id == id }) { return row }
        throw AgentVoiceError("\(run.title) is not on your agent list.")
    }

    static func desktopNumber(_ run: AgentRunEntity) throws -> Int {
        guard let number = AgentVoice.number(rowId: AgentRouteDestination.rowId(run.id)) else {
            throw AgentVoiceError("Siri can only do that for agents on your desktop.")
        }
        return number
    }

    static func name(_ run: AgentRunEntity) -> String {
        AgentVoice.number(rowId: AgentRouteDestination.rowId(run.id)).map { AgentVoice.name(number: $0) } ?? run.title
    }

    static func controlError(_ error: Error) -> AgentVoiceError {
        if let voice = error as? AgentVoiceError { return voice }
        if AgentOpsClient.errorCode(error) != nil { return AgentVoiceError(AgentOpsClient.describe(error)) }
        return AgentVoiceError(ControlErrorText.describe(error))
    }

    static func changed() {
        SurfacesFeature.refresh()
    }

    static func changes(_ client: AgentOpsClient, number: Int) async -> AgentDiff? {
        await withCheckedContinuation { (continuation: CheckedContinuation<AgentDiff?, Never>) in
            let gate = AgentVoiceGate()
            Task { @MainActor in
                let diff = try? await client.diff(sessionId: number)
                if !gate.done {
                    gate.done = true
                    continuation.resume(returning: diff)
                }
            }
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: UInt64(changesWaitSeconds * 1_000_000_000))
                if !gate.done {
                    gate.done = true
                    continuation.resume(returning: nil)
                }
            }
        }
    }

    static func rowsChanged(_ rows: [UnifiedAgentRow]) {
        let key = rows.map { "\($0.id)|\($0.title)" }.joined(separator: "\n")
        guard key != shortcutsKey else { return }
        shortcutsKey = key
        ForkMeshShortcuts.updateAppShortcutParameters()
    }
}

enum AgentRequestDecision: String, AppEnum {
    case approve
    case deny

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Decision"
    static let caseDisplayRepresentations: [AgentRequestDecision: DisplayRepresentation] = [
        .approve: "Approve",
        .deny: "Deny",
    ]
}

enum ForkMeshScreen: String, AppEnum {
    case agents
    case running
    case inbox
    case approvals
    case mentions
    case reviews
    case repositories
    case chat
    case notes
    case tasks
    case calendar
    case timers
    case desktops
    case activity
    case stats
    case search
    case settings

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Screen"
    static let caseDisplayRepresentations: [ForkMeshScreen: DisplayRepresentation] = [
        .agents: "Agents",
        .running: "Running agents",
        .inbox: "Inbox",
        .approvals: "Approvals",
        .mentions: "Mentions",
        .reviews: "Reviews",
        .repositories: "Repositories",
        .chat: "Chat",
        .notes: "Notes",
        .tasks: "Tasks",
        .calendar: "Calendar",
        .timers: "Timers",
        .desktops: "Desktops",
        .activity: "Activity",
        .stats: "Stats",
        .search: "Search",
        .settings: "Settings",
    ]

    var route: Route {
        switch self {
        case .agents: return .agents(.all)
        case .running: return .agents(.running)
        case .inbox: return .inbox(.all)
        case .approvals: return .inbox(.approvals)
        case .mentions: return .inbox(.mentions)
        case .reviews: return .inbox(.reviews)
        case .repositories: return .repos(.all)
        case .chat: return .chat
        case .notes: return .notes
        case .tasks: return .tasks(filter: nil)
        case .calendar: return .calendar(date: nil, mode: .agenda)
        case .timers: return .timers
        case .desktops: return .desktops
        case .activity: return .activity
        case .stats: return .stats
        case .search: return .search(query: "", scope: .everything)
        case .settings: return .settings(section: nil)
        }
    }
}

struct MergeAgentIntent: AppIntent {
    static let title: LocalizedStringResource = "Merge agent"
    static let description = IntentDescription("Merges an agent's branch on your desktop and cleans up its worktree.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @Parameter(title: "Agent", requestValueDialog: "Which agent should I merge?")
    var run: AgentRunEntity

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let number = try AgentVoiceSource.desktopNumber(run)
        let name = AgentVoice.name(number: number)
        let client = try await AgentVoiceSource.ops()
        let confirmation: AgentOpsConfirmation
        do {
            confirmation = try await client.prepareMerge(sessionId: number)
        } catch {
            throw AgentVoiceSource.controlError(error)
        }
        let question = confirmation.summary.isEmpty ? "Merge \(name) and clean up its branch?" : confirmation.summary
        try await requestConfirmation(result: .result(dialog: IntentDialog(stringLiteral: question)))
        if confirmation.isExpired(nowMs: client.nowMs) {
            throw AgentVoiceError(AgentOpsClient.describe(code: "confirmation_expired"))
        }
        let outcome: MergeOutcome
        do {
            outcome = try await client.merge(sessionId: number, token: confirmation.token, typed: confirmation.typeToConfirm)
        } catch {
            throw AgentVoiceSource.controlError(error)
        }
        AgentVoiceSource.changed()
        let merged = outcome.branch.isEmpty ? confirmation.typeToConfirm : outcome.branch
        let stats = outcome.stats.flatMap { $0.hasLanding ? $0 : nil }
        let spoken = AgentVoice.merged(
            name, branch: merged, files: stats?.files, added: stats?.insertions ?? 0, removed: stats?.deletions ?? 0)
        return .result(dialog: IntentDialog(stringLiteral: spoken))
    }
}

struct StopAgentIntent: AppIntent {
    static let title: LocalizedStringResource = "Stop agent"
    static let description = IntentDescription("Stops a running ForkMesh agent.")

    @Parameter(title: "Agent", requestValueDialog: "Which agent should I stop?")
    var run: AgentRunEntity

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let row = try await AgentVoiceSource.row(run)
        let name = AgentVoiceSource.candidate(row).name
        guard row.isActive else { return .result(dialog: IntentDialog(stringLiteral: "\(name) is not running.")) }
        if !row.outsideUser.isEmpty { throw AgentVoiceError(row.outsideSummary) }
        guard !row.outside || row.canStop else {
            throw AgentVoiceError(
                "\(name) was started outside ForkMesh and cannot be stopped from here. "
                    + (row.canPrompt ? "Tell it to stop, or stop it in the terminal that started it."
                        : "Stop it in the terminal that started it."))
        }
        do {
            try await HostsFeature.agents?.stop(row)
        } catch {
            throw AgentVoiceSource.controlError(error)
        }
        AgentVoiceSource.changed()
        if row.outside {
            return .result(dialog: IntentDialog(stringLiteral: "Interrupted \(name): pressed \(row.interruptKeyLabel) in its terminal."))
        }
        return .result(dialog: IntentDialog(stringLiteral: AgentVoice.stopped([name])))
    }
}

struct StopAllAgentsIntent: AppIntent {
    static let title: LocalizedStringResource = "Stop all agents"
    static let description = IntentDescription("Stops every running ForkMesh agent.")

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let center = try await AgentVoiceSource.center()
        await center.refresh()
        let running = center.rows.filter { $0.isActive && !$0.outside }
        if running.isEmpty { return .result(dialog: IntentDialog(stringLiteral: AgentVoice.stopped([]))) }
        let noun = running.count == 1 ? "agent" : "agents"
        try await requestConfirmation(
            result: .result(dialog: IntentDialog(stringLiteral: "Stop \(running.count) running \(noun)?")))
        var stopped: [String] = []
        var failure: Error?
        for row in running {
            do {
                try await center.stop(row)
                stopped.append(AgentVoiceSource.candidate(row).name)
            } catch {
                failure = error
            }
        }
        AgentVoiceSource.changed()
        if stopped.isEmpty, let failure { throw AgentVoiceSource.controlError(failure) }
        return .result(dialog: IntentDialog(stringLiteral: AgentVoice.stopped(stopped)))
    }
}

struct ContinueAgentIntent: AppIntent {
    static let title: LocalizedStringResource = "Continue agent"
    static let description = IntentDescription("Tells a finished desktop agent to keep going.")

    @Parameter(title: "Agent", requestValueDialog: "Which agent should continue?")
    var run: AgentRunEntity

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let row = try await AgentVoiceSource.row(run)
        let name = AgentVoiceSource.candidate(row).name
        guard row.isDesktop, !row.outside else { throw AgentVoiceError("Siri can only continue agents on your desktop.") }
        if row.isActive { return .result(dialog: IntentDialog(stringLiteral: "\(name) is still running.")) }
        guard row.canContinue else { throw AgentVoiceError("\(name) can't be continued.") }
        do {
            try await HostsFeature.agents?.continueRun(row)
        } catch {
            throw AgentVoiceSource.controlError(error)
        }
        AgentVoiceSource.changed()
        return .result(dialog: IntentDialog(stringLiteral: "\(name) is continuing."))
    }
}

struct TellAgentIntent: AppIntent {
    static let title: LocalizedStringResource = "Tell an agent"
    static let description = IntentDescription("Sends a message to a ForkMesh agent.")

    @Parameter(title: "Agent", requestValueDialog: "Which agent?")
    var run: AgentRunEntity

    @Parameter(title: "Message", requestValueDialog: "What should I tell it?")
    var message: String

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let text = message.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { throw $message.needsValueError("What should I tell it?") }
        let row = try await AgentVoiceSource.row(run)
        let name = AgentVoiceSource.candidate(row).name
        if !row.outsideUser.isEmpty { throw AgentVoiceError(row.outsideSummary) }
        guard !row.outside || row.canPrompt else {
            throw AgentVoiceError("\(name) was started outside ForkMesh. Steer it in the terminal that started it.")
        }
        let typed = row.outside && row.promptPath == .terminal
        do {
            try await HostsFeature.agents?.prompt(row, String(text.prefix(4_000)))
        } catch {
            throw AgentVoiceSource.controlError(error)
        }
        AgentVoiceSource.changed()
        if let notice = HostsFeature.agents?.promptNotice, !notice.isEmpty {
            return .result(dialog: IntentDialog(stringLiteral: notice))
        }
        return .result(dialog: IntentDialog(stringLiteral: typed ? "Typed into \(name)'s terminal." : "Sent to \(name)."))
    }
}

enum AgentDeleteScope: String, AppEnum {
    case one
    case all

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Agents"
    static let caseDisplayRepresentations: [AgentDeleteScope: DisplayRepresentation] = [
        .one: DisplayRepresentation(title: "An agent", synonyms: ["agent", "one agent"]),
        .all: DisplayRepresentation(
            title: "All agents",
            synonyms: ["all my agents", "all of my agents", "every agent", "all", "everything", "all things"]),
    ]
}

struct DeleteAgentIntent: AppIntent {
    static let title: LocalizedStringResource = "Delete agent"
    static let description = IntentDescription(
        "Deletes one desktop agent session, or all of them, with their branches and worktrees.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @Parameter(title: "Agents", default: .one)
    var scope: AgentDeleteScope

    @Parameter(title: "Agent")
    var run: AgentRunEntity?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        if scope == .all { return .result(dialog: IntentDialog(stringLiteral: try await deleteAll())) }
        guard let run else { throw $run.needsValueError("Which agent should I delete?") }
        let number = try AgentVoiceSource.desktopNumber(run)
        let name = AgentVoice.name(number: number)
        let client = try await AgentVoiceSource.ops()
        let confirmation: AgentOpsConfirmation
        do {
            confirmation = try await client.prepareDelete(sessionId: number)
        } catch {
            throw Self.prepareError(error)
        }
        let question = confirmation.summary.isEmpty ? "Delete \(name)?" : confirmation.summary
        try await requestConfirmation(result: .result(dialog: IntentDialog(stringLiteral: question)))
        if confirmation.isExpired(nowMs: client.nowMs) {
            throw AgentVoiceError(AgentOpsClient.describe(code: "confirmation_expired"))
        }
        let report: AgentDeleteReport?
        do {
            report = try await client.delete(sessionId: number, token: confirmation.token)
        } catch {
            throw AgentVoiceSource.controlError(error)
        }
        AgentVoiceSource.changed()
        if let report, report.agentKept { throw AgentVoiceError(report.summary(place: "on the desktop")) }
        return .result(dialog: IntentDialog(stringLiteral: report?.summary(place: "on the desktop") ?? "Deleted \(name)."))
    }

    @MainActor
    static func prepareError(_ error: Error) -> AgentVoiceError {
        AgentOpsClient.isUnsupported(error)
            ? AgentVoiceError("Update ForkMesh on the desktop to delete agents from here.")
            : AgentVoiceSource.controlError(error)
    }

    @MainActor
    private func deleteAll() async throws -> String {
        let center = try await AgentVoiceSource.center()
        let client = try await AgentVoiceSource.ops()
        await center.refresh()
        let rows = center.rows.filter {
            $0.isDesktop && !$0.outside && !$0.elsewhere && $0.outsideUser.isEmpty && $0.localId > 0
        }
        let unmerged = rows.filter { !$0.merged && !$0.branchName.isEmpty }.count
        let question = AgentVoice.deleteAllQuestion(
            count: rows.count, running: rows.filter(\.isActive).count, unmerged: unmerged)
        if rows.isEmpty { return question }
        try await requestConfirmation(result: .result(dialog: IntentDialog(stringLiteral: question)))
        var deleted = 0
        var failure: Error?
        for row in rows {
            do {
                let confirmation = try await client.prepareDelete(sessionId: row.localId)
                let report = try await client.delete(sessionId: row.localId, token: confirmation.token)
                if let report, report.agentKept {
                    failure = AgentVoiceError(report.summary(place: "on the desktop"))
                } else {
                    deleted += 1
                }
            } catch {
                if deleted == 0, failure == nil, AgentOpsClient.isUnsupported(error) { throw Self.prepareError(error) }
                failure = error
            }
        }
        AgentVoiceSource.changed()
        let failed = rows.count - deleted
        if deleted == 0, let failure { throw Self.prepareError(failure) }
        return AgentVoice.deletedAll(deleted: deleted, failed: failed)
    }
}

struct AnswerAgentRequestIntent: AppIntent {
    static let title: LocalizedStringResource = "Answer agent request"
    static let description = IntentDescription("Approves or denies the next request an agent is waiting on.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @Parameter(title: "Decision", default: .approve)
    var decision: AgentRequestDecision

    @Parameter(title: "Agent")
    var run: AgentRunEntity?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let queue = try await AgentVoiceSource.approvals()
        await queue.refresh()
        let number = run.flatMap { AgentVoice.number(rowId: AgentRouteDestination.rowId($0.id)) }
        let waiting = number.map { queue.approvals(forSession: $0) } ?? queue.approvals
        guard let request = waiting.first else {
            return .result(dialog: "Nothing is waiting on you.")
        }
        let asker = request.sessionId > 0 ? AgentVoice.name(number: request.sessionId) : "An agent"
        let asked = request.question.isEmpty ? request.command : request.question
        let verb = decision == .approve ? "Approve" : "Deny"
        let question = asked.isEmpty ? "\(verb) \(asker)'s request?" : "\(asker) asks: \(asked). \(verb) it?"
        try await requestConfirmation(result: .result(dialog: IntentDialog(stringLiteral: question)))
        let done = decision == .approve ? await queue.approve(request.id) : await queue.deny(request.id)
        guard done else {
            throw AgentVoiceError(queue.errorText.isEmpty ? "The desktop did not take the answer." : queue.errorText)
        }
        AgentVoiceSource.changed()
        let left = queue.approvals.count
        var reply = decision == .approve ? "Approved." : "Denied."
        if left > 0 { reply += left == 1 ? " One more request is waiting." : " \(left) more requests are waiting." }
        return .result(dialog: IntentDialog(stringLiteral: reply))
    }
}

struct OpenScreenIntent: AppIntent {
    static let title: LocalizedStringResource = "Open ForkMesh"
    static let description = IntentDescription("Opens a screen in ForkMesh.")
    static let openAppWhenRun: Bool = true

    @Parameter(title: "Screen")
    var screen: ForkMeshScreen?

    @Parameter(title: "Repository")
    var repository: RepositoryEntity?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        if let repository {
            guard let repo = repository.repo else { return .result() }
            try await SurfaceCommandCenter.shared.perform(
                .open(.repo(repo, section: nil)), waitSeconds: SurfaceAppIntentWait.open)
            return .result()
        }
        guard let screen else { throw $screen.needsValueError("Which screen?") }
        try await SurfaceCommandCenter.shared.perform(.open(screen.route), waitSeconds: SurfaceAppIntentWait.open)
        return .result()
    }
}

struct AgentRunSnippet: View {
    let candidate: AgentVoiceCandidate
    let changes: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(candidate.name)
                .font(.headline)
            if !candidate.detail.isEmpty {
                Text(candidate.detail)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            if !changes.isEmpty {
                Text(changes)
                    .font(.caption)
            }
        }
        .padding()
    }
}
