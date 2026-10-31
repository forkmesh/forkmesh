import FMAccount
import FMBase
import Foundation

@MainActor
public final class InboxReadState {
    public static let key = "inbox/read.v1"
    public static let dismissedTasksKey = "inbox/dismissed-tasks.v1"
    public static let maxEntries = 400

    private let store: KeyValueStore
    private let key: String
    private var order: [String]
    private var set: Set<String>

    public init(store: KeyValueStore, key: String = InboxReadState.key) {
        self.store = store
        self.key = key
        order = Array((store.stringList(forKey: key) ?? []).suffix(InboxReadState.maxEntries))
        set = Set(order)
    }

    public func isRead(_ id: String) -> Bool { set.contains(id) }

    public func markRead(_ ids: [String]) {
        var changed = false
        for id in ids where !id.isEmpty && set.insert(id).inserted {
            order.append(id)
            changed = true
        }
        guard changed else { return }
        if order.count > InboxReadState.maxEntries {
            let drop = order.count - InboxReadState.maxEntries
            for id in order.prefix(drop) { set.remove(id) }
            order.removeFirst(drop)
        }
        store.set(order, forKey: key)
    }
}

public struct PingSyncOperation: Codable, Equatable, Hashable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case markRead, delete
    }

    public var kind: Kind
    public var remoteId: String
    public var attempts: Int
    public var queuedAtMs: Int64

    public init(kind: Kind, remoteId: String, attempts: Int = 0, queuedAtMs: Int64) {
        self.kind = kind
        self.remoteId = remoteId
        self.attempts = attempts
        self.queuedAtMs = queuedAtMs
    }
}

@MainActor
public final class PingSyncQueue {
    public static let key = "inbox/ping-sync.v1"
    public static let maxOperations = 200
    public static let maxAttempts = 5

    private let store: KeyValueStore
    public private(set) var operations: [PingSyncOperation]

    public init(store: KeyValueStore) {
        self.store = store
        if let text = store.string(forKey: PingSyncQueue.key), let data = text.data(using: .utf8),
           let decoded = try? JSONDecoder().decode([PingSyncOperation].self, from: data) {
            operations = Array(decoded.suffix(PingSyncQueue.maxOperations))
        } else {
            operations = []
        }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(operations), let text = String(data: data, encoding: .utf8) {
            store.set(text, forKey: PingSyncQueue.key)
        }
    }

    public func enqueue(_ kind: PingSyncOperation.Kind, remoteId: String, nowMs: Int64) {
        guard !remoteId.isEmpty else { return }
        if kind == .delete { operations.removeAll { $0.remoteId == remoteId } }
        if operations.contains(where: { $0.remoteId == remoteId && $0.kind == kind }) { return }
        operations.append(PingSyncOperation(kind: kind, remoteId: remoteId, queuedAtMs: nowMs))
        if operations.count > PingSyncQueue.maxOperations { operations.removeFirst(operations.count - PingSyncQueue.maxOperations) }
        save()
    }

    public func state(for remoteId: String) -> InboxSyncState? {
        guard let op = operations.first(where: { $0.remoteId == remoteId }) else { return nil }
        return op.attempts >= PingSyncQueue.maxAttempts ? .failed : .pending
    }

    public func isDeleted(_ remoteId: String) -> Bool {
        operations.contains { $0.remoteId == remoteId && $0.kind == .delete }
    }

    public func isMarkedRead(_ remoteId: String) -> Bool {
        operations.contains { $0.remoteId == remoteId && $0.kind == .markRead }
    }

    public func drain(_ send: (PingSyncOperation) async -> Bool) async -> Int {
        var sent = 0
        var remaining: [PingSyncOperation] = []
        for var op in operations {
            if op.attempts >= PingSyncQueue.maxAttempts {
                remaining.append(op)
                continue
            }
            if await send(op) {
                sent += 1
            } else {
                op.attempts += 1
                remaining.append(op)
            }
        }
        operations = remaining
        save()
        return sent
    }

    public func discardFailed() {
        operations.removeAll { $0.attempts >= PingSyncQueue.maxAttempts }
        save()
    }

    public func removeAll() {
        guard !operations.isEmpty else { return }
        operations = []
        save()
    }
}

public struct LocalPing: Codable, Equatable, Hashable, Sendable {
    public var id: String
    public var surface: String
    public var title: String
    public var message: String
    public var tsMs: Int64
    public var read: Bool
    public var kind: String?
    public var conversation: String?
}

@MainActor
public final class LocalPingLog {
    public static let key = "inbox/local-pings.v1"
    public static let maxEntries = 50
    public static let maxPerHour = 8
    public static let messageLimit = 700
    public static let chatKind = "chat"

    private let store: KeyValueStore
    private let clock: Clock
    private let makeId: () -> String
    public private(set) var pings: [LocalPing]
    public nonisolated let listeners = Listeners()

    public init(store: KeyValueStore, clock: Clock = SystemClock(), makeId: @escaping () -> String = { UUID().uuidString }) {
        self.store = store
        self.clock = clock
        self.makeId = makeId
        pings = LocalPingLog.stored(store)
    }

    private static func stored(_ store: KeyValueStore) -> [LocalPing] {
        guard let text = store.string(forKey: LocalPingLog.key), let data = text.data(using: .utf8),
            let decoded = try? JSONDecoder().decode([LocalPing].self, from: data)
        else { return [] }
        return Array(decoded.prefix(LocalPingLog.maxEntries))
    }

