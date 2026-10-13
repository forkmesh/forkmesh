// The home shell's logic (the top bar's status line and account label, the page menu, the
// prompt bar and push routing) and the user status sheet's save rules.

import FMAccount
import FMBase
import XCTest
@testable import FMApi

private let utc = TimeZone(identifier: "UTC")!

final class HomeShellViewModelTests: XCTestCase {
    func testTheTabsMapOntoAppTab() {
        XCTAssertEqual(HomeShellCatalog.destinations.map(\.label), ["Code", "Agents", "Chat", "Inbox"])
        XCTAssertEqual(
            HomeShellCatalog.bottomBarOrder.map { HomeShellCatalog.destinations[$0].label }, ["Agents", "Inbox", "Code", "Chat"])
        XCTAssertEqual(HomeShellCatalog.index(of: "Agents"), 1)
        XCTAssertEqual(HomeShellCatalog.index(of: "Chat"), 2)
        XCTAssertEqual(HomeShellCatalog.bottomBarOrder.map(HomeShellCatalog.tab(at:)), AppTab.barOrder)
        for tab in AppTab.allCases {
            XCTAssertEqual(HomeShellCatalog.tab(at: HomeShellCatalog.index(of: tab)), tab)
        }
    }

    func testTopSearchStartsScopedToTheTab() {
        XCTAssertEqual(HomeShellCatalog.searchScope(for: .agents), .everything)
        XCTAssertEqual(HomeShellCatalog.searchScope(for: .inbox), .issues)
        XCTAssertEqual(HomeShellCatalog.searchScope(for: .code), .repos)
        XCTAssertEqual(HomeShellCatalog.searchScope(for: .chat), .chat)
    }

    func testStatusLabelAndSubtitle() {
        XCTAssertEqual(shellStatusLabel(.connected, onlineCount: 1), "Connected - 1 node online")
        XCTAssertEqual(shellStatusLabel(.connected, onlineCount: 3), "Connected - 3 nodes online")
        XCTAssertEqual(shellStatusLabel(.connecting, onlineCount: 3), "Connecting...")
        XCTAssertEqual(shellStatusLabel(.offline, onlineCount: 0), "Offline")

        // The banner below the bar says when the relay is down; the subtitle stays quiet.
        XCTAssertNil(shellSubtitle(.offline, onlineCount: 0, guest: false, relayLabel: ""))
        XCTAssertEqual(shellSubtitle(.connected, onlineCount: 2, guest: false, relayLabel: ""), "Connected - 2 nodes online")
        XCTAssertEqual(shellSubtitle(.offline, onlineCount: 0, guest: true, relayLabel: ""), "Guest (read-only)")
        XCTAssertEqual(
            shellSubtitle(.connected, onlineCount: 1, guest: true, relayLabel: ""), "Guest (read-only) - 1 node online")
        XCTAssertEqual(shellSubtitle(.offline, onlineCount: 0, guest: false, relayLabel: "Home"), "Home")
        XCTAssertEqual(
            shellSubtitle(.connected, onlineCount: 2, guest: false, relayLabel: "Home"), "Home - Connected - 2 nodes online")
    }

    func testAccountLabelFallsBackToDisplayNameThenKey() {
        let session = AuthSession.fromJSON(["nodeName": "alice", "email": "a@example.com"])
        XCTAssertEqual(shellAccountLabel(session, displayName: "Al", identityShortKey: "k"), "alice")
        XCTAssertEqual(
            shellAccountLabel(AuthSession.fromJSON(["email": "a@example.com"]), displayName: "Al", identityShortKey: "k"),
            "a@example.com")
        XCTAssertEqual(shellAccountLabel(nil, displayName: "Al", identityShortKey: "k"), "Al")
        XCTAssertEqual(shellAccountLabel(nil, displayName: "", identityShortKey: "abc123"), "abc123")
        XCTAssertEqual(shellAvatarLabel(statusLabel: "Offline", described: ""), "Offline - your node profile")
        XCTAssertEqual(shellAvatarLabel(statusLabel: "Offline", described: "Away"), "Offline - your node profile\nAway")
    }

    func testPromptBarCopy() {
        XCTAssertEqual(shellPromptHint, "Start a new agent...")
    }

