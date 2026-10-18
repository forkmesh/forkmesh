import FMBase
import Foundation

public enum ThemeTokenGroup: String, CaseIterable, Sendable {
    case surfaces
    case text
    case accent
    case status

    public var label: String {
        switch self {
        case .surfaces: return "Surfaces"
        case .text: return "Text"
        case .accent: return "Accent"
        case .status: return "Status"
        }
    }

    public var tokens: [ThemeToken] {
        ThemeToken.allCases.filter { $0.group == self }
    }
}

public enum ThemeToken: String, CaseIterable, Sendable {
    case bgBase
    case bgRaised
    case bgOverlay
    case bgElevated
    case border
    case textPrimary
    case textSecondary
    case textTertiary
    case accent
    case onAccent
    case success
    case danger
    case warning

    public var label: String {
        switch self {
        case .bgBase: return "Background"
        case .bgRaised: return "Cards and bars"
        case .bgOverlay: return "Sheets and fields"
        case .bgElevated: return "Raised controls"
        case .border: return "Borders"
        case .textPrimary: return "Text"
        case .textSecondary: return "Secondary text"
        case .textTertiary: return "Hints and captions"
        case .accent: return "Accent"
        case .onAccent: return "Text on accent"
        case .success: return "Success"
        case .danger: return "Danger"
        case .warning: return "Warning"
        }
    }

    public var group: ThemeTokenGroup {
        switch self {
        case .bgBase, .bgRaised, .bgOverlay, .bgElevated, .border: return .surfaces
        case .textPrimary, .textSecondary, .textTertiary: return .text
        case .accent, .onAccent: return .accent
        case .success, .danger, .warning: return .status
        }
    }
}

public enum ThemeSurface: String, CaseIterable, Sendable {
    case forkmesh
    case slate
    case zinc
    case stone
    case pure

    public static let fallback: ThemeSurface = .forkmesh

    public static func fromName(_ name: String?) -> ThemeSurface {
        name.flatMap(ThemeSurface.init(rawValue:)) ?? fallback
    }

    public var label: String {
        switch self {
        case .forkmesh: return "ForkMesh"
        case .slate: return "Slate"
        case .zinc: return "Zinc"
        case .stone: return "Stone"
        case .pure: return "Pure"
        }
    }

    public func colors(dark: Bool) -> [ThemeToken: UInt32] {
        let values: [UInt32]
        switch (self, dark) {
        case (.forkmesh, false): values = [0xF7F7F7, 0xFFFFFF, 0xF0F0F0, 0xE8E8E8, 0xF0F0F0, 0x0A0A0A]
        case (.forkmesh, true): values = [0x0B0B0C, 0x141414, 0x202022, 0x26262A, 0x252529, 0xF4F4F5]
        case (.slate, false): values = [0xF6F8FA, 0xFFFFFF, 0xEFF2F5, 0xE6EAEF, 0xE1E4E8, 0x1F2328]
        case (.slate, true): values = [0x010409, 0x0D1117, 0x161B22, 0x21262D, 0x30363D, 0xF0F6FC]
        case (.zinc, false): values = [0xFAFAFA, 0xFFFFFF, 0xF4F4F5, 0xE4E4E7, 0xE4E4E7, 0x18181B]
        case (.zinc, true): values = [0x09090B, 0x18181B, 0x27272A, 0x3F3F46, 0x27272A, 0xF4F4F5]
        case (.stone, false): values = [0xFAFAF9, 0xFFFFFF, 0xF5F5F4, 0xE7E5E4, 0xE7E5E4, 0x1C1917]
        case (.stone, true): values = [0x0C0A09, 0x1C1917, 0x292524, 0x44403C, 0x292524, 0xF5F5F4]
        case (.pure, false): values = [0xFFFFFF, 0xFFFFFF, 0xF2F2F2, 0xE8E8E8, 0xE5E5E5, 0x000000]
        case (.pure, true): values = [0x000000, 0x0A0A0A, 0x161616, 0x1F1F1F, 0x1F1F1F, 0xFFFFFF]
        }
        let tokens: [ThemeToken] = [.bgBase, .bgRaised, .bgOverlay, .bgElevated, .border, .textPrimary]
        return Dictionary(uniqueKeysWithValues: zip(tokens, values))
    }
}

