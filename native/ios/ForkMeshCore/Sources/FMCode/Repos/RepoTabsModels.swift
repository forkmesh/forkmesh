// The remaining repository tabs: Mirrors with its sync status card, Releases,
// Worktrees, Actions with its run detail screen, and Agents with its session
// detail screen. Each reads once when it opens and again on Refresh; nothing
// polls.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import Foundation
import Observation

/// `_MirrorsTab`.
@MainActor
@Observable
public final class RepoMirrorsTabModel {
    public let repo: Repository
    public let mirrorSet = RepoFuture<RepoMirrorSet>()
    @ObservationIgnored private let client: RepoClient

    public init(client: RepoClient, repo: Repository) {
        self.client = client
        self.repo = repo
    }

    public func start() async {
        let owner = repo.owner
        let name = repo.name
        await mirrorSet.start { try await client.mirrorSet(owner, name) }
    }

    public var mirrors: [RepoMirror] { mirrorSet.data?.mirrors ?? [] }
    public var syncCard: RepoSyncStatusCard? { mirrorSet.data?.syncStatus.map(RepoSyncStatusCard.init(status:)) }

    public static let emptyMessage = "No mirrors reported for this repository yet."

    /// A mirror row's icon, subtitle and badge.
    public static func mirrorIcon(_ mirror: RepoMirror) -> String { mirror.online ? "cloud_done_outlined" : "cloud_off_outlined" }
    public static func mirrorSubtitle(_ mirror: RepoMirror) -> String { mirror.online ? "Online mirror" : "Offline or recently unseen" }
    public static func mirrorBadge(_ mirror: RepoMirror) -> String { mirror.online ? "live" : "offline" }
}

/// `_SyncStatusCard`: the repository's standing in the shared
/// forkmesh-sync-status-v1 model.
public struct RepoSyncStatusCard: Equatable, Sendable {
    public var severity: String
    public var label: String
    public var summary: String

    public init(status: JSONObject) {
        severity = status["severity"]?.stringValue ?? ""
        label = (status["label"].flatMap { $0.isNull ? nil : $0 })?.text ?? ""
        summary = (status["summary"].flatMap { $0.isNull ? nil : $0 })?.text ?? ""
    }

    public var icon: String {
        switch severity {
        case "ok": return "verified_outlined"
        case "error": return "error_outline"
        case "warning": return "warning_amber_outlined"
        default: return "sync"
        }
    }

    public var tone: RepoBadgeTone {
        switch severity {
        case "ok": return .success
        case "error": return .danger
        case "warning": return .warning
        default: return .accent
        }
    }

    public var badge: String { label.lowercased() }
}

/// One desktop request button of a release card.
public struct RepoReleaseAction: Equatable, Sendable, Identifiable {
    public var command: String
    public var label: String
    public var icon: String
    /// FilledButton rather than OutlinedButton.
    public var filled: Bool
    public var successMessage: String

    public var id: String { command }

    /// The four buttons, in order.
    public static let all: [RepoReleaseAction] = [
        RepoReleaseAction(
            command: "release.draft", label: "Draft release", icon: "edit_note_outlined", filled: false,
            successMessage: "Draft release request queued. Approve it in the ForkMesh desktop app."),
        RepoReleaseAction(
            command: "release.notes", label: "Generate notes", icon: "notes_outlined", filled: false,
            successMessage: "Generate notes request queued. Approve it in the ForkMesh desktop app."),
        RepoReleaseAction(
            command: "release.create", label: "Create release", icon: "new_releases_outlined", filled: true,
            successMessage: "Create release request queued. Approve it in the ForkMesh desktop app."),
        RepoReleaseAction(
            command: "release.publish", label: "Publish artifacts", icon: "cloud_upload_outlined", filled: true,
            successMessage: "Publish artifacts request queued. Approve it in the ForkMesh desktop app."),
    ]
}

/// `_ReleasesTab`.
@MainActor
@Observable
public final class RepoReleasesTabModel {
    public let repo: Repository
    public let releases = RepoFuture<[RepoRelease]>()
    @ObservationIgnored private let client: RepoClient
    @ObservationIgnored private let signer: RepoSigner

