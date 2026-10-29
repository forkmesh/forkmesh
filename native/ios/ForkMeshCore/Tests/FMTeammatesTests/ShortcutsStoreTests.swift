import FMBase
import FMControl
import Foundation
import XCTest

@testable import FMTeammates

final class ShortcutsStoreTests: XCTestCase {
    static let catalogJSON: JSONObject = [
        "repo": "f/r",
        "repos": ["f/R", "f/other"],
        "entries": [
            ["file": "a.md", "scope": "shortcut", "name": "A", "uses": 1, "runnable": true],
            ["file": "deploy.md", "scope": "shortcut", "name": "Deploy", "uses": 3, "runnable": true],
            ["file": "b.skill", "scope": "repository", "name": "B", "uses": 4],
            ["file": "c.skill", "scope": "app", "name": "C", "uses": 0],
        ],
    ]

    @MainActor func testRefreshSplitsShortcutsAndSkills() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.shortcuts"] = Self.catalogJSON
        let store = ShortcutsStore(channel: channel)
        await store.refresh(repo: "f/r")
        let catalog = try XCTUnwrap(store.catalog)
        XCTAssertEqual(catalog.shortcuts.map(\.file), ["deploy.md", "a.md"])
        XCTAssertEqual(catalog.skills.map(\.file), ["b.skill", "c.skill"])
        XCTAssertEqual(channel.last("insights.shortcuts")?.str("repo"), "f/r")
        XCTAssertEqual(catalog.pick(preferred: "f/r"), "f/R")
        XCTAssertEqual(catalog.pick(preferred: "x/y"), "f/R")
        XCTAssertEqual(store.error, "")
    }

    @MainActor func testLoadFallsBackToARepoTheDesktopHasOnEveryVisit() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.shortcuts"] = Self.catalogJSON
        let store = ShortcutsStore(channel: channel)
        let first = await store.load(preferred: "x/y")
        XCTAssertEqual(first, "f/R")
        XCTAssertEqual(channel.requests.filter { $0.0 == "insights.shortcuts" }.map { $0.1.str("repo") }, ["x/y", "f/R"])
        let again = await store.load(preferred: "x/y")
        XCTAssertEqual(again, "f/R")
        XCTAssertEqual(channel.last("insights.shortcuts")?.str("repo"), "f/R")
        XCTAssertEqual(channel.count("insights.shortcuts"), 3)
        XCTAssertEqual(store.catalog?.shortcuts.map(\.file), ["deploy.md", "a.md"])
        let other = await store.load(preferred: "F/OTHER")
        XCTAssertEqual(other, "f/other")
        XCTAssertEqual(channel.last("insights.shortcuts")?.str("repo"), "f/other")
    }

    @MainActor func testRunGoesThroughPrepareAndRun() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.prepare"] = ["token": "t", "action": "runShortcut", "subject": "f/r|shortcut|deploy.md"]
        channel.replies["insights.run"] = ["started": true, "message": "Started agent #4 for Deploy."]
        let store = ShortcutsStore(channel: channel)
        let entry = ShortcutEntry(json: ["file": "deploy.md", "scope": "shortcut", "name": "Deploy", "runnable": true])
        let confirmation = try await store.prepare(repo: "f/r", entry: entry)
        XCTAssertEqual(channel.last("insights.prepare")?.str("action"), "runShortcut")
        XCTAssertEqual(channel.last("insights.prepare")?.str("file"), "deploy.md")
        let message = try await store.run(confirmation)
        XCTAssertEqual(message, "Started agent #4 for Deploy.")
        XCTAssertEqual(channel.last("insights.run")?.str("token"), "t")
    }

    @MainActor func testOneTapRunPreparesAndRunsWithoutASheet() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.prepare"] = ["token": "t", "action": "runShortcut", "subject": "f/r|shortcut|ship.sh"]
        channel.replies["insights.run"] = ["started": true, "message": "Running Ship on the desktop."]
        channel.replies["insights.shortcuts"] = Self.catalogJSON
        let store = ShortcutsStore(channel: channel)
        let entry = ShortcutEntry(json: ["file": "ship.sh", "scope": "shortcut", "name": "Ship", "kind": "script", "runnable": true])
        let message = try await store.runNow(repo: "f/r", entry: entry)
        XCTAssertEqual(message, "Running Ship on the desktop.")
        XCTAssertEqual(channel.last("insights.prepare")?.str("action"), "runShortcut")
        XCTAssertEqual(channel.last("insights.run")?.str("token"), "t")
        XCTAssertEqual(channel.last("insights.run")?.str("action"), "runShortcut")
        XCTAssertEqual(channel.count("insights.shortcuts"), 1)
    }

    @MainActor func testQueueAndUnqueueUseTheirOwnActions() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.prepare"] = ["token": "q", "action": "queueShortcut", "subject": "f/r|shortcut|ship.sh"]
        channel.replies["insights.run"] = ["queued": 1, "message": "Queued Ship: number 1 in line on the desktop."]
        let store = ShortcutsStore(channel: channel)
        let entry = ShortcutEntry(json: ["file": "ship.sh", "scope": "shortcut", "name": "Ship", "kind": "script", "runnable": true])
        XCTAssertTrue(entry.queueable)
        let message = try await store.queue(repo: "f/r", entry: entry)
        XCTAssertEqual(message, "Queued Ship: number 1 in line on the desktop.")
        XCTAssertEqual(channel.last("insights.prepare")?.str("action"), "queueShortcut")
        XCTAssertEqual(channel.last("insights.prepare")?.str("file"), "ship.sh")
        channel.replies["insights.prepare"] = ["token": "u", "action": "unqueueShortcut", "subject": "f/r|shortcut|ship.sh"]
        try await store.unqueue(repo: "f/r", entry: entry)
        XCTAssertEqual(channel.last("insights.prepare")?.str("action"), "unqueueShortcut")
        XCTAssertEqual(channel.last("insights.run")?.str("token"), "u")
    }

    @MainActor func testQueuedPositionAndBusyRepoComeFromTheCatalog() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.shortcuts"] = [
            "repo": "f/r", "repos": ["f/r"],
            "entries": [
                ["file": "build.sh", "scope": "shortcut", "name": "Build", "kind": "script", "runnable": true, "running": true],
                ["file": "ship.sh", "scope": "shortcut", "name": "Ship", "kind": "script", "runnable": true, "queued": 2],
                ["file": "fix.md", "scope": "shortcut", "name": "Fix", "kind": "prompt", "runnable": true],
            ],
        ]
        let store = ShortcutsStore(channel: channel)
        await store.refresh(repo: "f/r")
        let catalog = try XCTUnwrap(store.catalog)
        XCTAssertTrue(store.repoBusy(repo: "f/r"))
        XCTAssertEqual(catalog.entries.first { $0.file == "ship.sh" }?.queued, 2)
        XCTAssertEqual(catalog.entries.first { $0.file == "fix.md" }?.queueable, false)
    }

    @MainActor func testChangedEventRefreshesTheShownRepo() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.shortcuts"] = Self.catalogJSON
        let store = ShortcutsStore(channel: channel)
        channel.emit("insights.changed")
        XCTAssertEqual(channel.count("insights.shortcuts"), 0)
        await store.refresh(repo: "f/r")
        channel.emit("insights.changed")
        for _ in 0..<20 where channel.count("insights.shortcuts") < 2 { await Task.yield() }
        XCTAssertEqual(channel.count("insights.shortcuts"), 2)
    }

    @MainActor func testOutputEventsStreamIntoTheRun() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.shortcutRun"] = [
            "run": ["id": 2, "repo": "f/r", "file": "ship.sh", "name": "Ship", "running": true, "output": "$ bash ship.sh\nstart\n"],
        ]
        let store = ShortcutsStore(channel: channel)
        channel.emit("insights.shortcutOutput", ["id": 2, "repo": "f/r", "file": "ship.sh", "name": "Ship", "running": true, "chunk": "start\n"])
        for _ in 0..<20 where channel.count("insights.shortcutRun") < 1 { await Task.yield() }
        for _ in 0..<20 where store.lastRun?.output.hasPrefix("$ bash") != true { await Task.yield() }
        XCTAssertEqual(store.lastRun?.output, "$ bash ship.sh\nstart\n")
        channel.emit("insights.shortcutOutput", ["id": 2, "repo": "f/r", "file": "ship.sh", "running": true, "chunk": "building\n"])
        channel.emit("insights.shortcutOutput", ["id": 2, "repo": "f/r", "file": "ship.sh", "running": false, "ok": true, "message": "done (exit 0)", "chunk": "done (exit 0)\n"])
        let run = try XCTUnwrap(store.lastRun)
        XCTAssertEqual(run.output, "$ bash ship.sh\nstart\nbuilding\ndone (exit 0)\n")
        XCTAssertFalse(run.running)
        XCTAssertTrue(run.ok)
        XCTAssertEqual(run.message, "done (exit 0)")
        XCTAssertTrue(run.matches(repo: "F/R", file: "ship.sh"))
        XCTAssertEqual(channel.count("insights.shortcutRun"), 1)
    }

    @MainActor func testTwoRunsAtOnceKeepTheirOwnOutput() async throws {
        let channel = FakeTeamChannel()
        let store = ShortcutsStore(channel: channel)
        channel.emit("insights.shortcutOutput", ["id": 3, "repo": "f/r", "file": "adhoc.sh", "running": true, "chunk": "ad hoc\n"])
        channel.emit("insights.shortcutOutput", ["id": 4, "repo": "f/r", "file": "store.sh", "running": true, "chunk": "store\n"])
        channel.emit("insights.shortcutOutput", ["id": 3, "repo": "f/r", "file": "adhoc.sh", "running": false, "ok": true, "message": "done (exit 0)", "chunk": "done (exit 0)\n"])
        for _ in 0..<20 where channel.count("insights.shortcutRun") < 2 { await Task.yield() }
        XCTAssertEqual(store.runs[3]?.output, "ad hoc\ndone (exit 0)\n")
        XCTAssertEqual(store.runs[4]?.output, "store\n")
        XCTAssertEqual(store.lastRun?.id, 4)
        XCTAssertFalse(store.isRunning(repo: "f/r", file: "adhoc.sh"))
        XCTAssertTrue(store.isRunning(repo: "F/R", file: "store.sh"))
        XCTAssertEqual(store.liveRun(repo: "f/r", entry: ShortcutEntry(json: ["file": "adhoc.sh", "scope": "shortcut", "kind": "script"]))?.id, 3)
        XCTAssertEqual(Set(channel.requests.filter { $0.0 == "insights.shortcutRun" }.map { $0.1.int("id") }), [3, 4])
        channel.replies["insights.shortcutRun"] = [
            "run": ["id": 4, "repo": "f/r", "file": "store.sh", "running": true, "output": "store\nupload\n"],
            "runs": [["id": 4, "repo": "f/r", "file": "store.sh", "running": true]],
        ]
        await store.loadRun()
        XCTAssertEqual(Array(store.runs.keys), [4])
        XCTAssertEqual(store.shownRun?.output, "store\nupload\n")
    }

    @MainActor func testRowStatusShowsTheLiveLineThenTheDesktopsLastOutput() async throws {
        let channel = FakeTeamChannel()
        let store = ShortcutsStore(channel: channel)
        let idle = ShortcutEntry(json: ["file": "ship.sh", "scope": "shortcut", "kind": "script", "runnable": true])
        XCTAssertFalse(store.status(repo: "f/r", entry: idle).ran)
        let finished = ShortcutEntry(json: [
            "file": "ship.sh", "scope": "shortcut", "kind": "script", "runnable": true,
            "lastOutput": "artefact   ForkMesh.ipa", "lastOk": true, "finished": 2,
        ])
        XCTAssertTrue(finished.isScript)
        XCTAssertEqual(store.status(repo: "f/r", entry: finished),
                       ShortcutRowStatus(running: false, ok: true, line: "artefact   ForkMesh.ipa"))
        channel.emit("insights.shortcutOutput", ["id": 5, "repo": "f/r", "file": "ship.sh", "running": true, "chunk": "  > Archive\n"])
        XCTAssertEqual(store.status(repo: "F/R", entry: finished),
                       ShortcutRowStatus(running: true, ok: false, line: "> Archive"))
        XCTAssertEqual(store.status(repo: "f/other", entry: finished).line, "artefact   ForkMesh.ipa")
        channel.emit("insights.shortcutOutput", ["id": 5, "repo": "f/r", "file": "ship.sh", "running": false, "ok": false, "message": "failed (exit 1)", "chunk": "failed (exit 1)\n"])
        XCTAssertEqual(store.status(repo: "f/r", entry: finished).line, "artefact   ForkMesh.ipa")
    }

    func testProgressEstimatesFromTheLastRun() {
        XCTAssertEqual(ShortcutProgress.duration(5_400), "5s")
        XCTAssertEqual(ShortcutProgress.duration(65_000), "1m 05s")
        XCTAssertEqual(ShortcutProgress.duration(3_900_000), "1h 05m")
        let midway = ShortcutProgress(elapsedMs: 65_000, expectedMs: 90_000)
        XCTAssertEqual(midway.text, "1m 05s of about 1m 30s · 25s left")
        XCTAssertEqual(midway.fraction ?? 0, 65.0 / 90.0, accuracy: 0.001)
        let late = ShortcutProgress(elapsedMs: 100_000, expectedMs: 90_000)
        XCTAssertEqual(late.text, "1m 40s · 10s longer than the last run")
        XCTAssertEqual(late.fraction, 0.99)
        let first = ShortcutProgress(elapsedMs: 12_000, expectedMs: -1)
        XCTAssertNil(first.fraction)
        XCTAssertEqual(first.text, "Running for 12s · no earlier run to estimate from")
    }

    @MainActor func testRowStatusCarriesTheLastDurationAndALiveEstimate() async throws {
        let store = ShortcutsStore(channel: FakeTeamChannel())
        let idle = ShortcutEntry(json: [
            "file": "ship.sh", "scope": "shortcut", "kind": "script", "finished": 1, "lastOk": true, "lastMs": 90_000,
        ])
        XCTAssertEqual(store.status(repo: "f/r", entry: idle).tookText, "took 1m 30s")
        XCTAssertNil(store.status(repo: "f/r", entry: idle).progress(nowMs: 1_000_000))
        XCTAssertEqual(ShortcutEntry(json: ["file": "old.sh", "scope": "shortcut"]).lastMs, -1)

        let running = ShortcutEntry(
            json: [
                "file": "ship.sh", "scope": "shortcut", "kind": "script", "running": true, "lastMs": 90_000,
                "elapsedMs": 30_000, "expectedMs": 90_000,
            ], nowMs: 1_000_000)
        XCTAssertEqual(running.startedAtMs, 970_000)
        let status = store.status(repo: "f/r", entry: running)
        XCTAssertNil(status.tookText)
        XCTAssertEqual(status.progress(nowMs: 1_035_000)?.text, "1m 05s of about 1m 30s · 25s left")

        var run = ShortcutRun(
            json: ["id": 7, "repo": "f/r", "file": "ship.sh", "running": true, "startedAtMs": 8_000, "expectedMs": 4_000],
            nowMs: 10_000)
        XCTAssertEqual(run.progress(nowMs: 11_000)?.fraction ?? 0, 0.75, accuracy: 0.001)
        run.absorb(["id": 7, "running": true, "startedAtMs": 3_000, "expectedMs": 4_000, "chunk": "x\n"], nowMs: 12_000)
        XCTAssertEqual(run.startedAtMs, 8_000)
        run.absorb(["id": 7, "running": false, "ok": true, "message": "done (exit 0)"], nowMs: 13_000)
        XCTAssertNil(run.progress(nowMs: 13_000))
    }

    func testLastLineSkipsBlanksProgressFramesAndColour() {
        XCTAssertEqual(ShortcutRun.lastLine("one\ntwo\n\n  \n"), "two")
        XCTAssertEqual(ShortcutRun.lastLine("10%\r55%\r\u{1B}[32mdone\u{1B}[0m\n"), "done")
        XCTAssertEqual(ShortcutRun.lastLine("abcdef", maxChars: 3), "abc")
        XCTAssertEqual(ShortcutRun.lastLine(""), "")
    }

    func testRunOutputKeepsWholeLinesOfTheTail() {
        let long = String(repeating: "x", count: ShortcutRun.maxOutput) + "\nlast\n"
        XCTAssertEqual(ShortcutRun.tail("a\n"), "a\n")
        XCTAssertEqual(ShortcutRun.tail(long), "last\n")
    }

    func testScriptRefusalReadsPlainly() {
        XCTAssertEqual(TeamErrorText.describe(code: "script_on_desktop"), "Script shortcuts run on the desktop itself.")
        XCTAssertTrue(TeamErrorText.describe(code: "approval_required").contains("Allow"))
    }
}
