import FMAgents
import FMApi
import FMBase
import FMCode
import SwiftUI

@MainActor
enum NavigationRoutes {
    static let owner = "WS8"

    static func register(into services: AppServices) {
        let routes: RouteRegistry<AnyView> = services.routes
        routes.register(.search, by: owner) { route in
            guard case let .search(query, scope) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(GlobalSearchScreen(query: query, scope: GlobalSearchScope(scope)))
        }
        routes.register(.tour, by: owner) { _ in AnyView(TourView(onFinish: nil)) }
        routes.register(.updateRequired, by: owner) { _ in
            AnyView(UpdateRequiredView(requirement: UpdateRequiredCheck.shared.requirement))
        }
        routes.register(.launchedAgent, by: owner) { route in
            guard case let .launchedAgent(repo, sessionId) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            let (repository, session) = LaunchedSessions.lookup(repo, sessionId)
            if let center = HostsFeature.agents, let run = AgentLines.runRow(repository, session, rows: center.rows) {
                return AnyView(AgentRunScreen(center: center, row: run))
            }
            return AnyView(LaunchedAgentSessionScreen(repo: repository, session: session))
        }
        registerExisting(routes)
    }

    private static func fallback(
        _ routes: RouteRegistry<AnyView>, _ kind: Route.Kind, _ builder: @escaping @MainActor (Route) -> AnyView
    ) {
        if !routes.isRegistered(kind) {
            routes.register(kind, by: "\(owner) fallback", builder)
        }
    }

    private static func repository(_ ref: RepoRef) -> Repository {
        Repository(owner: ref.owner, name: ref.name)
    }

    static func detailTab(_ section: RepoSection?) -> RepoDetailTab? {
        guard let section = section else { return .code }
        switch section {
        case .code: return .code
        case .issues: return .issues
        case .pulls: return .pulls
        case .discussions: return .discussions
        case .commits: return .commits
        case .mirrors: return .mirrors
        case .releases: return .releases
        case .about: return .about
        case .actions: return .actions
        case .agents: return .agents
        case .worktrees: return .worktrees
        default: return nil
        }
    }

    private static func titled<Content: View>(_ title: String, _ content: Content) -> AnyView {
        AnyView(content.navigationTitle(title).navigationBarTitleDisplayMode(.inline))
    }

    private static func registerExisting(_ routes: RouteRegistry<AnyView>) {
        fallback(routes, .repos) { _ in AnyView(ReposScreen()) }
        fallback(routes, .repo) { route in
            guard case let .repo(ref, section) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            guard let tab = detailTab(section) else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(RepoDetailScreen(repo: repository(ref), initialTab: tab))
        }
        fallback(routes, .issue) { route in
            guard case let .issue(ref, number) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(IssueDetailScreen(repo: repository(ref), number: number))
        }
        fallback(routes, .newIssue) { route in
            guard case let .newIssue(ref) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(NewIssueScreen(repo: repository(ref)))
        }
        fallback(routes, .pull) { route in
            guard case let .pull(ref, number, _) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(PullDetailScreen(repo: repository(ref), number: number))
        }
        fallback(routes, .discussion) { route in
            guard case let .discussion(ref, number) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(DiscussionDetailScreen(repo: repository(ref), number: number))
        }
        fallback(routes, .commit) { route in
            guard case let .commit(ref, sha) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            let link = NotificationDeepLink(repo: repository(ref), initialTab: .commits, reference: sha)
            return AnyView(RepoDetailScreen(repo: link.repo, initialTab: .commits, initialTarget: link))
        }
        fallback(routes, .commits) { route in
            guard case let .commits(ref, branch) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            let link = NotificationDeepLink(repo: repository(ref), initialTab: .commits, branch: branch)
            return AnyView(RepoDetailScreen(repo: link.repo, initialTab: .commits, initialTarget: link))
        }
        fallback(routes, .tree) { route in
            guard case let .tree(ref, branch, _) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            let link = NotificationDeepLink(repo: repository(ref), initialTab: .code, branch: branch)
            return AnyView(RepoDetailScreen(repo: link.repo, initialTab: .code, initialTarget: link))
        }
        fallback(routes, .file) { route in
            guard case let .file(ref, branch, path) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(RepoFileScreen(repo: repository(ref), path: path, ref: branch))
        }
        fallback(routes, .localRepo) { route in
            guard case let .localRepo(id) = route,
                  let record = LocalRepoServices.store?.records.first(where: { $0.name == id })
            else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(LocalRepoScreen(record: record))
        }
        fallback(routes, .chat) { _ in AnyView(ChatScreen()) }
        fallback(routes, .settings) { _ in titled("Settings", SettingsScreen()) }
        fallback(routes, .pingSettings) { _ in AnyView(PingSettingsScreen()) }
        fallback(routes, .activity) { _ in titled("Activity", ActivityScreen()) }
        fallback(routes, .calories) { _ in AnyView(CaloriesScreen()) }
        fallback(routes, .steps) { _ in AnyView(StepsScreen()) }
        fallback(routes, .water) { _ in AnyView(WaterScreen()) }
        fallback(routes, .sleep) { _ in AnyView(SleepScreen()) }
        fallback(routes, .sugarBreak) { _ in AnyView(SugarBreakScreen()) }
        fallback(routes, .stretch) { _ in AnyView(StretchScreen()) }
        fallback(routes, .foodGuide) { _ in AnyView(FoodGuideScreen()) }
        fallback(routes, .speech) { _ in AnyView(SpeechScreen()) }
        fallback(routes, .glassesAlert) { _ in AnyView(GlassesAlertScreen()) }
        fallback(routes, .vault) { _ in AnyView(VaultScreen()) }
        fallback(routes, .desktops) { _ in AnyView(LocalDesktopScreen()) }
        fallback(routes, .localAI) { _ in titled("On-device AI", LocalAISettingsView()) }
        fallback(routes, .orgs) { _ in AnyView(OrgsScreen()) }
        fallback(routes, .org) { route in
            guard case let .org(name) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(OrgDetailScreen(orgName: name))
        }
        fallback(routes, .projects) { _ in AnyView(ProjectsRouteScreen()) }
        fallback(routes, .inbox) { _ in AnyView(NotificationsScreen()) }
    }
}

struct ProjectsRouteScreen: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        ScrollView {
            RelaySettingsSection()
                .padding(FmSpace.x4)
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationTitle("Projects")
        .navigationBarTitleDisplayMode(.inline)
    }
}
