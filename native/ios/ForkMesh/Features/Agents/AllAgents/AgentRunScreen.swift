// WS7: one run from the unified list: its transcript followed live (a desktop streams what it writes
// through `agents.follow`; an older desktop sends what is new on each bell; a node run is re-read
// from the relay), and stop, continue and follow-up where the run's transport supports them.

import FMAgents
import FMApi
import FMBase
import FMCode
import FMControl
import SwiftUI

struct AgentRunScreen: View {
    @Bindable var center: AgentControlCenter
    let row: UnifiedAgentRow

    @State private var follower = TranscriptFollower()
    @State private var blocks: [TranscriptBlock] = []
    @State private var pending = PendingPrompts()
    @State private var questions: [AgentQuestion] = []
    @State private var blockCache = TranscriptBlockCache()
    @State private var status = ""
    @State private var queue: AgentRunQueue?
    @State private var active = false
    @State private var prompt = ""
    @State private var photos: [PromptAttachment] = []
    @State private var error = ""
    @State private var notice = ""
    @State private var busy = false
    @State private var tapId: UUID?
    @State private var stream: DesktopTranscriptStream?
    @State private var liveRefused = false
    @State private var confirmDelete: AgentLineDeleteRequest?
    @State private var runnerSheet: RunnerSheet?
    @State private var leaveAfterMerge = false
    @State private var confirmDesktopMove = false
    @State private var movingToDesktop = false
    @State private var terminalResume: Bool?
    @State private var confirmTerminal = false
    @State private var openTerminal = false
    @State private var pickedModel: String?
    @State private var pickedMode: AgentPermissionMode?
    @State private var pullOpening = false
    @State private var openedPull = 0
    @Environment(\.dismiss) private var dismiss
    @Environment(\.shellStackTab) private var stackTab
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    var body: some View {
        VStack(spacing: 0) {
            Text(live.title)
                .fmText(.titleMedium)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 12)
                .padding(.top, 4)
                .accessibilityAddTraits(.isHeader)
                .accessibilityIdentifier("agent-run-title")
            HStack(spacing: 8) {
                TransportBadge(row: live)
                Text([live.repoLabel, live.isRunner ? "" : live.node].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Spacer()
                Text(status.isEmpty ? row.statusLabel : status)
                    .font(.caption.weight(.semibold))
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            if row.outside {
                Text(live.outsideSummary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.bottom, 6)
                    .accessibilityIdentifier("agent-run-outside-note")
            }
            if row.isDesktop, !row.outside {
                DesktopQueuePositionLine(center: center, sessionId: row.localId)
                    .padding(.horizontal, 12)
                    .padding(.bottom, 4)
            }
            if !row.isDesktop, let queue {
                RunnerQueueCard(queue: queue)
                    .padding(.horizontal, 12)
                    .padding(.bottom, 4)
            }
            ScrollViewReader { proxy in
                ScrollView {
                    if !follower.attachments.isEmpty {
                        AgentAttachmentStrip(refs: follower.attachments, loader: AgentAttachmentsSupport.loader(hub: center.hub))
                            .padding(.horizontal, 12)
                            .padding(.top, 12)
                    }
                    if !questions.isEmpty {
                        AgentQuestionCards(questions: questions, busy: busy) { question, answer in
                            Task { await self.answer(question, answer) }
                        }
                        .padding(.horizontal, 12)
                        .padding(.top, 12)
                    }
                    if shownBlocks.isEmpty {
                        Text("No transcript yet.")
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(12)
                    } else {
                        AgentTranscriptBlocksView(blocks: shownBlocks, repo: row.repoOwner.isEmpty || row.repoName.isEmpty ? nil : RepoRef(owner: row.repoOwner, name: row.repoName))
                            .padding(.horizontal, 12)
                            .padding(.vertical, FmSpace.x2)
                    }
                }
                .onChange(of: follower.next) { _, _ in
                    scrollToEnd(proxy)
                }
                .onChange(of: pending) { _, _ in
                    scrollToEnd(proxy)
                }
            }
            if !error.isEmpty {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(error)
                    .padding(.horizontal, 12)
            }
            if !notice.isEmpty {
                Text(notice)
                    .font(.footnote)
                    .foregroundStyle(.orange)
                    .fmMessageTools(notice)
                    .padding(.horizontal, 12)
                    .accessibilityIdentifier("agent-run-prompt-notice")
            }
        }
        .shellDock {
            if live.canPrompt {
                PromptComposer(
                    text: $prompt,
                    hint: live.composerHint,
                    attachments: row.outside ? nil : $photos,
                    modelLabel: modelChoices.isEmpty ? nil : modelLabel,
                    modelOptions: modelChoices.map(modelOption),
                    selectedModelId: modelId,
                    onSelectModel: { (id: String) in pickedModel = id },
                    modeOptions: live.offersModeChoice ? AgentPermissionMode.allCases.map(Self.modeOption) : [],
                    selectedModeId: runMode.rawValue,
                    onSelectMode: { (id: String) in pickedMode = AgentPermissionMode(label: id) },
                    busy: busy,
                    onSend: { Task { await send() } },
                    offersFix: true,
                    uploadsFiles: true,
                    accessibilityPrefix: "agent-run-composer")
                    .padding(.horizontal, FmSpace.x3)
                    .padding(.bottom, FmSpace.x2)
            }
        }
        .navigationBarTitleDisplayMode(.inline)
        .modifier(AgentRunToolbar {
            HStack(spacing: 6) {
                if row.isPhoneTerminal {
                    NavigationLink {
                        DesktopTerminalScreen(center: center, terminal: row.terminal)
                            .shellPageClearance()
                    } label: {
                        Label("Terminal", systemImage: "apple.terminal")
                            .labelStyle(.iconOnly)
                    }
                    .accessibilityIdentifier("agent-run-terminal")
                } else if row.offersTerminal && terminalResume == true {
                    Button {
                        if live.isActive || active {
                            confirmTerminal = true
                        } else {
                            openTerminal = true
                        }
                    } label: {
                        Label("Terminal", systemImage: "apple.terminal")
                            .labelStyle(.iconOnly)
                    }
                    .disabled(busy)
                    .accessibilityLabel(Text("Open in \(agentProviderLabel(row.provider))'s terminal"))
                    .accessibilityIdentifier("agent-run-open-terminal")
                }
                if let run = retryRun {
                    Button {
                        Task { await retry(run) }
                    } label: {
                        Label(run.canResume ? "Retry on its branch" : "Retry", systemImage: "arrow.clockwise")
                            .labelStyle(.iconOnly)
                    }
                    .disabled(busy)
                    .accessibilityIdentifier("agent-run-retry")
                } else if row.canContinue && !active {
                    Button {
                        Task { await perform { try await center.continueRun(row) } }
                    } label: {
                        Label("Continue", systemImage: "play.fill")
                            .labelStyle(.iconOnly)
                    }
                }
                if row.outside ? row.canStop : (row.canStop || active) {
                    Button(role: .destructive) {
                        Task { await perform { try await center.stop(row) } }
                    } label: {
                        Label("Stop", systemImage: "stop.fill")
                            .labelStyle(.iconOnly)
                    }
                }
                if canMoveToDesktop {
                    Button {
                        confirmDesktopMove = true
                    } label: {
                        if movingToDesktop {
                            ProgressView()
                        } else {
                            FmIcon("desktop_windows_outlined", size: 22, color: fm.textSecondary)
                        }
                    }
                    .disabled(movingToDesktop || busy)
                    .accessibilityLabel(Text("Continue on desktop"))
                    .accessibilityIdentifier("agent-run-move-to-desktop")
                }
                if isRunnerRun, pullNumber > 0, !row.repoName.isEmpty {
                    NavigationLink {
                        PullDetailScreen(repo: Repository(owner: row.repoOwner, name: row.repoName), number: pullNumber)
                            .shellPageClearance()
                    } label: {
                        RunnerPullLabel(text: "#\(pullNumber)", running: false)
                    }
                    .accessibilityLabel(Text("PR #\(pullNumber)"))
                    .accessibilityIdentifier("agent-run-open-pull")
                } else if isRunnerRun, live.offersPullOpen, !active, RunnerAgentFeature.api != nil {
                    Button {
                        Task { await createPull() }
                    } label: {
                        RunnerPullLabel(text: "PR", running: pullOpening)
                    }
                    .disabled(pullOpening || busy)
                    .accessibilityLabel(Text("Create PR"))
                    .accessibilityIdentifier("agent-run-create-pull")
                }
                if isRunnerRun, live.hasCloudBranch, let api = RunnerAgentFeature.api {
                    NavigationLink {
                        RunnerDiffScreen(
                            api: api, ref: live.runnerRef, canMerge: canMergeRunner, mergeOnOpen: true,
                            onFixConflicts: fixConflicts, onMerged: merged, onLeave: leave)
                            .shellPageClearance()
                    } label: {
                        Label("Merge & clean up", systemImage: "arrow.triangle.merge")
                            .labelStyle(.iconOnly)
                    }
                    .disabled(!live.offersMerge || live.isActive || active)
                    .accessibilityIdentifier("agent-run-merge")
                    if canUpdateRunner {
                        Button {
                            runnerSheet = .update
                        } label: {
                            Label(RunnerAgentOps.updateTitle(base: live.baseBranch), systemImage: "arrow.triangle.2.circlepath")
                                .labelStyle(.iconOnly)
                        }
                        .disabled(busy)
                        .accessibilityIdentifier("agent-run-update")
                    }
                    if live.conflicted && !live.merged {
                        fixConflictsButton
                            .labelStyle(.iconOnly)
                    }
                    if live.offersCleanup {
                        Button {
                            runnerSheet = .cleanup
                        } label: {
                            Label("Clean up", systemImage: "scissors")
                                .labelStyle(.iconOnly)
                        }
                        .disabled(busy)
                        .accessibilityIdentifier("agent-run-cleanup")
                    }
                }
                if row.isDesktop, row.localId > 0, let client = AgentOpsFeature.client {
                    NavigationLink {
                        AgentDiffScreen(
                            client: client, sessionId: row.localId, canMerge: canMergeDesktop, mergeOnOpen: true,
                            onMerged: merged, onLeave: leave)
                            .shellPageClearance()
                    } label: {
                        Label("Merge & clean up", systemImage: "arrow.triangle.merge")
                            .labelStyle(.iconOnly)
                    }
                    .disabled(row.isActive || active)
                    .accessibilityIdentifier("agent-run-merge")
                    if live.conflicted && !live.merged {
                        fixConflictsButton
                            .labelStyle(.iconOnly)
                    }
                }
                if deletesHere {
                    Button(role: .destructive) {
                        deleteNow()
                    } label: {
                        Label("Delete", systemImage: "trash")
                            .labelStyle(.iconOnly)
                    }
                    .disabled(busy)
                    .accessibilityIdentifier("agent-run-delete")
                }
            }
            .buttonStyle(AgentRunActionButtonStyle())
            .modifier(AgentRunActionsCap())
        } changes: {
            changesLink
        })
        .agentOutsideDeleteConfirmation($confirmDelete) { deleteAfterUndoWindow($0) }
        .sheet(item: $runnerSheet, onDismiss: leaveIfMerged) { (sheet: RunnerSheet) in
            if let api = RunnerAgentFeature.api {
                switch sheet {
                case .cleanup:
                    RunnerMergeSheet(api: api, ref: live.runnerRef, mode: .cleanup) { line in
                        notice = line
                        Task { await center.refresh() }
                    }
                case .update:
                    RunnerMergeSheet(api: api, ref: live.runnerRef, mode: .update, onFixConflicts: fixConflicts) { line in
                        notice = line
                        Task { await center.refresh() }
                    }
                }
            }
        }
        .onAppear(perform: leaveAfterChanges)
        .bareShellTop()
        .navigationDestination(isPresented: $openTerminal) {
            DesktopTerminalScreen(center: center, row: live)
                .shellPageClearance()
        }
        .confirmationDialog(
            "Open in \(agentProviderLabel(row.provider))'s terminal?", isPresented: $confirmTerminal,
            titleVisibility: .visible
        ) {
            Button("Open terminal") {
                openTerminal = true
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The desktop stops this run and resumes its conversation in \(agentProviderLabel(row.provider)) in a terminal you watch and type into here. Send \"continue\" there to pick the work up.")
        }
        .confirmationDialog(
            "Continue on \(desktopName)?", isPresented: $confirmDesktopMove, titleVisibility: .visible
        ) {
            Button("Continue on desktop") {
                Task { await moveToDesktop() }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(live.isActive || active
                 ? "The run on \(runnerLabel) is stopped and deleted, and the desktop picks it up on the same branch with the conversation so far."
                 : "The desktop picks it up on the same branch with the conversation so far, and the run on \(runnerLabel) is deleted.")
        }
        .task {
            if row.offersTerminal && terminalResume == nil {
                terminalResume = await DesktopTerminalSupport.check(center.hub, DesktopTerminalSupport.resumeCapability)
            }
        }
        .task {
            await follow()
            let id = center.hub.addEventTap { name, _ in
                if name == "agents.changed" && row.isDesktop && !streaming { Task { await load() } }
            }
            tapId = id
        }
        .task(id: followsCloudLive) {
            guard followsCloudLive else { return }
            let outcome = await center.followCloudLive(row) { (delta: AgentTranscriptDelta) in show(delta) }
            switch outcome {
            case .ended:
                Task { await load() }
            case .refused:
                liveRefused = true
            case .cancelled:
                break
            }
        }
        .task(id: follower.next) { await loadQuestions() }
        .task(id: queue == nil) {
            while let waiting = queue, !Task.isCancelled {
                let wait = waiting.refreshAfterMs(Int(Date().timeIntervalSince1970 * 1000))
                try? await Task.sleep(for: .milliseconds(wait))
                if Task.isCancelled { return }
                await load()
            }
        }
        .onChange(of: live.updatedAtMs) { (_: Int, _: Int) in
            if isRunnerRun { Task { await load() } }
        }
        .onDisappear {
            if let tapId { center.hub.removeEventTap(tapId) }
            stream?.stop()
            stream = nil
        }
        .fmRefreshable { await load() }
    }

    private var followsCloudLive: Bool { isRunnerRun && active && !liveRefused }

    private var shownBlocks: [TranscriptBlock] { pending.isEmpty ? blocks : blocks + pending.blocks }

    private func scrollToEnd(_ proxy: ScrollViewProxy) {
        let last = shownBlocks.count - 1
        if last < 0 { return }
        DispatchQueue.main.async { proxy.scrollTo(last, anchor: .bottom) }
    }

    @ViewBuilder private var changesLink: some View {
        if row.isDesktop, row.localId > 0, let client = AgentOpsFeature.client {
            NavigationLink {
                AgentDiffScreen(
                    client: client, sessionId: row.localId, canMerge: canMergeDesktop, onMerged: merged, onLeave: leave)
                    .shellPageClearance()
            } label: {
                changesLabel
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("agent-run-diff")
        } else if live.hasCloudBranch, let api = RunnerAgentFeature.api {
            NavigationLink {
                RunnerDiffScreen(
                    api: api, ref: live.runnerRef, canMerge: canMergeRunner, onFixConflicts: fixConflicts, onMerged: merged,
                    onLeave: leave)
                    .shellPageClearance()
            } label: {
                changesLabel
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("agent-run-diff")
        } else if !row.isDesktop, !row.branchName.isEmpty, !row.repoName.isEmpty {
            NavigationLink {
                BranchDiffScreen(
                    repo: Repository(owner: row.repoOwner, name: row.repoName), base: row.baseBranch,
                    head: row.branchName, title: row.title)
                    .shellPageClearance()
            } label: {
                changesLabel
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("agent-run-diff")
        }
    }

    private var changesLabel: some View {
        Label {
            Text("View branch diff")
        } icon: {
            FmIcon("difference_outlined", size: 20, color: Color.white)
                .shadow(color: Color.black.opacity(0.18), radius: 1, y: 0.5)
                .frame(width: 34, height: 34)
                .modifier(MergeGlass(tint: fm.success, reduceTransparency: reduceTransparency))
                .contentShape(RoundedRectangle(cornerRadius: MergeGlass.radius, style: .continuous))
        }
        .labelStyle(.iconOnly)
        .accessibilityValue(Text(live.branchName.isEmpty ? "" : branchLine))
    }

    private var streaming: Bool { stream?.isStreaming ?? false }

    private var canMergeDesktop: Bool { !(live.isActive || active) }

    private var canMergeRunner: Bool { live.offersMerge && !active }

    private var canUpdateRunner: Bool { canMergeRunner && live.canMerge && !live.isActive }

    private var pullNumber: Int { live.prNumber > 0 ? live.prNumber : openedPull }

    private func createPull() async {
        guard let api = RunnerAgentFeature.api, !pullOpening else { return }
        pullOpening = true
        defer { pullOpening = false }
        let model = RunnerPullModel(api: api, inbox: Services.get(InboxService.self), ref: live.runnerRef)
        let opened = await model.open()
        if model.number > 0 { openedPull = model.number }
        error = model.errorText
        notice = model.notice
        if opened && model.errorText.isEmpty {
            FmHaptics.commit()
        } else {
            FmHaptics.failure()
        }
        if opened { await center.refresh() }
    }

    private func merged(_ line: String) {
        notice = line
        if line != ConflictResolution.fixSent { leaveAfterMerge = true }
        Task { await center.refresh() }
    }

    private func leaveIfMerged() {
        guard leaveAfterMerge else { return }
        leave()
    }

    private func leaveAfterChanges() {
        guard leaveAfterMerge else { return }
        leaveAfterMerge = false
        Task {
            try? await Task.sleep(for: .milliseconds(350))
            ShellChrome.shared.leave(dismiss, stack: stackTab)
        }
    }

    private var leave: () -> Void {
        let dismiss: DismissAction = dismiss
        let stack: AppTab? = stackTab
        let flag: Binding<Bool> = $leaveAfterMerge
        return {
            flag.wrappedValue = false
            ShellChrome.shared.leave(dismiss, stack: stack)
        }
    }

    private var live: UnifiedAgentRow { center.rows.first { $0.id == row.id } ?? row }

    private var modelChoices: [AgentModelChoice] {
        guard live.offersModelChoice else { return [] }
        return AgentModelCatalog.followUpChoices(provider: live.provider)
    }

    private func modelOption(_ choice: AgentModelChoice) -> PromptModelOption {
        if choice.provider == live.provider {
            return PromptModelOption(id: choice.modelId, label: choice.label, note: choice.note)
        }
        return PromptModelOption(
            id: choice.modelId, label: choice.label,
            note: "Hands off to \(AgentModelCatalog.runtimeLabel(choice.provider))")
    }

    private var modelId: String {
        if let pickedModel { return pickedModel }
        let current = live.model.lowercased()
        return modelChoices.first { $0.modelId == current || $0.alias == current }?.modelId ?? live.model
    }

    private var modelLabel: String {
        modelId.isEmpty ? "Default model" : AgentModelCatalog.shortLabel(modelId)
    }

    private var runMode: AgentPermissionMode {
        pickedMode ?? AgentPermissionMode.resolve(live.mode, center.desktopDefaultMode)
    }

    private static func modeOption(_ mode: AgentPermissionMode) -> PromptModeOption {
        PromptModeOption(id: mode.rawValue, label: mode.label, note: mode.note, systemImage: mode.systemImage)
    }

    private var retryRun: AgentLaunchSession? {
        if live.isActive || active { return nil }
        return center.retryableRun(live)
    }

    private func retry(_ run: AgentLaunchSession) async {
        busy = true
        defer { busy = false }
        do {
            let outcome = try await Services.require(ApiService.self).retryAgentSession(run.owner, run.repo, run)
            error = ""
            notice = outcome.label
            FmHaptics.commit()
            await center.refresh()
        } catch let failure as AgentLaunchException {
            error = failure.message
            return
        } catch {
            self.error = ControlErrorText.describe(error)
            return
        }
        await load()
    }

    private var canMoveToDesktop: Bool {
        RunnerDesktopHandoff.canMove(live, desktopPaired: center.hub.desktop != nil)
    }

    private var desktopName: String {
        let name = center.hub.currentLink?.desktopName ?? center.hub.desktop?.name ?? ""
        return name.isEmpty ? "your desktop" : name
    }

    private var runnerLabel: String {
        let row = live
        return row.runner.isEmpty ? (row.node.isEmpty ? "the runner" : row.node) : row.runner
    }

    private func moveToDesktop() async {
        movingToDesktop = true
        defer { movingToDesktop = false }
        do {
            let move = try await center.continueOnDesktop(live)
            error = ""
            notice = move.message
            FmHaptics.commit()
            if !move.runnerKept {
                leave()
                return
            }
        } catch {
            self.error = ControlErrorText.describe(error)
            FmHaptics.failure()
        }
        await load()
    }

    private var branchLine: String {
        let row = live
        var text = row.baseBranch.isEmpty ? row.branchName : "\(row.branchName) → \(row.baseBranch)"
        if row.filesChanged >= 0 { text += row.filesChanged == 1 ? " · 1 file" : " · \(row.filesChanged) files" }
        if row.linesAdded > 0 || row.linesRemoved > 0 {
            text += " +\(max(row.linesAdded, 0)) \u{2212}\(max(row.linesRemoved, 0))"
        }
        if row.merged { text += " · merged" }
        return text
    }

    private var deletesOnDesktop: Bool {
        row.outside ? row.localId < 0 : row.isDesktop && row.localId > 0
    }

    private var isRunnerRun: Bool {
        !row.isDesktop && !row.outside && !row.cloudSessionId.isEmpty
    }

    private var deletesHere: Bool {
        if isRunnerRun { return RunnerAgentFeature.api != nil }
        return deletesOnDesktop && AgentOpsFeature.client != nil
    }

    private func deleteNow() {
        var target: UnifiedAgentRow = live
        if active { target.isActive = true }
        guard let request = AgentLineDeleteRequest(AgentLines.line(target)) else { return }
        if request.asksFirst {
            confirmDelete = request
        } else {
            deleteAfterUndoWindow(request)
        }
    }

    private func deleteAfterUndoWindow(_ request: AgentLineDeleteRequest) {
        request.runAfterUndoWindow()
        leave()
    }

    private func follow() async {
        stream?.stop()
        stream = nil
        if let opened = center.stream(row, since: follower.next, onEnded: { _ in ended() }, onDelta: { show($0) }) {
            stream = opened
            if !(await opened.start()) { await load() }
        } else {
            await load()
        }
    }

    private func load() async {
        if streaming { return }
        do {
            show(try await center.transcript(row, since: follower.next))
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }

    private func loadQuestions() async {
        guard row.isDesktop, !row.outside else {
            questions = []
            return
        }
        do {
            questions = try await center.questions(live)
        } catch {
            questions = []
        }
    }

    private func answer(_ question: AgentQuestion, _ text: String) async {
        await perform { try await center.answer(live, question: question, text: text) }
        if error.isEmpty {
            FmHaptics.commit()
            await loadQuestions()
        }
    }

    private func show(_ delta: AgentTranscriptDelta) {
        follower.apply(delta)
        blocks = blockCache.update(follower.text)
        pending.settle(blocks)
        status = delta.status
        queue = delta.queue
        active = delta.active
        error = ""
    }

    private func ended() {
        active = false
        if row.outside { status = "done" }
    }

    private func send() async {
        let text = prompt
        let images = photos
        if text.dartTrimmed.isEmpty && images.isEmpty { return }
        prompt = ""
        photos = []
        notice = ""
        let model = pickedModel.flatMap { $0 == live.model ? nil : $0 } ?? ""
        let provider = pickedModel.flatMap { id in modelChoices.first { $0.modelId == id }?.provider } ?? ""
        let mode = pickedMode?.rawValue ?? ""
        let echo = pending.add(promptOrPhotoNote(text, images), shown: blocks)
        await perform {
            try await center.prompt(live, text, images: images, model: model, mode: mode, provider: provider)
        }
        notice = error.isEmpty ? center.promptNotice : ""
        if error.isEmpty, row.isDesktop, !streaming { await follow() }
        if !error.isEmpty {
            pending.remove(echo)
            if prompt.isEmpty { prompt = text }
            if photos.isEmpty { photos = images }
        }
    }

    private var fixConflictsButton: some View {
        Button {
            Task {
                await perform { try await fixConflicts() }
                if error.isEmpty { notice = ConflictResolution.fixSent }
            }
        } label: {
            Label("Fix conflicts with agent", systemImage: "sparkles")
        }
        .disabled(live.isActive || active || busy)
    }

    private func fixConflicts() async throws {
        let target = live
        try await center.prompt(target, ConflictResolution.sessionPrompt(base: target.baseBranch), images: [])
    }

    private func perform(_ work: () async throws -> Void) async {
        busy = true
        defer { busy = false }
        do {
            try await work()
            error = ""
            await load()
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }
}

private struct AgentRunActionButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        AgentRunActionChip(configuration: configuration)
    }
}

private struct AgentRunActionChip: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.isEnabled) private var isEnabled
    let configuration: ButtonStyleConfiguration

    private var shape: RoundedRectangle { RoundedRectangle(cornerRadius: 9, style: .continuous) }

    var body: some View {
        configuration.label
            .labelStyle(.iconOnly)
            .font(.system(size: 14, weight: .semibold))
            .foregroundStyle(configuration.role == .destructive ? fm.danger : fm.accent)
            .padding(.horizontal, 7)
            .frame(minWidth: 32, minHeight: 32)
            .shellGlass(shape)
            .contentShape(shape)
            .opacity(isEnabled ? (configuration.isPressed ? 0.7 : 1) : 0.4)
            .scaleEffect(configuration.isPressed ? 0.94 : 1)
    }
}

private struct AgentRunActionsCap: ViewModifier {
    static let maxWidth: CGFloat = 184
    @State private var width: CGFloat = 0

    func body(content: Content) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            content
                .fixedSize()
                .onGeometryChange(for: CGFloat.self, of: { $0.size.width }) { (next: CGFloat) in
                    width = next
                }
        }
        .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
        .defaultScrollAnchor(.trailing)
        .frame(width: min(max(width, 1), AgentRunActionsCap.maxWidth), height: 44)
    }
}

private struct AgentRunToolbar<Actions: View, Changes: View>: ViewModifier {
    @ViewBuilder let actions: () -> Actions
    @ViewBuilder let changes: () -> Changes

    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.toolbar {
                actionsItem.sharedBackgroundVisibility(.hidden)
                changesItem.sharedBackgroundVisibility(.hidden)
            }
        } else {
            content.toolbar {
                actionsItem
                changesItem
            }
        }
        #else
        content.toolbar {
            actionsItem
            changesItem
        }
        #endif
    }

    private var actionsItem: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            actions()
        }
    }

    private var changesItem: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            changes()
        }
    }
}

