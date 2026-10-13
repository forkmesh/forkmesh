import Foundation
import XCTest

@testable import FMWatchWire
import FMAgentWatch
import FMBase

final class WireBeatTests: XCTestCase {
    func testRoundTripsTheSonifiersBeats() throws {
        let beats = beatsFromTranscript("==> Started\n> Fix it\n[Bash pytest -q]\nDone, **fixed**.\n")
        XCTAssertEqual(beats.map(\.motion), [.start, .prompt, .test, .speak])
        let wire = beats.map(WireBeat.init)
        let data = try JSONEncoder().encode(wire)
        let back = try JSONDecoder().decode([WireBeat].self, from: data)
        XCTAssertEqual(back, wire)
        XCTAssertEqual(back.map(\.watchBeat), beats)
        XCTAssertEqual(back.map(\.seed), beats.map(\.seed))
        XCTAssertEqual(back[3].said, "Done, fixed.")
        // Motions travel by name.
        XCTAssertTrue(String(decoding: data, as: UTF8.self).contains("\"m\":\"test\""))
    }

    func testEveryMotionRoundTripsByName() {
        for motion in AgentMotion.allCases {
            XCTAssertEqual(WireBeat.motion(named: motion.name), motion)
        }
        XCTAssertEqual(WireBeat.motion(named: "teleport"), .tick)
    }

    func testHaptics() {
        XCTAssertEqual(WireBeat(.fail, "x").haptic, .failure)
        XCTAssertEqual(WireBeat(.write, "x").haptic, .directionUp)
        XCTAssertNil(WireBeat(.usage, "x").haptic)
    }
}

final class WatchWireTests: XCTestCase {
    func frame(_ key: String, _ status: WireStatus, beats: Int = 0, updated: Int64 = 0) -> AgentFrame {
        AgentFrame(
            key: key, title: "Task \(key)", repo: "alice/app", status: status, model: "claude-opus-5-5",
            turns: 3, tokens: 1200, contextTokens: 50, contextWindow: 200, updatedAtMs: updated,
            controls: [.stop, .prompt],
            beats: (0..<beats).map { WireBeat(.run, "Bash step \($0) " + String(repeating: "x", count: 60)) })
    }

    func testSnapshotRoundTripsThroughEnvelope() throws {
        let snapshot = WatchSnapshot(generatedAtMs: 42, frames: [frame("a", .running, beats: 3)])
        let dictionary = try WatchWire.envelope(.snapshot, snapshot)
        XCTAssertEqual(WatchWire.kind(of: dictionary), .snapshot)
        let back = try WatchWire.open(.snapshot, dictionary, as: WatchSnapshot.self)
        XCTAssertEqual(back, snapshot)
        XCTAssertEqual(back.frames[0].face, "Opus")
        XCTAssertEqual(back.frames[0].contextFraction, 0.25)
        XCTAssertEqual(back.frames[0].controls, [.stop, .prompt])
    }

    func testEnvelopeRejectsOtherKindsAndNewerVersions() throws {
        let dictionary = try WatchWire.envelope(.reply, WatchReply.done("ok"))
        XCTAssertThrowsError(try WatchWire.open(.snapshot, dictionary, as: WatchSnapshot.self))
        var newer = dictionary
        newer["v"] = WatchWire.version + 1
        XCTAssertThrowsError(try WatchWire.open(.reply, newer, as: WatchReply.self))
        XCTAssertThrowsError(try WatchWire.open(.reply, ["x": 1], as: WatchReply.self))
    }

    func testCommandsRoundTrip() throws {
        let commands: [WatchCommand] = [
            .refresh, .watch(key: nil), .watch(key: "desktop:a/b:1"), .stop(key: "k"), .continue(key: "k"),
            .prompt(key: "k", text: "run the tests"), .retry(key: "q"), .listen(on: true),
            .start(text: "fix the login bug", repo: nil), .start(text: "add tests", repo: "alice/mobile"),
        ]
        for command in commands {
            let back = try WatchWire.open(.command, try WatchWire.envelope(.command, command), as: WatchCommand.self)
            XCTAssertEqual(back, command)
        }
        let bad = try JSONSerialization.data(withJSONObject: ["op": "stop"])
        XCTAssertThrowsError(try JSONDecoder().decode(WatchCommand.self, from: bad))
        XCTAssertNil(WatchCommand.start(text: "go", repo: nil).key)
        let unknown = try JSONSerialization.data(withJSONObject: ["op": "pause", "key": "k"])
        XCTAssertThrowsError(try JSONDecoder().decode(WatchCommand.self, from: unknown))
    }

    func testPromptProblem() {
        XCTAssertNotNil(WatchCommand.promptProblem("   "))
        XCTAssertNil(WatchCommand.promptProblem("go"))
        XCTAssertNotNil(WatchCommand.promptProblem(String(repeating: "x", count: WatchCommand.maxPromptChars + 1)))
    }

