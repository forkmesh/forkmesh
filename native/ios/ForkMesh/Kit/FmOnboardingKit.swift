import SwiftUI

enum FmOnboarding {
    static let canvas = Color(fmARGB: 0xFF050506)
    static let surface = Color(fmARGB: 0xFF141416)
    static let raised = Color(fmARGB: 0xFF29292C)
    static let hairline = Color.white.opacity(0.09)
    static let secondaryText = Color(fmARGB: 0xFF8D8D94)
    static let exit: Double = 0.16
    static let enter: Double = 0.14
    static let revealDuration: Double = 0.7
}

enum FmPillKind {
    case primary
    case dark
}

struct FmPillPressStyle: ButtonStyle {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let kind: FmPillKind

    func makeBody(configuration: Configuration) -> some View {
        let pressed: Bool = configuration.isPressed && !reduceMotion
        configuration.label
            .scaleEffect(pressed ? 1.03 : 1)
            .shadow(color: Color.white.opacity(pressed && kind == .primary ? 0.34 : 0), radius: 20)
            .animation(FmMotion.easeOutCubic(0.14, reduceMotion: reduceMotion), value: configuration.isPressed)
    }
}

struct FmPillButton: View {
    let label: String
    var kind: FmPillKind = .primary
    let onPressed: (() -> Void)?

    private var fill: some ShapeStyle {
        switch kind {
        case .primary:
            return AnyShapeStyle(
                LinearGradient(
                    stops: [
                        Gradient.Stop(color: Color.white, location: 0),
                        Gradient.Stop(color: Color(fmARGB: 0xFFF4F4F6), location: 0.55),
                        Gradient.Stop(color: Color(fmARGB: 0xFFD3D3D8), location: 1),
                    ],
                    startPoint: UnitPoint.top,
                    endPoint: UnitPoint.bottom))
        case .dark:
            return AnyShapeStyle(FmOnboarding.raised)
        }
    }

    var body: some View {
        Button {
            FmHaptics.tap()
            onPressed?()
        } label: {
            Text(label)
                .fmText(.titleMedium, color: kind == .primary ? Color.black : Color.white, weight: .semibold)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
                .frame(maxWidth: .infinity, minHeight: 58)
                .background(fill, in: Capsule())
                .overlay {
                    Capsule().strokeBorder(Color.white.opacity(kind == .primary ? 0.6 : 0.05), lineWidth: 1)
                }
                .contentShape(Capsule())
        }
        .buttonStyle(FmPillPressStyle(kind: kind))
        .opacity(onPressed == nil ? 0.55 : 1)
        .disabled(onPressed == nil)
    }
}

struct FmPillTextButton: View {
    let label: String
    var color: Color = Color.white
    let onPressed: () -> Void

    var body: some View {
        Button {
            FmHaptics.tap()
            onPressed()
        } label: {
            Text(label)
                .fmText(.labelLarge, color: color, weight: .medium)
                .frame(minHeight: 44)
                .padding(.horizontal, FmSpace.x3)
                .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: Color.white.opacity(0.08), cornerRadius: 22))
    }
}

struct FmPillField<Field: View>: View {
    let label: String
    let focused: Bool
    let field: Field

    init(label: String, focused: Bool, @ViewBuilder field: () -> Field) {
        self.label = label
        self.focused = focused
        self.field = field()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(label)
                .fmText(.labelLarge, color: Color.white, weight: .medium)
                .accessibilityHidden(true)
            field
                .fmText(.bodyLarge, color: Color.white)
                .tint(Color.white)
                .padding(.horizontal, 20)
                .frame(minHeight: 58)
                .background(FmOnboarding.surface.opacity(0.92), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
                .overlay {
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .strokeBorder(focused ? Color.white.opacity(0.42) : FmOnboarding.hairline, lineWidth: 1)
                }
                .animation(.easeOut(duration: 0.16), value: focused)
                .accessibilityLabel(Text(label))
        }
    }
}

extension Text {
    func fmPlaceholder() -> Text {
        foregroundColor(Color(fmARGB: 0xFF6E6E75))
    }
}