    func testPushRouting() {
        let unread = ["#mesh", "@bob", "#general"]
        XCTAssertEqual(pushChatConversation(isDirectMessage: true, unreadConversations: unread), "@bob")
        XCTAssertEqual(pushChatConversation(isDirectMessage: false, unreadConversations: unread), "#general")
        XCTAssertNil(pushChatConversation(isDirectMessage: false, unreadConversations: ["#mesh"]))
        let direct = "direct:" + String(repeating: "ab", count: 16)
        XCTAssertEqual(pushChatConversation(isDirectMessage: true, unreadConversations: ["#general", direct]), direct)
        XCTAssertEqual(
            pushChatConversation(isDirectMessage: true, unreadConversations: unread, pushed: direct), direct)
        XCTAssertEqual(
            pushChatConversation(isDirectMessage: false, unreadConversations: unread, pushed: "#office/design"),
            "#office/design")
        XCTAssertEqual(pushChatConversation(isDirectMessage: false, unreadConversations: [], pushed: "#mesh"), "#mesh")
        XCTAssertEqual(
            pushChatConversation(isDirectMessage: false, unreadConversations: unread, pushed: direct), "#general")
        XCTAssertEqual(
            pushChatConversation(isDirectMessage: true, unreadConversations: unread, pushed: "#general"), "@bob")
        XCTAssertEqual(
            pushChatConversation(isDirectMessage: false, unreadConversations: unread, pushed: "general"), "#general")
        XCTAssertEqual(pushDeepLink(pushHref: "/mona/forkmesh/pulls/3", rowHref: nil)?.number, 3)
        XCTAssertEqual(pushDeepLink(pushHref: "", rowHref: "/mona/forkmesh/issues/4")?.initialTab, .issues)
        XCTAssertNil(pushDeepLink(pushHref: "", rowHref: nil))
    }

    func testLocalDesktopLabels() {
        XCTAssertEqual(ShellLocalDesktop.connected("Mac").label, "Connected to Mac on this network")
        XCTAssertEqual(ShellLocalDesktop.awaitingApproval("Mac").label, "Approve this phone on Mac")
        XCTAssertEqual(ShellLocalDesktop.connecting("Mac").label, "Connecting to Mac")
        XCTAssertEqual(ShellLocalDesktop.inReach.label, "A desktop is on this network")
        XCTAssertEqual(ShellLocalDesktop.hidden.label, "No desktop connected")
        XCTAssertEqual(ShellLocalDesktop.relay("Mac").label, "Connected to Mac through the relay")
        XCTAssertTrue(ShellLocalDesktop.relay("Mac").isConnected)
        XCTAssertTrue(ShellLocalDesktop.connected("Mac").isConnected)
        XCTAssertFalse(ShellLocalDesktop.inReach.isConnected)
        XCTAssertFalse(ShellLocalDesktop.connecting("Mac").isConnected)
    }

    // MARK: user status sheet

    func testSaveRules() {
        let now = Date(epochMillis: 1_770_000_000_000)
        XCTAssertEqual(
            userStatusSaveAction(emoji: "no", text: "x", clearAfter: .never, customAt: nil, now: now),
            .problem(userStatusErrorMessage("bad_status_emoji")))
        XCTAssertEqual(userStatusSaveAction(emoji: " ", text: "  ", clearAfter: .oneHour, customAt: nil, now: now), .clear)
        XCTAssertEqual(
            userStatusSaveAction(emoji: "🌴", text: "Away", clearAfter: .custom, customAt: now, now: now),
            .problem(userStatusErrorMessage("status_expiry_in_past")))
        XCTAssertEqual(
            userStatusSaveAction(emoji: "🌴", text: " Away ", clearAfter: .oneHour, customAt: nil, now: now),
            .setCustom(emoji: "🌴", text: "Away", expiresAt: 1_770_000_000_000 + 3_600_000))
        let later = now.addingTimeInterval(7200)
        XCTAssertEqual(
            userStatusSaveAction(emoji: "", text: "Busy", clearAfter: .custom, customAt: later, now: now),
            .setCustom(emoji: "", text: "Busy", expiresAt: 1_770_000_000_000 + 7_200_000))
        XCTAssertEqual(
            userStatusSaveAction(emoji: "", text: "Busy", clearAfter: .never, customAt: nil, now: now),
            .setCustom(emoji: "", text: "Busy", expiresAt: 0))
    }

