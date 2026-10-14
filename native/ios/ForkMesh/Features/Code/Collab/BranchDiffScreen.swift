// BranchDiffScreen (what an agent's branch changes: the net diff against its
// base and the commits that got it there, from the mirror's compare endpoint),
// its comparison header and commit list, and PatchCommitScreen. The files are
// shown in DiffReviewStream. State: FMCode's BranchDiffModel.

import FMApi
import FMBase
import FMCode
import FMDiff
import SwiftUI

struct BranchDiffScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: BranchDiffModel
    @State private var tab: Int = 0
    @State private var composing: Bool = false
    @State private var openedFile: DiffFileRoute?

    /// `base` "" compares against the repository's default branch; `title`
    /// is shown under the branch name, e.g. the agent task that made it.
    init(repo: Repository, base: String, head: String, title: String = "") {
        let threads: BranchReviewSession = BranchReviewSession(
            owner: repo.owner, name: repo.name, branch: head, reader: ReviewEnv.api,
            writer: ReviewEnv.writer(owner: repo.owner, name: repo.name))
        _model = State(initialValue: BranchDiffModel(
            repo: repo, head: head, base: base, title: title, api: CollabEnv.api, store: Services.get(KeyValueStore.self),
            threads: threads))
    }

    var body: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .background(fm.canvas)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .principal) {
                    titleBlock
                }
                ToolbarItemGroup(placement: .topBarTrailing) {
                    FmIconButton(label: "Refresh", onPressed: model.loading ? nil : { reload() }) {
                        FmIcon("refresh", size: 22, color: model.loading ? fm.textDisabled : fm.textPrimary)
                    }
                    DiffViewMenu(extra: menuActions)
                }
            }
            .task {
                await model.start()
            }
            .sheet(isPresented: $composing) {
                if let threads = model.threads {
                    BranchCommentSheet(session: threads)
                }
            }
            .navigationDestination(item: $openedFile) { (route: DiffFileRoute) in
                DiffFileDestination(repo: model.repo, route: route)
                    .shellPageClearance()
            }
            .bareShellTop()
    }

    private var titleBlock: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(model.head)
                .fmText(.titleSmall)
                .lineLimit(1)
                .truncationMode(.tail)
            Text(model.repo.fullName)
                .fmText(.bodySmall, color: fm.textTertiary)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    private var menuActions: [DiffMenuAction] {
        guard let patch = model.comparison?.raw.patch, !patch.isEmpty else {
            return []
        }
        return [DiffMenuAction(label: "Copy patch", systemImage: "doc.on.doc") { diffCopy(patch, "Patch") }]
    }

    private func reload() {
        let model: BranchDiffModel = self.model
        Task {
            await model.load()
        }
    }

    @ViewBuilder private var content: some View {
        if let message = model.errorMessage {
            errorView(message)
        } else if let comparison = model.comparison {
            VStack(spacing: 0) {
                FmSegmentTabBar(
                    tabs: [
                        FmSegmentTab(label: BranchDiff.filesTab(comparison)),
                        FmSegmentTab(label: BranchDiff.commitsTab(comparison)),
                    ],
                    index: tab,
                    onChanged: { (index: Int) in tab = index })
                if tab == 0 {
                    filesTab(comparison)
                } else {
                    CollabNoticeLine(horizontalPadding: FmSpace.x4)
                    BranchCommitList(repo: model.repo, comparison: comparison)
                        .fmRefreshable {
                            await model.load()
                        }
                }
            }
        } else {
            FmSkeletonList(label: BranchDiff.loadingLabel)
                .padding(FmSpace.x4)
        }
    }

    private func errorView(_ message: String) -> some View {
        ScrollView {
            VStack(spacing: 0) {
                FmEmptyState(icon: "difference_outlined", title: BranchDiff.errorTitle, message: message)
                    .frame(minHeight: 280)
                FmOutlinedButton(BranchDiff.retry, icon: "refresh", onPressed: model.loading ? nil : { reload() })
            }
        }
        .fmRefreshable {
            await model.load()
        }
    }

    @ViewBuilder private func filesTab(_ comparison: BranchComparison) -> some View {
        if let review = model.review {
            let model: BranchDiffModel = self.model
            DiffReviewStream(
                model: review, emptyMessage: model.emptyMessage, refresh: { await model.load() }, inline: inlineReview,
                fileActions: { (file: DiffFile) -> [DiffMenuAction] in
                    diffFileActions(file, ref: model.head, editable: true) { (route: DiffFileRoute) in openedFile = route }
                }
            ) {
                ComparisonHeader(model: model, comparison: comparison)
            }
            .safeAreaInset(edge: .bottom) {
                selectionBar
            }
        }
    }

    private var inlineReview: DiffInlineReview? {
        guard let threads = model.threads else {
            return nil
        }
        return DiffInlineReview(session: threads, repo: RepoRef(owner: model.repo.owner, name: model.repo.name))
    }

    @ViewBuilder private var selectionBar: some View {
        if let threads = model.threads, let selection = threads.selection {
            HStack(spacing: FmSpace.x3) {
                Text(selection.label)
                    .fmMono(color: fm.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button("Clear") {
                    FmHaptics.selection(.close)
                    threads.clearSelection()
                }
                .accessibilityIdentifier("branch-comment-clear")
                Button("Comment") {
                    composing = true
                }
                .buttonStyle(.borderedProminent)
                .disabled(!threads.canComment)
                .accessibilityIdentifier("branch-comment-open")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
            .shellGlass(Capsule())
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
        }
    }
}

/// The task, head → base, the counts and the commits.
private struct ComparisonHeader: View {
    @Environment(\.fmPalette) private var fm

    let model: BranchDiffModel
    let comparison: BranchComparison

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                if !model.title.isEmpty {
                    Text(model.title)
                        .fmText(.titleSmall)
                        .lineLimit(3)
                        .truncationMode(.tail)
                }
                CollabWrap(spacing: FmSpace.x1, runSpacing: FmSpace.x1) {
                    FmBranchLabel(name: model.head)
                    FmIcon("arrow_forward", size: 14, color: fm.textTertiary)
                        .padding(.vertical, 2)
                    FmBranchLabel(name: model.base, color: fm.textSecondary)
                }
                CollabWrap(spacing: FmSpace.x3, runSpacing: FmSpace.x2) {
                    Text(BranchDiff.summary(commits: comparison.raw.commitCount, files: comparison.files.count))
                        .fmText(.bodyMedium, color: fm.textSecondary)
                    DiffStatSummary(additions: comparison.additions, deletions: comparison.deletions)
                        .padding(.vertical, 3)
                }
                if let head = BranchDiff.headLine(comparison.raw) {
                    Text(head)
                        .fmMono(color: fm.textTertiary)
                }
                if let threads = model.threads {
                    if !threads.summary.isEmpty {
                        Label(threads.summary, systemImage: "bubble.left.and.bubble.right")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    if threads.canComment {
                        Text(BranchReviewText.commentHint)
                            .fmText(.bodySmall, color: fm.textTertiary)
                    } else if !threads.canWrite && threads.hasBranchHead {
                        Text(BranchReviewText.noSigningKey)
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(BranchReviewText.noSigningKey)
                    }
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x2)
    }
}

private struct BranchCommentSheet: View {
    let session: BranchReviewSession

    @Environment(\.dismiss) private var dismiss
    @State private var text: String = ""
    @State private var error: String?
    @State private var confirmDiscard: Bool = false

    var body: some View {
        NavigationStack {
            Form {
                if let selection = session.selection {
                    Section {
                        Text(selection.label)
                            .font(.footnote.monospaced())
                        let preview: String = session.selectionPreview
                        if !preview.isEmpty {
                            Text(preview)
                                .font(.caption.monospaced())
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                Section("Comment") {
                    TextField("Leave feedback. Markdown works.", text: $text, axis: .vertical)
                        .lineLimit(4...12)
                        .accessibilityIdentifier("branch-comment-body")
                }
                if let error {
                    Text(error)
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            .navigationTitle("Comment")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(!text.isEmpty)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        if text.isEmpty {
                            dismiss()
                        } else {
                            confirmDiscard = true
                        }
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Comment") {
                        submit()
                    }
                    .disabled(session.busy || text.dartTrimmed.isEmpty)
                    .accessibilityIdentifier("branch-comment-send")
                }
            }
            .confirmationDialog("Discard this comment?", isPresented: $confirmDiscard) {
                Button("Discard", role: .destructive) {
                    dismiss()
                }
            }
        }
    }

    private func submit() {
        let session: BranchReviewSession = self.session
        let body: String = text
        Task { @MainActor in
            let result: CollabNotice = await session.comment(body: body)
            if result.isError {
                error = result.message
            } else {
                FmHaptics.commit()
                CollabMessenger.shared.show(result)
                dismiss()
            }
        }
    }
}

/// The branch's commits, newest first.
private struct BranchCommitList: View {
    @Environment(\.fmPalette) private var fm

    let repo: Repository
    let comparison: BranchComparison

    var body: some View {
        let commits: [PatchCommit] = comparison.commits.reversed()
        if commits.isEmpty {
            let empty: (title: String, message: String) = BranchDiff.commitListEmpty(comparison.raw.commitCount)
            ScrollView {
                FmEmptyState(icon: "commit", title: empty.title, message: empty.message)
                    .frame(minHeight: 280)
            }
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(commits.indices, id: \.self) { (index: Int) in
                        commitRow(commits[index])
                        if index != commits.count - 1 {
                            FmRowDivider()
                        }
                    }
                }
                .padding(.top, FmSpace.x2)
                .padding(.bottom, FmSpace.x5)
            }
        }
    }

    private func commitRow(_ commit: PatchCommit) -> some View {
        let files: [DiffFile] = comparison.filesOf(commit)
        return NavigationLink {
            PatchCommitScreen(repo: repo, commit: commit, files: files)
                .shellPageClearance()
        } label: {
            HStack(spacing: FmSpace.x4) {
                FmIcon("commit", size: 24, color: fm.accent)
                VStack(alignment: .leading, spacing: 2) {
                    Text(commit.subject)
                        .fmText(.bodyLarge, color: fm.textPrimary)
                        .lineLimit(2)
                        .truncationMode(.tail)
                        .multilineTextAlignment(.leading)
                    Text(BranchDiff.commitMeta(commit))
                        .fmMono(color: fm.textTertiary)
                        .lineLimit(1)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                DiffStatSummary(additions: diffAdditions(files), deletions: diffDeletions(files), showBar: false)
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x3)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("branch-commit-\(commit.hash)")
    }
}

/// PatchCommitScreen: one commit of the branch, its message and its own files.
struct PatchCommitScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: DiffReviewModel
    @State private var openedFile: DiffFileRoute?

    private let repo: Repository
    private let commit: PatchCommit

    /// `files`: the commit's patch already parsed; parsed here when nil.
    init(repo: Repository, commit: PatchCommit, files: [DiffFile]? = nil) {
        self.repo = repo
        self.commit = commit
        _model = State(initialValue: DiffReviewModel(
            files: files ?? UnifiedDiff.parse(commit.patch), scope: "\(repo.owner)/\(repo.name)/commit/\(commit.hash)",
            split: DiffViewSettings.shared.split, store: Services.get(KeyValueStore.self)))
    }

    private var menuActions: [DiffMenuAction] {
        let commit: PatchCommit = self.commit
        var actions: [DiffMenuAction] = [
            DiffMenuAction(label: "Copy commit id", systemImage: "number") { diffCopy(commit.hash, "Id") },
        ]
        if !commit.patch.isEmpty {
            actions.append(DiffMenuAction(label: "Copy patch", systemImage: "doc.on.doc") { diffCopy(commit.patch, "Patch") })
        }
        return actions
    }

    var body: some View {
        DiffReviewStream(
            model: model, extra: menuActions, emptyMessage: BranchDiff.commitEmpty,
            fileActions: { (file: DiffFile) -> [DiffMenuAction] in
                diffFileActions(file, ref: commit.hash, editable: false) { (route: DiffFileRoute) in openedFile = route }
            }
        ) {
            header
        }
        .background(fm.bgBase)
        .navigationTitle(commit.shortHash)
        .navigationBarTitleDisplayMode(.inline)
        .navigationDestination(item: $openedFile) { (route: DiffFileRoute) in
            DiffFileDestination(repo: repo, route: route)
                .shellPageClearance()
        }
    }

    private var header: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                LinkedText(commit.subject, repo: RepoRef(owner: repo.owner, name: repo.name))
                    .fmText(.titleSmall)
                    .textSelection(.enabled)
                if !commit.body.isEmpty {
                    LinkedText(commit.body, repo: RepoRef(owner: repo.owner, name: repo.name))
                        .fmText(.bodyMedium, color: fm.textSecondary, height: 1.45)
                        .textSelection(.enabled)
                }
                Text(BranchDiff.commitMeta(commit))
                    .fmMono(color: fm.textTertiary)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x2)
    }
}

struct DiffFileRoute: Identifiable, Hashable {
    let path: String
    let ref: String
    let edit: Bool

    var id: String { "\(edit ? "edit" : "view")|\(ref)|\(path)" }
}

func diffFileActions(
    _ file: DiffFile, ref: String, editable: Bool, open: @escaping (DiffFileRoute) -> Void
) -> [DiffMenuAction] {
    guard file.status != .deleted, !ref.isEmpty else {
        return []
    }
    var actions: [DiffMenuAction] = []
    if editable && !file.binary && RepoDisplay.previewKind(file.path) == .text {
        actions.append(DiffMenuAction(label: "Edit file", systemImage: "pencil") {
            open(DiffFileRoute(path: file.path, ref: ref, edit: true))
        })
    }
    actions.append(DiffMenuAction(label: "View file", systemImage: "doc.text") {
        open(DiffFileRoute(path: file.path, ref: ref, edit: false))
    })
    return actions
}

struct DiffFileDestination: View {
    let repo: Repository
    let route: DiffFileRoute

    var body: some View {
        if route.edit {
            QuickEditScreen(repo: repo, path: route.path, ref: route.ref)
        } else {
            RepoFileScreen(repo: repo, path: route.path, ref: route.ref)
        }
    }
}
