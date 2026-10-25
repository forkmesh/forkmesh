// The home shell: a slim header (the project, the connection line, the account
// overflow), the telemetry strip and the relay banner, the page (one
// NavigationStack per tab: Agents, Inbox, Code, Chat), and at the bottom a
// full-width Liquid Glass carousel: the four pages, Search, Report a bug and the
// hub's links. The prompt button sits in the header left of the avatar and opens
// the composer over the page with the keyboard up; Send starts the agent (or
// sends the chat message) straight away and lands on the run's transcript. A
// tapped push is routed to what it was about.

import FMAccount
import FMAgents
import FMApi
import FMAssistant
import FMBase
import FMCode
import FMCrypto
import FMInsights
import FMNet
import FMPackages
import FMTools
import SwiftUI
import UIKit

@MainActor
struct HomeShell: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    @Environment(\.horizontalSizeClass) var sizeClass
    @Environment(\.scenePhase) var scenePhase
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @SceneStorage("fm.navigation.v1") var savedNavigation: Data = Data()

    @State var model: HomeShellModel
    @State var router: AppRouter
    @State var composer: ShellComposer
    @State var updateCheck: UpdateRequiredCheck = UpdateRequiredCheck.shared
    @State var showContextPicker: Bool = false
    @State var showPalette: Bool = false
    @State var showHub: Bool = false
    @State var showTour: Bool = false
    @State var showProjects: Bool = false
    @State var projectHome: ProjectHome = ProjectHome.shared
    @State var pocket: ShellPocket = ShellPocket.shared
    @State var signInProvider: AgentSignInRequest?
    @State var showDeviceSetup: Bool = false
    @State var showGemmaSetup: Bool = false
    @State var deviceRun: AgentDeviceRun?
    @State var notice: FmNotice?
    @State var page: ShellPage?
    @State var offlineShown: Bool = false
    @State var relayDownSince: Date?
    @State var homeHold: ShellHomeHold?
    @State var pageReturn: PageReturn<ShellPage>?
    @State var showAgentForm: Bool = false
    @State var started: Bool = false
    @State var keyboardUp: Bool = false
    @State var barHeight: CGFloat = ShellLayoutMemory.value(.barHeight)
    @State var capsuleTop: CGFloat = 0
    @State var barBottom: CGFloat = 0
    @State var safeBottom: CGFloat = ShellLayoutMemory.value(.safeBottom)
    @State var chromeHeight: CGFloat = ShellLayoutMemory.value(.chromeHeight)
    @State var docks: [AppTab: ShellDock] = Dictionary(
        uniqueKeysWithValues: AppTab.allCases.map { (tab: AppTab) in (tab, ShellDock()) })
    @State var runnerAttention: RunnerAttentionModel? = Services.get(RunnerAttentionModel.self)
    @State var fleet: FleetLightsModel? = InsightsFeature.api.map {
        FleetLightsModel(
            transport: $0, prober: HTTPStatusProbe(http: Services.get(HTTPClient.self) ?? URLSessionHTTPClient()))
    }
    @State var accountPackages: AccountPackagesStore? = PackagesFeature.accountPackages
    @State var stackTops: TabStackTops = TabStackTops()
    @State var stackResets: [AppTab: Int] = [:]
    @State var mountedTabs: Set<AppTab> = []
    @State var genie: GenieHub = GenieHub.shared
    @State var voice: ShellVoiceLaunch = ShellVoiceLaunch.shared
    @State var genieLater: [PaletteCommand] = []
    @State var genieDevice: AgentDeviceRun?
    @State var live: ThrottledSnapshot<ShellLiveCounts>?

    private let navigator: AppNavigator
    private let auth: AuthService?
    private let settings: SettingsService?
    private let notifications: NotificationService?
    private let push: PushNotificationService?
    private let userStatus: UserStatusService?
    private let identityKey: String

    init() {
        let notifications: NotificationService? = Services.get(NotificationService.self)
        _model = State(
            initialValue: HomeShellModel(
                chat: Services.get(ShellChatBridge.self), notifications: notifications,
                targets: Services.get(ShellPromptTargets.self)))
        let navigator: AppNavigator = AppNavigator.shared ?? AppNavigator.install()
        self.navigator = navigator
        _router = State(initialValue: navigator.router)
        self.auth = Services.get(AuthService.self)
        self.settings = Services.get(SettingsService.self)
        self.notifications = notifications
        self.push = Services.get(PushNotificationService.self)
        self.userStatus = Services.get(UserStatusService.self)
        self.identityKey = Services.get(DeviceIdentity.self)?.shortKey ?? ""
        _page = State(initialValue: UIDevice.current.userInterfaceIdiom == .pad ? nil : .home)
        // Built once with the services (ComposerInstaller) so the composer
        // is ready the moment the prompt button is tapped.
        _composer = State(initialValue: Services.get(ShellComposer.self) ?? ComposerInstaller.makeComposer())
    }

    static let offlineGrace: TimeInterval = 4
    static let barMargin: CGFloat = FmSpace.x6
    static let hubDrop: CGFloat = 54
    static let hubWidth: CGFloat = 320
    static let hubHeight: CGFloat = 640

    private var usesSplit: Bool {
        sizeClass == .regular && UIDevice.current.userInterfaceIdiom == .pad
    }

    // MARK: Top bar

    private var barState: ShellTopBarState {
        let session: AuthSession? = auth?.session
        let signedIn: Bool = userStatus?.signedIn ?? false
        return ShellTopBarState(
            relayState: model.relayState,
            onlineCount: liveCounts.onlineCount,
            name: shellAccountLabel(session, displayName: settings?.displayName ?? "", identityShortKey: identityKey),
            avatarPng: session?.avatarPng ?? "",
            notificationUnread: notifications?.unreadCount ?? 0,
            guest: auth?.isGuest ?? false,
            statusDescription: signedIn ? (userStatus?.status.describe(Date()) ?? "") : "",
            statusEmoji: signedIn ? (userStatus?.status.emoji ?? "") : "",
            statusAway: signedIn && (userStatus?.status.away ?? false),
            canSetStatus: signedIn,
            diagnosticsTroubled: diagnosticsTroubled)
    }

    private var topBar: some View {
        ShellTopBar(
            state: barState,
            onOpenHub: { openHub() },
            onOpenSearch: { openSearch("", scope: HomeShellCatalog.searchScope(for: router.selectedTab)) },
            onSignIn: { AccountFeature.openSignIn() },
            onHome: { openProjectHome() },
            onNewAgent: newAgentAction,
            onPalette: { openPalette() },
            onVoice: voiceAction,
            showsSearch: usesSplit,
            showsSignIn: PackageGateSync.isInstalled(PhonePackages.signIn))
    }

    private var newAgentAction: (() -> Void)? {
        agentsOn ? { () -> Void in openComposer(nil) } : nil
    }

    private var voiceAction: ((CGRect) -> Void)? {
        agentsOn ? { (origin: CGRect) -> Void in openVoice(from: origin) } : nil
    }

    private var badges: [AppTab: Int] {
        [.chat: liveCounts.chatUnread, .inbox: notifications?.unreadCount ?? 0]
    }

    private var liveCounts: ShellLiveCounts {
        live?.value ?? ShellLiveCounts()
    }

    private var primaryBarItems: [ShellBarItem] {
        let live: ShellLiveCounts = liveCounts
        let order: [Int] = HomeShellCatalog.bottomBarOrder
        let selectedIndex: Int = HomeShellCatalog.index(of: router.selectedTab)
        let counts: ShellCounts = ShellCounts.shared
        var items: [ShellBarItem] = order.enumerated().map { (entry: (offset: Int, element: Int)) -> ShellBarItem in
            let tab: AppTab = HomeShellCatalog.tab(at: entry.element)
            let label: String = tab == .code ? "Repos" : HomeShellCatalog.destinations[entry.element].label
            let selected: Bool =
                page == nil && entry.element == selectedIndex && (tab != .code || router.path(for: .code).isEmpty)
            let badge: Int
            let count: Int?
            switch tab {
            case .agents:
                badge = ShellLaunchFlight.shared.badge(running: counts.agentsTotal)
                count = counts.agentsTotal
            case .inbox:
                badge = notifications?.unreadCount ?? 0
                count = notifications?.inboxTotal
            case .code:
                badge = 0
                count = live.repos
            case .chat:
                badge = live.chatUnread
                count = live.onlineCount
            }
            return ShellBarItem(
                id: label, title: label, symbol: HomeShell.tabSymbol(tab, selected: selected), badge: badge,
                selected: selected, kind: .tab(position: entry.offset, total: order.count),
                pulse: tab == .agents ? ShellLaunchFlight.shared.landings : 0, flightTarget: tab == .agents,
                active: tab == .agents && counts.agents > 0, count: count,
                live: tab == .agents ? counts.agents : 0,
                loading: tab == .code && ShellNavigatorCounts.shared.isLoading(.repos), action: { selectTab(tab) })
        }
        items.insert(
            ShellBarItem(
                id: ShellHome.homeId, title: "Home", symbol: page == .home ? "house.fill" : "house", badge: 0,
                selected: page == .home, kind: .action, count: counts.agents, action: { showHome() }),
            at: 0)
        if let index = items.firstIndex(where: { $0.id == "Repos" }) {
            let repo: RepoRef? = counts.currentRepo
            items.insert(
                ShellBarItem(
                    id: "currentRepo", title: "Code", symbol: "chevron.left.forwardslash.chevron.right",
                    badge: counts.newPullsBadge,
                    selected: page == nil && router.selectedTab == .code && !router.path(for: .code).isEmpty,
                    kind: .action, count: counts.repoCount(.pulls),
                    loading: repo.map { counts.isLoadingCounts($0) } ?? false,
                    action: { openCurrentRepo(repo) }),
                at: index + 1)
        }
        items.append(
            ShellBarItem(
                id: "Search", title: "Search", symbol: "magnifyingglass", badge: 0, selected: false, kind: .action,
                count: navigator.saved.saved.count,
                action: { openSearch("", scope: HomeShellCatalog.searchScope(for: router.selectedTab)) }))
        items.append(
            ShellBarItem(
                id: "reportBug", title: "Bug", symbol: "ladybug", badge: 0, selected: page == .reportBug,
                kind: .action, count: ShellNavigatorCounts.shared.bugReports, action: { reportBug() }))
        return items
    }

    nonisolated static func runsBadge(_ action: HubItem.Action) -> Bool {
        guard case let .route(route) = action else { return false }
        return route.kind == .timers
    }

    private func hubBarGroups(installed: Set<String>) -> [ShellMenuGroup] {
        let live: ShellLiveCounts = liveCounts
        return HubGroups.current(guest: auth?.isGuest ?? false, installed: installed).map { (group: HubGroup) -> ShellMenuGroup in
            let items: [ShellBarItem] = group.items
                .filter { HomeShell.movesToBar($0.action) }
                .map { (item: HubItem) -> ShellBarItem in
                    ShellBarItem(
                        id: item.id, title: item.title, symbol: item.symbol, badge: live.hubBadges[item.id] ?? 0,
                        selected: false, kind: .action, running: HomeShell.runsBadge(item.action),
                        active: live.hubActive.contains(item.id), count: live.hubCounts[item.id],
                        progress: live.hubProgress[item.id], loading: live.hubLoading.contains(item.id),
                        action: { hubAction(item.action) })
                }
            return ShellMenuGroup(id: group.id, title: group.title, items: items)
        }
    }

    nonisolated static let repoSections: [RepoSection] = [.pulls, .issues, .commits, .branches, .releases, .actions, .discussions]

    private var repoBarGroups: [ShellMenuGroup] {
        let counts: ShellCounts = ShellCounts.shared
        guard let repo = counts.currentRepo else { return [] }
        let open: Route? = page == nil && router.selectedTab == .code ? router.path(for: .code).last : nil
        let items: [ShellBarItem] = HomeShell.repoSections.map { (section: RepoSection) -> ShellBarItem in
            let entry: (RepoSection, String, String)? = RepoOverviewModel.order.first { $0.0 == section }
            return ShellBarItem(
                id: "repo-\(section.rawValue)", title: entry?.1 ?? RouteCatalog.sectionTitle(section),
                symbol: entry?.2 ?? "folder", badge: section == .pulls ? counts.newPullsBadge : 0,
                selected: open == Route.repo(repo, section: section), kind: .action,
                count: counts.repoCount(section), loading: counts.isLoadingCounts(repo),
                action: { openRepoSection(repo, section) })
        }
        return [ShellMenuGroup(id: "repo", title: repo.name, items: items)]
    }

    private func loadRepoCounts(force: Bool = false) async {
        let counts: ShellCounts = ShellCounts.shared
        guard let repo = counts.currentRepo else { return }
        counts.restoreCounts(repo)
        if force { counts.markFresh(repo, false) }
        guard !counts.isFresh(repo) else { return }
        counts.setLoading(repo)
        defer { if counts.isLoadingCounts(repo) { counts.setLoading(nil) } }
        let overview: RepoOverviewModel = RepoOverviewModel(
            repo: Repository(owner: repo.owner, name: repo.name), client: RepoServices.overviewClient)
        await overview.load()
        guard counts.currentRepo == repo else { return }
        if !overview.counts.isEmpty {
            counts.adopt(repo, counts: overview.counts)
            counts.markFresh(repo, true)
        }
        guard let api = Services.get(ApiService.self) else { return }
        async let runs: Int? = try? await api.actionsRuns(repo).runs.count
        async let discussions: Int? = try? await api.publishedDiscussions(repo.owner, repo.name).count
        var more: [RepoSection: Int] = [:]
        more[.actions] = await runs
        more[.discussions] = await discussions
        guard counts.currentRepo == repo else { return }
        counts.adopt(repo, counts: more)
    }

    nonisolated static func movesToBar(_ action: HubItem.Action) -> Bool {
        switch action {
        case .route, .world, .designSystem: return true
        case .reportBug, .setStatus, .signIn, .signOut: return false
        }
    }

    nonisolated static func tabSymbol(_ tab: AppTab, selected: Bool) -> String {
        switch tab {
        case .agents: return "sparkles"
        case .inbox: return selected ? "tray.fill" : "tray"
        case .code: return "books.vertical"
        case .chat: return selected ? "bubble.left.and.bubble.right.fill" : "bubble.left.and.bubble.right"
        }
    }

    private var visitedBarIds: [String] {
        let selected: [String] = (repoBarGroups.flatMap(\.items) + primaryBarItems).filter(\.selected).map(\.id)
        guard selected.isEmpty, page == nil else { return selected }
        let hub: String? = ShellBarOrder.hubItemId(
            opening: router.currentPath.last, in: HubGroups.current(guest: auth?.isGuest ?? false))
        return hub.map { [$0] } ?? []
    }

    private var barGroups: [ShellMenuGroup] {
        let installed: Set<String> = PackageGateSync.publish()
        let main: ShellMenuGroup = ShellMenuGroup(id: "main", title: "Go to", items: primaryBarItems)
        let gated: [ShellMenuGroup] = ([main] + repoBarGroups + hubBarGroups(installed: installed)).compactMap {
            (group: ShellMenuGroup) -> ShellMenuGroup? in
            let items: [ShellBarItem] = group.items.filter { PackageGate.shows(item: $0.id, installed: installed) }
            return items.isEmpty ? nil : ShellMenuGroup(id: group.id, title: group.title, items: items)
        }
        return ShellBarPrefs.shared.arranged(gated).map {
            (group: ShellMenuGroup) -> ShellMenuGroup in
            ShellMenuGroup(id: group.id, title: group.title, items: group.items.map(leavingPage))
        }
    }

    private func leavingPage(_ item: ShellBarItem) -> ShellBarItem {
        var shown: ShellBarItem = item
        shown.action = {
            leavePage(item.action)
        }
        return shown
    }

    private func bottomBar(_ groups: [ShellMenuGroup]) -> some View {
        ShellBottomBar(
            items: groups.flatMap(\.items),
            onPackage: { (id: String) in openPackage(id) },
            onCapsuleTop: { (top: CGFloat) in capsuleTop = top })
            .opacity(composer.isOpen ? 0 : 1)
            .allowsHitTesting(!composer.isOpen)
    }

    @ViewBuilder private func pageView(_ groups: [ShellMenuGroup]) -> some View {
        switch page {
        case .home:
            ShellHome(
                groups: groups, prefs: ShellBarPrefs.shared, onPackage: { (id: String) in openPackage(id) },
                isUpdated: { (id: String) in accountPackages?.isUpdated(hubItem: id) ?? false },
                onSearch: { (query: String) in
                    leavePage { openSearch(query) }
                },
                onClearUpdates: { accountPackages?.markAllViewed() },
                canShowUpdate: { (id: String) in accountPackages?.canMarkUpdated(hubItem: id) ?? false },
                onShowUpdates: { accountPackages?.markAllUnviewed() },
                onRefresh: { refreshEverything() }
            ) { (item: ShellBarItem) in
                accountPackages?.markViewed(hubItem: item.id)
                ShellBottomBar.run(item, prefs: ShellBarPrefs.shared, reduceMotion: reduceMotion)
            }
            .task {
                await ShellNavigatorCounts.shared.refresh(guest: auth?.isGuest ?? false)
            }
        case .reportBug:
            ReportBugSheet(
                screen: router.selectedTab.title.lowercased(),
                onClose: { page = nil },
                onFinished: { (filed: Bool) in
                    if filed {
                        notice = FmNotice(ReportBugModel.thanks)
                    }
                })
        case nil:
            EmptyView()
        }
    }

    private var statusPageOn: Bool {
        accountPackages?.statusPageShown ?? true
    }

    private var agentsOn: Bool {
        PackageGateSync.isInstalled("agents")
    }

    private var diagnosticsEnabled: Bool {
        settings?.showTelemetryStrip ?? true
    }

    private var islandSystems: [StatusLight] {
        guard statusPageOn, diagnosticsEnabled, let fleet = fleet, fleet.loaded else { return [] }
        return fleet.shownSystems
    }

    private var islandFleet: [FleetLight] {
        guard statusPageOn, diagnosticsEnabled, let fleet = fleet, fleet.loaded else { return [] }
        return fleet.shownLights
    }

    private var diagnosticsTroubled: Bool {
        diagnosticsEnabled && statusPageOn && fleet?.loaded == true && fleet?.allHealthy == false
    }

    private var statusWatch: StatusWatch {
        guard let account = accountPackages else { return StatusWatch() }
        let filter: StatusMonitorFilter = account.monitorRules.map { (rules: PackageMonitors) -> StatusMonitorFilter in
            StatusMonitorFilter(systems: rules.systems, prefixes: rules.prefixes)
        } ?? .all
        let custom: [MonitorTarget] = account.monitors.compactMap { (monitor: StatusMonitor) -> MonitorTarget? in
            monitor.address.map { (url: URL) -> MonitorTarget in MonitorTarget(id: monitor.id, name: monitor.name, url: url) }
        }
        return StatusWatch(
            filter: filter, onlyProblems: account.onlyProblems, healthyMinutes: account.refreshMinutes, custom: custom)
    }

    private var pageKeyboardUp: Bool {
        keyboardUp && !composer.isOpen
    }

    private var showsBar: Bool {
        !pageKeyboardUp && !usesSplit && !ShellChrome.shared.isBare(router.selectedTab)
    }

    private var genieFloor: CGFloat? {
        showsBar && barHeight > 0 ? barBottom - barHeight + capsuleTop - (dockLift > 0 ? dockLift + ShellDock.gap : 0) : nil
    }

    private var barClearance: CGFloat {
        showsBar ? max(0, barHeight - safeBottom) + HomeShell.barMargin : 0
    }

    private var dockBottom: CGFloat {
        showsBar ? max(0, barHeight - capsuleTop) + ShellDock.gap : 0
    }

    private var dockLift: CGFloat {
        page == nil ? docks[router.selectedTab]?.height ?? 0 : 0
    }

    private func barStack(_ groups: [ShellMenuGroup]) -> some View {
        bottomBar(groups)
        .coordinateSpace(.named(ShellDock.barSpace))
        .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { (height: CGFloat) in
            barHeight = height
            ShellLayoutMemory.remember(.barHeight, height)
        }
        .onGeometryChange(for: CGFloat.self, of: { $0.frame(in: .global).maxY }) { (bottom: CGFloat) in
            barBottom = bottom
        }
        .frame(maxHeight: .infinity, alignment: .bottom)
        .ignoresSafeArea(.container, edges: .bottom)
    }

    static func coversPage(_ note: Notification) -> Bool {
        guard let frame = note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect else { return false }
        return frame.height > 120
    }

    private var floatingButtons: some View {
        HStack(alignment: .bottom) {
            Spacer(minLength: 0)
            if PackageGateSync.isInstalled("bug-reports") {
                BugReportButton(onReport: { reportBug() })
            }
        }
        .padding(.horizontal, FmSpace.x5)
        .padding(.bottom, FmSpace.x4)
    }

    // MARK: Tabs

    private func pathBinding(_ tab: AppTab) -> Binding<[Route]> {
        let router: AppRouter = self.router
        return Binding<[Route]>(
            get: { router.path(for: tab) },
            set: { (path: [Route]) in router.setPath(path, for: tab) })
    }

    @ViewBuilder private func tabRoot(_ tab: AppTab) -> some View {
        switch tab {
        case .agents:
            // Rebuilt when the prompt bar starts a run, so the list shows it.
            AgentsScreen()
                .id(model.agentsGeneration)
        case .chat:
            ChatScreen()
                .environment(
                    \.chatShellActions,
                    ChatShellActions(
                        onHub: { openHub() },
                        onNewAgent: newAgentAction,
                        onPalette: { openPalette() },
                        onVoice: voiceAction,
                        name: barState.name, avatarPng: auth?.session?.avatarPng ?? ""))
        case .inbox:
            RouteDestinationView(route: .inbox(.all))
        case .code:
            ReposScreen()
        }
    }

    private func tab(_ tab: AppTab) -> some View {
        let selected: Bool = page == nil && tab == router.selectedTab
        return NavigationStack(path: pathBinding(tab)) {
            tabRoot(tab)
                .tabStackPage(tab, tops: stackTops)
                .shellPageClearance()
                .background { ShellRootBarHider(tab: tab) }
                .toolbar(.hidden, for: .navigationBar)
                .navigationDestination(for: Route.self) { (route: Route) in
                    RouteDestinationView(route: route)
                        .tabStackPage(tab, tops: stackTops)
                        .shellPageClearance()
                }
        }
        .scrollContentBackground(fm.wallpaper ? .hidden : .automatic)
        .environment(\.shellDock, docks[tab])
        .environment(\.shellStackTab, tab)
        .environment(\.shellTabHidden, !selected)
        .id([stackResets[tab] ?? 0, ShellChrome.shared.stackReturns[tab] ?? 0])
        .opacity(selected ? 1 : 0)
        .allowsHitTesting(selected)
        .accessibilityHidden(!selected)
    }

    private func mounted(_ tab: AppTab) -> Bool {
        tab == router.selectedTab || mountedTabs.contains(tab)
    }

    private var tabs: some View {
        let groups: [ShellMenuGroup] = barGroups
        return ZStack {
            if mounted(.code) { tab(.code) }
            if mounted(.agents) { tab(.agents) }
            if mounted(.chat) { tab(.chat) }
            if mounted(.inbox) { tab(.inbox) }
            pageView(groups)
                .safeAreaPadding(.bottom, barClearance)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(alignment: .bottom) {
            if showsBar {
                barStack(groups)
            }
        }
        .onGeometryChange(for: CGFloat.self, of: { $0.safeAreaInsets.bottom }) { (inset: CGFloat) in
            safeBottom = inset
            if !keyboardUp { ShellLayoutMemory.remember(.safeBottom, inset) }
        }
        .onChange(of: dockBottom, initial: true) { (_: CGFloat, bottom: CGFloat) in
            for dock in docks.values {
                dock.bottom = bottom
            }
        }
        .onChange(of: barBottom, initial: true) { (_: CGFloat, bottom: CGFloat) in
            for dock in docks.values {
                dock.barBottom = bottom
            }
        }
        .onChange(of: barClearance, initial: true) { (_: CGFloat, clearance: CGFloat) in
            for dock in docks.values {
                dock.clearance = clearance
            }
        }
        .onChange(of: floatingTop, initial: true) { (_: CGFloat, top: CGFloat) in
            for dock in docks.values {
                dock.top = top
            }
        }
    }

    private var floatsChrome: Bool {
        page == nil && router.path(for: router.selectedTab).isEmpty && !stackTops.covered(router.selectedTab)
    }

    private var floatingTop: CGFloat {
        floatsChrome ? chromeHeight : 0
    }

    private var topChrome: some View {
        VStack(spacing: 0) {
            if page != nil || showsTopBar(router.selectedTab) {
                topBar
            }
            if settings?.project.hasInstance != false {
                FmConnectionBanner(state: offlineShown ? .offline : .connected, onRetry: { model.chat?.connect() })
            }
            if notice != nil {
                FmNoticeLine($notice)
                    .onChange(of: notice, initial: true) { _, current in
                        guard let current else { return }
                        InboxLocalPings.notice(surface: "shell", title: current.isError ? "ForkMesh needs attention" : "ForkMesh",
                                               message: current.message, isError: current.isError)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(fm.bgBase)
            }
            if let mail: ShellMailNotice = ShellMailStatus.shared.notice {
                FmNoticeLine(mail.line, isError: mail.isError, onDismiss: { ShellMailStatus.shared.dismiss() })
                    .padding(.horizontal, FmSpace.x4)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(fm.bgBase)
                    .id(mail.id)
                    .transition(.move(edge: .top).combined(with: .opacity))
                    .accessibilityIdentifier("shell-mail-notice")
            }
            if let attention = runnerAttention, router.selectedTab != .agents {
                RunnerAttentionBanner(model: attention, compact: true)
                    .padding(.horizontal, FmSpace.x2)
                    .padding(.vertical, FmSpace.x1)
            }
        }
        .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { (height: CGFloat) in
            chromeHeight = height
            if notice == nil && !offlineShown { ShellLayoutMemory.remember(.chromeHeight, height) }
        }
    }

    // MARK: Body

    private var shellColumn: some View {
        ZStack(alignment: .bottom) {
            VStack(spacing: 0) {
                tabs
                    .padding(.top, floatsChrome ? 0 : chromeHeight)
                if !pageKeyboardUp && usesSplit {
                    floatingButtons
                }
            }
            .overlay(alignment: .top) {
                topChrome
            }
            .ignoresSafeArea(.keyboard, edges: composer.isOpen ? .bottom : [])
            .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { (note: Notification) in
                keyboardUp = HomeShell.coversPage(note)
            }
            .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { (_: Notification) in
                keyboardUp = false
            }
            // Mounted from the start and hidden: opening it only flips state
            // and focuses a field that already exists.
            ShellComposerOverlay(
                model: model,
                composer: composer,
                onSubmit: submitPrompt,
                onFix: { (fix: QuickLaunchFix) in runFix(fix) },
                onPickContext: { showContextPicker = true },
                onDiagnostics: sendDiagnostics)
        }
    }

    private var hubDropdown: some View {
        GeometryReader { (proxy: GeometryProxy) in
            ZStack(alignment: .topTrailing) {
                if showHub {
                    Color.black.opacity(0.2)
                        .ignoresSafeArea()
                        .contentShape(Rectangle())
                        .onTapGesture { showHub = false }
                        .transition(.opacity)
                        .accessibilityHidden(true)
                }
                ZStack(alignment: .topTrailing) {
                    if showHub {
                        YouHubSheet(
                            router: router, state: barState, linksInBar: !usesSplit,
                            onOpen: { (route: Route) in openFromHub(route) },
                            onAction: { (action: HubItem.Action) in hubAction(action) },
                            onClose: { showHub = false },
                            onSendDiagnostics: { (prompt: String) async -> String? in await diagnosticsLaunch(prompt) })
                            .frame(width: min(HomeShell.hubWidth, proxy.size.width - FmSpace.x3 * 2))
                            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                            .shadow(color: Color.black.opacity(0.28), radius: 24, y: 10)
                            .frame(
                                maxHeight: max(240, min(HomeShell.hubHeight, proxy.size.height - HomeShell.hubDrop - FmSpace.x4)),
                                alignment: .top)
                            .accessibilityAddTraits(.isModal)
                            .transition(
                                reduceMotion
                                    ? AnyTransition.opacity
                                    : AnyTransition.modifier(
                                        active: ShellHubUnroll(progress: 0), identity: ShellHubUnroll(progress: 1)))
                    }
                }
                .padding(.top, HomeShell.hubDrop)
                .padding(.trailing, FmSpace.x3)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
        }
        .allowsHitTesting(showHub)
        .animation(reduceMotion ? .easeOut(duration: 0.15) : .spring(response: 0.36, dampingFraction: 0.86), value: showHub)
    }

    private func showsTopBar(_ tab: AppTab) -> Bool {
        let chrome: ShellChrome = ShellChrome.shared
        if tab == .chat {
            return chrome.isRevealed(tab) && !chrome.isBare(tab)
        }
        return !chrome.isBare(tab)
    }

    @ViewBuilder private var layout: some View {
        if usesSplit {
            NavigationSplitView {
                ShellSidebar(
                    router: router, badges: badges, guest: auth?.isGuest ?? false,
                    onSelectTab: { (tab: AppTab) in selectTab(tab) },
                    onOpen: { (route: Route) in openRoute(route) })
            } detail: {
                shellColumn
            }
        } else {
            shellColumn
        }
    }

    var body: some View {
        appChanges(routeChanges(framed(sheets(overlays(layout)))))
    }

    private func framed<Content: View>(_ content: Content) -> some View {
        content
            .background(fm.canvas.ignoresSafeArea())
            .background {
                ShellKeyboardCommands(
                    onTab: { (tab: AppTab) in selectTab(tab) },
                    onPalette: { openPalette() },
                    onCompose: { openComposer(nil) },
                    onNewIssue: { newIssue() },
                    onBack: { back() },
                    onSettings: { openFromHub(.settings(section: nil)) },
                    onEscape: { escape() })
            }
            .overlay {
                ShellVoiceLaunchOverlay(
                    voice: voice, onType: { voiceToComposer(voice.heard) }, onCommands: { voiceToPalette() })
            }
            .overlay { ShellLaunchSparkles(flight: ShellLaunchFlight.shared) }
            .overlay { AquariumLayer(hidden: composer.isOpen || keyboardUp || voice.isOpen, floor: genieFloor) }
            .overlay {
                if !genie.spiderHidden {
                    GenieSpiderLayer(
                        hidden: composer.isOpen || keyboardUp || voice.isOpen || router.currentRoute?.kind == .spiderRoom,
                        floor: genieFloor)
                }
            }
            .overlay(alignment: .top) {
                if diagnosticsEnabled {
                    ShellIslandStatusDots(
                        lights: islandSystems, fleet: islandFleet, runnerAgents: ShellCounts.shared.runnerAgents,
                        desktop: Services.get(ShellLocalDesktopBridge.self)?.localDesktop ?? .hidden,
                        desktops: Services.get(ShellLocalDesktopBridge.self)?.desktopLights ?? [],
                        desktopLoad: InsightsFeature.desktopLoadLight?.load, buildMac: InsightsFeature.buildMacLight?.light)
                }
            }
            .overlay { hubDropdown }
            .onChange(of: genie.farewells) { (_: Int, _: Int) in
                let hub: GenieHub = genie
                notice = FmNotice(
                    "Spider hidden. Bring it back in Settings, General.", actionLabel: "Undo",
                    onAction: { hub.setSpiderHidden(false) })
            }
            .runnerSignInPresenter(runnerAttention)
            .onChange(of: composer.launcher.repository?.ref) { (_: RepoRef?, ref: RepoRef?) in
                if let ref {
                    CodeCurrentRepo.follow(ref)
                }
            }
            .onChange(of: runnerAttention?.request == nil) { (_: Bool, closed: Bool) in
                if closed {
                    let launcher: QuickAgentLauncher = composer.launcher
                    Task { await launcher.refresh() }
                }
            }
            .task { await start() }
            .task(id: ShellCounts.shared.currentRepo) {
                await loadRepoCounts()
                if !Task.isCancelled {
                    await ShellPullsWatch.shared.refresh()
                }
            }
            .task { await ShellPullsWatch.shared.poll() }
            .task { await ShellSuggestions.shared.run() }
            .modifier(ShellAutoSendPresenter())
            .task { DjStudioHub.shared.relaunch() }
            .task {
                try? await Task.sleep(for: .seconds(2))
                mountedTabs.insert(.agents)
            }
            .task(id: model.relayState) { await settleOffline(model.relayState) }
            .onOpenURL { (url: URL) in
                navigator.openIncoming(url)
            }
            .onContinueUserActivity(NSUserActivityTypeBrowsingWeb) { (activity: NSUserActivity) in
                if let url = activity.webpageURL {
                    navigator.openIncoming(url)
                }
            }
    }

    private var pushScreen: PushScreen {
        let covered: Bool = page != nil || showHub || showPalette || showTour || showProjects || showAgentForm
            || showContextPicker || showDeviceSetup || showGemmaSetup || genie.sheetShown || pocket.locked
            || router.presented != nil
        return PushScreen(
            tab: router.selectedTab, route: router.currentRoute, covered: covered, conversation: model.conversation)
    }

    private func routeChanges<Content: View>(_ content: Content) -> some View {
        content
            .onChange(of: pushScreen, initial: true) { (_: PushScreen, screen: PushScreen) in
                push?.screen = screen
            }
            .onChange(of: push?.pendingOpen) { (_: PushOpen?, next: PushOpen?) in
                if next != nil {
                    Task { await routePushOpen() }
                }
            }
            .onChange(of: router.currentRoute) { (_: Route?, route: Route?) in
                if route != .videoMixDjStudio, DjStudioHub.shared.stepAside() {
                    router.remove(.videoMixDjStudio)
                }
            }
            .onChange(of: router.selectedTab) { (left: AppTab, tab: AppTab) in
                mountedTabs.formUnion([left, tab])
                if let back = pageReturn, back.tab != tab {
                    pageReturn = nil
                }
                if !holdsHome(tab: tab, path: router.currentPath) {
                    page = nil
                }
                model.select(tab: tab)
                if let package = PackageGate.packageId(for: tab) { accountPackages?.markViewed(package) }
                GenieCues.shared.tabChanged(to: AppTab.barOrder.firstIndex(of: tab) ?? 0, of: AppTab.barOrder.count)
                GenieScreenScan.shared.invalidate()
                genie.record(tab: tab)
            }
            .onChange(of: router.currentPath) { (old: [Route], path: [Route]) in
                if !holdsHome(tab: router.selectedTab, path: path) {
                    page = nil
                }
                returnToPage(path: path)
                if let route = path.last, let package = PackageGate.packageId(for: route) {
                    accountPackages?.markViewed(package)
                }
                GenieScreenScan.shared.invalidate()
                if path.count > old.count {
                    GenieCues.shared.play(.nod)
                    if let route = path.last { genie.record(route: route) }
                }
            }
            .onChange(of: visitedBarIds) { (_: [String], next: [String]) in
                ShellBarPrefs.shared.visit(next)
            }
            .onChange(of: page) { (_: ShellPage?, next: ShellPage?) in
                if next != .home {
                    homeHold = nil
                }
            }
            .onChange(of: projectHome.requests) { (_: Int, _: Int) in
                openProjectHome()
            }
            .onChange(of: router.composeRequest) { (_: ComposeTarget?, next: ComposeTarget?) in
                if next != nil, let target = router.takeComposeRequest() {
                    openComposer(target)
                }
            }
            .onChange(of: router.landings) { (_: Int, _: Int) in
                dismissOverlays()
            }
            .onChange(of: router.recents) { (_: [Route], recents: [Route]) in
                ShellQuickActions.refresh(recents: recents)
            }
    }

    private func appChanges<Content: View>(_ content: Content) -> some View {
        content
            .onChange(of: statusPageOn) { (_: Bool, on: Bool) in
                if on, diagnosticsEnabled, let fleet = fleet {
                    Task { await fleet.load() }
                }
            }
            .onChange(of: statusWatch) { (old: StatusWatch, next: StatusWatch) in
                guard let fleet = fleet else { return }
                fleet.watch = next
                if statusPageOn && diagnosticsEnabled && old.custom != next.custom {
                    Task { await fleet.probeCustom() }
                }
            }
            .onChange(of: settings?.showTelemetryStrip ?? true) { (_: Bool, enabled: Bool) in
                if enabled, statusPageOn, let fleet = fleet {
                    Task { await fleet.refresh() }
                }
            }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                if phase != .active {
                    if phase == .background { savedNavigation = router.archiveData() }
                    voice.cancel()
                    return
                }
                if let accountPackages = accountPackages {
                    Task { await accountPackages.refreshIfStale() }
                }
                Task { await ShellPullsWatch.shared.refresh() }
                if let fleet = fleet, statusPageOn, diagnosticsEnabled {
                    Task { await fleet.refresh() }
                }
                if diagnosticsEnabled, let desktopLoad = InsightsFeature.desktopLoadLight {
                    Task { await desktopLoad.refresh() }
                }
            }
            .onChange(of: auth?.isAuthenticated) { (_: Bool?, _: Bool?) in
                navigator.accountChanged()
                if let accountPackages = accountPackages {
                    Task { await accountPackages.refresh() }
                }
            }
            .onDisappear {
                savedNavigation = router.archiveData()
                push?.screen = nil
            }
            .fmInterfaceSounds(router: router)
    }

    private func overlays<Content: View>(_ content: Content) -> some View {
        content
            .sheet(isPresented: $showPalette) {
                CommandPaletteSheet(context: paletteContext, onCommand: { (command: PaletteCommand) in
                    runPalette(command)
                })
            }
            .sheet(isPresented: $showTour) {
                TourView(onFinish: { showTour = false }, onJump: { (jump: TourJump) in tourJump(jump) })
            }
            .sheet(isPresented: $genie.sheetShown, onDismiss: genieClosed) {
                if let session = genie.session {
                    GenieSheet(session: session, modelName: session.brain.modelName)
                        .presentationDetents([.large])
                        .presentationDragIndicator(.visible)
                }
            }
            .projectDrawer(isPresented: $showProjects)
            .sheet(isPresented: presentedSheet) {
                if let route = router.presented {
                    NavigationStack {
                        RouteDestinationView(route: route)
                            .toolbar {
                                ToolbarItem(placement: .cancellationAction) {
                                    Button("Done") { router.dismissPresented() }
                                }
                            }
                    }
                }
            }
            .fullScreenCover(isPresented: updateRequiredShown) {
                UpdateRequiredView(
                    requirement: updateCheck.requirement,
                    available: updateCheck.available,
                    onClose: updateCheck.blocksUse ? nil : { router.dismissPresented() })
            }
            .fullScreenCover(isPresented: nexusShown) {
                RouteDestinationView(route: .nexus)
            }
    }

    private func sheets<Content: View>(_ content: Content) -> some View {
        content
            .sheet(isPresented: $showContextPicker) {
                QuickLaunchPickerSheet(launcher: composer.launcher)
            }
            .sheet(isPresented: $showDeviceSetup) {
                AgentSetupSheet(initial: nil) { (_: AgentCredentials?) in
                    composer.launcher.credentialsChanged()
                    composer.clearProblem()
                }
                .fmSheetStyle()
            }
            .sheet(isPresented: $showGemmaSetup, onDismiss: { composer.clearProblem() }) {
                NavigationStack {
                    LocalAIFeature.settingsRoot()
                }
            }
            .fullScreenCover(item: $signInProvider) { (request: AgentSignInRequest) in
                NavigationStack {
                    AgentLoginTerminalScreen(node: request.node, provider: request.provider)
                        .toolbar {
                            ToolbarItem(placement: .topBarLeading) {
                                Button("Done") {
                                    signInProvider = nil
                                    composer.clearProblem()
                                }
                            }
                        }
                }
            }
            .fullScreenCover(isPresented: $pocket.locked) {
                PocketLockScreen(notifications: notifications)
            }
            .fullScreenCover(item: desktopAlarmRing) { (ring: DesktopAlarmRing) in
                DesktopAlarmView(ring: ring, onStop: { DesktopAlarm.shared.stop() })
            }
            .fullScreenCover(item: $deviceRun) { (run: AgentDeviceRun) in
                NavigationStack {
                    LocalRepoScreen(record: run.record, agentPrompt: run.prompt, agentAutoStart: true)
                        .toolbar {
                            ToolbarItem(placement: .topBarLeading) {
                                Button("Done") { deviceRun = nil }
                            }
                        }
                }
            }
            .sheet(isPresented: $showAgentForm, onDismiss: agentFormClosed) {
                AgentStartCard()
                    .fmSheetStyle(fitsContent: false)
            }
    }

    private var desktopAlarmRing: Binding<DesktopAlarmRing?> {
        let current: DesktopAlarmRing? = DesktopAlarm.shared.ringing
        return Binding<DesktopAlarmRing?>(
            get: { current },
            set: { (ring: DesktopAlarmRing?) in
                if ring == nil { DesktopAlarm.shared.stop() }
            })
    }

    private var presentedSheet: Binding<Bool> {
        let router: AppRouter = self.router
        return Binding<Bool>(
            get: {
                guard let route = router.presented else { return false }
                return route != .updateRequired && route != .nexus
            },
            set: { (shown: Bool) in
                if !shown {
                    router.dismissPresented()
                }
            })
    }

    private var nexusShown: Binding<Bool> {
        let router: AppRouter = self.router
        return Binding<Bool>(
            get: { router.presented == .nexus },
            set: { (shown: Bool) in
                if !shown {
                    router.dismissPresented()
                }
            })
    }

    private var updateRequiredShown: Binding<Bool> {
        let router: AppRouter = self.router
        let check: UpdateRequiredCheck = updateCheck
        return Binding<Bool>(
            get: { router.presented == .updateRequired || check.blocksUse },
            set: { (shown: Bool) in
                if !shown {
                    router.dismissPresented()
                }
            })
    }

    private var paletteContext: PaletteContext {
        PaletteContext(
            target: composeContext, tab: router.selectedTab, pendingApprovals: 0, recents: router.recents,
            guest: auth?.isGuest ?? false)
    }

    private var composeContext: ComposeTarget {
        ComposeContext.target(path: router.currentPath, tab: router.selectedTab, conversation: model.conversation)
    }

    // MARK: Behaviour

    /// initState's post-frame callback: read the inbox, refresh the avatar
    /// (a login-time snapshot), and route a push that launched the app.
    private func start() async {
        if started {
            return
        }
        started = true
        if live == nil {
            let auth: AuthService? = self.auth
            live = ShellLiveCounts.snapshot(
                chat: model.chat, fleet: fleet, attention: runnerAttention, guest: { auth?.isGuest ?? false })
        }
        attachGenie()
        if !router.restored {
            router.load(savedNavigation)
        }
        router.activate(project: settings?.relays.activeId ?? router.project)
        navigator.installChatIntercept()
        navigator.accountChanged()
        model.select(tab: router.selectedTab)
        let landed: ShellHomeHold = ShellHomeHold(tab: router.selectedTab, path: router.currentPath)
        navigator.drainEarly()
        if projectHome.takeArrival() {
            openProjectHome()
        } else if !usesSplit && router.presented == nil
            && landed == ShellHomeHold(tab: router.selectedTab, path: router.currentPath) {
            showHome()
        } else if page == .home {
            page = nil
        }
        if let target = router.takeComposeRequest() {
            openComposer(target)
        }
        ShellQuickActions.refresh(recents: router.recents)
        if !ShellTour.seen && PackageGateSync.isInstalled(PhonePackages.tour) {
            showTour = true
        }
        Task { await updateCheck.check() }
        Task { await updateCheck.checkStore() }
        if let accountPackages = accountPackages {
            Task { await accountPackages.refresh() }
        }
        if let fleet = fleet {
            fleet.watch = statusWatch
            if statusPageOn && diagnosticsEnabled {
                Task { await fleet.refresh() }
            }
        }
        Task { await HubPackageState.shared.refresh() }
        Task { await ShellNavigatorCounts.shared.refresh(guest: auth?.isGuest ?? false) }
        notifications?.start()
        if let auth = auth {
            Task { await auth.refreshAvatar() }
        }
        // Settle the composer's repository and target once, so the first
        // Send does not wait on the relay.
        let launcher: QuickAgentLauncher = composer.launcher
        Task { await launcher.prepare() }
        await routePushOpen()
        await push?.collectLaunchOpen()
    }

    private func settleOffline(_ state: ShellRelayState) async {
        switch state {
        case .connected:
            relayDownSince = nil
            revealOffline(false)
        case .connecting:
            if relayDownSince == nil {
                relayDownSince = Date()
            }
        case .offline:
            let since: Date = relayDownSince ?? Date()
            relayDownSince = since
            let wait: TimeInterval = HomeShell.offlineGrace - Date().timeIntervalSince(since)
            if wait > 0 {
                try? await Task.sleep(for: .seconds(wait))
            }
            if !Task.isCancelled {
                revealOffline(true)
            }
        }
    }

    private func revealOffline(_ shown: Bool) {
        if offlineShown == shown {
            return
        }
        withAnimation(FmMotion.easeOutCubic(FmMotion.normal, reduceMotion: reduceMotion)) {
            offlineShown = shown
        }
    }

    private func dismissOverlays() {
        pageReturn = nil
        showPalette = false
        showHub = false
        showProjects = false
        showAgentForm = false
        page = nil
        composer.close()
    }

    private func back() {
        if page != nil {
            page = nil
        } else {
            _ = router.back()
        }
    }

    private func refreshEverything() {
        let guest: Bool = auth?.isGuest ?? false
        Task {
            await HubPackageState.shared.refresh()
            await ShellNavigatorCounts.shared.refresh(guest: guest, everything: true)
        }
        Task {
            await loadRepoCounts(force: true)
            await ShellPullsWatch.shared.refresh()
        }
        if let accountPackages = accountPackages {
            Task { await accountPackages.refresh(catalog: true) }
        }
        if let fleet = fleet, statusPageOn, diagnosticsEnabled {
            Task { await fleet.refresh() }
        }
        if let notifications = notifications {
            Task { await notifications.refresh(silent: true) }
        }
        let launcher: QuickAgentLauncher = composer.launcher
        Task { await launcher.refresh() }
    }

    private func showHome() {
        ShellKeyboard.dismiss()
        composer.close()
        homeHold = ShellHomeHold(tab: router.selectedTab, path: router.currentPath)
        page = .home
    }

    private func openProjectHome() {
        dismissOverlays()
        router.dismissPresented()
        showHome()
    }

    private func leavePage(_ run: () -> Void) {
        let from: ShellPage? = page == .reportBug ? nil : page
        let tab: AppTab = router.selectedTab
        let path: [Route] = router.currentPath
        page = nil
        run()
        pageReturn = PageReturn.after(
            leaving: from, fromTab: tab, fromPath: path, tab: router.selectedTab, path: router.currentPath)
    }

    private func returnToPage(path: [Route]) {
        guard let back = pageReturn else { return }
        if back.restores(tab: router.selectedTab, path: path) {
            pageReturn = nil
            if back.page == .home {
                showHome()
            } else {
                ShellKeyboard.dismiss()
                page = back.page
            }
        } else if !back.holds(tab: router.selectedTab, path: path) {
            pageReturn = nil
        }
    }

    private func holdsHome(tab: AppTab, path: [Route]) -> Bool {
        page == .home && homeHold == ShellHomeHold(tab: tab, path: path)
    }

    private func openPackage(_ id: String) {
        accountPackages?.markViewed(id)
        var transaction: Transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            leavePage {
                openFromHub(.package(id: id))
            }
        }
    }

    private func escape() {
        if composer.isOpen {
            composer.close()
        } else if showPalette {
            showPalette = false
        } else if showHub {
            showHub = false
        } else if page != nil {
            page = nil
        }
    }

    private func tourJump(_ jump: TourJump) {
        showTour = false
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(450))
            switch jump {
            case let .tab(tab): selectTab(tab)
            case .home: showHome()
            case .hub: openHub()
            case .composer: openComposer(nil)
            case .palette: openPalette()
            case .projects: showProjects = true
            case let .route(route): openRoute(route)
            }
        }
    }

    private func openHub() {
        ShellKeyboard.dismiss()
        FmHaptics.tap()
        showHub = true
    }

    private func reportBug() {
        ShellKeyboard.dismiss()
        composer.close()
        page = .reportBug
    }

    private func openPalette() {
        ShellKeyboard.dismiss()
        composer.close()
        showPalette = true
    }

    private func openSearch(_ query: String, scope: SearchScope = .everything) {
        ShellKeyboard.dismiss()
        router.open(.search(query: query, scope: scope), placement: .currentTab)
    }

    private func newIssue() {
        if let repo = composeContext.repo {
            router.open(.newIssue(repo))
        } else {
            router.focus(.code)
        }
    }

    private func openRoute(_ route: Route) {
        if route.homeTab == nil {
            openFromHub(route)
        } else {
            router.remember(route)
            router.open(route)
        }
    }

    private func openFromHub(_ route: Route) {
        ShellKeyboard.dismiss()
        guard let action = HubCatalog.guestAction(for: route), auth?.isGuest ?? false else {
            openOnTop(route)
            return
        }
        Task { @MainActor in
            if await AccountFeature.requireAccount(action: action) {
                var transaction: Transaction = Transaction()
                transaction.disablesAnimations = true
                withTransaction(transaction) {
                    openOnTop(route)
                }
            }
        }
    }

    private func hubAction(_ action: HubItem.Action) {
        switch action {
        case let .route(route):
            openFromHub(route)
        case .world:
            openWorld()
        case .designSystem:
            openDesignSystem()
        case .signIn:
            AccountFeature.openSignIn()
        case .signOut:
            guard let auth = auth else { return }
            Task { @MainActor in
                await auth.logout()
                navigator.accountChanged()
                notice = FmNotice("Signed out")
            }
        case .reportBug, .setStatus:
            break
        }
    }

    private func runPalette(_ command: PaletteCommand) {
        switch command {
        case let .compose(target):
            openComposer(target)
        case let .open(route):
            openRoute(route)
        case let .selectTab(tab):
            selectTab(tab)
        case .switchProject:
            showProjects = true
        case let .searchEverywhere(query):
            openSearch(query)
        }
    }

    private func openWorld() {
        guard let uri = settings?.worldUri else {
            return
        }
        openURL(uri) { (accepted: Bool) in
            if !accepted {
                notice = FmNotice.error("Could not open ForkMesh World at \(uri.absoluteString)")
            }
        }
    }

    private func openDesignSystem() {
        let uri: URL = LegalLinks.designFor(settings?.homepageUri ?? LegalLinks.activeHomepage)
        openURL(uri) { (accepted: Bool) in
            if !accepted {
                notice = FmNotice.error("Could not open the design system at \(uri.absoluteString)")
            }
        }
    }

    private func openCurrentRepo(_ repo: RepoRef?) {
        ShellKeyboard.dismiss()
        guard let repo = repo else {
            selectTab(.code)
            notice = FmNotice("Open a repository and Code brings you back to it")
            return
        }
        let route: Route = .repo(repo, section: nil)
        landOnTop(.code)
        router.remember(route)
        router.setPath([route], for: .code)
        router.focus(.code)
        model.select(tab: .code)
    }

    private func openRepoSection(_ repo: RepoRef, _ section: RepoSection) {
        ShellKeyboard.dismiss()
        let route: Route = .repo(repo, section: section)
        landOnTop(.code)
        router.remember(route)
        router.setPath([.repo(repo, section: nil), route], for: .code)
        router.focus(.code)
        model.select(tab: .code)
    }

    private func selectTab(_ tab: AppTab) {
        ShellKeyboard.dismiss()
        landOnTop(tab)
        router.select(tab)
        model.select(tab: tab)
    }

    private func landOnTop(_ tab: AppTab) {
        if stackTops.covered(tab) {
            stackResets[tab, default: 0] += 1
        }
    }

    private func openOnTop(_ route: Route) {
        landOnTop(router.selectedTab)
        router.openFromHub(route)
    }

    private func attachGenie() {
        let composer: ShellComposer = self.composer
        let voice: ShellVoiceLaunch = self.voice
        genie.blocked = { composer.isOpen || voice.isOpen }
        genie.attach {
            let bridge: GenieShellBridge = GenieShellBridge(
                context: { paletteContext },
                onCommand: { (command: PaletteCommand) in genieRun(command) },
                onStart: { (prompt: String, repository: String?) in await genieLaunch(prompt, repository: repository) })
            return GenieFeature.makeSession(
                bridge: bridge,
                settings: { GenieFeature.settingsLine(composer.launcher) },
                launch: { (handoff: GenieHandoff) in await genieLaunch(handoff.prompt, repository: GenieHandoff.repository) })
        }
    }

    private func genieRun(_ command: PaletteCommand) {
        if genie.sheetShown {
            genieLater.append(command)
        } else {
            ShellKeyboard.dismiss()
            runPalette(command)
        }
    }

    private func genieClosed() {
        let later: [PaletteCommand] = genieLater
        genieLater = []
        for command in later {
            runPalette(command)
        }
        if let run = genieDevice {
            genieDevice = nil
            deviceRun = run
        }
    }

    private func genieLaunch(_ prompt: String, repository: String?) async -> String {
        let outcome: QuickLaunchOutcome = await composer.launcher.send(prompt: prompt, repository: repository, onDevice: true)
        switch outcome {
        case .queued(let repo, let session, _):
            FmHaptics.send(.success)
            if let session = session {
                genieRun(.open(LaunchedSessions.remember(repo, session)))
            }
        case .queuedWithoutSession, .desktop, .done:
            FmHaptics.send(.success)
        case .device(let record, let text, let images, let credentials):
            Services.get(AgentPromptHandoff.self)?.offer(images: images, credentials: credentials)
            if genie.sheetShown {
                genieDevice = AgentDeviceRun(record: record, prompt: text)
            } else {
                deviceRun = AgentDeviceRun(record: record, prompt: text)
            }
        case .ignored, .needsFix, .failed:
            FmHaptics.error()
        }
        return GenieFeature.outcomeLine(outcome)
    }

    private func diagnosticsLaunch(_ prompt: String) async -> String? {
        let outcome: QuickLaunchOutcome = await composer.launcher.send(
            prompt: prompt, repository: DiagnosticsBundle.repository, onDevice: false)
        switch outcome {
        case .queued(let repo, let session, _):
            FmHaptics.send(.success)
            agentStarted()
            selectTab(.agents)
            if let session = session {
                router.setPath([LaunchedSessions.remember(repo, session)], for: .agents)
            }
            return nil
        case .queuedWithoutSession(let message), .desktop(_, _, let message):
            FmHaptics.send(.success)
            agentStarted(message)
            selectTab(.agents)
            return nil
        case .device:
            return "Agents for \(DiagnosticsBundle.repository) run on this iPhone, so the report cannot start one here. Send it from a desktop or a mirror."
        case .needsFix(_, let message), .failed(let message):
            return message
        case .ignored:
            return "There was nothing to send."
        case .done:
            return nil
        }
    }

    private func openVoice(from origin: CGRect) {
        guard !composer.isOpen, !voice.isOpen else { return }
        ShellKeyboard.dismiss()
        showHub = false
        voice.open(from: origin) { (text: String) in voiceSend(text) }
    }

    private func voiceToComposer(_ heard: String) {
        voice.dismiss()
        if !heard.isEmpty {
            model.prompt = model.prompt.isEmpty ? heard : model.prompt + " " + heard
        }
        openComposer(nil)
    }

    private func voiceToPalette() {
        voice.dismiss()
        openPalette()
    }

    private func voiceSend(_ text: String) {
        let flight: ShellLaunchFlight = ShellLaunchFlight.shared
        let source: CGRect = flight.source
        let card: CGRect = flight.card
        if !usesSplit {
            flight.source = voice.orb
            flight.card = voice.orb
            flight.charge(reduceMotion: reduceMotion)
        }
        Task { @MainActor in
            let outcome: QuickLaunchOutcome = await composer.launcher.send(prompt: text, images: [])
            switch outcome {
            case .queued(let repo, let session, _):
                voice.sent()
                agentStarted()
                if let session = session {
                    router.setPath([LaunchedSessions.remember(repo, session)], for: .agents)
                }
            case .queuedWithoutSession(let message), .desktop(_, _, let message):
                voice.sent()
                agentStarted(message)
            case .device(let record, let prompt, let images, let credentials):
                voice.sent()
                Services.get(AgentPromptHandoff.self)?.offer(images: images, credentials: credentials)
                deviceRun = AgentDeviceRun(record: record, prompt: prompt)
            case .needsFix(let fix, let message):
                FmHaptics.error()
                voiceToComposer(text)
                composer.fix = fix
                composer.message = message
            case .failed(let message):
                FmHaptics.error()
                voiceToComposer(text)
                composer.message = message
            case .done(let message):
                voice.sent()
                notice = FmNotice(message)
            case .ignored:
                voice.dismiss()
            }
            flight.discharge()
            flight.source = source
            flight.card = card
        }
    }

    private func openComposer(_ requested: ComposeTarget?) {
        let target: ComposeTarget = requested ?? composeContext
        let onDevice: Bool = requested?.onDevice ?? false
        if let repo = requested?.repo?.slug ?? composer.screenRepo ?? target.repo?.slug {
            let launcher: QuickAgentLauncher = composer.launcher
            Task { @MainActor in
                await launcher.prepare(currentRepo: repo)
                if onDevice {
                    launcher.prefer(.device)
                }
            }
        }
        if let text = target.text, !text.isEmpty, model.prompt.isEmpty || onDevice {
            model.prompt = text
        }
        composer.open()
    }

    /// Send: an agent run that starts now, or the page's own target. Whatever
    /// cannot run keeps the text and says why.
    private func submitPrompt() {
        if composer.busy {
            return
        }
        composer.clearProblem()
        if model.promptTarget != nil {
            Task { @MainActor in
                composer.busy = true
                defer { composer.busy = false }
                switch await model.submitPrompt() {
                case .handled(let message):
                    FmHaptics.send(.arrive)
                    composer.close()
                    notice = FmNotice(message)
                case .failed(let message):
                    FmHaptics.error()
                    composer.message = message
                default:
                    break
                }
            }
            return
        }
        let text: String = model.prompt
        let images: [PromptAttachment] = composer.attachments
        if !usesSplit {
            ShellLaunchFlight.shared.charge(reduceMotion: reduceMotion)
        }
        Task { @MainActor in
            composer.busy = true
            let outcome: QuickLaunchOutcome = await composer.launcher.send(prompt: text, images: images)
            composer.busy = false
            handle(outcome, sent: text)
            ShellLaunchFlight.shared.discharge()
        }
    }

    private func sendDiagnostics() {
        composer.clearProblem()
        Task { @MainActor in
            guard let report = await PerformanceReportBuilder.report(router: router, draft: model.prompt) else {
                FmHaptics.error()
                composer.message = "The performance report is unavailable in this build, or the prompt has no room left for it."
                return
            }
            FmHaptics.success()
            model.targets.bypassed = true
            model.prompt = PerformanceReportBuilder.prompt(adding: report, to: model.prompt)
            composer.focus.focus()
        }
    }

    private func handle(_ outcome: QuickLaunchOutcome, sent: String) {
        switch outcome {
        case .ignored:
            return
        case .queued(let repo, let session, _):
            FmHaptics.send(.success)
            finishSend(sent)
            agentStarted()
            if let session = session {
                router.setPath([LaunchedSessions.remember(repo, session)], for: .agents)
            }
        case .queuedWithoutSession(let message):
            FmHaptics.send(.success)
            finishSend(sent)
            agentStarted(message)
        case .device(let record, let prompt, let images, let credentials):
            finishSend(sent)
            Services.get(AgentPromptHandoff.self)?.offer(images: images, credentials: credentials)
            deviceRun = AgentDeviceRun(record: record, prompt: prompt)
        case .desktop(_, _, let message):
            FmHaptics.send(.success)
            finishSend(sent)
            agentStarted(message)
        case .done(let message):
            FmHaptics.send(.success)
            finishSend(sent)
            notice = FmNotice(message)
        case .needsFix(let fix, let message):
            FmHaptics.error()
            composer.fix = fix
            composer.message = message
        case .failed(let message):
            FmHaptics.error()
            composer.message = message
        }
    }

    /// The run is away: clear what was sent (not anything typed since) and
    /// put the composer away.
    private func finishSend(_ sent: String) {
        if model.prompt == sent {
            model.prompt = ""
        }
        composer.attachments = []
        composer.close()
    }

    /// The one tap that clears what stood in the way; the text stays.
    private func runFix(_ fix: QuickLaunchFix) {
        switch fix {
        case .signInAccount:
            composer.close()
            Task { @MainActor in
                if await AccountFeature.requireAccount(action: "start an agent") {
                    composer.clearProblem()
                    await composer.launcher.prepare(currentRepo: nil)
                }
            }
        case .chooseRepository:
            showContextPicker = true
        case .signInProvider(let node, let provider):
            composer.close()
            signInProvider = AgentSignInRequest(node: node, provider: provider)
        case .signInRunner(let runner, let provider):
            composer.close()
            composer.clearProblem()
            runnerAttention?.open(runner: runner, provider: provider)
        case .setUpDevice:
            composer.close()
            showDeviceSetup = true
        case .setUpGemma:
            composer.close()
            showGemmaSetup = true
        case .mirrorToDevice:
            // Mirroring is a longer job with its own progress: the full form
            // does it, with the prompt and photos carried over.
            composer.close()
            let handoff: AgentPromptHandoff? = Services.get(AgentPromptHandoff.self)
            handoff?.offer(prompt: model.prompt, images: composer.attachments, onStarted: {
                showAgentForm = false
                model.prompt = ""
                composer.attachments = []
                agentStarted()
            })
            showAgentForm = true
        }
    }

    private func agentStarted(_ message: String? = nil) {
        GenieCues.shared.play(.cheer)
        genie.agentStarted()
        if usesSplit {
            if let message = message {
                notice = FmNotice(message)
            }
        } else {
            ShellLaunchFlight.shared.launch(running: ShellCounts.shared.agentsTotal, reduceMotion: reduceMotion)
            if let message = message {
                UIAccessibility.post(notification: .announcement, argument: message)
            }
        }
        router.popToRoot(.agents)
        router.focus(.agents)
        model.agentStarted()
    }

    private func agentFormClosed() {
        Services.get(AgentPromptHandoff.self)?.abandoned()
    }

    /// Take the user where the tapped push was about. The
    /// screen the app was left on is not where the tap points.
    private func routePushOpen() async {
        guard let push = push, let open = push.takePendingOpen() else {
            return
        }
        showHub = false
        showPalette = false
        showAgentForm = false
        page = nil
        composer.close()
        DjStudioHub.shared.stepAside()
        router.remove(.videoMixDjStudio)
        if let attention = runnerAttention, attention.openHref(open.href) {
            router.focus(.agents)
            if push.pendingOpen != nil {
                await routePushOpen()
            }
            return
        }
        let route: ShellPushRoute? = await model.routePush(
            isChat: open.isChat, isDirectMessage: open.isDirectMessage, alertId: open.alertId, href: open.href,
            agent: open.agent, ping: open.ping, conversation: open.conversation, channelId: open.channelId)
        switch route {
        case .some(.chat):
            router.popToRoot(.chat)
            router.focus(.chat)
        case .some(.notifications):
            router.popToRoot(.inbox)
            router.focus(.inbox)
        case .some(.repo(let link)):
            router.show(stack: link.routeStack)
        case .some(.ping(let id)):
            notifications?.requestOpen(id)
            router.popToRoot(.inbox)
            router.focus(.inbox)
        case .some(.agent(let id)):
            router.show(.agent(id: id))
        case .none:
            break
        }
        // Another tap may have landed while this one was awaiting the inbox.
        if push.pendingOpen != nil {
            await routePushOpen()
        }
    }
}

