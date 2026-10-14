// The Commits tab (History | Branches | Graph, the clone URL footer): the commit
// history timeline, the branches view, and a commit's detail: its summary over
// the same DiffReviewStream a branch diff uses.

import FMApi
import FMBase
import FMCode
import FMControl
import FMDiff
import SwiftUI

/// A commit to open, as the map the detail screen reads.
struct RepoCommitTarget: Identifiable, Hashable {
    let id: UUID = UUID()
    let commit: JSONObject

    static func == (lhs: RepoCommitTarget, rhs: RepoCommitTarget) -> Bool {
        return lhs.id == rhs.id
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }
}

struct RepoCommitsTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoCommitsTabModel
    @State private var marks: RepoSurfaceMarksModel = RepoCommitsTab.surfaceMarks()
    @State private var opened: RepoCommitTarget?
    @State private var notice: FmNotice?

    init(repo: Repository, ref: String? = nil) {
        _model = State(initialValue: RepoCommitsTabModel(client: RepoServices.client, repo: repo, ref: ref))
    }

    private var segments: some View {
        FmSegmentTabBar(
            tabs: [
                FmSegmentTab(label: RepoGitView.history.label, count: model.historyCount),
                FmSegmentTab(label: RepoGitView.branches.label, count: model.branchCount),
                FmSegmentTab(label: RepoGitView.graph.label),
            ],
            index: model.view.rawValue,
            trailing: AnyView(
                FmIconButton(icon: "refresh_rounded", label: "Refresh history", size: 20, color: fm.textSecondary, onPressed: {
                    Task {
                        await model.refresh()
                        if model.view == .graph { await marks.load() }
                    }
                })),
            onChanged: { (index: Int) in
                model.view = RepoGitView(rawValue: index) ?? .history
            })
    }

    @ViewBuilder private var page: some View {
        switch model.view {
        case .history:
            RepoHistoryView(model: model, onOpen: { (commit: JSONObject) in opened = RepoCommitTarget(commit: commit) })
        case .branches:
            RepoBranchesView(model: model, onOpen: { (commit: JSONObject) in opened = RepoCommitTarget(commit: commit) })
        case .graph:
            RepoGraphView(model: model, marks: marks, onOpen: { (commit: JSONObject) in opened = RepoCommitTarget(commit: commit) })
        }
    }

    private var footer: some View {
        FmBottomBar(
            actions: [
                AnyView(
                    FmToneButton(label: "Clone URL", icon: "link_rounded", tone: .accent, tooltip: "Copy clone URL", onPressed: {
                        notice = repoCopy("Clone URL", model.cloneURL)
                    })),
            ]
        ) {
            FmBranchLabel(name: model.branchLabel)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            segments
            page
                .frame(maxHeight: .infinity)
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
            }
            footer
        }
        .background(fm.listSurface)
        .task {
            await model.start()
        }
        .task(id: model.view) {
            if model.view == .graph && !marks.loaded {
                await marks.load()
            }
        }
        .navigationDestination(item: $opened) { (target: RepoCommitTarget) in
            RepoCommitDetailScreen(repo: model.repo, commit: target.commit)
                .shellPageClearance()
        }
    }
}

extension RepoCommitsTab {
    static func surfaceMarks() -> RepoSurfaceMarksModel {
        RepoSurfaceMarksModel(loaders: [
            .mobile: {
                let commit: String = Services.get(AppConfig.self)?.build.commit ?? ""
                return commit.isEmpty ? nil : commit
            },
            .web: {
                guard let api: ApiService = Services.get(ApiService.self),
                    let version: JSONObject = try? await api.serverVersion()
                else { return nil }
                return RepoSurfaceMarksModel.webCommit(version)
            },
            .desktop: {
                guard let hub: DesktopControlHub = HostsFeature.hub, hub.desktop != nil,
                    let status: JSONObject = try? await hub.request("status", [:], timeout: 8)
                else { return nil }
                return RepoSurfaceMarksModel.desktopCommit(status)
            },
        ])
    }
}

struct RepoHistoryView: View {
    let model: RepoCommitsTabModel
    let onOpen: (JSONObject) -> Void

