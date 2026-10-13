import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMDesktopRemote

final class FilesBrowserStoreTests: XCTestCase {
    @MainActor func testRootsGroupWorktreesUnderTheirRepository() async {
        let channel = FakeRemoteChannel()
        channel.replies["files.browse.roots"] = [
            "roots": [
                ["id": "f/forkmesh", "label": "f/forkmesh", "kind": "repo", "branch": "main", "repo": "f/forkmesh"],
                ["id": "f/forkmesh#b", "label": "zeta", "kind": "worktree", "branch": "zeta", "repo": "f/forkmesh"],
                ["id": "f/forkmesh#a", "label": "alpha", "kind": "worktree", "branch": "alpha", "repo": "f/forkmesh"],
                ["id": "f/other#x", "label": "x", "kind": "worktree", "repo": "f/other"],
            ]
        ]
        let store = FilesBrowserStore(channel: channel)
        await store.loadRoots()
        let groups = store.groups
        XCTAssertEqual(groups.map(\.id), ["f/forkmesh", "f/other"])
        XCTAssertEqual(groups[0].worktrees.map(\.label), ["alpha", "zeta"])
        XCTAssertEqual(groups[1].repo.label, "f/other")
    }

    @MainActor func testListCachesAndReadDecodes() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["files.browse.list"] = [
            "root": "f/r", "path": "src", "total": 2, "truncated": false,
            "entries": [
                ["name": "lib", "kind": "dir"],
                ["name": ".env", "kind": "file", "size": 20.0, "sensitive": true],
                ["name": "out", "kind": "symlink", "escapes": true],
            ],
        ]
        let store = FilesBrowserStore(channel: channel)
        let listing = try await store.list(root: "f/r", path: "src")
        XCTAssertEqual(channel.last("files.browse.list")?.str("path"), "src")
        XCTAssertEqual(store.cached(root: "f/r", path: "src"), listing)
        XCTAssertTrue(listing.entries[0].kind.isFolder)
        XCTAssertFalse(listing.entries[1].canOpen)
        XCTAssertFalse(listing.entries[2].canOpen)

        channel.replies["files.browse.read"] = [
            "size": 10.0, "offset": 0.0, "length": 4.0, "eof": false, "binary": true, "base64": "AAEC",
        ]
        let slice = try await store.read(root: "f/r", path: "a.bin")
        XCTAssertEqual(slice.data, Data([0, 1, 2]))
        XCTAssertEqual(slice.nextOffset, 4)
    }

    @MainActor func testStatusAndGitActions() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["files.browse.status"] = [
            "root": "f/r", "branch": "main", "upstream": "origin/main", "ahead": 0, "behind": 3, "total": 2,
            "staged": 1, "unstaged": 1, "untracked": 0, "conflicted": 0,
            "files": [
                ["path": "a", "index": "M", "worktree": " ", "kind": "modified"],
                ["path": "b", "index": " ", "worktree": "M", "kind": "modified"],
            ],
        ]
        let store = FilesBrowserStore(channel: channel)
        channel.emit("files.browse.changed", ["root": "f/r"])
        XCTAssertTrue(store.changedRoots.contains("f/r"))
        let status = try await store.status(root: "f/r")
        XCTAssertFalse(store.changedRoots.contains("f/r"))
        XCTAssertEqual(status.syncLine, "3 behind origin/main")
        XCTAssertEqual(status.changesLine, "1 staged · 1 modified")
        XCTAssertTrue(status.canFastForward)
        XCTAssertTrue(status.files[0].isStaged)
        XCTAssertFalse(status.files[1].isStaged)

        channel.replies["files.browse.prepare"] = ["token": "t1", "action": "pull", "subject": "f/r", "summary": "Pull"]
        channel.replies["files.browse.run"] = ["started": true, "message": "Pulling"]
        let confirmation = try await store.prepare(.pull, root: "f/r")
        XCTAssertFalse(confirmation.needsTypedConfirmation)
        let message = try await store.run(confirmation)
        XCTAssertEqual(message, "Pulling")
        XCTAssertEqual(channel.last("files.browse.run")?.str("root"), "f/r")
        XCTAssertEqual(channel.last("files.browse.run")?.str("token"), "t1")
    }

    func testRemotePaths() {
        XCTAssertEqual(RemotePath.join("", "a"), "a")
        XCTAssertEqual(RemotePath.join("a", "b"), "a/b")
        XCTAssertEqual(RemotePath.parent("a/b"), "a")
        XCTAssertEqual(RemotePath.parent("a"), "")
        XCTAssertNil(RemotePath.parent(""))
        XCTAssertEqual(RemotePath.crumbs("a/b").map(\.path), ["a", "a/b"])
        XCTAssertTrue(RemotePath.isImage("x/Logo.PNG"))
        XCTAssertFalse(RemotePath.isImage("x/main.cpp"))
    }
}

