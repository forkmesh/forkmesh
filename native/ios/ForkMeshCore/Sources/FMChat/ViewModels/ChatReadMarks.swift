import FMApi
import FMBase
import Foundation

@MainActor
public final class ChatReadMarks {
    public static let endpoint = "/api/chat/reads"
    public static let topic = "chat-read"
    public static let postMax = 50
    static let futureSlackMs: Int64 = 24 * 60 * 60 * 1_000

    let api: ChatAccountCalling
    let clock: Clock
    let delay: TimeInterval
    let sleeper: @Sendable (TimeInterval) async throws -> Void
    public private(set) var marks: [String: Int64] = [:]
    private var pending: [String: Int64] = [:]
    private var flushTask: Task<Void, Never>?

    public init(
        api: ChatAccountCalling, clock: Clock = SystemClock(), delay: TimeInterval = 1,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.api = api
        self.clock = clock
        self.delay = delay
        self.sleeper = sleep
    }

    public nonisolated static func isSyncable(_ key: String) -> Bool {
        let hex = Set("0123456789abcdef")
        for prefix in ["private:", "direct:"] where key.hasPrefix(prefix) {
            let id = key.dropFirst(prefix.count)
            return id.count == 32 && id.allSatisfy { hex.contains($0) }
        }
        guard key.hasPrefix("#") else { return false }
        let name = key.dropFirst()
        let allowed = Set("abcdefghijklmnopqrstuvwxyz0123456789_-")
        guard let first = name.first, first != "_", first != "-" else { return false }
        return name.count <= 64 && name.allSatisfy { allowed.contains($0) }
    }

    public func mark(for key: String) -> Int64 { marks[key] ?? 0 }

    public func isRead(_ key: String, _ timestamp: Date) -> Bool {
        let ts = timestamp.epochMillis
        return ts > 0 && ts <= mark(for: key)
    }

    public func queue(_ key: String, _ timestamp: Date) {
        let ts = timestamp.epochMillis
        guard ChatReadMarks.isSyncable(key), ts > mark(for: key), ts > pending[key] ?? 0,
            ts <= clock.now.epochMillis + ChatReadMarks.futureSlackMs
        else { return }
        pending[key] = ts
        if flushTask != nil { return }
        flushTask = Task { [weak self] in
            guard let self else { return }
            try? await self.sleeper(self.delay)
            await self.flush()
        }
    }

    public func flush() async {
        flushTask = nil
        let batch = Dictionary(
            uniqueKeysWithValues: pending.sorted { $0.key < $1.key }.prefix(ChatReadMarks.postMax).map { ($0.key, $0.value) })
        pending.removeAll()
        if batch.isEmpty { return }
        for (key, ts) in batch { marks[key] = max(mark(for: key), ts) }
        var body: JSONObject = [:]
        for (key, ts) in batch { body[key] = .int(ts) }
        _ = try? await api.accountApiCall("POST", ChatReadMarks.endpoint, ["marks": .object(body)])
    }

    @discardableResult
    public func fetch() async -> Bool {
        guard let payload = try? await api.accountApiCall("GET", ChatReadMarks.endpoint, nil),
            let listed = payload["marks"]?.objectValue
        else { return false }
        for key in listed.keys {
            let ts = listed[key]?.int64Value ?? 0
            if ts > mark(for: key) { marks[key] = ts }
        }
        return true
    }

    public func clear() {
        flushTask?.cancel()
        flushTask = nil
        pending.removeAll()
        marks.removeAll()
    }
}
