import Foundation
import Observation

public enum RoutePlacement: Equatable, Sendable {
    case automatic
    case currentTab
    case tab(AppTab)
}

public enum LinkOutcome: Equatable, Sendable {
    case opened
    case waitingForSignIn
    case switchProject(String)
    case unknownProject(String)
    case ignored
}

public struct TabNavigationState: Codable, Equatable, Sendable {
    public var selectedTab: AppTab
    public var paths: [String: [Route]]
    public var recents: [Route]
    public var savedAt: Double

    public init(selectedTab: AppTab = .agents, paths: [String: [Route]] = [:], recents: [Route] = [], savedAt: Double = 0) {
        self.selectedTab = selectedTab
        self.paths = paths
        self.recents = recents
        self.savedAt = savedAt
    }
}

public struct NavigationArchive: Codable, Equatable, Sendable {
    public var projects: [String: TabNavigationState]

    public init(projects: [String: TabNavigationState] = [:]) {
        self.projects = projects
    }

    public static func decode(_ data: Data?) -> NavigationArchive {
        guard let data = data, !data.isEmpty,
              let archive = try? JSONDecoder().decode(NavigationArchive.self, from: data) else {
            return NavigationArchive()
        }
        return archive
    }

    public func encoded() -> Data {
        (try? JSONEncoder().encode(self)) ?? Data()
    }
}

@MainActor
@Observable
public final class AppRouter {
    public static let restoreWindow: TimeInterval = 8 * 60 * 60
    public static let maxDepth = 32
    public static let maxRecents = HubCatalog.maxRecents

    public private(set) var selectedTab: AppTab = .agents
    public private(set) var paths: [AppTab: [Route]] = [:]
    public private(set) var reselects: [AppTab: Int] = [:]
    public private(set) var recents: [Route] = []
    public private(set) var presented: Route?
    public private(set) var composeRequest: ComposeTarget?
    public private(set) var pendingLink: ParsedLink?
    public private(set) var project: String
    public private(set) var landings: Int = 0
    public private(set) var restored = false
    public var accountReady: Bool {
        didSet {
            if accountReady && !oldValue { flushPending() }
        }
    }

    @ObservationIgnored public var intercept: (@MainActor (Route) -> Bool)?
    @ObservationIgnored private var archive = NavigationArchive()
    @ObservationIgnored private let now: () -> Date

    public init(project: String = "", accountReady: Bool = true, now: @escaping () -> Date = Date.init) {
        self.project = project
        self.accountReady = accountReady
        self.now = now
    }

    public func path(for tab: AppTab) -> [Route] {
        paths[tab] ?? []
    }

    public var currentPath: [Route] { path(for: selectedTab) }

    public var currentRoute: Route? { currentPath.last }

    public func setPath(_ path: [Route], for tab: AppTab) {
        let capped = Array(path.suffix(AppRouter.maxDepth))
        if paths[tab] ?? [] != capped {
            paths[tab] = capped
        }
    }

    public func select(_ tab: AppTab) {
        if tab == selectedTab && path(for: tab).isEmpty {
            reselects[tab, default: 0] += 1
            return
        }
        if selectedTab != tab { selectedTab = tab }
        if !path(for: tab).isEmpty { paths[tab] = [] }
    }

    public func focus(_ tab: AppTab) {
        if selectedTab != tab { selectedTab = tab }
    }

    public func popToRoot(_ tab: AppTab? = nil) {
        paths[tab ?? selectedTab] = []
    }

    @discardableResult
    public func back() -> Bool {
        var path = currentPath
        guard !path.isEmpty else { return false }
        path.removeLast()
        paths[selectedTab] = path
        return true
    }

    public func targetTab(for route: Route, placement: RoutePlacement = .automatic) -> AppTab {
        switch placement {
        case .automatic: return route.homeTab ?? selectedTab
        case .currentTab: return selectedTab
        case let .tab(tab): return tab
        }
    }

    public func open(_ route: Route, placement: RoutePlacement = .automatic) {
        if intercept?(route) == true { return }
        if route.isModal {
            present(route)
            return
        }
        let tab = targetTab(for: route, placement: placement)
        selectedTab = tab
        if route == tab.root {
            paths[tab] = []
            return
        }
        var path = self.path(for: tab)
        if path.last != route {
            path.append(route)
        }
        setPath(path, for: tab)
    }

    public func openFromHub(_ route: Route) {
        remember(route)
        let path = currentPath
        guard !route.isModal, let index = path.firstIndex(of: route) else {
            open(route, placement: .currentTab)
            return
        }
        setPath(Array(path.prefix(through: index)), for: selectedTab)
    }

