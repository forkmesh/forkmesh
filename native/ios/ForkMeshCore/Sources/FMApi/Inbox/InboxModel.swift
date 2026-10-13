import FMAccount
import FMBase
import Foundation
import Observation

@MainActor
public protocol InboxClient: AnyObject {
    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject
    func tasksApi(_ method: String, _ suffix: String, _ body: JSONObject?) async throws -> JSONObject
}

extension ApiService: InboxClient {}

public enum PingPauseChoice: CaseIterable, Sendable {
    case oneHour, oneDay, oneWeek

    public var label: String {
        switch self {
        case .oneHour: return "For 1 hour"
        case .oneDay: return "For 1 day"
        case .oneWeek: return "Until next week"
        }
    }

    public var durationMs: Int64 {
        switch self {
        case .oneHour: return 3_600_000
        case .oneDay: return 86_400_000
        case .oneWeek: return 7 * 86_400_000
        }
    }
}

@MainActor
@Observable
public final class InboxModel {
    public static let pingPageSize = 100
    public static let maxPauseMs: Int64 = 30 * 86_400_000

    public private(set) var items: [InboxItem] = []
    public private(set) var loading = false
    public private(set) var errors: [String] = []
    public private(set) var pauseUntilMs: Int64 = 0
    public private(set) var loaded = false
    public var filter: InboxFilter
    public var unreadOnly = false

    @ObservationIgnored private let client: InboxClient
    @ObservationIgnored private let node: @MainActor () -> String
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored public let readState: InboxReadState
    @ObservationIgnored public let dismissedTasks: InboxReadState
    @ObservationIgnored public let syncQueue: PingSyncQueue
    @ObservationIgnored public let localPings: LocalPingLog
    @ObservationIgnored private let snapshot: PingSnapshot?
    @ObservationIgnored public var onChanged: (@MainActor () -> Void)?
    @ObservationIgnored private var pingItems: [InboxItem] = []
    @ObservationIgnored private var taskItems: [InboxItem] = []
    @ObservationIgnored private var running: Task<Void, Never>?
    @ObservationIgnored private var loadedAtMs: Int64 = 0
    @ObservationIgnored nonisolated(unsafe) private var localToken: ListenerToken?

    public init(client: InboxClient, node: @escaping @MainActor () -> String, store: KeyValueStore,
                filter: InboxFilter = .all, clock: Clock = SystemClock(), localPings: LocalPingLog? = nil,
                snapshot: PingSnapshot? = nil) {
        self.client = client
        self.node = node
        self.clock = clock
        self.filter = filter
        readState = InboxReadState(store: store)
        dismissedTasks = InboxReadState(store: store, key: InboxReadState.dismissedTasksKey)
        syncQueue = PingSyncQueue(store: store)
        self.localPings = localPings ?? LocalPingLog(store: store, clock: clock)
        self.snapshot = snapshot
        if let snapshot, !account.isEmpty {
            pingItems = snapshot.pings(for: account).compactMap(pingItem)
            rebuild()
        }
        localToken = self.localPings.listeners.add { [weak self] in
            MainActor.assumeIsolated { self?.rebuild() }
        }
    }

    deinit {
        if let localToken { localPings.listeners.remove(localToken) }
    }

    private func pingItem(_ ping: ForkNotification) -> InboxItem? {
        if syncQueue.isDeleted(ping.id) { return nil }
        var item = InboxItem(ping: ping)
        if syncQueue.isMarkedRead(ping.id) { item.unread = false }
        if let state = syncQueue.state(for: ping.id) { item.sync = state }
        return item
    }

    public func item(forPing ping: ForkNotification) -> InboxItem? {
        if let existing = items.first(where: { $0.source == .ping && $0.remoteId == ping.id }) { return existing }
        guard let item = pingItem(ping) else { return nil }
        pingItems.insert(item, at: 0)
        rebuild()
        return item
    }

