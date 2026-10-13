import FMAgentWatch
import FMControl
import XCTest

@testable import FMAgents

@MainActor
private final class FakeStream: WatchTranscriptStream {
    var isStreaming = false
    var started = 0
    var stopped = 0

    func start() async -> Bool {
        started += 1
        isStreaming = true
        return true
    }

    func stop() {
        stopped += 1
        isStreaming = false
    }
}

@MainActor
final class ControlWatchFeedsTests: XCTestCase {
    private func row(_ id: String, localId: Int, badge: AgentTransportBadge = .desktopLan) -> UnifiedAgentRow {
        UnifiedAgentRow(
            id: id, badge: badge, title: id, repoOwner: "me", repoName: "app", node: "desk", status: "running",
            statusLabel: "Running", isActive: true, localId: localId)
    }

    func testStreamsDesktopRowsAndHandsWatchWhatTheyWrote() async {
        var streams: [String: FakeStream] = [:]
        var deltas: [String: @MainActor (AgentTranscriptDelta) -> Void] = [:]
        var ends: [String: @MainActor (String) -> Void] = [:]
        let feeds = ControlWatchFeeds(limit: 2) { row, onEnded, onDelta in
            let stream = FakeStream()
            streams[row.id] = stream
            deltas[row.id] = onDelta
            ends[row.id] = onEnded
            return stream
        }
        var changes = 0
        feeds.onChange = { changes += 1 }
        feeds.follow([
            row("d1", localId: 1), row("d2", localId: 2), row("d3", localId: 3),
            row("n1", localId: 0, badge: .node),
        ])
        feeds.follow([row("d1", localId: 1)])
        await Task.yield()
        XCTAssertEqual(streams.keys.sorted(), ["d1", "d2"])
        XCTAssertEqual(streams["d1"]?.started, 1)
        XCTAssertNil(feeds.text("d1"))
        XCTAssertTrue(feeds.lagging)

        deltas["d1"]?(AgentTranscriptDelta(text: "[Bash ls]\n", next: 10, reset: true, active: true, status: "running"))
        deltas["d1"]?(AgentTranscriptDelta(text: "[Read a]\n", next: 19, reset: false, active: true, status: ""))
        XCTAssertEqual(feeds.text("d1"), ControlWatchText(text: "[Bash ls]\n[Read a]\n", status: "running"))
        XCTAssertEqual(changes, 2)
        XCTAssertTrue(feeds.streaming("d1"))
        XCTAssertFalse(feeds.streaming("d3"))

        ends["d2"]?("unknown_session")
        XCTAssertEqual(feeds.text("d2")?.status, "done")
        XCTAssertTrue(feeds.streaming("d2"))
        XCTAssertTrue(feeds.lagging)

        feeds.stop()
        XCTAssertEqual(streams["d1"]?.stopped, 1)
        XCTAssertNil(feeds.text("d1"))
        deltas["d1"]?(AgentTranscriptDelta(text: "late", next: 23, reset: false, active: true, status: "running"))
        XCTAssertEqual(changes, 3)
    }

    func testAnEndedStreamHandsItsSlotToTheNextRowAndCountsWhatCameIn() async {
        var streams: [String: FakeStream] = [:]
        var deltas: [String: @MainActor (AgentTranscriptDelta) -> Void] = [:]
        var ends: [String: @MainActor (String) -> Void] = [:]
        let feeds = ControlWatchFeeds(limit: 1) { row, onEnded, onDelta in
            let stream = FakeStream()
            streams[row.id] = stream
            deltas[row.id] = onDelta
            ends[row.id] = onEnded
            return stream
        }
        feeds.follow([row("d1", localId: 1), row("d2", localId: 2)])
        await Task.yield()
        XCTAssertEqual(streams.keys.sorted(), ["d1"])
        XCTAssertEqual(feeds.count, 2)
        XCTAssertEqual(feeds.live, 1)
        deltas["d1"]?(AgentTranscriptDelta(text: "[Bash ls]\n", next: 10, reset: true, active: true, status: "running"))
        XCTAssertEqual(feeds.bytesIn, 10)
        XCTAssertEqual(feeds.deltas, 1)

        ends["d1"]?("unknown_session")
        XCTAssertEqual(streams["d1"]?.stopped, 1)
        XCTAssertEqual(streams.keys.sorted(), ["d1", "d2"])
        await Task.yield()
        XCTAssertEqual(streams["d2"]?.started, 1)
        XCTAssertEqual(feeds.live, 1)
        ends["d1"]?("unknown_session")
        XCTAssertEqual(streams["d1"]?.stopped, 1)
        feeds.stop()
    }

    func testTheListsStatusOutlivesTheLastStreamedOne() {
        var waiting = row("d1", localId: 1)
        waiting.status = "waiting_for_input"
        XCTAssertEqual(controlWatchStatus(listed: waiting, streamed: "running", fallback: "running"), "waiting_for_input")
        var restarted = row("d1", localId: 1)
        restarted.status = "running"
        XCTAssertEqual(controlWatchStatus(listed: restarted, streamed: "done", fallback: "done"), "running")
        var outside = row("o1", localId: 4)
        outside.outside = true
        XCTAssertEqual(controlWatchStatus(listed: outside, streamed: "waiting", fallback: "running"), "waiting")
        XCTAssertEqual(controlWatchStatus(listed: outside, streamed: "", fallback: "done"), "running")
        XCTAssertEqual(controlWatchStatus(listed: nil, streamed: "done", fallback: "running"), "done")
        XCTAssertEqual(controlWatchStatus(listed: nil, streamed: "", fallback: "running"), "running")
    }
}
