import FMBase
import FMChat
import Foundation

public struct MiniAppIntent: Equatable, Sendable, Identifiable {
    public let id: String
    public let plugin: String
    public let method: String
    public let params: JSONObject
    public let createdMs: Int64
    public let expiresMs: Int64
    public var attempts: Int

    public init(id: String = UUID().uuidString, plugin: String, method: String, params: JSONObject = [:],
                createdMs: Int64, ttlMs: Int64 = MiniAppOutbox.defaultTTLMs, attempts: Int = 0) {
        self.id = id
        self.plugin = plugin
        self.method = method
        self.params = params
        self.createdMs = createdMs
        self.expiresMs = createdMs + ttlMs
        self.attempts = attempts
    }

    init?(_ object: JSONObject) {
        guard !object.str("id").isEmpty, !object.str("plugin").isEmpty, !object.str("method").isEmpty else { return nil }
        id = object.str("id")
        plugin = object.str("plugin")
        method = object.str("method")
        params = object.obj("params")
        createdMs = object.int64("createdMs")
        expiresMs = object.int64("expiresMs")
        attempts = object.int("attempts")
    }

    var json: JSONValue {
        ["id": .string(id), "plugin": .string(plugin), "method": .string(method), "params": .object(params),
         "createdMs": .int(createdMs), "expiresMs": .int(expiresMs), "attempts": .int(Int64(attempts))]
    }
}

public struct MiniAppFlushReport: Equatable, Sendable {
    public var sent: [String] = []
    public var dropped: [String: String] = [:]
    public var kept: [String] = []
}

public final class MiniAppOutbox: @unchecked Sendable {
    public static let defaultTTLMs: Int64 = 24 * 60 * 60 * 1000
    public static let maxIntents = 100
    public static let maxAttempts = 5
    static let key = "miniapps/outbox"

    private let store: KeyValueStore
    private let lock = NSLock()

    public init(store: KeyValueStore) { self.store = store }

    public var pending: [MiniAppIntent] {
        lock.lock()
        defer { lock.unlock() }
        return load()
    }

    private func load() -> [MiniAppIntent] {
        guard let text = store.string(forKey: Self.key), let value = try? JSON.parse(text) else { return [] }
        return (value.arrayValue ?? []).compactMap { $0.objectValue }.compactMap(MiniAppIntent.init)
    }

    private func save(_ intents: [MiniAppIntent]) {
        if intents.isEmpty {
            store.remove(Self.key)
        } else {
            store.set(JSON.encode(.array(intents.map(\.json))), forKey: Self.key)
        }
    }

    @discardableResult
    public func enqueue(_ intent: MiniAppIntent) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        var intents = load()
        guard intents.count < Self.maxIntents, !intents.contains(where: { $0.id == intent.id }) else { return false }
        intents.append(intent)
        save(intents)
        return true
    }

    public func flush(nowMs: Int64, call: (String, JSONObject) async throws -> JSONObject) async -> MiniAppFlushReport {
        var report = MiniAppFlushReport()
        var remaining: [MiniAppIntent] = []
        let intents = pending
        var stopped = false
        for var intent in intents {
            if intent.expiresMs <= nowMs {
                report.dropped[intent.id] = "expired"
                continue
            }
            if stopped {
                remaining.append(intent)
                report.kept.append(intent.id)
                continue
            }
            do {
                _ = try await call(MiniAppContract.method(intent.plugin, intent.method), intent.params)
                report.sent.append(intent.id)
            } catch {
                let code = MiniAppOutbox.code(of: error)
                intent.attempts += 1
                if MiniAppContract.isPermanent(code) {
                    report.dropped[intent.id] = code
                } else if intent.attempts >= Self.maxAttempts {
                    report.dropped[intent.id] = "gave_up"
                } else {
                    remaining.append(intent)
                    report.kept.append(intent.id)
                    stopped = true
                }
            }
        }
        lock.lock()
        let added = load().filter { candidate in !intents.contains(where: { $0.id == candidate.id }) }
        save(remaining + added)
        lock.unlock()
        return report
    }

    public static func code(of error: Error) -> String {
        if let coded = error as? MiniAppError { return coded.code }
        if let control = error as? LocalControlException { return control.code }
        return "transient"
    }
}

public struct MiniAppError: Error, Equatable, Sendable {
    public let code: String
    public init(_ code: String) { self.code = code }
}

public final class MiniAppSnapshots: @unchecked Sendable {
    private let store: KeyValueStore

    public init(store: KeyValueStore) { self.store = store }

    static func key(_ plugin: String) -> String { "miniapps/card/\(plugin)" }

    public func save(_ card: MiniAppCard, for plugin: String, atMs: Int64) {
        let value: JSONValue = ["card": .object(card.json), "fetchedMs": .int(atMs)]
        store.set(JSON.encode(value), forKey: Self.key(plugin))
    }

    public func load(_ app: MiniAppDescriptor) -> (card: MiniAppCard, fetchedMs: Int64)? {
        guard let text = store.string(forKey: Self.key(app.id)), let value = try? JSON.parse(text),
              let object = value.objectValue else { return nil }
        return (MiniAppCard(object.obj("card"), for: app), object.int64("fetchedMs"))
    }

    public func forget(_ plugin: String) { store.remove(Self.key(plugin)) }
}

public enum MiniAppReach: Equatable, Sendable {
    case live(transport: String)
    case desktopAway(lastSeenMs: Int64?)
    case unpaired

    public var canCall: Bool {
        if case .live = self { return true }
        return false
    }
}

public enum MiniAppBackgroundPlan {
    public static let refreshTaskIdentifier = "com.forkmesh.miniapps.refresh"
    public static let minimumIntervalSeconds = 15 * 60
    public static let wakeBudgetSeconds = 25

    public static func nextRefresh(cards: [MiniAppCard], pendingIntents: Int, now: Date) -> Date? {
        let intervals = cards.map(\.refreshSeconds).filter { $0 > 0 }
        guard !intervals.isEmpty || pendingIntents > 0 else { return nil }
        let wanted = intervals.min() ?? minimumIntervalSeconds
        return now.addingTimeInterval(TimeInterval(max(wanted, minimumIntervalSeconds)))
    }

    public static func stale(fetchedMs: Int64, card: MiniAppCard, nowMs: Int64) -> Bool {
        let window = Int64(max(card.refreshSeconds, MiniAppCard.minRefreshSeconds)) * 1000
        return card.refreshSeconds > 0 ? nowMs - fetchedMs > window : nowMs - fetchedMs > Int64(minimumIntervalSeconds) * 1000
    }
}