    public var visible: [InboxItem] { InboxFeed.filter(items, filter, unreadOnly: unreadOnly) }
    public var unreadCounts: [InboxFilter: Int] { InboxFeed.unreadCounts(items) }
    public var unreadTotal: Int { unreadCounts[.all] ?? 0 }
    public var isPaused: Bool { pauseUntilMs > clock.nowMillis }
    public var pendingSyncCount: Int { syncQueue.operations.count }
    public var canDeleteAllPings: Bool { !items.isEmpty }

    public var pauseLabel: String {
        guard isPaused else { return "" }
        let minutes = (pauseUntilMs - clock.nowMillis + 59_999) / 60_000
        func phrase(_ count: Int64, _ unit: String) -> String { "Pings paused for \(count) more \(unit)\(count == 1 ? "" : "s")" }
        if minutes < 60 { return phrase(minutes, "minute") }
        let hours = (minutes + 59) / 60
        if hours < 48 { return phrase(hours, "hour") }
        return phrase((hours + 23) / 24, "day")
    }

    private var account: String { node().trimmingCharacters(in: .whitespaces).lowercased() }

    private func rebuild() {
        items = InboxFeed.merge(pings: pingItems, tasks: taskItems, local: localPings.items)
    }

    public func refreshLocal() { rebuild() }

    public static let freshMs: Int64 = 15_000

    public func load() async {
        let task = Task { await self.fetch() }
        running = task
        await task.value
        if running == task { running = nil }
    }

    public func loadIfStale() async {
        if let running {
            await running.value
            return
        }
        if loaded && clock.nowMillis - loadedAtMs < InboxModel.freshMs { return }
        await load()
    }

    private func fetch() async {
        let me = account
        guard !me.isEmpty else {
            items = localPings.items
            errors = ["Sign in to see your inbox."]
            return
        }
        loading = true
        defer {
            loading = false
            loaded = true
            loadedAtMs = clock.nowMillis
        }
        var problems: [String] = []
        _ = await syncQueue.drain { op in await self.send(op, node: me) }
        do {
            let encoded = DartURI.encodeComponent(me)
            let data = try await client.accountApiCall("GET", "/api/notifications?node=\(encoded)&limit=\(InboxModel.pingPageSize)", nil)
            let page = NotificationPage(json: data)
            pauseUntilMs = data.int64("pauseUntil")
            let pings = snapshot?.save(page.notifications, for: me) ?? page.notifications
            pingItems = pings.compactMap(pingItem)
        } catch {
            problems.append("Pings could not be loaded.")
        }
        do {
            let data = try await client.tasksApi("GET", "", nil)
            let actor = data.str("actor").isEmpty ? me : data.str("actor").lowercased()
            taskItems = data.maps("tasks").compactMap { InboxFeed.taskItem($0, actor: actor) }
                .filter { !dismissedTasks.isRead($0.id) }.map { item in
                var copy = item
                copy.unread = !readState.isRead(item.id)
                return copy
            }
        } catch let error as OrgApiException where ["org_member_required", "forbidden", "http_403", "http_404"].contains(error.code) {
            taskItems = []
        } catch {
            problems.append("Assigned tasks could not be loaded.")
        }
        errors = problems
        rebuild()
    }

    private func send(_ op: PingSyncOperation, node: String) async -> Bool {
        do {
            switch op.kind {
            case .markRead:
                _ = try await client.accountApiCall("POST", "/api/notifications", ["node": .string(node), "ids": .array([.string(op.remoteId)])])
            case .delete:
                _ = try await client.accountApiCall("DELETE", "/api/notifications", ["node": .string(node), "id": .string(op.remoteId)])
            }
            return true
        } catch {
            return false
        }
    }

    private func update(_ id: String, _ change: (inout InboxItem) -> Void) {
        if let index = pingItems.firstIndex(where: { $0.id == id }) { change(&pingItems[index]) }
        if let index = taskItems.firstIndex(where: { $0.id == id }) { change(&taskItems[index]) }
        rebuild()
    }

