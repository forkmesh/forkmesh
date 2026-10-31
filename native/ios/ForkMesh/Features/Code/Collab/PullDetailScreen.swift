// A pull request and what only it uses: the changed files card with its diff
// stats, the timeline, the merge card, the desktop requests and the review actions
// with the review composer. State: FMCode's PullDetailModel and PullMergeModel.

import FMAgents
import FMApi
import FMBase
import FMCode
import FMControl
import FMCrypto
import SwiftUI

struct PullDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: PullDetailModel
    @State private var showComment: Bool = false
    @State private var commentText: String = ""
    @State private var composer: PullReviewComposerModel?

    /// Opened by number (a deep link or a notification).
    init(repo: Repository, number: Int) {
        _model = State(initialValue: PullDetailModel(
            repo: repo, number: number, api: CollabEnv.api, account: { CollabEnv.account() },
            identity: CollabEnv.identity))
    }

    /// Opened from a list that already holds the pull request.
    init(repo: Repository, pull: PublishedPull) {
        _model = State(initialValue: PullDetailModel(
            repo: repo, number: pull.number, initial: pull, api: CollabEnv.api, account: { CollabEnv.account() },
            identity: CollabEnv.identity))
    }

    private var repo: Repository { model.repo }

    private var composerShown: Binding<Bool> {
        Binding<Bool>(
            get: { composer != nil },
            set: { (shown: Bool) in
                if !shown {
                    composer = nil
                }
            })
    }

    var body: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .background(fm.canvas)
            .navigationTitle(model.title)
            .navigationBarTitleDisplayMode(.inline)
            .task {
                await model.start()
            }
            .task {
                await DesktopCommandAvailability.shared.refresh(
                    owner: model.repo.owner, name: model.repo.name, probe: CollabEnv.api)
            }
            .fmDialog(isPresented: $showComment) {
                commentPrompt
            }
            .fmDialog(isPresented: composerShown, barrierDismissible: false) {
                composerDialog
            }
    }

    @ViewBuilder private var content: some View {
        if let pull = model.pull {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    headerCard(pull)
                    changesCard(pull)
                    timelineCard(pull)
                    if model.showsFederatedReplies {
                        FederatedRepliesCard(repo: repo, kind: "pull", number: pull.number)
                    }
                    PullMergeCard(
                        repo: repo,
                        pull: pull,
                        onMerged: { refresh() })
                        .id("\(pull.number)-\(pull.status)")
                    CollabNoticeLine()
                    reviewActions(pull)
                    if DesktopCommandAvailability.shared.available && !model.offeredShipCommands.isEmpty {
                        shipControls(pull)
                    }
                }
                .padding(20)
            }
            .fmRefreshable {
                await model.reload()
            }
        } else if let error = model.loadError {
            CollabStateScroll {
                CollabEmptyCard(message: error)
            }
        } else {
            CollabStateScroll {
                CollabLoadingCard()
            }
        }
    }

    // MARK: Cards

    private func headerCard(_ pull: PublishedPull) -> some View {
        let state: PullState = PullState(pull)
        let summary: String = PullState.summary(pull, commits: model.commits.count)
        return CollabInfoCard(title: pull.title) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: FmSpace.x2) {
                    PullStatePill(state: state)
                    Text("#\(pull.number)")
                        .fmText(.bodyMedium, color: fm.textTertiary)
                    if pull.signed {
                        CollabChipView(chip: CollabChip("verified_outlined", "signed"))
                    }
                }
                Text(summary)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(summary)
                    .padding(.top, 10)
                PullBranchRow(pull: pull)
                    .padding(.top, 10)
                if state == .open {
                    FmInboxPendingNote()
                        .padding(.top, 10)
                }
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("pull-header")
    }

    @ViewBuilder private func changesCard(_ pull: PublishedPull) -> some View {
        if !model.diffFiles.isEmpty {
            NavigationLink {
                changesScreen(pull)
                    .shellPageClearance()
            } label: {
                PatchFilesCard(
                    files: model.patchFiles, commits: model.commits.count, footer: PullDetail.reviewChanges)
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("pull-files-changed")
        } else if PullDetail.canEditHead(pull) {
            CollabInfoCard(title: PullDetail.changesTitle) {
                NavigationLink {
                    BranchDiffScreen(repo: repo, base: pull.base, head: pull.head, title: pull.title)
                        .shellPageClearance()
                } label: {
                    FmToneLabel(label: PullDetail.compareBranch, icon: "difference_outlined", tone: .accent)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("pull-compare-branch")
            }
        } else {
            CollabInfoCard(title: PullDetail.changesTitle) {
                Text(model.refreshing ? PullDetail.loadingChanges : PullDetail.noPatch)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(model.refreshing ? nil : PullDetail.noPatch)
            }
        }
    }

    private func changesScreen(_ pull: PublishedPull) -> PullChangesScreen {
        PullChangesScreen(
            repo: repo, pull: pull, files: model.diffFiles, commits: model.commits, scope: model.reviewScope)
    }

    private func timelineCard(_ pull: PublishedPull) -> some View {
        let events: [PullEvent] = PullTimeline.events(pull)
        return CollabInfoCard(title: PullDetail.conversationTitle) {
            VStack(spacing: 10) {
                openingComment(pull)
                ForEach(events.indices, id: \.self) { (index: Int) in
                    timelineTile(events[index])
                }
                if events.isEmpty {
                    Text(PullDetail.timelineEmpty)
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
        .accessibilityIdentifier("pull-timeline")
    }

    private func openingComment(_ pull: PublishedPull) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                FmIcon("chat_bubble_outline", size: 18, color: fm.textTertiary)
                Text(PullDetail.openedLine(pull))
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .opensProfile(pull.authorName)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            CollabBodyText(text: PullDetail.body(pull), repo: RepoRef(owner: repo.owner, name: repo.name))
                .padding(.top, FmSpace.x2)
        }
        .padding(12)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
    }

    private func timelineTile(_ event: PullEvent) -> CollabTimelineTile {
        CollabTimelineTile(
            icon: PullTimeline.icon(event),
            tone: PullTimeline.tone(event),
            title: event.displayTitle,
            author: PullTimeline.author(event),
            text: event.body,
            repo: RepoRef(owner: repo.owner, name: repo.name),
            profile: event.authorName)
    }

    private func shipControls(_ pull: PublishedPull) -> some View {
        CollabInfoCard(title: PullDetail.desktopTitle) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(PullDetail.desktopCopy)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                AgentChipRow {
                    ForEach(model.offeredShipCommands.indices, id: \.self) { (index: Int) in
                        shipButton(model.offeredShipCommands[index])
                    }
                }
            }
        }
    }

    private func shipButton(_ command: PullShipCommand) -> some View {
        FmToneButton(
            label: command.chipLabel, icon: command.icon, tone: shipTone(command), tooltip: command.label,
            onPressed: model.isEnabled(command) ? { request(command) } : nil
        )
        .accessibilityIdentifier("pull-ship-\(command.command)")
    }

    private func shipTone(_ command: PullShipCommand) -> FmTone {
        switch command.command {
        case "pull.merge", "pull.merge_delete": return .success
        case "pull.fix_agent", "pull.reopen": return .accent
        case "pull.close", "pull.delete": return .danger
        default: return .neutral
        }
    }

    private func reviewActions(_ pull: PublishedPull) -> some View {
        let reviewable: Bool = PullState.reviewable(pull)
        return CollabInfoCard(title: reviewable ? PullDetail.reviewTitle : PullDetail.commentTitle) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(reviewable ? PullDetail.reviewCopy : PullDetail.doneReviewCopy)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                AgentChipRow {
                    if reviewable {
                        FmToneButton(label: "Approve", icon: "check_circle_outline", tone: .success) {
                            openReview(pull, state: "approve")
                        }
                        .accessibilityIdentifier("pull-review-approve")
                        FmToneButton(label: "Request changes", icon: "cancel_outlined", tone: .danger) {
                            openReview(pull, state: "request-changes")
                        }
                        .accessibilityIdentifier("pull-review-request-changes")
                    }
                    FmToneButton(label: "Comment", icon: "chat_bubble_outline", tone: .neutral) {
                        openComment()
                    }
                    .accessibilityIdentifier("pull-review-comment")
                    if !model.diffFiles.isEmpty {
                        NavigationLink {
                            changesScreen(pull)
                                .shellPageClearance()
                        } label: {
                            FmToneLabel(label: PullDetail.changesTitle, icon: "difference_outlined", tone: .accent)
                        }
                        .buttonStyle(.plain)
                    }
                }
                FmInboxPendingNote()
                Text(CollabCopy.threadSubscriptionNote)
                    .fmText(.bodySmall, color: fm.textTertiary)
                ThreadSubscriptionButton(repo: repo, source: "pull", number: pull.number)
            }
        }
    }

    // MARK: Actions

    private func request(_ command: PullShipCommand) {
        let model: PullDetailModel = self.model
        Task {
            if !(await CollabEnv.requireAccount(DesktopCommand.guestAction)) {
                return
            }
            let message: String = await model.request(command)
            CollabMessenger.shared.show(CollabNotice(message, isError: message != command.successMessage))
        }
    }

    private func refresh() {
        let model: PullDetailModel = self.model
        Task {
            await model.reload()
        }
    }

    private func openComment() {
        Task {
            if await CollabEnv.requireAccount(PullDetail.guestAction) {
                commentText = ""
                showComment = true
            }
        }
    }

    private func openReview(_ pull: PublishedPull, state: String) {
        Task {
            if await CollabEnv.requireAccount(PullDetail.guestAction) {
                composer = PullReviewComposerModel(number: pull.number, state: state)
            }
        }
    }

    private var commentPrompt: some View {
        CollabPromptDialog(
            text: $commentText,
            title: PullReview.commentTitle(model.number),
            multiline: true,
            onCancel: { showComment = false },
            onSubmit: {
                showComment = false
                let writer: CollabWriter = CollabEnv.writer(repo)
                let number: Int = model.number
                let text: String = commentText
                Task {
                    let notice: CollabNotice? = await writer.commentOnPull(number, text)
                    CollabMessenger.shared.show(notice)
                }
            })
    }

    @ViewBuilder private var composerDialog: some View {
        if let composer = composer {
            PullReviewComposer(
                model: composer,
                onCancel: { self.composer = nil },
                onSubmit: { (body: String) in
                    self.composer = nil
                    let writer: CollabWriter = CollabEnv.writer(repo)
                    let number: Int = composer.number
                    let state: String = composer.state
                    Task {
                        let notice: CollabNotice = await writer.review(number, state: state, body: body)
                        CollabMessenger.shared.show(notice)
                    }
                })
        }
    }
}

