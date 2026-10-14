// The Agents tab's session screens and cards: AgentSessionDetailScreen (a desktop-pushed session and its transcript),
// LaunchedAgentSessionScreen (a queued run: history, steering, retry, drop),
// AgentRetryCard and the chips.

import FMAgents
import FMApi
import FMBase
import FMCode
import FMControl
import SwiftUI

/// Maps FMAgents' tone to the palette.
@MainActor
func agentToneColor(_ tone: AgentBadgeTone, _ fm: FmPalette) -> Color {
    switch tone {
    case .accent: return fm.accent
    case .success: return fm.success
    case .danger: return fm.danger
    case .secondary: return fm.textSecondary
    case .tertiary: return fm.textTertiary
    }
}

/// A status badge coloured by what its label says.
struct AgentChip: View {
    @Environment(\.fmPalette) private var fm
    let label: String

    var body: some View {
        FmStatusBadge(label: label, color: agentToneColor(AgentsLogic.chipTone(label), fm))
    }
}

/// A desktop session's status.
struct AgentStatusBadge: View {
    @Environment(\.fmPalette) private var fm
    let session: AgentSession

    var body: some View {
        FmStatusBadge(label: session.statusLabel, color: agentToneColor(AgentsLogic.statusTone(session.statusLabel), fm))
    }
}

/// The agent's branch, as a badge that opens what it changes.
struct AgentBranchChip: View {
    @Environment(\.fmPalette) private var fm
    let repo: Repository
    let branch: String
    let base: String

    var body: some View {
        NavigationLink {
            BranchDiffScreen(repo: repo, base: base, head: branch)
                .shellPageClearance()
        } label: {
            HStack(spacing: FmSpace.x1) {
                FmIcon("account_tree_outlined", size: 12, color: fm.accent)
                Text(branch)
                    .fmText(.labelSmall, color: fm.accent)
                    .lineLimit(1)
                    .truncationMode(.tail)
                FmIcon("chevron_right", size: 12, color: fm.accent)
            }
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
            .background(fm.accent.fmWithAlpha(31))
            .clipShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text("View changes on \(branch)"))
    }
}

/// A wrapping row of chips.
struct AgentChipRow<Content: View>: View {
    private let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        AgentFlowLayout(spacing: FmSpace.x2) {
            content
        }
    }
}

/// Children left to right, onto a new line when full.
struct AgentFlowLayout: Layout {
    var spacing: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width: CGFloat = proposal.width ?? CGFloat.infinity
        var x: CGFloat = 0
        var y: CGFloat = 0
        var lineHeight: CGFloat = 0
        var widest: CGFloat = 0
        for subview in subviews {
            let size: CGSize = subview.sizeThatFits(.unspecified)
            if x > 0 && x + size.width > width {
                x = 0
                y += lineHeight + spacing
                lineHeight = 0
            }
            x += size.width + spacing
            widest = max(widest, x - spacing)
            lineHeight = max(lineHeight, size.height)
        }
        return CGSize(width: min(widest, width), height: y + lineHeight)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x: CGFloat = bounds.minX
        var y: CGFloat = bounds.minY
        var lineHeight: CGFloat = 0
        for subview in subviews {
            let size: CGSize = subview.sizeThatFits(.unspecified)
            if x > bounds.minX && x + size.width > bounds.maxX {
                x = bounds.minX
                y += lineHeight + spacing
                lineHeight = 0
            }
            subview.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(size))
            x += size.width + spacing
            lineHeight = max(lineHeight, size.height)
        }
    }
}

// MARK: - Desktop session

