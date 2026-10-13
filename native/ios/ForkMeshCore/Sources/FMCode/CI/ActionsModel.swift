import FMApi
import FMBase
import Foundation
import Observation

public enum ActionsControlKind: String, Sendable, CaseIterable {
    case dispatch, rerun, cancel, approve, reject
    case setWorkflowEnabled = "set-workflow-enabled"
    case setRepositoryEnabled = "set-repository-enabled"

    public var label: String {
        switch self {
        case .dispatch: return "Run workflow"
        case .rerun: return "Re-run"
        case .cancel: return "Cancel"
        case .approve: return "Approve"
        case .reject: return "Reject"
        case .setWorkflowEnabled: return "Workflow switch"
        case .setRepositoryEnabled: return "Actions switch"
        }
    }

    public var needsAuthentication: Bool { self == .approve || self == .dispatch || self == .setRepositoryEnabled }
    public var isDestructive: Bool { self == .cancel || self == .reject }
}

public struct ActionsControlRequest: Equatable, Sendable {
    public var requestId: String
    public var kind: ActionsControlKind
    public var node: String
    public var runId: Int?
    public var workflow: String?
    public var ref: String?
    public var enabled: Bool?

    public static let requestIdPattern = try! NSRegularExpression(pattern: "^[A-Za-z0-9_-]{16,64}$")
    public static let workflowPattern = try! NSRegularExpression(pattern: "^\\.forkmesh/[A-Za-z0-9._-]{1,96}\\.ya?ml$")

    public static func makeRequestId() -> String { B64.encodeUrlNoPadding(Bytes.random(18)) }

    public static func matches(_ regex: NSRegularExpression, _ text: String) -> Bool {
        regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
    }

    public static func branchRef(_ branch: String) -> String {
        let trimmed = branch.trimmingCharacters(in: .whitespaces)
        return trimmed.hasPrefix("refs/") ? trimmed : "refs/heads/\(trimmed)"
    }

    public var isValid: Bool {
        guard ActionsControlRequest.matches(ActionsControlRequest.requestIdPattern, requestId), !node.isEmpty else { return false }
        switch kind {
        case .rerun, .cancel, .approve, .reject:
            return (runId ?? 0) > 0
        case .dispatch:
            guard let workflow, let ref else { return false }
            return ActionsControlRequest.matches(ActionsControlRequest.workflowPattern, workflow)
                && ref.hasPrefix("refs/heads/") && ref.count > "refs/heads/".count && ref.count <= 160
        case .setWorkflowEnabled:
            guard let workflow else { return false }
            return enabled != nil && ActionsControlRequest.matches(ActionsControlRequest.workflowPattern, workflow)
        case .setRepositoryEnabled:
            return enabled != nil
        }
    }

    public var body: JSONObject {
        var body: JSONObject = ["requestId": .string(requestId), "action": .string(kind.rawValue), "node": .string(node)]
        if let runId { body["runId"] = .int(Int64(runId)) }
        if let workflow { body["workflow"] = .string(workflow) }
        if let ref { body["ref"] = .string(ref) }
        if let enabled { body["enabled"] = .bool(enabled) }
        return body
    }
}

public struct ActionsPendingControl: Equatable, Sendable, Identifiable {
    public var request: ActionsControlRequest
    public var sentAtMs: Int64
    public var id: String { request.requestId }
}

public struct ActionsNotice: Equatable, Sendable {
    public var text: String
    public var good: Bool
}

public enum CILog {
    public static let maxBytes = 16 * 1024
    public static let maxLines = 400

    public static func lines(_ text: String, maxLines: Int = CILog.maxLines) -> [String] {
        var clipped = text
        if clipped.utf8.count > maxBytes {
            clipped = String(decoding: Array(clipped.utf8.suffix(maxBytes)), as: UTF8.self)
        }
        let all = clipped.replacingOccurrences(of: "\r\n", with: "\n").split(separator: "\n", omittingEmptySubsequences: false)
            .map { stripAnsi(String($0)) }
        var trimmed = Array(all.suffix(maxLines))
        while let last = trimmed.last, last.trimmingCharacters(in: .whitespaces).isEmpty { trimmed.removeLast() }
        return trimmed
    }

    private static let ansi = try! NSRegularExpression(pattern: "\u{1B}\\[[0-9;?]*[A-Za-z]")

    public static func stripAnsi(_ line: String) -> String {
        ansi.stringByReplacingMatches(in: line, range: NSRange(line.startIndex..., in: line), withTemplate: "")
    }

