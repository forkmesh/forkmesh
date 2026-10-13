// The logic half of the local desktop screen: the connection card, the chip,
// the new-agent form, and LocalAgentScreen (one agent's transcript, a box to message it, Stop /
// Continue).

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// What the connection card shows.
public struct LocalDesktopCard: Equatable, Sendable {
    /// Material icon name.
    public let icon: String
    public let tone: LocalDesktopTone
    public let title: String
    public let detail: String
}

public enum LocalDesktopTone: Sendable {
    case success, warning, secondary, danger, accent
}

@MainActor
public enum LocalDesktopText {
    public static func card(_ control: LocalControlService) -> LocalDesktopCard {
        switch control.status {
        case .connected:
            return LocalDesktopCard(
                icon: "link", tone: .success, title: "Connected to \(control.desktopName)",
                detail: "Directly over this network, encrypted end to end. Agents, files and status go straight to "
                    + "the desktop — nothing passes through the relay.")
        case .awaitingApproval:
            return LocalDesktopCard(
                icon: "hourglass_top", tone: .warning, title: "Approve this phone on \(control.desktopName)",
                detail: "The desktop is asking whether to let this phone control it. Allow it only if it shows the "
                    + "same code.")
        case .connecting:
            return LocalDesktopCard(
                icon: "sync", tone: .secondary, title: "Connecting to \(control.desktopName)…", detail: "")
        case .failed:
            return LocalDesktopCard(icon: "link_off", tone: .danger, title: "Not connected", detail: control.error)
        case .idle:
            return LocalDesktopCard(
                icon: "link_off", tone: .secondary, title: "Not connected",
                detail: "Pick a desktop below. Once it approves this phone, the two reconnect on their own whenever "
                    + "they share a network.")
        }
    }

    /// "123 456".
    public static func spacedCode(_ code: String) -> String? {
        guard code.count == 6 else { return nil }
        return "\(code.prefix(3)) \(code.suffix(3))"
    }

    /// `LocalDesktopChip`: a label and tone, or nil for no chip.
    public static func chip(_ control: LocalControlService) -> (label: String, tone: LocalDesktopTone)? {
        switch control.status {
        case .connected: return ("\(control.desktopName) · LAN", .success)
        case .awaitingApproval: return ("Approve on \(control.desktopName)", .warning)
        case .connecting: return ("Connecting to \(control.desktopName)…", .secondary)
        case .idle, .failed:
            let hosts = control.hosts
            if hosts.isEmpty { return nil }
            return (hosts.count == 1 ? "\(hosts[0].label) on this network" : "\(hosts.count) desktops on this network", .accent)
        }
    }

    public static func hostsEmpty(blocked: Bool) -> String {
        blocked
            ? "This phone cannot reach the local network. On iPhone, turn on Local Network for ForkMesh in Settings → "
                + "Privacy & Security."
            : "Looking for ForkMesh on this Wi-Fi… Open ForkMesh on your desktop on the same network; it appears here "
                + "within a few seconds."
    }

    public static func hostSubtitle(_ host: MeshControlHost) -> String {
        ((host.ownerUser.isEmpty ? [] : [host.ownerUser]) + [host.address.address]).joined(separator: " · ")
    }

    public static func errorText(_ error: Error) -> String {
        if let control = error as? LocalControlException {
            switch control.code {
            case "not_connected", "closed": return "The desktop is not connected."
            case "no_checkout": return "The desktop has no local checkout of that repository."
            case "not_running": return "That agent is not running."
            case "not_continuable": return "That agent is still running."
            case "unknown_session": return "The desktop no longer has that agent."
            case "bad_task": return "Write what the agent should do (8000 characters at most)."
            case "not_started":
                return control.message.isEmpty
                    ? "The desktop could not start the agent."
                    : "The desktop could not start the agent. \(control.message)"
            case "timeout": return "The desktop did not answer in time."
            default: return "The desktop refused (\(control.code))."
            }
        }
        return "Something went wrong: \(error)"
    }

    public static func sizeLabel(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return String(format: "%.1f KB", Double(bytes) / 1024) }
        return String(format: "%.1f MB", Double(bytes) / (1024 * 1024))
    }

    public static func isImage(_ name: String) -> Bool {
        let lower = name.lowercased()
        return [".png", ".jpg", ".jpeg", ".gif", ".webp"].contains { lower.hasSuffix($0) }
    }

    public static func agentSubtitle(_ row: LocalAgentRow) -> String {
        [row.session.statusLabel, row.repoLabel, row.session.providerLabel].filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public static let filesIntro =
        "Files you send land in the desktop's Downloads, under \"ForkMesh from phone\". Files the desktop sends from "
        + "Settings → Phone appear here."

    public static let aiToggleTitle = "Share on-device AI with this desktop"
    public static let aiToggleDetail =
        "Lets a desktop you paired with run prompts on this phone's own model, over this network only. Off by default."
}

