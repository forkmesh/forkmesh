import FMBase
import FMChat
import FMCrypto
import Foundation
import XCTest

@testable import FMControl

final class DesktopTranscriptStreamTests: XCTestCase {
    @MainActor private func hub(_ desktop: FakeRelayDesktop) throws -> DesktopControlHub {
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        return DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
    }

    private func output(_ id: Int, _ text: String, from: Int, reset: Bool = false, status: String = "running") -> JSONObject {
        [
            "id": .int(Int64(id)), "transcript": .string(text), "from": .int(Int64(from)),
            "next": .int(Int64(from + text.utf16.count)), "reset": .bool(reset), "active": .bool(status == "running"),
            "status": .string(status),
        ]
    }

    @MainActor func testTheDesktopPushesOutputAsItIsWritten() async throws {
        let desktop = try FakeRelayDesktop()
        var followed: JSONObject = [:]
        var read: JSONObject = [:]
        desktop.handler = { method, params in
            if method == "agents.transcript" {
                read = params
                return self.output(7, String(repeating: "x", count: 32), from: 8)
            }
            guard method == "agents.follow" else { return nil }
            followed = params
            var answer = self.output(7, "abc", from: 3)
            answer["streaming"] = true
            return answer
        }
        let hub = try hub(desktop)
        var deltas: [AgentTranscriptDelta] = []
        var ended = ""
        let stream = DesktopTranscriptStream(hub: hub, sessionId: 7, since: 3, onEnded: { ended = $0 }) {
            deltas.append($0)
        }
        let streaming = await stream.start()
        XCTAssertTrue(streaming)
        XCTAssertEqual(followed.int("id"), 7)
        XCTAssertEqual(followed.int("since"), 3)
        XCTAssertEqual(deltas.map(\.text), ["abc"])

        desktop.event("agents.output", output(7, "de", from: 6))
        desktop.event("agents.output", output(8, "other run", from: 6))
        desktop.event("agents.output", output(7, "stale", from: 2))
        desktop.event("agents.output", output(7, "tail", from: 40))
        desktop.event("agents.output", output(7, "", from: 44, status: "completed"))
        let got = await waitFor { deltas.count == 5 }
        XCTAssertTrue(got)
        XCTAssertEqual(read.int("since"), 8, "a gap reads what was missed")
        XCTAssertEqual(deltas.map(\.text), ["abc", "de", String(repeating: "x", count: 32), "tail", ""])
        XCTAssertEqual(deltas.map(\.reset), [false, false, false, false, false])
        XCTAssertEqual(deltas.last?.status, "completed")
        XCTAssertEqual(stream.next, 44)

        desktop.event("agents.output", ["id": 7, "error": "unknown_session"])
        let over = await waitFor { ended == "unknown_session" }
        XCTAssertTrue(over)
        XCTAssertFalse(stream.isStreaming)
        let unfollowed = await waitFor {
            desktop.requests.contains { $0.str("method") == "agents.unfollow" && $0.obj("params").int("id") == 7 }
        }
        XCTAssertTrue(unfollowed)
    }

    @MainActor func testAStreamThatStopsLeavesTheDesktopFollowToTheOthers() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.handler = { method, _ in
            guard method == "agents.follow" else { return nil }
            var answer = self.output(7, "abc", from: 0)
            answer["streaming"] = true
            return answer
        }
        let hub = try hub(desktop)
        var page: [AgentTranscriptDelta] = []
        let list = DesktopTranscriptStream(hub: hub, sessionId: 7) { _ in }
        let run = DesktopTranscriptStream(hub: hub, sessionId: 7) { page.append($0) }
        let other = DesktopTranscriptStream(hub: hub, sessionId: 8) { _ in }
        _ = await list.start()
        _ = await run.start()
        _ = await other.start()
        list.stop()
        other.stop()
        let unfollowedOther = await waitFor {
            desktop.requests.contains { $0.str("method") == "agents.unfollow" && $0.obj("params").int("id") == 8 }
        }
        XCTAssertTrue(unfollowedOther)
        XCTAssertFalse(
            desktop.requests.contains { $0.str("method") == "agents.unfollow" && $0.obj("params").int("id") == 7 },
            "the run page still follows session 7")

        desktop.event("agents.output", output(7, "de", from: 3))
        let got = await waitFor { page.count == 2 }
        XCTAssertTrue(got)
        XCTAssertEqual(page.map(\.text), ["abc", "de"])

