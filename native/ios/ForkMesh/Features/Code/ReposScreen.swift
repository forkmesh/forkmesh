import FMAccount
import FMApi
import FMBase
import FMChat
import FMCode
import FMControl
import SwiftUI

struct ReposScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: ReposScreenModel
    @State private var desktop: DesktopReposModel
    @State private var settings: SettingsService? = Services.get(SettingsService.self)

    init() {
        _model = State(initialValue: ReposScreenModel(client: RepoServices.client))
        let hub: DesktopControlHub? = HostsFeature.hub
        _desktop = State(
            initialValue: DesktopReposModel(
                channel: hub, desktopName: hub?.desktop?.name ?? "",
                fallback: { HostsFeature.agents?.desktopSnapshot?.repositories ?? [] }))
    }

    var body: some View {
        catalog
            .task(id: hasInstance) {
                if hasInstance {
                    await model.start()
                }
            }
            .task {
                await desktop.start()
            }
    }

    private var hasInstance: Bool { settings?.project.hasInstance != false }

    private func reloadNetwork() async {
        if hasInstance {
            await model.reload()
        }
    }

    // MARK: Search, filters, sort

    private var header: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(spacing: FmSpace.x2) {
                searchField
                refreshButton
            }
            ReposCatalogControls(
                filter: model.filter,
                sort: model.sort,
                onFilterChanged: { (filter: RepoFilter) in model.filter = filter },
                onSortChanged: { (sort: RepoSort) in model.sort = sort })
        }
        .padding(FmSpace.x4)
    }

    private var searchField: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("search", size: 18, color: fm.textTertiary)
            TextField("", text: $model.queryText, prompt: Text("Find a repository...").foregroundColor(fm.textTertiary))
                .fmText(.bodyLarge)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .submitLabel(.search)
        }
        .padding(.horizontal, FmSpace.x4)
        .frame(height: 48)
        .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
    }

    private var refreshButton: some View {
        Button(action: {
            FmHaptics.tap()
            Task { await reloadNetwork() }
            Task { await desktop.load() }
        }) {
            FmIcon("refresh", size: 24, color: fm.textSecondary)
                .frame(width: 48, height: 48)
                .shellGlass(Circle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08), cornerRadius: 24))
        .accessibilityLabel(Text("Refresh repositories"))
    }

    // MARK: Catalog

    private var catalog: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                LazyVStack(spacing: 0) {
                    TabTopAnchor()
                    DesktopReposSection(model: desktop, query: model.query)
                    LocalReposSection(showsNetwork: hasInstance)
                    if hasInstance {
                        FmSliverFadeIn(stateKey: model.stateKey) {
                            networkSection
                        }
                    }
                }
            }
            .background(fm.listSurface)
            .fmRefreshable {
                await reloadNetwork()
                await desktop.load()
            }
            .safeAreaInset(edge: .top, spacing: 0) {
                header
            }
            .onTabReselect(.code) {
                withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(TabTopAnchor.id, anchor: .top) }
            }
        }
    }

    @ViewBuilder private var networkSection: some View {
        if model.catalog.loading {
            // Placeholders shaped like the rows they become keep the geometry still.
            FmSkeletonList(rows: 6, label: "Loading repositories")
        } else if let error = model.catalog.error {
            ReposErrorView(message: error, onRetry: { Task { await model.reload() } })
                .frame(minHeight: 360)
        } else if model.visibleRepos.isEmpty {
            FmEmptyState(icon: "book_outlined", title: model.emptyTitle, message: model.emptyMessage)
                .frame(minHeight: 360)
        } else {
            repoRows
        }
    }

    private var repoRows: some View {
        let repos: [Repository] = model.visibleRepos
        let store: LocalRepoStore? = LocalRepoServices.store
        return LazyVStack(spacing: 0) {
            ForEach(Array(repos.enumerated()), id: \.offset) { (entry: (offset: Int, element: Repository)) in
                RepoCatalogRow(repo: entry.element, local: store?.record(for: entry.element), onTap: { CodeCurrentRepo.open(entry.element) })
                if entry.offset != repos.count - 1 {
                    FmRowDivider()
                }
            }
        }
        .padding(.bottom, FmSpace.x5)
    }
}

struct DesktopReposSection: View {
    @Environment(\.fmPalette) private var fm

    let model: DesktopReposModel
    let query: String

    private var rows: [DesktopRepo] {
        if query.isEmpty { return model.repos }
        return model.repos.filter { $0.id.contains(query) || $0.description.lowercased().contains(query) }
    }

    private var title: String {
        model.desktopName.isEmpty ? "Desktop" : "Desktop · \(model.desktopName)"
    }

    var body: some View {
        if model.available && (!model.repos.isEmpty || model.problem != nil || model.loading) {
            VStack(alignment: .leading, spacing: 0) {
                FmSectionHeader(title: title, icon: "desktop_windows_outlined", count: model.repos.count)
                    .padding(EdgeInsets(top: FmSpace.x2, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
                if model.repos.isEmpty, let problem = model.problem {
                    Text(problem)
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(problem)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.bottom, FmSpace.x3)
                } else if model.repos.isEmpty {
                    FmSkeletonList(rows: 2, label: "Loading desktop repositories")
                }
                ForEach(rows) { (repo: DesktopRepo) in
                    DesktopRepoRow(repo: repo) { CodeCurrentRepo.open(repo.repository) }
                    FmRowDivider()
                }
                FmHairline()
            }
            .accessibilityIdentifier("repos-desktop-section")
        }
    }
}

struct DesktopRepoRow: View {
    @Environment(\.fmPalette) private var fm

    let repo: DesktopRepo
    let onTap: () -> Void

