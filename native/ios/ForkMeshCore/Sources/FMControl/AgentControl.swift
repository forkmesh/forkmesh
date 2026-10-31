// New for the native app (WS7): every agent the owner can steer, in one list, whatever it runs on.
//
//   - the desktop's own agents, over the LAN control channel or through the relay (the same
//     agents.* methods either way, so stop/continue/prompt/transcript work away from home);
//   - runs on nodes (mirror nodes, runners, desktops that took website work), read from the relay's
//     agent-session queue, with the relay's follow-up and the new stop.
//
// Each row carries a transport badge and the controls its transport supports. The watch app (WS6)
// drives agents through the phone: `AgentControlling` is the contract it can call, and the row and
// transcript types are plain `Sendable` values it can map onto its own wire frames.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMNet
import Foundation
import Observation

/// Where a run lives and how the phone reaches it.
public enum AgentTransportBadge: String, Equatable, Sendable, CaseIterable {
    /// The desktop, on this network.
    case desktopLan = "lan"
    /// The desktop, through the relay.
    case desktopRelay = "relay"
    /// A node (mirror node, runner, or a desktop's website queue), through the relay queue.
    case node = "node"

    public var label: String {
        switch self {
        case .desktopLan: return "Desktop · LAN"
        case .desktopRelay: return "Desktop · relay"
        case .node: return "Node"
        }
    }

    public static func forDesktop(_ transport: ControlTransport) -> AgentTransportBadge {
        transport == .lan ? .desktopLan : .desktopRelay
    }
}

public enum AgentPromptPath: String, Equatable, Sendable {
    case none
    case terminal
    case peer
}

public struct AgentQuestionOption: Equatable, Sendable, Identifiable {
    public let label: String
    public let detail: String

    public var id: String { label }

    init(_ json: JSONObject) {
        label = json.str("label")
        detail = json.str("description")
    }
}

public struct AgentQuestionField: Equatable, Sendable, Identifiable {
    public let id: String
    public let title: String
    public let multiSelect: Bool
    public let isSecret: Bool
    public let options: [AgentQuestionOption]

    init(_ json: JSONObject, index: Int) {
        id = String(index)
        title = json.str("question", json.str("header"))
        multiSelect = json.bool("multiSelect")
        isSecret = json.bool("isSecret")
        options = json.maps("options").map(AgentQuestionOption.init)
    }
}

public struct AgentQuestion: Equatable, Sendable, Identifiable {
    public let id: String
    public let fields: [AgentQuestionField]

    public static let composerHint = "Type your answer for the agent"

    init?(_ json: JSONObject) {
        id = json.str("id")
        fields = json.obj("input").maps("questions").enumerated().map { AgentQuestionField($0.element, index: $0.offset) }
        if id.isEmpty || fields.isEmpty { return nil }
    }
}

/// One agent in the unified list.
public struct UnifiedAgentRow: Equatable, Sendable, Identifiable {
    public var id: String
    public var badge: AgentTransportBadge
    public var title: String
    public var repoOwner: String
    public var repoName: String
    /// The machine it runs on: the desktop's name, or the node the relay queued it for.
    public var node: String
    public var status: String
    public var statusLabel: String
    public var isActive: Bool
    public var provider: String
    public var model: String
    public var updatedAtMs: Int
    public var canStop: Bool
    public var canPrompt: Bool
    public var canContinue: Bool
    /// The desktop's local session id (desktop rows), or the node's (node rows, 0 until claimed).
    public var localId: Int
    /// The relay session id of a node row.
    public var cloudSessionId: String
    public var orgScope: Bool
    public var outside: Bool
    public var kindLabel: String
    public var cwd: String
    public var branchName: String
    public var baseBranch: String
    public var filesChanged: Int
    public var ahead: Int
    public var behind: Int
    public var linesAdded: Int
    public var linesRemoved: Int
    public var conflicted: Bool
    public var hasWorktree: Bool
    public var prNumber: Int
    public var merged: Bool
    public var attachments: [AgentAttachmentRef]
    public var promptPath: AgentPromptPath
    public var outsideKind: String
    public var outsideUser: String
    public var runner: String = ""
    public var restartsOnPrompt = false
    public var canMerge = false
    public var canOpenPull = false
    public var branchMerge = AgentBranchMerge()
    public var terminal: String = ""
    public var mode: String = ""
    public var priority = 50
    public var finishedAtMs = 0
    public var createdAtMs = 0
    public var unread: Bool?
    public var backgroundTasks = 0
    public var tokensIn = 0
    public var tokensOut = 0
    public var desktopKey = ""
    public var elsewhere = false

    public init(
        id: String, badge: AgentTransportBadge, title: String, repoOwner: String, repoName: String, node: String,
        status: String, statusLabel: String, isActive: Bool, provider: String = "", model: String = "",
        updatedAtMs: Int = 0, canStop: Bool = false, canPrompt: Bool = false, canContinue: Bool = false,
        localId: Int = 0, cloudSessionId: String = "", orgScope: Bool = false, outside: Bool = false,
        kindLabel: String = "", cwd: String = "", branchName: String = "", baseBranch: String = "",
        filesChanged: Int = -1, ahead: Int = -1, behind: Int = -1, linesAdded: Int = -1, linesRemoved: Int = -1,
        conflicted: Bool = false, hasWorktree: Bool = false, prNumber: Int = 0, merged: Bool = false,
        attachments: [AgentAttachmentRef] = [], promptPath: AgentPromptPath = .none, outsideKind: String = "",
        outsideUser: String = ""
    ) {
        self.id = id
        self.badge = badge
        self.title = title
        self.repoOwner = repoOwner
        self.repoName = repoName
        self.node = node
        self.status = status
        self.statusLabel = statusLabel
        self.isActive = isActive
        self.provider = provider
        self.model = model
        self.updatedAtMs = updatedAtMs
        self.canStop = canStop
        self.canPrompt = canPrompt
        self.canContinue = canContinue
        self.localId = localId
        self.cloudSessionId = cloudSessionId
        self.orgScope = orgScope
        self.outside = outside
        self.kindLabel = kindLabel
        self.cwd = cwd
        self.branchName = branchName
        self.baseBranch = baseBranch
        self.filesChanged = filesChanged
        self.ahead = ahead
        self.behind = behind
        self.linesAdded = linesAdded
        self.linesRemoved = linesRemoved
        self.conflicted = conflicted
        self.hasWorktree = hasWorktree
        self.prNumber = prNumber
        self.merged = merged
        self.attachments = attachments
        self.promptPath = promptPath
        self.outsideKind = outsideKind
        self.outsideUser = outsideUser
    }

