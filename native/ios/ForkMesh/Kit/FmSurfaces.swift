// FmGlassSurface, FmCard, FmSectionHeader, FmStatusBadge, FmEmptyState,
// FmBottomActionBar, FmPanelHeader, FmField, FmKeyValueRow, FmIconButton and
// FmConnectionBanner.

import SwiftUI

/// Frosted-glass surface: blurs whatever scrolls behind it, tints it with a
/// translucent wash of the raised surface colour, and edges it with a hairline
/// plus a faint top highlight so it reads as a pane of glass rather than a
/// smudge. Host it over scrolling content (a ZStack, a safeAreaInset).
///
/// With Increase Contrast or Reduce Transparency on, the surface goes opaque: a
/// see-through bar is exactly what those settings ask apps to stop drawing.
/// SwiftUI has no blur radius to set; the system's thinnest material stands
/// in for one.
struct FmGlassSurface<Content: View>: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.colorSchemeContrast) private var contrast
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    private let cornerRadius: CGFloat
    private let content: Content

    init(cornerRadius: CGFloat = FmRadius.full, @ViewBuilder content: () -> Content) {
        self.cornerRadius = cornerRadius
        self.content = content()
    }

    private var opaque: Bool {
        return reduceTransparency || contrast == .increased
    }

    private var shape: FmRoundedShape {
        return FmRoundedShape(radius: cornerRadius)
    }

    private var fill: Color {
        if opaque {
            return fm.bgRaised
        }
        return fm.bgRaised.opacity(fm.dark ? 0.55 : 0.66)
    }

    private var edge: Color {
        return fm.dark ? Color.white.opacity(0.12) : Color.black.opacity(0.06)
    }

    /// The specular edge: light catching the top of the pane.
    private var specular: LinearGradient {
        let top: Color = Color.white.opacity(fm.dark ? 0.07 : 0.45)
        return LinearGradient(
            colors: [top, Color.white.opacity(0)],
            startPoint: UnitPoint.top,
            endPoint: UnitPoint.center
        )
    }

    @ViewBuilder private var backdrop: some View {
        if opaque {
            shape.fill(fill)
        } else {
            shape.fill(fill).background(.ultraThinMaterial, in: shape)
        }
    }

    var body: some View {
        content
            .background { shape.fill(specular) }
            .background { backdrop }
            .overlay { shape.strokeBorder(edge, lineWidth: 0.5) }
            .clipShape(shape)
            .shadow(color: Color.black.opacity(fm.dark ? 0.45 : 0.10), radius: 12, x: 0, y: 8)
    }
}

/// FmCard: a raised, borderless, rounded layer; tappable with a light haptic
/// when `onTap` is set.
///
/// SwiftUI sizes to content, and a frame applied from outside would not stretch
/// the card's background, so the card fills the width itself unless `fill` is
/// false (a card inside a row).
struct FmCard<Content: View>: View {
    @Environment(\.fmPalette) private var fm

    private let onTap: (() -> Void)?
    private let color: Color?
    private let padding: EdgeInsets
    private let radius: CGFloat
    private let margin: EdgeInsets
    private let fill: Bool
    private let content: Content

    init(
        onTap: (() -> Void)? = nil,
        color: Color? = nil,
        padding: EdgeInsets? = nil,
        radius: CGFloat? = nil,
        margin: EdgeInsets? = nil,
        fill: Bool = true,
        @ViewBuilder content: () -> Content
    ) {
        self.onTap = onTap
        self.color = color
        self.padding = padding ?? EdgeInsets.fmAll(FmSpace.x4)
        self.radius = radius ?? FmRadius.md
        self.margin = margin ?? EdgeInsets()
        self.fill = fill
        self.content = content()
    }

    private var surface: some View {
        content
            .padding(padding)
            .frame(maxWidth: fill ? CGFloat.infinity : nil, alignment: .leading)
            .background(color ?? fm.bgRaised)
            .clipShape(FmRoundedShape(radius: radius))
    }

    @ViewBuilder private var tappable: some View {
        if let onTap = onTap {
            Button(action: {
                FmHaptics.tap()
                onTap()
            }) {
                surface
            }
            .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08), cornerRadius: radius))
        } else {
            surface
        }
    }

    var body: some View {
        tappable.padding(margin)
    }
}

/// FmSectionHeader: an uppercase eyebrow with an optional icon and a trailing
/// view or count.
struct FmSectionHeader: View {
    @Environment(\.fmPalette) private var fm

    private let title: String
    private let icon: String?
    private let trailing: AnyView?
    private let count: Int?

