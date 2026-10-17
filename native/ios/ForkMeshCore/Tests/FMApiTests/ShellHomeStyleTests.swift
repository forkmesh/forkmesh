import FMBase
import XCTest
@testable import FMApi

final class ShellHomeStyleTests: XCTestCase {
    private let sections: [ShellHomeSection] = [
        ShellHomeSection(id: "main", title: "Go to", entries: [
            ShellHomeEntry(id: "Agents", title: "Agents", badge: 31, active: true),
            ShellHomeEntry(id: "Inbox", title: "Inbox", count: 0),
            ShellHomeEntry(id: "Repos", title: "Repos", badge: 2),
        ]),
        ShellHomeSection(id: "workspace", title: "Workspace", entries: [
            ShellHomeEntry(id: "notes", title: "Notes", count: 9),
            ShellHomeEntry(id: "tasks", title: "Tasks", count: 16),
        ]),
        ShellHomeSection(id: "business", title: "Business", entries: [
            ShellHomeEntry(id: "crm", title: "CRM", count: 1840),
            ShellHomeEntry(id: "market", title: "Market", count: 3),
            ShellHomeEntry(id: "adReview", title: "Ad review"),
            ShellHomeEntry(id: "analytics", title: "Analytics", active: true),
        ]),
    ]

    private func ids(_ style: ShellHomeStyle, uses: [String: Int] = [:], recent: [String] = []) -> [[String]] {
        style.arranged(sections, uses: uses, recent: recent).map(\.ids)
    }

    func testTheDefaultKeepsEverySectionInItsOwnOrder() {
        let style: ShellHomeStyle = ShellHomeStyle()
        XCTAssertEqual(ids(style), sections.map { $0.entries.map(\.id) })
        XCTAssertEqual(style.arranged(sections, uses: [:], recent: []).map(\.title), ["Go to", "Workspace", "Business"])
        XCTAssertTrue(style.headed)
        XCTAssertEqual(style.across, 5)
        XCTAssertTrue(style.highlightFrequent)
    }