    public func reload() {
        let next = LocalPingLog.stored(store)
        if next == pings { return }
        pings = next
        listeners.notify()
    }

    private func save() {
        if let data = try? JSONEncoder().encode(pings), let text = String(data: data, encoding: .utf8) {
            store.set(text, forKey: LocalPingLog.key)
        }
    }

    public static func redact(_ text: String) -> String {
        var out = text
        let patterns = [
            "(?i)(bearer\\s+)[A-Za-z0-9._~+/=-]{8,}",
            "(?i)((?:token|secret|password|key|sig)[\"']?\\s*[:=]\\s*[\"']?)[^\\s\"'&,}]{4,}",
            "/(?:Users|home|var/mobile)/[^\\s\"']+",
        ]
        for pattern in patterns {
            guard let regex = try? NSRegularExpression(pattern: pattern) else { continue }
            let template = pattern.hasPrefix("/") ? "<path>" : "$1<redacted>"
            out = regex.stringByReplacingMatches(in: out, range: NSRange(out.startIndex..., in: out), withTemplate: template)
        }
        return String(out.prefix(messageLimit))
    }

    @discardableResult
    public func record(surface: String, title: String, message: String) -> LocalPing? {
        record(surface: surface, title: title, message: message, kind: nil, rateLimited: true)
    }

    @discardableResult
    public func recordNotice(surface: String, title: String, message: String, isError: Bool = false) -> LocalPing? {
        guard !message.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
        return record(surface: surface, title: title, message: message,
                      kind: isError ? "local_failure" : "local_notice", rateLimited: false)
    }

    private func record(surface: String, title: String, message: String, kind: String?, rateLimited: Bool) -> LocalPing? {
        let now = clock.nowMillis
        let clean = LocalPingLog.redact(message)
        let cleanTitle = String(LocalPingLog.redact(title).trimmingCharacters(in: .whitespacesAndNewlines).prefix(160))
        guard !cleanTitle.isEmpty else { return nil }
        if let index = pings.firstIndex(where: { $0.surface == surface && $0.title == cleanTitle && $0.message == clean }) {
            pings[index].tsMs = now
            pings[index].read = false
            pings[index].kind = kind
            let moved = pings.remove(at: index)
            pings.insert(moved, at: 0)
            save()
            listeners.notify()
            return moved
        }
        let recent = pings.filter { now - $0.tsMs < 3_600_000 }.count
        guard !rateLimited || recent < LocalPingLog.maxPerHour else { return nil }
        let ping = LocalPing(id: makeId(), surface: String(surface.prefix(60)), title: cleanTitle, message: clean, tsMs: now, read: false, kind: kind)
        pings.insert(ping, at: 0)
        if pings.count > LocalPingLog.maxEntries { pings.removeLast(pings.count - LocalPingLog.maxEntries) }
        save()
        listeners.notify()
        return ping
    }

    @discardableResult
    public func recordChat(
        messageId: String, sender: String, title: String, message: String, conversation: String, tsMs: Int64, read: Bool
    ) -> LocalPing? {
        let id = "chat:\(messageId)"
        let cleanTitle = String(title.trimmingCharacters(in: .whitespacesAndNewlines).prefix(160))
        guard !messageId.isEmpty, !cleanTitle.isEmpty, !pings.contains(where: { $0.id == id }) else { return nil }
        let ping = LocalPing(
            id: id, surface: String(sender.prefix(60)), title: cleanTitle, message: String(message.prefix(LocalPingLog.messageLimit)),
            tsMs: tsMs, read: read, kind: LocalPingLog.chatKind, conversation: conversation)
        pings.insert(ping, at: pings.firstIndex { $0.tsMs < tsMs } ?? pings.count)
        if pings.count > LocalPingLog.maxEntries { pings.removeLast(pings.count - LocalPingLog.maxEntries) }
        save()
        listeners.notify()
        return ping
    }

    public func removeChats(where matches: (String) -> Bool) {
        let before = pings.count
        pings.removeAll { $0.kind == LocalPingLog.chatKind && matches($0.conversation ?? "") }
        guard pings.count != before else { return }
        save()
        listeners.notify()
    }

    public func markConversationRead(_ conversation: String) {
        guard pings.contains(where: { !$0.read && $0.conversation == conversation }) else { return }
        for index in pings.indices where pings[index].conversation == conversation { pings[index].read = true }
        save()
        listeners.notify()
    }

    public func markRead(_ id: String) {
        guard let index = pings.firstIndex(where: { $0.id == id }), !pings[index].read else { return }
        pings[index].read = true
        save()
    }

    public func markAllRead() {
        guard pings.contains(where: { !$0.read }) else { return }
        for index in pings.indices { pings[index].read = true }
        save()
    }

    public func remove(_ id: String) {
        pings.removeAll { $0.id == id }
        save()
    }

    public func removeAll() {
        guard !pings.isEmpty else { return }
        pings = []
        save()
    }

    public var items: [InboxItem] {
        pings.map {
            InboxItem(
                id: "local:\($0.id)", source: .local, category: .ping, kind: $0.kind ?? "local_failure", title: $0.title,
                body: $0.message, actor: $0.surface, ts: Int($0.tsMs), unread: !$0.read, sync: .localOnly,
                route: $0.conversation.map { Route.channel(id: $0) }, remoteId: $0.id)
        }
    }
}
