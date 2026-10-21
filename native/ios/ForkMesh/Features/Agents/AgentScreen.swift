// The agent surface for one repository on this phone. Three states: not configured (no provider yet),
// idle with nothing said, or a running transcript; the composer is always at
// the bottom. The run belongs to AgentSessionService, so leaving and coming
// back finds it still going. Logic: FMAgents' AgentScreenModel.

import FMAgents
import FMApi
import FMBase
import FMCode
import FMControl
import FMLocalAI
import SwiftUI

struct AgentScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: AgentScreenModel
    @State private var showSetup: Bool = false
    @State private var showAttach: Bool = false
    @State private var showLocalAI: Bool = false
    @State private var preparation: AgentPreparedPull?
    @State private var notice: FmNotice?
    @State private var confirmMove: Bool = false
    @State private var sent: Int = 0

    init(repoName: String, workspace: GitWorkspace, record: LocalRepoRecord?, initialPrompt: String = "", autoStart: Bool = false) {
        _model = State(initialValue: AgentScreenModel(
            repoName: repoName,
            workspace: GitAgentWorkspace(workspace),
            gitWorkspace: workspace,
            record: record,
            pullRequests: AgentsFeature.pullRequests(),
            sessions: AgentsFeature.sessions(),
            store: AgentsFeature.credentialStore(),
            author: AgentsFeature.author(),
            localModels: { AgentsFeature.localModels() },
            initialPrompt: initialPrompt,
            // Photos a composer handed over with the prompt it auto-starts.
            initialImages: autoStart ? (Services.get(AgentPromptHandoff.self)?.takeImages() ?? []) : [],
            initialCredentials: autoStart ? Services.get(AgentPromptHandoff.self)?.takeCredentials() : nil,
            autoStart: autoStart,
            desktop: HubAgentHandoffDesktop { HostsFeature.hub }
        ))
    }

    var body: some View {
        VStack(spacing: 0) {
            if let error = model.error {
                AgentErrorNote(message: error) { model.error = nil }
                    .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x2, FmSpace.x4, 0))
            }
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x2, FmSpace.x4, 0))
            }
            AgentPromptDeck(deck: PromptDeck(said: model.events.compactMap(\.userText)), accessibilityPrefix: "agent-prompt-deck")
                .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x2, FmSpace.x4, 0))
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            if let handle = model.prCandidate {
                AgentPullRequestOffer(branch: handle.branch, busy: model.prBusy) {
                    Task { await offerPullRequest() }
                }
            }
            if let readiness = model.gemmaReadiness, !readiness.isReady {
                GemmaReadinessNote(readiness: readiness) { showLocalAI = true }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.bottom, FmSpace.x2)
            }
        }
        .shellDock {
            PromptComposer(
                text: $model.draft,
                hint: model.composerHint,
                attachments: model.acceptsImages ? $model.draftImages : nil,
                modelLabel: model.modelChipLabel,
                onPickModel: { Task { await openSetup() } },
                running: model.isRunning,
                onStop: { model.stop() },
                onSend: { Task { await send() } },
                offersFix: true,
                extraAttachAction: PromptAttachAction(
                    label: "Reference a file", systemImage: "doc.text", run: { showAttach = true }),
                uploadsFiles: true,
                accessibilityPrefix: "agent-composer")
        }
        .background(fm.canvas)
        .navigationTitle(model.repoName)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { toolbarItems }
        .task {
            if model.isLoading && model.load() {
                await send()
            }
        }
        .sheet(isPresented: $showSetup) {
            AgentSetupSheet(initial: model.credentials) { (saved: AgentCredentials?) in
                model.credentialsChanged(saved)
            }
            .fmSheetStyle()
        }
        .sheet(isPresented: $showAttach) {
            AgentAttachFileSheet(root: model.gitWorkspaceRoot) { (path: String) in
                model.attach(path)
            }
            .fmSheetStyle(fitsContent: false)
        }
        .sheet(isPresented: $showLocalAI) {
            NavigationStack {
                LocalAISettingsView()
            }
        }
        .sheet(item: $preparation) { (prepared: AgentPreparedPull) in
            AgentPullRequestSheet(preparation: prepared.preparation, suggestedTitle: model.suggestedPullTitle) { (title: String) in
                Task { await submitPullRequest(prepared.preparation, title: title) }
            }
            .fmSheetStyle()
        }
        .confirmationDialog("Continue on \(model.desktopName)?", isPresented: $confirmMove, titleVisibility: .visible) {
            Button("Continue on desktop") {
                Task { await moveToDesktop() }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(model.isRunning
                ? "The agent stops here. Its work is committed and pushed, and the desktop picks it up on the same branch with the conversation so far."
                : "Its work is committed and pushed, and the desktop picks it up on the same branch with the conversation so far.")
        }
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItemGroup(placement: .topBarTrailing) {
            if model.isRunning {
                Button {
                    model.stop()
                } label: {
                    FmIcon("stop_circle_outlined", size: 22, color: fm.danger)
                }
                .accessibilityLabel(Text("Stop"))
            }
            if model.canMoveToDesktop {
                Button {
                    confirmMove = true
                } label: {
                    if model.moving {
                        ProgressView()
                    } else {
                        FmIcon("desktop_windows_outlined", size: 22, color: fm.textSecondary)
                    }
                }
                .disabled(model.moving)
                .accessibilityLabel(Text("Continue on desktop"))
                .accessibilityIdentifier("agent-move-to-desktop")
            }
            Button {
                Task { await openSetup() }
            } label: {
                FmIcon("tune", size: 22, color: fm.textSecondary)
            }
            .accessibilityLabel(Text("Agent settings"))
        }
    }

    @ViewBuilder private var content: some View {
        if model.isLoading {
            FmSkeletonList(rows: 5, hasLeading: false, label: "Loading session")
        } else if model.credentials == nil {
            AgentNotConfigured { Task { await openSetup() } }
        } else if model.events.isEmpty {
            AgentIdle()
        } else {
            AgentTranscriptList(events: model.events, attachments: { (index: Int) -> [AgentAttachmentRef] in
                model.handle?.attachments(at: index) ?? []
            }, pin: sent)
        }
    }

    // MARK: Behaviour

    private func send() async {
        if model.draft.dartTrimmed.isEmpty && model.draftImages.isEmpty {
            return
        }
        if !(await AccountFeature.requireAccount(action: "run the coding agent")) {
            return
        }
        sent += 1
        let outcome: AgentSendOutcome = await model.send()
        if outcome == .needsSetup {
            await openSetup()
        }
    }

    private func openSetup() async {
        if !(await AccountFeature.requireAccount(action: "set up the coding agent")) {
            return
        }
        showSetup = true
    }

    private func moveToDesktop() async {
        if !(await AccountFeature.requireAccount(action: "move the agent to your desktop")) {
            return
        }
        if let move = await model.moveToDesktop() {
            notice = FmNotice(move.message)
        }
    }

    private func offerPullRequest() async {
        if !(await AccountFeature.requireAccount(action: "open a pull request")) {
            return
        }
        if let prepared = await model.preparePullRequest() {
            preparation = AgentPreparedPull(preparation: prepared)
        }
    }

    private func submitPullRequest(_ prepared: PullRequestPreparation, title: String) async {
        preparation = nil
        if let line = await model.submitPullRequest(prepared, title: title) {
            notice = FmNotice(line)
        }
    }
}

