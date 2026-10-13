import FMAccount
import FMApi
import FMBase
import Foundation
import UserNotifications

@MainActor
enum NotificationActions {
    static var onOpenRoute: ((Route) -> Void)?
    static var onOpenLink: ((URL) -> Void)?
    static var onCompose: ((ComposeTarget) -> Void)?
    private(set) static var pendingRoute: Route?

    static func takePendingRoute() -> Route? {
        defer { pendingRoute = nil }
        return pendingRoute
    }

    static func categories() -> Set<UNNotificationCategory> {
        Set(NotificationCategoryId.allCases.map { (category: NotificationCategoryId) -> UNNotificationCategory in
            let actions: [UNNotificationAction] = category.actions.map(action)
            let options: UNNotificationCategoryOptions = category == .ping ? [] : [.customDismissAction]
            return UNNotificationCategory(
                identifier: category.rawValue, actions: actions, intentIdentifiers: [],
                hiddenPreviewsBodyPlaceholder: nil, categorySummaryFormat: nil, options: options)
        })
    }

    private static func action(_ id: NotificationActionId) -> UNNotificationAction {
        var options: UNNotificationActionOptions = []
        if id.requiresAuthentication {
            options.insert(.authenticationRequired)
        }
        if id.isDestructive {
            options.insert(.destructive)
        }
        if id.opensApp {
            options.insert(.foreground)
        }
        if id.isTextInput {
            return UNTextInputNotificationAction(
                identifier: id.rawValue, title: id.title, options: options,
                textInputButtonTitle: id == .reply ? "Send" : "Continue",
                textInputPlaceholder: id == .reply ? "Reply" : "What next?")
        }
        return UNNotificationAction(identifier: id.rawValue, title: id.title, options: options)
    }

    static func register() {
        UNUserNotificationCenter.current().setNotificationCategories(categories())
    }

    static func payload(category: String, userInfo: [AnyHashable: Any]) -> NotificationPayload {
        var fields: [String: String] = [:]
        for key in NotificationPayload.userInfoKeys {
            if let value = userInfo[key] as? String {
                fields[key] = value
            } else if let number = userInfo[key] as? NSNumber {
                fields[key] = number.stringValue
            }
        }
        return NotificationPayload(category: category, fields: fields)
    }

    static func handle(actionIdentifier: String, category: String, userInfo: [AnyHashable: Any], text: String?) async {
        let payload: NotificationPayload = payload(category: category, userInfo: userInfo)
        let command: NotificationCommand = NotificationActionRouter.command(actionId: actionIdentifier, payload: payload, text: text)
        await perform(command)
    }

    static func perform(_ command: NotificationCommand) async {
        do {
            switch command {
            case let .open(route, href):
                open(route: route, href: href)
            case let .markRead(alertId):
                try await markRead(alertId)
            case let .reply(channel, text):
                guard let sender = Services.get((any NotificationReplySending).self) else {
                    open(route: .dm(user: channel), href: "")
                    return
                }
                try await sender.reply(channel: channel, text: text)
            case let .approve(sessionId, approvalId), let .deny(sessionId, approvalId):
                let approve: Bool = {
                    if case .approve = command {
                        return true
                    }
                    return false
                }()
                guard let decider = Services.get((any AgentApprovalDeciding).self) else {
                    open(route: .approval(id: "\(sessionId)/\(approvalId)"), href: "")
                    return
                }
                try await decider.decide(sessionId: sessionId, approvalId: approvalId, approve: approve)
            case let .merge(sessionId):
                guard let acting = Services.get((any AgentSessionActing).self) else {
                    open(route: .agent(id: sessionId), href: "")
                    return
                }
                try await acting.mergeAndCleanUp(sessionId: sessionId)
            case let .continueRun(sessionId, text):
                guard let acting = Services.get((any AgentSessionActing).self), !text.isEmpty else {
                    open(route: .agent(id: sessionId), href: "")
                    return
                }
                try await acting.continueRun(sessionId: sessionId, text: text)
            case let .retry(sessionId):
                guard let acting = Services.get((any AgentSessionActing).self) else {
                    open(route: .agent(id: sessionId), href: "")
                    return
                }
                try await acting.retry(sessionId: sessionId)
            case let .compose(target):
                if let compose = onCompose {
                    compose(target)
                } else {
                    open(route: .compose(target), href: "")
                }
            case let .approvePull(repo, number):
                guard let inbox = Services.get(InboxService.self) else {
                    open(route: .pull(repo, number: number, tab: .conversation), href: "")
                    return
                }
                try await inbox.reviewPull(repo.owner, repo.name, number, "approve", "")
            case .ignore:
                return
            }
        } catch {
            InboxLocalPings.report(surface: "notification", title: "A notification action did not go through", message: "\(error)")
        }
    }

    static func open(route: Route?, href: String) {
        if let route = route {
            if let handler = onOpenRoute {
                handler(route)
            } else {
                pendingRoute = route
            }
            return
        }
        if let url = Services.get(ApiService.self)?.webLink(href) {
            onOpenLink?(url)
        }
    }

    private static func markRead(_ alertId: String) async throws {
        guard let api = Services.get(ApiService.self),
              let node = Services.get(AuthService.self)?.session?.nodeName, !node.isEmpty else {
            return
        }
        try await api.markNotificationsRead(node, ids: [alertId])
        await Services.get(NotificationService.self)?.refresh(silent: true)
    }
}

@MainActor
enum InboxLocalPings {
    static var log: LocalPingLog? {
        if let existing = Services.get(LocalPingLog.self) {
            return existing
        }
        guard let prefs = Services.get(KeyValueStore.self) else {
            return nil
        }
        return Services.put(LocalPingLog(store: prefs))
    }

    static func report(surface: String, title: String, message: String) {
        log?.record(surface: surface, title: title, message: message)
    }
}