    func testUnknownStatusAndMotionDecodeSafely() throws {
        let json = #"{"k":"x","st":"hibernating","so":"satellite","b":[{"m":"teleport","t":"hi"}]}"#
        let back = try JSONDecoder().decode(AgentFrame.self, from: Data(json.utf8))
        XCTAssertEqual(back.status, .unknown)
        XCTAssertEqual(back.source, .desktop)
        XCTAssertEqual(back.beats.first?.motion, .tick)
    }

    func testStatusMapping() {
        XCTAssertEqual(WireStatus.from("Running"), .running)
        XCTAssertEqual(WireStatus.from("waiting_for_input"), .waiting)
        XCTAssertEqual(WireStatus.from("cancelled"), .stopped)
        XCTAssertEqual(WireStatus.from("done", merged: true), .merged)
        XCTAssertEqual(WireStatus.from("??"), .unknown)
    }

    func testOrderingRunningFirstThenRecent() {
        let frames = [frame("old", .done, updated: 1), frame("run", .running, updated: 0), frame("new", .failed, updated: 9)]
        XCTAssertEqual(AgentFrame.ordered(frames).map(\.key), ["run", "new", "old"])
    }

    func testFramesKeepAtMost64Beats() {
        let f = frame("a", .running, beats: 100)
        XCTAssertEqual(f.beats.count, AgentFrame.maxBeats)
        XCTAssertEqual(f.beatCount, 100)
        XCTAssertEqual(f.beats.last?.text, "Bash step 99 " + String(repeating: "x", count: 60))
    }

    func testFittedStaysUnderTheCapAndKeepsTheWatchedFrame() {
        var frames: [AgentFrame] = []
        for i in 0..<30 { frames.append(frame("f\(i)", i < 3 ? .running : .done, beats: 64, updated: Int64(i))) }
        let snapshot = WatchSnapshot(frames: frames)
        XCTAssertGreaterThan(WatchWire.encodedSize(snapshot), 20_000)
        let fitted = WatchWire.fitted(snapshot, maxBytes: 20_000, keep: "f1")
        XCTAssertLessThanOrEqual(WatchWire.encodedSize(fitted), 20_000)
        XCTAssertEqual(fitted.frame("f1")?.beats.count, 64)
        XCTAssertEqual(fitted.frames.first?.status, .running)
        // A small snapshot passes through untouched (apart from its order).
        let small = WatchSnapshot(frames: [frame("x", .running, beats: 2)])
        XCTAssertEqual(WatchWire.fitted(small), small)
    }

    func testNewBeats() {
        let before = frame("a", .running, beats: 5)
        var after = frame("a", .running, beats: 8)
        XCTAssertEqual(WatchWire.newBeats(previous: before, next: after).count, 3)
        XCTAssertEqual(WatchWire.newBeats(previous: nil, next: after).count, 0)
        after.beatCount = 5
        XCTAssertEqual(WatchWire.newBeats(previous: before, next: after).count, 0)
    }

    func testSummaryAndRelevance() {
        let snapshot = WatchSnapshot(generatedAtMs: 1000, frames: [
            frame("a", .running, beats: 1, updated: 5), frame("b", .done, updated: 900), frame("c", .queued),
        ])
        let summary = snapshot.summary
        XCTAssertEqual(summary.running, 1)
        XCTAssertEqual(summary.active, 2)
        XCTAssertEqual(summary.topKey, "a")
        XCTAssertEqual(summary.inlineText, "1 agent running")
        XCTAssertEqual(summary.relevance(nowMs: 2000), 1.0)
        let idle = ComplicationSummary(lastFinishedAtMs: 1000)
        XCTAssertEqual(idle.relevance(nowMs: 1000 + 60_000), 0.6)
        XCTAssertEqual(idle.relevance(nowMs: 1000 + 3_600_000), 0.05)
        XCTAssertEqual(ComplicationSummary(running: 2).inlineText, "2 agents running")
    }

    func testGroupStoreAndDeepLink() throws {
        let items = InMemorySharedItemStore()
        let summary = ComplicationSummary(running: 1, topKey: "desktop:a/b:3", topTitle: "Fix")
        XCTAssertNil(WatchGroup.load(from: items))
        WatchGroup.save(summary, to: items)
        XCTAssertEqual(WatchGroup.load(from: items), summary)
        let url = try XCTUnwrap(WatchGroup.agentURL("desktop:a/b:3"))
        XCTAssertEqual(WatchGroup.agentKey(from: url), "desktop:a/b:3")
        XCTAssertNil(WatchGroup.agentKey(from: URL(string: "https://forkmesh.com")!))
    }

    func testHapticPacer() {
        var pacer = HapticPacer()
        XCTAssertEqual(pacer.next(for: [WireBeat(.run, "a"), WireBeat(.fail, "b")], now: 0), .failure)
        XCTAssertNil(pacer.next(for: [WireBeat(.finish, "c")], now: 0.3))
        XCTAssertEqual(pacer.next(for: [WireBeat(.finish, "c")], now: 0.7), .success)
        XCTAssertNil(pacer.next(for: [WireBeat(.usage, "quiet")], now: 5))
    }
}
