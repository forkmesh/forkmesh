import FMApi
import FMBase
import FMCode
import SwiftUI

enum RepoHubPush: Hashable {
    case section(RepoSection, ref: String?)
    case readme
}

struct RepoDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoDetailModel
    @State private var overview: RepoOverviewModel
    @State private var destination: RepoDetailDestination?
    @State private var pushed: RepoHubPush?
    @State private var composingIssue: Bool = false
    @State private var started: Bool = false

    init(repo: Repository, initialTab: RepoDetailTab = .code, initialTarget: NotificationDeepLink? = nil) {
        _model = State(initialValue: RepoDetailModel(repo: repo, initialTab: initialTab, initialTarget: initialTarget))
        _overview = State(initialValue: RepoOverviewModel(repo: repo, client: RepoServices.overviewClient))
    }

    init(ref: RepoRef) {
        self.init(repo: CodeCurrentRepo.known(ref) ?? ReviewEnv.repository(ref))
    }

    private var repo: Repository { overview.repo }

    private var ref: RepoRef { RepoRef(owner: repo.owner, name: repo.name) }

    private var canEditSettings: Bool {
        RepoSettingsRules.canEdit(repoOwner: repo.owner, account: ReviewEnv.accountName, organizationOwner: false)
    }

    private var destinationShown: Binding<Bool> {
        Binding<Bool>(
            get: { destination != nil },
            set: { (shown: Bool) in
                if !shown {
                    destination = nil
                }
            })
    }

    static let primaryCount: Int = 6

    var body: some View {
        ScrollView {
            let tiles: [RepoOverviewTile] = overview.tiles(canEditSettings: canEditSettings)
            VStack(alignment: .leading, spacing: 0) {
                header
                actionRow
                    .padding(.top, FmSpace.x4)
                sectionGroup(Array(tiles.prefix(RepoDetailScreen.primaryCount)), readme: true)
                    .padding(.top, FmSpace.x5)
                if tiles.count > RepoDetailScreen.primaryCount {
                    sectionGroup(Array(tiles.dropFirst(RepoDetailScreen.primaryCount)), readme: false)
                        .padding(.top, FmSpace.x5)
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas)
        .fmLoading(overview.loading)
        .fmRefreshable {
            await overview.load()
        }
        .onChange(of: overview.counts) { (_: [RepoSection: Int], next: [RepoSection: Int]) in
            ShellCounts.shared.adopt(ref, counts: next)
        }
        .navigationTitle(repo.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            if canEditSettings {
                ToolbarItem(placement: .topBarTrailing) {
                    NavigationLink(value: Route.repoSettings(ref, section: RepoSettingsSection.mirror.rawValue)) {
                        Image(systemName: "arrow.left.arrow.right")
                    }
                    .accessibilityLabel("Mirror to GitHub or GitLab")
                }
            }
            ToolbarItem(placement: .topBarTrailing) {
                RepoActionsMenu(repo: ref, ref: "")
            }
        }
        .navigationDestination(item: $pushed) { (push: RepoHubPush) in
            switch push {
            case let .section(section, branch):
                RepoSectionScreen(repo: ref, section: section, ref: branch, full: repo)
                    .shellPageClearance()
            case .readme:
                RepoFileScreen(repo: repo, path: "README.md", ref: "")
                    .shellPageClearance()
            }
        }
        .navigationDestination(isPresented: destinationShown) {
            destinationView
                .shellPageClearance()
        }
        .navigationDestination(isPresented: $composingIssue) {
            NewIssueScreen(repo: repo)
                .shellPageClearance()
        }
        .task {
            CodeCurrentRepo.remember(repo)
            if started { return }
            started = true
            openInitial()
            if repo.updatedMs == 0, let catalog = try? await ReviewEnv.api.repositories() {
                overview.adopt(catalog)
            }
            await overview.load()
        }
    }

    private func openInitial() {
        if let next = model.takeInitialDestination() {
            destination = next
            return
        }
        guard let target = model.initialTarget else {
            if model.tab != .code, let section = RepoSection(rawValue: model.tab.rawValue) {
                pushed = .section(section, ref: nil)
            }
            return
        }
        if let section = RepoSection(rawValue: target.initialTab.rawValue) {
            pushed = .section(section, ref: target.branch.isEmpty ? nil : target.branch)
        }
    }

    private var header: some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmAvatar(name: repo.displayOwner, size: 40, fontSize: FmType.heading)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: FmSpace.x2) {
                    Text(repo.fullName)
                        .fmText(.titleMedium)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Circle()
                        .fill(ReposCatalog.isOnline(repo) ? fm.success : fm.offline)
                        .frame(width: 8, height: 8)
                        .accessibilityLabel(Text(ReposCatalog.isOnline(repo) ? "Online" : "Offline"))
                }
                if !repo.description.isEmpty {
                    Text(repo.description)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(2)
                }
                RepoWrap(spacing: FmSpace.x3, runSpacing: FmSpace.x1) {
                    RepoMetaItemView(item: RepoMetaItem(icon: "account_tree_outlined", text: repo.defaultBranch))
                    ForEach(ReposCatalog.metaItems(repo).filter { $0.icon != "update" }, id: \.self) { (item: RepoMetaItem) in
                        RepoMetaItemView(item: item)
                    }
                }
                .padding(.top, 2)
            }
        }
    }

    private var actionRow: some View {
        HStack(spacing: FmSpace.x2) {
            RepoHubAction(title: "Agent", symbol: "sparkles", primary: true) {
                AppNavigator.shared?.router.requestCompose(ComposeTarget(repo: ref))
            }
            RepoHubAction(title: "Issue", symbol: "plus.circle") {
                Task {
                    if await AccountFeature.requireAccount(action: "open an issue") {
                        composingIssue = true
                    }
                }
            }
            RepoHubAction(title: "Files", symbol: "folder") {
                pushed = .section(.code, ref: nil)
            }
            CloneToDeviceButton(repo: repo)
                .frame(minWidth: 44, minHeight: 44)
        }
    }

    private func sectionGroup(_ tiles: [RepoOverviewTile], readme: Bool) -> some View {
        VStack(spacing: 0) {
            if readme {
                RepoHubRow(title: "README.md", symbol: "doc.richtext", count: nil, identifier: "repo-hub-readme") {
                    pushed = .readme
                }
            }
            ForEach(Array(tiles.enumerated()), id: \.element.id) { (entry: (offset: Int, element: RepoOverviewTile)) in
                if readme || entry.offset > 0 {
                    FmRowDivider(indent: 52)
                }
                RepoHubRow(
                    title: entry.element.title, symbol: entry.element.symbol, count: entry.element.count,
                    identifier: "repo-hub-\(entry.element.section.rawValue)"
                ) {
                    FmHaptics.selection()
                    pushed = .section(entry.element.section, ref: nil)
                }
            }
        }
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg * 2))
    }

    @ViewBuilder private var destinationView: some View {
        switch destination {
        case .some(.issue(let number)):
            IssueDetailScreen(repo: repo, number: number)
        case .some(.pull(let number)):
            PullDetailScreen(repo: repo, number: number)
        case .some(.discussion(let number)):
            DiscussionDetailScreen(repo: repo, number: number)
        case .some(.commit(let commit)):
            RepoCommitDetailScreen(repo: repo, commit: commit)
        case .none:
            EmptyView()
        }
    }
}