    public var repoLabel: String { repoOwner.isEmpty ? repoName : "\(repoOwner)/\(repoName)" }

    public var isDesktop: Bool { badge != .node }

    public var isRunner: Bool { !isDesktop && !runner.isEmpty }

    public var badgeLabel: String { isDesktop ? badge.label : RunnerAgentText.badge(runner) }

    public var hasCloudBranch: Bool { !isDesktop && !cloudSessionId.isEmpty && !branchName.isEmpty }

    public var offersMerge: Bool { hasCloudBranch && !isActive && !merged }

    public var offersCleanup: Bool { hasCloudBranch && !isActive && branchMerge.needsCleanup }

    public var offersPullOpen: Bool { hasCloudBranch && canOpenPull && prNumber <= 0 && !isActive && !merged }

    public var isPhoneTerminal: Bool { outside && !terminal.isEmpty }

    public var offersTerminal: Bool {
        isDesktop && !outside && localId > 0 && (provider == "claude-code" || provider == "codex")
    }

    public var offersModelChoice: Bool { isDesktop && !outside && localId > 0 }

    public var offersModeChoice: Bool { offersModelChoice && AgentPermissionMode.providerTakesMode(provider) }

    public var outsideNote: String {
        let what = kindLabel.isEmpty ? "This agent" : kindLabel
        let desktop = node.isEmpty ? "your desktop" : node
        let folder = cwd.isEmpty ? "" : ", in \(cwd)"
        if !outsideUser.isEmpty {
            return "\(what) is running in \(outsideUser)'s login on \(desktop). The desktop runs in a different "
                + "login, so it cannot show this agent's transcript or steer it. Open ForkMesh in \(outsideUser)'s "
                + "login to watch and steer it there."
        }
        if isPhoneTerminal {
            return "\(what) was started from your phone in a terminal on \(desktop)\(folder). Open the terminal "
                + "for full control; what you send here is typed into it."
        }
        if promptPath == .terminal {
            return "\(what) was started outside ForkMesh on \(desktop)\(folder). The desktop cannot show its "
                + "transcript; what you send here is typed into its terminal."
        }
        if canPrompt {
            return "\(what) was started outside ForkMesh on \(desktop)\(folder). The desktop cannot show its "
                + "transcript; you can still steer it here."
        }
        return "\(what) was started outside ForkMesh on \(desktop)\(folder). The desktop cannot show its "
            + "transcript; follow and steer it in the terminal that started it."
    }

    public var outsideSummary: String {
        let what = kindLabel.isEmpty ? "This agent" : kindLabel
        let desktop = node.isEmpty ? "your desktop" : node
        let folder = cwd.isEmpty ? "" : " in \(URL(fileURLWithPath: cwd).lastPathComponent)"
        if !outsideUser.isEmpty {
            return "\(what) is running in \(outsideUser)'s login on \(desktop). ForkMesh cannot read or steer "
                + "another login's agents; open ForkMesh in that login to follow it."
        }
        if isPhoneTerminal {
            return "\(what) started from your phone in a terminal on \(desktop)\(folder). Open the terminal for "
                + "full control; what you send here is typed into it."
        }
        if promptPath == .terminal {
            return "\(what) started outside ForkMesh on \(desktop)\(folder). What you send here is typed into its "
                + "terminal."
        }
        if canPrompt {
            return "\(what) started outside ForkMesh on \(desktop)\(folder). You can watch and steer it here."
        }
        return "\(what) started outside ForkMesh on \(desktop)\(folder). You can watch it here; steer it in "
            + "the terminal that started it."
    }

    public var composerHint: String {
        switch (outside, promptPath) {
        case (false, _): return restartsOnPrompt ? "Follow-up (runs the task again)" : "Follow-up for the agent"
        case (true, .terminal): return "Types into your terminal"
        case (true, .peer): return "Message (arrives as another session)"
        case (true, .none): return "Message for \(kindLabel.isEmpty ? "the agent" : kindLabel)"
        }
    }

    public var interruptKeyLabel: String {
        switch outsideKind {
        case "claude": return "Esc"
        case "codex": return "Esc"
        default: return "its interrupt key"
        }
    }

    public static func outsidePromptPath(_ agent: LocalOutsideAgent) -> AgentPromptPath {
        if agent.typeable { return .terminal }
        if agent.steer && agent.kind == "claude" { return .peer }
        return .none
    }

    public static func fromOutside(_ agent: LocalOutsideAgent, transport: ControlTransport, desktop: String)
        -> UnifiedAgentRow
    {
        let path = outsidePromptPath(agent)
        var row = UnifiedAgentRow(
            id: "outside:\(agent.id)", badge: .forDesktop(transport),
            title: agent.title.isEmpty ? agent.label : agent.title, repoOwner: agent.owner, repoName: agent.name,
            node: desktop, status: "running", statusLabel: "Running", isActive: true, provider: agent.provider,
            model: agent.model, updatedAtMs: agent.lastActivityMs, canStop: agent.stoppable, canPrompt: path != .none,
            localId: agent.id, outside: true, kindLabel: agent.label, cwd: agent.cwd, promptPath: path,
            outsideKind: agent.kind, outsideUser: agent.otherUser)
        row.terminal = agent.terminal
        return row
    }

    /// A desktop agent, from the desktop's own list.
    public static func fromDesktop(_ row: LocalAgentRow, transport: ControlTransport, desktop: String)
        -> UnifiedAgentRow
    {
        let session = row.session
        let updated = max(session.finishedAtMs, session.startedAtMs, session.createdAtMs)
        var unified = UnifiedAgentRow(
            id: "desktop:\(session.id)", badge: .forDesktop(transport), title: row.title, repoOwner: row.owner,
            repoName: row.name, node: desktop, status: session.status, statusLabel: session.statusLabel,
            isActive: session.isActive, provider: session.provider, model: session.model, updatedAtMs: updated,
            canStop: row.stoppable, canPrompt: true, canContinue: row.canContinue, localId: session.id,
            branchName: session.branchName, baseBranch: session.diffBase, filesChanged: session.filesChanged,
            ahead: session.ahead, behind: session.behind, linesAdded: session.linesAdded,
            linesRemoved: session.linesRemoved, conflicted: session.conflicted, hasWorktree: session.hasWorktree,
            prNumber: session.prNumber, merged: session.merged,
            attachments: row.attachments.map { AgentAttachmentRef(meta: $0, origin: .desktop(sessionId: session.id)) })
        unified.mode = row.mode
        unified.priority = session.priority
        unified.finishedAtMs = session.finishedAtMs
        unified.createdAtMs = session.createdAtMs
        unified.unread = row.unread
        unified.backgroundTasks = row.backgroundTasks
        unified.tokensIn = row.tokensIn
        unified.tokensOut = row.tokensOut
        return unified
    }

