import FMApi
import FMBase
import FMCode
import SwiftUI

/// A file to open from the explorer or the go-to-file sheet.
struct RepoFileTarget: Hashable {
    let path: String
    let ref: String
    /// Blob id from the tree listing ("" from a search hit: no claim).
    let servedBlobSha: String
}

struct RepoCodeTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoCodeTabModel
    @State private var openedFile: RepoFileTarget?
    @State private var goToFileShown: Bool = false

    init(repo: Repository) {
        _model = State(initialValue: RepoCodeTabModel(client: RepoServices.client, repo: repo))
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(spacing: 0) {
                    RepoExplorerHeader(model: model)
                    content
                    Color.clear.frame(height: FmSpace.x4)
                }
            }
            RepoExplorerPathBar(
                crumbs: model.crumbs,
                canGoUp: model.canGoUp,
                onUp: { Task { await model.up() } },
                onPath: { (path: String) in Task { await model.openDir(path) } },
                onGoToFile: { goToFileShown = true })
        }
        .background(fm.listSurface)
        .task {
            await model.start()
        }
        .navigationDestination(item: $openedFile) { (target: RepoFileTarget) in
            RepoFileScreen(repo: model.repo, path: target.path, ref: target.ref, servedBlobSha: target.servedBlobSha)
                .shellPageClearance()
        }
        .sheet(isPresented: $goToFileShown) {
            RepoGoToFileSheet(
                repo: model.repo,
                ref: model.selectedRef,
                onFileSelected: { (path: String) in
                    goToFileShown = false
                    openedFile = RepoFileTarget(path: path, ref: model.selectedRef, servedBlobSha: "")
                })
        }
    }

    @ViewBuilder private var content: some View {
        if model.tree.loading {
            RepoLoadingCard(label: "Loading files…")
                .padding(FmSpace.x4)
        } else if let message = model.treeErrorMessage {
            RepoErrorCard(message: message, onRetry: { Task { await model.openDir(model.path, refresh: true) } })
                .padding(FmSpace.x4)
        } else if let entries = model.tree.data?.entries, !entries.isEmpty {
            RepoFileList(entries: entries) { (entry: RepoTreeEntry) in
                if entry.isDirectory {
                    Task { await model.openDir(entry.path) }
                } else {
                    openedFile = RepoFileTarget(path: entry.path, ref: model.selectedRef, servedBlobSha: entry.sha)
                }
            }
        } else {
            RepoEmptyCard(message: "No files found in this folder.")
                .padding(FmSpace.x4)
        }
    }
}

/// The description at the root, the branch switcher and
/// the node that served the tree.
struct RepoExplorerHeader: View {
    @Environment(\.fmPalette) private var fm

    let model: RepoCodeTabModel

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if model.showDescription && !model.repo.description.isEmpty {
                Text(model.repo.description)
                    .fmText(.bodyMedium, color: fm.textSecondary, height: 1.35)
                    .lineLimit(2)
                    .truncationMode(.tail)
                    .padding(.bottom, FmSpace.x3)
            }
            HStack(spacing: FmSpace.x2) {
                RepoBranchSelector(model: model)
                if model.sourceLabel != nil {
                    RepoSourceChip(source: model.source)
                }
                Spacer(minLength: 0)
            }
        }
        .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x3, FmSpace.x4, FmSpace.x3))
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct RepoBranchSelector: View {
    @Environment(\.fmPalette) private var fm

    let model: RepoCodeTabModel

    private var pill: some View {
        HStack(spacing: 0) {
            FmIcon("account_tree_outlined", size: 16, color: model.branchPillMuted ? fm.textTertiary : fm.success)
            Text(model.branchPillLabel)
                .fmText(.labelMedium, color: fm.textPrimary)
                .lineLimit(1)
                .truncationMode(.tail)
                .padding(.leading, FmSpace.x2)
            if model.branches.loading {
                FmCircularProgressIndicator(size: 14, strokeWidth: 2, color: fm.textTertiary)
                    .padding(.leading, FmSpace.x1)
            } else {
                FmIcon(model.canSwitchBranch ? "keyboard_arrow_down_rounded" : "lock_outline", size: 17, color: fm.textTertiary)
                    .padding(.leading, FmSpace.x1)
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 36)
        .background(fm.bgOverlay)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
    }

    var body: some View {
        Menu {
            ForEach(model.branchNames, id: \.self) { (name: String) in
                Button(action: { Task { await model.switchRef(name) } }) {
                    if name == model.selectedRef {
                        Label(name, systemImage: "checkmark.circle.fill")
                    } else {
                        Label(name, systemImage: "circle")
                    }
                }
            }
        } label: {
            pill
        }
        .disabled(!model.canSwitchBranch)
        .accessibilityLabel(Text("Switch branch"))
    }
}

struct RepoFileList: View {
    @Environment(\.fmPalette) private var fm

    let entries: [RepoTreeEntry]
    let onOpen: (RepoTreeEntry) -> Void

    private func row(_ entry: RepoTreeEntry) -> some View {
        FmListRow(
            title: entry.name,
            subtitle: RepoCodeTabModel.entryMeta(entry),
            leading: AnyView(FmIconTile(icon: RepoCodeTabModel.entryGlyph(entry), tint: entry.isDirectory ? fm.accent : nil)),
            onTap: { onOpen(entry) }
        )
    }

    var body: some View {
        VStack(spacing: 0) {
            FmHairline()
            RepoDividedRows(entries) { (entry: RepoTreeEntry) in
                row(entry)
            }
        }
    }
}

/// Up arrow, breadcrumb trail kept scrolled to its end,
/// and "Go to file".
struct RepoExplorerPathBar: View {
    @Environment(\.fmPalette) private var fm

    let crumbs: [RepoCrumb]
    let canGoUp: Bool
    let onUp: () -> Void
    let onPath: (String) -> Void
    let onGoToFile: () -> Void

    private var upAction: (() -> Void)? {
        if !canGoUp {
            return nil
        }
        return onUp
    }

    private func crumb(_ item: RepoCrumb) -> some View {
        Button(action: { onPath(item.path) }) {
            Text(item.label)
                .font(.system(size: FmType.small, weight: item.current ? FmType.emphasis : FmType.regular))
                .foregroundStyle(item.current ? fm.textPrimary : fm.textSecondary)
                .lineLimit(1)
                .padding(.horizontal, FmSpace.x1)
                .padding(.vertical, FmSpace.x2)
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08), cornerRadius: FmRadius.sm))
    }

    private var trail: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 0) {
                    ForEach(Array(crumbs.enumerated()), id: \.offset) { (entry: (offset: Int, element: RepoCrumb)) in
                        if entry.offset > 0 {
                            FmIcon("chevron_right", size: 16, color: fm.textDisabled)
                        }
                        crumb(entry.element)
                            .id(entry.offset)
                    }
                }
            }
            .onAppear {
                proxy.scrollTo(crumbs.count - 1, anchor: .trailing)
            }
            .onChange(of: crumbs) { (_: [RepoCrumb], newCrumbs: [RepoCrumb]) in
                proxy.scrollTo(newCrumbs.count - 1, anchor: .trailing)
            }
        }
    }

    var body: some View {
        FmBottomBar(
            actions: [
                AnyView(FmToneButton(label: "Go to file", icon: "search", tone: .neutral, onPressed: onGoToFile)),
            ]
        ) {
            HStack(spacing: 0) {
                FmIconButton(icon: "arrow_back_rounded", label: "Up", size: 20, color: fm.textSecondary, onPressed: upAction)
                trail
            }
        }
    }
}