    var body: some View {
        ScrollView {
            if model.commits.loading {
                RepoLoadingCard(label: "Loading commits...")
                    .padding(FmSpace.x4)
            } else if let message = model.commitsErrorMessage {
                RepoErrorCard(message: message, onRetry: { Task { await model.refresh() } })
                    .padding(FmSpace.x4)
            } else if (model.commits.data ?? []).isEmpty {
                RepoEmptyCard(message: "No commits.")
                    .padding(FmSpace.x4)
            } else {
                if let notice = model.refNotice {
                    RepoRefNoticeCard(message: notice)
                        .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x4, FmSpace.x4, 0))
                }
                LazyVStack(spacing: 0) {
                    ForEach(model.commitRows()) { (row: RepoCommitRow) in
                        RepoCommitTimelineRow(row: row, onTap: { onOpen(row.commit) })
                    }
                }
                .padding(.bottom, FmSpace.x4)
            }
        }
    }
}

struct RepoRefNoticeCard: View {
    @Environment(\.fmPalette) private var fm

    let message: String

    var body: some View {
        RepoInfoCard(title: "Default branch shown") {
            Text(message)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(message)
        }
    }
}

/// The default branch first, green with its badge.
struct RepoBranchesView: View {
    @Environment(\.fmPalette) private var fm

    let model: RepoCommitsTabModel
    let onOpen: (JSONObject) -> Void

    private func row(_ item: RepoBranchRow) -> some View {
        let tip: JSONObject? = item.tipCommit
        let badge: AnyView? = item.current ? AnyView(FmStatusBadge(label: "Default", color: fm.success)) : nil
        var action: (() -> Void)? = nil
        if let commit = tip {
            action = { onOpen(commit) }
        }
        return FmListRow(
            title: item.branch.name,
            subtitle: item.subtitle,
            leading: AnyView(FmIconTile(icon: "account_tree_outlined", tint: item.current ? fm.success : nil)),
            trailing: badge,
            onTap: action,
            showChevron: tip != nil,
            titleColor: item.current ? fm.success : nil
        )
    }

    var body: some View {
        ScrollView {
            if model.branches.loading {
                RepoLoadingCard(label: "Loading branches...")
                    .padding(FmSpace.x4)
            } else if let message = model.branchesErrorMessage {
                RepoErrorCard(message: message, onRetry: { Task { await model.refresh() } })
                    .padding(FmSpace.x4)
            } else if model.branchRows.isEmpty {
                RepoEmptyCard(message: "No branches reported for this repository.")
                    .padding(FmSpace.x4)
            } else {
                RepoDividedRows(model.branchRows) { (item: RepoBranchRow) in
                    row(item)
                }
                .padding(.bottom, FmSpace.x4)
            }
        }
    }
}

/// A commit timeline row with its rail node.
struct RepoCommitTimelineRow: View {
    @Environment(\.fmPalette) private var fm

    let row: RepoCommitRow
    let onTap: () -> Void

    private var lineColor: Color {
        return fm.dark ? Color(fmARGB: 0xFF4A4A4D) : Color(fmARGB: 0xFFC6C6C8)
    }

    private var rail: some View {
        let nodeColor: Color = row.isHead ? fm.success : lineColor
        return ZStack {
            VStack(spacing: 0) {
                Rectangle()
                    .fill(row.isFirst ? Color.clear : lineColor)
                    .frame(width: 2)
                Rectangle()
                    .fill(row.isLast ? Color.clear : lineColor)
                    .frame(width: 2)
            }
            Circle()
                .fill(row.isHead ? nodeColor : fm.listSurface)
                .frame(width: 18, height: 18)
                .overlay {
                    if !row.isHead {
                        Circle().strokeBorder(nodeColor, lineWidth: 2)
                    }
                }
        }
        .frame(width: 54)
    }

    private var headBadge: some View {
        Text("HEAD")
            .font(.system(size: FmType.micro, weight: FmType.emphasis))
            .foregroundStyle(fm.success)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
            .background(fm.success.opacity(0.18))
            .clipShape(FmRoundedShape(radius: FmRadius.sm))
    }

    private var hashBadge: some View {
        let blue: Color = Color(fmARGB: 0xFF58A6FF)
        return Text(row.short)
            .font(FmFonts.monoFont(size: FmType.meta, weight: FmType.emphasis))
            .foregroundStyle(blue)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
            .background(blue.opacity(fm.dark ? 0.18 : 0.12))
            .clipShape(FmRoundedShape(radius: FmRadius.sm))
    }