    func testNameSortsWithinEachSection() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.sort = .name
        XCTAssertEqual(ids(style)[2], ["adReview", "analytics", "crm", "market"])
        style.sort = .nameDescending
        XCTAssertEqual(ids(style)[2], ["market", "crm", "analytics", "adReview"])
    }

    func testUseSortsKeepTheCustomOrderForTies() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.sort = .mostUsed
        let uses: [String: Int] = ["Repos": 4, "Inbox": 9]
        XCTAssertEqual(ids(style, uses: uses)[0], ["Inbox", "Repos", "Agents"])
        style.sort = .leastUsed
        XCTAssertEqual(ids(style, uses: uses)[0], ["Agents", "Repos", "Inbox"])
    }

    func testRecentPutsTheNewestFirstAndTheNeverOpenedLast() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.sort = .recent
        XCTAssertEqual(ids(style, recent: ["market", "Repos", "crm"])[2], ["market", "crm", "adReview", "analytics"])
    }

    func testAttentionPutsUnreadThenActiveFirst() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.sort = .attention
        XCTAssertEqual(ids(style)[0], ["Agents", "Repos", "Inbox"])
        XCTAssertEqual(ids(style)[2], ["analytics", "crm", "market", "adReview"])
    }

    func testBiggestCountPutsUnknownCountsLast() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.sort = .count
        XCTAssertEqual(ids(style)[2], ["crm", "market", "adReview", "analytics"])
        XCTAssertEqual(ids(style)[0], ["Agents", "Repos", "Inbox"])
    }

    func testHiddenSectionsLeaveHomeButNotASearch() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.setShown("workspace", false)
        XCTAssertFalse(style.shows("workspace"))
        XCTAssertEqual(style.arranged(sections, uses: [:], recent: []).map(\.id), ["main", "business"])
        XCTAssertEqual(style.arranged(sections, uses: [:], recent: [], searching: true).count, 3)
        style.setShown("workspace", true)
        XCTAssertTrue(style.hidden.isEmpty)
    }

    func testBarIconsLeaveHomeByDefaultButNotASearch() {
        var style: ShellHomeStyle = ShellHomeStyle()
        XCTAssertTrue(style.hideBarIcons)
        let docked: Set<String> = ["Agents", "Inbox", "Repos", "notes"]
        let arranged: [ShellHomeArranged] = style.arranged(sections, uses: [:], recent: [], docked: docked)
        XCTAssertEqual(arranged.map(\.id), ["workspace", "business"])
        XCTAssertEqual(arranged[0].ids, ["tasks"])
        XCTAssertEqual(
            style.arranged(sections, uses: [:], recent: [], docked: docked, searching: true).map(\.ids),
            sections.map { $0.entries.map(\.id) })
        style.grouped = false
        XCTAssertFalse(style.arranged(sections, uses: [:], recent: [], docked: docked)[0].ids.contains("Agents"))
        style = ShellHomeStyle()
        style.hideBarIcons = false
        XCTAssertEqual(
            style.arranged(sections, uses: [:], recent: [], docked: docked).map(\.ids), sections.map { $0.entries.map(\.id) })
        XCTAssertFalse(style.isDefault)
    }

    func testUngroupedIsOneSectionSortedAcrossEverything() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.grouped = false
        style.sort = .name
        let arranged: [ShellHomeArranged] = style.arranged(sections, uses: [:], recent: [])
        XCTAssertEqual(arranged.count, 1)
        XCTAssertEqual(arranged[0].id, ShellHomeStyle.allId)
        XCTAssertEqual(arranged[0].ids.prefix(4), ["adReview", "Agents", "analytics", "crm"])
        XCTAssertFalse(style.headed)
        XCTAssertFalse(style.reorders)
    }

    func testSectionOrders() {
        var style: ShellHomeStyle = ShellHomeStyle()
        style.groupOrder = .name
        XCTAssertEqual(style.arranged(sections, uses: [:], recent: []).map(\.id), ["business", "main", "workspace"])
        style.groupOrder = .size
        XCTAssertEqual(style.arranged(sections, uses: [:], recent: []).map(\.id), ["business", "main", "workspace"])
        style.groupOrder = .mostUsed
        XCTAssertEqual(
            style.arranged(sections, uses: ["notes": 5, "crm": 2], recent: []).map(\.id), ["workspace", "business", "main"])
    }

    func testSavingRoundTripsAndTheDefaultLeavesNoKey() {
        let store: InMemoryKeyValueStore = InMemoryKeyValueStore()
        ShellHomeStyle().save(store)
        XCTAssertNil(store.string(forKey: ShellHomeStyle.key))
        var style: ShellHomeStyle = ShellHomeStyle()
        style.sort = .attention
        style.icons = .tiles
        style.across = 6
        style.highlightFrequent = false
        style.hideBarIcons = false
        style.setShown("personal", false)
        style.save(store)
        XCTAssertEqual(ShellHomeStyle.load(store), style)
        style = ShellHomeStyle()
        style.save(store)
        XCTAssertNil(store.string(forKey: ShellHomeStyle.key))
    }

    func testAnOldOrBrokenSaveFallsBackFieldByField() {
        let store: InMemoryKeyValueStore = InMemoryKeyValueStore()
        store.set("{\"sort\":\"name\",\"across\":12,\"icons\":\"sparkly\"}", forKey: ShellHomeStyle.key)
        let style: ShellHomeStyle = ShellHomeStyle.load(store)
        XCTAssertEqual(style.sort, .name)
        XCTAssertEqual(style.across, 6)
        XCTAssertEqual(style.icons, .outline)
        XCTAssertTrue(style.grouped)
        XCTAssertTrue(style.hideBarIcons)
        store.set("not json", forKey: ShellHomeStyle.key)
        XCTAssertEqual(ShellHomeStyle.load(store), ShellHomeStyle())
    }

    func testRowsCountEachSectionOnItsOwnRows() {
        let arranged: [ShellHomeArranged] = ShellHomeStyle().arranged(sections, uses: [:], recent: [])
        XCTAssertEqual(ShellHomeStyle.rows(arranged, across: 5), 3)
        XCTAssertEqual(ShellHomeStyle.rows(arranged, across: 3), 4)
    }
}
