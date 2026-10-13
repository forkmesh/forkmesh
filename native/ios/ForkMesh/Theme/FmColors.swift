// FmColors (the raw palette), FmAccent, and FmTheme's brightness-resolved
// colours as FmPalette.

import SwiftUI
import UIKit

extension Color {
    /// A colour from a packed `0xAARRGGBB` value.
    init(fmARGB argb: UInt32) {
        let alpha: Double = Double((argb >> 24) & 0xFF) / 255.0
        let red: Double = Double((argb >> 16) & 0xFF) / 255.0
        let green: Double = Double((argb >> 8) & 0xFF) / 255.0
        let blue: Double = Double(argb & 0xFF) / 255.0
        self.init(.sRGB, red: red, green: green, blue: blue, opacity: alpha)
    }

    /// "#RRGGBB", "RRGGBB", "#AARRGGBB" or "#RGB"; nil for anything else.
    /// For colours that arrive as text (issue labels, repository languages).
    static func fmHex(_ text: String) -> Color? {
        var digits: String = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if digits.hasPrefix("#") {
            digits.removeFirst()
        }
        if digits.count == 3 {
            var doubled: String = ""
            for digit in digits {
                doubled.append(digit)
                doubled.append(digit)
            }
            digits = doubled
        }
        guard digits.count == 6 || digits.count == 8 else {
            return nil
        }
        guard let value = UInt32(digits, radix: 16) else {
            return nil
        }
        let argb: UInt32 = digits.count == 6 ? (0xFF000000 | value) : value
        return Color(fmARGB: argb)
    }

    /// The colour at alpha n, n in 0...255. The palette colours these
    /// are applied to are opaque, so scaling the opacity gives the same result.
    func fmWithAlpha(_ alpha: Int) -> Color {
        return self.opacity(Double(alpha) / 255.0)
    }

    /// A colour that follows the trait collection rather than the ForkMesh
    /// environment: for the few places no SwiftUI environment reaches (UIKit
    /// appearance proxies, a launch background). Views read `fmPalette`.
    static func fmDynamic(light: Color, dark: Color) -> Color {
        let provider: (UITraitCollection) -> UIColor = { traits in
            if traits.userInterfaceStyle == .dark {
                return UIColor(dark)
            }
            return UIColor(light)
        }
        return Color(uiColor: UIColor(dynamicProvider: provider))
    }
}

/// FmColors: the raw palette. Widgets read the brightness-resolved FmPalette
/// from the environment; these constants are for the places that use them
/// directly (the presence dot, the sender names).
enum FmColors {
    static let bgBase = Color(fmARGB: 0xFFF7F7F7)
    static let bgRaised = Color(fmARGB: 0xFFFFFFFF)
    static let bgOverlay = Color(fmARGB: 0xFFF0F0F0)
    static let bgElevated = Color(fmARGB: 0xFFE8E8E8)

    static let darkBgBase = Color(fmARGB: 0xFF0B0B0C)
    static let darkBgRaised = Color(fmARGB: 0xFF141414)
    static let darkBgOverlay = Color(fmARGB: 0xFF202022)
    static let darkBgElevated = Color(fmARGB: 0xFF26262A)

    static let textPrimary = Color(fmARGB: 0xFF0A0A0A)
    static let textSecondary = Color(fmARGB: 0x990A0A0A)
    static let textTertiary = Color(fmARGB: 0x660A0A0A)
    static let textDisabled = Color(fmARGB: 0x400A0A0A)

    static let darkTextPrimary = Color(fmARGB: 0xFFF4F4F5)
    static let darkTextSecondary = Color(fmARGB: 0xB3F4F4F5)
    static let darkTextTertiary = Color(fmARGB: 0x80F4F4F5)
    static let darkTextDisabled = Color(fmARGB: 0x4DF4F4F5)

