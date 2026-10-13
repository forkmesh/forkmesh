package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.testing.assertEq

class AppearanceTest {
    fun testStoredNamesMatchFlutterEnumNames() {
        assertEq(ThemeModeChoice.dark, ThemeModeChoice.fromName("dark"))
        assertEq(ThemeModeChoice.light, ThemeModeChoice.fromName("light"))
        assertEq(ThemeModeChoice.system, ThemeModeChoice.fromName("system"))
        assertEq(AccentChoice.teal, AccentChoice.fromName("teal"))
        assertEq(AccentChoice.purple, AccentChoice.fromName("purple"))
    }

    fun testUnknownOrMissingValuesFallBackLikeFlutter() {
        assertEq(ThemeModeChoice.system, ThemeModeChoice.fromName(null))
        assertEq(ThemeModeChoice.system, ThemeModeChoice.fromName("sepia"))
        assertEq(AccentChoice.blue, AccentChoice.fromName(null))
        assertEq(AccentChoice.blue, AccentChoice.fromName("orange"))
    }

    fun testThemeModeResolvesAgainstTheSystem() {
        assertEq(true, ThemeModeChoice.system.isDark(true))
        assertEq(false, ThemeModeChoice.system.isDark(false))
        assertEq(true, ThemeModeChoice.dark.isDark(false))
        assertEq(false, ThemeModeChoice.light.isDark(true))
    }

    fun testReadUsesTheMigratedFlutterKeys() {
        val stored = mapOf("appearance/themeMode" to "dark", "appearance/accent" to "teal")
        val appearance = Appearance.read { stored[it] }
        assertEq(Appearance(ThemeModeChoice.dark, AccentChoice.teal), appearance)
        assertEq(0xFF2DD4BF, appearance.accent.accent(dark = true))
        assertEq(0xFF0D9488, appearance.accent.accent(dark = false))
    }

    fun testAccentColoursMatchFlutter() {
        assertEq(0xFF007AFF, AccentChoice.blue.light)
        assertEq(0x33164A8CL, AccentChoice.blue.subtleOnDark)
        assertEq(0x1A6161F2L, AccentChoice.purple.accentSubtle(dark = false))
    }
}
