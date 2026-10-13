// Shared plumbing for the Kit: press highlights, edge insets and optional
// child views that SwiftUI does not provide.

import SwiftUI

extension View {
    /// For the optional slots of the kit (`leading:`, `trailing:`, `actions:`),
    /// which take `AnyView?` so a view with several slots needs no generic
    /// parameter per slot: `leading: FmIconTile(icon: "folder").fmAny`.
    var fmAny: AnyView {
        return AnyView(self)
    }
}

extension EdgeInsets {
    /// EdgeInsets.all.
    static func fmAll(_ value: CGFloat) -> EdgeInsets {
        return EdgeInsets(top: value, leading: value, bottom: value, trailing: value)
    }

    /// EdgeInsets.symmetric.
    static func fmSymmetric(horizontal: CGFloat = 0, vertical: CGFloat = 0) -> EdgeInsets {
        return EdgeInsets(top: vertical, leading: horizontal, bottom: vertical, trailing: horizontal)
    }

    /// EdgeInsets.fromLTRB.
    static func fmLTRB(_ left: CGFloat, _ top: CGFloat, _ right: CGFloat, _ bottom: CGFloat) -> EdgeInsets {
        return EdgeInsets(top: top, leading: left, bottom: bottom, trailing: right)
    }
}

/// `BorderRadius.circular(radius)` as a shape. The radius is clamped to half
/// the shorter side, so `FmRadius.full` (999) draws a stadium at any size
/// instead of depending on how the system treats an oversized radius.
struct FmRoundedShape: InsettableShape {
    var radius: CGFloat
    var insetAmount: CGFloat = 0

    init(radius: CGFloat) {
        self.radius = radius
    }

    func path(in rect: CGRect) -> Path {
        let box: CGRect = rect.insetBy(dx: insetAmount, dy: insetAmount)
        if box.isNull || box.width <= 0 || box.height <= 0 {
            return Path()
        }
        let limit: CGFloat = min(box.width, box.height) / 2
        let wanted: CGFloat = max(0, radius - insetAmount)
        return Path(roundedRect: box, cornerRadius: min(wanted, limit))
    }

    func inset(by amount: CGFloat) -> FmRoundedShape {
        var copy: FmRoundedShape = self
        copy.insetAmount += amount
        return copy
    }
}

/// InkWell's pressed state: a wash over the label while the finger is down.
/// There is no ripple on iOS; the highlight alone is drawn.
struct FmPressStyle: ButtonStyle {
    let highlight: Color
    let cornerRadius: CGFloat

    init(highlight: Color, cornerRadius: CGFloat = 0) {
        self.highlight = highlight
        self.cornerRadius = cornerRadius
    }

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .overlay {
                FmRoundedShape(radius: cornerRadius)
                    .fill(highlight)
                    .opacity(configuration.isPressed ? 1 : 0)
                    .allowsHitTesting(false)
            }
            .contentShape(Rectangle())
    }
}

/// A 1pt rule in the border colour on one edge.
struct FmHairline: View {
    @Environment(\.fmPalette) private var fm

    private let color: Color?

    init(color: Color? = nil) {
        self.color = color
    }

    var body: some View {
        Rectangle()
            .fill(color ?? fm.border)
            .frame(height: 1)
    }
}