struct RepoHubRow: View {
    @Environment(\.fmPalette) private var fm

    let title: String
    let symbol: String
    let count: Int?
    let identifier: String
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: FmSpace.x3) {
                Image(systemName: symbol)
                    .font(.system(size: 17, weight: .regular))
                    .foregroundStyle(fm.textSecondary)
                    .frame(width: 24)
                Text(title)
                    .fmText(.bodyLarge, height: FmType.leadingTight)
                    .lineLimit(1)
                Spacer(minLength: FmSpace.x2)
                if let count = count {
                    Text(verbatim: RepoHubRow.countText(count))
                        .fmText(.bodyLarge, color: fm.textTertiary, height: FmType.leadingTight, tabular: true)
                }
                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(fm.textDisabled)
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(minHeight: 48)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06)))
        .accessibilityLabel(Text(count.map { "\(title), \($0)" } ?? title))
        .accessibilityIdentifier(identifier)
    }

    static func countText(_ count: Int) -> String {
        count > 999 ? "999+" : "\(count)"
    }
}

struct RepoHubAction: View {
    @Environment(\.fmPalette) private var fm

    let title: String
    let symbol: String
    var primary: Bool = false
    let action: () -> Void

    var body: some View {
        Button {
            FmHaptics.tap()
            action()
        } label: {
            HStack(spacing: 6) {
                Image(systemName: symbol)
                    .font(.system(size: 14, weight: .semibold))
                Text(title)
                    .fmText(.labelMedium, color: primary ? fm.onPrimary : fm.textPrimary)
                    .lineLimit(1)
            }
            .foregroundStyle(primary ? fm.onPrimary : fm.textPrimary)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(primary ? fm.accent : fm.bgRaised, in: Capsule())
        }
        .buttonStyle(.plain)
    }
}

@MainActor
enum CodeCurrentRepo {
    static var memory: CurrentRepoMemory { CurrentRepoMemory(store: Services.get(KeyValueStore.self)) }

    private static var records: [String: Repository] = [:]

    static func remember(_ repo: Repository) {
        memory.remember(owner: repo.owner, name: repo.name)
        ShellCounts.shared.currentRepo = memory.ref
    }

    static func follow(_ ref: RepoRef) {
        guard ShellCounts.shared.currentRepo != ref else { return }
        memory.remember(owner: ref.owner, name: ref.name)
        ShellCounts.shared.currentRepo = ref
    }

    static func known(_ ref: RepoRef) -> Repository? {
        records[ref.slug.lowercased()]
    }

    static func open(_ repo: Repository) {
        let ref: RepoRef = RepoRef(owner: repo.owner, name: repo.name)
        records[ref.slug.lowercased()] = repo
        guard let router = AppNavigator.shared?.router else { return }
        let route: Route = .repo(ref, section: nil)
        router.remember(route)
        let tab: AppTab = router.selectedTab
        if ShellDetailHost.shared.active,
            ShellPanes.layout(path: router.currentPath, tab: tab, wide: true) == .split(list: nil, detail: router.currentPath)
        {
            ShellDetailHost.shared.clear(tab)
            router.setPath([route], for: tab)
            return
        }
        router.setPath(router.currentPath + [route], for: tab)
    }
}