final class DiagnosticsStoreTests: XCTestCase {
    @MainActor func testHealthParses() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["diag.health"] = [
            "desktop": "box", "level": "busy", "cpuPercent": 42.0, "cpuCount": 8, "load1": 3.5,
            "memory": ["total": 1000.0, "available": 250.0, "appResident": 100.0],
            "pressure": ["valid": true, "memoryFull": 0.5],
            "disk": ["total": 2000.0, "free": 500.0],
            "agents": ["running": 2, "queued": 1, "total": 5],
            "stalls": 3,
            "power": ["onBattery": true, "percent": 61],
        ]
        let store = DiagnosticsStore(channel: channel)
        await store.refreshHealth()
        let health = try XCTUnwrap(store.health)
        XCTAssertEqual(health.level, .busy)
        XCTAssertEqual(health.memoryUsedShare, 0.75, accuracy: 0.001)
        XCTAssertEqual(health.diskUsedShare, 0.75, accuracy: 0.001)
        XCTAssertEqual(health.agentsLine, "2 running · 1 queued")
        XCTAssertEqual(health.loadLine, "3.5 on 8 CPUs")
        XCTAssertTrue(health.onBattery)
        XCTAssertEqual(health.batteryShare, 0.61, accuracy: 0.001)
        XCTAssertEqual(health.batteryLine, "61% left")
    }

    func testHealthWithoutPowerIsNotOnBattery() {
        let health = DesktopHealth(json: ["cpuPercent": 5.0])
        XCTAssertFalse(health.onBattery)
        XCTAssertEqual(health.batteryPercent, -1)
        XCTAssertEqual(health.batteryLine, "On battery")
    }

    @MainActor func testLogTailPagesBackwards() async {
        let channel = FakeRemoteChannel()
        channel.replies["diag.log"] = [
            "lines": [
                ["offset": 100, "t": 1, "severity": "info", "text": "2026-09-24 10:00:00  hello"],
                ["offset": 200, "t": 1, "severity": "error", "text": "2026-09-24 10:00:01  boom failed"],
            ],
            "start": 100, "end": 300, "more": true,
        ]
        let store = DiagnosticsStore(channel: channel)
        let tail = DesktopLogTail(store: store, pageSize: 2)
        tail.severity = .error
        tail.query = "boom"
        await tail.loadLatest()
        XCTAssertEqual(tail.lines.count, 2)
        XCTAssertEqual(tail.lines[1].body, "boom failed")
        XCTAssertEqual(tail.errorCount, 1)
        XCTAssertEqual(channel.last("diag.log")?.str("severity"), "error")
        XCTAssertEqual(channel.last("diag.log")?.str("query"), "boom")
        XCTAssertNil(channel.last("diag.log")?["before"])
        channel.replies["diag.log"] = [
            "lines": [["offset": 10, "t": 1, "severity": "info", "text": "older"]],
            "start": 10, "end": 100, "more": false,
        ]
        await tail.loadOlder()
        XCTAssertEqual(channel.last("diag.log")?["before"], .int(100))
        XCTAssertEqual(tail.lines.map(\.offset), [10, 100, 200])
        XCTAssertFalse(tail.more)
        await tail.loadOlder()
        XCTAssertEqual(channel.count("diag.log"), 2, "no request once the start of the log is reached")
    }

    @MainActor func testProcessesAndStopAgent() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["diag.processes"] = [
            "memoryTotal": 1000.0,
            "processes": [
                ["pid": 10, "name": "claude", "rssBytes": 400.0, "percent": 40.0,
                 "agent": ["session": 7, "label": "Fix bug", "stoppable": true]],
                ["pid": 11, "name": "chrome", "rssBytes": 100.0, "percent": 10.0],
            ],
        ]
        channel.replies["diag.prepare"] = ["token": "t", "action": "stopAgent", "subject": "7", "summary": "Stop"]
        channel.replies["diag.run"] = ["started": true, "message": "Stop requested."]
        let store = DiagnosticsStore(channel: channel)
        await store.refreshProcesses()
        let list = try XCTUnwrap(store.processes)
        XCTAssertEqual(list.agentProcesses.map(\.pid), [10])
        XCTAssertEqual(list.share(of: list.processes[0]), 0.4, accuracy: 0.001)
        let confirmation = try await store.prepare(.stopAgent(session: 7))
        XCTAssertEqual(channel.last("diag.prepare")?["session"], .int(7))
        let message = try await store.run(confirmation)
        XCTAssertEqual(message, "Stop requested.")
        XCTAssertEqual(channel.count("diag.processes"), 2, "the list refreshes after a stop")
    }

    @MainActor func testStalls() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["diag.stalls"] = [
            "canSend": true,
            "stalls": [["index": 1, "when": "2026-09-24 09:00:00", "summary": "UI froze", "lines": 30]],
        ]
        channel.replies["diag.stall"] = ["index": 1, "text": "frame", "truncated": true]
        let store = DiagnosticsStore(channel: channel)
        await store.refreshStalls()
        XCTAssertEqual(store.stalls?.stalls.first?.summary, "UI froze")
        XCTAssertTrue(store.stalls?.canSend ?? false)
        let detail = try await store.stall(1)
        XCTAssertTrue(detail.truncated)
        XCTAssertEqual(DiagnosticsAction.sendStall(index: 1).params.str("action"), "sendStall")
    }

    @MainActor func testBackups() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["diag.backups"] = [
            "enabled": true, "keep": 24, "running": false, "result": "failed",
            "location": "ForkMesh data \u{203A} backups", "count": 2, "totalBytes": 3072.0,
            "lastBackupAtMs": 1_700_000_000_000.0, "lastBackupBytes": 2048.0,
            "nextDueAtMs": 1_700_003_600_000.0, "error": "tar_failed", "failedAtMs": 1_700_000_100_000.0,
        ]
        let store = DiagnosticsStore(channel: channel)
        await store.refreshBackups()
        let backups = try XCTUnwrap(store.backups)
        XCTAssertEqual(channel.count("diag.backups"), 1)
        XCTAssertTrue(backups.enabled)
        XCTAssertEqual(backups.keep, 24)
        XCTAssertEqual(backups.result, .failed)
        XCTAssertEqual(backups.location, "ForkMesh data \u{203A} backups")
        XCTAssertEqual(backups.count, 2)
        XCTAssertEqual(backups.lastBackupAtMs, 1_700_000_000_000)
        XCTAssertEqual(backups.nextDueAtMs, 1_700_003_600_000)
        XCTAssertTrue(backups.hasBackup)
        XCTAssertFalse(backups.errorText.isEmpty)
        XCTAssertTrue(backups.sizeLine.contains("2 copies"))

        let off = DesktopBackupStatus(json: ["enabled": false, "result": "off", "count": 0])
        XCTAssertEqual(off.result, .off)
        XCTAssertEqual(off.sizeLine, "—")
        XCTAssertEqual(off.errorText, "")
        XCTAssertEqual(DesktopBackupResult(wire: "later"), .unknown)

        channel.failures["diag.backups"] = LocalControlException("unsupported")
        await store.refreshBackups()
        XCTAssertTrue(store.unsupported)
    }

    func testErrorTextFallsBack() {
        XCTAssertEqual(RemoteErrorText.describe(code: "sensitive_file"),
                       "That file looks like a key or credential, so it stays on the desktop.")
        XCTAssertEqual(RemoteErrorText.describe(code: "timeout"), "The desktop did not answer in time.")
    }
}
