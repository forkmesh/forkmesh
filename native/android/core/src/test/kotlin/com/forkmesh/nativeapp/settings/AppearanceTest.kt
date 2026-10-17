package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.testing.assertEq

class AppearanceTest {
    fun testStoredNamesMatchEnumNames() {
        assertEq(ThemeModeChoice.dark, ThemeModeChoice.fromName("dark"))
        assertEq(ThemeModeChoice.light, ThemeModeChoice.fromName("light"))
        assertEq(ThemeModeChoice.system, ThemeModeChoice.fromName("system"))
        assertEq(AccentChoice.teal, AccentChoice.fromName("teal"))
        assertEq(AccentChoice.purple, AccentChoice.fromName("purple"))
    }

    fun testUnknownOrMissingValuesFallBackToDefaults() {
        assertEq(ThemeModeChoice.system, ThemeModeChoice.fromName(null))
        assertEq(ThemeModeChoice.system, ThemeModeChoice.fromName("sepia"))
        assertEq(AccentChoice.blue, AccentChoice.fromName(null))
        assertEq(AccentChoice.blue, AccentChoice.fromName("chartreuse"))
    }

    fun testThemeModeResolvesAgainstTheSystem() {
        assertEq(true, ThemeModeChoice.system.isDark(true))
        assertEq(false, ThemeModeChoice.system.isDark(false))
        assertEq(true, ThemeModeChoice.dark.isDark(false))
        assertEq(false, ThemeModeChoice.light.isDark(true))
    }

    fun testReadUsesTheStoredKeys() {
        val stored = mapOf("appearance/themeMode" to "dark", "appearance/accent" to "teal")
        val appearance = Appearance.read { stored[it] }
        assertEq(Appearance(ThemeModeChoice.dark, AccentChoice.teal), appearance)
        assertEq(0xFF2DD4BF, appearance.accent.accent(dark = true))
        assertEq(0xFF0D9488, appearance.accent.accent(dark = false))
    }

    fun testAccentColoursStayStable() {
        assertEq(0xFF007AFF, AccentChoice.blue.light)
        assertEq(0x33164A8CL, AccentChoice.blue.subtleOnDark)
        assertEq(0x1A6161F2L, AccentChoice.purple.accentSubtle(dark = false))
    }

    fun testEveryAccentHasALabelAndTheSameNamesAsIos() {
        assertEq(
            listOf("blue", "cyan", "teal", "green", "lime", "amber", "orange", "red", "pink", "violet", "purple", "graphite"),
            AccentChoice.entries.map { it.name },
        )
        assertEq(AccentChoice.orange, AccentChoice.fromName("orange"))
        assertEq(0xFFC2410C, AccentChoice.orange.accent(dark = false))
        assertEq(0xFFFB923C, AccentChoice.orange.accent(dark = true))
        for (accent in AccentChoice.entries) {
            assertEq(true, accent.label.isNotEmpty())
            assertEq(accent.light and 0xFFFFFF, accent.subtle and 0xFFFFFF)
        }
    }
}