/// A prepared pull request on its way into the sheet.
struct AgentPreparedPull: Identifiable {
    let id: UUID = UUID()
    let preparation: PullRequestPreparation
}

extension AgentScreenModel {
    /// The worktree the file picker walks.
    var gitWorkspaceRoot: URL? {
        return (workspace as? GitAgentWorkspace)?.worktree
    }
}

// MARK: - States

/// Shown until a provider is configured: the agent cannot work without one,
/// so this says so instead of failing at the first prompt.
struct AgentNotConfigured: View {
    @Environment(\.fmPalette) private var fm
    let onConfigure: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            FmIcon("key_outlined", size: 40, color: fm.textTertiary)
            Text("Connect a model to get started")
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x4)
            Text("The agent runs on your own Claude or OpenAI account, or on Gemma 4 on this iPhone. A key is stored on this device and sent only to that provider.")
                .fmText(.bodyMedium, color: fm.textSecondary, height: 1.45)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x2)
            FmToneButton(label: "Choose a model", icon: "add", tone: .accent, filled: true, onPressed: onConfigure)
                .padding(.top, FmSpace.x4)
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// Configured, nothing said yet.
struct AgentIdle: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        VStack(spacing: 0) {
            FmIcon("auto_awesome_outlined", size: 36, color: fm.accent)
            Text("What should we build?")
                .fmText(.titleLarge)
                .padding(.top, FmSpace.x4)
            Text("Work happens on its own branch, so your current work is safe.")
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x2)
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// The transcript, pinned to its newest event.
struct AgentTranscriptList: View {
    let events: [AgentEvent]
    var attachments: (Int) -> [AgentAttachmentRef] = { (_: Int) -> [AgentAttachmentRef] in [] }
    var pin: Int = 0
    @State private var loader: AgentAttachmentLoader = AgentAttachmentsSupport.loader()