    public init(client: RepoClient, repo: Repository, signer: RepoSigner) {
        self.client = client
        self.repo = repo
        self.signer = signer
    }

    public func start() async {
        let owner = repo.owner
        let name = repo.name
        await releases.start { try await client.releases(owner, name) }
    }

    public static let emptyMessage = "No releases yet. Draft and publish from a paired desktop node."
    public static let noArtifacts = "No artifacts are attached to this release yet."

    /// `_requestRelease`: answers the inline notice text.
    public func request(_ action: RepoReleaseAction, for release: RepoRelease) async -> String {
        await RepoDesktopCommands.request(
            client: client, signer: signer, repo: repo, command: action.command, target: "release:\(release.tag)",
            payload: RepoDesktopCommands.releasePayload(release), successMessage: action.successMessage)
    }

    /// `_ReleaseAssetRow`'s title.
    public static func assetTitle(_ asset: RepoReleaseAsset) -> String { asset.name.isEmpty ? asset.shortSha : asset.name }
}

/// `_WorktreesTab`: the branches the desktop checked out as worktrees.
@MainActor
@Observable
public final class RepoWorktreesTabModel {
    public let repo: Repository
    public let worktrees = RepoFuture<[RepoBranch]>()
    @ObservationIgnored private let client: RepoClient

    public init(client: RepoClient, repo: Repository) {
        self.client = client
        self.repo = repo
    }

    public func start() async {
        let owner = repo.owner
        let name = repo.name
        await worktrees.start { try await client.branches(owner, name).filter(\.hasWorktree) }
    }

    public static let emptyMessage = "No worktrees found for this repository."

    /// The row chips: "default" and the short sha.
    public static func chips(_ branch: RepoBranch) -> [String] {
        var chips: [String] = []
        if branch.isDefault { chips.append("default") }
        if !branch.sha.isEmpty { chips.append(RepoDisplay.shortHash(branch.sha)) }
        return chips
    }
}

/// `_AgentsTabState`.
@MainActor
@Observable
public final class RepoAgentsTabModel {
    public let repo: Repository
    public let sessions = RepoFuture<[AgentSession]>()
    @ObservationIgnored private let client: RepoClient
    @ObservationIgnored public let signer: RepoSigner

    public init(client: RepoClient, repo: Repository, signer: RepoSigner) {
        self.client = client
        self.repo = repo
        self.signer = signer
    }

    public var ownerAccount: String { signer.ownerAccount(for: repo) }

    public func start() async {
        let request = (repo.owner, repo.name, ownerAccount)
        await sessions.start { try await client.agentSessions(request.0, request.1, ownerAccount: request.2) }
    }

    public func refresh() async {
        let request = (repo.owner, repo.name, ownerAccount)
        await sessions.load { try await client.agentSessions(request.0, request.1, ownerAccount: request.2) }
    }

    public var items: [AgentSession] { sessions.data ?? [] }
    public var running: Bool { items.contains { $0.isActive } }
    public var countLabel: String? { sessions.data.map { "\($0.count)" } }
    public var showsLoading: Bool { sessions.loading && items.isEmpty }

    public static let emptyMessage = "No agent sessions have been pushed by the desktop node for this repo yet."

    public static func subtitle(_ session: AgentSession) -> String {
        var parts: [String] = []
        if session.issueNumber > 0 { parts.append("Issue #\(session.issueNumber)") }
        parts.append(session.providerLabel)
        parts.append(session.latestStatusSummary(80))
        return parts.filter { !$0.isEmpty }.joined(separator: " • ")
    }

    public static func chips(_ session: AgentSession, now: Date = Date()) -> [String] {
        var chips: [String] = []
        if session.issueNumber > 0 { chips.append("#\(session.issueNumber)") }
        chips.append(session.providerLabel)
        chips.append(session.latestStatusSummary(80))
        let countdown = session.rateLimitCountdownLabel(now: now)
        if !countdown.isEmpty { chips.append(countdown) }
        if !session.branchName.isEmpty { chips.append(session.branchName) }
        if session.numTurns > 0 { chips.append("\(session.numTurns) turns") }
        if !session.durationLabel.isEmpty { chips.append(session.durationLabel) }
        if !session.costLabel.isEmpty { chips.append(session.costLabel) }
        if !session.prLabel.isEmpty { chips.append(session.prLabel) }
        if !session.diffLabel.isEmpty { chips.append(session.diffLabel) }
        if session.conflicted { chips.append("Conflict") }
        if !session.tokenLabel.isEmpty { chips.append(session.tokenLabel) }
        return chips
    }
}