    public static func forPull(_ rows: [UnifiedAgentRow], owner: String, name: String, number: Int, head: String)
        -> UnifiedAgentRow?
    {
        rows.first { row in
            !row.outside && !row.merged && row.repoName == name && (row.repoOwner.isEmpty || row.repoOwner == owner)
                && ((number > 0 && row.prNumber == number) || (!head.isEmpty && row.branchName == head))
        }
    }

    public static func forRoute(_ id: String, in rows: [UnifiedAgentRow]) -> UnifiedAgentRow? {
        if let exact = rows.first(where: { $0.id == id }) { return exact }
        if id.hasPrefix("node:") {
            let session = String(id.dropFirst("node:".count))
            if session.isEmpty { return nil }
            return rows.first { !$0.isDesktop && !$0.outside && $0.cloudSessionId == session }
        }
        guard id.hasPrefix("desktop:") else { return nil }
        let elsewhere = rows.filter { $0.elsewhere && $0.id.hasSuffix(":\(id)") }
        return elsewhere.count == 1 ? elsewhere[0] : nil
    }

    /// A run on the relay queue.
    public static func fromCloud(_ session: AgentLaunchSession) -> UnifiedAgentRow {
        let updated = max(session.updatedAtMs, session.completedAtMs, session.createdAtMs)
        let title = session.title.dartTrimmed.isEmpty ? session.retryPrompt : session.title
        let stats = session.branchStats
        var row = UnifiedAgentRow(
            id: "node:\(session.sessionId)", badge: .node,
            title: title.isEmpty ? "Agent run" : String(title.prefix(120)),
            repoOwner: session.owner, repoName: session.repo,
            node: session.runner.isEmpty ? session.targetNode : session.runner, status: session.status,
            statusLabel: session.statusLabel, isActive: session.isActive, provider: session.provider,
            model: session.requestedModel, updatedAtMs: updated,
            canStop: session.isActive, canPrompt: session.isPromptable || session.restartsOnPrompt,
            canContinue: session.localAgentId > 0 && !session.isActive && !session.isMergedBranch,
            localId: session.localAgentId, cloudSessionId: session.sessionId, orgScope: session.orgScope,
            branchName: session.branchName, baseBranch: session.baseBranch,
            filesChanged: stats.known ? stats.files : -1, ahead: stats.known ? stats.ahead : -1,
            behind: stats.known ? stats.behind : -1, linesAdded: stats.known ? stats.added : -1,
            linesRemoved: stats.known ? stats.removed : -1, conflicted: stats.conflicted || session.branchMerge.isConflict,
            prNumber: session.prNumber, merged: session.isMergedBranch,
            attachments: session.history.first(where: \.isUser)
                .map { session.attachmentRefs($0, owner: session.owner, repo: session.repo) } ?? [])
        row.runner = session.runner
        row.restartsOnPrompt = session.restartsOnPrompt
        row.canMerge = session.canMerge
        row.canOpenPull = session.canOpenPull && session.prNumber <= 0
        row.branchMerge = session.branchMerge
        row.finishedAtMs = session.completedAtMs
        row.createdAtMs = session.createdAtMs
        return row
    }

    public static func cloudTranscript(_ session: AgentLaunchSession) -> String {
        var parts: [String] = []
        for turn in session.history {
            let text = turn.text.replacingOccurrences(of: "\r\n", with: "\n").dartTrimmed
            if text.isEmpty { continue }
            if turn.isUser {
                let lines = text.components(separatedBy: "\n").filter { !$0.dartTrimmed.isEmpty }
                parts.append("> " + lines.joined(separator: "\n"))
            } else if turn.role.lowercased() == "security" {
                parts.append("!! " + text.components(separatedBy: "\n").joined(separator: " "))
            } else {
                parts.append(text)
            }
        }
        if session.isFailed, !session.lastError.dartTrimmed.isEmpty {
            parts.append("!! " + session.lastError.dartTrimmed.components(separatedBy: "\n").joined(separator: " "))
        }
        if session.isActive, !session.liveText.dartTrimmed.isEmpty {
            parts.append(session.liveText.dartTrimmed)
        }
        return parts.joined(separator: "\n\n")
    }
}

/// Merges the desktop's list with the relay queue: running first, then newest. A relay run the
/// desktop in reach is itself running (the desktop took the website job) is shown once, as the
/// desktop row, whose controls are live.
public func mergeAgentRows(desktop: [UnifiedAgentRow], cloud: [UnifiedAgentRow], desktopName: String)
    -> [UnifiedAgentRow]
{
    let name = desktopName.lowercased()
    var held = Set(desktop.map { "\($0.node.lowercased())#\($0.localId)" })
    if !name.isEmpty { held.formUnion(desktop.filter { !$0.elsewhere }.map { "\(name)#\($0.localId)" }) }
    let others = cloud.filter { row in
        !(row.localId > 0 && !row.node.isEmpty && held.contains("\(row.node.lowercased())#\(row.localId)"))
    }
    return (desktop + others).sorted { a, b in
        if a.isActive != b.isActive { return a.isActive }
        if a.updatedAtMs != b.updatedAtMs { return a.updatedAtMs > b.updatedAtMs }
        return a.id < b.id
    }
}

public struct OtherDesktopAgents: Equatable, Sendable, Identifiable {
    public let key: String
    public let name: String
    public let rows: [UnifiedAgentRow]
    public let note: String

    public init(key: String, name: String, rows: [UnifiedAgentRow], note: String = "") {
        self.key = key
        self.name = name
        self.rows = rows
        self.note = note
    }

    public var id: String { key }
    public var running: Int { rows.filter(\.isActive).count }

    public var summary: String {
        if !note.isEmpty { return note }
        if rows.isEmpty { return "No agents" }
        let total = rows.count == 1 ? "1 agent" : "\(rows.count) agents"
        return running > 0 ? "\(total) · \(running) running" : total
    }