/// Merge from the phone at pinned commits, for an owner.
private struct PullMergeCard: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: PullMergeModel

    private let onMerged: () -> Void

    init(
        repo: Repository,
        pull: PublishedPull,
        onMerged: @escaping () -> Void
    ) {
        self.onMerged = onMerged
        _model = State(initialValue: PullMergeModel(repo: repo, pull: pull, api: CollabEnv.api))
    }

    var body: some View {
        Group {
            if model.state != .open {
                PullDoneCard(pull: model.pull, state: model.state)
            } else if model.visible {
                CollabInfoCard(title: PullMerge.title) {
                    VStack(alignment: .leading, spacing: 0) {
                        status
                        if !model.message.isEmpty {
                            Text(model.message)
                                .fmText(.bodyMedium, color: model.merged ? fm.success : fm.danger)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .fmMessageTools(model.message)
                                .padding(.top, 10)
                        }
                    }
                }
            }
        }
        .task {
            await model.load()
        }
        .fmDialog(isPresented: $model.offerReviewWaiver) {
            FmConfirmDialog(
                title: PullMerge.waiverTitle,
                message: PullMerge.waiverCopy,
                confirmLabel: PullMerge.waiverConfirm,
                onCancel: { model.offerReviewWaiver = false },
                onConfirm: {
                    model.offerReviewWaiver = false
                    merge(overrideReview: true)
                })
        }
    }

    @ViewBuilder private var status: some View {
        if model.loading {
            Text(PullMerge.checking)
                .fmText(.bodyMedium, color: fm.textPrimary)
        } else if model.merged {
            Text(PullMerge.merged)
                .fmText(.bodyMedium, color: fm.success)
        } else if !model.authorized {
            Text(PullMerge.notOwner(model.repo.displayOwner))
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(PullMerge.notOwner(model.repo.displayOwner))
        } else if let plan = model.plan {
            planView(plan)
        } else {
            Text(PullMerge.notPinned)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(PullMerge.notPinned)
        }
    }

    private func planView(_ plan: PullMergePlan) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(PullMerge.planCopy(plan, model.pull))
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(PullMerge.planCopy(plan, model.pull))
            if let conflicts = PullMerge.conflicts(plan) {
                Text(conflicts)
                    .fmText(.bodyMedium, color: fm.warning)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(conflicts)
            }
            AgentChipRow {
                FmToneButton(
                    label: PullMerge.mergeLabel(busy: model.busy), icon: "call_merge", tone: .success, filled: true,
                    onPressed: mergeAction
                )
                .accessibilityIdentifier("pull-merge")
                FmToneButton(
                    label: PullMerge.mergeDeleteLabel, icon: "delete_sweep_outlined", tone: .success,
                    onPressed: mergeDeleteAction
                )
                .accessibilityIdentifier("pull-merge-delete")
                FmToneButton(label: PullMerge.recheck, icon: "refresh", onPressed: recheckAction)
                if !plan.clean || !plan.conflicts.isEmpty {
                    PullConflictFixButton(repo: model.repo, pull: model.pull, files: plan.conflicts)
                }
            }
            .padding(.top, FmSpace.x1)
        }
    }

    private var mergeAction: (() -> Void)? {
        if !model.canMerge {
            return nil
        }
        return { merge(overrideReview: false) }
    }

    private var mergeDeleteAction: (() -> Void)? {
        if !model.canMerge { return nil }
        return { merge(overrideReview: false, deleteHead: true) }
    }

    private var recheckAction: (() -> Void)? {
        if model.busy {
            return nil
        }
        let model: PullMergeModel = self.model
        return {
            Task {
                await model.load()
            }
        }
    }

    private func merge(overrideReview: Bool, deleteHead: Bool = false) {
        let model: PullMergeModel = self.model
        Task {
            if !(await CollabEnv.requireAccount(PullMerge.guestAction)) {
                return
            }
            if let notice = await model.merge(overrideReview: overrideReview, deleteHead: deleteHead) {
                CollabMessenger.shared.show(CollabNotice(notice, isError: !model.merged))
            }
            if model.merged {
                FmHaptics.commit()
                onMerged()
            }
        }
    }
}