    // The default accent (FmAccent.blue). The user's choice travels through
    // the environment; read it as `fmPalette.accent`, not these constants.
    static let accent = Color(fmARGB: 0xFF007AFF)
    static let accentSubtle = Color(fmARGB: 0x1A007AFF)
    static let accentOnDark = Color(fmARGB: 0xFF0A84FF)
    static let accentSubtleOnDark = Color(fmARGB: 0x33164A8C)
    static let success = Color(fmARGB: 0xFF148A45)
    static let successOnDark = Color(fmARGB: 0xFF46E07F)
    static let successBtn = FmColors.textPrimary
    static let successBtnHover = Color(fmARGB: 0xFF2A2E36)
    static let danger = Color(fmARGB: 0xFFC93532)
    static let dangerOnDark = Color(fmARGB: 0xFFFF6B6B)
    static let warning = Color(fmARGB: 0xFFB7791F)
    static let warningOnDark = Color(fmARGB: 0xFFE5C84C)
    static let offline = Color(fmARGB: 0xFFA0A7B2)
    static let accentEdge = Color(fmARGB: 0xFF8250DF)
    static let darkBorder = Color(fmARGB: 0xFF252529)
    static let editorGutter = Color(fmARGB: 0xFF101012)
    static let editorCurrentLine = Color(fmARGB: 0xFF202024)
    static let codeKeyword = Color(fmARGB: 0xFFC792EA)
    static let codeType = Color(fmARGB: 0xFFFF6E73)
    static let codeString = Color(fmARGB: 0xFFC3E88D)
    static let codeComment = Color(fmARGB: 0xFF777A82)
    static let codeNumber = Color(fmARGB: 0xFFF78C6C)
    static let codeFunction = Color(fmARGB: 0xFF82AAFF)

    // Light-canvas code palette: the same hue roles, darkened so they keep
    // contrast on a white editor surface.
    static let codeKeywordLight = Color(fmARGB: 0xFF7C3AED)
    static let codeTypeLight = Color(fmARGB: 0xFFC2410C)
    static let codeStringLight = Color(fmARGB: 0xFF15803D)
    static let codeCommentLight = Color(fmARGB: 0xFF6B7280)
    static let codeNumberLight = Color(fmARGB: 0xFFB45309)
    static let codeFunctionLight = Color(fmARGB: 0xFF1D4ED8)

    static let canvas = FmColors.bgBase
    static let surface = FmColors.bgRaised
    static let rail = FmColors.bgRaised
    static let border = Color(fmARGB: 0xFFDADADA)
    static let borderMuted = FmColors.bgOverlay
    static let text = FmColors.textPrimary
    static let textMuted = FmColors.textSecondary

    /// Sender name palette (hashed per user), mirrors kSenderPalette.
    static let senders: [Color] = [
        Color(fmARGB: 0xFFF85149),
        Color(fmARGB: 0xFFE3B341),
        Color(fmARGB: 0xFF3FB950),
        Color(fmARGB: 0xFF58A6FF),
        Color(fmARGB: 0xFFBC8CFF),
        Color(fmARGB: 0xFFDB61A2),
        Color(fmARGB: 0xFF39C5CF),
        Color(fmARGB: 0xFFDB6D28),
    ]

    /// Index into `senders`: the same hash over UTF-16 code units as the other
    /// clients, so one name gets one colour on every client.
    static func senderIndex(_ name: String) -> Int {
        var hash: Int = 0
        for unit in name.utf16 {
            hash = (hash * 31 + Int(unit)) & 0x7fffffff
        }
        return hash % senders.count
    }

    static func senderColor(_ name: String) -> Color {
        return senders[senderIndex(name)]
    }
}

/// The user-selectable accent colours offered in Settings. The raw value is
/// the stored name. The dark variant is brighter, so
/// it keeps contrast on the near-black canvas. Blue is the default.
enum FmAccent: String, CaseIterable {
    case blue
    case purple
    case teal

    static let fallback: FmAccent = .blue

    /// Resolves a stored name, falling back for anything unknown (a value
    /// written by a newer build, or nothing stored yet).
    static func fromName(_ name: String?) -> FmAccent {
        guard let name = name, let accent = FmAccent(rawValue: name) else {
            return fallback
        }
        return accent
    }

    var label: String {
        switch self {
        case .blue: return "Blue"
        case .purple: return "Purple"
        case .teal: return "Teal"
        }
    }

    var light: Color {
        switch self {
        case .blue: return Color(fmARGB: 0xFF007AFF)
        case .purple: return Color(fmARGB: 0xFF6161F2)
        case .teal: return Color(fmARGB: 0xFF0D9488)
        }
    }

    var subtle: Color {
        switch self {
        case .blue: return Color(fmARGB: 0x1A007AFF)
        case .purple: return Color(fmARGB: 0x1A6161F2)
        case .teal: return Color(fmARGB: 0x1A0D9488)
        }
    }

    var onDark: Color {
        switch self {
        case .blue: return Color(fmARGB: 0xFF0A84FF)
        case .purple: return Color(fmARGB: 0xFF6C63FF)
        case .teal: return Color(fmARGB: 0xFF2DD4BF)
        }
    }

    var subtleOnDark: Color {
        switch self {
        case .blue: return Color(fmARGB: 0x33164A8C)
        case .purple: return Color(fmARGB: 0x332F2A74)
        case .teal: return Color(fmARGB: 0x33115E59)
        }
    }
}

