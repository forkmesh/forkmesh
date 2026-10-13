import FMAgentWatch
import FMControl
import Foundation
import Observation

@MainActor
public protocol WatchTranscriptStream: AnyObject {
    var isStreaming: Bool { get }
    @discardableResult func start() async -> Bool
    func stop()
}

extension DesktopTranscriptStream: WatchTranscriptStream {}

public struct ControlWatchText: Equatable, Sendable {
    public let text: String
    public let status: String
}

@MainActor
public final class ControlWatchFeeds {
    public typealias Opener = @MainActor (
        UnifiedAgentRow, @escaping @MainActor (String) -> Void, @escaping @MainActor (AgentTranscriptDelta) -> Void
    ) -> WatchTranscriptStream?

    public nonisolated static let defaultLimit = 6

    private struct Feed {
        let row: UnifiedAgentRow
        var follower = TranscriptFollower()
        var status = ""
        var streamed = false
        var ended = false
        var stream: WatchTranscriptStream?
    }

    private let open: Opener
    private let limit: Int
    private var feeds: [String: Feed] = [:]
    private var opened = 0
    private var order: [String] = []
    private var stopped = false
    public var onChange: (@MainActor () -> Void)?
    public private(set) var bytesIn = 0
    public private(set) var deltas = 0

    public init(limit: Int = ControlWatchFeeds.defaultLimit, open: @escaping Opener) {
        self.limit = limit
        self.open = open
    }

    public convenience init(_ center: AgentControlCenter, limit: Int = ControlWatchFeeds.defaultLimit) {
        self.init(limit: limit) { row, onEnded, onDelta in
            center.stream(row, since: 0, onEnded: onEnded, onDelta: onDelta)
        }
    }

    public func follow(_ rows: [UnifiedAgentRow]) {
        if stopped { return }
        for row in rows where row.isDesktop && row.localId != 0 && feeds[row.id] == nil {
            feeds[row.id] = Feed(row: row)
            order.append(row.id)
        }
        fill()
    }

    public var count: Int { feeds.count }

    public var live: Int {
        feeds.values.filter { !$0.ended && $0.stream?.isStreaming == true }.count
    }

    private func fill() {
        for id in order where opened < limit {
            guard var feed = feeds[id], feed.stream == nil, !feed.ended else { continue }
            guard let stream = open(
                feed.row,
                { [weak self] reason in self?.ended(id, reason) },
                { [weak self] delta in self?.apply(id, delta) })
            else { continue }
            feed.stream = stream
            feeds[id] = feed
            opened += 1
            Task { await stream.start() }
        }
    }

    public func text(_ id: String) -> ControlWatchText? {
        guard let feed = feeds[id], feed.streamed || feed.ended else { return nil }
        return ControlWatchText(text: feed.follower.text, status: feed.ended ? "done" : feed.status)
    }

    public func streaming(_ id: String) -> Bool {
        guard let feed = feeds[id] else { return false }
        return feed.ended || feed.stream?.isStreaming == true
    }

    public var lagging: Bool {
        feeds.values.contains { !$0.ended && $0.stream?.isStreaming != true }
    }

    public func stop() {
        if stopped { return }
        stopped = true
        for feed in feeds.values { feed.stream?.stop() }
        feeds.removeAll()
        order.removeAll()
        onChange = nil
    }

    private func apply(_ id: String, _ delta: AgentTranscriptDelta) {
        guard !stopped, var feed = feeds[id] else { return }
        feed.follower.apply(delta)
        if !delta.status.isEmpty { feed.status = delta.status }
        feed.streamed = true
        feeds[id] = feed
        bytesIn += delta.text.utf8.count
        deltas += 1
        onChange?()
    }

    private func ended(_ id: String, _ reason: String) {
        guard !stopped, var feed = feeds[id] else { return }
        if reason == "unknown_session" && !feed.ended {
            feed.ended = true
            if let stream = feed.stream {
                stream.stop()
                opened -= 1
            }
        }
        feeds[id] = feed
        fill()
        onChange?()
    }
}

@MainActor
public final class ControlWatchRoster {
    public let feeds: ControlWatchFeeds
    private let center: AgentControlCenter
    private let rows: @MainActor (AgentControlCenter) -> [UnifiedAgentRow]
    private let limit: Int
    private var watching = false

    public init(
        _ center: AgentControlCenter, feeds: ControlWatchFeeds? = nil, limit: Int = 24,
        rows: @escaping @MainActor (AgentControlCenter) -> [UnifiedAgentRow]
    ) {
        self.center = center
        self.feeds = feeds ?? ControlWatchFeeds(center)
        self.limit = limit
        self.rows = rows
    }

    public func targets() -> [AgentWatchTarget] {
        let chosen = Array(rows(center).prefix(limit))
        feeds.follow(chosen)
        return chosen.map { controlWatchTarget(center, $0, feeds: feeds) }
    }

    public func watch(_ onTargets: @escaping @MainActor ([AgentWatchTarget]) -> Void) {
        watching = true
        observe(onTargets)
    }

    public func stop() {
        watching = false
        feeds.stop()
    }

    private func observe(_ onTargets: @escaping @MainActor ([AgentWatchTarget]) -> Void) {
        if !watching { return }
        let center = self.center
        withObservationTracking {
            _ = center.rows
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                guard let self, self.watching else { return }
                onTargets(self.targets())
                self.observe(onTargets)
            }
        }
    }
}

@MainActor
public func controlWatchTarget(
    _ center: AgentControlCenter, _ row: UnifiedAgentRow, feeds: ControlWatchFeeds? = nil
) -> AgentWatchTarget {
    AgentWatchTarget(
        key: "control:\(row.id)",
        id: max(row.localId, 0),
        title: row.title,
        subtitle: row.repoLabel.isEmpty ? row.cwd : row.repoLabel,
        project: row.repoName.isEmpty ? (row.cwd.split(separator: "/").last.map(String.init) ?? "") : row.repoName,
        provider: row.provider,
        model: row.model,
        load: {
            let listed = await MainActor.run { center.rows.first { $0.id == row.id } }
            let current = listed?.status
            if let streamed = await feeds?.text(row.id) {
                let status = controlWatchStatus(listed: listed, streamed: streamed.status, fallback: row.status)
                return AgentWatchSnapshot(status: status, beats: beatsFromTranscript(streamed.text))
            }
            let delta = try await center.watchTranscript(row)
            let status = delta.status.isEmpty ? (current ?? row.status) : delta.status
            return AgentWatchSnapshot(status: status, beats: beatsFromTranscript(delta.text))
        })
}

public func controlWatchStatus(listed: UnifiedAgentRow?, streamed: String, fallback: String) -> String {
    if let listed, !listed.outside, !listed.status.isEmpty { return listed.status }
    if !streamed.isEmpty { return streamed }
    return listed?.status ?? fallback
}