    public nonisolated static func listed(
        _ snapshot: LocalAgentsSnapshot, key: String, name: String, transport: ControlTransport
    ) -> OtherDesktopAgents {
        let rows = (snapshot.sessions.map { UnifiedAgentRow.fromDesktop($0, transport: transport, desktop: name) }
            + snapshot.outside.map { UnifiedAgentRow.fromOutside($0, transport: transport, desktop: name) })
            .map { (row: UnifiedAgentRow) -> UnifiedAgentRow in
                var tagged = row
                tagged.id = "\(key):\(row.id)"
                tagged.desktopKey = key
                tagged.elsewhere = true
                return tagged
            }
            .sorted { a, b in
                if a.isActive != b.isActive { return a.isActive }
                if a.updatedAtMs != b.updatedAtMs { return a.updatedAtMs > b.updatedAtMs }
                return a.id < b.id
            }
        return OtherDesktopAgents(key: key, name: name, rows: rows)
    }
}

public struct DesktopListCover: Equatable, Sendable {
    public nonisolated static let listCap = 200

    public let oldestCreatedAtMs: Int

    public init(oldestCreatedAtMs: Int = 0) {
        self.oldestCreatedAtMs = oldestCreatedAtMs
    }

    public init(_ snapshot: LocalAgentsSnapshot) {
        let created = snapshot.sessions.map(\.session.createdAtMs).filter { $0 > 0 }
        self.init(oldestCreatedAtMs: snapshot.sessions.count >= Self.listCap ? created.min() ?? 0 : 0)
    }

    public func covers(createdAtMs: Int) -> Bool {
        oldestCreatedAtMs <= 0 || createdAtMs >= oldestCreatedAtMs
    }
}

public func desktopAgents(_ rows: [UnifiedAgentRow], shown: [(repo: String, id: Int)], idle: Int = 20)
    -> [UnifiedAgentRow]
{
    let known = Set(shown.map { "\($0.repo.lowercased())#\($0.id)" })
    let listed = rows.filter { row in
        row.isDesktop && (row.outside || !known.contains("\(row.repoName.lowercased())#\(row.localId)"))
    }
    let recent = listed.filter { !$0.isActive }.sorted { $0.updatedAtMs > $1.updatedAtMs }.prefix(max(0, idle))
    return listed.filter(\.isActive) + recent
}

public func liveDesktopRow(_ rows: [UnifiedAgentRow], repoName: String, sessionId: Int) -> UnifiedAgentRow? {
    let repo = repoName.lowercased()
    return rows.first { $0.isDesktop && !$0.outside && $0.localId == sessionId && $0.repoName.lowercased() == repo }
}

extension AgentControlling {
    public func desktopAttachments(repoName: String, sessionId: Int) async throws -> [AgentAttachmentRef]? {
        guard let row = liveDesktopRow(rows, repoName: repoName, sessionId: sessionId) else { return nil }
        return try await transcript(row, since: 0).attachments
    }
}

/// What a desktop answers for an outside agent whose transcript it cannot read: an older desktop, a CLI
/// whose transcript it does not know, or an agent that has exited.
let outsideTranscriptMissing: Set<String> = ["unknown_session", "no_transcript", "unsupported"]

/// What is new in a run's transcript since the phone last read it.
public struct AgentTranscriptDelta: Equatable, Sendable {
    /// The new text (or the whole tail when `reset`).
    public var text: String
    /// Where the next read continues.
    public var next: Int
    /// The log was rewritten or trimmed: replace what is shown instead of appending.
    public var reset: Bool
    public var active: Bool
    public var status: String
    public var attachments: [AgentAttachmentRef]?
    public var from: Int
    public var queue: AgentRunQueue?

    public init(
        text: String, next: Int, reset: Bool, active: Bool, status: String, attachments: [AgentAttachmentRef]? = nil,
        from: Int = 0, queue: AgentRunQueue? = nil
    ) {
        self.text = text
        self.next = next
        self.reset = reset
        self.active = active
        self.status = status
        self.attachments = attachments
        self.from = from
        self.queue = queue
    }

    /// The desktop's answer to `agents.transcript` with `since`.
    public static func fromDesktop(_ json: JSONObject, since: Int, sessionId: Int = 0) -> AgentTranscriptDelta {
        let from = json.int("from")
        let listed: [AgentAttachmentRef]? = json["attachments"] == nil
            ? nil : LocalAgentTranscript.fromJSON(json, sessionId: sessionId).attachments
        // An older desktop ignores `since` and returns the whole tail.
        let understood = json["next"] != nil
        return AgentTranscriptDelta(
            text: json.str("transcript"), next: understood ? json.int("next") : 0,
            reset: !understood || json.bool("reset") || (since > 0 && from < since && from == 0),
            active: json.bool("active"), status: json.str("status"), attachments: listed, from: from)
    }
}

/// Follows one run's transcript: appends deltas, and keeps only the newest `limit` characters.
public struct TranscriptFollower: Equatable, Sendable {
    public private(set) var text = ""
    public private(set) var next = 0
    public private(set) var attachments: [AgentAttachmentRef] = []
    public let limit: Int
    private var listed: [AgentAttachmentRef]?

    public init(limit: Int = 200_000) {
        self.limit = limit
    }

    public mutating func apply(_ delta: AgentTranscriptDelta) {
        if delta.reset { text = delta.text } else { text += delta.text }
        next = delta.next
        if text.utf16.count > limit {
            text = String(text.suffix(limit))
        }
        if let delta = delta.attachments { listed = delta }
        attachments = listed ?? AgentTranscriptAttachments.references(in: text)
    }
}

/// The relay calls node runs need; ApiService provides them.
@MainActor
public protocol CloudAgentsApi: AnyObject {
    func accountAgentSessions() async throws -> AgentSessionList
    func agentLaunchSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> AgentLaunchSession
    func stopAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> AgentStopResult
    func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
    func steerAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult
    func retryAgentSession(
        _ owner: String, _ repo: String, _ session: AgentLaunchSession, followUp: String,
        attachments: [PromptAttachment]
    ) async throws -> AgentRetryResult
    func agentLiveStream(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> HTTPStreamedResponse
}

extension CloudAgentsApi {
    public func agentLiveStream(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> HTTPStreamedResponse
    {
        AgentLiveStream.unavailable
    }
}

extension ApiService: CloudAgentsApi {}

/// The agent controls the phone offers, and the watch app reaches through the phone.
@MainActor
public protocol AgentControlling: AnyObject {
    func refresh() async
    var rows: [UnifiedAgentRow] { get }
    func stop(_ row: UnifiedAgentRow) async throws
    func prompt(_ row: UnifiedAgentRow, _ text: String, images: [PromptAttachment]) async throws
    var promptNotice: String { get }
    func continueRun(_ row: UnifiedAgentRow) async throws
    /// What is new since `since` (0 for everything the transport keeps).
    func transcript(_ row: UnifiedAgentRow, since: Int) async throws -> AgentTranscriptDelta
    /// Starts an agent on the desktop in reach; returns its local id.
    func startOnDesktop(
        owner: String, name: String, task: String, provider: String, images: [PromptAttachment]
    ) async throws -> Int
}

/// The unified list and its controls.
@MainActor
@Observable
public final class AgentControlCenter: AgentControlling {
    @ObservationIgnored public let hub: DesktopControlHub
    @ObservationIgnored private let cloud: CloudAgentsApi?

