// After native/android api/NotificationInbox.kt: the account's notification inbox ("Pings").
//
// There is no inbox timer. The account event channel delivers new pings as they happen and asks
// for a read on every (re)connect; beyond that the inbox is read when the app starts or returns
// to the foreground and when the reader pulls to refresh.

import FMAccount
import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class NotificationService {
    @ObservationIgnored let api: ApiService
    @ObservationIgnored let nodeName: @MainActor () -> String
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let snapshot: PingSnapshot?
    @ObservationIgnored public let listeners = Listeners()

    @ObservationIgnored private var started = false
    @ObservationIgnored private var inFlight: Task<Void, Never>?
    @ObservationIgnored private var inFlightId = 0
    @ObservationIgnored private var trailing: Task<Void, Never>?

    private var items: [ForkNotification] = []
    private var unread = 0
    private var unreadKnown = false
    private var total = 0
    private var isLoading = false
    private var errorText = ""
    private var activeNode = ""
    /// Bumped on every change, so views re-render even when only the node changed.
    public private(set) var revision = 0
    public private(set) var openRequest: String?
    public private(set) var celebration: ForkNotification?
    public private(set) var celebrationCount = 0
    @ObservationIgnored private var celebrated: Set<String> = []

    /// Ids of rows swiped away whose Worker delete has not answered yet.
    @ObservationIgnored private var deleting: Set<String> = []

    /// `nodeName` is the signed-in account's node name ("" when signed out), read at every use.
    public init(
        api: ApiService, nodeName: @escaping @MainActor () -> String, clock: Clock = SystemClock(),
        snapshot: PingSnapshot? = nil
    ) {
        self.api = api
        self.nodeName = nodeName
        self.clock = clock
        self.snapshot = snapshot
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    private func notifyListeners() {
        revision &+= 1
        listeners.notify()
    }

    private var node: String { nodeName().dartTrimmed.lowercased() }
    private var hasCurrentNode: Bool { node == activeNode }

    public var notifications: [ForkNotification] { hasCurrentNode ? items : [] }
    public var unreadCount: Int { hasCurrentNode ? unread : 0 }

    /// Whether `unreadCount` is the relay's answer for the signed-in account rather than the zero
    /// it starts from. The app icon badge follows the count only once this holds.
    public var unreadCountKnown: Bool { hasCurrentNode && unreadKnown }

    /// How many notifications the account holds server-side (the list is one capped page).
    public var inboxTotal: Int { hasCurrentNode ? total : 0 }
    public var loading: Bool { hasCurrentNode && isLoading }
    /// The last read's failure as display text (`dartErrorText`), or "".
    public var error: String { hasCurrentNode ? errorText : "" }
    public var hasLoaded: Bool { hasCurrentNode && (isLoading || !items.isEmpty || !errorText.isEmpty) }

    /// The shell is up: read the inbox once for this account.
    public func start() {
        started = true
        let nodeChanged = node != activeNode
        let loaded = hasLoaded
        adoptSnapshot()
        if nodeChanged || !loaded { Task { await self.refresh(silent: true, catchUp: true) } }
    }

    private func adoptSnapshot() {
        let node = self.node
        if node == activeNode { return }
        activeNode = node
        items = snapshot?.pings(for: node) ?? []
        unread = items.filter(\.isUnread).count
        unreadKnown = false
        total = items.count
        errorText = ""
        isLoading = false
        notifyListeners()
    }

    @discardableResult
    public func insertFromPush(_ ping: ForkNotification) -> ForkNotification? {
        if ping.id.isEmpty || node.isEmpty { return nil }
        adoptSnapshot()
        if let existing = items.first(where: { $0.id == ping.id }) { return existing }
        snapshot?.insert(ping, for: node)
        let index = items.firstIndex { $0.ts < ping.ts } ?? items.count
        items.insert(ping, at: index)
        if ping.isUnread { unread += 1 }
        total += 1
        notifyListeners()
        return ping
    }

    private func celebrateFresh() {
        let fresh = PingCelebration.fresh(items, seen: celebrated, now: Int(clock.nowMillis))
        guard let newest = fresh.max(by: { $0.ts < $1.ts }) else { return }
        celebrated.formUnion(fresh.map(\.id))
        celebration = newest
        celebrationCount &+= 1
    }

    public func requestOpen(_ id: String) {
        openRequest = id
        notifyListeners()
    }

    public func takeOpenRequest() -> String? {
        let id = openRequest
        openRequest = nil
        return id
    }

    /// Catches up once on whatever arrived while the app was in the background.
    public func appResumed() {
        if !started { return }
        Task { await self.refresh(silent: true, catchUp: true) }
    }

    /// Reads the account inbox. `silent` never flashes a spinner over a list already on screen. A
    /// `catchUp` ask is satisfied by a read already in flight; any other ask arriving mid-read runs
    /// one more read after it.
    public func refresh(silent: Bool = false, catchUp: Bool = false) async {
        if let running = inFlight, node == activeNode {
            if catchUp { return await running.value }
            if let trailing { return await trailing.value }
            let next = Task { @MainActor in
                await running.value
                self.trailing = nil
                await self.refresh(silent: silent)
            }
            trailing = next
            return await next.value
        }
        inFlightId += 1
        let id = inFlightId
        let read = Task { @MainActor in
            await self.performRefresh(silent: silent)
            if self.inFlightId == id { self.inFlight = nil }
        }
        inFlight = read
        await read.value
    }

    private func performRefresh(silent: Bool) async {
        let node = self.node
        adoptSnapshot()
        if node.isEmpty { return }
        let quiet = silent && (!items.isEmpty || !errorText.isEmpty)
        if !quiet {
            isLoading = true
            errorText = ""
            notifyListeners()
        }
        do {
            let page = try await api.notifications(node)
            if node != activeNode { return }
            // A refresh that lands while a swiped row's delete is in flight must not bring it back.
            let merged = snapshot?.save(page.notifications, for: node) ?? page.notifications
            items = deleting.isEmpty ? merged : merged.filter { !deleting.contains($0.id) }
            unread = page.unread
            unreadKnown = true
            total = page.total
            errorText = ""
            celebrateFresh()
        } catch {
            if node == activeNode { errorText = dartErrorText(error) }
        }
        if node == activeNode {
            isLoading = false
            notifyListeners()
        }
    }

    public func markAllRead() async throws {
        let node = self.node
        if node.isEmpty || node != activeNode || items.isEmpty { return }
        try await api.markNotificationsRead(node, all: true)
        let now = Int(clock.nowMillis)
        items = items.map { $0.copyWith(readAt: now) }
        unread = 0
        notifyListeners()
    }

    /// Deletes every notification in the account inbox (not recoverable; confirm first). Local
    /// state is emptied only after the Worker accepts.
    public func deleteAll() async throws {
        let node = self.node
        if node.isEmpty || node != activeNode || items.isEmpty { return }
        try await api.deleteAllNotifications(node)
        if node != activeNode { return }
        items = []
        unread = 0
        unreadKnown = true
        total = 0
        errorText = ""
        notifyListeners()
    }

    /// Deletes one notification (the swipe on a ping row). The row leaves at once; if the Worker
    /// refuses, it goes back where it was and the error is rethrown.
    public func delete(_ notification: ForkNotification) async throws {
        let id = notification.id
        let node = self.node
        if id.isEmpty || node.isEmpty || node != activeNode { return }
        guard let index = items.firstIndex(where: { $0.id == id }) else { return }
        let removed = items[index]
        deleting.insert(id)
        defer { deleting.remove(id) }
        items.remove(at: index)
        if removed.isUnread && unread > 0 { unread -= 1 }
        if total > 0 { total -= 1 }
        notifyListeners()
        do {
            try await api.deleteNotification(node, id)
        } catch {
            if node == activeNode && !items.contains(where: { $0.id == id }) {
                items.insert(removed, at: min(max(index, 0), items.count))
                if removed.isUnread { unread += 1 }
                total += 1
                notifyListeners()
            }
            throw error
        }
    }

    public func markRead(_ notification: ForkNotification) async throws {
        if !notification.isUnread || notification.id.isEmpty { return }
        let node = self.node
        if node.isEmpty || node != activeNode { return }
        applyRead(notification.id)
        try await api.markNotificationsRead(node, ids: [notification.id])
    }

    public func markReadSoon(_ notification: ForkNotification) {
        if !notification.isUnread || notification.id.isEmpty { return }
        let node = self.node
        if node.isEmpty || node != activeNode { return }
        applyRead(notification.id)
        let api = self.api
        Task { try? await api.markNotificationsRead(node, ids: [notification.id]) }
    }

    private func applyRead(_ id: String) {
        let now = Int(clock.nowMillis)
        items = items.map { $0.id == id ? $0.copyWith(readAt: now) : $0 }
        unread = items.filter(\.isUnread).count
        notifyListeners()
    }
}
