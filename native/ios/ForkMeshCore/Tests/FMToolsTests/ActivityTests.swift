// ActivityScreenModel, plus the node
// activity section's words.

import FMApi
import FMBase
import XCTest
@testable import FMTools

final class ActivityTests: XCTestCase {
    @MainActor
    func testNetworkActivityShowsAnErrorAndCanRetry() async {
        var statsCalls = 0
        let model = ActivityScreenModel(
            stats: {
                statsCalls += 1
                if statsCalls == 1 { throw ApiException("network unavailable") }
                return NetworkStats(nodesOnline: 2, hostsOnline: 1, repos: 3)
            },
            leaderboards: { NetworkLeaderboards() })
        XCTAssertEqual(model.stats, .loading)
        await model.reload()
        XCTAssertEqual(model.stats, .failed("Exception: network unavailable"))

        await model.reload()
        XCTAssertEqual(statsCalls, 2)
        XCTAssertEqual(model.stats.value?.nodesOnline, 2)
        XCTAssertEqual(model.leaderboards.value, NetworkLeaderboards())
    }

    @MainActor
    func testLeaderboardActivityShowsAnErrorAndCanRetry() async {
        var calls = 0
        let model = ActivityScreenModel(
            stats: { NetworkStats(nodesOnline: 2, hostsOnline: 1, repos: 3) },
            leaderboards: {
                calls += 1
                if calls == 1 { throw ApiException("leaderboards unavailable") }
                return NetworkLeaderboards(fundsMainnodes: [FundsReceivedEntry(name: "mainnode-a", lamports: 42)])
            })
        await model.reload()
        XCTAssertEqual(model.leaderboards, .failed("Exception: leaderboards unavailable"))
        await model.reload()
        XCTAssertEqual(calls, 2)
        let groups = ActivityScreenModel.fundsGroups(model.leaderboards.value!)
        XCTAssertEqual(groups.count, 1)
        XCTAssertEqual(groups[0].title, "Mainnodes")
        XCTAssertEqual(groups[0].rows[0].name, "mainnode-a")
        XCTAssertEqual(groups[0].rows[0].amount, "42 lamports")
    }

    @MainActor
    func testActivityLogPrintsItsTimestampsAsA12HourClock() async {
        let hooks = ToolsHooks()
        hooks.relayLog = { ["00:04:05  relay connected", "13:30:00  peer left"] }
        let model = ActivityScreenModel(stats: { NetworkStats() }, leaderboards: { NetworkLeaderboards() }, hooks: hooks)
        let lines = model.visibleLogs
        XCTAssertEqual(lines.map(\.timestamp), ["1:30:00 PM", "12:04:05 AM"])
        XCTAssertEqual(lines.map(\.body), ["peer left", "relay connected"])
        XCTAssertEqual(lines.map(\.icon), ["logout", "check_circle_outline"])
        XCTAssertEqual(lines[1].tone, .success)
        XCTAssertEqual(model.logCountLabel, "2 events")
        XCTAssertFalse(model.showsLogToggle)
    }

    @MainActor
    func testTheLogShowsTwelveLinesUntilExpanded() async {
        let hooks = ToolsHooks()
        hooks.relayLog = { (0..<15).map { "10:00:\($0 < 10 ? "0" : "")\($0)  Relay failed \($0)" } }
        let model = ActivityScreenModel(stats: { NetworkStats() }, leaderboards: { NetworkLeaderboards() }, hooks: hooks)
        XCTAssertEqual(model.visibleLogs.count, 12)
        XCTAssertEqual(model.visibleLogs.first?.body, "Relay failed 14")
        XCTAssertEqual(model.visibleLogs.first?.tone, .danger)
        XCTAssertTrue(model.showsLogToggle)
        XCTAssertEqual(model.logToggleLabel, "Show all 15 logs")
        model.showAllLogs = true
        XCTAssertEqual(model.visibleLogs.count, 15)
        XCTAssertEqual(model.logToggleLabel, "Show less")
        // Eight characters that are not a stamp print as they are, as clockTime12From24 leaves them.
        XCTAssertEqual(ActivityLogLine(id: 0, line: "starting").timestamp, "starting")
        XCTAssertEqual(ActivityLogLine(id: 0, line: "short").timestamp, "")
        XCTAssertEqual(ActivityLogLine(id: 0, line: "starting").body, "starting")
    }

