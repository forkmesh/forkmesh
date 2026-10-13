// Icons by Material icon name, following native/android's ui/theme/FmIcon.kt.

import SwiftUI

/// Icons by their Material names: `smart_toy_outlined` is
/// `FmIcon("smart_toy_outlined")`. Glyphs come from the MaterialIcons font in
/// Resources/Fonts (Apache-2.0), so no icon library is needed.
/// `FmIcons.notificationLine` (MingCute) is `FmIcons.notificationLine`.
enum FmIcons {
    static let notificationLine: String = "mingcute:notification_line"

    /// PostScript names, read from the name tables of the bundled files (name
    /// ID 6). `Font.custom` resolves by PostScript name, not by file name.
    static let materialFontName: String = "MaterialIcons-Regular"
    static let mingCuteFontName: String = "MingCute"

    private static let notificationLineCodepoint: UInt32 = 0xf127

    static func has(_ name: String) -> Bool {
        if name == notificationLine {
            return true
        }
        return lookup(name) != nil
    }

    /// The codepoint table files the default (filled) style under a
    /// `_baseline` suffix, so `smart_toy` and `smart_toy_baseline` are one glyph.
    private static func lookup(_ name: String) -> UInt32? {
        if let code = MaterialIconCodepoints.table[name] {
            return code
        }
        let suffix: String = "_baseline"
        if name.hasSuffix(suffix) {
            return MaterialIconCodepoints.table[String(name.dropLast(suffix.count))]
        }
        return nil
    }

    /// The codepoint for a name; unknown names draw `help_outline`.
    static func codepoint(_ name: String) -> UInt32 {
        if name == notificationLine {
            return notificationLineCodepoint
        }
        if let code = lookup(name) {
            return code
        }
        return MaterialIconCodepoints.table["help_outline"] ?? 0xe30b
    }

    static func glyph(_ name: String) -> String {
        guard let scalar = Unicode.Scalar(codepoint(name)) else {
            return "?"
        }
        return String(Character(scalar))
    }

    static func fontName(_ name: String) -> String {
        return name == notificationLine ? mingCuteFontName : materialFontName
    }
}

/// An icon by name, with an optional size and colour. Without a size or colour
/// it takes the theme's `iconTheme`: 20pt in textSecondary. It does not grow with Dynamic Type, and it is hidden from VoiceOver unless it
/// is given a label.
struct FmIcon: View {
    @Environment(\.fmPalette) private var fm

    private let name: String
    private let size: CGFloat
    private let color: Color?
    private let label: String?
    private let tone: FmTextTone

    /// `tone` picks the palette colour used when `color` is nil: secondary is
    /// the iconTheme; Material buttons draw their glyphs in primary.
    init(_ name: String, size: CGFloat = 20, color: Color? = nil, label: String? = nil, tone: FmTextTone = .secondary) {
        self.name = name
        self.size = size
        self.color = color
        self.label = label
        self.tone = tone
        FontRegistration.registerBundledFonts()
    }

    private var resolvedColor: Color {
        if let color = color {
            return color
        }
        switch tone {
        case .primary: return fm.textPrimary
        case .secondary: return fm.textSecondary
        case .tertiary: return fm.textTertiary
        }
    }

    var body: some View {
        // fixedSize keeps a glyph whose advance rounds a hair over the frame
        // from being truncated to an ellipsis.
        Text(FmIcons.glyph(name))
            .font(Font.custom(FmIcons.fontName(name), fixedSize: size))
            .foregroundStyle(resolvedColor)
            .fixedSize()
            .frame(width: size, height: size)
            .accessibilityHidden(label == nil)
            .accessibilityLabel(Text(label ?? ""))
    }
}