    private var titleView: some View {
        HStack(spacing: FmSpace.x2) {
            Text(repo.slug)
                .fmText(.titleSmall)
                .lineLimit(1)
                .truncationMode(.middle)
            if repo.runningAgents > 0 {
                FmStatusBadge(label: "\(repo.runningAgents) running", color: fm.success)
            }
        }
    }

    var body: some View {
        FmListRow(
            title: repo.slug,
            titleView: AnyView(titleView),
            subtitle: repo.detail,
            leading: AnyView(FmIconTile(icon: repo.isPrivate ? "lock_outline" : "desktop_windows_outlined", tint: fm.accent, size: 36)),
            onTap: onTap,
            minHeight: 52,
            padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x4, vertical: FmSpace.x2)
        )
    }
}

/// The filter chips and the sort menu.
struct ReposCatalogControls: View {
    @Environment(\.fmPalette) private var fm

    let filter: RepoFilter
    let sort: RepoSort
    let onFilterChanged: (RepoFilter) -> Void
    let onSortChanged: (RepoSort) -> Void

    private func chip(_ option: RepoFilter) -> some View {
        let selected: Bool = option == filter
        return Button(action: {
            FmHaptics.selection()
            onFilterChanged(option)
        }) {
            Text(option.label)
                .fmText(.labelMedium, color: selected ? fm.accent : fm.textSecondary)
                .padding(.horizontal, FmSpace.x3)
                .frame(height: 32)
                .background(selected ? fm.accentSubtle : Color.clear)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                .overlay {
                    FmRoundedShape(radius: FmRadius.md).strokeBorder(selected ? fm.accent : Color.clear, lineWidth: 1)
                }
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private var chips: some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x2) {
                ForEach(RepoFilter.allCases, id: \.self) { (option: RepoFilter) in
                    chip(option)
                }
            }
            // Trailing room so the final chip can scroll clear of the fade.
            .padding(.trailing, FmSpace.x4)
        }
        .frame(height: 36)
    }

    private var sortMenu: some View {
        Menu {
            ForEach(RepoSort.allCases, id: \.self) { (option: RepoSort) in
                Button(action: { onSortChanged(option) }) {
                    if option == sort {
                        Label(option.menuLabel, systemImage: "checkmark")
                    } else {
                        Text(option.menuLabel)
                    }
                }
            }
        } label: {
            HStack(spacing: FmSpace.x1) {
                FmIcon("sort", size: 16, color: fm.textSecondary)
                Text(sort.buttonLabel)
                    .fmText(.labelMedium, color: fm.textSecondary)
            }
            .padding(.horizontal, FmSpace.x3)
            .frame(height: 36)
            .shellGlass(Capsule())
        }
        .accessibilityLabel(Text("Sort repositories"))
    }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            chips
                .frame(maxWidth: .infinity)
            sortMenu
        }
    }
}

struct RepoCatalogRow: View {
    @Environment(\.fmPalette) private var fm

    let repo: Repository
    var local: LocalRepoRecord? = nil
    let onTap: () -> Void

    private var online: Bool { ReposCatalog.isOnline(repo) }

    private var localBadge: AnyView? {
        guard let local = local else { return nil }
        return AnyView(FmStatusBadge(label: local.isCloned ? "Cloned" : "On device", color: fm.accent))
    }

    private var titleView: some View {
        HStack(spacing: FmSpace.x2) {
            Text(repo.fullName)
                .fmText(.titleSmall)
                .lineLimit(2)
                .truncationMode(.tail)
            Circle()
                .fill(online ? fm.success : fm.offline)
                .frame(width: 8, height: 8)
                .accessibilityLabel(Text(online ? "Online" : "Offline"))
        }
    }

    private var subtitleView: some View {
        VStack(alignment: .leading, spacing: 0) {
            if !repo.description.isEmpty {
                Text(repo.description)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .lineLimit(2)
                    .truncationMode(.tail)
                    .padding(.top, 1)
            }
            RepoWrap(spacing: FmSpace.x3, runSpacing: FmSpace.x1) {
                ForEach(ReposCatalog.metaItems(repo), id: \.self) { (item: RepoMetaItem) in
                    RepoMetaItemView(item: item)
                }
            }
            .padding(.top, FmSpace.x2)
        }
    }

    var body: some View {
        FmListRow(
            title: repo.fullName,
            titleView: AnyView(titleView),
            subtitleView: AnyView(subtitleView),
            leading: AnyView(FmIconTile(icon: repo.isPrivate ? "lock_outline" : "book_outlined", tint: fm.accent, size: 40)),
            trailing: localBadge,
            onTap: onTap,
            minHeight: 64,
            padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x4, vertical: FmSpace.x3)
        )
    }
}

struct RepoMetaItemView: View {
    @Environment(\.fmPalette) private var fm

    let item: RepoMetaItem

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon(item.icon, size: item.isLanguage ? 9 : 14, color: item.isLanguage ? fm.accentEdge : fm.textTertiary)
            Text(item.text)
                .fmText(.bodySmall, color: fm.textTertiary, tabular: true)
        }
        .accessibilityElement(children: .combine)
        .accessibilityHint(Text(item.tooltip ?? ""))
    }
}

struct ReposErrorView: View {
    let message: String
    let onRetry: () -> Void

    var body: some View {
        FmCard(padding: EdgeInsets.fmAll(FmSpace.x4), radius: FmRadius.lg) {
            VStack(spacing: FmSpace.x3) {
                FmEmptyState(icon: "cloud_off", title: "Could not load repositories", message: message)
                FmOutlinedButton("Retry", onPressed: onRetry)
            }
        }
        .padding(FmSpace.x4)
    }
}