    public static func failureExcerpt(_ text: String, lines count: Int = 60) -> String {
        let all = lines(text)
        let markers = ["error", "failed", "fatal", "panic", "exception", "assert"]
        if let index = all.lastIndex(where: { line in markers.contains { line.lowercased().contains($0) } }) {
            let start = max(0, index - count / 2)
            let end = min(all.count, start + count)
            return all[start..<end].joined(separator: "\n")
        }
        return all.suffix(count).joined(separator: "\n")
    }
}

public enum CIPrompts {
    public static func fixFailedRun(_ run: ActionsRunSummary, repo: RepoRef) -> ComposeTarget {
        var lines = [
            "The \(run.displayName) workflow failed on \(run.refLabel.isEmpty ? "this branch" : run.refLabel) (run #\(run.id), commit \(run.shortCommit)).",
            "Find the cause and fix it.",
        ]
        let failedSteps = run.steps.filter { $0.status == "failed" }.map(\.name)
        if !failedSteps.isEmpty { lines.append("Failed steps: \(failedSteps.joined(separator: ", ")).") }
        let excerpt = CILog.failureExcerpt(run.logTail)
        if !excerpt.isEmpty { lines.append("Log excerpt:\n```\n\(excerpt)\n```") }
        return ComposeTarget(repo: repo, pull: run.pullNumber > 0 ? run.pullNumber : nil, text: lines.joined(separator: "\n\n"))
    }
}

public struct PullChecksSummary: Equatable, Sendable {
    public var runs: [ActionsRunSummary]

    public init(runs: [ActionsRunSummary], pull: Int) {
        var latest: [String: ActionsRunSummary] = [:]
        for run in runs where run.pullNumber == pull {
            let key = run.workflowPath.isEmpty ? run.workflow : run.workflowPath
            if let existing = latest[key], existing.createdAtMs >= run.createdAtMs { continue }
            latest[key] = run
        }
        self.runs = latest.values.sorted { $0.displayName < $1.displayName }
    }

    public var passed: Int { runs.filter { $0.status == .success || $0.status == .skipped }.count }
    public var failed: Int { runs.filter { $0.status == .failed || $0.status == .rejected || $0.status == .cancelled }.count }
    public var pending: Int { runs.filter(\.status.isActive).count }
    public var awaitingApproval: [ActionsRunSummary] { runs.filter { $0.status == .awaitingApproval } }

    public var state: String {
        if runs.isEmpty { return "none" }
        if failed > 0 { return "failed" }
        if pending > 0 { return "pending" }
        return "passed"
    }

    public var label: String {
        switch state {
        case "none": return "No checks"
        case "failed": return "\(failed) of \(runs.count) checks failed"
        case "pending": return "\(pending) of \(runs.count) checks running"
        default: return "All \(runs.count) checks passed"
        }
    }
}

@MainActor
@Observable
public final class RepoActionsModel {
    public let repo: RepoRef
    public private(set) var summary: ActionsSummary?
    public private(set) var loading = false
    public private(set) var error: String?
    public private(set) var pending: [ActionsPendingControl] = []
    public private(set) var notice: ActionsNotice?
    public var workflowFilter: String?
    public private(set) var fromDesktop = false
    public private(set) var desktopProblem: String?
    public private(set) var desktopLogs: [Int: String] = [:]

    @ObservationIgnored private let client: CIClient
    @ObservationIgnored private let desktop: DesktopActions?
    @ObservationIgnored private var desktopTap: UUID?
    @ObservationIgnored private var followed: Int?
    @ObservationIgnored private var logNext: [Int: Int] = [:]
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let makeRequestId: () -> String
    @ObservationIgnored private var generation = 0

    public static let pendingTimeoutMs: Int64 = 120_000
    public static let followUpDelaySeconds: TimeInterval = 5
    public static let pollDelaySeconds: TimeInterval = 30

    public init(client: CIClient, repo: RepoRef, desktop: DesktopActions? = nil, clock: Clock = SystemClock(),
                makeRequestId: @escaping () -> String = ActionsControlRequest.makeRequestId) {
        self.client = client
        self.desktop = desktop
        self.repo = repo
        self.clock = clock
        self.makeRequestId = makeRequestId
    }

    public var commitFilter: String?

    public var runs: [ActionsRunSummary] {
        var all = summary?.runs ?? []
        if let commit = commitFilter { all = all.filter { RepoActionsModel.sameCommit($0.commit, commit) } }
        guard let filter = workflowFilter else { return all }
        return all.filter { $0.workflowPath == filter || $0.workflow == filter }
    }

    public static func sameCommit(_ run: String, _ wanted: String) -> Bool {
        let a = run.dartTrimmed.lowercased()
        let b = wanted.dartTrimmed.lowercased()
        guard a.count >= 7, b.count >= 7 else { return false }
        return a.hasPrefix(b) || b.hasPrefix(a)
    }