private struct PullStatePill: View {
    @Environment(\.fmPalette) private var fm

    let state: PullState

    var body: some View {
        let color: Color = fm.collabTone(state.tone)
        HStack(spacing: 6) {
            FmIcon(state.icon, size: 16, color: fm.onPrimary)
            Text(state.label)
                .fmText(.labelLarge, color: fm.onPrimary, height: 1)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 30)
        .background(color)
        .clipShape(Capsule())
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("Status: \(state.label)"))
        .accessibilityIdentifier("pull-state-\(state.rawValue)")
    }
}

private struct PullDoneCard: View {
    @Environment(\.fmPalette) private var fm

    let pull: PublishedPull
    let state: PullState

    var body: some View {
        let merged: Bool = state == .merged
        let copy: String = merged ? PullState.mergedCopy(pull) : PullState.closedCopy
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmIcon(state.icon, size: 22, color: fm.collabTone(state.tone))
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(merged ? PullState.mergedTitle : PullState.closedTitle)
                    .fmText(.titleSmall, color: fm.textPrimary)
                Text(copy)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(copy)
            }
        }
        .padding(FmSpace.x4)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay {
            FmRoundedShape(radius: FmRadius.lg).strokeBorder(fm.collabTone(state.tone), lineWidth: 1)
        }
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier(merged ? "pull-merged-box" : "pull-closed-box")
    }
}

