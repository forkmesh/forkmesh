package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class ShellBarStyleTest {
    private fun near(expected: Double, actual: Double) = expect(abs(expected - actual) < 0.001, "expected <$expected> but was <$actual>")

    fun testTheDefaultIsTheGlassCapsuleOfBefore() {
        val style = ShellBarStyle()
        assertEq(ShellBarShape.capsule, style.shape)
        assertEq(ShellBarFill.glass, style.fill)
        assertEq(ShellHomeIconStyle.outline, style.icons)
        assertEq(ShellBarLabels.always, style.labels)
        assertEq(ShellBarSelection.fill, style.selection)
        assertEq(ShellBarLayout.BAR_LIMIT, style.limit)
        expect(!style.showCounts)
        expect(style.showActivity)
        assertEq(ShellBarMarks.home, style.marks)
        expect(style.isDefault)
        val metrics = style.metrics(366.0, 6, 64.0, 52.0)
        assertEq(58.0, metrics.slot)
        assertEq(52.0, metrics.height)
        assertEq(2.0, metrics.gap)
        assertEq(366.0, metrics.width)
        expect(!metrics.scrolls)
        assertEq(30.0, style.barRadius(52.0))
    }

    fun testAFewIconsKeepTheirWidthAndFitHugsThem() {
        val style = ShellBarStyle()
        assertEq(64.0, style.metrics(366.0, 3, 64.0, 52.0).slot)
        style.width = ShellBarWidth.fit
        val fit = style.metrics(366.0, 3, 64.0, 52.0)
        assertEq(8.0 + 3 * 64 + 2 * 2, fit.width)
    }

    fun testSquaresAreSquareAndSpreadAcrossAFullBar() {
        val style = ShellBarStyle()
        style.shape = ShellBarShape.squares
        val full = style.metrics(366.0, 5, 64.0, 52.0)
        assertEq(52.0, full.slot)
        assertEq(52.0, full.height)
        near((366.0 - 8 - 5 * 52) / 4, full.gap)
        assertEq(366.0, full.width)
        style.width = ShellBarWidth.fit
        style.spacing = 0.5
        val fit = style.metrics(366.0, 5, 64.0, 52.0)
        assertEq(10.0, fit.gap)
        assertEq(8.0 + 5 * 52 + 4 * 10, fit.width)
    }

    fun testCrowdedSquaresShrinkToTheFloorThenScroll() {
        val style = ShellBarStyle()
        style.shape = ShellBarShape.circles
        val seven = style.metrics(300.0, 7, 64.0, 52.0)
        assertEq(ShellBarStyle.MINIMUM_SLOT, seven.slot)
        expect(seven.scrolls)
        assertEq(25.0, style.cellRadius(50.0))
    }

    fun testADoubleHeightBarStacksTwoRowsAndHoldsTwiceAsMany() {
        val style = ShellBarStyle()
        assertEq(ShellBarRows.single, style.rows)
        assertEq(6, style.capacity)
        assertEq(listOf(listOf(1, 2, 3)), style.rowSplit(listOf(1, 2, 3)))
        style.rows = ShellBarRows.double
        expect(!style.isDefault)
        assertEq(12, style.capacity)
        assertEq(listOf(listOf(1, 2, 3), listOf(4, 5)), style.rowSplit(listOf(1, 2, 3, 4, 5)))
        assertEq(listOf(listOf(1)), style.rowSplit(listOf(1)))
        val twelve = style.metrics(366.0, 12, 64.0, 52.0)
        assertEq(2, twelve.rows)
        assertEq(6, twelve.columns)
        assertEq(58.0, twelve.slot)
        assertEq(52.0, twelve.height)
        assertEq(2.0, twelve.rowGap)
        expect(!twelve.scrolls)
        val one = style.metrics(366.0, 1, 64.0, 52.0)
        assertEq(1, one.rows)
        assertEq(0.0, one.rowGap)
        style.shape = ShellBarShape.squares
        val squares = style.metrics(366.0, 10, 64.0, 52.0)
        assertEq(5, squares.columns)
        near((366.0 - 8 - 5 * 52) / 4, squares.gap)
        assertEq(4.0, squares.rowGap)
    }

    fun testSizeAndLabelsChangeTheHeight() {
        val style = ShellBarStyle()
        style.size = ShellBarSize.extraLarge
        assertEq(68.0, style.cellHeight(52.0))
        style.labels = ShellBarLabels.never
        assertEq(54.0, style.cellHeight(52.0))
        style.size = ShellBarSize.small
        assertEq(ShellBarStyle.MINIMUM_SLOT, style.cellHeight(52.0))
        expect(ShellBarLabels.selected.shows(true))
        expect(!ShellBarLabels.selected.shows(false))
    }

    fun testCornersFollowTheShape() {
        val style = ShellBarStyle()
        style.shape = ShellBarShape.panel
        style.corner = 0.0
        assertEq(4.0, style.barRadius(52.0))
        style.corner = 1.0
        assertEq(30.0, style.barRadius(52.0))
        style.shape = ShellBarShape.squares
        assertEq(26.0, style.cellRadius(52.0))
        style.corner = 0.5
        assertEq(14.0, style.cellRadius(52.0))
        style.shape = ShellBarShape.icons
        expect(!style.shape.backed)
        expect(style.shape.separate)
    }

    fun testSavingRoundTripsAndTheDefaultLeavesNoKey() {
        val store = MemoryPrefStore()
        ShellBarStyle().save(store)
        assertEq(null, store.getString(ShellBarStyle.KEY))
        val style = ShellBarStyle()
        style.shape = ShellBarShape.squares
        style.fill = ShellBarFill.tinted
        style.icons = ShellHomeIconStyle.tiles
        style.size = ShellBarSize.large
        style.labels = ShellBarLabels.selected
        style.selection = ShellBarSelection.dot
        style.width = ShellBarWidth.fit
        style.corner = 0.25
        style.spacing = 0.75
        style.limit = 8
        style.showCounts = true
        style.showActivity = false
        style.highlightFrequent = true
        style.marks = ShellBarMarks.plain
        style.rows = ShellBarRows.double
        style.save(store)
        assertEq(style, ShellBarStyle.load(store))
        ShellBarStyle().save(store)
        assertEq(null, store.getString(ShellBarStyle.KEY))
    }

    fun testTheSavedJsonUsesTheSharedFieldNamesAndValues() {
        val store = MemoryPrefStore()
        val style = ShellBarStyle()
        style.size = ShellBarSize.extraLarge
        style.selection = ShellBarSelection.underline
        style.corner = 0.25
        style.limit = 7
        style.save(store)
        val json = com.forkmesh.nativeapp.json.Json.parseObject(store.getString("shell/bar-style")!!)
        assertEq("extraLarge", json["size"])
        assertEq("underline", json["selection"])
        assertEq("capsule", json["shape"])
        assertEq(0.25, json["corner"])
        assertEq(7L, json["limit"])
        assertEq("single", json["rows"])
        assertEq(15, json.size)
    }

    fun testAnOldOrBrokenSaveFallsBackFieldByField() {
        val store = MemoryPrefStore()
        store.putString(ShellBarStyle.KEY, "{\"shape\":\"squares\",\"limit\":40,\"corner\":7,\"spacing\":-2,\"fill\":\"chrome\"}")
        val style = ShellBarStyle.load(store)
        assertEq(ShellBarShape.squares, style.shape)
        assertEq(8, style.limit)
        assertEq(1.0, style.corner)
        assertEq(0.0, style.spacing)
        assertEq(ShellBarFill.glass, style.fill)
        assertEq(ShellBarMarks.home, style.marks)
        store.putString(ShellBarStyle.KEY, "{\"marks\":\"plain\"}")
        assertEq(ShellBarMarks.plain, ShellBarStyle.load(store).marks)
        store.putString(ShellBarStyle.KEY, "{\"rows\":\"triple\"}")
        assertEq(ShellBarRows.single, ShellBarStyle.load(store).rows)
        store.putString(ShellBarStyle.KEY, "{\"marks\":\"sparkly\"}")
        assertEq(ShellBarMarks.home, ShellBarStyle.load(store).marks)
        store.putString(ShellBarStyle.KEY, "{\"limit\":5.5,\"showCounts\":1}")
        assertEq(6, ShellBarStyle.load(store).limit)
        expect(!ShellBarStyle.load(store).showCounts)
        store.putString(ShellBarStyle.KEY, "not json")
        assertEq(ShellBarStyle(), ShellBarStyle.load(store))
        val wild = ShellBarStyle()
        wild.limit = 1
        wild.corner = Double.NaN
        assertEq(3, wild.clamped.limit)
        assertEq(0.0, wild.clamped.corner)
    }
}