extension AccentChoice {
    public var rgb: (light: UInt32, dark: UInt32, subtleDark: UInt32) {
        switch self {
        case .blue: return (light: 0x0066CC, dark: 0x409CFF, subtleDark: 0x164A8C)
        case .cyan: return (light: 0x0E7490, dark: 0x22D3EE, subtleDark: 0x0E7490)
        case .teal: return (light: 0x0F766E, dark: 0x2DD4BF, subtleDark: 0x115E59)
        case .green: return (light: 0x147C3B, dark: 0x4ADE80, subtleDark: 0x15803D)
        case .lime: return (light: 0x4B780F, dark: 0xA3E635, subtleDark: 0x4D7C0F)
        case .amber: return (light: 0xAF5109, dark: 0xFBBF24, subtleDark: 0xB45309)
        case .orange: return (light: 0xC0400C, dark: 0xFB923C, subtleDark: 0xC2410C)
        case .red: return (light: 0xD22222, dark: 0xF87171, subtleDark: 0xB91C1C)
        case .pink: return (light: 0xCB226D, dark: 0xF472B6, subtleDark: 0xBE185D)
        case .violet: return (light: 0x9333EA, dark: 0xC084FC, subtleDark: 0x7E22CE)
        case .purple: return (light: 0x5856D6, dark: 0x8B85FF, subtleDark: 0x2F2A74)
        case .graphite: return (light: 0x52525B, dark: 0xA1A1AA, subtleDark: 0x52525B)
        }
    }
}

public enum ThemeColor {
    public static let minimumContrast: Double = 4.5

    public static func hex(_ rgb: UInt32) -> String {
        String(format: "#%06x", rgb & 0xFFFFFF)
    }

    public static func parseHex(_ text: String) -> UInt32? {
        var digits: Substring = Substring(text.trimmingCharacters(in: .whitespacesAndNewlines))
        if digits.hasPrefix("#") {
            digits = digits.dropFirst()
        }
        guard digits.count == 6, digits.allSatisfy(\.isHexDigit), let value = UInt32(digits, radix: 16) else {
            return nil
        }
        return value
    }

    public static func luminance(_ rgb: UInt32) -> Double {
        let channel: (UInt32) -> Double = { (shift: UInt32) -> Double in
            let value: Double = Double((rgb >> shift) & 0xFF) / 255
            return value <= 0.03928 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0)
    }

    public static func contrast(_ a: UInt32, _ b: UInt32) -> Double {
        let first: Double = luminance(a)
        let second: Double = luminance(b)
        return (max(first, second) + 0.05) / (min(first, second) + 0.05)
    }

    public static func mix(_ a: UInt32, _ b: UInt32, amount: Double) -> UInt32 {
        let part: (UInt32) -> UInt32 = { (shift: UInt32) -> UInt32 in
            let from: Double = Double((a >> shift) & 0xFF)
            let to: Double = Double((b >> shift) & 0xFF)
            return UInt32(min(255, max(0, (from + (to - from) * amount).rounded()))) << shift
        }
        return part(16) | part(8) | part(0)
    }

    public static func hsl(_ rgb: UInt32) -> (hue: Double, saturation: Double, lightness: Double) {
        let red: Double = Double((rgb >> 16) & 0xFF) / 255
        let green: Double = Double((rgb >> 8) & 0xFF) / 255
        let blue: Double = Double(rgb & 0xFF) / 255
        let high: Double = max(red, green, blue)
        let low: Double = min(red, green, blue)
        let lightness: Double = (high + low) / 2
        guard high > low else {
            return (hue: 0, saturation: 0, lightness: lightness)
        }
        let delta: Double = high - low
        let saturation: Double = lightness > 0.5 ? delta / (2 - high - low) : delta / (high + low)
        var hue: Double
        if high == red {
            hue = (green - blue) / delta + (green < blue ? 6 : 0)
        } else if high == green {
            hue = (blue - red) / delta + 2
        } else {
            hue = (red - green) / delta + 4
        }
        hue *= 60
        return (hue: hue, saturation: saturation, lightness: lightness)
    }

    public static func fromHsl(hue: Double, saturation: Double, lightness: Double) -> UInt32 {
        let chroma: Double = saturation * min(lightness, 1 - lightness)
        let part: (Double) -> UInt32 = { (n: Double) -> UInt32 in
            let k: Double = (n + hue / 30).truncatingRemainder(dividingBy: 12)
            let value: Double = lightness - chroma * max(-1, min(k - 3, 9 - k, 1))
            return UInt32(min(255, max(0, (value * 255).rounded())))
        }
        return (part(0) << 16) | (part(8) << 8) | part(4)
    }

    public static func passes(_ rgb: UInt32, on surfaces: [UInt32], minimum: Double = minimumContrast) -> Bool {
        surfaces.allSatisfy { contrast(rgb, $0) >= minimum }
    }