    init(title: String, icon: String? = nil, trailing: AnyView? = nil, count: Int? = nil) {
        self.title = title
        self.icon = icon
        self.trailing = trailing
        self.count = count
    }

    @ViewBuilder private var trailingView: some View {
        if let trailing = trailing {
            trailing.padding(.leading, FmSpace.x3)
        } else if let count = count {
            Text(String(count))
                .fmText(.bodySmall, tabular: true)
                .padding(.leading, FmSpace.x3)
        }
    }

    var body: some View {
        HStack(spacing: 0) {
            if let icon = icon {
                FmIcon(icon, size: 14, color: fm.textTertiary)
                    .padding(.trailing, FmSpace.x2)
            }
            Text(title.uppercased())
                .fmText(.labelSmall)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)
            trailingView
        }
    }
}

/// FmStatusBadge: an uppercase pill tinted with `color` at alpha 31.
struct FmStatusBadge: View {
    private let label: String
    private let color: Color
    private let backgroundColor: Color?

    init(label: String, color: Color, backgroundColor: Color? = nil) {
        self.label = label
        self.color = color
        self.backgroundColor = backgroundColor
    }

    var body: some View {
        Text(label.uppercased())
            .fmText(.labelSmall, color: color)
            .lineLimit(1)
            .truncationMode(.tail)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
            .background(backgroundColor ?? color.fmWithAlpha(31))
            .clipShape(Capsule())
    }
}

/// FmEmptyState: a centred icon, title and optional message. It fills the
/// space it is given.
struct FmEmptyState: View {
    @Environment(\.fmPalette) private var fm

    private let icon: String
    private let title: String
    private let message: String?

    init(icon: String, title: String, message: String? = nil) {
        self.icon = icon
        self.title = title
        self.message = message
    }

    var body: some View {
        VStack(spacing: 0) {
            FmIcon(icon, size: 40, color: fm.textTertiary)
            Text(title)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x4)
            if let message = message {
                Text(message)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .multilineTextAlignment(.center)
                    .padding(.top, FmSpace.x2)
            }
            FmMessageTools(message.map { "\(title)\n\($0)" } ?? title)
                .padding(.top, FmSpace.x2)
        }
        .padding(.horizontal, FmSpace.x5)
        .padding(.vertical, FmSpace.x7)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .center)
    }
}

/// FmBottomActionBar: a glass footer panel. Place it with
/// `.safeAreaInset(edge: .bottom)` or at the bottom of a VStack.
struct FmBottomActionBar<Content: View>: View {
    @Environment(\.fmPalette) private var fm

    private let padding: EdgeInsets
    private let content: Content

    init(padding: EdgeInsets? = nil, @ViewBuilder content: () -> Content) {
        self.padding = padding ?? EdgeInsets.fmAll(FmSpace.x4)
        self.content = content()
    }

    var body: some View {
        content
            .padding(padding)
            .frame(maxWidth: .infinity)
            .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg * 2, style: .continuous))
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
    }
}

/// FmPanelHeader: a 56pt header with a title, optional subtitle, leading and
/// trailing.
///
/// `titleBadge` is a small label that belongs to the title rather than to the
/// bar: a build number, a state chip. It sits on the title's baseline,
/// immediately after the text, and keeps its width while the title ellipsizes,
/// because a truncated title still reads but a truncated badge does not.
struct FmPanelHeader: View {
    private let title: String
    private let subtitle: String?
    private let leading: AnyView?
    private let titleBadge: AnyView?
    private let trailing: AnyView?

    init(
        title: String,
        subtitle: String? = nil,
        leading: AnyView? = nil,
        titleBadge: AnyView? = nil,
        trailing: AnyView? = nil
    ) {
        self.title = title
        self.subtitle = subtitle
        self.leading = leading
        self.titleBadge = titleBadge
        self.trailing = trailing
    }

    private var titleRow: some View {
        HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
            Text(title)
                .fmText(.titleMedium)
                .lineLimit(1)
                .truncationMode(.tail)
            if let titleBadge = titleBadge {
                titleBadge
                    .fixedSize()
                    .layoutPriority(1)
            }
        }
    }

    private var titles: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            titleRow
            if let subtitle = subtitle {
                Text(subtitle)
                    .fmText(.bodySmall)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            if let leading = leading {
                leading
            }
            titles
            if let trailing = trailing {
                trailing
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .frame(minHeight: 56)
    }
}