/// Code search on the selected branch.
struct RepoGoToFileSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: RepoGoToFileModel

    private let onFileSelected: (String) -> Void

    init(repo: Repository, ref: String, onFileSelected: @escaping (String) -> Void) {
        _model = State(initialValue: RepoGoToFileModel(client: RepoServices.client, repo: repo, ref: ref))
        self.onFileSelected = onFileSelected
    }

    private func search() {
        Task { await model.search() }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 0) {
                Text("Go to file")
                    .fmText(.titleLarge, color: fm.textPrimary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmIconButton(icon: "close", label: "Close", onPressed: { dismiss() })
            }
            HStack(spacing: FmSpace.x2) {
                FmTextField(text: $model.query, hint: "Search file names or code...", autofocus: true, onSubmit: search)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .submitLabel(.search)
                FmFilledButton("Search", onPressed: search)
            }
            .padding(.top, FmSpace.x3)
            Text(model.scopeLine)
                .fmText(.bodySmall, color: fm.textTertiary)
                .padding(.top, FmSpace.x2)
        }
        .padding(FmSpace.x4)
    }

    @ViewBuilder private var results: some View {
        if let future = model.results {
            if future.loading {
                FmSkeletonList(rows: 5, label: "Searching files")
            } else if let message = model.errorMessage {
                RepoErrorCard(message: message, onRetry: search)
                    .padding(FmSpace.x4)
            } else if let matches = future.data, !matches.isEmpty {
                matchList(matches)
            } else {
                FmEmptyState(icon: "search_off", title: "No file matches", message: model.noMatchesMessage)
            }
        } else {
            FmEmptyState(
                icon: "search", title: "Search this repository",
                message: "Type at least two characters to find files quickly.")
        }
    }

    private func matchRow(_ match: RepoCodeSearchMatch) -> some View {
        FmListRow(
            title: match.path,
            titleView: AnyView(
                Text(match.path)
                    .fmText(.labelMedium, color: fm.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.tail)),
            subtitle: RepoGoToFileModel.matchSubtitle(match),
            leading: AnyView(FmIconTile(icon: RepoDisplay.fileGlyph(RepoDisplay.baseName(match.path)), size: 34, iconSize: 17)),
            onTap: { onFileSelected(match.path) },
            minHeight: 52,
            padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x2, vertical: FmSpace.x2)
        )
    }

    private func matchList(_ matches: [RepoCodeSearchMatch]) -> some View {
        ScrollView {
            VStack(spacing: 0) {
                ForEach(Array(matches.enumerated()), id: \.offset) { (entry: (offset: Int, element: RepoCodeSearchMatch)) in
                    matchRow(entry.element)
                    if entry.offset != matches.count - 1 {
                        FmRowDivider(indent: 54)
                    }
                }
            }
            .padding(EdgeInsets.fmLTRB(FmSpace.x2, 0, FmSpace.x2, FmSpace.x5))
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            results
                .frame(maxHeight: .infinity, alignment: .top)
        }
        .fmPreGlassBackground(fm.bgRaised)
        .fmSheetStyle(fitsContent: false)
    }
}
