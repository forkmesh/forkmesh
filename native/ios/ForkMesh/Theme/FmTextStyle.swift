// The type scale through Material's named roles, FmFonts and FmTheme.mono.
// A role on a Text is `.fmText(.titleMedium)`; with a colour and weight,
// `.fmText(.bodySmall, color: c, weight: w)`.

import SwiftUI

/// The roles of the type scale:
///
/// | role          | size | weight   | used for                        |
/// |---------------|------|----------|---------------------------------|
/// | displayLarge  | 40   | emphasis | the signed-out splash hero      |
/// | displaySmall  | 28   | emphasis | celebration and empty-state     |
/// | titleLarge    | 20   | emphasis | screen titles                   |
/// | titleMedium   | 17   | emphasis | panel and dialog headers        |
/// | titleSmall    | 15   | emphasis | list row titles                 |
/// | bodyLarge     | 15   | regular  | chat messages, primary prose    |
/// | bodyMedium    | 13   | regular  | descriptions, secondary prose   |
/// | bodySmall     | 12   | regular  | metadata, timestamps, counts    |
/// | labelLarge    | 15   | emphasis | button labels                   |
/// | labelMedium   | 13   | emphasis | compact buttons, tab labels     |
/// | labelSmall    | 11   | emphasis | badges and all-caps eyebrows    |
enum FmTextRole {
    case displayLarge
    case displaySmall
    case titleLarge
    case titleMedium
    case titleSmall
    case bodyLarge
    case bodyMedium
    case bodySmall
    case labelLarge
    case labelMedium
    case labelSmall
}

/// Which palette colour a role draws in unless the call site overrides it:
/// bodySmall is muted, labelSmall tertiary, the rest primary.
enum FmTextTone {
    case primary
    case secondary
    case tertiary
}

/// One resolved role: size, weight, line-height multiplier, tracking, tone.
struct FmTextSpec {
    let size: CGFloat
    let weight: Font.Weight
    let height: CGFloat
    let tracking: CGFloat
    let tone: FmTextTone

    static func of(_ role: FmTextRole) -> FmTextSpec {
        switch role {
        case .displayLarge:
            return FmTextSpec(size: FmType.hero, weight: FmType.emphasis, height: 1.02, tracking: -1, tone: .primary)
        case .displaySmall:
            return FmTextSpec(size: FmType.display, weight: FmType.emphasis, height: FmType.leadingTight, tracking: -0.5, tone: .primary)
        case .titleLarge:
            return FmTextSpec(size: FmType.title, weight: FmType.emphasis, height: FmType.leadingTight, tracking: -0.3, tone: .primary)
        case .titleMedium:
            return FmTextSpec(size: FmType.heading, weight: FmType.emphasis, height: FmType.leadingTight, tracking: -0.2, tone: .primary)
        case .titleSmall:
            return FmTextSpec(size: FmType.body, weight: FmType.emphasis, height: FmType.leadingTight, tracking: 0, tone: .primary)
        case .bodyLarge:
            return FmTextSpec(size: FmType.body, weight: FmType.regular, height: FmType.leadingRelaxed, tracking: 0, tone: .primary)
        case .bodyMedium:
            return FmTextSpec(size: FmType.small, weight: FmType.regular, height: FmType.leadingSnug, tracking: 0, tone: .primary)
        case .bodySmall:
            return FmTextSpec(size: FmType.meta, weight: FmType.regular, height: FmType.leadingSnug, tracking: 0, tone: .secondary)
        case .labelLarge:
            return FmTextSpec(size: FmType.body, weight: FmType.emphasis, height: FmType.leadingSnug, tracking: 0, tone: .primary)
        case .labelMedium:
            return FmTextSpec(size: FmType.small, weight: FmType.emphasis, height: FmType.leadingSnug, tracking: 0, tone: .primary)
        case .labelSmall:
            return FmTextSpec(size: FmType.micro, weight: FmType.emphasis, height: 1, tracking: 0.5, tone: .tertiary)
        }
    }
}

/// FmFonts. The UI face is SF Pro, which is the system font here, so there is
/// nothing to name: the system picks the Text or Display optical size by point
/// size, splitting at 20pt. The mono face is Menlo, present on every iOS
/// device.
enum FmFonts {
    static let mono: String = "Menlo"

