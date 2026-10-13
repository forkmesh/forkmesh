import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMDesktopRemote

final class DesktopUpdateStoreTests: XCTestCase {
    static let idleStatus: JSONObject = [
        "run": 1, "running": false, "version": "v1.0.0", "startedAtMs": 1000, "lines": [], "seq": 0,
    ]

    @MainActor func testStatusLoadsVersionAndBacklog() async {
        let channel = FakeRemoteChannel()
        var status = Self.idleStatus
        status["running"] = true
        status["kind"] = "update"
        status["percent"] = 60
        status["phase"] = "compiling"
        status["lines"] = ["$ cmake --build", "[1/9] Building"]
        channel.replies["update.status"] = status
        let store = DesktopUpdateStore(channel: channel)
        await store.refresh()
        XCTAssertEqual(store.version, "v1.0.0")
        XCTAssertEqual(store.lines, ["$ cmake --build", "[1/9] Building"])
        XCTAssertEqual(store.stage, .running, "an update started on the desktop shows too")
        XCTAssertEqual(store.state.headline, "60% · compiling")
        XCTAssertTrue(store.busy)
    }

    @MainActor func testRunStreamsProgressLines() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["update.status"] = Self.idleStatus
        channel.replies["update.prepare"] = [
            "token": "tok", "action": "updateRestart", "subject": "desktop", "summary": "Pull and restart",
        ]
        channel.replies["update.run"] = ["started": true, "action": "updateRestart", "message": "Updating."]
        let store = DesktopUpdateStore(channel: channel)
        await store.refresh()
        let confirmation = try await store.prepare()
        XCTAssertEqual(channel.last("update.prepare")?.str("action"), "updateRestart")
        XCTAssertEqual(confirmation.summary, "Pull and restart")
        let message = try await store.run(confirmation)
        XCTAssertEqual(message, "Updating.")
        XCTAssertEqual(channel.last("update.run")?.str("token"), "tok")
        XCTAssertEqual(store.stage, .running)

        channel.emit("update.progress", [
            "run": 2, "running": true, "kind": "update", "percent": 20, "phase": "pulling the source",
            "activity": "20% pulling the source", "lines": ["==> Pulling", "Already up to date."], "skipped": 0,
        ])
        XCTAssertEqual(store.lines, ["==> Pulling", "Already up to date."])
        XCTAssertEqual(store.state.percent, 20)
        XCTAssertEqual(store.state.headline, "20% pulling the source")
        channel.emit("update.progress", [
            "run": 2, "running": true, "percent": 60, "lines": ["[40/41] Linking"], "skipped": 12,
        ])
        XCTAssertEqual(store.lines.count, 4)
        XCTAssertEqual(store.lines[2], "… 12 earlier lines not shown")
        XCTAssertEqual(store.lines.last, "[40/41] Linking")
        channel.emit("fleet.progress", ["lines": ["other family"]])
        XCTAssertEqual(store.lines.count, 4, "other families' events are ignored")
    }

    @MainActor func testFailureStopsTracking() async {
        let channel = FakeRemoteChannel()
        channel.replies["update.status"] = Self.idleStatus
        let store = DesktopUpdateStore(channel: channel)
        await store.refresh()
        channel.emit("update.progress", ["run": 2, "running": true, "percent": 35, "lines": ["$ cmake"]])
        XCTAssertEqual(store.stage, .running)
        channel.emit("update.progress", [
            "run": 2, "running": false, "error": "Update failed: cmake exited 1", "lines": ["!! Update failed"],
        ])
        XCTAssertEqual(store.stage, .failed)
        XCTAssertEqual(store.state.error, "Update failed: cmake exited 1")
        channel.emit("__closed", ["reason": "gone"])
        await settle()
        XCTAssertEqual(store.stage, .failed, "a dropped link after a failure is not a restart")
    }

    @MainActor func testRelaunchWaitsForTheNewDesktop() async {
        let channel = FakeRemoteChannel()
        channel.replies["update.status"] = Self.idleStatus
        var polls = 0
        var clock = 0
        let store = DesktopUpdateStore(
            channel: channel, now: { clock },
            sleep: { ms in
                clock += ms
                polls += 1
                if polls == 1 { channel.failures["update.status"] = LocalControlException("not_connected") }
                if polls == 3 {
                    channel.failures["update.status"] = nil
                    channel.replies["update.status"] = [
                        "run": 0, "running": false, "version": "v1.1.0", "startedAtMs": 99000, "lines": [],
                    ]
                }
            })
        await store.refresh()
        channel.emit("update.progress", ["run": 2, "running": true, "percent": 80, "lines": ["==> Installing"]])
        channel.emit("update.progress", [
            "run": 2, "running": true, "percent": 100, "status": "Relaunching...", "relaunching": true,
            "lines": ["==> Relaunching..."],
        ])
        XCTAssertEqual(store.stage, .restarting)
        for _ in 0..<10 { await settle() }
        XCTAssertEqual(store.stage, .back)
        XCTAssertEqual(store.previousVersion, "v1.0.0")
        XCTAssertEqual(store.version, "v1.1.0")
        XCTAssertGreaterThanOrEqual(polls, 3)
    }

    @MainActor func testSameDesktopStillBuildingKeepsWaiting() async {
        let channel = FakeRemoteChannel()
        channel.replies["update.status"] = Self.idleStatus
        var polls = 0
        let store = DesktopUpdateStore(
            channel: channel, now: { 0 },
            sleep: { _ in
                polls += 1
                var still = Self.idleStatus
                still["running"] = .bool(polls < 2)
                still["error"] = .string(polls < 2 ? "" : "Update installed but the relaunch failed")
                channel.replies["update.status"] = still
            })
        await store.refresh()
        channel.emit("update.progress", ["run": 2, "running": true, "percent": 60])
        channel.emit("__closed", ["reason": "relay"])
        for _ in 0..<10 { await settle() }
        XCTAssertEqual(store.stage, .failed)
        XCTAssertEqual(store.state.error, "Update installed but the relaunch failed")
        XCTAssertEqual(polls, 2)
    }

    @MainActor func testOldDesktopIsUnsupported() async {
        let channel = FakeRemoteChannel()
        channel.failures["update.status"] = LocalControlException("unsupported")
        let store = DesktopUpdateStore(channel: channel)
        await store.refresh()
        XCTAssertTrue(store.unsupported)
        store.stop()
        XCTAssertEqual(channel.tapCount, 0)
    }
}