    public func show(stack: [Route], placement: RoutePlacement = .automatic) {
        guard let last = stack.last else { return }
        if intercept?(last) == true { return }
        if last.isModal {
            present(last)
            return
        }
        landings += 1
        let tab = targetTab(for: last, placement: placement)
        selectedTab = tab
        var trimmed = stack.filter { !$0.isModal }
        if trimmed.first == tab.root {
            trimmed.removeFirst()
        }
        if last.homeTab == nil && placement == .automatic {
            let current = path(for: tab)
            if current.suffix(trimmed.count).elementsEqual(trimmed) { return }
            setPath(current + trimmed, for: tab)
        } else {
            setPath(trimmed, for: tab)
        }
    }

    public func show(_ route: Route) {
        show(stack: RouteParser.stack(for: route))
    }

    @discardableResult
    public func open(link: ParsedLink, knownProjects: Set<String> = []) -> LinkOutcome {
        guard !link.stack.isEmpty else { return .ignored }
        var link = link
        if let target = link.project, !target.isEmpty, target != project {
            if knownProjects.contains(target) {
                pendingLink = link
                return .switchProject(target)
            }
            if link.explicitProject { return .unknownProject(target) }
            link.project = nil
        }
        guard accountReady else {
            pendingLink = link
            return .waitingForSignIn
        }
        pendingLink = nil
        show(stack: link.stack)
        return .opened
    }

    public func discardPending() {
        pendingLink = nil
    }

    public func present(_ route: Route) {
        if case let .compose(target) = route {
            composeRequest = target
            return
        }
        presented = route
    }

    public func requestCompose(_ target: ComposeTarget) {
        composeRequest = target
    }

    public func takeComposeRequest() -> ComposeTarget? {
        defer { composeRequest = nil }
        return composeRequest
    }

    public func dismissPresented() {
        presented = nil
    }

    public func remember(_ route: Route) {
        guard !route.isModal else { return }
        recents = Array(([route] + recents.filter { $0 != route }).prefix(AppRouter.maxRecents))
    }

    public func activate(project id: String) {
        if id != project {
            if !project.isEmpty {
                archive.projects[project] = state()
            }
            project = id
            apply(archive.projects[id])
        }
        flushPending()
    }

    public func load(_ data: Data?) {
        restored = true
        archive = NavigationArchive.decode(data)
        apply(archive.projects[project])
    }

    public func archiveData() -> Data {
        var copy = archive
        if !project.isEmpty {
            copy.projects[project] = state()
        }
        archive = copy
        return copy.encoded()
    }

    public func state() -> TabNavigationState {
        var stored: [String: [Route]] = [:]
        for (tab, path) in paths where !path.isEmpty {
            stored[tab.rawValue] = path
        }
        return TabNavigationState(
            selectedTab: selectedTab, paths: stored, recents: recents, savedAt: now().timeIntervalSince1970)
    }

    private func apply(_ state: TabNavigationState?) {
        guard let state = state else {
            selectedTab = .agents
            paths = [:]
            recents = []
            return
        }
        var restored: [AppTab: [Route]] = [:]
        for (key, path) in state.paths {
            if let tab = AppTab(rawValue: key) {
                restored[tab] = Array(path.suffix(AppRouter.maxDepth))
            }
        }
        recents = Array(state.recents.prefix(AppRouter.maxRecents))
        if now().timeIntervalSince1970 - state.savedAt > AppRouter.restoreWindow {
            restored[.agents] = nil
            selectedTab = .agents
        } else {
            selectedTab = state.selectedTab
        }
        paths = restored
    }

    private func flushPending() {
        guard accountReady, let link = pendingLink else { return }
        if let target = link.project, !target.isEmpty, target != project { return }
        pendingLink = nil
        show(stack: link.stack)
    }
}

public enum ComposeContext {
    public static func target(path: [Route], tab: AppTab, conversation: String = "") -> ComposeTarget {
        for route in path.reversed() {
            switch route {
            case let .agent(id):
                return ComposeTarget(agentId: id)
            case let .launchedAgent(repo, sessionId):
                return ComposeTarget(repo: repo, agentId: sessionId)
            case let .pull(repo, number, _), let .review(repo, number):
                return ComposeTarget(repo: repo, pull: number)
            case let .issue(repo, number):
                return ComposeTarget(repo: repo, issue: number)
            case let .channel(id):
                return ComposeTarget(channel: id)
            case let .thread(channel, _):
                return ComposeTarget(channel: channel)
            case let .dm(user):
                return ComposeTarget(channel: "@\(user)")
            default:
                if let repo = route.repoRef {
                    return ComposeTarget(repo: repo)
                }
            }
        }
        if tab == .chat, !conversation.isEmpty {
            return ComposeTarget(channel: conversation)
        }
        return ComposeTarget()
    }

    public static func startsAgent(_ target: ComposeTarget) -> Bool {
        target.channel == nil
    }
}
