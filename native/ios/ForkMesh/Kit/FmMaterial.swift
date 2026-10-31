// The component themes the screens use as stock Material widgets: TextButton,
// FilledButton, ElevatedButton and OutlinedButton, the floating action button,
// the progress indicators (`progressIndicatorTheme`) and TextField under
// `inputDecorationTheme`. Follows native/android's ui/kit/FmMaterial.kt.
// Tooltips have no iOS counterpart (the accessibility label carries the text),
// and popup menus are SwiftUI's own `Menu`.

import SwiftUI

/// The pressed wash and the disabled dimming shared by the Material buttons.
struct FmMaterialButtonStyle: ButtonStyle {
    let highlight: Color
    let radius: CGFloat

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .overlay {
                FmRoundedShape(radius: radius)
                    .fill(highlight)
                    .opacity(configuration.isPressed ? 1 : 0)
                    .allowsHitTesting(false)
            }
            .contentShape(Rectangle())
    }
}

/// The icon-and-text row inside every Material button.
struct FmButtonLabel: View {
    let label: String
    let icon: String?
    let color: Color
    let role: FmTextRole
    var busy: Bool = false
    var ringColor: Color? = nil

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            if let icon = icon {
                FmIcon(icon, size: 18, color: color)
                    .fmBusyRing(busy, size: 26, color: ringColor)
            }
            Text(label)
                .fmText(role, color: color)
                .lineLimit(1)
        }
    }
}

/// The themed TextButton: accent label, 44pt tall, 12pt
/// side padding, stadium. `color` is `foregroundColor`, `minHeight` the height
/// of `minimumSize`. Nil `onPressed` disables it.
struct FmTextButton: View {
    @Environment(\.fmPalette) private var fm

    private let label: String
    private let icon: String?
    private let color: Color?
    private let minHeight: CGFloat
    private let fillWidth: Bool
    private let busy: Bool?
    private let onPressed: (() -> Void)?
    @State private var tap: FmBusyTap = FmBusyTap()

    init(
        _ label: String,
        icon: String? = nil,
        color: Color? = nil,
        minHeight: CGFloat = 44,
        fillWidth: Bool = false,
        busy: Bool? = nil,
        onPressed: (() -> Void)?
    ) {
        self.label = label
        self.icon = icon
        self.color = color
        self.minHeight = minHeight
        self.fillWidth = fillWidth
        self.busy = busy
        self.onPressed = onPressed
    }

    private var tone: Color {
        return color ?? fm.accent
    }

    private var working: Bool {
        return busy ?? tap.busy
    }

    private func press() {
        guard let onPressed = onPressed, !working else {
            return
        }
        tap.mark()
        onPressed()
    }

    var body: some View {
        Button(action: press) {
            FmButtonLabel(label: label, icon: icon, color: tone, role: .labelLarge, busy: working)
                .padding(.horizontal, FmSpace.x3)
                .frame(maxWidth: fillWidth ? CGFloat.infinity : nil, minHeight: minHeight)
                .fmBusyRing(working && icon == nil, corner: FmRadius.full)
        }
        .buttonStyle(FmMaterialButtonStyle(highlight: tone.opacity(0.10), radius: FmRadius.full))
        .disabled(onPressed == nil || working)
        .opacity(onPressed == nil && !working ? 0.38 : 1)
        .fmBusyTracking($tap, enabled: onPressed != nil)
    }
}

/// FilledButton under `filledButtonTheme`: primary-filled stadium with the
/// on-primary label. `background`, `foreground`, `minHeight`, `radius` and
/// `fillWidth` are the `styleFrom` overrides the screens use
/// (`minimumSize: Size.fromHeight(48)` is `minHeight: 48, fillWidth: true`).
struct FmFilledButton: View {
    @Environment(\.fmPalette) private var fm

    private let label: String
    private let icon: String?
    private let background: Color?
    private let foreground: Color?
    private let minHeight: CGFloat
    private let radius: CGFloat
    private let fillWidth: Bool
    private let role: FmTextRole
    private let busy: Bool?
    private let onPressed: (() -> Void)?
    @State private var tap: FmBusyTap = FmBusyTap()