struct FmRevealLine: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let text: String
    var role: FmTextRole = .displaySmall
    var color: Color = Color.white
    var delay: Double = 0
    var duration: Double = FmOnboarding.revealDuration
    @State private var shown = false

    private var wipe: some View {
        GeometryReader { geometry in
            let width: CGFloat = geometry.size.width * 1.7
            LinearGradient(
                stops: [
                    Gradient.Stop(color: Color.black, location: 0),
                    Gradient.Stop(color: Color.black, location: 0.62),
                    Gradient.Stop(color: Color.clear, location: 1),
                ],
                startPoint: UnitPoint.leading,
                endPoint: UnitPoint.trailing)
                .frame(width: width, height: geometry.size.height)
                .offset(x: shown ? 0 : -width)
        }
    }

    var body: some View {
        Text(text)
            .fmText(role, color: color)
            .mask { wipe }
            .offset(y: shown ? 0 : 12)
            .rotationEffect(Angle.degrees(shown ? 0 : 1.8), anchor: UnitPoint.leading)
            .onAppear {
                withAnimation(FmMotion.easeOutCubic(duration, reduceMotion: reduceMotion)?.delay(delay)) {
                    shown = true
                }
            }
    }
}

struct FmRevealTitle: View {
    let lines: [String]
    var alignment: HorizontalAlignment = .center
    var role: FmTextRole = .displaySmall
    var delay: Double = 0

    var body: some View {
        VStack(alignment: alignment, spacing: 2) {
            ForEach(Array(lines.enumerated()), id: \.offset) { index, line in
                FmRevealLine(text: line, role: role, delay: delay + Double(index) * 0.16)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(lines.joined(separator: " ")))
        .accessibilityAddTraits(.isHeader)
    }
}

private struct FmRevealModifier: ViewModifier {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let delay: Double
    let rise: CGFloat
    let scale: CGFloat
    let from: Double
    @State private var shown = false

    func body(content: Content) -> some View {
        content
            .opacity(shown ? 1 : from)
            .scaleEffect(shown ? 1 : scale)
            .offset(y: shown ? 0 : rise)
            .onAppear {
                withAnimation(FmMotion.easeOutCubic(0.55, reduceMotion: reduceMotion)?.delay(delay)) {
                    shown = true
                }
            }
    }
}

extension View {
    func fmReveal(delay: Double = 0, rise: CGFloat = 18, scale: CGFloat = 0.97) -> some View {
        modifier(FmRevealModifier(delay: delay, rise: rise, scale: scale, from: 0))
    }

    func fmBrighten(delay: Double = 0) -> some View {
        modifier(FmRevealModifier(delay: delay, rise: 0, scale: 1, from: 0.32))
    }

    func fmForcedDark() -> some View {
        environment(\.fmPalette, FmPalette.darkDefault)
            .environment(\.colorScheme, ColorScheme.dark)
            .preferredColorScheme(ColorScheme.dark)
    }
}

struct FmOnboardingProgress: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let count: Int
    let index: Int

    var body: some View {
        HStack(spacing: 5) {
            ForEach(0..<count, id: \.self) { item in
                Capsule()
                    .fill(item == index ? Color.white : Color.white.opacity(0.3))
                    .frame(width: item == index ? 34 : 7, height: 4)
            }
        }
        .animation(FmMotion.easeOutCubic(0.45, reduceMotion: reduceMotion), value: index)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Step \(index + 1) of \(count)"))
    }
}

struct FmOnboardingBackButton: View {
    let onPressed: () -> Void

    var body: some View {
        Button {
            FmHaptics.tap()
            onPressed()
        } label: {
            Image(systemName: "arrow.left")
                .font(.system(size: 17, weight: .medium))
                .foregroundStyle(Color.white)
                .frame(width: 44, height: 44)
                .background(Color(fmARGB: 0xFF1A1A1D).opacity(0.72), in: Circle())
                .overlay { Circle().strokeBorder(FmOnboarding.hairline, lineWidth: 1) }
                .contentShape(Circle())
        }
        .buttonStyle(FmPillPressStyle(kind: .dark))
        .accessibilityLabel(Text("Back"))
    }
}

struct FmGlassIcon: View {
    let size: CGFloat