    func testSheetStartsFromTheStoredStatus() {
        let now = Date(epochMillis: 1_770_000_000_000)
        XCTAssertEqual(initialClearAfter(UserStatus(text: "x")), .never)
        XCTAssertNil(initialCustomAt(UserStatus(text: "x")))
        let timed = UserStatus(text: "x", expiresAt: 1_770_000_600_000)
        XCTAssertEqual(initialClearAfter(timed), .custom)
        XCTAssertEqual(initialCustomAt(timed), Date(epochMillis: 1_770_000_600_000))
        XCTAssertEqual(userStatusCustomLabel(nil, now: now), "Pick a date and time")
        XCTAssertEqual(userStatusPickerStart(nil, now: now), now.addingTimeInterval(3600))
        XCTAssertEqual(userStatusPickerStart(now.addingTimeInterval(60), now: now), now.addingTimeInterval(60))
        XCTAssertEqual(userStatusPickerStart(now.addingTimeInterval(-60), now: now), now.addingTimeInterval(3600))
    }

    func testSettingsSummaryAndRepaintDelay() {
        let now = Date(epochMillis: 1_770_000_000_000)
        XCTAssertEqual(userStatusSummary(UserStatus(away: true), now: now), "Set a status")
        XCTAssertEqual(userStatusSummary(UserStatus(emoji: "🎧", text: "Focus"), now: now), "🎧 Focus")
        let timed = UserStatus(emoji: "🎧", expiresAt: 1_770_000_000_000 + 30 * 60_000)
        XCTAssertEqual(
            userStatusSummary(timed, now: now, timeZone: utc),
            "🎧 · \(timed.untilLabel(now, timeZone: utc))")
        XCTAssertEqual(userStatusRepaintDelay(timed, now: now), 1800)
        XCTAssertNil(userStatusRepaintDelay(UserStatus(emoji: "🎧"), now: now))
        XCTAssertNil(userStatusRepaintDelay(timed, now: now.addingTimeInterval(4000)))
    }

    func testBarKeepsItsOrderUntilRecentFirstIsOn() {
        let order = ShellBarOrder(store: InMemoryKeyValueStore())
        let items: [String] = ["Agents", "Inbox", "Repos", "Search", "settings", "world"]
        order.use("world")
        XCTAssertEqual(order.sort, .usual)
        XCTAssertEqual(order.ordered(items) { $0 }, items)
        order.sort = .recent
        XCTAssertEqual(order.ordered(items) { $0 }, ["world", "Agents", "Inbox", "Repos", "Search", "settings"])
        order.use("settings")
        order.use("Inbox")
        order.use("world")
        XCTAssertEqual(order.recent, ["world", "Inbox", "settings"])
        XCTAssertEqual(order.ordered(items) { $0 }, ["world", "Inbox", "settings", "Agents", "Repos", "Search"])
        order.clear()
        XCTAssertEqual(order.ordered(items) { $0 }, items)
    }

    func testRecentListIsCappedAndIgnoresGoneItems() {
        let many: [String] = (0..<(ShellBarOrder.limit + 5)).map { "page\($0)" }
        let recent: [String] = many.reduce([String]()) { ShellBarOrder.used($1, recent: $0) }
        XCTAssertEqual(recent.count, ShellBarOrder.limit)
        XCTAssertEqual(recent.first, many.last)
        XCTAssertEqual(ShellBarOrder.ordered(["a", "b"], recent: ["gone", "b"]) { $0 }, ["b", "a"])
    }

    func testMostUsedSortsByUseCountThenRecency() {
        let order = ShellBarOrder(store: InMemoryKeyValueStore())
        let items: [String] = ["Agents", "Inbox", "Repos", "Search", "settings", "world"]
        order.sort = .frequent
        XCTAssertEqual(order.ordered(items) { $0 }, items)
        for id in ["settings", "world", "settings", "Inbox", "settings", "world", "Search"] {
            order.use(id)
        }
        XCTAssertEqual(order.uses, ["settings": 3, "world": 2, "Inbox": 1, "Search": 1])
        XCTAssertEqual(order.ordered(items) { $0 }, ["settings", "world", "Search", "Inbox", "Agents", "Repos"])
        order.sort = .recent
        XCTAssertEqual(order.ordered(items) { $0 }, ["Search", "world", "settings", "Inbox", "Agents", "Repos"])
        order.clear()
        order.sort = .frequent
        XCTAssertEqual(order.uses, [:])
        XCTAssertEqual(order.ordered(items) { $0 }, items)
    }