private struct ShellRootBarHider: UIViewRepresentable {
    let tab: AppTab

    func makeUIView(context: Context) -> ShellRootBarHiderView {
        let view: ShellRootBarHiderView = ShellRootBarHiderView()
        view.tab = tab
        return view
    }

    func updateUIView(_ uiView: ShellRootBarHiderView, context: Context) {
        uiView.tab = tab
        uiView.settle()
    }
}

private final class ShellRootBarHiderView: UIView {
    var tab: AppTab?
    private var returning: Bool = false

    override func didMoveToWindow() {
        super.didMoveToWindow()
        isUserInteractionEnabled = false
        settle()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        settle()
    }

    override func safeAreaInsetsDidChange() {
        super.safeAreaInsetsDidChange()
        settle()
    }

    func settle() {
        guard window != nil, let navigation = owningNavigation() else { return }
        correctTopInset(in: navigation)
        if let coordinator = navigation.transitionCoordinator {
            if !returning, let page = owningPage(in: navigation), coordinator.viewController(forKey: .to) === page {
                comeBack(in: navigation, with: coordinator)
            }
            coordinator.animate(alongsideTransition: nil) { [weak self] (_: UIViewControllerTransitionCoordinatorContext) in
                self?.hideWhenOnTop()
            }
        } else {
            hideWhenOnTop()
        }
    }

