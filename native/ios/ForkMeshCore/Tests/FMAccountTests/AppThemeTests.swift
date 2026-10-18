import FMBase
import XCTest
@testable import FMAccount

final class AppThemeTests: XCTestCase {
    func testEveryAccentClearsAAOnEverySurfaceFamilyInBothModes() {
        for accent in AccentChoice.allCases {
            for surface in ThemeSurface.allCases {
                for dark in [false, true] {
                    let resolved = AppTheme(surface: surface, accent: accent).resolve(dark: dark)
                    let surfaces = [resolved.rgb(.bgBase), resolved.rgb(.bgRaised), resolved.rgb(.bgOverlay)]
                    for token in [ThemeToken.accent, .textPrimary, .success, .danger, .warning] {
                        XCTAssertTrue(
                            ThemeColor.passes(resolved.rgb(token), on: surfaces),
                            "\(accent) \(token) on \(surface) dark=\(dark)")
                    }
                    XCTAssertGreaterThanOrEqual(
                        ThemeColor.contrast(resolved.rgb(.accent), resolved.rgb(.onAccent)), 4.5,
                        "\(accent) label on \(surface) dark=\(dark)")
                }
            }
        }
    }

    func testTheCuratedAccentsNeedNoAdjustmentOnTheForkMeshSurfaces() {
        for accent in AccentChoice.allCases {
            XCTAssertEqual(AppTheme(accent: accent).resolve(dark: false).rgb(.accent), accent.rgb.light, "\(accent)")
            XCTAssertEqual(AppTheme(accent: accent).resolve(dark: true).rgb(.accent), accent.rgb.dark, "\(accent)")
        }
        XCTAssertEqual(AppTheme.standard.resolve(dark: false).rgb(.accent), 0x0066CC)
        XCTAssertEqual(AppTheme.standard.resolve(dark: true).rgb(.accent), 0x409CFF)
    }

    func testAPaleCustomAccentIsDarkenedInLightModeAndADeepOneLightenedInDarkMode() {
        let pale = AppTheme(customAccent: 0xFFE066)
        let light = pale.resolve(dark: false)
        XCTAssertNotEqual(light.rgb(.accent), 0xFFE066)
        XCTAssertGreaterThanOrEqual(ThemeColor.contrast(light.rgb(.accent), 0xF0F0F0), 4.5)
        XCTAssertLessThan(abs(ThemeColor.hsl(light.rgb(.accent)).hue - ThemeColor.hsl(0xFFE066).hue), 4)

        let deep = AppTheme(customAccent: 0x1A0A6B).resolve(dark: true)
        XCTAssertGreaterThanOrEqual(ThemeColor.contrast(deep.rgb(.accent), 0x202022), 4.5)

        var raw = pale
        raw.readable = false
        XCTAssertEqual(raw.resolve(dark: false).rgb(.accent), 0xFFE066)
    }

    func testEditsWinOverTheSurfaceFamilyAndTheAccent() {
        var theme = AppTheme(surface: .stone)
        theme.setColor(0x123456, for: .bgBase, dark: true)
        theme.setColor(0x7FDBFF, for: .accent, dark: true)
        theme.setColor(0x00FF00, for: .textSecondary, dark: false)
        let dark = theme.resolve(dark: true)
        XCTAssertEqual(dark.rgb(.bgBase), 0x123456)
        XCTAssertEqual(dark.rgb(.bgRaised), 0x1C1917)
        XCTAssertEqual(dark.rgb(.accent), 0x7FDBFF)
        XCTAssertEqual(theme.resolve(dark: false)[.textSecondary], 0xFF00FF00)
        XCTAssertEqual(theme.resolve(dark: true)[.textSecondary] >> 24, 0xB3)

        theme.choose(accent: .pink)
        XCTAssertNil(theme.darkColors[.accent])
        XCTAssertEqual(theme.darkColors[.bgBase], 0x123456)
        theme.choose(surface: .zinc)
        XCTAssertTrue(theme.darkColors.isEmpty)
        XCTAssertEqual(theme.lightColors.count, 0)
    }