    public private(set) var rows: [UnifiedAgentRow] = []
    public private(set) var desktopSnapshot: LocalAgentsSnapshot?
    public private(set) var loading = false
    /// Why the desktop's half is missing, when it is ("" when it loaded or no desktop is paired).
    public private(set) var desktopNote = ""
    public private(set) var cloudNote = ""
    public private(set) var promptNotice = ""
    public private(set) var otherDesktops: [OtherDesktopAgents] = []

    public nonisolated static let otherDesktopLimit = 4

    public var otherDesktopNotes: [String] {
        otherDesktops.filter { !$0.note.isEmpty }.map { "\($0.name): \($0.note)" }
    }

    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var refreshing = false
    @ObservationIgnored private var wantDesktop = false
    @ObservationIgnored private var wantCloud = false
    @ObservationIgnored private var cloudSessions: [AgentLaunchSession] = []
    @ObservationIgnored private var epoch = 0
    @ObservationIgnored private var cloudReads: [String: CloudRead] = [:]
    @ObservationIgnored private var cloudFull: [String: AgentLaunchSession] = [:]
    @ObservationIgnored private var cloudLive: [String: AgentLiveText] = [:]
    @ObservationIgnored private var deskRows: [UnifiedAgentRow] = []
    @ObservationIgnored private var nodeRows: [UnifiedAgentRow] = []
    @ObservationIgnored private var otherTaps: [String: UUID] = [:]
    @ObservationIgnored private var othersLoading = false
    @ObservationIgnored private var othersAgain = false

    public nonisolated static let cloudReadReuse: TimeInterval = 30

    public var desktopFolders: [LocalAgentFolder] { desktopSnapshot?.folders ?? [] }

    public var desktopCover: DesktopListCover? {
        guard let desktopSnapshot, desktopNote.isEmpty, hub.candidates.count == 1 else { return nil }
        return DesktopListCover(desktopSnapshot)
    }

    private struct CloudRead {
        let mark: String
        let at: TimeInterval
        let task: Task<AgentTranscriptDelta, Error>
    }

    public init(hub: DesktopControlHub, cloud: CloudAgentsApi?) {
        self.hub = hub
        self.cloud = cloud
        tapId = hub.addEventTap { [weak self] name, _ in
            // The desktop rings on every change; the relay's own "agents"/"cloud-agents" topics
            // are wired by whoever holds the account event channel (see `cloudChanged`).
            if name == "agents.changed" { Task { await self?.refreshDesktop() } }
        }
    }

    /// The account's "cloud-agents" topic rang.
    public func cloudChanged() {
        Task { await refresh() }
    }

    public func refresh() async {
        await reload(cloud: true)
    }

    public func refreshDesktop() async {
        await reload(cloud: false)
    }

    public func row(forRoute id: String) -> UnifiedAgentRow? {
        if let row = UnifiedAgentRow.forRoute(id, in: rows) { return row }
        guard id.hasPrefix("node:") else { return nil }
        let session = String(id.dropFirst("node:".count))
        guard let claimed = cloudSessions.first(where: { $0.sessionId == session }).map(UnifiedAgentRow.fromCloud),
              claimed.localId > 0
        else { return nil }
        let node = claimed.node.lowercased()
        let desktop = (hub.currentLink?.desktopName ?? "").lowercased()
        return rows.first { row in
            row.isDesktop && !row.outside && row.localId == claimed.localId
                && (row.node.lowercased() == node || (!row.elsewhere && !desktop.isEmpty && desktop == node))
        }
    }

    public func resetForProjectSwitch() {
        epoch &+= 1
        rows = []
        desktopSnapshot = nil
        desktopNote = ""
        cloudNote = ""
        promptNotice = ""
        otherDesktops = []
        deskRows = []
        nodeRows = []
        for id in otherTaps.values { hub.removeEventTap(id) }
        otherTaps = [:]
        othersLoading = false
        othersAgain = false
        cloudSessions = []
        cloudReads = [:]
        cloudFull = [:]
        cloudLive = [:]
        wantDesktop = false
        wantCloud = false
        refreshing = false
        loading = false
    }

    private func reload(cloud: Bool) async {
        wantDesktop = true
        if cloud { wantCloud = true }
        if refreshing { return }
        let started = epoch
        refreshing = true
        loading = true
        defer {
            if started == epoch {
                refreshing = false
                loading = false
            }
        }
        while started == epoch && (wantDesktop || wantCloud) {
            let withCloud = wantCloud
            wantDesktop = false
            wantCloud = false
            let desk: [UnifiedAgentRow]
            let nodes: [UnifiedAgentRow]
            if withCloud {
                let cloudRows = Task { await self.loadCloud(started) }
                desk = await loadDesktop(started)
                if started != epoch {
                    cloudRows.cancel()
                    return
                }
                publish(desk, cloudSessions.map(UnifiedAgentRow.fromCloud))
                nodes = await cloudRows.value
            } else {
                desk = await loadDesktop(started)
                nodes = cloudSessions.map(UnifiedAgentRow.fromCloud)
            }
            if started != epoch { return }
            publish(desk, nodes)
            if withCloud { await refreshOtherDesktops(started) }
        }
    }

    private func refreshOtherDesktops(_ started: Int) async {
        if othersLoading {
            othersAgain = true
            return
        }
        othersLoading = true
        defer { if started == epoch { othersLoading = false } }
        repeat {
            othersAgain = false
            await loadOtherDesktops(started)
        } while othersAgain && started == epoch
    }

    private func listenToOtherDesktops(_ keys: [String]) {
        for (key, id) in otherTaps where !keys.contains(key) {
            hub.removeEventTap(id)
            otherTaps[key] = nil
        }
        for key in keys where otherTaps[key] == nil {
            otherTaps[key] = hub.addEventTap({ [weak self] name, _ in
                guard let self, name == "agents.changed", key != self.hub.activeKey else { return }
                let started = self.epoch
                Task { await self.refreshOtherDesktops(started) }
            }, for: key)
        }
    }