    init(
        _ label: String,
        icon: String? = nil,
        background: Color? = nil,
        foreground: Color? = nil,
        minHeight: CGFloat = 40,
        radius: CGFloat = FmRadius.full,
        fillWidth: Bool = false,
        role: FmTextRole = .labelLarge,
        busy: Bool? = nil,
        onPressed: (() -> Void)?
    ) {
        self.label = label
        self.icon = icon
        self.background = background
        self.foreground = foreground
        self.minHeight = minHeight
        self.radius = radius
        self.fillWidth = fillWidth
        self.role = role
        self.busy = busy
        self.onPressed = onPressed
    }

    private var working: Bool {
        return busy ?? tap.busy
    }

    private var enabled: Bool {
        return onPressed != nil || working
    }

    private func press() {
        guard let onPressed = onPressed, !working else {
            return
        }
        tap.mark()
        onPressed()
    }

    /// Material's disabled fill and label: on-surface at 12% and 38%.
    private var fillColor: Color {
        return enabled ? (background ?? fm.textPrimary) : fm.textPrimary.opacity(0.12)
    }

    private var labelColor: Color {
        return enabled ? (foreground ?? fm.onPrimary) : fm.textPrimary.opacity(0.38)
    }

    var body: some View {
        Button(action: press) {
            FmButtonLabel(label: label, icon: icon, color: labelColor, role: role, busy: working, ringColor: labelColor)
                .padding(.horizontal, FmSpace.x5)
                .frame(maxWidth: fillWidth ? CGFloat.infinity : nil, minHeight: minHeight)
                .background(fillColor)
                .clipShape(FmRoundedShape(radius: radius))
                .fmBusyRing(working && icon == nil, corner: radius, color: labelColor)
        }
        .buttonStyle(FmMaterialButtonStyle(highlight: labelColor.opacity(0.12), radius: radius))
        .disabled(onPressed == nil || working)
        .fmBusyTracking($tap, enabled: onPressed != nil)
    }
}

/// ElevatedButton under `elevatedButtonTheme`, drawn exactly like the filled
/// button: flat, primary-filled, stadium.
struct FmElevatedButton: View {
    private let label: String
    private let icon: String?
    private let fillWidth: Bool
    private let busy: Bool?
    private let onPressed: (() -> Void)?

    init(_ label: String, icon: String? = nil, fillWidth: Bool = false, busy: Bool? = nil, onPressed: (() -> Void)?) {
        self.label = label
        self.icon = icon
        self.fillWidth = fillWidth
        self.busy = busy
        self.onPressed = onPressed
    }

    var body: some View {
        FmFilledButton(label, icon: icon, fillWidth: fillWidth, busy: busy, onPressed: onPressed)
    }
}

/// OutlinedButton under `outlinedButtonTheme`: primary label, a border-colour
/// outline, stadium.
struct FmOutlinedButton: View {
    @Environment(\.fmPalette) private var fm

    private let label: String
    private let icon: String?
    private let foreground: Color?
    private let minHeight: CGFloat
    private let radius: CGFloat
    private let fillWidth: Bool
    private let role: FmTextRole
    private let busy: Bool?
    private let onPressed: (() -> Void)?
    @State private var tap: FmBusyTap = FmBusyTap()

    init(
        _ label: String,
        icon: String? = nil,
        foreground: Color? = nil,
        minHeight: CGFloat = 40,
        radius: CGFloat = FmRadius.full,
        fillWidth: Bool = false,
        role: FmTextRole = .labelLarge,
        busy: Bool? = nil,
        onPressed: (() -> Void)?
    ) {
        self.label = label
        self.icon = icon
        self.foreground = foreground
        self.minHeight = minHeight
        self.radius = radius
        self.fillWidth = fillWidth
        self.role = role
        self.busy = busy
        self.onPressed = onPressed
    }

    private var working: Bool {
        return busy ?? tap.busy
    }

    private var labelColor: Color {
        if onPressed == nil && !working {
            return fm.textPrimary.opacity(0.38)
        }
        return foreground ?? fm.textPrimary
    }

