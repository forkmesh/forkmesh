import Foundation

@MainActor
public final class UserActivitySync {
    public static let interval: TimeInterval = 120
    public static let readInterval: TimeInterval = 600
    public static let forcedFloor: TimeInterval = 10

    public typealias Post = @MainActor (JSONObject) async throws -> (status: Int, body: JSONObject)

    private let log: UserActivityLog
    private let kind: String
    private let post: Post
    private let clock: Clock
    private var account = ""
    private var sentRevision = -1
    private var syncedAt = Date.distantPast
    private var busy = false

    public init(log: UserActivityLog, kind: String, clock: Clock = SystemClock(), post: @escaping Post) {
        self.log = log
        self.kind = kind
        self.clock = clock
        self.post = post
    }

    @discardableResult
    public func sync(account: String, force: Bool = false) async -> Bool {
        if account.isEmpty {
            reset()
            return false
        }
        if self.account.lowercased() != account.lowercased() {
            reset()
            self.account = account
        }
        if busy { return false }
        let now = clock.now
        let revision = log.revision
        let changed = revision != sentRevision
        if !changed && now.timeIntervalSince(syncedAt) < (force ? Self.forcedFloor : Self.readInterval) { return false }
        let day = log.today
        var payload: JSONObject = [
            "day": .string(day),
            "device": .string(log.deviceKey),
            "kind": .string(kind),
        ]
        if changed { payload["counts"] = .array(log.localCounts().map { .int(Int64($0)) }) }
        busy = true
        defer { busy = false }
        guard let result = try? await post(payload), (200..<300).contains(result.status),
            self.account == account,
            result.body.str("nodeName").lowercased() == account.lowercased(),
            let values = result.body["others"]?.arrayValue, values.count == ActivityGrid.slots
        else { return false }
        let kinds = (result.body["sources"]?.arrayValue ?? []).compactMap { (value: JSONValue) -> String? in
            guard let source = value.objectValue, source["self"]?.boolValue != true,
                (source["total"]?.intValue ?? 0) > 0
            else { return nil }
            return source.str("kind")
        }
        if changed { sentRevision = revision }
        syncedAt = now
        log.setOthers(day: day, counts: values.map { $0.intValue ?? 0 }, kinds: kinds)
        return true
    }

    public func reset() {
        account = ""
        sentRevision = -1
        syncedAt = .distantPast
        log.clearOthers()
    }
}