    public static func readable(_ rgb: UInt32, on surfaces: [UInt32], minimum: Double = minimumContrast) -> UInt32 {
        guard let first = surfaces.first, !passes(rgb, on: surfaces, minimum: minimum) else {
            return rgb & 0xFFFFFF
        }
        let darken: Bool = luminance(first) > 0.18
        let start = hsl(rgb)
        var low: Double = darken ? 0 : start.lightness
        var high: Double = darken ? start.lightness : 1
        var best: UInt32 = darken ? 0x000000 : 0xFFFFFF
        for _ in 0..<24 {
            let middle: Double = (low + high) / 2
            let candidate: UInt32 = fromHsl(hue: start.hue, saturation: start.saturation, lightness: middle)
            if passes(candidate, on: surfaces, minimum: minimum) {
                best = candidate
                if darken { low = middle } else { high = middle }
            } else {
                if darken { high = middle } else { low = middle }
            }
        }
        return best
    }

    public static func label(on fill: UInt32, preferred: UInt32) -> UInt32 {
        if contrast(fill, preferred) >= minimumContrast {
            return preferred
        }
        let white: UInt32 = 0xFFFFFF
        let black: UInt32 = 0x0A0A0A
        return contrast(fill, white) >= contrast(fill, black) ? white : black
    }
}

public struct ResolvedTheme: Equatable, Sendable {
    public let dark: Bool
    public let colors: [ThemeToken: UInt32]
    public let textDisabled: UInt32
    public let accentSubtle: UInt32

    public subscript(token: ThemeToken) -> UInt32 {
        colors[token] ?? 0xFF000000
    }

    public func rgb(_ token: ThemeToken) -> UInt32 {
        self[token] & 0xFFFFFF
    }
}

public struct AppTheme: Equatable, Sendable {
    public var surface: ThemeSurface
    public var accent: AccentChoice
    public var customAccent: UInt32?
    public var readable: Bool
    public var lightColors: [ThemeToken: UInt32]
    public var darkColors: [ThemeToken: UInt32]

    public init(
        surface: ThemeSurface = .fallback, accent: AccentChoice = .fallback, customAccent: UInt32? = nil,
        readable: Bool = true, lightColors: [ThemeToken: UInt32] = [:], darkColors: [ThemeToken: UInt32] = [:]
    ) {
        self.surface = surface
        self.accent = accent
        self.customAccent = customAccent.map { $0 & 0xFFFFFF }
        self.readable = readable
        self.lightColors = lightColors
        self.darkColors = darkColors
    }

    public static let standard: AppTheme = AppTheme()

    public static let customAccentName: String = "custom"

    public var isStandard: Bool { self == AppTheme.standard }

    public var accentName: String { customAccent == nil ? accent.rawValue : AppTheme.customAccentName }

    public func edits(dark: Bool) -> [ThemeToken: UInt32] {
        dark ? darkColors : lightColors
    }

    public mutating func setColor(_ rgb: UInt32?, for token: ThemeToken, dark: Bool) {
        if dark {
            darkColors[token] = rgb.map { $0 & 0xFFFFFF }
        } else {
            lightColors[token] = rgb.map { $0 & 0xFFFFFF }
        }
    }

    public mutating func choose(accent: AccentChoice) {
        self.accent = accent
        customAccent = nil
        clearEdits(in: .accent)
    }

    public mutating func choose(customAccent rgb: UInt32) {
        customAccent = rgb & 0xFFFFFF
        clearEdits(in: .accent)
    }

    public mutating func choose(surface: ThemeSurface) {
        self.surface = surface
        clearEdits(in: .surfaces)
        clearEdits(in: .text)
    }

    public mutating func clearEdits(in group: ThemeTokenGroup) {
        for token in group.tokens {
            lightColors[token] = nil
            darkColors[token] = nil
        }
    }

    public static func shuffled<Generator: RandomNumberGenerator>(using generator: inout Generator) -> AppTheme {
        let hue: Double = Double(Int.random(in: 0..<360, using: &generator))
        let surface: ThemeSurface = ThemeSurface.allCases.randomElement(using: &generator) ?? .fallback
        let accent: UInt32 = ThemeColor.fromHsl(hue: hue, saturation: 0.75, lightness: 0.5)
        return AppTheme(surface: surface, customAccent: accent)
    }

