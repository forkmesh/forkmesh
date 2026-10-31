import FMAccount
import FMApi
import FMBase
import FMChat
import FMPackages
import SwiftUI
import UIKit

@MainActor
final class AppNavigator {
    static var shared: AppNavigator? { Services.get(AppNavigator.self) }
    static var early: [Route] = []

    let router: AppRouter
    let saved: SavedSearchStore
    private var lastURL: URL?
    private var lastHandledAt: Date = .distantPast

    init(router: AppRouter, saved: SavedSearchStore) {
        self.router = router
        self.saved = saved
    }

    static func install() -> AppNavigator {
        if let existing = shared {
            return existing
        }
        let settings: SettingsService? = Services.get(SettingsService.self)
        let router = AppRouter(project: "", accountReady: false)
        let navigator = AppNavigator(router: router, saved: SavedSearchStore(store: Services.get(KeyValueStore.self)))
        router.activate(project: settings?.relays.activeId ?? "")
        router.cover = "Launch"
        _ = Services.put(navigator)
        _ = Services.put(router)
        AppServices.current?.monitor.screenName = { [weak router] in router?.screenTitle ?? "" }
        navigator.wireNotifications()
        return navigator
    }

    var knownProjects: Set<String> {
        Set(Services.get(SettingsService.self)?.relays.relays.map(\.id) ?? [])
    }

    var me: String {
        Services.get(AuthService.self)?.session?.nodeName ?? ""
    }

    func accountChanged() {
        let auth: AuthService? = Services.get(AuthService.self)
        router.accountReady = (auth?.isAuthenticated ?? false)
    }

    @discardableResult
    func handle(url: URL) -> Bool {
        let now = Date()
        if url == lastURL && now.timeIntervalSince(lastHandledAt) < 2 {
            return true
        }
        lastURL = url
        lastHandledAt = now
        guard let link = RouteParser.parse(url, me: me) else {
            return false
        }
        switch router.open(link: link, knownProjects: knownProjects) {
        case let .switchProject(id):
            Task { @MainActor in
                _ = await Services.get(RelaySwitchController.self)?.switchTo(id)
            }
        case .unknownProject:
            UIApplication.shared.open(url, options: [UIApplication.OpenExternalURLOptionsKey.universalLinksOnly: false])
        case .waitingForSignIn:
            Task { @MainActor in
                if await AccountFeature.requireAccount(action: "open this link") {
                    self.accountChanged()
                }
            }
        case .opened, .ignored:
            break
        }
        return true
    }

    func openIncoming(_ url: URL) {
        if handle(url: url) { return }
        if url.scheme?.lowercased() == "https" {
            UIApplication.shared.open(url, options: [UIApplication.OpenExternalURLOptionsKey.universalLinksOnly: false])
        }
    }

    func open(_ route: Route) {
        router.show(route)
    }

    func perform(quickAction: ShellQuickAction, payload: String) {
        guard let route = quickAction.route(payload: payload) else {
            return
        }
        router.show(route)
    }

    func drainEarly() {
        let routes: [Route] = AppNavigator.early
        AppNavigator.early = []
        for route in routes {
            router.show(route)
        }
        if let route = NotificationActions.takePendingRoute() {
            router.show(route)
        }
    }

    private func wireNotifications() {
        NotificationActions.onOpenRoute = { (route: Route) in
            AppNavigator.shared?.open(route)
        }
        NotificationActions.onOpenLink = { (url: URL) in
            if AppNavigator.shared?.handle(url: url) != true {
                UIApplication.shared.open(url)
            }
        }
        NotificationActions.onCompose = { (target: ComposeTarget) in
            AppNavigator.shared?.router.requestCompose(target)
        }
    }

    func installChatIntercept() {
        router.intercept = { (route: Route) -> Bool in
            guard let services = AppServices.current else {
                return false
            }
            if let package = PackageGate.packageId(for: route), PhonePackages.isRemoved(package) {
                Services.get(AppRouter.self)?.open(.package(id: package))
                return true
            }
            let conversation: String
            switch route {
            case let .channel(id) where !services.routes.isRegistered(.channel):
                conversation = id
            case let .dm(user) where !services.routes.isRegistered(.dm):
                conversation = user.hasPrefix("@") || DirectMessageFrames.isDirect(user) ? user : "@\(user)"
            default:
                return false
            }
            guard let chat = Services.get(ShellChatBridge.self) else {
                return false
            }
            chat.switchConversation(conversation)
            if let router = Services.get(AppRouter.self) {
                router.popToRoot(.chat)
                router.focus(.chat)
            }
            return true
        }
    }
}

enum LaunchedSessions {
    @MainActor private static var sessions: [String: (Repository, AgentLaunchSession)] = [:]

    @MainActor static func remember(_ repo: Repository, _ session: AgentLaunchSession) -> Route {
        sessions[session.sessionId] = (repo, session)
        return .launchedAgent(RepoRef(owner: repo.owner, name: repo.name), sessionId: session.sessionId)
    }

    @MainActor static func lookup(_ repo: RepoRef, _ sessionId: String) -> (Repository, AgentLaunchSession) {
        if let known = sessions[sessionId] {
            return known
        }
        let repository = Repository(owner: repo.owner, name: repo.name)
        return (repository, AgentLaunchSession(sessionId: sessionId, owner: repo.owner, repo: repo.name))
    }
}