    var body: some View {
        TranscriptFollowScroll(tail: events.count, pin: pin, accessibilityPrefix: "agent-transcript") {
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(Array(events.indices), id: \.self) { (index: Int) in
                    AgentEventTile(event: events[index], attachments: attachments(index), loader: loader)
                        .id(index)
                }
            }
            .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x3, FmSpace.x4, FmSpace.x3))
        }
    }
}

/// One transcript entry. Tool results collapse by default: what matters at a
/// glance is which tool ran, not its output.
struct AgentEventTile: View {
    @Environment(\.fmPalette) private var fm
    let event: AgentEvent
    var attachments: [AgentAttachmentRef] = []
    var loader: AgentAttachmentLoader = AgentAttachmentLoader()
    @State private var expanded: Bool = false

    var body: some View {
        switch event {
        case .userMessage(let text):
            bubble(text)
        case .assistantMessage(let text):
            Text(text)
                .fmText(.bodyMedium, height: 1.5)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageActions(text: text)
                .padding(.bottom, FmSpace.x3)
        case .toolCall(let call):
            toolRow(icon: "build_outlined", title: call.name, detail: Self.argSummary(call), tone: fm.textSecondary, expandable: false)
        case .toolResult(let result):
            toolRow(
                icon: result.isError ? "error_outline" : "check_circle_outline",
                title: result.name,
                detail: result.output,
                tone: result.isError ? fm.danger : fm.success,
                expandable: true
            )
        case .notice(let text, let isError):
            notice(text, isError: isError)
        case .done(let done):
            doneRow(done)
        }
    }

    private func bubble(_ text: String) -> some View {
        HStack {
            Spacer(minLength: FmSpace.x7)
            VStack(alignment: .trailing, spacing: FmSpace.x2) {
                if !attachments.isEmpty {
                    AgentAttachmentStrip(refs: attachments, loader: loader, trailing: true)
                }
                Text(text)
                    .fmText(.bodyMedium, height: 1.4)
                    .padding(.horizontal, FmSpace.x3)
                    .padding(.vertical, FmSpace.x2)
                    .background(fm.bgOverlay)
                    .clipShape(FmRoundedShape(radius: FmRadius.lg))
                    .fmMessageActions(text: text)
            }
        }
        .padding(.bottom, FmSpace.x3)
    }