private struct PullConflictFixButton: View {
    let repo: Repository
    let pull: PublishedPull
    let files: [String]

    @State private var fix = ConflictFixModel()

    private var agent: UnifiedAgentRow? {
        guard let center = HostsFeature.agents else { return nil }
        return UnifiedAgentRow.forPull(center.rows, owner: repo.owner, name: repo.name, number: pull.number, head: pull.head)
    }

    var body: some View {
        let target = agent
        if target != nil || !pull.head.isEmpty {
            FmToneButton(
                label: fix.sending ? "Asking…" : "Fix with agent", icon: "smart_toy_outlined", tone: .accent,
                onPressed: action(target))
                .accessibilityIdentifier("pull-fix-conflicts")
                .task { await HostsFeature.agents?.refresh() }
        }
    }

    private func action(_ target: UnifiedAgentRow?) -> (() -> Void)? {
        if fix.sending || fix.sent || target?.isActive == true { return nil }
        return {
            Task {
                let ok = await fix.send {
                    if let target, let center = HostsFeature.agents {
                        try await center.prompt(target, ConflictResolution.sessionPrompt(base: pull.base, files: files), images: [])
                    } else {
                        _ = try await ReviewEnv.api.launchAgentSession(
                            repo.owner, repo.name,
                            prompt: ConflictResolution.agentPrompt(branch: pull.head, base: pull.base, files: files),
                            title: "Resolve conflicts on \(pull.head)", createPr: false)
                    }
                }
                if ok {
                    FmHaptics.commit()
                    CollabMessenger.shared.show(target == nil ? "Agent queued." : ConflictResolution.fixSent)
                } else {
                    FmHaptics.failure()
                    CollabMessenger.shared.show(CollabNotice(fix.errorText, isError: true))
                }
            }
        }
    }
}