    func testColorsRoundTripThroughJSONAndBadEntriesAreDropped() {
        var theme = AppTheme()
        XCTAssertEqual(theme.colorsJSON(), "")
        theme.setColor(0xABCDEF, for: .border, dark: false)
        theme.setColor(0x010203, for: .warning, dark: true)
        let json = theme.colorsJSON()
        XCTAssertEqual(json, ##"{"dark":{"warning":"#010203"},"light":{"border":"#abcdef"}}"##)
        let parsed = AppTheme.parseColors(json)
        XCTAssertEqual(parsed.light, [.border: 0xABCDEF])
        XCTAssertEqual(parsed.dark, [.warning: 0x010203])
        let messy = AppTheme.parseColors(##"{"light":{"border":"blue","nope":"#000000","accent":"#00ff00"},"sepia":{}}"##)
        XCTAssertEqual(messy.light, [.accent: 0x00FF00])
        XCTAssertTrue(AppTheme.parseColors("not json").light.isEmpty)
    }

    func testHexAndHslHelpers() {
        XCTAssertEqual(ThemeColor.parseHex("#0066cc"), 0x0066CC)
        XCTAssertEqual(ThemeColor.parseHex(" 0066CC "), 0x0066CC)
        XCTAssertNil(ThemeColor.parseHex("#06c"))
        XCTAssertNil(ThemeColor.parseHex("#00GG00"))
        XCTAssertEqual(ThemeColor.hex(0xFF0066CC), "#0066cc")
        for rgb: UInt32 in [0x0066CC, 0xFB923C, 0x52525B, 0xFFFFFF, 0x000000] {
            let hsl = ThemeColor.hsl(rgb)
            XCTAssertEqual(ThemeColor.fromHsl(hue: hsl.hue, saturation: hsl.saturation, lightness: hsl.lightness), rgb)
        }
        XCTAssertEqual(ThemeColor.contrast(0xFFFFFF, 0x000000), 21, accuracy: 0.01)
        XCTAssertEqual(ThemeColor.label(on: 0xFBBF24, preferred: 0xFFFFFF), 0x0A0A0A)
    }

    func testShuffleAlwaysGivesAReadableCustomAccent() {
        var generator = SystemRandomNumberGenerator()
        for _ in 0..<40 {
            let theme = AppTheme.shuffled(using: &generator)
            XCTAssertNotNil(theme.customAccent)
            XCTAssertTrue(theme.readable)
            for dark in [false, true] {
                let resolved = theme.resolve(dark: dark)
                XCTAssertTrue(
                    ThemeColor.passes(
                        resolved.rgb(.accent), on: [resolved.rgb(.bgBase), resolved.rgb(.bgRaised), resolved.rgb(.bgOverlay)]))
            }
        }
    }

    @MainActor
    func testTheThemePersistsAndAPresetAccentClearsTheCustomOne() async {
        let h = Harness()
        XCTAssertEqual(h.settings.theme, AppTheme.standard)
        var theme = AppTheme(surface: .slate, customAccent: 0x7C3AED, readable: false)
        theme.setColor(0x111111, for: .bgBase, dark: true)
        h.settings.setTheme(theme)
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.accentKey), "custom")
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.accentCustomKey), "#7c3aed")
        XCTAssertEqual(h.prefs.string(forKey: SettingsService.surfaceKey), "slate")
        XCTAssertEqual(h.prefs.bool(forKey: SettingsService.readableColorsKey), false)
        XCTAssertEqual(h.settings.theme, theme)
        XCTAssertEqual(h.settings.accent, .blue)

        h.settings.setAccent(.teal)
        XCTAssertNil(h.prefs.string(forKey: SettingsService.accentCustomKey))
        XCTAssertNil(h.settings.theme.customAccent)
        XCTAssertEqual(h.settings.theme.accent, .teal)

        h.settings.setTheme(.standard)
        XCTAssertNil(h.prefs.string(forKey: SettingsService.surfaceKey))
        XCTAssertNil(h.prefs.string(forKey: SettingsService.themeColorsKey))
        XCTAssertNil(h.prefs.bool(forKey: SettingsService.readableColorsKey))
        XCTAssertEqual(h.settings.theme, AppTheme.standard)
    }
}