    private func loadOtherDesktops(_ started: Int) async {
        let targets = Array(hub.otherDesktops.prefix(Self.otherDesktopLimit))
        listenToOtherDesktops(targets.map(\.key))
        let hub = self.hub
        let loads = targets.map { (target: PairedDesktop) in
            Task { @MainActor () -> OtherDesktopAgents? in
                do {
                    let link = try await hub.link(to: target.key)
                    let listed = try await link.request("agents.list", [:], timeout: 15)
                    let name = link.desktopName.isEmpty ? target.name : link.desktopName
                    return OtherDesktopAgents.listed(
                        LocalAgentsSnapshot.fromJSON(listed), key: target.key, name: name, transport: link.transport)
                } catch {
                    if ControlErrorText.isCancellation(error) { return nil }
                    return OtherDesktopAgents(key: target.key, name: target.name, rows: [], note: "Not reachable")
                }
            }
        }
        var out: [OtherDesktopAgents] = []
        for (target, load) in zip(targets, loads) {
            if let loaded = await load.value {
                out.append(loaded)
            } else if let kept = otherDesktops.first(where: { $0.key == target.key }) {
                out.append(kept)
            }
        }
        if started != epoch { return }
        let active = hub.activeKey
        out.removeAll { $0.key == active }
        if out != otherDesktops { otherDesktops = out }
        republish()
    }

    private func publish(_ desk: [UnifiedAgentRow], _ nodes: [UnifiedAgentRow]) {
        deskRows = desk
        nodeRows = nodes
        republish()
    }

    private func republish() {
        let active = hub.activeKey
        let desk = deskRows + otherDesktops.filter { $0.key != active }.flatMap(\.rows)
        let nodes = nodeRows
        let merged = PerformanceMonitorService.section("agents.mergeRows") {
            mergeAgentRows(desktop: desk, cloud: nodes, desktopName: hub.currentLink?.desktopName ?? "")
        }
        if merged != rows { rows = merged }
    }

    public func stream(
        _ row: UnifiedAgentRow, since: Int, onEnded: @escaping @MainActor (String) -> Void = { _ in },
        onDelta: @escaping @MainActor (AgentTranscriptDelta) -> Void
    ) -> DesktopTranscriptStream? {
        guard row.isDesktop, row.localId != 0 else { return nil }
        return DesktopTranscriptStream(
            hub: hub, sessionId: row.localId, since: since, desktopKey: row.desktopKey, onEnded: onEnded,
            onDelta: onDelta)
    }

    private func loadDesktop(_ started: Int) async -> [UnifiedAgentRow] {
        if hub.desktop == nil { await hub.refreshAccountDesktops() }
        if started != epoch { return [] }
        if hub.desktop == nil {
            if !desktopNote.isEmpty { desktopNote = "" }
            if desktopSnapshot != nil { desktopSnapshot = nil }
            return []
        }
        do {
            let link = try await hub.link()
            let listed = try await link.request("agents.list", [:])
            if started != epoch { return [] }
            let key = hub.activeKey
            return PerformanceMonitorService.section("agents.desktopList") {
                let snapshot = LocalAgentsSnapshot.fromJSON(listed)
                if snapshot != desktopSnapshot { desktopSnapshot = snapshot }
                if !desktopNote.isEmpty { desktopNote = "" }
                return (snapshot.sessions.map {
                    UnifiedAgentRow.fromDesktop($0, transport: link.transport, desktop: link.desktopName)
                } + snapshot.outside.map {
                    UnifiedAgentRow.fromOutside($0, transport: link.transport, desktop: link.desktopName)
                }).map { (row: UnifiedAgentRow) -> UnifiedAgentRow in
                    var tagged = row
                    tagged.desktopKey = key
                    return tagged
                }
            }
        } catch {
            if started != epoch { return [] }
            if ControlErrorText.isCancellation(error) { return rows.filter(\.isDesktop) }
            let note = ControlErrorText.describe(error)
            if note != desktopNote { desktopNote = note }
            return []
        }
    }

    private func loadCloud(_ started: Int) async -> [UnifiedAgentRow] {
        guard let cloud else { return [] }
        do {
            let listed = try await cloud.accountAgentSessions()
            if started != epoch { return [] }
            if !cloudNote.isEmpty { cloudNote = "" }
            cloudSessions = listed.keeping(cloudSessions)
        } catch let refusal as AgentLaunchException {
            if started != epoch { return [] }
            if refusal.message != cloudNote { cloudNote = refusal.message }
        } catch {
            if started != epoch { return [] }
            if !ControlErrorText.isCancellation(error) {
                let note = String(describing: error)
                if note != cloudNote { cloudNote = note }
            }
        }
        let nodes = cloudSessions.map(UnifiedAgentRow.fromCloud)
        if started == epoch { publish(deskRows, nodes) }
        return nodes
    }

    public func stop(_ row: UnifiedAgentRow) async throws {
        if row.outside {
            guard row.canStop else { throw LocalControlException("outside_stop_unsupported") }
            _ = try await hub.request("agents.stop", ["id": .int(Int64(row.localId))], to: row.desktopKey)
            await refresh()
            return
        }
        if row.isDesktop {
            _ = try await hub.request("agents.stop", ["id": .int(Int64(row.localId))], to: row.desktopKey)
        } else {
            guard let cloud else { throw LocalControlException("not_connected") }
            _ = try await cloud.stopAgentSession(row.repoOwner, row.repoName, row.cloudSessionId, orgScope: row.orgScope)
        }
        await refresh()
    }

    public func prompt(_ row: UnifiedAgentRow, _ text: String, images: [PromptAttachment] = []) async throws {
        try await prompt(row, text, images: images, model: "", mode: "")
    }

    public func prompt(
        _ row: UnifiedAgentRow, _ text: String, images: [PromptAttachment], model: String, mode: String,
        provider: String = ""
    ) async throws {
        promptNotice = ""
        if row.outside {
            guard row.canPrompt else { throw LocalControlException("outside_cli_unsupported") }
            if !images.isEmpty { throw LocalControlException("outside_images_unsupported") }
            let trimmed = text.dartTrimmed
            if trimmed.isEmpty { return }
            let answer = try await hub.request(
                "agents.prompt", ["id": .int(Int64(row.localId)), "text": .string(trimmed)], timeout: 30,
                to: row.desktopKey)
            if answer.str("status") == "held_for_approval" {
                promptNotice = ControlErrorText.describe(code: "held_for_approval")
            }
            await refresh()
            return
        }
        let trimmed = promptOrPhotoNote(text.dartTrimmed, images)
        if trimmed.isEmpty { return }
        if row.isDesktop {
            let link = try await hub.link(to: row.desktopKey)
            var asked: JSONObject = ["id": .int(Int64(row.localId)), "text": .string(trimmed)]
            if row.offersModelChoice, !model.isEmpty { asked["model"] = .string(model) }
            if row.offersModelChoice, !provider.isEmpty, provider != row.provider {
                asked["provider"] = .string(provider)
            }
            if row.offersModeChoice, !mode.isEmpty { asked["mode"] = .string(mode) }
            let params = try await DesktopPromptImages.params(asked, images: images, over: link)
            _ = try await link.request("agents.prompt", params, timeout: 30)
        } else {
            guard let cloud else { throw LocalControlException("not_connected") }
            if row.restartsOnPrompt, let session = retryableRun(row) {
                let outcome = try await cloud.retryAgentSession(
                    row.repoOwner, row.repoName, session, followUp: trimmed, attachments: images)
                promptNotice = outcome.label
                await refresh()
                return
            }
            _ = try await cloud.steerAgentSession(
                row.repoOwner, row.repoName, row.cloudSessionId, prompt: trimmed, orgScope: row.orgScope,
                attachments: images)
        }
        await refresh()
    }