    private func notice(_ text: String, isError: Bool) -> some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon(isError ? "warning_amber" : "info_outline", size: 15, color: isError ? fm.warning : fm.textTertiary)
            Text(text)
                .fmText(.bodySmall, color: fm.textSecondary, height: 1.4)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmMessageTools(text)
        }
        .padding(.bottom, FmSpace.x3)
    }

    private func toolRow(icon: String, title: String, detail: String, tone: Color, expandable: Bool) -> some View {
        let preview: String = detail.components(separatedBy: "\n").first ?? ""
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                FmIcon(icon, size: 14, color: tone)
                Text(title)
                    .fmMono(size: FmType.meta, weight: FmType.emphasis, color: fm.textPrimary)
                Text(preview)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if expandable {
                    FmIcon(expanded ? "expand_less" : "expand_more", size: 16, color: fm.textTertiary)
                }
            }
            if expanded && expandable {
                Text(detail)
                    .fmMono(size: FmType.micro, color: fm.textSecondary, height: 1.4)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(FmSpace.x3)
                    .background(fm.bgOverlay)
                    .clipShape(FmRoundedShape(radius: FmRadius.md))
            }
        }
        .contentShape(Rectangle())
        .onTapGesture {
            if expandable {
                expanded.toggle()
            }
        }
        .padding(.bottom, FmSpace.x2)
    }

    private func doneRow(_ done: AgentDone) -> some View {
        let label: String
        let tone: Color
        switch done.reason {
        case .completed:
            label = "Finished"
            tone = fm.success
        case .cancelled:
            label = "Stopped"
            tone = fm.textTertiary
        case .turnLimit:
            label = "Stopped at the turn limit"
            tone = fm.textTertiary
        case .error:
            label = "Failed"
            tone = fm.danger
        case .refused:
            label = "Declined"
            tone = fm.danger
        }
        return HStack(spacing: FmSpace.x3) {
            Rectangle().fill(fm.border).frame(height: 1)
            Text("\(label) · \(done.turns) \(done.turns == 1 ? "turn" : "turns")")
                .fmText(.labelSmall, color: tone, weight: FmType.emphasis)
                .fixedSize()
            Rectangle().fill(fm.border).frame(height: 1)
        }
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x4)
    }

    static func argSummary(_ call: AgentToolCall) -> String {
        if let path = (call.arguments["path"] ?? call.arguments["pattern"])?.stringValue, !path.isEmpty {
            return path
        }
        if let message = call.arguments["message"]?.stringValue, !message.isEmpty {
            return message.components(separatedBy: "\n").first ?? message
        }
        return ""
    }
}

// MARK: - Notes

struct AgentErrorNote: View {
    @Environment(\.fmPalette) private var fm
    let message: String
    let onDismiss: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon("error_outline", size: 16, color: fm.danger)
            Text(message)
                .fmText(.bodySmall, height: 1.4)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmMessageTools(message)
            Button(action: onDismiss) {
                FmIcon("close", size: 16, color: fm.textTertiary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Dismiss"))
        }
        .padding(FmSpace.x3)
        .background(fm.bgOverlay)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.md).stroke(fm.danger.opacity(0.4), lineWidth: 1))
    }
}

/// The post-run affordance: the work is committed on its branch; one tap
/// turns it into a pull request.
struct AgentPullRequestOffer: View {
    @Environment(\.fmPalette) private var fm
    let branch: String
    let busy: Bool
    let onTap: () -> Void

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("call_merge", size: 18, color: fm.accent)
            Text("The work is committed on \(branch).")
                .fmText(.bodySmall, color: fm.textSecondary)
                .lineLimit(2)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmToneButton(
                label: busy ? "Preparing…" : "Create pull request",
                tone: .accent,
                filled: true,
                height: 30,
                onPressed: busy ? nil : onTap
            )
        }
        .padding(FmSpace.x3)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.lg).stroke(fm.border, lineWidth: 1))
        .padding(EdgeInsets.fmLTRB(FmSpace.x3, 0, FmSpace.x3, FmSpace.x2))
    }
}
