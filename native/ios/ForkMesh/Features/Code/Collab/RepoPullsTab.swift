// The Pulls and Discussions tabs, with the "New PR" and "New discussion"
// floating actions and the sheets they open. RepoDetailScreen leaves these two
// actions to the tabs.

import FMApi
import FMBase
import FMCode
import SwiftUI

struct RepoPullsTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoPullsModel
    @State private var composing: Bool = false
    @State private var mergeAll: PullMergeAllFlow

    init(repo: Repository) {
        _model = State(initialValue: RepoPullsModel(repo: repo, api: CollabEnv.api))
        _mergeAll = State(initialValue: PullMergeAllFlow(owner: repo.owner, name: repo.name))
    }

    var body: some View {
        VStack(spacing: 0) {
            CollabNoticeLine(horizontalPadding: FmSpace.x4)
            content
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(fm.listSurface)
        .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) {
            FmFloatingActionButton(icon: "add", label: "New PR", accessibilityLabel: "New PR") {
                Task {
                    if await CollabEnv.requireAccount(NewPullComposer.guestAction) {
                        composing = true
                    }
                }
            }
            .padding(FmSpace.x4)
        }
        .sheet(isPresented: $composing) {
            NewPullSheet(repo: model.repo)
        }
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                PullMergeAllToolbarButton(flow: mergeAll)
            }
        }
        .pullMergeAllConfirmation(mergeAll)
        .task {
            await model.start()
            adoptOpenCount()
        }
    }

    private func adoptOpenCount() {
        guard model.state.value != nil else { return }
        let counts: ShellCounts = ShellCounts.shared
        let ref: RepoRef = RepoRef(owner: model.repo.owner, name: model.repo.name)
        guard counts.knowsCounts(of: ref) else { return }
        counts.adopt(ref, counts: [.pulls: model.count(.open)])
    }

    @ViewBuilder private var content: some View {
        switch model.state {
        case .loading:
            CollabStateScroll {
                CollabLoadingCard()
            }
        case .failed(let message):
            CollabStateScroll {
                CollabEmptyCard(message: message)
            }
        case .ready(let pulls):
            if pulls.isEmpty {
                CollabStateScroll {
                    CollabEmptyCard(message: RepoPullsModel.emptyList)
                }
            } else {
                VStack(spacing: 0) {
                    PullStatusBar(model: model)
                    list
                }
            }
        }
    }

    @ViewBuilder private var list: some View {
        let visible: [PublishedPull] = model.visible
        if visible.isEmpty {
            CollabStateScroll {
                CollabEmptyCard(message: model.noMatch)
            }
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(visible) { (pull: PublishedPull) in
                        pullRow(pull)
                        FmRowDivider(indent: FmSpace.x4 + 26)
                    }
                }
                .padding(.bottom, FmSpace.x4 + 72)
            }
        }
    }

    private func pullRow(_ pull: PublishedPull) -> some View {
        NavigationLink {
            PullDetailScreen(repo: model.repo, pull: pull)
                .shellPageClearance()
        } label: {
            PullCompactRow(pull: pull)
        }
        .buttonStyle(.plain)
    }
}

private struct PullStatusBar: View {
    @Environment(\.fmPalette) private var fm

    let model: RepoPullsModel

