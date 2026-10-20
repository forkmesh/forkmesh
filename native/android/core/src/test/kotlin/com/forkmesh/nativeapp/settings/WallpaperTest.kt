package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class WallpaperTest {
    fun testDefaultsMatchIos() {
        val wallpaper = Wallpaper()
        assertEq("", wallpaper.image)
        assertEq(0.4, wallpaper.dim)
        assertEq(false, wallpaper.active)
        assertEq(0.9, Wallpaper.MAX_DIM)
    }

    fun testStoredKeysMatchIos() {
        assertEq("appearance/backgroundImage", AppearanceKeys.BACKGROUND_IMAGE)
        assertEq("appearance/backgroundDim", AppearanceKeys.BACKGROUND_DIM)
    }

    fun testDimClampsToZeroAndNinety() {
        assertEq(0.0, Wallpaper.clampDim(-0.3))
        assertEq(0.9, Wallpaper.clampDim(1.4))
        assertEq(0.55, Wallpaper.clampDim(0.55))
        assertEq(0.0, Wallpaper.clampDim(0.0))
        assertEq(0.9, Wallpaper.clampDim(0.9))
    }

    fun testMissingOrInvalidDimFallsBackToTheDefault() {
        assertEq(0.4, Wallpaper.clampDim(null))
        assertEq(0.4, Wallpaper.clampDim(Double.NaN))
    }

    fun testDimSnapsToFivePercentSteps() {
        assertEq(0.45, Wallpaper.snapDim(0.4549))
        assertEq(0.9, Wallpaper.snapDim(0.93))
        assertEq(0.0, Wallpaper.snapDim(0.02))
    }

    fun testPercentRounds() {
        assertEq(40, Wallpaper.percent(0.4))
        assertEq(90, Wallpaper.percent(5.0))
        assertEq(0, Wallpaper.percent(-1.0))
    }

    fun testImageNamesWithPathsAreRejected() {
        assertEq("", Wallpaper.cleanName("../secret.jpg"))
        assertEq("", Wallpaper.cleanName("a/b.jpg"))
        assertEq("", Wallpaper.cleanName(null))
        assertEq("wallpaper-1.jpg", Wallpaper.cleanName("wallpaper-1.jpg"))
        assertEq("wallpaper-1700000000000.jpg", Wallpaper.fileName(1_700_000_000_000))
    }

    fun testReadUsesTheSharedKeys() {
        val strings = mapOf("appearance/backgroundImage" to "wallpaper-9.jpg")
        val doubles = mapOf("appearance/backgroundDim" to 0.7)
        val wallpaper = Wallpaper.read({ strings[it] }, { doubles[it] })
        assertEq(Wallpaper("wallpaper-9.jpg", 0.7), wallpaper)
        expect(wallpaper.active)
    }

    fun testReadFallsBackWhenNothingIsStored() {
        assertEq(Wallpaper(), Wallpaper.read({ null }, { null }))
        assertEq(Wallpaper("", 0.9), Wallpaper.read({ null }, { 3.0 }))
    }

    fun testAppearanceCarriesTheWallpaper() {
        val strings = mapOf("appearance/themeMode" to "dark", "appearance/backgroundImage" to "wallpaper-2.jpg")
        val appearance = Appearance.read({ strings[it] }, { null }, { if (it == "appearance/backgroundDim") 0.2 else null })
        assertEq(ThemeModeChoice.dark, appearance.themeMode)
        assertEq(Wallpaper("wallpaper-2.jpg", 0.2), appearance.wallpaper)
        assertEq(Wallpaper(), Appearance.read { null }.wallpaper)
    }

    fun testWithersClean() {
        assertEq(Wallpaper("a.jpg", 0.4), Wallpaper().withImage("a.jpg"))
        assertEq(Wallpaper("", 0.4), Wallpaper().withImage("x/y"))
        assertEq(0.9, Wallpaper().withDim(2.0).dim)
    }

    fun testSampleSizeHalvesWhileStillCoveringTheTarget() {
        assertEq(1, Wallpaper.sampleSize(1080, 2400, 1080, 2400))
        assertEq(2, Wallpaper.sampleSize(4000, 6000, 1080, 2400))
        assertEq(4, Wallpaper.sampleSize(8000, 12000, 1080, 2400))
        assertEq(1, Wallpaper.sampleSize(0, 100, 1080, 2400))
        assertEq(1, Wallpaper.sampleSize(500, 500, 1080, 2400))
    }

    fun testFitSizeKeepsAspectAndNeverEnlarges() {
        assertEq(2800 to 1400, Wallpaper.fitSize(5600, 2800, 2800))
        assertEq(1400 to 2800, Wallpaper.fitSize(3000, 6000, 2800))
        assertEq(800 to 600, Wallpaper.fitSize(800, 600, 2800))
    }

    fun testCoverSizeShrinksToJustCoverTheScreen() {
        assertEq(1600 to 2400, Wallpaper.coverSize(4000, 6000, 1080, 2400))
        val (w, h) = Wallpaper.coverSize(6000, 4000, 1080, 2400)
        expect(w >= 1080 && h >= 2400, "covers the screen")
        expect(w < 6000 && h < 4000, "smaller than the source")
        assertEq(800 to 600, Wallpaper.coverSize(800, 600, 1080, 2400))
    }
}