    public var commitEmptyMessage: String {
        "No runs for this commit yet. Workflows that run on push start a few seconds after the merge lands."
    }

    public var workflows: [ActionsWorkflowSummary] { summary?.workflows ?? [] }
    public var dispatchableWorkflows: [ActionsWorkflowSummary] { canControl ? workflows.filter(\.canDispatch) : [] }
    public var canControl: Bool { (summary?.controlAvailable ?? false) && !(summary?.node.isEmpty ?? true) }
    public var hasActiveRuns: Bool { runs.contains { $0.status.isActive } }
    public var isUnavailable: Bool { summary.map { !$0.available } ?? false }

    public var emptyMessage: String {
        if isUnavailable {
            if let desktopProblem { return "No node that hosts this repository is answering right now. \(desktopProblem)" }
            return "No node that hosts this repository is answering right now. Pull to refresh later."
        }
        if summary?.actionsEnabled == false { return "Actions are turned off for this repository." }
        return "No workflow runs yet."
    }

    public func run(_ id: Int) -> ActionsRunSummary? {
        guard var run = summary?.runs.first(where: { $0.id == id }) else { return nil }
        if fromDesktop, let log = desktopLogs[id] { run.logTail = log }
        return run
    }

    public func checks(forPull number: Int) -> PullChecksSummary { PullChecksSummary(runs: summary?.runs ?? [], pull: number) }

    public func load() async {
        generation += 1
        let mine = generation
        loading = true
        var local: ActionsSummary?
        if let desktop {
            watchDesktop(desktop)
            do {
                local = try await desktop.summary(repo)
                guard mine == generation else { return }
                desktopProblem = nil
            } catch {
                guard mine == generation else { return }
                desktopProblem = DesktopActions.explain(error)
            }
        }
        if let local, !local.runs.isEmpty {
            apply(local, fromDesktop: true)
            loading = false
            return
        }
        do {
            let loaded = try await client.actionsRuns(repo)
            guard mine == generation else { return }
            if !loaded.available, let local {
                apply(local, fromDesktop: true)
            } else {
                apply(loaded, fromDesktop: false)
            }
        } catch {
            guard mine == generation else { return }
            if let local {
                apply(local, fromDesktop: true)
            } else {
                self.error = "\(error)"
            }
        }
        loading = false
    }

    private func apply(_ loaded: ActionsSummary, fromDesktop: Bool) {
        summary = loaded
        self.fromDesktop = fromDesktop
        error = nil
        settle()
    }

    private func watchDesktop(_ desktop: DesktopActions) {
        if desktopTap != nil { return }
        desktopTap = desktop.onChange { [weak self] in
            guard let self else { return }
            Task {
                await self.load()
                if let runId = self.followed { await self.loadLog(runId: runId) }
            }
        }
    }

    public func stop() {
        if let desktopTap { desktop?.removeTap(desktopTap) }
        desktopTap = nil
        followed = nil
    }

    public func follow(runId: Int) async {
        followed = runId
        await loadLog(runId: runId)
    }

    public func loadLog(runId: Int) async {
        guard fromDesktop, let desktop else { return }
        let since = logNext[runId]
        guard let slice = try? await desktop.log(repo, runId: runId, since: since) else { return }
        let base = since != nil && !slice.reset && slice.from == since ? desktopLogs[runId] ?? "" : ""
        var text = base + slice.text
        if text.count > DesktopActions.maxLogChars { text = String(text.suffix(DesktopActions.maxLogChars)) }
        desktopLogs[runId] = text
        logNext[runId] = slice.next
    }

    public func actions(for run: ActionsRunSummary) -> [ActionsControlKind] {
        guard canControl else { return [] }
        if pending.contains(where: { $0.request.runId == run.id }) { return [] }
        switch run.status {
        case .awaitingApproval: return [.approve, .reject]
        case .queued, .running: return [.cancel]
        case .success, .failed, .rejected, .cancelled, .skipped: return [.rerun]
        case .unknown: return []
        }
    }

    public func isPending(runId: Int) -> Bool { pending.contains { $0.request.runId == runId } }

