// The scalar tokens (FmSpace, FmType, FmRadius, FmMotion). Colours are in FmColors.swift, the environment and type scale in
// ForkMeshTheme.swift and FmTextStyle.swift.

import SwiftUI

/// FmSpace: the spacing scale.
enum FmSpace {
    static let x0: CGFloat = 0
    static let x1: CGFloat = 4
    static let x2: CGFloat = 8
    static let x3: CGFloat = 12
    static let x4: CGFloat = 16
    static let x5: CGFloat = 24
    static let x6: CGFloat = 32
    static let x7: CGFloat = 48
    static let x8: CGFloat = 64
}

/// FmType: seven interface sizes plus the hero, three weights, three leadings,
/// and nothing outside this set. Read styles through `.fmText(_:)` rather than
/// declaring sizes inline.
enum FmType {
    /// 40: the marketing hero on the signed-out splash, and nothing else.
    static let hero: CGFloat = 40
    /// 28: celebration and empty-state numerals.
    static let display: CGFloat = 28
    /// 20: screen titles.
    static let title: CGFloat = 20
    /// 17: panel headers, dialog titles, primary row titles.
    static let heading: CGFloat = 17
    /// 15: row titles, button labels, chat message bodies.
    static let body: CGFloat = 15
    /// 13: secondary body copy and descriptions.
    static let small: CGFloat = 13
    /// 12: metadata, timestamps, counts.
    static let meta: CGFloat = 12
    /// 11: badges and all-caps eyebrows.
    static let micro: CGFloat = 11
    /// 9 and 7: the live telemetry strip only.
    static let gauge: CGFloat = 9
    static let gaugeCaption: CGFloat = 7

    /// w400, w500 and w600. w700 and above are deliberately absent.
    static let regular: Font.Weight = .regular
    static let medium: Font.Weight = .medium
    static let emphasis: Font.Weight = .semibold

    /// Line heights, as multipliers of the font size.
    static let leadingTight: CGFloat = 1.2
    static let leadingSnug: CGFloat = 1.35
    static let leadingRelaxed: CGFloat = 1.5
}

/// FmRadius.
enum FmRadius {
    static let sm: CGFloat = 4
    static let md: CGFloat = 8
    static let lg: CGFloat = 12
    static let full: CGFloat = 999
}

/// FmMotion: durations in seconds plus the two curves the widgets use.
/// `FmMotion.effective(context, d)` is `FmMotion.effective(d, reduceMotion:)`,
/// with the flag read from `@Environment(\.accessibilityReduceMotion)`.
enum FmMotion {
    static let fast: Double = 0.1
    static let normal: Double = 0.2

    /// Zero when the platform asks for reduced motion.
    static func effective(_ duration: Double, reduceMotion: Bool) -> Double {
        return reduceMotion ? 0 : duration
    }

    /// Curves.easeOutCubic over `duration`; nil (no animation) under reduced motion.
    static func easeOutCubic(_ duration: Double, reduceMotion: Bool) -> Animation? {
        if reduceMotion || duration <= 0 {
            return nil
        }
        return Animation.timingCurve(0.215, 0.61, 0.355, 1.0, duration: duration)
    }

    /// Curves.easeInCubic over `duration`; nil under reduced motion.
    static func easeInCubic(_ duration: Double, reduceMotion: Bool) -> Animation? {
        if reduceMotion || duration <= 0 {
            return nil
        }
        return Animation.timingCurve(0.55, 0.055, 0.675, 0.19, duration: duration)
    }
}
