package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class InstanceThemeTest {
    private val resolved = """
        {"ok":true,"theme":{"preset":"blt"},"themeColors":{
        "dark":{"background":"#101820","foreground":"#f0f0f0","card":"#18202a","popover":"#1c2430","muted":"#222a36",
        "muted-foreground":"#8090a0","border":"#2a3340","primary":"#ff8800","primary-foreground":"#101010",
        "success":"#33cc66","destructive":"#ee4444","attention":"#ddbb22","link":"#66aaff"},
        "light":{"background":"#fafafa","foreground":"#111111","muted":"#eeeeee","secondary":"#dddddd","primary":"#0055aa"}}}
    """.trimIndent()

    private fun body(text: String): Map<*, *> = Json.parse(text) as Map<*, *>

    fun testEveryDashboardTokenLandsOnItsAppToken() {
        val theme = InstanceTheme.theme(InstanceTheme.themeColors(body(resolved)))!!
        val dark = theme.darkColors
        assertEq(0x101820L, dark[ThemeToken.bgBase])
        assertEq(0x18202AL, dark[ThemeToken.bgRaised])
        assertEq(0x1C2430L, dark[ThemeToken.bgOverlay])
        assertEq(0x222A36L, dark[ThemeToken.bgElevated])
        assertEq(0x2A3340L, dark[ThemeToken.border])
        assertEq(0xF0F0F0L, dark[ThemeToken.textPrimary])
        assertEq(ThemeColor.mix(0x8090A0, 0xF0F0F0, 0.35), dark[ThemeToken.textSecondary])
        assertEq(0x8090A0L, dark[ThemeToken.textTertiary])
        assertEq(0xFF8800L, dark[ThemeToken.accent])
        assertEq(0x101010L, dark[ThemeToken.onAccent])
        assertEq(0x33CC66L, dark[ThemeToken.success])
        assertEq(0xEE4444L, dark[ThemeToken.danger])
        assertEq(0xDDBB22L, dark[ThemeToken.warning])
        assertEq(ThemeToken.entries.size, dark.size)
    }

    fun testOnlyThePresentTokensAreSetAndTheFallbacksFillGaps() {
        val theme = InstanceTheme.theme(InstanceTheme.themeColors(body(resolved)))!!
        assertEq(
            mapOf(
                ThemeToken.bgBase to 0xFAFAFAL,
                ThemeToken.bgOverlay to 0xEEEEEEL,
                ThemeToken.bgElevated to 0xEEEEEEL,
                ThemeToken.textPrimary to 0x111111L,
                ThemeToken.accent to 0x0055AAL,
            ),
            theme.lightColors,
        )
        assertEq(
            mapOf(ThemeToken.bgElevated to 0x222222L, ThemeToken.textSecondary to 0xCCCCCCL),
            InstanceTheme.instanceColors(mapOf("secondary" to 0x222222L, "secondary-foreground" to 0xCCCCCCL)),
        )
        assertEq(AppTheme.standard.resolve(false).rgb(ThemeToken.bgRaised), theme.resolve(false).rgb(ThemeToken.bgRaised))
    }

    fun testTheDefaultLookAndJunkGiveNoTheme() {
        expect(InstanceTheme.theme(emptyMap<String, Any?>()) == null)
        expect(InstanceTheme.theme(body("""{"dark":{"link":"#112233","primary":"red"}}""")) == null)
        assertEq("", InstanceTheme.colorsJson(body("""{"ok":true,"theme":{},"themeColors":{}}""")))
    }

    fun testAnOlderServerFallsBackToTheStoredOverrides() {
        assertEq("""{"dark":{"accent":"#123456"}}""", InstanceTheme.colorsJson(body("""{"ok":true,"theme":{"preset":"blt","dark":{"primary":"#123456"}}}""")))
        val resolvedWins = body("""{"theme":{"dark":{"primary":"#123456"}},"themeColors":{"dark":{"primary":"#654321"}}}""")
        assertEq("""{"dark":{"accent":"#654321"}}""", InstanceTheme.colorsJson(resolvedWins))
    }

    fun testFetchReadsTheInstanceDocumentAndReportsFailuresAsNil() {
        var reply = HttpClient.Response(200, emptyMap(), resolved.toByteArray())
        val asked = ArrayList<String>()
        val transport = HttpTransport { request ->
            asked.add(request.url)
            reply
        }
        val colors = InstanceTheme.fetch(transport, "https://relay.example.com")
        assertEq("https://relay.example.com/api/instance", asked.last())
        assertEq(0xFF8800L, AppTheme.parseColors(colors).dark[ThemeToken.accent])
        reply = HttpClient.Response(200, emptyMap(), """{"ok":true,"theme":{},"themeColors":{}}""".toByteArray())
        assertEq("", InstanceTheme.fetch(transport, "https://relay.example.com"))
        reply = HttpClient.Response(503, emptyMap(), "down".toByteArray())
        assertEq(null, InstanceTheme.fetch(transport, "https://relay.example.com"))
        assertEq(null, InstanceTheme.fetch(transport, ""))
    }

    fun testTheEffectiveThemeIsTheInstanceOnlyWhileMatching() {
        val own = AppTheme(surface = ThemeSurface.stone, accent = AccentChoice.entries.last(), readable = false)
        val json = InstanceTheme.colorsJson(body(resolved))
        assertEq(own, InstanceTheme.effective(own, false, json))
        val instance = InstanceTheme.effective(own, true, json)
        assertEq(ThemeSurface.fallback, instance.surface)
        expect(!instance.readable)
        assertEq(0xFF8800L, instance.darkColors[ThemeToken.accent])
        assertEq(0x0055AAL, instance.lightColors[ThemeToken.accent])
        assertEq(own, InstanceTheme.effective(own, true, ""))
    }

    fun testTheMatchSwitchAndColoursAreReadWithTheAppearance() {
        val prefs = mapOf(AppearanceKeys.MATCH_INSTANCE_THEME to "true", AppearanceKeys.INSTANCE_THEME_COLORS to """{"dark":{"accent":"#ff8800"}}""")
        val appearance = Appearance.read { prefs[it] }
        expect(appearance.matchInstanceTheme)
        assertEq(0xFF8800L, appearance.effectiveTheme.darkColors[ThemeToken.accent])
        assertEq(AppTheme.standard, Appearance.read { null }.effectiveTheme)
    }
}