    @discardableResult
    public func send(_ kind: ActionsControlKind, runId: Int? = nil, workflow: String? = nil, branch: String? = nil,
                     enabled: Bool? = nil) async -> Bool {
        if fromDesktop, let desktop {
            return await sendToDesktop(desktop, kind, runId: runId, workflow: workflow, branch: branch, enabled: enabled)
        }
        guard let node = summary?.node, canControl else {
            notice = ActionsNotice(text: "\(kind.label): this repository's node does not accept remote control.", good: false)
            return false
        }
        let request = ActionsControlRequest(
            requestId: makeRequestId(), kind: kind, node: node, runId: runId, workflow: workflow,
            ref: branch.map(ActionsControlRequest.branchRef), enabled: enabled)
        guard request.isValid else {
            notice = ActionsNotice(text: "\(kind.label): that request is not valid.", good: false)
            return false
        }
        pending.append(ActionsPendingControl(request: request, sentAtMs: clock.nowMillis))
        notice = nil
        do {
            try await client.actionsControl(repo, request)
            notice = ActionsNotice(text: "\(kind.label) sent to \(node).", good: true)
            return true
        } catch {
            pending.removeAll { $0.id == request.requestId }
            notice = ActionsNotice(text: "\(kind.label): \(error)", good: false)
            return false
        }
    }

    private func sendToDesktop(_ desktop: DesktopActions, _ kind: ActionsControlKind, runId: Int?, workflow: String?,
                               branch: String?, enabled: Bool?) async -> Bool {
        guard DesktopActions.action(for: kind, enabled: enabled) != nil else {
            notice = ActionsNotice(text: "\(kind.label): change this on the desktop.", good: false)
            return false
        }
        let request = ActionsControlRequest(
            requestId: makeRequestId(), kind: kind, node: desktop.desktopName, runId: runId, workflow: workflow,
            ref: branch.map(ActionsControlRequest.branchRef), enabled: enabled)
        pending.append(ActionsPendingControl(request: request, sentAtMs: clock.nowMillis))
        notice = nil
        do {
            let message = try await desktop.control(
                repo, kind, runId: runId, workflow: workflow, branch: branch, enabled: enabled)
            pending.removeAll { $0.id == request.requestId }
            notice = ActionsNotice(text: message.isEmpty ? "\(kind.label) sent to \(desktop.desktopName)." : message, good: true)
            await load()
            return true
        } catch {
            pending.removeAll { $0.id == request.requestId }
            notice = ActionsNotice(text: "\(kind.label): \(DesktopActions.explain(error))", good: false)
            return false
        }
    }

    public func dispatch(workflow: ActionsWorkflowSummary, branch: String) async -> Bool {
        await send(.dispatch, workflow: workflow.path, branch: branch)
    }

    public var canSwitchWorkflows: Bool { canControl }

    public func isSwitching(_ workflow: ActionsWorkflowSummary) -> Bool {
        pending.contains { $0.request.kind == .setWorkflowEnabled && $0.request.workflow == workflow.path }
    }

    @discardableResult
    public func setEnabled(_ workflow: ActionsWorkflowSummary, _ enabled: Bool) async -> Bool {
        guard workflow.enabled != enabled, !isSwitching(workflow) else { return true }
        return await send(.setWorkflowEnabled, workflow: workflow.path, enabled: enabled)
    }

    public func latestRun(of workflow: ActionsWorkflowSummary) -> ActionsRunSummary? {
        (summary?.runs ?? []).first { $0.workflowPath == workflow.path || $0.workflow == workflow.path }
    }

    public func runs(of workflow: ActionsWorkflowSummary) -> [ActionsRunSummary] {
        (summary?.runs ?? []).filter { $0.workflowPath == workflow.path || $0.workflow == workflow.path }
    }

    public var manualWorkflows: [ActionsWorkflowSummary] { workflows.filter { $0.enabled && $0.manual } }
    public var automaticWorkflows: [ActionsWorkflowSummary] { workflows.filter { $0.enabled && !$0.manual } }
    public var disabledWorkflows: [ActionsWorkflowSummary] { workflows.filter { !$0.enabled } }

    func settle() {
        let controls = summary?.controls ?? []
        let now = clock.nowMillis
        var outcome: ActionsNotice?
        pending.removeAll { entry in
            if let result = controls.first(where: { $0.requestId == entry.request.requestId }) {
                let label = entry.request.kind.label
                outcome = result.applied
                    ? ActionsNotice(text: "\(label) applied.", good: true)
                    : ActionsNotice(text: "\(label) was refused\(result.reason.isEmpty ? "" : ": \(result.reason.replacingOccurrences(of: "_", with: " "))").", good: false)
                return true
            }
            return now - entry.sentAtMs > RepoActionsModel.pendingTimeoutMs
        }
        if let outcome { notice = outcome }
    }

    public var needsFollowUp: Bool { !pending.isEmpty }

    public var nextLoadDelaySeconds: TimeInterval {
        needsFollowUp || hasActiveRuns ? RepoActionsModel.followUpDelaySeconds : RepoActionsModel.pollDelaySeconds
    }
}