    var body: some View {
        let radius: CGFloat = size * 0.27
        let inset: CGFloat = size * 0.11
        ZStack {
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .fill(
                    LinearGradient(
                        colors: [Color.white, Color(fmARGB: 0xFFEDE7FF), Color(fmARGB: 0xFFC9BDF4)],
                        startPoint: UnitPoint.topLeading,
                        endPoint: UnitPoint.bottomTrailing))
                .overlay {
                    RoundedRectangle(cornerRadius: radius, style: .continuous)
                        .strokeBorder(Color.white, lineWidth: 1.5)
                }
            AuthLogo(size: size - inset * 2, shadow: false)
                .overlay {
                    RoundedRectangle(cornerRadius: (size - inset * 2) * 0.24, style: .continuous)
                        .strokeBorder(Color.black.opacity(0.18), lineWidth: 1)
                }
        }
        .frame(width: size, height: size)
        .shadow(color: Color(fmARGB: 0xFF8A5CFF).opacity(0.55), radius: size * 0.2, y: size * 0.06)
        .accessibilityHidden(true)
    }
}

struct FmAgentOrb: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let size: CGFloat
    let colors: [Color]
    @State private var turned = false
    @State private var blink = false

    private var eyes: some View {
        HStack(spacing: size * 0.09) {
            ForEach(0..<2, id: \.self) { _ in
                Capsule()
                    .fill(Color.white)
                    .frame(width: size * 0.17, height: size * 0.105)
                    .scaleEffect(x: 1, y: blink ? 0.14 : 1)
            }
        }
        .offset(y: -size * 0.05)
    }

    var body: some View {
        ZStack {
            Circle()
                .fill(AngularGradient(colors: colors + [colors[0]], center: UnitPoint.center))
                .hueRotation(Angle.degrees(turned ? 34 : 0))
                .rotationEffect(Angle.degrees(turned ? 120 : 0))
            Circle()
                .fill(
                    RadialGradient(
                        stops: [
                            Gradient.Stop(color: Color.black.opacity(0.9), location: 0),
                            Gradient.Stop(color: Color.black.opacity(0.62), location: 0.55),
                            Gradient.Stop(color: Color.clear, location: 1),
                        ],
                        center: UnitPoint(x: 0.5, y: 0.42),
                        startRadius: 0,
                        endRadius: size * 0.52))
            Circle()
                .strokeBorder(Color.white.opacity(0.26), lineWidth: 1)
            eyes
        }
        .frame(width: size, height: size)
        .shadow(color: colors[0].opacity(0.32), radius: size * 0.16)
        .accessibilityHidden(true)
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.easeInOut(duration: 5).repeatForever(autoreverses: true)) { turned = true }
        }
        .task {
            guard !reduceMotion else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(2.8))
                withAnimation(.easeInOut(duration: 0.08)) { blink = true }
                try? await Task.sleep(for: .seconds(0.13))
                withAnimation(.easeInOut(duration: 0.1)) { blink = false }
            }
        }
    }
}

struct FmWorldDots: View {
    private static let columns: Int = 80
    private static let land: [[Int]] = [
        [16, 22, 29, 36, 55, 60],
        [12, 24, 27, 37, 52, 64],
        [8, 26, 28, 36, 44, 46, 50, 72],
        [4, 25, 30, 36, 42, 75],
        [3, 23, 26, 28, 31, 34, 35, 36, 41, 76],
        [4, 19, 24, 28, 32, 33, 40, 74],
        [6, 20, 23, 28, 38, 38, 40, 44, 46, 72],
        [9, 21, 24, 26, 37, 39, 41, 70],
        [10, 24, 25, 27, 40, 70, 72, 72],
        [12, 26, 39, 68],
        [12, 24, 40, 67, 71, 71],
        [13, 23, 38, 41, 43, 44, 45, 66, 70, 71],
        [13, 22, 38, 40, 44, 44, 46, 67, 68, 68, 70, 70],
        [14, 21, 37, 40, 42, 42, 48, 67],
        [15, 20, 37, 52, 54, 68],
        [16, 20, 36, 53, 56, 66],
        [17, 19, 21, 23, 36, 54, 55, 66, 67, 67],
        [18, 21, 24, 24, 36, 54, 56, 60, 62, 65],
        [19, 22, 36, 53, 57, 59, 62, 64, 68, 68],
        [21, 23, 36, 51, 57, 58, 63, 65, 68, 68],
        [23, 30, 37, 52, 58, 58, 64, 64],
        [24, 31, 38, 52, 64, 64, 67, 69],
        [24, 33, 41, 51, 63, 65, 66, 69],
        [24, 34, 42, 50, 64, 65, 67, 70],
        [24, 35, 43, 50, 70, 70, 73, 76],
        [25, 36, 43, 49, 66, 68, 72, 77],
        [26, 36, 44, 49],
        [26, 35, 44, 49, 51, 51, 68, 70, 72, 73],
        [27, 34, 44, 48, 50, 51, 66, 74],
        [28, 33, 45, 48, 50, 51, 65, 75],
        [28, 32, 45, 47, 50, 50, 65, 75],
        [28, 32, 45, 47, 65, 75],
        [28, 31, 46, 46, 66, 74],
        [28, 30, 68, 69, 71, 74],
        [27, 29, 72, 73, 78, 78],
        [27, 28, 73, 73, 77, 77],
        [27, 28],
        [27, 27],
        [27, 28],
    ]