enum RunnerSheet: String, Identifiable {
    case cleanup, update

    var id: String { rawValue }
}

private struct AgentQuestionCards: View {
    let questions: [AgentQuestion]
    let busy: Bool
    let submit: (AgentQuestion, String) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            ForEach(questions) { question in
                AgentQuestionCard(question: question, busy: busy, submit: submit)
            }
        }
    }
}

private struct AgentQuestionCard: View {
    @Environment(\.fmPalette) private var fm
    let question: AgentQuestion
    let busy: Bool
    let submit: (AgentQuestion, String) -> Void
    @State private var selections: [String: Set<String>] = [:]
    @State private var others: [String: String] = [:]

    private func selected(_ field: AgentQuestionField, _ label: String) -> Bool {
        selections[field.id, default: []].contains(label)
    }

    private func pick(_ field: AgentQuestionField, _ label: String) {
        var values = selections[field.id, default: []]
        if field.multiSelect {
            if values.contains(label) { values.remove(label) } else { values.insert(label) }
        } else {
            values = [label]
            others[field.id] = ""
        }
        selections[field.id] = values
    }

    private var answer: String {
        let lines = question.fields.compactMap { field -> String? in
            var values = Array(selections[field.id, default: []])
            if let other = others[field.id]?.dartTrimmed, !other.isEmpty { values.append(other) }
            guard !values.isEmpty else { return nil }
            let value = values.sorted().joined(separator: ", ")
            return question.fields.count == 1 ? value : "\(field.title) → \(value)"
        }
        guard lines.count == question.fields.count else { return "" }
        return lines.joined(separator: "\n")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Label("Agent needs your input", systemImage: "questionmark.circle")
                .fmText(.labelMedium, color: fm.accent, weight: FmType.emphasis)
            ForEach(question.fields) { field in
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text(field.title)
                        .fmText(.bodyMedium, weight: FmType.emphasis)
                        .fixedSize(horizontal: false, vertical: true)
                    ForEach(field.options) { option in
                        Button { pick(field, option.label) } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Label(option.label, systemImage: selected(field, option.label) ? "checkmark.circle.fill" : "circle")
                                    .fmText(.bodyMedium, color: selected(field, option.label) ? fm.accent : fm.textPrimary)
                                if !option.detail.isEmpty {
                                    Text(option.detail)
                                        .fmText(.bodySmall, color: fm.textSecondary)
                                        .padding(.leading, 24)
                                }
                            }
                            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                            .padding(FmSpace.x2)
                            .background(selected(field, option.label) ? fm.accentSubtle : fm.bgOverlay, in: RoundedRectangle(cornerRadius: FmRadius.md))
                        }
                        .buttonStyle(.plain)
                    }
                    let response = Binding<String>(
                        get: { others[field.id, default: ""] },
                        set: { value in
                            others[field.id] = value
                            if !field.multiSelect, !value.dartTrimmed.isEmpty { selections[field.id] = [] }
                        })
                    Group {
                        if field.isSecret {
                            SecureField("Response", text: response)
                        } else {
                            TextField("Other", text: response)
                        }
                    }
                    .textFieldStyle(.roundedBorder)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .frame(minHeight: 44)

                }
            }
            Button("Send answer") { submit(question, answer) }
                .buttonStyle(.borderedProminent)
                .disabled(answer.isEmpty || busy)
                .accessibilityIdentifier("agent-question-send")
        }
        .padding(12)
        .background(fm.accentSubtle, in: RoundedRectangle(cornerRadius: FmRadius.lg))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.lg).stroke(fm.border, lineWidth: 1))
    }
}