    public func promptDesktop(_ localId: Int, _ text: String) async throws {
        _ = try await hub.request("agents.prompt", ["id": .int(Int64(localId)), "text": .string(text)], timeout: 30)
        await refresh()
    }

    public func questions(_ row: UnifiedAgentRow) async throws -> [AgentQuestion] {
        guard row.isDesktop, !row.outside, row.localId > 0 else { return [] }
        let answer = try await hub.request(
            "agents.questions", ["id": .int(Int64(row.localId))], to: row.desktopKey)
        return answer.maps("questions").compactMap(AgentQuestion.init)
    }

    public func answer(_ row: UnifiedAgentRow, question: AgentQuestion, text: String) async throws {
        guard row.isDesktop, !row.outside, row.localId > 0 else { throw LocalControlException("unsupported") }
        let answer = text.dartTrimmed
        guard !answer.isEmpty else { return }
        _ = try await hub.request(
            "agents.answer",
            ["id": .int(Int64(row.localId)), "question": .string(question.id), "answer": .string(answer)],
            timeout: 30, to: row.desktopKey)
        await refresh()
    }

    public func continueRun(_ row: UnifiedAgentRow) async throws {
        if !row.isDesktop && !row.outside {
            guard let cloud, row.localId > 0, !row.cloudSessionId.isEmpty else { throw LocalControlException("unsupported") }
            _ = try await cloud.steerAgentSession(
                row.repoOwner, row.repoName, row.cloudSessionId, prompt: RunnerAgentText.continuePrompt,
                orgScope: row.orgScope, attachments: [])
            await refresh()
            return
        }
        guard row.isDesktop, !row.outside else { throw LocalControlException("unsupported") }
        _ = try await hub.request("agents.continue", ["id": .int(Int64(row.localId))], to: row.desktopKey)
        await refresh()
    }

    public func retryableRun(_ row: UnifiedAgentRow) -> AgentLaunchSession? {
        guard !row.isDesktop, !row.outside, !row.cloudSessionId.isEmpty else { return nil }
        return cloudSessions.first { $0.sessionId == row.cloudSessionId && $0.isRetryable }
    }

    public func continueOnDesktop(_ row: UnifiedAgentRow) async throws -> RunnerDesktopMove {
        guard let cloud, !row.isDesktop, !row.outside, !row.cloudSessionId.isEmpty else {
            throw LocalControlException("unsupported")
        }
        if hub.desktop == nil { await hub.refreshAccountDesktops() }
        guard hub.desktop != nil else { throw LocalControlException("not_connected") }
        let repo = row.repoLabel
        var session = try await cloud.agentLaunchSession(
            row.repoOwner, row.repoName, row.cloudSessionId, orgScope: row.orgScope)
        do {
            let checked = try await hub.request(
                RunnerDesktopHandoff.capability, RunnerDesktopHandoff.params(session, row: row, check: true), timeout: 30)
            if checked.bool("already") {
                let kept = await dropRunnerCopy(row, cloud)
                await refresh()
                return move(checked, runnerKept: kept)
            }
        } catch {
            throw RunnerDesktopMoveError(RunnerDesktopHandoff.failureText(error, repo: repo))
        }
        var stopped = ""
        if session.isActive {
            do {
                _ = try await cloud.stopAgentSession(
                    row.repoOwner, row.repoName, row.cloudSessionId, orgScope: row.orgScope)
            } catch {
                throw RunnerDesktopMoveError(
                    "Could not stop the run on \(RunnerDesktopHandoff.runnerName(session)), so it was not moved: "
                        + String(describing: error))
            }
            stopped = RunnerDesktopHandoff.runnerName(session)
            if let read = try? await cloud.agentLaunchSession(
                row.repoOwner, row.repoName, row.cloudSessionId, orgScope: row.orgScope)
            {
                session = read
            }
        }
        do {
            let answer = try await hub.request(
                RunnerDesktopHandoff.capability, RunnerDesktopHandoff.params(session, row: row, check: false),
                timeout: 90)
            let kept = await dropRunnerCopy(row, cloud)
            await refresh()
            return move(answer, runnerKept: kept)
        } catch {
            await refresh()
            throw RunnerDesktopMoveError(RunnerDesktopHandoff.failureText(error, repo: repo, stoppedOn: stopped))
        }
    }

    private func dropRunnerCopy(_ row: UnifiedAgentRow, _ cloud: CloudAgentsApi) async -> Bool {
        do {
            try await cloud.cancelAgentSession(row.repoOwner, row.repoName, row.cloudSessionId, orgScope: row.orgScope)
        } catch let refusal as AgentLaunchException where refusal.status == 404 {
        } catch {
            return true
        }
        cloudSessions.removeAll { $0.sessionId == row.cloudSessionId }
        return false
    }

    private func move(_ answer: JSONObject, runnerKept: Bool) -> RunnerDesktopMove {
        let linked = hub.currentLink?.desktopName ?? ""
        let paired = hub.desktop?.name ?? ""
        let name = !linked.isEmpty ? linked : (!paired.isEmpty ? paired : "your desktop")
        return RunnerDesktopMove(
            agentId: answer.int("id"), desktopName: name, branch: answer.str("branch"), fresh: answer.bool("fresh"),
            already: answer.bool("already"), runnerKept: runnerKept)
    }