struct AgentSessionDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: AgentTranscriptModel

    init(repo: Repository, session: AgentSession, ownerAccount: String) {
        let api: AgentsApi = Services.require(ApiService.self)
        var desktop: (@MainActor () async throws -> [AgentAttachmentRef]?)?
        if let center: AgentControlCenter = HostsFeature.agents {
            desktop = { try await center.desktopAttachments(repoName: repo.name, sessionId: session.id) }
        }
        _model = State(initialValue: AgentTranscriptModel(
            api: api, repo: repo, session: session, ownerAccount: ownerAccount, desktopAttachments: desktop))
    }

    private var session: AgentSession { model.session }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                summaryCard
                DesktopSessionOpsCard(session: session, repo: model.repo)
                transcriptCard
            }
            .padding(FmSpace.x5)
        }
        .fmRefreshable { await model.load() }
        .background(fm.canvas)
        .navigationTitle("Agent session #\(session.id)")
        .navigationBarTitleDisplayMode(.inline)
        .task { await model.load() }
    }

    private var chips: some View {
        AgentChipRow {
            AgentChip(label: session.providerLabel)
            AgentChip(label: session.latestStatusSummary(120))
            if !session.rateLimitCountdownLabel().isEmpty {
                AgentChip(label: session.rateLimitCountdownLabel())
            }
            if !session.model.isEmpty {
                AgentChip(label: session.model)
            }
            if session.issueNumber > 0 {
                AgentChip(label: "#\(session.issueNumber)")
            }
            if !session.branchName.isEmpty {
                AgentBranchChip(repo: model.repo, branch: session.branchName, base: session.diffBase)
            }
            if session.numTurns > 0 {
                AgentChip(label: "\(session.numTurns) turns")
            }
            moreChips
        }
    }

    @ViewBuilder private var moreChips: some View {
        let labels: [String] = [session.durationLabel, session.costLabel, session.prLabel, session.diffLabel].filter { !$0.isEmpty }
        ForEach(labels, id: \.self) { (label: String) in
            AgentChip(label: label)
        }
        if session.conflicted {
            AgentChip(label: "Conflict")
        }
        if !session.tokenLabel.isEmpty {
            AgentChip(label: session.tokenLabel)
        }
    }

    private var summaryCard: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(session.displayTitle)
                    .fmText(.titleMedium)
                chips
                Text("Desktop-controlled session. While this phone is paired with the desktop, answer its requests, stop, continue or delete it, and merge and clean up its branch from the card below.")
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    .fixedSize(horizontal: false, vertical: true)
                actions
                if !session.statusFailureReason(400).isEmpty {
                    Text(session.statusFailureReason(400))
                        .fmText(.bodyMedium, color: fm.danger)
                        .fmMessageTools(session.statusFailureReason(400))
                }
            }
        }
    }

    @ViewBuilder private var actions: some View {
        AgentChipRow {
            if let watch = AgentWatchScreen.forRelaySession(model.repo, session, ownerAccount: model.ownerAccount) {
                NavigationLink {
                    watch
                        .shellPageClearance()
                } label: {
                    AgentWatchEntryLabel(text: session.isActive ? "Watch & listen" : "Replay & listen")
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("agent-session-watch")
            }
            if session.prNumber > 0 || !session.branchName.isEmpty {
                pullActions
            }
        }
    }

    @ViewBuilder private var pullActions: some View {
        if session.prNumber > 0 || !session.branchName.isEmpty {
            HStack(spacing: FmSpace.x2) {
                if !session.branchName.isEmpty {
                    NavigationLink {
                        BranchDiffScreen(repo: model.repo, base: session.diffBase, head: session.branchName)
                            .shellPageClearance()
                    } label: {
                        agentOutlinedLabel(icon: "difference_outlined", text: "View changes")
                    }
                    .buttonStyle(.plain)
                }
                if session.prNumber > 0 {
                    NavigationLink {
                        RepoDetailScreen(
                            repo: model.repo,
                            initialTab: .pulls,
                            initialTarget: NotificationDeepLink(repo: model.repo, initialTab: .pulls, number: session.prNumber)
                        )
                        .shellPageClearance()
                    } label: {
                        agentOutlinedLabel(icon: "call_merge_outlined", text: "Open PR #\(session.prNumber)")
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    @ViewBuilder private var transcriptBody: some View {
        if model.isLoading && model.transcript == nil && model.error == nil {
            FmLinearProgressIndicator()
        } else if let error = model.error {
            Text(error)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .fmMessageTools(error)
        } else {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text("Transcript")
                    .fmText(.titleSmall)
                AgentChipRow {
                    AgentChip(label: model.statusLabel)
                    AgentChip(label: model.repo.fullName)
                }
                if !model.attachments.isEmpty {
                    Text("Attachments")
                        .fmText(.labelMedium, color: fm.textSecondary)
                    AgentAttachmentStrip(refs: model.attachments, loader: AgentAttachmentsSupport.loader())
                }
                AgentTranscriptBlocksView(blocks: AgentTranscriptFormat.blocks(model.bodyText), repo: RepoRef(owner: model.repo.owner, name: model.repo.name))
            }
        }
    }

    private var transcriptCard: some View {
        FmCard(radius: FmRadius.lg) {
            transcriptBody
        }
    }
}

/// An outlined icon+label face.
@MainActor
func agentOutlinedLabel(icon: String, text: String) -> some View {
    AgentOutlinedFace(icon: icon, text: text)
}

struct AgentOutlinedFace: View {
    @Environment(\.fmPalette) private var fm
    let icon: String
    let text: String

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon(icon, size: 16, color: fm.accent)
            Text(text)
                .fmText(.labelMedium, color: fm.accent)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 36)
        .overlay(
            RoundedRectangle(cornerRadius: FmRadius.full)
                .stroke(fm.border, lineWidth: 1)
        )
    }
}

// MARK: - Queued session

struct LaunchedAgentSessionScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: LaunchedAgentSessionModel
    @State private var confirmDrop: Bool = false
    @State private var signal: AgentListSignal? = Services.get(AgentListSignal.self)

    init(repo: Repository, session: AgentLaunchSession, pulls: [PublishedPull] = []) {
        let api: AgentsApi = Services.require(ApiService.self)
        _model = State(initialValue: LaunchedAgentSessionModel(
            api: api, repo: repo, session: session, pulls: pulls, attachmentLoader: AgentAttachmentsSupport.loader()))
    }

    private var session: AgentLaunchSession { model.session }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                content.padding(FmSpace.x4)
            }
            .fmRefreshable { await model.refresh() }
        }
        .shellDock { composer }
        .background(fm.canvas)
        .navigationTitle(session.displayTitle)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { toolbarItems }
        .task { await model.follow() }
        .task(id: model.followsLive) { await model.followLive() }
        .onChange(of: signal?.rings ?? 0) { (_: Int, _: Int) in
            Task { await model.follow() }
        }
        .onChange(of: signal?.liveRings ?? 0) { (_: Int, _: Int) in
            if session.isActive {
                Task { await model.follow() }
            }
        }
        .confirmationDialog("Drop this session?", isPresented: $confirmDrop, titleVisibility: .visible) {
            Button("Drop", role: .destructive) {
                Task { await drop() }
            }
            Button("Keep", role: .cancel) {}
        } message: {
            Text("The queue entry and its record here go away. Work already running on the node keeps going there.")
        }
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            Button(role: .destructive) {
                confirmDrop = true
            } label: {
                Label("Delete", systemImage: "trash")
                    .labelStyle(.iconOnly)
            }
            .disabled(model.busy)
            .accessibilityIdentifier("agent-run-delete")
        }
    }

    private func drop() async {
        if await model.cancel() {
            dismiss()
        }
    }

    private var chips: some View {
        AgentChipRow {
            AgentChip(label: session.statusLabel)
            if !session.targetNode.isEmpty {
                AgentChip(label: session.targetNode)
            }
            if !session.provider.isEmpty {
                AgentChip(label: session.provider)
            }
            if !session.requestedModel.isEmpty {
                AgentChip(label: session.requestedModel)
            }
            if session.createPr {
                AgentChip(label: "opens PR")
            }
            if !session.branchName.isEmpty {
                AgentBranchChip(repo: model.repo, branch: session.branchName, base: session.baseBranch)
            }
        }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            chips
            if session.isRetryable {
                AgentRetryCard(session: session, busy: model.busy) {
                    Task { await model.retry() }
                }
            }
            notes
            history
            outcome
        }
    }

    @ViewBuilder private var notes: some View {
        if !session.diagnosticNote.isEmpty {
            Text(session.diagnosticNote).fmText(.bodyMedium, color: fm.warning)
                .fmMessageTools(session.diagnosticNote)
        }
        if !session.lastError.isEmpty {
            Text(session.lastError).fmText(.bodyMedium, color: fm.danger)
                .fmMessageTools(session.lastError)
        }
    }

    @ViewBuilder private var history: some View {
        let turns: [AgentLaunchMessage] = model.displayHistory
        if turns.isEmpty {
            Text("No transcript yet. It fills in as the node works.")
                .fmText(.bodyMedium, color: fm.textSecondary)
        }
        LazyVStack(alignment: .leading, spacing: FmSpace.x3) {
            ForEach(Array(turns.indices), id: \.self) { (index: Int) in
                AgentHistoryEntry(
                    message: turns[index],
                    attachments: model.attachments(at: index),
                    loader: model.attachmentLoader,
                    sending: model.isPending(index),
                    repo: RepoRef(owner: model.repo.owner, name: model.repo.name))
            }
        }
    }

    @ViewBuilder private var outcome: some View {
        if let notice = model.notice {
            Text(notice).fmText(.bodyMedium, color: fm.success)
                .fmMessageTools(notice)
        }
        if let error = model.error {
            Text(error).fmText(.bodyMedium, color: fm.danger)
                .fmMessageTools(error)
        }
    }

    /// The follow-up composer, always there so a run started from the
    /// shell lands ready for the next instruction. Until the relay takes
    /// follow-ups (a node has claimed the run) it says so and does not send.
    private var composer: some View {
        let open: Bool = model.followUpClosedText == nil
        let send: (() -> Void)? = open ? { () -> Void in Task { await model.send() } } : nil
        return PromptComposer(
            text: $model.followUp,
            hint: model.followUpClosedText ?? model.followUpHint,
            attachments: $model.followUpAttachments,
            busy: model.busy,
            onSend: send,
            offersFix: true,
            uploadsFiles: true,
            accessibilityPrefix: "session-composer")
            .padding(.horizontal, FmSpace.x3)
            .padding(.bottom, FmSpace.x2)
    }
}

