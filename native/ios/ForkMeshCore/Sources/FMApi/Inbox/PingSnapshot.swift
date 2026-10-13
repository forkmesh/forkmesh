import FMBase
import Foundation

@MainActor
public final class PingSnapshot {
    public static let keyPrefix = "inbox/pings.v1/"
    public static let maxEntries = 100
    public static let pendingMs: Int64 = 10 * 60_000

    private struct Pending {
        let ping: ForkNotification
        let insertedAtMs: Int64
    }

    private let store: KeyValueStore
    private let clock: Clock
    private var pending: [String: [String: Pending]] = [:]

    public init(store: KeyValueStore, clock: Clock = SystemClock()) {
        self.store = store
        self.clock = clock
    }

    private static func account(_ node: String) -> String { node.dartTrimmed.lowercased() }

    public func pings(for node: String) -> [ForkNotification] {
        let account = PingSnapshot.account(node)
        if account.isEmpty { return [] }
        guard let text = store.string(forKey: PingSnapshot.keyPrefix + account),
              let rows = (try? JSON.parse(text))?.arrayValue else { return [] }
        return rows.compactMap(\.objectValue).map(ForkNotification.init(json:)).filter { !$0.id.isEmpty }
    }

    @discardableResult
    public func save(_ page: [ForkNotification], for node: String) -> [ForkNotification] {
        let account = PingSnapshot.account(node)
        if account.isEmpty { return page }
        let now = clock.nowMillis
        let fetched = Set(page.map(\.id))
        var waiting = pending[account] ?? [:]
        waiting = waiting.filter { !fetched.contains($0.key) && now - $0.value.insertedAtMs < PingSnapshot.pendingMs }
        pending[account] = waiting
        let merged = PingSnapshot.newestFirst(waiting.values.map(\.ping) + page)
        write(merged, account)
        return merged
    }

    @discardableResult
    public func insert(_ ping: ForkNotification, for node: String) -> Bool {
        let account = PingSnapshot.account(node)
        if account.isEmpty || ping.id.isEmpty { return false }
        var rows = pings(for: account)
        if rows.contains(where: { $0.id == ping.id }) { return false }
        let now = clock.nowMillis
        let since = ping.ts > 0 ? min(now, Int64(ping.ts)) : now
        pending[account, default: [:]][ping.id] = Pending(ping: ping, insertedAtMs: since)
        rows.insert(ping, at: 0)
        write(PingSnapshot.newestFirst(rows), account)
        return true
    }

    private static func newestFirst(_ rows: [ForkNotification]) -> [ForkNotification] {
        rows.enumerated().sorted { a, b in
            a.element.ts != b.element.ts ? a.element.ts > b.element.ts : a.offset < b.offset
        }.map(\.element)
    }

    private func write(_ rows: [ForkNotification], _ account: String) {
        let capped = Array(rows.prefix(PingSnapshot.maxEntries))
        store.set(JSON.encode(.array(capped.map { .object($0.json) })), forKey: PingSnapshot.keyPrefix + account)
    }
}
