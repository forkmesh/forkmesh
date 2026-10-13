// View model of the Notifications screen: the filter, the body state,
// delete-all and swipe-to-delete with their refusals, and what a tapped ping
// opens. The inbox itself is NotificationService.

import FMBase
import Foundation
import Observation

/// Where a tapped ping goes (`_openNotification`).
public enum NotificationTapTarget: Equatable, Sendable {
    /// The repository record its link (or its repository) names.
    case repo(NotificationDeepLink)
    /// A web page; if it cannot be opened, show the ping (`.context`) after
    /// saying so.
    case web(URL)
    /// The ping written out in full, so a tap never does nothing.
    case context
}

@MainActor
@Observable
public final class NotificationsScreenModel {
    public let service: NotificationService
    public var filter = "All"
    /// The inline notice under the list's header; the view clears it.
    public var notice: String?
    public private(set) var noticeIsError = false
    /// Refused swipes per ping id: a row put back after a refused delete comes
    /// back under a new identity, since the swiped-away row may still be on
    /// its way out when the refusal lands.
    public private(set) var refusedSwipes: [String: Int] = [:]

    @ObservationIgnored private let webLink: (String) -> URL?
    @ObservationIgnored private var started = false

    public init(service: NotificationService, webLink: @escaping (String) -> URL? = { _ in nil }) {
        self.service = service
        self.webLink = webLink
    }

    public convenience init(service: NotificationService, api: ApiService) {
        self.init(service: service, webLink: { href in api.webLink(href) })
    }

    public var body: NotificationsBody {
        notificationsBody(loading: service.loading, error: service.error, all: service.notifications, filter: filter)
    }

    public var unreadLabel: String { notificationsUnreadLabel(service.unreadCount) }
    public var canMarkAllRead: Bool { FMApi.canMarkAllRead(service.unreadCount) }
    public var canDeleteAll: Bool { canDeleteAllNotifications(service.notifications.count) }
    public var deleteAllTitle: String { notificationsDeleteAllTitle(service.inboxTotal) }

    /// The row identity (`ValueKey('ping-<id>-<refusals>')`).
    public func rowKey(_ notification: ForkNotification) -> String {
        "ping-\(notification.id)-\(refusedSwipes[notification.id] ?? 0)"
    }

    /// First appearance: read the inbox (`didChangeDependencies`).
    public func start() async {
        if started { return }
        started = true
        await service.refresh()
    }

    public func refresh() async {
        await service.refresh()
    }

    /// A failure leaves the list as it was; the next refresh shows the
    /// truth.
    public func markAllRead() async {
        do {
            try await service.markAllRead()
        } catch {
            Log.warning("notifications", "mark all read failed: \(dartErrorText(error))")
        }
    }

    public func deleteAll() async {
        do {
            try await service.deleteAll()
            say(NotificationsCopy.deleted)
        } catch {
            say(notificationsDeleteErrorText(error), error: true)
        }
    }

    /// A swipe: the row is gone at once; a refusal puts it back under a new
    /// identity and says why.
    public func delete(_ notification: ForkNotification) async {
        do {
            try await service.delete(notification)
        } catch {
            refusedSwipes[notification.id, default: 0] += 1
            say(notificationDeleteErrorText(error), error: true)
        }
    }

    /// `_openNotification`: marks the ping read (best effort), then says what
    /// to open.
    public func open(_ notification: ForkNotification) async -> NotificationTapTarget {
        await markRead(notification)
        return target(notification)
    }

    public func target(_ notification: ForkNotification) -> NotificationTapTarget {
        if let link = notificationTapLink(notification) { return .repo(link) }
        if let web = webLink(notification.href) { return .web(web) }
        return .context
    }

    public func markRead(_ notification: ForkNotification) async {
        try? await service.markRead(notification)
    }

    public func current(_ notification: ForkNotification) -> ForkNotification {
        if notification.id.isEmpty { return notification }
        return service.notifications.first { $0.id == notification.id } ?? notification
    }

    public func deletePing(_ notification: ForkNotification) async -> String? {
        do {
            try await service.delete(notification)
            say(PingDetailCopy.deleted)
            return nil
        } catch {
            return notificationDeleteErrorText(error)
        }
    }

    /// The web page could not be opened.
    public func webOpenFailed(_ url: URL) {
        say("Could not open \(url.absoluteString)", error: true)
    }

    private func say(_ text: String, error: Bool = false) {
        noticeIsError = error
        notice = text
    }
}
