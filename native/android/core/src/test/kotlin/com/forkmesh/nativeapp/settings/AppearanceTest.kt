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

    fun testLayoutModeDefaultsToControlTowerAndReadsByName() {
        assertEq(LayoutModeChoice.controlTower, Appearance.read { null }.layoutMode)
        assertEq(LayoutModeChoice.focus, Appearance.read { if (it == "appearance/layoutMode") "focus" else null }.layoutMode)
        assertEq(LayoutModeChoice.controlTower, LayoutModeChoice.fromName("cockpit"))
        assertEq("appearance/layoutMode", AppearanceKeys.LAYOUT_MODE)
        assertEq(listOf("controlTower", "focus"), LayoutModeChoice.entries.map { it.name })
        assertEq(true, LayoutModeChoice.controlTower.dense)
        assertEq(false, LayoutModeChoice.focus.dense)
        assertEq("Control Tower", LayoutModeChoice.controlTower.label)
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
        assertEq(Appearance(ThemeModeChoice.dark, AppTheme(accent = AccentChoice.teal)), appearance)
        assertEq(0xFF2DD4BF, appearance.accent.accent(dark = true))
        assertEq(0xFF0F766E, appearance.accent.accent(dark = false))
    }

    fun testReadBuildsTheWholeTheme() {
        val stored = mapOf(
            "appearance/accent" to "custom",
            "appearance/accentCustom" to "#7c3aed",
            "appearance/surface" to "slate",
            "appearance/themeColors" to "{\"dark\":{\"bgBase\":\"#111111\"}}",
        )
        val theme = Appearance.read({ stored[it] }) { if (it == "appearance/readableColors") false else null }.theme
        assertEq(ThemeSurface.slate, theme.surface)
        assertEq(0x7C3AEDL, theme.customAccent)
        assertEq(AccentChoice.blue, theme.accent)
        assertEq("custom", theme.accentName)
        assertEq(false, theme.readable)
        assertEq(mapOf(ThemeToken.bgBase to 0x111111L), theme.darkColors)
        assertEq(AppTheme.standard, Appearance.read { null }.theme)
        assertEq(null, Appearance.read { if (it == "appearance/accentCustom") "#7c3aed" else null }.theme.customAccent)
    }

    fun testAccentColoursStayStable() {
        assertEq(0xFF0066CC, AccentChoice.blue.light)
        assertEq(0xFF409CFF, AccentChoice.blue.onDark)
        assertEq(0x33164A8CL, AccentChoice.blue.subtleOnDark)
        assertEq(0x1A5856D6L, AccentChoice.purple.accentSubtle(dark = false))
        assertEq(0xFF8B85FF, AccentChoice.purple.accent(dark = true))
    }

    fun testEveryAccentHasALabelAndTheSameNamesAsIos() {
        assertEq(
            listOf("blue", "cyan", "teal", "green", "lime", "amber", "orange", "red", "pink", "violet", "purple", "graphite"),
            AccentChoice.entries.map { it.name },
        )
        assertEq(AccentChoice.orange, AccentChoice.fromName("orange"))
        assertEq(0xFFC0400C, AccentChoice.orange.accent(dark = false))
        assertEq(0xFFFB923C, AccentChoice.orange.accent(dark = true))
        for (accent in AccentChoice.entries) {
            assertEq(true, accent.label.isNotEmpty())
            assertEq(accent.light and 0xFFFFFF, accent.subtle and 0xFFFFFF)
        }
    }
}
