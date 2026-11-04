import FMAccount
import FMApi
import FMBase
import FMChat
import FMCode
import FMControl
import SwiftUI

struct ReposScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.shellPaneOpen) private var paneOpen
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
            .onChange(of: model.catalog.loading) { (_: Bool, loading: Bool) in
                guard !loading, model.catalog.error == nil, let repos = model.catalog.data else { return }
                ShellNavigatorCounts.shared.adopt(.repos, count: repos.count)
            }
    }

    private var hasInstance: Bool { settings?.project.hasInstance != false }

    private func reloadNetwork() async {
        if hasInstance {
            await model.reload()
        }
    }

    // MARK: Search, filters, sort

    private var counts: [RepoFilter: Int] {
        let all: [Repository] = model.catalog.data ?? []
        let online: Int = all.filter { ReposCatalog.isOnline($0) }.count
        return [.all: all.count, .online: online, .offline: all.count - online]
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(spacing: FmSpace.x2) {
                Text("Repos")
                    .fmText(.displaySmall)
                    .accessibilityAddTraits(.isHeader)
                Spacer(minLength: 0)
                refreshButton
            }
            searchField
            ReposCatalogControls(
                filter: model.filter,
                sort: model.sort,
                counts: counts,
                onFilterChanged: { (filter: RepoFilter) in model.filter = filter },
                onSortChanged: { (sort: RepoSort) in model.sort = sort })
        }
        .padding(EdgeInsets(top: FmSpace.x2, leading: FmSpace.x4, bottom: FmSpace.x3, trailing: FmSpace.x4))
        .genieKeepsClear()
    }

    private var searchField: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("search", size: 18, color: fm.textTertiary)
            TextField("", text: $model.queryText, prompt: Text("Find a repository").foregroundColor(fm.textTertiary))
                .fmText(.bodyLarge)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .submitLabel(.search)
        }
        .padding(.horizontal, FmSpace.x4)
        .frame(height: 44)
        .shellGlass(Capsule())
    }

    private var refreshButton: some View {
        Button(action: {
            FmHaptics.tap()
            Task { await reloadNetwork() }
            Task { await desktop.load() }
        }) {
            FmIcon("refresh", size: 20, color: fm.textPrimary)
                .frame(width: 44, height: 44)
                .shellGlass(Circle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08), cornerRadius: 22))
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
            .modifier(ReposPinnedHeader(background: fm.listSurface) { header })
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
                    .shellPaneSelected(
                        ShellPanes.opens(
                            paneOpen, owners: [entry.element.owner, entry.element.displayOwner], name: entry.element.name))
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
    @Environment(\.shellPaneOpen) private var paneOpen

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
                FmSectionHeader(title: title, count: model.repos.isEmpty ? nil : model.repos.count)
                    .padding(EdgeInsets(top: FmSpace.x4, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
                if model.repos.isEmpty, let problem = model.problem {
                    Text(problem)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .lineLimit(2)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(problem)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.bottom, FmSpace.x3)
                } else if model.repos.isEmpty {
                    FmSkeletonList(rows: 2, label: "Loading desktop repositories")
                }
                ForEach(rows) { (repo: DesktopRepo) in
                    DesktopRepoRow(repo: repo) { CodeCurrentRepo.open(repo.repository) }
                        .shellPaneSelected(
                            ShellPanes.opens(
                                paneOpen, owners: [repo.repository.owner, repo.repository.displayOwner],
                                name: repo.repository.name))
                    FmRowDivider()
                }
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
            leading: AnyView(
                FmIcon(repo.isPrivate ? "lock_outline" : "desktop_windows_outlined", size: 20, color: fm.textSecondary)
                    .frame(width: 36, height: 36)),
            onTap: onTap,
            minHeight: 52,
            padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x4, vertical: FmSpace.x2)
        )
    }
}