    private var lines: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: 0) {
                if row.isHead {
                    headBadge
                        .padding(.trailing, FmSpace.x2)
                }
                Text(row.subject)
                    .fmText(.titleSmall, color: fm.textPrimary, height: 1.15)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if !row.time.isEmpty {
                    Text(row.time)
                        .fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis)
                        .padding(.leading, FmSpace.x1)
                }
            }
            HStack(spacing: 0) {
                if !row.short.isEmpty {
                    hashBadge
                        .padding(.trailing, FmSpace.x2)
                }
                Text(row.author)
                    .fmText(.bodySmall, color: fm.textSecondary, weight: FmType.emphasis)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(EdgeInsets.fmLTRB(FmSpace.x1, FmSpace.x3, FmSpace.x2, FmSpace.x3))
    }

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 0) {
                rail
                lines
                FmIcon("chevron_right", color: fm.textTertiary)
                    .padding(.trailing, FmSpace.x3)
            }
            .frame(height: 82)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
    }
}

struct RepoCommitDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoCommitDetailModel
    @State private var openedFile: DiffFileRoute?

    init(repo: Repository, commit: JSONObject) {
        _model = State(initialValue: RepoCommitDetailModel(
            client: RepoServices.client, repo: repo, commit: commit, store: Services.get(KeyValueStore.self)))
    }

    private var repoRef: RepoRef { RepoRef(owner: model.repo.owner, name: model.repo.name) }

    private var menuActions: [DiffMenuAction] {
        let hash: String = model.hash
        let patch: String = model.shownDetail.diff
        var actions: [DiffMenuAction] = [
            DiffMenuAction(label: "Copy commit id", systemImage: "number") { diffCopy(hash, "Id") },
        ]
        if !patch.isEmpty {
            actions.append(DiffMenuAction(label: "Copy patch", systemImage: "doc.on.doc") { diffCopy(patch, "Patch") })
        }
        return actions
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            RepoCommitSummaryCard(summary: model.summary, repo: repoRef)
            if model.shownDetail.truncated {
                Text("Diff truncated by the desktop host to keep the mobile response bounded.")
                    .fmText(.bodyMedium, color: fm.warning, weight: FmType.emphasis)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("Diff truncated by the desktop host to keep the mobile response bounded.")
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x2)
    }

    @ViewBuilder private var content: some View {
        if let review = model.review {
            let hash: String = model.hash
            DiffReviewStream(
                model: review, extra: menuActions, emptyMessage: BranchDiff.commitEmpty,
                fileActions: { (file: DiffFile) -> [DiffMenuAction] in
                    diffFileActions(file, ref: hash, editable: false) { (route: DiffFileRoute) in openedFile = route }
                }
            ) {
                header
            }
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    RepoCommitSummaryCard(summary: model.summary, repo: repoRef)
                    if model.loadsDiff {
                        if let message = model.errorMessage {
                            RepoEmptyCard(message: message)
                        } else {
                            RepoLoadingCard(label: "Loading commit diff...")
                        }
                    }
                }
                .padding(20)
            }
        }
    }

    var body: some View {
        content
            .background(fm.canvas)
            .navigationTitle(model.title)
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(item: $openedFile) { (route: DiffFileRoute) in
                DiffFileDestination(repo: model.repo, route: route)
                    .shellPageClearance()
            }
            .task {
                await model.start()
            }
    }
}

struct RepoCommitSummaryCard: View {
    let summary: RepoCommitSummary
    var repo: RepoRef? = nil

    private var chips: [(icon: String, label: String)] {
        var chips: [(icon: String, label: String)] = [("tag", summary.hashLabel)]
        if !summary.author.isEmpty {
            chips.append(("person_outline", summary.author))
        }
        chips.append(("folder_outlined", summary.repoName))
        if let source = summary.sourceLabel {
            chips.append(("dns_outlined", source))
        }
        return chips
    }

    var body: some View {
        RepoInfoCard(title: summary.title) {
            VStack(alignment: .leading, spacing: 16) {
                RepoChipWrap(chips)
                if !summary.body.isEmpty {
                    LinkedText(summary.body, repo: repo)
                        .fmText(.bodyMedium, height: 1.45)
                        .textSelection(.enabled)
                }
            }
        }
    }
}
