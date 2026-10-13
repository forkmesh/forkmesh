import FMBase
import Foundation
import Observation

public struct AgentReadState: Equatable, Sendable {
    public static let seenKey = "agents/seenFinishedAt"
    public static let baselineKey = "agents/readBaselineMs"
    public static let limit = 400

    public var baselineMs: Int
    public var seen: [String: Int]

    public init(baselineMs: Int, seen: [String: Int] = [:]) {
        self.baselineMs = baselineMs
        self.seen = seen
    }

    public static func load(_ store: KeyValueStore, nowMs: Int) -> AgentReadState {
        let baseline: Int
        if let stored = store.int(forKey: baselineKey) {
            baseline = stored
        } else {
            baseline = nowMs
            store.set(nowMs, forKey: baselineKey)
        }
        var seen: [String: Int] = [:]
        for (key, value) in JSON.parseObject(store.string(forKey: seenKey) ?? "") {
            let ms = value.int(or: -1)
            if !key.isEmpty, ms >= 0 { seen[key] = ms }
        }
        return AgentReadState(baselineMs: baseline, seen: seen)
    }

    public func save(_ store: KeyValueStore) {
        var object = JSONObject()
        for key in seen.keys.sorted() {
            object[key] = .int(Int64(seen[key] ?? 0))
        }
        store.set(JSON.encode(object), forKey: Self.seenKey)
        store.set(baselineMs, forKey: Self.baselineKey)
    }

    public static func finishedAt(_ line: AgentLine) -> Int {
        if line.isActive { return 0 }
        switch line.status {
        case .running, .queued, .waiting: return 0
        default: break
        }
        return line.finishedAtMs > 0 ? line.finishedAtMs : max(line.updatedAtMs, 0)
    }

    public func hasSeen(_ line: AgentLine) -> Bool {
        guard let at = seen[line.readKey] else { return false }
        return at >= Self.finishedAt(line)
    }

    public func isUnread(_ line: AgentLine) -> Bool {
        if line.readKey.isEmpty || hasSeen(line) { return false }
        if let desktop = line.desktopUnread { return desktop }
        if line.status == .merged { return false }
        return Self.finishedAt(line) > baselineMs
    }

    @discardableResult
    public mutating func markRead(_ line: AgentLine) -> Bool {
        let at = Self.finishedAt(line)
        if line.readKey.isEmpty || (at <= 0 && line.desktopUnread != true) { return false }
        if let old = seen[line.readKey], old >= at { return false }
        seen[line.readKey] = at
        prune()
        return true
    }

    public mutating func prune(limit: Int = AgentReadState.limit) {
        guard seen.count > limit else { return }
        let kept = seen.sorted { a, b in a.value != b.value ? a.value > b.value : a.key < b.key }.prefix(max(limit, 0))
        seen = Dictionary(uniqueKeysWithValues: kept.map { ($0.key, $0.value) })
    }

    public func applied(_ lines: [AgentLine], open: String = "") -> [AgentLine] {
        lines.map { line in
            var marked = line
            marked.unread = (open.isEmpty || line.readKey != open) && isUnread(line)
            return marked
        }
    }
}

@MainActor
@Observable
public final class AgentReadTracker {
    public private(set) var state: AgentReadState
    public private(set) var openKey = ""

    @ObservationIgnored private let store: KeyValueStore?
    @ObservationIgnored private let markDesktop: @MainActor (Int) -> Void

    public init(
        store: KeyValueStore?, nowMs: Int = Int(Date().timeIntervalSince1970 * 1000),
        markDesktop: @escaping @MainActor (Int) -> Void = { _ in }
    ) {
        self.store = store
        self.markDesktop = markDesktop
        state = store.map { AgentReadState.load($0, nowMs: nowMs) } ?? AgentReadState(baselineMs: nowMs)
    }

    public func lines(_ lines: [AgentLine]) -> [AgentLine] {
        state.applied(lines, open: openKey)
    }

    public func open(_ line: AgentLine) {
        openKey = line.readKey
        record(line)
        if line.desktopSessionId > 0 { markDesktop(line.desktopSessionId) }
    }

    public func close(_ line: AgentLine) {
        if openKey == line.readKey { openKey = "" }
        let changed = record(line)
        if line.desktopSessionId > 0, changed || line.desktopUnread == true { markDesktop(line.desktopSessionId) }
    }

    @discardableResult
    private func record(_ line: AgentLine) -> Bool {
        var next = state
        guard next.markRead(line) else { return false }
        state = next
        if let store { next.save(store) }
        return true
    }
}