/// The reachability segments, the visibility and sort menu, and the active visibility pill.
struct ReposCatalogControls: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Namespace private var selection

    let filter: RepoFilter
    let sort: RepoSort
    var counts: [RepoFilter: Int] = [:]
    let onFilterChanged: (RepoFilter) -> Void
    let onSortChanged: (RepoSort) -> Void

    static let segments: [RepoFilter] = [.all, .online, .offline]
    static let visibility: [RepoFilter] = [.public, .private]

    private func pick(_ option: RepoFilter) {
        guard option != filter else { return }
        withAnimation(reduceMotion ? nil : .snappy) {
            onFilterChanged(option)
        }
    }

    private func segment(_ option: RepoFilter) -> some View {
        let selected: Bool = option == filter
        return Button(action: { pick(option) }) {
            HStack(spacing: 5) {
                Text(option.label)
                    .fmText(.labelLarge, color: selected ? fm.textPrimary : fm.textSecondary)
                if let count = counts[option] {
                    Text(String(count))
                        .fmText(.bodyLarge, color: fm.textTertiary, height: FmType.leadingTight, tabular: true)
                        .contentTransition(.numericText())
                }
            }
            .lineLimit(1)
            .frame(maxWidth: .infinity, minHeight: 36)
            .background {
                if selected {
                    Capsule()
                        .fill(fm.bgRaised)
                        .shadow(color: Color.black.opacity(fm.dark ? 0.3 : 0.07), radius: 3, y: 1)
                        .matchedGeometryEffect(id: "segment", in: selection)
                }
            }
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(option.label))
        .accessibilityValue(Text(counts[option].map { "\($0) repositories" } ?? ""))
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private var segments: some View {
        HStack(spacing: 0) {
            ForEach(ReposCatalogControls.segments, id: \.self) { (option: RepoFilter) in
                segment(option)
            }
        }
        .padding(4)
        .shellGlass(Capsule())
        .sensoryFeedback(.selection, trigger: filter)
    }

    private var visibilityActive: Bool { ReposCatalogControls.visibility.contains(filter) }

    private var filterMenu: some View {
        Menu {
            Section("Visibility") {
                Button { pick(.all) } label: {
                    Label("Any", systemImage: visibilityActive ? "circle" : "checkmark")
                }
                ForEach(ReposCatalogControls.visibility, id: \.self) { (option: RepoFilter) in
                    Button { pick(option) } label: {
                        Label(option.label, systemImage: option == filter ? "checkmark" : (option == .private ? "lock" : "globe"))
                    }
                }
            }
            Section("Sort by") {
                ForEach(RepoSort.allCases, id: \.self) { (option: RepoSort) in
                    Button(action: { onSortChanged(option) }) {
                        if option == sort {
                            Label(option.menuLabel, systemImage: "checkmark")
                        } else {
                            Text(option.menuLabel)
                        }
                    }
                }
            }
        } label: {
            Image(systemName: "line.3.horizontal.decrease")
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(fm.textPrimary)
                .frame(width: 44, height: 44)
                .overlay(alignment: .topTrailing) {
                    if visibilityActive {
                        Circle().fill(fm.accent).frame(width: 8, height: 8).offset(x: -9, y: 9)
                    }
                }
                .shellGlass(Circle())
        }
        .accessibilityLabel(Text("Filter and sort repositories"))
        .accessibilityValue(Text(visibilityActive ? "\(filter.label), \(sort.buttonLabel)" : sort.buttonLabel))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                segments
                filterMenu
            }
            if visibilityActive {
                Button { pick(.all) } label: {
                    HStack(spacing: FmSpace.x1 + 2) {
                        Text(filter.label)
                            .fmText(.labelMedium)
                        Image(systemName: "xmark")
                            .font(.system(size: 10, weight: .bold))
                            .foregroundStyle(fm.textSecondary)
                    }
                    .padding(.horizontal, FmSpace.x3)
                    .frame(height: 30)
                    .background(fm.bgRaised, in: Capsule())
                    .frame(minHeight: 44)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Showing \(filter.label.lowercased()) repositories. Clear"))
                .transition(.opacity.combined(with: .scale(scale: 0.9, anchor: .leading)))
            }
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
            (Text(repo.displayOwner + "/").foregroundColor(fm.textTertiary) + Text(repo.name).foregroundColor(fm.textPrimary))
                .fmText(.titleSmall)
                .lineLimit(1)
                .truncationMode(.middle)
            Circle()
                .fill(online ? fm.success : fm.offline)
                .frame(width: 8, height: 8)
                .accessibilityLabel(Text(online ? "Online" : "Offline"))
        }
    }

    private var meta: String {
        ReposCatalog.metaItems(repo).map(\.text).joined(separator: " · ")
    }

    private var subtitleView: some View {
        VStack(alignment: .leading, spacing: 2) {
            if !repo.description.isEmpty {
                Text(repo.description)
                    .fmText(.bodyMedium, color: fm.textPrimary.opacity(0.82))
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            Text(meta)
                .fmText(.bodySmall, color: fm.textTertiary, tabular: true)
                .lineLimit(1)
        }
    }

    var body: some View {
        FmListRow(
            title: repo.fullName,
            titleView: AnyView(titleView),
            subtitleView: AnyView(subtitleView),
            leading: AnyView(FmAvatar(name: repo.displayOwner, size: 36, fontSize: FmType.body)),
            trailing: localBadge,
            onTap: onTap,
            minHeight: 60,
            padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x4, vertical: FmSpace.x3)
        )
        .contextMenu {
            if !repo.description.isEmpty {
                Text(repo.description)
            }
            Button { onTap() } label: { Label("Open", systemImage: "arrow.up.right") }
        } preview: {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text(repo.fullName).fmText(.titleMedium)
                if !repo.description.isEmpty {
                    Text(repo.description).fmText(.bodyLarge, color: fm.textSecondary)
                }
                Text(meta).fmText(.bodySmall, color: fm.textTertiary)
            }
            .padding(FmSpace.x4)
            .frame(width: 320, alignment: .leading)
            .background(fm.bgRaised)
        }
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

private struct ReposPinnedHeader<Header: View>: ViewModifier {
    let background: Color
    @ViewBuilder let header: () -> Header

    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            content
                .safeAreaBar(edge: .top, spacing: 0, content: header)
                .scrollEdgeEffectStyle(.hard, for: .top)
        } else {
            content.safeAreaInset(edge: .top, spacing: 0) {
                header().background(background)
            }
        }
    }
}