/// `_AgentSessionDetailScreen`.
@MainActor
@Observable
public final class RepoAgentSessionModel {
    public let repo: Repository
    public let session: AgentSession
    public let ownerAccount: String
    public let transcript = RepoFuture<AgentTranscript>()
    public private(set) var desktopAttachments: [AgentAttachmentRef] = []
    @ObservationIgnored private let client: RepoClient
    @ObservationIgnored private let desktopFetch: (@MainActor () async throws -> [AgentAttachmentRef]?)?

    public init(
        client: RepoClient, repo: Repository, session: AgentSession, ownerAccount: String,
        desktopAttachments: (@MainActor () async throws -> [AgentAttachmentRef]?)? = nil
    ) {
        self.client = client
        self.repo = repo
        self.session = session
        self.ownerAccount = ownerAccount
        self.desktopFetch = desktopAttachments
    }

    public var title: String { "Agent session #\(session.id)" }

    public func start() async {
        let request = (repo.owner, repo.name, session.id, ownerAccount)
        await transcript.start { try await client.agentTranscript(request.0, request.1, request.2, ownerAccount: request.3) }
        if let desktopFetch, let listed = try? await desktopFetch(), !listed.isEmpty {
            desktopAttachments = listed
        }
    }

    public var attachments: [AgentAttachmentRef] {
        if !desktopAttachments.isEmpty { return desktopAttachments }
        guard let detail = transcript.data, !detail.sealedForOtherDevice else { return [] }
        return AgentTranscriptAttachments.references(in: detail.transcript)
    }

    /// The session chips: (icon, label).
    public func chips(now: Date = Date()) -> [(icon: String, label: String)] {
        var chips: [(icon: String, label: String)] = [
            ("smart_toy_outlined", session.providerLabel),
            ("info_outline", session.latestStatusSummary(120)),
        ]
        let countdown = session.rateLimitCountdownLabel(now: now)
        if !countdown.isEmpty { chips.append(("timer_outlined", countdown)) }
        if !session.model.isEmpty { chips.append(("memory_outlined", session.model)) }
        if session.issueNumber > 0 { chips.append(("error_outline", "#\(session.issueNumber)")) }
        if !session.branchName.isEmpty { chips.append(("account_tree_outlined", session.branchName)) }
        if session.numTurns > 0 { chips.append(("chat_bubble_outline", "\(session.numTurns) turns")) }
        if !session.durationLabel.isEmpty { chips.append(("timer_outlined", session.durationLabel)) }
        if !session.costLabel.isEmpty { chips.append(("paid_outlined", session.costLabel)) }
        if !session.prLabel.isEmpty { chips.append(("call_merge_outlined", session.prLabel)) }
        if !session.diffLabel.isEmpty { chips.append(("difference_outlined", session.diffLabel)) }
        if session.conflicted { chips.append(("warning_amber_outlined", "Conflict")) }
        if !session.tokenLabel.isEmpty { chips.append(("data_usage_outlined", session.tokenLabel)) }
        return chips
    }

    public var failureReason: String { session.statusFailureReason(400) }

    public static let explainer =
        "Desktop-controlled session. Mobile can watch progress and open results; start, stop, retry, resume, merge, and apply stay on the desktop node until signed pairing is designed."

    /// The transcript card's status: the transcript's own, else the session's.
    public var transcriptStatus: String {
        let detail = transcript.data ?? AgentTranscript(status: "", transcript: "")
        return detail.status.isEmpty ? session.statusLabel : AgentSession(id: session.id, status: detail.status).statusLabel
    }

    public var transcriptText: String {
        let text = (transcript.data?.transcript ?? "").dartTrimmed
        return text.isEmpty ? "No transcript has been pushed by the desktop node yet." : text
    }
}
