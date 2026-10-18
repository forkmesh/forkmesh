package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs
import kotlin.random.Random

class AppThemeTest {
    private fun surfacesOf(resolved: ResolvedTheme): List<Long> =
        listOf(resolved.rgb(ThemeToken.bgBase), resolved.rgb(ThemeToken.bgRaised), resolved.rgb(ThemeToken.bgOverlay))

    fun testEveryAccentClearsAAOnEverySurfaceFamilyInBothModes() {
        for (accent in AccentChoice.entries) {
            for (surface in ThemeSurface.entries) {
                for (dark in listOf(false, true)) {
                    val resolved = AppTheme(surface = surface, accent = accent).resolve(dark)
                    val surfaces = surfacesOf(resolved)
                    for (token in listOf(ThemeToken.accent, ThemeToken.textPrimary, ThemeToken.success, ThemeToken.danger, ThemeToken.warning)) {
                        expect(ThemeColor.passes(resolved.rgb(token), surfaces), "$accent $token on $surface dark=$dark")
                    }
                    expect(
                        ThemeColor.contrast(resolved.rgb(ThemeToken.accent), resolved.rgb(ThemeToken.onAccent)) >= 4.5,
                        "$accent label on $surface dark=$dark",
                    )
                }
            }
        }
    }

    fun testTheCuratedAccentsNeedNoAdjustmentOnTheForkMeshSurfaces() {
        for (accent in AccentChoice.entries) {
            assertEq(accent.light and 0xFFFFFF, AppTheme(accent = accent).resolve(false).rgb(ThemeToken.accent), "$accent")
            assertEq(accent.onDark and 0xFFFFFF, AppTheme(accent = accent).resolve(true).rgb(ThemeToken.accent), "$accent")
        }
        assertEq(0x0066CCL, AppTheme.standard.resolve(false).rgb(ThemeToken.accent))
        assertEq(0x409CFFL, AppTheme.standard.resolve(true).rgb(ThemeToken.accent))
    }

    fun testAPaleCustomAccentIsDarkenedInLightModeAndADeepOneLightenedInDarkMode() {
        val pale = AppTheme(customAccent = 0xFFE066)
        val light = pale.resolve(false)
        expect(light.rgb(ThemeToken.accent) != 0xFFE066L)
        expect(ThemeColor.contrast(light.rgb(ThemeToken.accent), 0xF0F0F0) >= 4.5)
        expect(abs(ThemeColor.hsl(light.rgb(ThemeToken.accent)).hue - ThemeColor.hsl(0xFFE066).hue) < 4)

        val deep = AppTheme(customAccent = 0x1A0A6B).resolve(true)
        expect(ThemeColor.contrast(deep.rgb(ThemeToken.accent), 0x202022) >= 4.5)

        val raw = pale.copy(readable = false)
        assertEq(0xFFE066L, raw.resolve(false).rgb(ThemeToken.accent))
    }

    fun testEditsWinOverTheSurfaceFamilyAndTheAccent() {
        var theme = AppTheme(surface = ThemeSurface.stone)
            .setColor(0x123456, ThemeToken.bgBase, dark = true)
            .setColor(0x7FDBFF, ThemeToken.accent, dark = true)
            .setColor(0x00FF00, ThemeToken.textSecondary, dark = false)
        val dark = theme.resolve(true)
        assertEq(0x123456L, dark.rgb(ThemeToken.bgBase))
        assertEq(0x1C1917L, dark.rgb(ThemeToken.bgRaised))
        assertEq(0x7FDBFFL, dark.rgb(ThemeToken.accent))
        assertEq(0xFF00FF00, theme.resolve(false)[ThemeToken.textSecondary])
        assertEq(0xB3L, theme.resolve(true)[ThemeToken.textSecondary] shr 24)

        theme = theme.chooseAccent(AccentChoice.pink)
        assertEq(null, theme.darkColors[ThemeToken.accent])
        assertEq(0x123456L, theme.darkColors[ThemeToken.bgBase])
        theme = theme.chooseSurface(ThemeSurface.zinc)
        assertEq(true, theme.darkColors.isEmpty())
        assertEq(0, theme.lightColors.size)
        assertEq(null, theme.setColor(0x010101, ThemeToken.danger, dark = false).setColor(null, ThemeToken.danger, dark = false).lightColors[ThemeToken.danger])
    }