/// The pull request review composer dialog.
private struct PullReviewComposer: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: PullReviewComposerModel

    let onCancel: () -> Void
    let onSubmit: (String) -> Void

    var body: some View {
        FmDialog(
            title: model.title,
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    CollabIdentityBar(verb: "Reviewing")
                    Text(model.helper)
                        .fmText(.bodyMedium, color: fm.textPrimary, height: 1.4)
                    FmTextField(text: $model.text, hint: PullReview.reviewHint, multiline: true, autofocus: true)
                        .accessibilityLabel(Text(PullReview.reviewHint))
                        .padding(.top, FmSpace.x1)
                    if !model.errorText.isEmpty {
                        Text(model.errorText)
                            .fmText(.bodySmall, color: fm.danger)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(model.errorText)
                    }
                    FmInboxPendingNote()
                        .padding(.top, FmSpace.x1)
                }
            },
            actions: {
                FmDialogActions(
                    onCancel: onCancel,
                    onConfirm: {
                        if let body = model.confirm() {
                            onSubmit(body)
                        }
                    },
                    confirmLabel: PullReview.submitLabel)
            })
    }
}

/// The files a published patch touches.
struct PatchFilesCard: View {
    @Environment(\.fmPalette) private var fm

    let files: [PatchFile]
    var commits: Int = 0
    var footer: String = ""

    private static let shownRows = 8

    var body: some View {
        let totals: (adds: Int, dels: Int) = patchFileTotals(files)
        let shown: [PatchFile] = footer.isEmpty ? files : Array(files.prefix(Self.shownRows))
        FmCard(padding: EdgeInsets(), radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: FmSpace.x2) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(changedFilesTitle(files.count))
                            .fmText(.titleMedium)
                        if commits > 0 {
                            Text(PullDetail.commitsLine(commits))
                                .fmText(.bodySmall, color: fm.textTertiary)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    if files.count > 1 {
                        PatchStat(adds: totals.adds, dels: totals.dels)
                    }
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.top, FmSpace.x4)
                .padding(.bottom, FmSpace.x3)
                FmHairline()
                ForEach(shown.indices, id: \.self) { (index: Int) in
                    PatchFileRow(file: shown[index])
                    if index != shown.count - 1 {
                        FmRowDivider(indent: 56)
                    }
                }
                if !footer.isEmpty {
                    FmHairline()
                    HStack(spacing: FmSpace.x2) {
                        FmIcon("difference_outlined", size: 18, color: fm.accent)
                        Text(files.count > shown.count ? "\(footer) · \(files.count - shown.count) more" : footer)
                            .fmText(.labelLarge, color: fm.accent)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        FmIcon("chevron_right", size: 20, color: fm.textTertiary)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .frame(minHeight: 48)
                }
            }
            .padding(.bottom, footer.isEmpty ? FmSpace.x1 : 0)
        }
        .contentShape(Rectangle())
    }
}

/// "+12 -3", nothing when both are zero.
private struct PatchStat: View {
    @Environment(\.fmPalette) private var fm

    let adds: Int
    let dels: Int

    var body: some View {
        if adds != 0 || dels != 0 {
            HStack(spacing: 6) {
                Text("+\(adds)")
                    .fmText(.bodySmall, color: fm.success, weight: FmType.emphasis)
                Text("-\(dels)")
                    .fmText(.bodySmall, color: fm.danger, weight: FmType.emphasis)
            }
        }
    }
}

/// The change badge, the directory dimmed before the name,
/// and the non-zero counts.
private struct PatchFileRow: View {
    @Environment(\.fmPalette) private var fm

    let file: PatchFile

    private var pathText: Text {
        Text(file.directory).foregroundColor(fm.textTertiary) + Text(file.name).foregroundColor(fm.textPrimary)
    }

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            FmChangeBadge(kind: file.kind.fmKind, size: 26)
            pathText
                .fmText(.bodyMedium, weight: FmType.emphasis, height: 1.25)
                .lineLimit(2)
                .truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)
            HStack(spacing: 6) {
                if file.adds > 0 {
                    Text("+\(file.adds)")
                        .fmText(.bodySmall, color: fm.success, weight: FmType.emphasis)
                }
                if file.dels > 0 {
                    Text("-\(file.dels)")
                        .fmText(.bodySmall, color: fm.danger, weight: FmType.emphasis)
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .frame(minHeight: 52)
    }
}