    public func resolve(dark: Bool) -> ResolvedTheme {
        let edits: [ThemeToken: UInt32] = edits(dark: dark)
        var base: [ThemeToken: UInt32] = surface.colors(dark: dark)
        base.merge(edits.filter { $0.key.group == .surfaces || $0.key == .textPrimary }) { $1 }
        let surfaces: [UInt32] = [base[.bgBase] ?? 0, base[.bgRaised] ?? 0, base[.bgOverlay] ?? 0]
        let fix: (UInt32) -> UInt32 = { (rgb: UInt32) -> UInt32 in
            readable ? ThemeColor.readable(rgb, on: surfaces) : rgb
        }
        let text: UInt32 = fix(base[.textPrimary] ?? 0)
        let accentSeed: UInt32 = edits[.accent] ?? customAccent ?? (dark ? accent.rgb.dark : accent.rgb.light)
        let accentColor: UInt32 = fix(accentSeed)
        let status: (light: [UInt32], dark: [UInt32]) = (light: [0x0F7A3B, 0xC93532, 0x8F5A0A], dark: [0x46E07F, 0xFF6B6B, 0xE5C84C])
        let statusSeeds: [UInt32] = dark ? status.dark : status.light
        let preferredLabel: UInt32 = edits[.onAccent] ?? (dark ? (base[.bgBase] ?? 0) : 0xFFFFFF)
        let onAccent: UInt32 = readable ? ThemeColor.label(on: accentColor, preferred: preferredLabel) : preferredLabel
        let opaque: (UInt32) -> UInt32 = { 0xFF000000 | ($0 & 0xFFFFFF) }
        let alpha: (UInt32, UInt32) -> UInt32 = { (value: UInt32, rgb: UInt32) -> UInt32 in (value << 24) | (rgb & 0xFFFFFF) }
        var colors: [ThemeToken: UInt32] = [:]
        for token in ThemeTokenGroup.surfaces.tokens {
            colors[token] = opaque(base[token] ?? 0)
        }
        colors[.textPrimary] = opaque(text)
        colors[.textSecondary] = edits[.textSecondary].map(opaque) ?? alpha(dark ? 0xB3 : 0xB8, text)
        colors[.textTertiary] = edits[.textTertiary].map(opaque) ?? alpha(dark ? 0x80 : 0x99, text)
        colors[.accent] = opaque(accentColor)
        colors[.onAccent] = opaque(onAccent)
        colors[.success] = opaque(fix(edits[.success] ?? statusSeeds[0]))
        colors[.danger] = opaque(fix(edits[.danger] ?? statusSeeds[1]))
        colors[.warning] = opaque(fix(edits[.warning] ?? statusSeeds[2]))
        let subtle: UInt32
        if !dark {
            subtle = alpha(0x1A, accentColor)
        } else if edits[.accent] == nil, customAccent == nil {
            subtle = alpha(0x33, accent.rgb.subtleDark)
        } else {
            subtle = alpha(0x33, ThemeColor.mix(accentColor, 0x000000, amount: 0.45))
        }
        return ResolvedTheme(dark: dark, colors: colors, textDisabled: alpha(dark ? 0x4D : 0x40, text), accentSubtle: subtle)
    }

    public func colorsJSON() -> String {
        guard !lightColors.isEmpty || !darkColors.isEmpty else {
            return ""
        }
        let encode: ([ThemeToken: UInt32]) -> [String: String] = { (colors: [ThemeToken: UInt32]) -> [String: String] in
            Dictionary(uniqueKeysWithValues: colors.map { ($0.key.rawValue, ThemeColor.hex($0.value)) })
        }
        var document: [String: [String: String]] = [:]
        if !darkColors.isEmpty { document["dark"] = encode(darkColors) }
        if !lightColors.isEmpty { document["light"] = encode(lightColors) }
        guard let data = try? JSONSerialization.data(withJSONObject: document, options: [.sortedKeys]) else {
            return ""
        }
        return String(decoding: data, as: UTF8.self)
    }

    public static func parseColors(_ json: String?) -> (light: [ThemeToken: UInt32], dark: [ThemeToken: UInt32]) {
        guard let json, let data = json.data(using: .utf8),
            let document = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        else {
            return (light: [:], dark: [:])
        }
        let decode: (Any?) -> [ThemeToken: UInt32] = { (value: Any?) -> [ThemeToken: UInt32] in
            guard let entries = value as? [String: Any] else { return [:] }
            var colors: [ThemeToken: UInt32] = [:]
            for (name, raw) in entries {
                if let token = ThemeToken(rawValue: name), let text = raw as? String, let rgb = ThemeColor.parseHex(text) {
                    colors[token] = rgb
                }
            }
            return colors
        }
        return (light: decode(document["light"]), dark: decode(document["dark"]))
    }
}