/// `_NewAgentCard`: which repository and agent a new desktop run uses.
@MainActor
@Observable
public final class LocalNewAgentModel {
    @ObservationIgnored let control: LocalControlService
    public var task = ""
    public var repo: String?
    public var provider: String?
    public private(set) var busy = false
    public private(set) var notice: String?
    public private(set) var noticeIsError = false

    public init(control: LocalControlService) {
        self.control = control
    }

    public var repoLabels: [String] { control.agents?.repositories.map(\.label) ?? [] }

    public var selectedRepo: String? {
        let labels = repoLabels
        if let repo, labels.contains(repo) { return repo }
        return labels.first
    }

    public var selectedProvider: String? {
        guard let agents = control.agents else { return nil }
        let ids = agents.providers.map(\.id)
        if let provider, ids.contains(provider) { return provider }
        if ids.contains(agents.defaultProvider) { return agents.defaultProvider }
        return ids.first
    }

    /// Starts the agent and sets the inline `notice` (success or the error sentence); nil when there
    /// was nothing to start.
    @discardableResult
    public func start() async -> String? {
        guard let agents = control.agents, let label = selectedRepo,
            let target = agents.repositories.first(where: { $0.label == label })
        else { return nil }
        let text = task.dartTrimmed
        if text.isEmpty { return nil }
        busy = true
        defer { busy = false }
        do {
            let id = try await control.startAgent(
                owner: target.owner, name: target.name, task: text, provider: selectedProvider ?? agents.defaultProvider)
            task = ""
            noticeIsError = false
            notice = "Started agent #\(id) on \(control.desktopName)."
        } catch {
            noticeIsError = true
            notice = LocalDesktopText.errorText(error)
        }
        return notice
    }
}

/// `LocalAgentScreen`: one desktop agent session.
@MainActor
@Observable
public final class LocalAgentModel {
    @ObservationIgnored let control: LocalControlService
    public let sessionId: Int
    public private(set) var transcript = ""
    public private(set) var attachments: [AgentAttachmentRef] = []
    public private(set) var loading = true
    public private(set) var busy = false
    public var prompt = ""
    @ObservationIgnored private var reloadQueued = false
    @ObservationIgnored private var token: ListenerToken?
    @ObservationIgnored private var lastAgents: LocalAgentsSnapshot?
    @ObservationIgnored private var debounce: Task<Void, Never>?

    public init(control: LocalControlService, sessionId: Int) {
        self.control = control
        self.sessionId = sessionId
        lastAgents = control.agents
    }

    public var row: LocalAgentRow? { control.agents?.sessions.first { $0.session.id == sessionId } }

    public var title: String { row?.title ?? "Agent #\(sessionId)" }

    public var subtitle: String {
        guard let row else { return "" }
        var parts = [row.session.statusLabel, row.repoLabel, row.session.providerLabel]
        if !row.session.branchName.isEmpty { parts.append(row.session.branchName) }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public var transcriptText: String { transcript.isEmpty ? "Nothing in the transcript yet." : transcript }

    /// Starts following the desktop: a new Agents list means the transcript may have grown.
    public func start() {
        if token == nil {
            token = control.addListener { [weak self] in
                MainActor.assumeIsolated { self?.controlChanged() }
            }
        }
        Task { await reload() }
    }

    public func stop() {
        if let token { control.removeListener(token) }
        token = nil
        debounce?.cancel()
    }

    private func controlChanged() {
        let agents = control.agents
        if agents == lastAgents { return }
        lastAgents = agents
        debounce?.cancel()
        debounce = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 400_000_000)
            if Task.isCancelled { return }
            await self?.reload()
        }
    }

    public func reload() async {
        if !control.connected {
            loading = false
            return
        }
        if busy {
            reloadQueued = true
            return
        }
        if let detail = try? await control.transcriptDetail(sessionId) {
            transcript = detail.text
            if detail.attachments != attachments { attachments = detail.attachments }
        }
        loading = false
        if reloadQueued {
            reloadQueued = false
            await reload()
        }
    }

    private func run(_ action: () async throws -> Void, _ done: String) async -> String {
        busy = true
        let message: String
        do {
            try await action()
            message = done
        } catch {
            message = LocalDesktopText.errorText(error)
        }
        busy = false
        Task { await reload() }
        return message
    }

    public func stopAgent() async -> String {
        await run({ try await self.control.stopAgent(self.sessionId) }, "Stopped.")
    }

    public func continueAgent() async -> String {
        await run({ try await self.control.continueAgent(self.sessionId) }, "Continuing on the desktop.")
    }

    /// Sends the prompt box; nil when it was empty.
    public func send() async -> String? {
        let text = prompt.dartTrimmed
        if text.isEmpty { return nil }
        prompt = ""
        return await run({ try await self.control.sendPrompt(self.sessionId, text) }, "Sent to the agent.")
    }
}