    /// The UI face at a fixed size. Views use `.fmText`, which also follows
    /// Dynamic Type; this is for the few APIs that want a Font value.
    static func ui(size: CGFloat, weight: Font.Weight = FmType.regular) -> Font {
        return Font.system(size: size, weight: weight)
    }

    /// The mono face at a fixed size.
    static func monoFont(size: CGFloat, weight: Font.Weight = FmType.regular) -> Font {
        return Font.custom(FmFonts.mono, fixedSize: size).weight(weight)
    }

    /// The space SwiftUI must add between lines for a line `height`
    /// multiplier. SF Pro's natural line is about 1.19 em and Menlo's 1.17, so
    /// only the part of the multiplier above that is extra; a multiplier below
    /// it cannot be expressed and is left at the natural line.
    static func lineSpacing(size: CGFloat, height: CGFloat) -> CGFloat {
        let natural: CGFloat = 1.19
        let extra: CGFloat = (height - natural) * size
        return extra > 0 ? extra : 0
    }
}

/// Applies one role of the type scale. Sizes follow Dynamic Type through one
/// scale factor for every role, so the platform text scale applies to every
/// style alike.
struct FmTextStyleModifier: ViewModifier {
    @Environment(\.fmPalette) private var fm
    @ScaledMetric(relativeTo: .body) private var scale: CGFloat = 1

    let role: FmTextRole
    let color: Color?
    let weight: Font.Weight?
    let height: CGFloat?
    let tracking: CGFloat?
    let tabular: Bool

    private var spec: FmTextSpec {
        return FmTextSpec.of(role)
    }

    private var font: Font {
        let base: Font = Font.system(size: spec.size * scale, weight: weight ?? spec.weight)
        return tabular ? base.monospacedDigit() : base
    }

    private var resolvedColor: Color {
        if let color = color {
            return color
        }
        switch spec.tone {
        case .primary: return fm.textPrimary
        case .secondary: return fm.textSecondary
        case .tertiary: return fm.textTertiary
        }
    }

    private var spacing: CGFloat {
        return FmFonts.lineSpacing(size: spec.size * scale, height: height ?? spec.height)
    }

    func body(content: Content) -> some View {
        content
            .font(font)
            .tracking(tracking ?? spec.tracking)
            .lineSpacing(spacing)
            .foregroundStyle(resolvedColor)
    }
}

/// FmTheme.mono: hashes, paths, diffs, and anything whose alignment carries
/// meaning. Sized off the scale like everything else.
struct FmMonoStyleModifier: ViewModifier {
    @Environment(\.fmPalette) private var fm
    @ScaledMetric(relativeTo: .body) private var scale: CGFloat = 1

    let size: CGFloat
    let weight: Font.Weight
    let color: Color?
    let height: CGFloat

    func body(content: Content) -> some View {
        content
            .font(FmFonts.monoFont(size: size * scale, weight: weight))
            .lineSpacing(FmFonts.lineSpacing(size: size * scale, height: height))
            .foregroundStyle(color ?? fm.textPrimary)
    }
}

extension View {
    /// `FmTheme.text(context).<role>`, with the `copyWith` overrides the
    /// screens use. `tabular` is `fontFeatures: FmType.tabular`: digits that
    /// hold their column when a count changes.
    func fmText(
        _ role: FmTextRole,
        color: Color? = nil,
        weight: Font.Weight? = nil,
        height: CGFloat? = nil,
        tracking: CGFloat? = nil,
        tabular: Bool = false
    ) -> some View {
        return modifier(
            FmTextStyleModifier(
                role: role,
                color: color,
                weight: weight,
                height: height,
                tracking: tracking,
                tabular: tabular
            )
        )
    }

    /// `FmTheme.mono(context, size:, weight:, color:, height:)`.
    func fmMono(
        size: CGFloat = FmType.meta,
        weight: Font.Weight = FmType.regular,
        color: Color? = nil,
        height: CGFloat = FmType.leadingSnug
    ) -> some View {
        return modifier(FmMonoStyleModifier(size: size, weight: weight, color: color, height: height))
    }
}
