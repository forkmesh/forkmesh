import FMBase
import XCTest
@testable import FMApi

final class ShellBarStyleTests: XCTestCase {
    func testTheDefaultIsTheGlassCapsuleOfBefore() {
        let style: ShellBarStyle = ShellBarStyle()
        XCTAssertEqual(style.shape, .capsule)
        XCTAssertEqual(style.fill, .glass)
        XCTAssertEqual(style.icons, .filled)
        XCTAssertEqual(style.labels, .always)
        XCTAssertEqual(style.selection, .lens)
        XCTAssertEqual(style.limit, ShellBarLayout.barLimit)
        XCTAssertFalse(style.showCounts)
        XCTAssertTrue(style.showActivity)
        XCTAssertEqual(style.marks, .home)
        XCTAssertTrue(style.isDefault)
        let metrics: ShellBarMetrics = style.metrics(available: 366, count: 6, cell: 64, height: 52)
        XCTAssertEqual(metrics.slot, 58)
        XCTAssertEqual(metrics.height, 52)
        XCTAssertEqual(metrics.gap, 2)
        XCTAssertEqual(metrics.width, 366)
        XCTAssertFalse(metrics.scrolls)
        XCTAssertEqual(style.barRadius(height: 52), 30)
    }

    func testAFewIconsKeepTheirWidthAndFitHugsThem() {
        var style: ShellBarStyle = ShellBarStyle()
        XCTAssertEqual(style.metrics(available: 366, count: 3, cell: 64, height: 52).slot, 64)
        style.width = .fit
        let fit: ShellBarMetrics = style.metrics(available: 366, count: 3, cell: 64, height: 52)
        XCTAssertEqual(fit.width, 8 + 3 * 64 + 2 * 2)
    }

    func testSquaresAreSquareAndSpreadAcrossAFullBar() {
        var style: ShellBarStyle = ShellBarStyle()
        style.shape = .squares
        let full: ShellBarMetrics = style.metrics(available: 366, count: 5, cell: 64, height: 52)
        XCTAssertEqual(full.slot, 52)
        XCTAssertEqual(full.height, 52)
        XCTAssertEqual(full.gap, (366 - 8 - 5 * 52) / 4, accuracy: 0.001)
        XCTAssertEqual(full.width, 366)
        style.width = .fit
        style.spacing = 0.5
        let fit: ShellBarMetrics = style.metrics(available: 366, count: 5, cell: 64, height: 52)
        XCTAssertEqual(fit.gap, 10)
        XCTAssertEqual(fit.width, 8 + 5 * 52 + 4 * 10)
    }

    func testCrowdedSquaresShrinkToTheFloorThenScroll() {
        var style: ShellBarStyle = ShellBarStyle()
        style.shape = .circles
        let seven: ShellBarMetrics = style.metrics(available: 300, count: 7, cell: 64, height: 52)
        XCTAssertEqual(seven.slot, ShellBarStyle.minimumSlot)
        XCTAssertTrue(seven.scrolls)
        XCTAssertEqual(style.cellRadius(side: 50), 25)
    }

    func testADoubleHeightBarStacksTwoRowsAndHoldsTwiceAsMany() {
        var style: ShellBarStyle = ShellBarStyle()
        XCTAssertEqual(style.rows, .single)
        XCTAssertEqual(style.capacity, 6)
        XCTAssertEqual(style.rowSplit([1, 2, 3]), [[1, 2, 3]])
        style.rows = .double
        XCTAssertFalse(style.isDefault)
        XCTAssertEqual(style.capacity, 12)
        XCTAssertEqual(style.rowSplit([1, 2, 3, 4, 5]), [[1, 2, 3], [4, 5]])
        XCTAssertEqual(style.rowSplit([1]), [[1]])
        let twelve: ShellBarMetrics = style.metrics(available: 366, count: 12, cell: 64, height: 52)
        XCTAssertEqual(twelve.rows, 2)
        XCTAssertEqual(twelve.columns, 6)
        XCTAssertEqual(twelve.slot, 58)
        XCTAssertEqual(twelve.height, 52)
        XCTAssertEqual(twelve.rowGap, 2)
        XCTAssertFalse(twelve.scrolls)
        let one: ShellBarMetrics = style.metrics(available: 366, count: 1, cell: 64, height: 52)
        XCTAssertEqual(one.rows, 1)
        XCTAssertEqual(one.rowGap, 0)
        style.shape = .squares
        let squares: ShellBarMetrics = style.metrics(available: 366, count: 10, cell: 64, height: 52)
        XCTAssertEqual(squares.columns, 5)
        XCTAssertEqual(squares.gap, (366 - 8 - 5 * 52) / 4, accuracy: 0.001)
        XCTAssertEqual(squares.rowGap, 4)
    }