    func testHeatGrowsWithUsesAndTopsOutAtTheMostUsed() {
        let ids: [String] = ["settings", "world", "Inbox", "Repos"]
        let uses: [String: Int] = ["settings": 40, "world": 5, "Inbox": 1, "gone": 400]
        XCTAssertEqual(ShellBarOrder.heat("settings", uses: uses, among: ids), 1, accuracy: 0.0001)
        XCTAssertEqual(ShellBarOrder.heat("Repos", uses: uses, among: ids), 0)
        let world: Double = ShellBarOrder.heat("world", uses: uses, among: ids)
        let inbox: Double = ShellBarOrder.heat("Inbox", uses: uses, among: ids)
        XCTAssertGreaterThan(world, inbox)
        XCTAssertGreaterThan(inbox, 0)
        XCTAssertLessThan(world, 1)
        XCTAssertEqual(ShellBarOrder.heat("settings", uses: [:], among: ids), 0)
    }

    func testFrequentMarkNeedsThreeOpensAndHalfTheTopHeat() {
        let ids: [String] = ["settings", "world", "Inbox", "Repos", "Search"]
        let uses: [String: Int] = ["settings": 40, "world": 12, "Inbox": 2, "Search": 1, "gone": 400]
        XCTAssertEqual(ShellBarOrder.frequentMark("settings", uses: uses, among: ids) ?? 0, 1, accuracy: 0.0001)
        let world: Double? = ShellBarOrder.frequentMark("world", uses: uses, among: ids)
        XCTAssertNotNil(world)
        XCTAssertLessThan(world ?? 1, 1)
        XCTAssertNil(ShellBarOrder.frequentMark("Inbox", uses: uses, among: ids))
        XCTAssertNil(ShellBarOrder.frequentMark("Search", uses: uses, among: ids))
        XCTAssertNil(ShellBarOrder.frequentMark("Repos", uses: uses, among: ids))
        XCTAssertNil(ShellBarOrder.frequentMark("Search", uses: ["Search": 2], among: ids))
        XCTAssertNotNil(ShellBarOrder.frequentMark("Search", uses: ["Search": 3], among: ids))
    }

    func testPullingHomeDownPastTheThresholdRevealsSearchOnce() {
        XCTAssertFalse(ShellBarOrder.revealsSearch(pulled: 0, shown: false))
        XCTAssertFalse(ShellBarOrder.revealsSearch(pulled: -200, shown: false))
        XCTAssertFalse(ShellBarOrder.revealsSearch(pulled: ShellBarOrder.searchPull - 1, shown: false))
        XCTAssertTrue(ShellBarOrder.revealsSearch(pulled: ShellBarOrder.searchPull, shown: false))
        XCTAssertFalse(ShellBarOrder.revealsSearch(pulled: 200, shown: true))
    }

    func testFittedTileShrinksRowsToTheScreenWithinBounds() {
        XCTAssertEqual(ShellBarOrder.fittedTile(height: 700, fixed: 100, rows: 10, tile: 42, minimum: 28), 42)
        XCTAssertEqual(ShellBarOrder.fittedTile(height: 500, fixed: 100, rows: 12, tile: 42, minimum: 28), 400.0 / 12, accuracy: 0.0001)
        XCTAssertEqual(ShellBarOrder.fittedTile(height: 300, fixed: 100, rows: 12, tile: 42, minimum: 28), 28)
        XCTAssertEqual(ShellBarOrder.fittedTile(height: 0, fixed: 100, rows: 12, tile: 42, minimum: 28), 42)
        XCTAssertEqual(ShellBarOrder.fittedTile(height: 500, fixed: 100, rows: 0, tile: 42, minimum: 28), 42)
        XCTAssertEqual(ShellBarOrder.fittedTile(height: 50, fixed: 100, rows: 4, tile: 20, minimum: 28), 20)
    }