struct AgentHistoryEntry: View {
    @Environment(\.fmPalette) private var fm
    let message: AgentLaunchMessage
    var attachments: [AgentAttachmentRef] = []
    var loader: AgentAttachmentLoader = AgentAttachmentLoader()
    var sending: Bool = false
    var repo: RepoRef? = nil

    private var speaker: String {
        if !message.isUser {
            return "Agent"
        }
        let name: String = message.author.isEmpty ? "You" : message.author
        return sending ? "\(name) · sending…" : name
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(speaker)
                .fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis)
            if !attachments.isEmpty {
                AgentAttachmentStrip(refs: attachments, loader: loader)
                    .padding(.vertical, FmSpace.x1)
            }
            LinkedText(message.text, repo: repo)
                .fmText(.bodyMedium, height: 1.45)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .fmMessageActions(text: message.text)
        .opacity(sending ? 0.7 : 1)
    }
}

/// The offer to run a failed or stopped session again.
struct AgentRetryCard: View {
    @Environment(\.fmPalette) private var fm
    let session: AgentLaunchSession
    let busy: Bool
    let onRetry: () -> Void

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(session.isStopped ? "This run stopped" : "This run failed")
                    .fmText(.titleSmall)
                Text(AgentsLogic.retryDetail(session))
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    .fixedSize(horizontal: false, vertical: true)
                FmToneButton(
                    label: session.canResume ? "Retry on its branch" : "Retry",
                    icon: "refresh",
                    tone: .accent,
                    filled: true,
                    onPressed: busy ? nil : onRetry
                )
                .padding(.top, FmSpace.x2)
            }
        }
    }
}