    func testSizeAndLabelsChangeTheHeight() {
        var style: ShellBarStyle = ShellBarStyle()
        style.size = .extraLarge
        XCTAssertEqual(style.cellHeight(52), 68)
        style.labels = .never
        XCTAssertEqual(style.cellHeight(52), 54)
        style.size = .small
        XCTAssertEqual(style.cellHeight(52), ShellBarStyle.minimumSlot)
        XCTAssertTrue(ShellBarLabels.selected.shows(selected: true))
        XCTAssertFalse(ShellBarLabels.selected.shows(selected: false))
    }

    func testCornersFollowTheShape() {
        var style: ShellBarStyle = ShellBarStyle()
        style.shape = .panel
        style.corner = 0
        XCTAssertEqual(style.barRadius(height: 52), 4)
        style.corner = 1
        XCTAssertEqual(style.barRadius(height: 52), 30)
        style.shape = .squares
        XCTAssertEqual(style.cellRadius(side: 52), 26)
        style.corner = 0.5
        XCTAssertEqual(style.cellRadius(side: 52), 14)
        style.shape = .icons
        XCTAssertFalse(style.shape.backed)
        XCTAssertTrue(style.shape.separate)
    }

    func testSavingRoundTripsAndTheDefaultLeavesNoKey() {
        let store: InMemoryKeyValueStore = InMemoryKeyValueStore()
        ShellBarStyle().save(store)
        XCTAssertNil(store.string(forKey: ShellBarStyle.key))
        var style: ShellBarStyle = ShellBarStyle()
        style.shape = .squares
        style.fill = .tinted
        style.icons = .tiles
        style.size = .large
        style.labels = .selected
        style.selection = .dot
        style.width = .fit
        style.corner = 0.25
        style.spacing = 0.75
        style.limit = 8
        style.showCounts = true
        style.showActivity = false
        style.highlightFrequent = true
        style.marks = .plain
        style.rows = .double
        style.save(store)
        XCTAssertEqual(ShellBarStyle.load(store), style)
        ShellBarStyle().save(store)
        XCTAssertNil(store.string(forKey: ShellBarStyle.key))
    }

    func testAnOldOrBrokenSaveFallsBackFieldByField() {
        let store: InMemoryKeyValueStore = InMemoryKeyValueStore()
        store.set(
            "{\"shape\":\"squares\",\"limit\":40,\"corner\":7,\"spacing\":-2,\"fill\":\"chrome\"}",
            forKey: ShellBarStyle.key)
        let style: ShellBarStyle = ShellBarStyle.load(store)
        XCTAssertEqual(style.shape, .squares)
        XCTAssertEqual(style.limit, 8)
        XCTAssertEqual(style.corner, 1)
        XCTAssertEqual(style.spacing, 0)
        XCTAssertEqual(style.fill, .glass)
        XCTAssertEqual(style.marks, .home)
        store.set("{\"marks\":\"plain\"}", forKey: ShellBarStyle.key)
        XCTAssertEqual(ShellBarStyle.load(store).marks, .plain)
        store.set("{\"rows\":\"triple\"}", forKey: ShellBarStyle.key)
        XCTAssertEqual(ShellBarStyle.load(store).rows, .single)
        store.set("{\"marks\":\"sparkly\"}", forKey: ShellBarStyle.key)
        XCTAssertEqual(ShellBarStyle.load(store).marks, .home)
        store.set("not json", forKey: ShellBarStyle.key)
        XCTAssertEqual(ShellBarStyle.load(store), ShellBarStyle())
        var wild: ShellBarStyle = ShellBarStyle()
        wild.limit = 1
        wild.corner = .nan
        XCTAssertEqual(wild.clamped.limit, 3)
        XCTAssertEqual(wild.clamped.corner, 0)
    }
}