    func testVisitCountsAPageOpenedAnotherWayOnce() {
        let order = ShellBarOrder(store: InMemoryKeyValueStore())
        order.use("Inbox")
        XCTAssertFalse(order.visit(["Inbox"]))
        XCTAssertTrue(order.visit(["Agents"]))
        XCTAssertFalse(order.visit(["Agents"]))
        XCTAssertFalse(order.visit([""]))
        XCTAssertFalse(order.visit([]))
        XCTAssertTrue(order.visit(["Inbox"]))
        order.use("repo-pulls")
        XCTAssertFalse(order.visit(["repo-pulls", "currentRepo"]))
        XCTAssertTrue(order.visit(["repo-issues", "currentRepo"]))
        XCTAssertEqual(order.uses, ["Inbox": 2, "Agents": 1, "repo-pulls": 1, "repo-issues": 1])
        XCTAssertEqual(order.recent, ["repo-issues", "repo-pulls", "Inbox", "Agents"])
    }

    func testHubItemIdMatchesThePageAHubItemOpens() {
        let groups: [HubGroup] = [
            HubGroup(id: "tools", title: "Tools", items: [
                HubItem(id: "timers", title: "Timers", symbol: "timer", action: .route(.timers)),
                HubItem(id: "world", title: "World", symbol: "globe", action: .world),
            ]),
            HubGroup(id: "you", title: "You", items: [
                HubItem(id: "settings", title: "Settings", symbol: "gear", action: .route(.settings(section: nil))),
            ]),
        ]
        XCTAssertEqual(ShellBarOrder.hubItemId(opening: .timers, in: groups), "timers")
        XCTAssertEqual(ShellBarOrder.hubItemId(opening: .settings(section: nil), in: groups), "settings")
        XCTAssertNil(ShellBarOrder.hubItemId(opening: .settings(section: "account"), in: groups))
        XCTAssertNil(ShellBarOrder.hubItemId(opening: .hosts, in: groups))
        XCTAssertNil(ShellBarOrder.hubItemId(opening: nil, in: groups))
    }

    func testSortReadsTheOldRecentFirstSwitchAndSeedsCountsFromRecent() {
        let store = InMemoryKeyValueStore()
        store.set(true, forKey: ShellBarOrder.recentFirstKey)
        store.set(["world", "Inbox"], forKey: ShellBarOrder.recentKey)
        let order = ShellBarOrder(store: store)
        XCTAssertEqual(order.sort, .recent)
        XCTAssertEqual(order.uses, ["world": 1, "Inbox": 1])
        order.sort = .frequent
        XCTAssertFalse(store.contains(ShellBarOrder.recentFirstKey))
        XCTAssertEqual(ShellBarOrder(store: store).sort, .frequent)
        XCTAssertEqual(order.ranking, ["world", "Inbox"])
        order.use("Inbox")
        XCTAssertEqual(order.ranking, ["Inbox", "world"])
    }

    func testUseCountsAreCappedAndKeepTheItemJustUsed() {
        var uses: [String: Int] = [:]
        var recent: [String] = []
        for index in 0..<ShellBarOrder.limit {
            let id = "page\(index)"
            recent = ShellBarOrder.used(id, recent: recent)
            uses = ShellBarOrder.counted(id, uses: uses, recent: recent)
            recent = ShellBarOrder.used(id, recent: recent)
            uses = ShellBarOrder.counted(id, uses: uses, recent: recent)
        }
        recent = ShellBarOrder.used("new", recent: recent)
        uses = ShellBarOrder.counted("new", uses: uses, recent: recent)
        XCTAssertEqual(uses.count, ShellBarOrder.limit)
        XCTAssertEqual(uses["new"], 1)
        XCTAssertNil(uses["page0"])
        XCTAssertEqual(ShellBarOrder.decodeUses(ShellBarOrder.encodeUses(["a:b": 2, "c": 5])), ["a:b": 2, "c": 5])
        XCTAssertEqual(ShellBarOrder.decodeUses(["x", "0:zero", "-1:neg", "3:", "2:ok"]), ["ok": 2])
    }
}
