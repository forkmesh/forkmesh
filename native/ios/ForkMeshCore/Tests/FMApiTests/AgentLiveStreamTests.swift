import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMApi

final class AgentLiveFrameParserTests: XCTestCase {
    private func feed(_ lines: [String]) -> (events: [AgentLiveEvent], parser: AgentLiveFrameParser) {
        var parser = AgentLiveFrameParser()
        var events: [AgentLiveEvent] = []
        for line in lines {
            if let event = parser.feedLine(line) { events.append(event) }
        }
        return (events, parser)
    }

    func testAFullTextFrameThenAnAppendFrame() {
        let (events, parser) = feed([
            "retry: 1000",
            "",
            "event: live",
            #"data: {"at":1700000000000,"text":"Reading the code"}"#,
            "",
            "event: live",
            #"data: {"at":1700000002500,"append":"\n\n▸ Edit: app.py"}"#,
            "",
        ])
        XCTAssertEqual(events, [
            .text("Reading the code", atMs: 1_700_000_000_000),
            .append("\n\n▸ Edit: app.py", atMs: 1_700_000_002_500),
        ])
        XCTAssertEqual(parser.retryMs, 1000)
    }

    func testKeepalivesAreIgnored() {
        let (events, _) = feed([":", "", ": keepalive", "", "event: live", ": in between", #"data: {"at":5,"text":"x"}"#, ""])
        XCTAssertEqual(events, [.text("x", atMs: 5000)])
    }

    func testEndCarriesTheStatus() {
        let (events, _) = feed(["event: end", #"data: {"status":"completed"}"#, ""])
        XCTAssertEqual(events, [.end(status: "completed")])
    }

    func testEndWithoutDataStillEnds() {
        let (events, _) = feed(["event: end", ""])
        XCTAssertEqual(events, [.end(status: "")])
    }

    func testDataSplitOverSeveralLinesIsJoined() {
        let (events, _) = feed(["event: live", #"data: {"at":1,"#, #"data: "text":"two lines"}"#, ""])
        XCTAssertEqual(events, [.text("two lines", atMs: 1000)])
    }

    func testUnknownEventsAndUnnamedMessagesAreIgnored() {
        let (events, _) = feed([
            "event: progress", #"data: {"text":"no"}"#, "",
            #"data: {"text":"unnamed"}"#, "",
            "id: 7", "event: live", #"data: not json"#, "",
            "event: live", #"data: {"at":2,"other":"x"}"#, "",
            "event: live", #"data: {"at":3,"text":"yes"}"#, "",
        ])
        XCTAssertEqual(events, [.text("yes", atMs: 3000)])
    }

    func testAFrameWithoutItsBlankLineIsNotDispatched() {
        let (events, _) = feed(["event: live", #"data: {"text":"pending"}"#])
        XCTAssertEqual(events, [])
    }

    func testTheEventNameDoesNotLeakIntoTheNextFrame() {
        let (events, _) = feed(["event: end", "", #"data: {"status":"failed"}"#, ""])
        XCTAssertEqual(events, [.end(status: "")])
    }

    func testAMalformedRetryIsIgnored() {
        let (_, parser) = feed(["retry: soon", "retry: -5", "retry:"])
        XCTAssertNil(parser.retryMs)
    }

    func testBytesSplitAcrossChunksAndLineEndings() {
        let stream = "event: live\r\ndata: {\"at\":9,\"text\":\"héllo\"}\r\n\r\n: ping\revent: live\rdata: {\"append\":\" wörld\"}\r\revent: end\ndata: {\"status\":\"failed\"}\n\n"
        let bytes = Array(stream.utf8)
        for size in [1, 2, 3, 7, bytes.count] {
            var parser = AgentLiveFrameParser()
            var events: [AgentLiveEvent] = []
            var offset = 0
            while offset < bytes.count {
                let end = min(offset + size, bytes.count)
                events += parser.feed(Data(bytes[offset..<end]))
                offset = end
            }
            XCTAssertEqual(events, [
                .text("héllo", atMs: 9000),
                .append(" wörld", atMs: 0),
                .end(status: "failed"),
            ], "chunk size \(size)")
        }
    }

    func testLiveTextReplacesAndAppends() {
        var live = AgentLiveText()
        XCTAssertFalse(live.apply(.append("orphan", atMs: 1)))
        XCTAssertEqual(live.text, "")
        XCTAssertTrue(live.apply(.text("Hello", atMs: 10)))
        XCTAssertTrue(live.apply(.append(", world", atMs: 0)))
        XCTAssertEqual(live.text, "Hello, world")
        XCTAssertEqual(live.atMs, 10)
        XCTAssertTrue(live.apply(.append("!", atMs: 20)))
        XCTAssertEqual(live.atMs, 20)
        XCTAssertTrue(live.apply(.text("Fresh", atMs: 30)))
        XCTAssertEqual(live.text, "Fresh")
        XCTAssertFalse(live.apply(.end(status: "completed")))
    }
}

@MainActor
final class AgentLiveStreamRunTests: XCTestCase {
    private final class Script: @unchecked Sendable {
        var responses: [Result<HTTPStreamedResponse, Error>] = []
        var opened = 0
        var sleeps: [TimeInterval] = []
    }

    private static func response(_ chunks: [String], status: Int = 200, error: Error? = nil) -> HTTPStreamedResponse {
        let body = AsyncThrowingStream<Data, Error> { continuation in
            for chunk in chunks { continuation.yield(Data(chunk.utf8)) }
            continuation.finish(throwing: error)
        }
        return HTTPStreamedResponse(status: status, headers: ["Content-Type": "text/event-stream"], body: body)
    }

    private func stream(_ script: Script) -> AgentLiveStream {
        AgentLiveStream(
            open: {
                script.opened += 1
                if script.responses.isEmpty { throw CancellationError() }
                return try script.responses.removeFirst().get()
            },
            sleep: { seconds in script.sleeps.append(seconds) })
    }

    func testFramesReachTheScreenUntilTheRunEnds() async {
        let script = Script()
        script.responses = [.success(Self.response([
            "retry: 1000\n\nevent: live\ndata: {\"at\":1,\"text\":\"Hi\"}\n\n",
            ": keepalive\n\nevent: live\ndata: {\"at\":2,\"append\":\" there\"}\n\n",
            "event: end\ndata: {\"status\":\"completed\"}\n\n",
        ]))]
        var seen: [String] = []
        let outcome = await stream(script).run { seen.append($0.text) }
        XCTAssertEqual(outcome, .ended("completed"))
        XCTAssertEqual(seen, ["Hi", "Hi there"])
        XCTAssertEqual(script.opened, 1)
    }

    func testARefusalStopsAtOnce() async {
        for status in [401, 403, 404, 405] {
            let script = Script()
            script.responses = [.success(Self.response([], status: status))]
            let outcome = await stream(script).run { _ in XCTFail("no frames") }
            XCTAssertEqual(outcome, .refused(status))
            XCTAssertEqual(script.sleeps, [])
        }
    }

    func testAnUnavailableRelayIsAskedAgainLater() async {
        let script = Script()
        script.responses = [
            .success(Self.response([#"{"ok":false,"error":"live_unavailable","retryable":true}"#], status: 503)),
            .success(Self.response([], status: 429)),
            .success(Self.response(["event: end\ndata: {\"status\":\"completed\"}\n\n"])),
        ]
        let outcome = await stream(script).run { _ in XCTFail("no frames") }
        XCTAssertEqual(outcome, .ended("completed"))
        XCTAssertEqual(script.sleeps, [2, 4])
    }

    func testNetworkErrorsBackOffAndAStreamThatClosesReconnectsWithAFreshText() async {
        let script = Script()
        script.responses = [
            .failure(HTTPError.transport("offline")),
            .failure(HTTPError.transport("offline")),
            .failure(HTTPError.timedOut),
            .failure(HTTPError.transport("offline")),
            .failure(HTTPError.transport("offline")),
            .failure(HTTPError.transport("offline")),
            .success(Self.response(["retry: 500\n\nevent: live\ndata: {\"at\":1,\"text\":\"one\"}\n\n"])),
            .success(Self.response(["event: live\ndata: {\"append\":\" lost\"}\n\nevent: live\ndata: {\"at\":3,\"text\":\"one two\"}\n\n"],
                                   error: HTTPError.transport("reset"))),
            .success(Self.response(["event: end\ndata: {\"status\":\"failed\"}\n\n"])),
        ]
        var seen: [String] = []
        let outcome = await stream(script).run { seen.append($0.text) }
        XCTAssertEqual(outcome, .ended("failed"))
        XCTAssertEqual(seen, ["one", "one two"])
        XCTAssertEqual(script.sleeps, [2, 4, 8, 16, 30, 30, 0.5, 2])
    }

    func testAnEmptyStreamBacksOffInsteadOfSpinning() async {
        let script = Script()
        script.responses = [.success(Self.response([])), .success(Self.response([": hi\n\n"]))]
        let outcome = await stream(script).run { _ in }
        XCTAssertEqual(outcome, .cancelled)
        XCTAssertEqual(script.sleeps, [2, 4])
    }

    func testCancellationEndsTheFollow() async {
        let script = Script()
        let gate = AsyncThrowingStream<Data, Error> { continuation in
            continuation.yield(Data("event: live\ndata: {\"text\":\"x\"}\n\n".utf8))
        }
        script.responses = [.success(HTTPStreamedResponse(status: 200, body: gate))]
        let live = stream(script)
        let task = Task { @MainActor in await live.run { _ in } }
        try? await Task.sleep(nanoseconds: 50_000_000)
        task.cancel()
        let outcome = await task.value
        XCTAssertEqual(outcome, .cancelled)
    }

    func testTheRequestAsksForTheEventStreamOnTheSessionPath() async throws {
        let http = FakeHTTPClient { _ in
            HTTPResponse(status: 200, headers: ["Content-Type": "text/event-stream"], text: "event: end\ndata: {\"status\":\"completed\"}\n\n")
        }
        let api = ApiPortFixture.api(http)
        let owner = try await api.agentLiveStream("alice", "app", "sess 1")
        XCTAssertEqual(owner.status, 200)
        _ = try await api.agentLiveStream("acme", "app", "sess-2", orgScope: true)
        XCTAssertEqual(http.requests.map(portMethodPath), [
            "GET /api/repo/alice/app/agent-sessions/sess%201/live",
            "GET /api/orgs/acme/repos/app/agent-bots/sess-2/live",
        ])
        let first = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(first.headers["Accept"], "text/event-stream")
        XCTAssertEqual(first.headers["Authorization"], "Bearer session-token")
        XCTAssertNil(first.body)

        let outcome = await AgentLiveStream(open: { try await api.agentLiveStream("alice", "app", "sess-1") }).run { _ in }
        XCTAssertEqual(outcome, .ended("completed"))
    }
}