    var body: some View {
        InboxGlassGroup {
            HStack(spacing: 6) {
                ForEach(PullStatusFilter.allCases, id: \.self) { (option: PullStatusFilter) in
                    chip(option)
                }
                Spacer(minLength: 0)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func chip(_ option: PullStatusFilter) -> some View {
        let selected: Bool = model.status == option
        return Button {
            FmHaptics.selection()
            model.status = option
        } label: {
            HStack(spacing: 4) {
                Text(option.label)
                Text("\(model.count(option))")
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(selected ? fm.accent : fm.textSecondary)
            }
            .font(.subheadline.weight(selected ? .semibold : .regular))
            .foregroundStyle(fm.textPrimary)
            .padding(.horizontal, 12)
            .frame(height: 32)
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .inboxGlass(Capsule(), tint: selected ? fm.accent : nil, fallback: selected ? fm.accentSubtle : fm.bgRaised)
        .accessibilityIdentifier("pull-status-\(option.rawValue)")
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

private struct PullCompactRow: View {
    @Environment(\.fmPalette) private var fm

    let pull: PublishedPull

    private var tint: Color { fm.collabTone(PullCard.tone(pull)) }

    var body: some View {
        let branches: String = PullCard.branches(pull)
        let snippet: String = PullCard.snippet(pull)
        HStack(alignment: .top, spacing: 10) {
            FmIcon("call_merge", size: 16, color: tint)
                .padding(.top, 2)
            VStack(alignment: .leading, spacing: 3) {
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(pull.title)
                        .fmText(.bodyMedium, weight: FmType.emphasis, height: 1.2)
                        .lineLimit(2)
                        .truncationMode(.tail)
                        .multilineTextAlignment(.leading)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    statusTag
                }
                HStack(spacing: 4) {
                    Text(PullCard.meta(pull))
                        .fmText(.labelSmall, color: fm.textSecondary, tabular: true)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    if pull.signed {
                        FmIcon("verified_outlined", size: 12, color: fm.success)
                            .accessibilityLabel(Text("Signed"))
                    }
                    if let review = PullCard.review(pull) {
                        Text(review)
                            .fmText(.labelSmall, color: review == "Approved" ? fm.success : fm.danger, weight: FmType.emphasis)
                            .lineLimit(1)
                    }
                }
                if !branches.isEmpty {
                    Text(branches)
                        .fmMono(size: 11, color: fm.textTertiary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                if !snippet.isEmpty {
                    Text(snippet)
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
    }

    private var statusTag: some View {
        Text(pull.status.uppercased())
            .font(.system(size: 9, weight: .bold))
            .tracking(0.4)
            .foregroundStyle(tint)
            .padding(.horizontal, 6)
            .frame(height: 16)
            .background(tint.opacity(0.14), in: Capsule())
            .fixedSize()
    }
}

struct RepoDiscussionsTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoDiscussionsModel
    @State private var composing: Bool = false

    init(repo: Repository) {
        _model = State(initialValue: RepoDiscussionsModel(repo: repo, api: CollabEnv.api))
    }

    var body: some View {
        VStack(spacing: 0) {
            CollabNoticeLine(horizontalPadding: FmSpace.x4)
            content
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(fm.listSurface)
        .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) {
            FmFloatingActionButton(icon: "add", label: "New discussion", accessibilityLabel: "New discussion") {
                Task {
                    if await CollabEnv.requireAccount(NewDiscussionComposer.guestAction) {
                        composing = true
                    }
                }
            }
            .padding(FmSpace.x4)
        }
        .sheet(isPresented: $composing) {
            NewDiscussionSheet(repo: model.repo)
        }
        .task {
            await model.start()
        }
    }

    @ViewBuilder private var content: some View {
        switch model.state {
        case .loading:
            CollabStateScroll {
                CollabLoadingCard()
            }
        case .failed(let message):
            CollabStateScroll {
                CollabEmptyCard(message: message)
            }
        case .ready(let discussions):
            if discussions.isEmpty {
                CollabStateScroll {
                    CollabEmptyCard(message: RepoDiscussionsModel.emptyList)
                }
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(discussions) { (discussion: RepoDiscussion) in
                            discussionRow(discussion)
                            FmRowDivider()
                        }
                    }
                    .padding(.bottom, FmSpace.x4 + 72)
                }
            }
        }
    }

    private func discussionRow(_ discussion: RepoDiscussion) -> some View {
        NavigationLink {
            DiscussionDetailScreen(repo: model.repo, discussion: discussion)
                .shellPageClearance()
        } label: {
            CollabRow(
                icon: "forum_outlined",
                tone: .accent,
                title: discussion.title,
                subtitle: DiscussionCard.subtitle(discussion),
                badge: discussion.category)
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Composers

/// showNewPullDialog: title, base and head branches and a unified diff, sent
/// to the signed pull inbox. Closes itself; the inline notice lands on the tab.
struct NewPullSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: NewPullModel
    @State private var busy: Bool = false

    init(repo: Repository) {
        _model = State(initialValue: NewPullModel(repo: repo))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Text(NewPullComposer.title)
                    .fmText(.titleMedium)
                    .padding(.bottom, FmSpace.x2)
                CollabIdentityBar(verb: "Posting")
                FmField(label: "Title") {
                    FmTextField(text: $model.title)
                }
                HStack(alignment: .top, spacing: 10) {
                    FmField(label: "Base branch") {
                        FmTextField(text: $model.base)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled(true)
                    }
                    FmField(label: "Head branch") {
                        FmTextField(text: $model.head)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled(true)
                    }
                }
                FmField(label: "Unified diff (patch)") {
                    FmTextField(text: $model.patch, multiline: true, mono: true)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled(true)
                        .frame(minHeight: 110, alignment: .top)
                }
                FmDialogActions(
                    onCancel: { dismiss() },
                    onConfirm: { submit() },
                    confirmLabel: "Submit",
                    busy: busy)
                    .padding(.top, FmSpace.x2)
            }
            .padding(FmSpace.x5)
        }
        .fmSheetStyle(fitsContent: false)
    }

    private func submit() {
        // Like the dialog, a submit without a title or a patch just closes.
        if !model.ready {
            dismiss()
            return
        }
        busy = true
        let writer: CollabWriter = CollabEnv.writer(model.repo)
        let title: String = model.title
        let base: String = model.base
        let head: String = model.head
        let patch: String = model.patch
        Task {
            let notice: CollabNotice? = await writer.submitPull(title: title, base: base, head: head, patch: patch)
            busy = false
            dismiss()
            CollabMessenger.shared.show(notice)
        }
    }
}

/// showNewDiscussionDialog: title, category and body, sent to the signed
/// discussion inbox. Closes itself; the inline notice lands on the tab.
struct NewDiscussionSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: NewDiscussionModel
    @State private var busy: Bool = false

    init(repo: Repository) {
        _model = State(initialValue: NewDiscussionModel(repo: repo))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(NewDiscussionComposer.title)
                    .fmText(.titleMedium)
                    .padding(.bottom, FmSpace.x1)
                CollabIdentityBar(verb: "Posting")
                FmField(label: "Title") {
                    FmTextField(text: $model.title)
                }
                FmField(label: "Category") {
                    categories
                }
                FmField(label: "Body") {
                    FmTextField(text: $model.body, multiline: true)
                        .frame(minHeight: 110, alignment: .top)
                }
                FmInboxPendingNote()
                FmDialogActions(
                    onCancel: { dismiss() },
                    onConfirm: { submit() },
                    confirmLabel: "Submit",
                    busy: busy)
                    .padding(.top, FmSpace.x2)
            }
            .padding(FmSpace.x5)
        }
        .fmSheetStyle(fitsContent: false)
    }

    private var categories: some View {
        CollabWrap(spacing: FmSpace.x2, runSpacing: FmSpace.x2) {
            ForEach(discussionCategoryLabels.indices, id: \.self) { (index: Int) in
                categoryChip(discussionCategoryLabels[index].key, discussionCategoryLabels[index].label)
            }
        }
    }

    private func categoryChip(_ key: String, _ label: String) -> some View {
        let selected: Bool = model.category == key
        return Button(action: {
            FmHaptics.selection()
            model.category = key
        }) {
            Text(label)
                .fmText(.labelLarge, color: selected ? fm.accent : fm.textSecondary)
                .padding(.horizontal, FmSpace.x3)
                .frame(height: 32)
                .background(selected ? fm.accentSubtle : fm.bgRaised)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .overlay {
                    FmRoundedShape(radius: FmRadius.md).strokeBorder(selected ? fm.accent : fm.border, lineWidth: 1)
                }
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private func submit() {
        if !model.ready {
            dismiss()
            return
        }
        busy = true
        let writer: CollabWriter = CollabEnv.writer(model.repo)
        let title: String = model.title
        let body: String = model.body
        let category: String = model.category
        Task {
            let notice: CollabNotice? = await writer.submitDiscussion(title: title, body: body, category: category)
            busy = false
            dismiss()
            CollabMessenger.shared.show(notice)
        }
    }
}