    func testClockTime12From24RewritesStampsAndLeavesOdditiesAlone() {
        XCTAssertEqual(Format.clockTime12From24("00:00:00"), "12:00:00 AM")
        XCTAssertEqual(Format.clockTime12From24("11:59:59"), "11:59:59 AM")
        XCTAssertEqual(Format.clockTime12From24("12:00:00"), "12:00:00 PM")
        XCTAssertEqual(Format.clockTime12From24("23:07:09"), "11:07:09 PM")
        XCTAssertEqual(Format.clockTime12From24("09:05"), "9:05:00 AM")
        XCTAssertEqual(Format.clockTime12From24("24:00:00"), "24:00:00")
        XCTAssertEqual(Format.clockTime12From24("starting"), "starting")
    }

    func testNodeActivityWords() {
        XCTAssertNil(NodeActivityPresentation(nil))
        XCTAssertNil(NodeActivityPresentation(NodeActivitySnapshot(mode: .disabled, connection: .live)))

        let now = Date(timeIntervalSince1970: 1_000_000)
        let active = NodeActivitySnapshot(
            mode: .active, connection: .connecting, owner: "alice", lastSyncAt: now.addingTimeInterval(-300),
            pendingTotal: 3,
            counts: [
                .init(kind: "agentPrompts", count: 2, label: "agent prompts", hostBound: true),
                .init(kind: "issues", count: 1, label: "issue", hostBound: false),
                .init(kind: "about", count: 0, label: "about edits", hostBound: true),
            ],
            ackedTotal: 4, lastError: "", syncing: true, noDesktop: true,
            applier: .init(appliedTotal: 1, leftTotal: 2, leftReasons: [.init(reason: "no local clone", times: 2)]))
        let words = NodeActivityPresentation(active, now: now)!
        XCTAssertEqual(words.badge, "connecting")
        XCTAssertEqual(words.badgeTone, .warning)
        XCTAssertEqual(words.title, "This device is acting as a node")
        XCTAssertTrue(words.body.hasPrefix("It signs as alice and reads"))
        XCTAssertEqual(words.tokens, ["Synced 5m ago", "3 pending", "2 agent prompts", "1 issue", "4 applied"])
        XCTAssertEqual(words.needsDesktopDetail,
                       "2 agent prompts reached this phone but need a host to apply. They are held here and never acknowledged, so a desktop node can still pick the work up.")
        XCTAssertEqual(words.applierTokens, ["1 committed here", "2 left for a desktop"])
        XCTAssertNotNil(words.appliedNote)
        XCTAssertEqual(words.leftReasonLines, ["· no local clone (×2)"])
        XCTAssertEqual(words.syncLabel, "Syncing…")

        var standby = active
        standby.mode = .standby
        standby.noDesktop = false
        standby.lastSyncAt = nil
        standby.syncing = false
        standby.applier = nil
        let quiet = NodeActivityPresentation(standby, now: now)!
        XCTAssertEqual(quiet.badge, "standby")
        XCTAssertEqual(quiet.badgeTone, .tertiary)
        XCTAssertEqual(quiet.title, "A desktop node is handling this account")
        XCTAssertEqual(quiet.tokens.first, "Never synced")
        XCTAssertNil(quiet.needsDesktopDetail)
        XCTAssertTrue(quiet.applierTokens.isEmpty)
        XCTAssertEqual(quiet.syncLabel, "Sync now")
    }

    @MainActor
    func testTheNodeSectionReadsTheHook() async {
        let hooks = ToolsHooks()
        var synced = 0
        hooks.nodeActivity = { NodeActivitySnapshot(mode: .active, connection: .live, owner: "bob") }
        hooks.syncNodeNow = { synced += 1 }
        let model = ActivityScreenModel(stats: { NetworkStats() }, leaderboards: { NetworkLeaderboards() }, hooks: hooks)
        XCTAssertEqual(model.node?.badge, "live")
        XCTAssertEqual(model.node?.badgeTone, .success)
        model.syncNodeNow()
        XCTAssertEqual(synced, 1)
        let bare = ActivityScreenModel(stats: { NetworkStats() }, leaderboards: { NetworkLeaderboards() })
        XCTAssertNil(bare.node)
        XCTAssertTrue(bare.visibleLogs.isEmpty)
    }
}