    fun testColorsRoundTripThroughJsonAndBadEntriesAreDropped() {
        var theme = AppTheme()
        assertEq("", theme.colorsJson())
        theme = theme.setColor(0xABCDEF, ThemeToken.border, dark = false).setColor(0x010203, ThemeToken.warning, dark = true)
        val json = theme.colorsJson()
        assertEq("{\"dark\":{\"warning\":\"#010203\"},\"light\":{\"border\":\"#abcdef\"}}", json)
        val parsed = AppTheme.parseColors(json)
        assertEq(mapOf(ThemeToken.border to 0xABCDEFL), parsed.light)
        assertEq(mapOf(ThemeToken.warning to 0x010203L), parsed.dark)
        val messy = AppTheme.parseColors("{\"light\":{\"border\":\"blue\",\"nope\":\"#000000\",\"accent\":\"#00ff00\"},\"sepia\":{}}")
        assertEq(mapOf(ThemeToken.accent to 0x00FF00L), messy.light)
        assertEq(true, AppTheme.parseColors("not json").light.isEmpty())
        assertEq(true, AppTheme.parseColors(null).dark.isEmpty())
    }

    fun testHexAndHslHelpers() {
        assertEq(0x0066CCL, ThemeColor.parseHex("#0066cc"))
        assertEq(0x0066CCL, ThemeColor.parseHex(" 0066CC "))
        assertEq(null, ThemeColor.parseHex("#06c"))
        assertEq(null, ThemeColor.parseHex("#00GG00"))
        assertEq("#0066cc", ThemeColor.hex(0xFF0066CC))
        for (rgb in listOf(0x0066CCL, 0xFB923CL, 0x52525BL, 0xFFFFFFL, 0x000000L)) {
            assertEq(rgb, ThemeColor.fromHsl(ThemeColor.hsl(rgb)), ThemeColor.hex(rgb))
        }
        expect(abs(ThemeColor.contrast(0xFFFFFF, 0x000000) - 21) < 0.01)
        assertEq(0x0A0A0AL, ThemeColor.label(0xFBBF24, 0xFFFFFF))
    }

    fun testShuffleAlwaysGivesAReadableCustomAccent() {
        val random = Random(2263)
        repeat(40) {
            val theme = AppTheme.shuffled(random)
            expect(theme.customAccent != null)
            expect(theme.readable)
            for (dark in listOf(false, true)) {
                val resolved = theme.resolve(dark)
                expect(ThemeColor.passes(resolved.rgb(ThemeToken.accent), surfacesOf(resolved)), "${theme.customAccent} dark=$dark")
            }
        }
    }

    fun testSubtleWashFollowsThePresetOrTheAccent() {
        assertEq(0x1A0066CCL, AppTheme.standard.resolve(false).accentSubtle)
        assertEq(0x33164A8CL, AppTheme.standard.resolve(true).accentSubtle)
        val custom = AppTheme(customAccent = 0x22D3EE).resolve(true)
        assertEq((0x33L shl 24) or ThemeColor.mix(custom.rgb(ThemeToken.accent), 0x000000, 0.45), custom.accentSubtle)
        assertEq(0xFF0B0B0C, AppTheme.standard.resolve(true)[ThemeToken.onAccent])
        assertEq(0xFFFFFFFF, AppTheme.standard.resolve(false)[ThemeToken.onAccent])
        assertEq(0x4DF4F4F5L, AppTheme.standard.resolve(true).textDisabled)
    }

    fun testTokenGroupsMatchTheEditor() {
        assertEq(13, ThemeToken.entries.size)
        assertEq(listOf("bgBase", "bgRaised", "bgOverlay", "bgElevated", "border"), ThemeTokenGroup.surfaces.tokens.map { it.name })
        assertEq(listOf("accent", "onAccent"), ThemeTokenGroup.accent.tokens.map { it.name })
        assertEq(listOf("forkmesh", "slate", "zinc", "stone", "pure"), ThemeSurface.entries.map { it.name })
        assertEq(ThemeSurface.forkmesh, ThemeSurface.fromName("sepia"))
    }
}