    private func press() {
        guard let onPressed = onPressed, !working else {
            return
        }
        tap.mark()
        onPressed()
    }

    var body: some View {
        Button(action: press) {
            FmButtonLabel(label: label, icon: icon, color: labelColor, role: role, busy: working)
                .padding(.horizontal, FmSpace.x5)
                .frame(maxWidth: fillWidth ? CGFloat.infinity : nil, minHeight: minHeight)
                .overlay {
                    FmRoundedShape(radius: radius).strokeBorder(fm.border, lineWidth: 1)
                }
                .fmBusyRing(working && icon == nil, corner: radius)
        }
        .buttonStyle(FmMaterialButtonStyle(highlight: labelColor.opacity(0.08), radius: radius))
        .disabled(onPressed == nil || working)
        .fmBusyTracking($tap, enabled: onPressed != nil)
    }
}

/// The themed FloatingActionButton: accent fill, white glyph,
/// 14pt corners, a soft shadow. With a `label` it is the extended form.
/// Position it yourself with `.safeAreaInset(edge: .bottom, alignment:
/// .trailing, spacing: 0)` and 16pt of padding, so lists scroll clear of it.
struct FmFloatingActionButton: View {
    @Environment(\.fmPalette) private var fm

    private let icon: String
    private let label: String?
    private let accessibilityName: String
    private let busy: Bool
    private let onPressed: () -> Void

    init(
        icon: String, label: String? = nil, accessibilityLabel: String, busy: Bool = false,
        onPressed: @escaping () -> Void
    ) {
        self.icon = icon
        self.label = label
        self.accessibilityName = accessibilityLabel
        self.busy = busy
        self.onPressed = onPressed
    }

    private var face: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon(icon, size: 24, color: Color.white)
                .fmBusyRing(busy, size: 34, color: Color.white)
            if let label = label {
                Text(label)
                    .fmText(.labelLarge, color: Color.white)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, label == nil ? 0 : FmSpace.x4)
        .frame(minWidth: 56, minHeight: 56)
        .background(fm.accent)
        .clipShape(FmRoundedShape(radius: 14))
        .shadow(color: Color.black.opacity(0.25), radius: 3, x: 0, y: 2)
    }

    var body: some View {
        Button(action: {
            FmHaptics.tap()
            onPressed()
        }) {
            face
        }
        .buttonStyle(FmMaterialButtonStyle(highlight: Color.white.opacity(0.16), radius: 14))
        .disabled(busy)
        .accessibilityLabel(Text(accessibilityName))
    }
}

/// CircularProgressIndicator under `progressIndicatorTheme`: accent on a
/// transparent track, 36pt and a 4pt stroke unless sized. With a `value`
/// (0...1) it is determinate; without, it spins.
struct FmCircularProgressIndicator: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.shellTabHidden) private var tabHidden

    private let value: Double?
    private let size: CGFloat
    private let strokeWidth: CGFloat
    private let color: Color?

    init(value: Double? = nil, size: CGFloat = 36, strokeWidth: CGFloat = 4, color: Color? = nil) {
        self.value = value
        self.size = size
        self.strokeWidth = strokeWidth
        self.color = color
    }

    private var sweep: CGFloat {
        guard let value = value else {
            return 0.72
        }
        return CGFloat(min(max(value, 0), 1))
    }

    private var arc: some View {
        Circle()
            .trim(from: 0, to: sweep)
            .stroke(color ?? fm.accent, style: StrokeStyle(lineWidth: strokeWidth, lineCap: .butt))
            .rotationEffect(Angle.degrees(-90))
            .padding(strokeWidth / 2)
            .frame(width: size, height: size)
    }

    var body: some View {
        if value == nil {
            TimelineView(.animation(minimumInterval: nil, paused: tabHidden)) { (context: TimelineViewDefaultContext) in
                arc
                    .rotationEffect(Angle.degrees(context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1) * 360))
            }
            .accessibilityLabel(Text(verbatim: "Loading"))
        } else {
            arc
                .accessibilityLabel(Text(verbatim: "Progress"))
                .accessibilityValue(Text(verbatim: "\(Int(sweep * 100)) percent"))
        }
    }
}

