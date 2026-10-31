package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ShellIslandTest {
    fun testPunchHoleWithATallStatusBarPutsDotsAboveTheHoleAndLightsBesideTheClock() {
        val hole = IslandRect(195f, 12f, 225f, 40f)
        val layout = ShellIsland.layout(420f, 48f, hole)
        expect(layout.inStatusBar)
        assertEq(IslandBand(0f, 0f, 420f, 12f), layout.dots)
        assertEq(IslandBand(24f, 2f, 171f, 12f), layout.lights)
        assertEq(IslandBand(235f, 2f, 175f, 12f), layout.charts)
    }

    fun testANotchSendsTheDotsUnderTheStatusBar() {
        val notch = IslandRect(140f, 0f, 280f, 44f)
        val layout = ShellIsland.layout(420f, 46f, notch)
        expect(layout.inStatusBar)
        assertEq(IslandBand(0f, 47f, 420f, ShellIsland.STRIP), layout.dots)
        assertEq(116f, layout.lights.width)
    }

    fun testAShortStatusBarSharesOneRowUnderIt() {
        val layout = ShellIsland.layout(400f, 24f, IslandRect(190f, 6f, 210f, 22f))
        expect(!layout.inStatusBar)
        assertEq(null, layout.charts)
        assertEq(IslandBand(0f, 25f, 400f, ShellIsland.EAR_HEIGHT), layout.lights)
        assertEq(layout.lights, layout.dots)
    }

    fun testASideCutoutCountsAsNoIsland() {
        assertEq(null, ShellIsland.island(800f, 48f, IslandRect(0f, 10f, 30f, 40f)))
        assertEq(null, ShellIsland.island(800f, 48f, IslandRect(380f, 60f, 420f, 90f)))
        val layout = ShellIsland.layout(420f, 48f, null)
        expect(layout.inStatusBar)
        assertEq(IslandBand(24f, 2f, 123f, 12f), layout.lights)
        assertEq(IslandBand(0f, 0f, 420f, 7f), layout.dots)
    }

    fun testChartsShrinkToFitTheEar() {
        assertEq(12f, ShellIsland.fittedChartWidth(200f, 5, 1f, 12f))
        assertEq(5f, ShellIsland.fittedChartWidth(29f, 5, 1f, 12f))
        assertEq(2f, ShellIsland.fittedChartWidth(4f, 5, 1f, 12f))
    }
}
