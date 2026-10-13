import XCTest

@testable import FMApi

final class PackageGateSearchTests: XCTestCase {
    override func tearDown() {
        PackageGate.publish(nil)
        super.tearDown()
    }

    func testNothingPublishedShowsEverything() {
        PackageGate.publish(nil)
        XCTAssertEqual(DestinationsSearchSource.defaultRoutes, DestinationsSearchSource.allRoutes)
        XCTAssertEqual(CommandPalette.tabEntries.count, AppTab.barOrder.count)
    }

    func testSearchDestinationsFollowTheGate() async throws {
        PackageGate.publish([])
        let routes = DestinationsSearchSource.defaultRoutes
        XCTAssertTrue(routes.contains(.repos(.all)))
        XCTAssertTrue(routes.contains(.settings(section: nil)))
        XCTAssertTrue(routes.contains(.market(filter: nil)))
        XCTAssertFalse(routes.contains(.notes))
        XCTAssertFalse(routes.contains(.agents(.all)))
        XCTAssertFalse(routes.contains(.chat))
        XCTAssertFalse(routes.contains(.status))
        let hits = try await DestinationsSearchSource().search("notes")
        XCTAssertFalse(hits.contains { $0.route == .notes })
        PackageGate.publish(["notes"])
        let shown = try await DestinationsSearchSource().search("notes")
        XCTAssertTrue(shown.contains { $0.route == .notes })
    }

    func testThePaletteHidesEntriesOfRemovedPackages() {
        PackageGate.publish([])
        XCTAssertEqual(CommandPalette.tabEntries.map(\.id), ["tab:code"])
        let context = PaletteContext(recents: [.notes, .repos(.all)])
        let entries = CommandPalette.sections(query: "", context: context).flatMap(\.entries)
        XCTAssertFalse(entries.contains { $0.id == "compose" })
        XCTAssertFalse(entries.contains { $0.id == "newNote" })
        XCTAssertFalse(entries.contains { $0.id == "timer" })
        XCTAssertFalse(entries.contains { $0.command == .open(.notes) })
        XCTAssertTrue(entries.contains { $0.command == .open(.repos(.all)) })
        XCTAssertFalse(CommandPalette.destinationEntries().contains { $0.command == .open(.notes) })
        PackageGate.publish(["agents", "notes"])
        let more = CommandPalette.sections(query: "", context: context).flatMap(\.entries)
        XCTAssertTrue(more.contains { $0.id == "compose" })
        XCTAssertTrue(more.contains { $0.id == "newNote" })
    }

    func testQuickActionsFollowTheGate() {
        PackageGate.publish([])
        XCTAssertEqual(ShellQuickAction.availableActions, [.search])
        XCTAssertEqual(ShellQuickAction.dynamicItems(recents: [.agent(id: "a1")]).count, 0)
        PackageGate.publish(["notes", "agents"])
        XCTAssertEqual(ShellQuickAction.availableActions, [.newAgent, .search, .newNote])
        XCTAssertEqual(ShellQuickAction.dynamicItems(recents: [.agent(id: "a1")]).map(\.payload), ["a1"])
    }

    func testSearchBucketsFollowTheGate() {
        let sources: [GlobalSearchSource] = [DestinationsSearchSource(), CommitsSearchSource(repos: { _ in [] }) { _ in [] }]
        XCTAssertEqual(PackageGate.filter(sources, installed: []).map(\.kind), [.destinations])
        XCTAssertEqual(PackageGate.filter(sources, installed: ["commits"]).map(\.kind), [.destinations, .commits])
    }
}