    public func transcript(_ row: UnifiedAgentRow, since: Int) async throws -> AgentTranscriptDelta {
        if row.outside {
            var params: JSONObject = ["id": .int(Int64(row.localId))]
            if since > 0 { params["since"] = .int(Int64(since)) }
            do {
                return AgentTranscriptDelta.fromDesktop(
                    try await hub.request("agents.transcript", params, to: row.desktopKey), since: since,
                    sessionId: row.localId)
            } catch let refusal as LocalControlException where outsideTranscriptMissing.contains(refusal.code) {
                if since > 0 && refusal.code == "unknown_session" {
                    return AgentTranscriptDelta(text: "", next: since, reset: false, active: false, status: "done")
                }
                let note = row.outsideNote
                return AgentTranscriptDelta(
                    text: note, next: note.utf16.count, reset: true, active: false, status: row.status)
            }
        }
        if row.isDesktop {
            var params: JSONObject = ["id": .int(Int64(row.localId))]
            if since > 0 { params["since"] = .int(Int64(since)) }
            return AgentTranscriptDelta.fromDesktop(
                try await hub.request("agents.transcript", params, to: row.desktopKey), since: since,
                sessionId: row.localId)
        }
        guard let cloud else { throw LocalControlException("not_connected") }
        let session = try await cloud.agentLaunchSession(
            row.repoOwner, row.repoName, row.cloudSessionId, orgScope: row.orgScope)
        cloudFull[row.id] = session.isActive ? session : nil
        return cloudDelta(row, session)
    }

    public func followCloudLive(
        _ row: UnifiedAgentRow, onDelta: @escaping @MainActor (AgentTranscriptDelta) -> Void
    ) async -> AgentLiveOutcome {
        guard let cloud, !row.isDesktop, !row.outside, !row.cloudSessionId.isEmpty else { return .refused(0) }
        let key = row.id
        let stream = AgentLiveStream(open: {
            try await cloud.agentLiveStream(row.repoOwner, row.repoName, row.cloudSessionId, orgScope: row.orgScope)
        })
        let outcome = await stream.run { live in
            cloudLive[key] = live
            if let session = cloudFull[key] { onDelta(cloudDelta(row, session)) }
        }
        cloudLive[key] = nil
        return outcome
    }

    private func cloudDelta(_ row: UnifiedAgentRow, _ read: AgentLaunchSession) -> AgentTranscriptDelta {
        var session = read
        if let live = cloudLive[row.id], session.isActive, live.atMs >= session.liveAtMs {
            session.liveText = live.text
            session.liveAtMs = live.atMs
        }
        let text = UnifiedAgentRow.cloudTranscript(session)
        let attachments: [AgentAttachmentRef] = session.history.flatMap {
            session.attachmentRefs($0, owner: row.repoOwner, repo: row.repoName)
        }
        return AgentTranscriptDelta(
            text: text, next: text.utf16.count, reset: true, active: session.isActive, status: session.status,
            attachments: attachments, queue: session.queue)
    }

    public func watchTranscript(_ row: UnifiedAgentRow, now: TimeInterval = ProcessInfo.processInfo.systemUptime)
        async throws -> AgentTranscriptDelta
    {
        if row.isDesktop || row.outside { return try await transcript(row, since: 0) }
        let listed = rows.first { $0.id == row.id } ?? row
        let mark = "\(listed.status)|\(listed.isActive)|\(listed.updatedAtMs)"
        if let read = cloudReads[row.id], read.mark == mark, now - read.at < Self.cloudReadReuse {
            return try await read.task.value
        }
        cloudReads = cloudReads.filter { now - $0.value.at < Self.cloudReadReuse }
        let task = Task { try await self.transcript(row, since: 0) }
        cloudReads[row.id] = CloudRead(mark: mark, at: now, task: task)
        do {
            return try await task.value
        } catch {
            if cloudReads[row.id]?.task == task { cloudReads[row.id] = nil }
            throw error
        }
    }

    public var queue: LocalAgentQueue? { desktopNote.isEmpty ? desktopSnapshot?.queue : nil }

    public var desktopDefaultMode: String { desktopSnapshot?.defaultMode ?? "" }

    public var supportsQueue: Bool { queue != nil }

    public func queuePosition(_ row: UnifiedAgentRow) -> Int? {
        guard row.isDesktop, !row.outside, !row.elsewhere, row.localId > 0 else { return nil }
        return queue?.position(of: row.localId)
    }

    public var queuedRows: [UnifiedAgentRow] {
        guard let queue else { return [] }
        var byId: [Int: UnifiedAgentRow] = [:]
        for row in rows where row.isDesktop && !row.outside && !row.elsewhere && row.localId > 0 {
            if byId[row.localId] == nil { byId[row.localId] = row }
        }
        return queue.order.compactMap { byId[$0] }
    }

    public func setQueueLimit(_ limit: Int) async throws {
        let answer = try await hub.request("agents.queue.limit", ["limit": .int(Int64(limit))])
        applyQueue(answer)
        await refreshDesktop()
    }

    public func moveQueued(_ row: UnifiedAgentRow, to index: Int) async throws {
        guard row.isDesktop, !row.outside, row.localId > 0 else { throw LocalControlException("not_queued") }
        try await moveQueued(id: row.localId, to: index)
    }

    public func moveQueued(id: Int, to index: Int) async throws {
        let target = max(0, index)
        if let current = desktopSnapshot?.queue { applyQueue(current.moved(id, to: target)) }
        let answer: JSONObject
        do {
            answer = try await hub.request("agents.queue.move", ["id": .int(Int64(id)), "to": .int(Int64(target))])
        } catch {
            await refreshDesktop()
            throw error
        }
        applyQueue(answer)
        await refreshDesktop()
    }

    private func applyQueue(_ answer: JSONObject) {
        guard let queue = LocalAgentQueue.fromJSON(.object(answer)) else { return }
        applyQueue(queue)
    }

    private func applyQueue(_ queue: LocalAgentQueue) {
        guard let snapshot = desktopSnapshot else { return }
        desktopSnapshot = snapshot.with(queue: queue)
    }

    public func markRead(id: Int) async {
        guard id > 0, hub.desktop != nil else { return }
        _ = try? await hub.request("agents.markRead", ["id": .int(Int64(id))])
    }

    public func startOnDesktop(
        owner: String, name: String, task: String, provider: String = "", images: [PromptAttachment] = []
    ) async throws -> Int {
        var params: JSONObject = [
            "owner": .string(owner), "name": .string(name), "task": .string(promptOrPhotoNote(task, images)),
        ]
        if !provider.isEmpty { params["provider"] = .string(provider) }
        let link = try await hub.link()
        params = try await DesktopPromptImages.params(params, images: images, over: link)
        let result = try await link.request("agents.start", params, timeout: 30)
        await refresh()
        return result.int("id")
    }
}