        run.stop()
        let unfollowed = await waitFor {
            desktop.requests.contains { $0.str("method") == "agents.unfollow" && $0.obj("params").int("id") == 7 }
        }
        XCTAssertTrue(unfollowed)
    }

    @MainActor func testAStreamAheadOfTheDesktopKeepsOnlyWhatIsNew() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.handler = { method, _ in
            guard method == "agents.follow" else { return nil }
            var answer = self.output(7, "abcdefgh", from: 0)
            answer["streaming"] = true
            return answer
        }
        let hub = try hub(desktop)
        var deltas: [AgentTranscriptDelta] = []
        let stream = DesktopTranscriptStream(hub: hub, sessionId: 7) { deltas.append($0) }
        _ = await stream.start()
        XCTAssertEqual(stream.next, 8)

        desktop.event("agents.output", output(7, "ghij", from: 6))
        desktop.event("agents.output", output(7, "j", from: 9))
        desktop.event("agents.output", output(7, "k", from: 10))
        let got = await waitFor { deltas.count == 3 }
        XCTAssertTrue(got)
        XCTAssertEqual(deltas.map(\.text), ["abcdefgh", "ij", "k"])
        XCTAssertEqual(deltas.map(\.from), [0, 8, 10])
        XCTAssertEqual(stream.next, 11)
        XCTAssertFalse(desktop.requests.contains { $0.str("method") == "agents.transcript" })
        stream.stop()
    }

    @MainActor func testRefollowPicksUpWhatAQuietFollowMissed() async throws {
        let desktop = try FakeRelayDesktop()
        var follows: [JSONObject] = []
        desktop.handler = { method, params in
            guard method == "agents.follow" else { return nil }
            follows.append(params)
            var answer = follows.count == 1 ? self.output(7, "abc", from: 0) : self.output(7, "reply", from: 3)
            answer["streaming"] = true
            return answer
        }
        let hub = try hub(desktop)
        var deltas: [AgentTranscriptDelta] = []
        let stream = DesktopTranscriptStream(hub: hub, sessionId: 7) { deltas.append($0) }
        let streaming = await stream.start()
        XCTAssertTrue(streaming)
        XCTAssertEqual(stream.next, 3)

        let again = await stream.refollow()
        XCTAssertTrue(again)
        XCTAssertEqual(follows.map { $0.int("since") }, [0, 3])
        XCTAssertEqual(deltas.map(\.text), ["abc", "reply"])
        XCTAssertEqual(stream.next, 8)
        XCTAssertFalse(desktop.requests.contains { $0.str("method") == "agents.unfollow" })

        desktop.event("agents.output", output(7, "!", from: 8))
        let got = await waitFor { deltas.count == 3 }
        XCTAssertTrue(got)
        XCTAssertEqual(stream.next, 9)
        stream.stop()
    }

    @MainActor func testRefollowReopensAnEndedFollow() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.handler = { method, _ in
            guard method == "agents.follow" else { return nil }
            var answer = self.output(7, "", from: 0, status: "completed")
            answer["streaming"] = true
            return answer
        }
        let hub = try hub(desktop)
        var ended = ""
        let stream = DesktopTranscriptStream(hub: hub, sessionId: 7, onEnded: { ended = $0 }) { _ in }
        _ = await stream.start()
        desktop.event("agents.output", ["id": 7, "error": "gone"])
        let over = await waitFor { ended == "gone" }
        XCTAssertTrue(over)
        XCTAssertFalse(stream.isStreaming)

        let again = await stream.refollow()
        XCTAssertTrue(again)
        XCTAssertTrue(stream.isStreaming)
        stream.stop()
    }

    @MainActor func testAnOlderDesktopLeavesTheCallerReading() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.refuse = { method, _ in method == "agents.follow" ? "unsupported" : nil }
        let hub = try hub(desktop)
        var deltas: [AgentTranscriptDelta] = []
        let stream = DesktopTranscriptStream(hub: hub, sessionId: 7) { deltas.append($0) }
        let streaming = await stream.start()
        XCTAssertFalse(streaming)
        XCTAssertTrue(deltas.isEmpty)
        stream.stop()
    }

    @MainActor func testTheFastestDesktopWinsTheRace() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                if target.name == "asleep" {
                    try await Task.sleep(nanoseconds: 3_000_000_000)
                    throw LocalControlException("timeout")
                }
                return try await RelayControlConnection.open(
                    channel: desktop, identity: identity, desktopKey: target.key, node: target.name)
            },
            paired: {
                [
                    PairedDesktop(key: "A" + String(repeating: "B", count: 42), name: "asleep", pairedAtMs: 2),
                    PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1),
                ]
            })
        var changes = 0
        hub.addLinkListener { changes += 1 }
        let started = Date()
        let answer = try await hub.request("status")
        XCTAssertEqual(answer.str("method"), "status")
        XCTAssertLessThan(Date().timeIntervalSince(started), 2, "a desktop that does not answer does not hold up one that does")
        XCTAssertEqual(hub.reach, .connected(.relay, "desk"))
        XCTAssertEqual(changes, 1)
        await hub.disconnectRelay()
        XCTAssertEqual(changes, 2)
    }
}
