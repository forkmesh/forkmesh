import FMBase
import FMNet
import XCTest
@testable import FMAccount

final class InstanceThemeTests: XCTestCase {
    static let resolved: String = """
        {"ok":true,"theme":{"preset":"blt"},"themeColors":{
        "dark":{"background":"#101820","foreground":"#f0f0f0","card":"#18202a","popover":"#1c2430","muted":"#222a36",
        "muted-foreground":"#8090a0","border":"#2a3340","primary":"#ff8800","primary-foreground":"#101010",
        "success":"#33cc66","destructive":"#ee4444","attention":"#ddbb22","link":"#66aaff"},
        "light":{"background":"#fafafa","foreground":"#111111","muted":"#eeeeee","secondary":"#dddddd","primary":"#0055aa"}}}
        """

    func testEveryDashboardTokenLandsOnItsAppToken() throws {
        let theme = try XCTUnwrap(AppTheme.instance(themeColors: InstanceTheme.themeColors(in: JSON.parseObject(Self.resolved))))
        let dark = theme.darkColors
        XCTAssertEqual(dark[.bgBase], 0x101820)
        XCTAssertEqual(dark[.bgRaised], 0x18202A)
        XCTAssertEqual(dark[.bgOverlay], 0x1C2430)
        XCTAssertEqual(dark[.bgElevated], 0x222A36)
        XCTAssertEqual(dark[.border], 0x2A3340)
        XCTAssertEqual(dark[.textPrimary], 0xF0F0F0)
        XCTAssertEqual(dark[.textSecondary], ThemeColor.mix(0x8090A0, 0xF0F0F0, amount: 0.35))
        XCTAssertEqual(dark[.textTertiary], 0x8090A0)
        XCTAssertEqual(dark[.accent], 0xFF8800)
        XCTAssertEqual(dark[.onAccent], 0x101010)
        XCTAssertEqual(dark[.success], 0x33CC66)
        XCTAssertEqual(dark[.danger], 0xEE4444)
        XCTAssertEqual(dark[.warning], 0xDDBB22)
        XCTAssertEqual(dark.count, ThemeToken.allCases.count)
    }

    func testOnlyThePresentTokensAreSetAndTheFallbacksFillGaps() throws {
        let theme = try XCTUnwrap(AppTheme.instance(themeColors: InstanceTheme.themeColors(in: JSON.parseObject(Self.resolved))))
        XCTAssertEqual(
            theme.lightColors,
            [.bgBase: 0xFAFAFA, .textPrimary: 0x111111, .bgOverlay: 0xEEEEEE, .bgElevated: 0xEEEEEE, .accent: 0x0055AA])
        let secondary = AppTheme.instanceColors(["secondary": 0x222222, "secondary-foreground": 0xCCCCCC])
        XCTAssertEqual(secondary, [.bgElevated: 0x222222, .textSecondary: 0xCCCCCC])
        XCTAssertEqual(theme.resolve(dark: false).rgb(.bgRaised), AppTheme.standard.resolve(dark: false).rgb(.bgRaised))
    }

    func testTheDefaultLookAndJunkGiveNoTheme() {
        XCTAssertNil(AppTheme.instance(themeColors: [:]))
        XCTAssertNil(AppTheme.instance(themeColors: JSON.parseObject(##"{"dark":{"link":"#112233","primary":"red"}}"##)))
        XCTAssertEqual(InstanceTheme.colorsJSON(in: JSON.parseObject(##"{"ok":true,"theme":{},"themeColors":{}}"##)), "")
    }

    func testAnOlderServerFallsBackToTheStoredOverrides() {
        let body = JSON.parseObject(##"{"ok":true,"theme":{"preset":"blt","dark":{"primary":"#123456"}}}"##)
        XCTAssertEqual(InstanceTheme.colorsJSON(in: body), ##"{"dark":{"accent":"#123456"}}"##)
        let resolvedWins = JSON.parseObject(
            ##"{"theme":{"dark":{"primary":"#123456"}},"themeColors":{"dark":{"primary":"#654321"}}}"##)
        XCTAssertEqual(InstanceTheme.colorsJSON(in: resolvedWins), ##"{"dark":{"accent":"#654321"}}"##)
    }

    func testFetchReadsTheInstanceDocumentAndReportsFailuresAsNil() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: try! JSON.parse(Self.resolved))
        let colors = await InstanceTheme.fetch(http: http, origin: "https://relay.example.com")
        XCTAssertEqual(http.lastRequest?.url.absoluteString, "https://relay.example.com/api/instance")
        XCTAssertEqual(AppTheme.parseColors(colors).dark[.accent], 0xFF8800)

        http.enqueue(status: 200, json: .object(JSON.parseObject(##"{"ok":true,"theme":{},"themeColors":{}}"##)))
        let plain = await InstanceTheme.fetch(http: http, origin: "https://relay.example.com")
        XCTAssertEqual(plain, "")

        http.enqueue(status: 503, text: "down")
        let down = await InstanceTheme.fetch(http: http, origin: "https://relay.example.com")
        XCTAssertNil(down)
        let none = await InstanceTheme.fetch(http: http, origin: "")
        XCTAssertNil(none)
    }

    @MainActor
    func testTheEffectiveThemeIsTheInstanceOnlyWhileMatchingAndColoursAreStored() async {
        let h = Harness()
        var own = AppTheme(surface: .stone, accent: .teal)
        own.readable = false
        h.settings.setTheme(own)
        XCTAssertFalse(h.settings.matchesInstanceTheme)
        let json = InstanceTheme.colorsJSON(in: JSON.parseObject(Self.resolved))
        h.settings.setInstanceThemeColors(json)
        XCTAssertEqual(h.settings.effectiveTheme, own)

        h.settings.setMatchesInstanceTheme(true)
        XCTAssertEqual(h.prefs.bool(forKey: SettingsService.matchInstanceThemeKey), true)
        let instance = h.settings.effectiveTheme
        XCTAssertEqual(instance.surface, .fallback)
        XCTAssertFalse(instance.readable)
        XCTAssertEqual(instance.darkColors[.accent], 0xFF8800)
        XCTAssertEqual(instance.lightColors[.accent], 0x0055AA)
        XCTAssertEqual(h.settings.theme, own)

        h.settings.setInstanceThemeColors("")
        XCTAssertNil(h.prefs.string(forKey: SettingsService.instanceThemeColorsKey))
        XCTAssertEqual(h.settings.effectiveTheme, own)

        h.settings.setInstanceThemeColors(json)
        h.settings.setMatchesInstanceTheme(false)
        XCTAssertNil(h.prefs.bool(forKey: SettingsService.matchInstanceThemeKey))
        XCTAssertEqual(h.settings.instanceThemeColors, "")
        XCTAssertEqual(h.settings.effectiveTheme, own)
    }
}
