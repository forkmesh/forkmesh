import FMBase
import XCTest
@testable import FMApi

final class ShellMenuLayoutTests: XCTestCase {
    private let defaults: [(String, [String])] = [
        ("main", ["home", "Agents", "Inbox", "Repos"]),
        ("repo", ["repo-pulls", "repo-issues"]),
        ("workspace", ["notes", "tasks"]),
        ("business", []),
    ]

    private func arranged(_ layout: ShellMenuLayout, _ groups: [(String, [String])]? = nil) -> [[String]] {
        layout.arranged(groups ?? defaults, group: { $0.0 }, items: { $0.1 }, id: { $0 })
    }

    private func current(_ layout: ShellMenuLayout) -> [(String, [String])] {
        Array(zip(defaults.map(\.0), arranged(layout)))
    }

    func testAnEmptyLayoutKeepsTheDefaultGroups() {
        XCTAssertEqual(arranged(ShellMenuLayout()), defaults.map(\.1))
        XCTAssertTrue(ShellMenuLayout().isEmpty)
    }

    func testMovingForwardInAGroupLandsAfterTheTarget() {
        var layout = ShellMenuLayout()
        layout.move("home", to: "main", near: "Inbox", current: current(layout))
        XCTAssertEqual(arranged(layout)[0], ["Agents", "Inbox", "home", "Repos"])
    }

    func testMovingBackwardInAGroupLandsBeforeTheTarget() {
        var layout = ShellMenuLayout()
        layout.move("Repos", to: "main", near: "Agents", current: current(layout))
        XCTAssertEqual(arranged(layout)[0], ["home", "Repos", "Agents", "Inbox"])
    }

    func testAnItemMovesIntoAnotherGroup() {
        var layout = ShellMenuLayout()
        layout.move("repo-pulls", to: "main", near: "Repos", current: current(layout))
        XCTAssertEqual(arranged(layout)[0], ["home", "Agents", "Inbox", "repo-pulls", "Repos"])
        XCTAssertEqual(arranged(layout)[1], ["repo-issues"])
        layout.move("tasks", to: "business", near: nil, current: current(layout))
        XCTAssertEqual(arranged(layout)[2], ["notes"])
        XCTAssertEqual(arranged(layout)[3], ["tasks"])
    }

    func testAnAbsentItemKeepsItsPlaceForWhenItComesBack() {
        var layout = ShellMenuLayout()
        layout.move("repo-pulls", to: "main", near: "home", current: current(layout))
        let noRepo: [(String, [String])] = defaults.filter { $0.0 != "repo" }
        let withoutRepo: [(String, [String])] = Array(zip(noRepo.map(\.0), arranged(layout, noRepo)))
        XCTAssertEqual(withoutRepo[0].1, ["home", "Agents", "Inbox", "Repos"])
        layout.move("notes", to: "main", near: nil, current: withoutRepo)
        XCTAssertEqual(arranged(layout)[0], ["repo-pulls", "home", "Agents", "Inbox", "Repos", "notes"])
    }

    func testNewItemsJoinTheEndOfTheirGroupAndAMissingGroupFallsBack() {
        var layout = ShellMenuLayout()
        layout.move("tasks", to: "business", near: nil, current: current(layout))
        var grown = defaults
        grown[0].1.append("Search")
        XCTAssertEqual(arranged(layout, grown)[0], ["home", "Agents", "Inbox", "Repos", "Search"])
        let noBusiness: [(String, [String])] = defaults.filter { $0.0 != "business" }
        XCTAssertEqual(arranged(layout, noBusiness)[2], ["notes", "tasks"])
    }

    func testTheLayoutRoundTripsThroughTheStore() {
        let store = InMemoryKeyValueStore()
        var layout = ShellMenuLayout()
        layout.move("notes", to: "main", near: "home", current: current(layout))
        layout.save(store)
        XCTAssertEqual(ShellMenuLayout.load(store), layout)
        ShellMenuLayout().save(store)
        XCTAssertNil(store.string(forKey: ShellMenuLayout.key))
    }
}