    public func markReadLocally(_ item: InboxItem) {
        if item.unread { update(item.id) { $0.unread = false } }
    }

    public func markRead(_ item: InboxItem) async {
        guard item.unread else { return }
        switch item.source {
        case .local:
            localPings.markRead(item.remoteId)
            rebuild()
        case .task:
            readState.markRead([item.id])
            update(item.id) { $0.unread = false }
        case .ping:
            update(item.id) { $0.unread = false }
            let me = account
            if !(await send(PingSyncOperation(kind: .markRead, remoteId: item.remoteId, queuedAtMs: clock.nowMillis), node: me)) {
                syncQueue.enqueue(.markRead, remoteId: item.remoteId, nowMs: clock.nowMillis)
                update(item.id) { $0.sync = .pending }
            }
            onChanged?()
        }
    }

    public func markAllRead() async {
        let me = account
        readState.markRead(taskItems.map(\.id))
        localPings.markAllRead()
        let unreadPings = pingItems.filter(\.unread)
        for index in pingItems.indices { pingItems[index].unread = false }
        for index in taskItems.indices { taskItems[index].unread = false }
        rebuild()
        guard !unreadPings.isEmpty else { return }
        do {
            _ = try await client.accountApiCall("POST", "/api/notifications", ["node": .string(me), "all": true])
        } catch {
            for item in unreadPings { syncQueue.enqueue(.markRead, remoteId: item.remoteId, nowMs: clock.nowMillis) }
        }
        onChanged?()
    }

    public func delete(_ item: InboxItem) async {
        switch item.source {
        case .local:
            localPings.remove(item.remoteId)
            rebuild()
        case .task:
            readState.markRead([item.id])
            dismissedTasks.markRead([item.id])
            taskItems.removeAll { $0.id == item.id }
            rebuild()
        case .ping:
            pingItems.removeAll { $0.id == item.id }
            rebuild()
            if !(await send(PingSyncOperation(kind: .delete, remoteId: item.remoteId, queuedAtMs: clock.nowMillis), node: account)) {
                syncQueue.enqueue(.delete, remoteId: item.remoteId, nowMs: clock.nowMillis)
            }
            onChanged?()
        }
    }

    @discardableResult
    public func deleteAllPings() async -> String? {
        let me = account
        if !me.isEmpty {
            do {
                _ = try await client.accountApiCall("DELETE", "/api/notifications", ["node": .string(me), "all": true])
            } catch {
                return "Pings could not be deleted."
            }
            pingItems = []
            syncQueue.removeAll()
        }
        readState.markRead(taskItems.map(\.id))
        dismissedTasks.markRead(taskItems.map(\.id))
        taskItems = []
        localPings.removeAll()
        rebuild()
        onChanged?()
        return nil
    }

    public func item(remoteId: String) -> InboxItem? { items.first { $0.remoteId == remoteId } }

    @discardableResult
    public func pause(_ choice: PingPauseChoice?) async -> String? {
        let now = clock.nowMillis
        let until = choice.map { now + min($0.durationMs, InboxModel.maxPauseMs) } ?? 0
        do {
            let data = try await client.accountApiCall("POST", "/api/notifications", ["node": .string(account), "pauseUntil": .int(until)])
            pauseUntilMs = data.contains("pauseUntil") ? data.int64("pauseUntil") : until
            return nil
        } catch {
            return "Pings could not be \(choice == nil ? "resumed" : "paused")."
        }
    }

    public func prompt(for item: InboxItem) -> ComposeTarget {
        let text: String
        if item.source == .ping {
            let ping = ForkNotification(
                id: item.remoteId, kind: item.kind, title: item.title, body: item.body, repo: item.repo, href: item.href,
                actor: item.actor, ts: item.ts)
            text = pingAgentPrompt(ping)
        } else {
            text = [item.title, item.body].filter { !$0.isEmpty }.joined(separator: "\n\n")
        }
        return ComposeTarget(repo: RepoRef(slug: item.repo), text: text)
    }
}