/// FmTheme's colours resolved for one brightness and one accent:
/// `FmTheme.textPrimary(context)` is `fm.textPrimary`, with
/// `@Environment(\.fmPalette) private var fm` in the view.
struct FmPalette: Equatable {
    /// FmTheme.isDark(context).
    let dark: Bool
    let accentChoice: FmAccent

    let bgBase: Color
    let bgRaised: Color
    let bgOverlay: Color
    let bgElevated: Color
    let textPrimary: Color
    let textSecondary: Color
    let textTertiary: Color
    let textDisabled: Color
    let accent: Color
    let accentSubtle: Color
    let success: Color
    let danger: Color
    let warning: Color
    /// FmTheme.border: borderMuted (#F0F0F0) in light, darkBorder in dark.
    let border: Color
    let codeKeyword: Color
    let codeType: Color
    let codeString: Color
    let codeComment: Color
    let codeNumber: Color
    let codeFunction: Color
    /// FmTheme.editorGutter.
    let editorGutter: Color
    /// FmTheme.editorActiveLine.
    let editorActiveLine: Color

    init(dark: Bool, accent: FmAccent = .fallback) {
        self.dark = dark
        self.accentChoice = accent
        bgBase = dark ? FmColors.darkBgBase : FmColors.bgBase
        bgRaised = dark ? FmColors.darkBgRaised : FmColors.bgRaised
        bgOverlay = dark ? FmColors.darkBgOverlay : FmColors.bgOverlay
        bgElevated = dark ? FmColors.darkBgElevated : FmColors.bgElevated
        textPrimary = dark ? FmColors.darkTextPrimary : FmColors.textPrimary
        textSecondary = dark ? FmColors.darkTextSecondary : FmColors.textSecondary
        textTertiary = dark ? FmColors.darkTextTertiary : FmColors.textTertiary
        textDisabled = dark ? FmColors.darkTextDisabled : FmColors.textDisabled
        self.accent = dark ? accent.onDark : accent.light
        accentSubtle = dark ? accent.subtleOnDark : accent.subtle
        success = dark ? FmColors.successOnDark : FmColors.success
        danger = dark ? FmColors.dangerOnDark : FmColors.danger
        warning = dark ? FmColors.warningOnDark : FmColors.warning
        border = dark ? FmColors.darkBorder : FmColors.borderMuted
        codeKeyword = dark ? FmColors.codeKeyword : FmColors.codeKeywordLight
        codeType = dark ? FmColors.codeType : FmColors.codeTypeLight
        codeString = dark ? FmColors.codeString : FmColors.codeStringLight
        codeComment = dark ? FmColors.codeComment : FmColors.codeCommentLight
        codeNumber = dark ? FmColors.codeNumber : FmColors.codeNumberLight
        codeFunction = dark ? FmColors.codeFunction : FmColors.codeFunctionLight
        editorGutter = dark ? FmColors.editorGutter : FmColors.bgOverlay
        editorActiveLine = dark ? FmColors.editorCurrentLine : FmColors.bgOverlay.opacity(0.72)
    }

    static let light = FmPalette(dark: false)
    static let darkDefault = FmPalette(dark: true)

    var offline: Color { return FmColors.offline }
    var accentEdge: Color { return FmColors.accentEdge }
    var count: Color { return dark ? FmAccent.blue.onDark : FmAccent.blue.light }

    /// FmTheme.activeUnderline.
    var activeUnderline: Color { return dark ? Color.white : FmColors.textPrimary }

    /// FmTheme.listSurface: explorer rows sit on the raised white layer in
    /// light and the base near-black canvas in dark, so they read as part of
    /// the page rather than as stacked cards.
    var listSurface: Color { return dark ? FmColors.darkBgBase : FmColors.bgRaised }

    /// FmTheme.editorSurface.
    var editorSurface: Color { return dark ? FmColors.darkBgBase : FmColors.bgRaised }

    /// `bottomSheetTheme.backgroundColor` and the dialog and popup-menu
    /// surfaces: a shadow does almost nothing in dark mode, so overlays sit on
    /// the overlay layer there, one step lighter than the page behind them.
    var sheetSurface: Color { return dark ? FmColors.darkBgOverlay : FmColors.bgRaised }

    /// `modalBarrierColor` of the bottom sheet theme.
    var sheetBarrier: Color { return Color(fmARGB: dark ? 0x8C000000 : 0x59000000) }

    /// The label on a primary-filled button (`onPrimary`).
    var onPrimary: Color { return dark ? FmColors.darkBgBase : Color.white }
}