/// FmField: a form field that holds its label on a line above the control, so
/// the label never moves and the hint is free to carry a real example.
///
/// The visible label is decoration; the control below carries the same string
/// as its accessible name, so announcing both would stutter.
struct FmField<Content: View>: View {
    private let label: String
    private let content: Content

    init(label: String, @ViewBuilder content: () -> Content) {
        self.label = label
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(label)
                .fmText(.bodySmall, weight: FmType.medium)
                .accessibilityHidden(true)
            content
                .accessibilityLabel(Text(label))
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// FmKeyValueRow: a 96pt mono key and a wrapping value.
struct FmKeyValueRow: View {
    @Environment(\.fmPalette) private var fm

    private let label: String
    private let value: String

    init(label: String, value: String) {
        self.label = label
        self.value = value
    }

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x4) {
            Text(label)
                .fmMono(weight: FmType.medium, color: fm.textTertiary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(width: 96, alignment: .leading)
            Text(value)
                .fmText(.bodyMedium)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.vertical, FmSpace.x2)
    }
}

/// FmIconButton: an icon button with an explicit accessibility label (iOS has
/// no tooltip to match it). A 48pt target around the glyph, as Material lays
/// an IconButton out; nil `onPressed` disables it.
struct FmIconButton<Content: View>: View {
    @Environment(\.fmPalette) private var fm

    private let label: String
    private let onPressed: (() -> Void)?
    private let busy: Bool?
    private let content: Content
    @State private var tap: FmBusyTap = FmBusyTap()

    init(label: String, busy: Bool? = nil, onPressed: (() -> Void)?, @ViewBuilder content: () -> Content) {
        self.label = label
        self.busy = busy
        self.onPressed = onPressed
        self.content = content()
    }

    private var working: Bool {
        return busy ?? tap.busy
    }

    private func press() {
        guard let onPressed = onPressed, !working else {
            return
        }
        FmHaptics.tap(.forLabel(label, fallback: .click))
        tap.mark()
        onPressed()
    }

    var body: some View {
        Button(action: press) {
            content
                .frame(width: 48, height: 48)
        }
        .buttonStyle(FmIconButtonStyle(highlight: fm.textPrimary.opacity(0.08)))
        .disabled(onPressed == nil || working)
        .opacity(onPressed == nil && !working ? 0.38 : 1)
        .fmBusyRing(working, size: 36)
        .fmBusyTracking($tap, enabled: onPressed != nil)
        .accessibilityLabel(Text(label))
    }
}

extension FmIconButton where Content == FmIcon {
    /// `FmIconButton(icon: Icon(Icons.refresh, size: 20), ...)`. Inside an
    /// IconButton Material draws the glyph at 24pt in the on-surface colour
    /// (textPrimary) unless the call site says otherwise.
    init(
        icon: String, label: String, size: CGFloat = 24, color: Color? = nil, busy: Bool? = nil,
        onPressed: (() -> Void)?
    ) {
        self.init(label: label, busy: busy, onPressed: onPressed) {
            FmIcon(icon, size: size, color: color, tone: .primary)
        }
    }
}

/// The round 40pt pressed wash of a Material IconButton.
struct FmIconButtonStyle: ButtonStyle {
    let highlight: Color

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .background {
                Circle()
                    .fill(highlight)
                    .frame(width: 40, height: 40)
                    .opacity(configuration.isPressed ? 1 : 0)
            }
            .contentShape(Rectangle())
    }
}

/// The relay states FmConnectionBanner and AvatarWithDot draw. The Kit takes
/// values rather than reading services, so screens map the core's state onto
/// this one.
enum FmConnectionState {
    case offline
    case connecting
    case connected
}

/// FmConnectionBanner: the inline relay offline strip. Nothing while the relay
/// is connecting or connected: the avatar's dot shows a reconnect.
struct FmConnectionBanner: View {
    @Environment(\.fmPalette) private var fm

    private let state: FmConnectionState
    private let onRetry: (() -> Void)?

    init(state: FmConnectionState, onRetry: (() -> Void)? = nil) {
        self.state = state
        self.onRetry = onRetry
    }

    private var strip: some View {
        HStack(spacing: FmSpace.x3) {
            FmIcon("cloud_off_outlined", size: 18, color: fm.danger)
            Text("You are offline. Messages and live updates are paused.")
                .fmText(.bodyMedium, weight: FmType.medium)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmMessageTools("You are offline. Messages and live updates are paused.")
            if let onRetry = onRetry {
                FmTextButton("Retry", onPressed: onRetry)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .background(fm.danger.fmWithAlpha(22))
    }

    var body: some View {
        if state == .offline {
            strip
        }
    }
}