/// LinearProgressIndicator under `progressIndicatorTheme`: full width, 4pt,
/// accent on a bgOverlay track. With a `value` (0...1) it is determinate;
/// without, a segment sweeps across.
struct FmLinearProgressIndicator: View {
    @Environment(\.fmPalette) private var fm

    private let value: Double?
    private let color: Color?
    private let height: CGFloat
    private let period: Double

    init(value: Double? = nil, color: Color? = nil, height: CGFloat = 4, period: Double = 1.4) {
        self.value = value
        self.color = color
        self.height = height
        self.period = period
    }

    private func bar(width: CGFloat, at date: Date) -> some View {
        let fraction: CGFloat = CGFloat(min(max(value ?? 0.4, 0), 1))
        let barWidth: CGFloat = width * fraction
        let travel: CGFloat = width + barWidth
        let progress: CGFloat = CGFloat(date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: period) / period)
        let offset: CGFloat = value == nil ? -barWidth + travel * progress : 0
        return Rectangle()
            .fill(color ?? fm.accent)
            .frame(width: barWidth, height: height)
            .offset(x: offset, y: 0)
            .frame(width: width, height: height, alignment: .leading)
    }

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            TimelineView(.animation(paused: value != nil)) { (context: TimelineViewDefaultContext) in
                bar(width: proxy.size.width, at: context.date)
            }
        }
        .frame(height: height)
        .background(fm.bgOverlay)
        .clipped()
        .accessibilityLabel(Text(verbatim: value == nil ? "Loading" : "Progress"))
    }
}

/// TextField under `inputDecorationTheme`: filled with the raised layer, a
/// 12pt outline in the border colour that turns accent (1.2pt) on focus, 16/12
/// content padding, bodyLarge text, the hint in tertiary. There is no floating
/// label: labels live above the field, in FmField.
///
/// Keyboard and capitalisation options are SwiftUI's own modifiers applied to
/// this view (`.keyboardType`, `.textInputAutocapitalization`,
/// `.autocorrectionDisabled`, `.submitLabel`); they reach the field inside.
struct FmTextField: View {
    @Environment(\.fmPalette) private var fm
    @FocusState private var focused: Bool

    @Binding private var text: String
    private let hint: String
    private let secure: Bool
    private let multiline: Bool
    private let mono: Bool
    private let autofocus: Bool
    private let onSubmit: (() -> Void)?

    init(
        text: Binding<String>,
        hint: String = "",
        secure: Bool = false,
        multiline: Bool = false,
        mono: Bool = false,
        autofocus: Bool = false,
        onSubmit: (() -> Void)? = nil
    ) {
        self._text = text
        self.hint = hint
        self.secure = secure
        self.multiline = multiline
        self.mono = mono
        self.autofocus = autofocus
        self.onSubmit = onSubmit
    }

    private var prompt: Text {
        return Text(hint).foregroundColor(fm.textTertiary)
    }

    @ViewBuilder private var field: some View {
        if secure {
            SecureField("", text: $text, prompt: prompt)
        } else if multiline {
            TextField("", text: $text, prompt: prompt, axis: .vertical)
                .lineLimit(1...8)
        } else {
            TextField("", text: $text, prompt: prompt)
        }
    }

    @ViewBuilder private var styledField: some View {
        if mono {
            field.fmMono(size: FmType.small)
        } else {
            field.fmText(.bodyLarge)
        }
    }

    private var outline: some View {
        FmRoundedShape(radius: FmRadius.lg)
            .strokeBorder(focused ? fm.accent : fm.border, lineWidth: focused ? 1.2 : 1)
    }

    var body: some View {
        styledField
            .focused($focused)
            .tint(fm.accent)
            .onSubmit {
                onSubmit?()
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x3)
            .background(fm.bgRaised)
            .clipShape(FmRoundedShape(radius: FmRadius.lg))
            .overlay { outline }
            .contentShape(Rectangle())
            .onTapGesture {
                focused = true
            }
            .onAppear {
                if autofocus {
                    focused = true
                }
            }
    }
}
