import FMApi
import FMBase
import FMCode
import FMDiff
import SwiftUI

struct PullChangesScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var review: DiffReviewModel
    @State private var tab: Int = 0
    @State private var openedFile: DiffFileRoute?

    private let repo: Repository
    private let pull: PublishedPull
    private let commits: [PatchCommit]
    private let additions: Int
    private let deletions: Int

    init(repo: Repository, pull: PublishedPull, files: [DiffFile], commits: [PatchCommit], scope: String) {
        self.repo = repo
        self.pull = pull
        self.commits = commits
        additions = diffAdditions(files)
        deletions = diffDeletions(files)
        _review = State(initialValue: DiffReviewModel(
            files: files, scope: scope, split: DiffViewSettings.shared.split, store: Services.get(KeyValueStore.self)))
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
            }
            .navigationDestination(item: $openedFile) { (route: DiffFileRoute) in
                DiffFileDestination(repo: repo, route: route)
                    .shellPageClearance()
            }
    }

    private var titleBlock: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(PullDetail.changesTitle)
                .fmText(.titleSmall)
                .lineLimit(1)
            Text("#\(pull.number) · \(repo.fullName)")
                .fmText(.bodySmall, color: fm.textTertiary)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    @ViewBuilder private var content: some View {
        if commits.isEmpty {
            filesTab
        } else {
            VStack(spacing: 0) {
                FmSegmentTabBar(
                    tabs: [
                        FmSegmentTab(label: PullDetail.filesTab(review.files.count)),
                        FmSegmentTab(label: PullDetail.commitsTab(commits.count)),
                    ],
                    index: tab,
                    onChanged: { (index: Int) in tab = index })
                if tab == 0 {
                    filesTab
                } else {
                    CollabNoticeLine(horizontalPadding: FmSpace.x4)
                    PullCommitList(repo: repo, commits: commits)
                }
            }
        }
    }

    private var menuActions: [DiffMenuAction] {
        let patch: String = pull.patch
        if patch.isEmpty {
            return []
        }
        return [DiffMenuAction(label: "Copy patch", systemImage: "doc.on.doc") { diffCopy(patch, "Patch") }]
    }

    private var filesTab: some View {
        let pull: PublishedPull = self.pull
        return DiffReviewStream(
            model: review, extra: menuActions, emptyMessage: PullDetail.noPatch,
            fileActions: { (file: DiffFile) -> [DiffMenuAction] in
                if !PullDetail.canEditHead(pull) {
                    return []
                }
                return diffFileActions(file, ref: pull.head, editable: true) { (route: DiffFileRoute) in openedFile = route }
            }
        ) {
            header
        }
    }

    private var header: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(pull.title)
                    .fmText(.titleSmall)
                    .lineLimit(3)
                    .truncationMode(.tail)
                PullBranchRow(pull: pull)
                CollabWrap(spacing: FmSpace.x3, runSpacing: FmSpace.x2) {
                    Text(changedFilesTitle(review.files.count))
                        .fmText(.bodyMedium, color: fm.textSecondary)
                    DiffStatSummary(additions: additions, deletions: deletions)
                        .padding(.vertical, 3)
                }
                if PullDetail.canEditHead(pull) {
                    NavigationLink {
                        BranchDiffScreen(repo: repo, base: pull.base, head: pull.head, title: pull.title)
                            .shellPageClearance()
                    } label: {
                        FmToneLabel(label: PullDetail.compareBranch, icon: "account_tree_outlined", tone: .accent)
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("pull-compare-branch")
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x2)
    }
}

struct PullBranchRow: View {
    @Environment(\.fmPalette) private var fm

    let pull: PublishedPull

    var body: some View {
        if !pull.head.isEmpty || !pull.base.isEmpty {
            CollabWrap(spacing: FmSpace.x1, runSpacing: FmSpace.x1) {
                if !pull.head.isEmpty {
                    FmBranchLabel(name: pull.head)
                    FmIcon("arrow_forward", size: 14, color: fm.textTertiary)
                        .padding(.vertical, 2)
                }
                FmBranchLabel(name: pull.base.isEmpty ? "main" : pull.base, color: fm.textSecondary)
            }
        }
    }
}

struct FmToneLabel: View {
    @Environment(\.fmPalette) private var fm

    let label: String
    let icon: String
    let tone: FmTone

    private var color: Color {
        switch tone {
        case .accent: return fm.accent
        case .success: return fm.success
        case .warning: return fm.warning
        case .danger: return fm.danger
        case .neutral: return fm.textSecondary
        }
    }

    var body: some View {
        HStack(spacing: 6) {
            FmIcon(icon, size: 16, color: color)
            Text(label)
                .fmText(.labelMedium, color: color, height: 1)
                .lineLimit(1)
        }
        .padding(.horizontal, 10)
        .frame(height: 34)
        .background(tone == .neutral ? fm.bgOverlay : color.opacity(fm.dark ? 0.20 : 0.13))
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .contentShape(Rectangle())
    }
}

private struct PullCommitList: View {
    @Environment(\.fmPalette) private var fm

    let repo: Repository
    let commits: [PatchCommit]

    var body: some View {
        let ordered: [PatchCommit] = commits.reversed()
        ScrollView {
            LazyVStack(spacing: 0) {
                ForEach(ordered.indices, id: \.self) { (index: Int) in
                    row(ordered[index])
                    if index != ordered.count - 1 {
                        FmRowDivider()
                    }
                }
            }
            .padding(.top, FmSpace.x2)
            .padding(.bottom, FmSpace.x5)
        }
    }

    private func row(_ commit: PatchCommit) -> some View {
        let files: [DiffFile] = UnifiedDiff.parse(commit.patch)
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
        .accessibilityIdentifier("pull-commit-\(commit.hash)")
    }
}