    var body: some View {
        Canvas { context, size in
            let step: CGFloat = size.width / CGFloat(Self.columns)
            let rowStep: CGFloat = step * 1.3
            let dot: CGFloat = step * 0.62
            var path = Path()
            for (row, spans) in Self.land.enumerated() {
                let shift: CGFloat = row.isMultiple(of: 2) ? 0 : step / 2
                var index = 0
                while index + 1 < spans.count {
                    for column in spans[index]...spans[index + 1] {
                        let rect = CGRect(
                            x: CGFloat(column) * step + shift, y: CGFloat(row) * rowStep, width: dot, height: dot)
                        path.addEllipse(in: rect)
                    }
                    index += 2
                }
            }
            context.fill(path, with: GraphicsContext.Shading.color(Color.white.opacity(0.3)))
        }
        .accessibilityHidden(true)
    }
}

struct FmAuroraBackground: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var breathing = false

    private var sky: some View {
        LinearGradient(
            stops: [
                Gradient.Stop(color: Color(fmARGB: 0xFF1F24E8), location: 0),
                Gradient.Stop(color: Color(fmARGB: 0xFF4A2CF0), location: 0.1),
                Gradient.Stop(color: Color(fmARGB: 0xFF9A3DE6), location: 0.2),
                Gradient.Stop(color: Color(fmARGB: 0xFFD768C8), location: 0.27),
                Gradient.Stop(color: Color(fmARGB: 0xFFF2A38C), location: 0.33),
                Gradient.Stop(color: Color(fmARGB: 0xFF9A3A97), location: 0.4),
                Gradient.Stop(color: Color(fmARGB: 0xFF3B124F), location: 0.47),
                Gradient.Stop(color: Color(fmARGB: 0xFF0B0410), location: 0.55),
                Gradient.Stop(color: Color.black, location: 0.62),
            ],
            startPoint: UnitPoint.top,
            endPoint: UnitPoint.bottom)
    }

    private var edges: some View {
        ZStack {
            RadialGradient(
                colors: [Color(fmARGB: 0xFF1422D8).opacity(0.85), Color.clear],
                center: UnitPoint(x: 0.92, y: 0.04),
                startRadius: 0,
                endRadius: 320)
            RadialGradient(
                colors: [Color(fmARGB: 0xFF1422D8).opacity(0.7), Color.clear],
                center: UnitPoint(x: 0.06, y: 0.02),
                startRadius: 0,
                endRadius: 260)
            RadialGradient(
                colors: [Color.black.opacity(0.62), Color.clear],
                center: UnitPoint(x: 1.05, y: 0.34),
                startRadius: 0,
                endRadius: 240)
        }
    }

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .top) {
                Color.black
                sky
                edges
                FmWorldDots()
                    .frame(width: geometry.size.width, height: geometry.size.width * 0.82)
                    .mask {
                        LinearGradient(
                            stops: [
                                Gradient.Stop(color: Color.clear, location: 0.1),
                                Gradient.Stop(color: Color.black, location: 0.3),
                                Gradient.Stop(color: Color.black, location: 0.56),
                                Gradient.Stop(color: Color.clear, location: 0.72),
                            ],
                            startPoint: UnitPoint.top,
                            endPoint: UnitPoint.bottom)
                    }
                    .padding(.top, geometry.size.height * 0.055)
                    .opacity(breathing ? 1 : 0.72)
            }
        }
        .ignoresSafeArea()
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.easeInOut(duration: 3.6).repeatForever(autoreverses: true)) { breathing = true }
        }
    }
}