    private func comeBack(in navigation: UINavigationController, with coordinator: UIViewControllerTransitionCoordinator) {
        returning = true
        let tab: AppTab? = self.tab
        if let tab {
            withAnimation(.easeOut(duration: coordinator.transitionDuration)) {
                ShellChrome.shared.returnToRoot(on: tab)
            }
        }
        let hid: Bool = !navigation.isNavigationBarHidden
        if hid {
            navigation.setNavigationBarHidden(true, animated: coordinator.isAnimated)
            correctTopInset(in: navigation)
        }
        coordinator.animate(alongsideTransition: nil) { [weak self, weak navigation] (context: UIViewControllerTransitionCoordinatorContext) in
            self?.returning = false
            guard context.isCancelled else { return }
            if let tab { ShellChrome.shared.stayOff(tab) }
            if hid, let navigation, navigation.isNavigationBarHidden {
                navigation.setNavigationBarHidden(false, animated: false)
            }
        }
    }

    private func hideWhenOnTop() {
        guard window != nil, let navigation = owningNavigation(), !navigation.isNavigationBarHidden,
              let top = navigation.topViewController?.viewIfLoaded, isDescendant(of: top) else { return }
        navigation.setNavigationBarHidden(true, animated: false)
        correctTopInset(in: navigation)
    }

    private func correctTopInset(in navigation: UINavigationController) {
        guard let window, let page = owningPage(in: navigation), let pageView = page.viewIfLoaded,
              pageView.window != nil else { return }
        let applied: CGFloat = page.additionalSafeAreaInsets.top
        let correction: CGFloat
        if navigation.isNavigationBarHidden {
            let natural: CGFloat = pageView.safeAreaInsets.top - applied
            let stackTop: CGFloat = navigation.view.convert(navigation.view.bounds, to: window).minY
            let expected: CGFloat = max(0, window.safeAreaInsets.top - stackTop)
            correction = min(0, expected - natural)
        } else {
            correction = 0
        }
        if abs(correction - applied) > 0.5 {
            page.additionalSafeAreaInsets.top = correction
        }
    }

    private func owningPage(in navigation: UINavigationController) -> UIViewController? {
        var responder: UIResponder? = next
        while let current = responder {
            if let controller = current as? UIViewController, controller.parent === navigation {
                return controller
            }
            responder = current.next
        }
        return nil
    }

    private func owningNavigation() -> UINavigationController? {
        var responder: UIResponder? = next
        while let current = responder {
            if let navigation = current as? UINavigationController {
                return navigation
            }
            responder = current.next
        }
        return nil
    }
}
