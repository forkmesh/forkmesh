import FMApi
import FMPackages
import SwiftUI

struct TourStage: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let scene: TourScene
    let title: String
    let active: Bool

    @State private var phase: Int = 0
    @State private var quietUntil: Date = .distantPast

    static let height: CGFloat = 330
    static let beat: Double = 1.9

    var body: some View {
        content
            .frame(maxWidth: .infinity)
            .frame(height: TourStage.height, alignment: .top)
            .background(fm.bgBase)
            .clipShape(RoundedRectangle(cornerRadius: 30, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 30, style: .continuous)
                    .strokeBorder(fm.border, lineWidth: 1)
            }
            .shadow(color: Color.black.opacity(fm.dark ? 0.5 : 0.12), radius: 18, y: 8)
            .padding(.horizontal, FmSpace.x2)
            .accessibilityElement(children: .contain)
            .accessibilityLabel(Text("Preview: \(title)"))
            .task(id: active) { await play() }
            .onChange(of: active) { (_: Bool, now: Bool) in
                if !now { phase = 0 }
            }
    }

    @ViewBuilder private var content: some View {
        let act: (Int) -> Void = { (next: Int) in touch(next) }
        switch scene {
        case .home: TourHomeScene(phase: phase, act: act)
        case .agents: TourAgentsScene(phase: phase, act: act)
        case .newAgent: TourNewAgentScene(phase: phase, act: act)
        case .inbox: TourInboxScene(phase: phase, act: act)
        case .hub: TourHubScene(phase: phase, act: act)
        case .voice: TourVoiceScene(phase: phase, act: act)
        case .code: TourCodeScene(phase: phase, act: act)
        case .palette: TourPaletteScene(phase: phase, act: act)
        case .search: TourSearchScene(phase: phase, act: act)
        case .bar: TourBarScene(phase: phase, act: act)
        case .chat: TourChatScene(phase: phase, act: act)
        case .projects: TourProjectsScene(phase: phase, act: act)
        case .desktops: TourDesktopsScene(phase: phase, act: act)
        case .workspace: TourWorkspaceScene(phase: phase, act: act)
        case .market: TourMarketScene(phase: phase, act: act)
        case .personal: TourPersonalScene(phase: phase, act: act)
        case .system: TourSystemScene(phase: phase, act: act)
        case .looks: TourLooksScene(phase: phase, act: act)
        }
    }

    private func touch(_ next: Int) {
        FmHaptics.tap()
        quietUntil = Date().addingTimeInterval(6)
        withAnimation(reduceMotion ? nil : .snappy(duration: 0.35)) {
            phase = next
        }
    }

    private func play() async {
        guard active else { return }
        while !Task.isCancelled {
            try? await Task.sleep(for: .seconds(TourStage.beat))
            guard !Task.isCancelled else { return }
            if Date() >= quietUntil {
                withAnimation(reduceMotion ? nil : .snappy(duration: 0.45)) {
                    phase += 1
                }
            }
        }
    }
}

struct TourTargetRing: ViewModifier {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let lit: Bool
    let round: Bool

    func body(content: Content) -> some View {
        content.overlay {
            if lit {
                ring
                    .padding(-5)
                    .phaseAnimator([false, true]) { view, on in
                        view
                            .scaleEffect(on && !reduceMotion ? 1.12 : 1)
                            .opacity(on && !reduceMotion ? 0.25 : 0.95)
                    } animation: { _ in
                        .easeInOut(duration: 0.85)
                    }
                    .allowsHitTesting(false)
                    .transition(.opacity)
            }
        }
    }

    private var ring: AnyView {
        if round {
            return AnyView(Circle().stroke(fm.accent, lineWidth: 2.5))
        }
        return AnyView(Capsule().stroke(fm.accent, lineWidth: 2.5))
    }
}

extension View {
    func tourTarget(_ lit: Bool, round: Bool = false) -> some View {
        modifier(TourTargetRing(lit: lit, round: round))
    }

    @ViewBuilder func tourGlass<S: Shape>(_ shape: S, fallback: Color) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.glassEffect(.regular, in: shape)
        } else {
            self.background(fallback, in: shape)
        }
        #else
        self.background(fallback, in: shape)
        #endif
    }
}

struct TourSparkleButton: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let stars: Int
    var size: CGFloat = 36
    var lit: Bool = false

    var body: some View {
        ZStack {
            Circle().fill(
                LinearGradient(
                    colors: [fm.accent.opacity(0.95), fm.accent, fm.accent.opacity(0.7)], startPoint: .top,
                    endPoint: .bottom))
            ShellSparkleField(tilt: .zero, energy: 0.35, paused: reduceMotion, stars: stars)
            Image(systemName: "plus")
                .font(.system(size: size * 0.42, weight: .bold))
                .foregroundStyle(Color.white)
                .shadow(color: Color.black.opacity(0.3), radius: 2, y: 1)
                .symbolEffect(.bounce, value: stars)
        }
        .frame(width: size, height: size)
        .clipShape(Circle())
        .shadow(color: fm.accent.opacity(0.45), radius: 8, y: 3)
        .tourTarget(lit, round: true)
    }
}

struct TourTopBar: View {
    @Environment(\.fmPalette) private var fm

    var project: String = "Personal"
    var stars: Int = 2
    var buttonLit: Bool = false
    var avatarLit: Bool = false
    var projectLit: Bool = false
    var onButton: (() -> Void)? = nil
    var onAvatar: (() -> Void)? = nil
    var onProject: (() -> Void)? = nil

    var body: some View {
        HStack(spacing: 10) {
            Button(action: { onProject?() }) {
                HStack(spacing: 6) {
                    RoundedRectangle(cornerRadius: 7, style: .continuous)
                        .fill(fm.accent.opacity(0.85))
                        .frame(width: 26, height: 26)
                        .overlay {
                            Text(String(project.prefix(1)))
                                .font(.system(size: 13, weight: .bold))
                                .foregroundStyle(Color.white)
                        }
                        .tourTarget(projectLit)
                    Text(project)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                        .contentTransition(.opacity)
                }
            }
            .buttonStyle(.plain)
            .allowsHitTesting(onProject != nil)
            .accessibilityLabel(Text("Project \(project)"))
            Spacer(minLength: 0)
            Image(systemName: "magnifyingglass")
                .font(.system(size: 16, weight: .regular))
                .foregroundStyle(fm.textPrimary)
                .accessibilityHidden(true)
            Button(action: { onButton?() }) {
                TourSparkleButton(stars: stars, lit: buttonLit)
            }
            .buttonStyle(.plain)
            .allowsHitTesting(onButton != nil)
            .accessibilityLabel(Text("New agent"))
            Button(action: { onAvatar?() }) {
                AvatarWithDot(state: .connected, label: "You", size: 30, rounded: true)
                    .tourTarget(avatarLit, round: true)
            }
            .buttonStyle(.plain)
            .allowsHitTesting(onAvatar != nil)
            .accessibilityLabel(Text("Your hub"))
        }
        .padding(.horizontal, 14)
        .padding(.top, 14)
        .padding(.bottom, 6)
    }
}

struct TourBottomBar: View {
    @ScaledMetric(relativeTo: .body) private var iconSize: CGFloat = 17
    var style: ShellBarStyle = ShellBarStyle()

    var body: some View {
        ShellBarPreview(style: style, iconSize: iconSize, cellWidth: 50, cellHeight: 44)
            .padding(.horizontal, 8)
            .padding(.bottom, 8)
            .contentShape(Rectangle())
            .accessibilityHidden(true)
    }
}

struct TourTitle: View {
    @Environment(\.fmPalette) private var fm

    let text: String
    var trailing: String? = nil

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(text)
                .font(.system(size: 24, weight: .bold))
                .foregroundStyle(fm.textPrimary)
            Spacer(minLength: 0)
            if let trailing = trailing {
                Text(trailing)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(Color.white)
                    .padding(.horizontal, 7)
                    .padding(.vertical, 2)
                    .background(fm.accent, in: Capsule())
                    .contentTransition(.numericText())
            }
        }
        .padding(.horizontal, 16)
    }
}

struct TourSectionLabel: View {
    @Environment(\.fmPalette) private var fm

    let text: String

    var body: some View {
        Text(text.uppercased())
            .font(.system(size: 11, weight: .semibold))
            .tracking(0.6)
            .foregroundStyle(fm.textTertiary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 16)
            .padding(.top, 6)
    }
}

struct TourRow<Trailing: View>: View {
    @Environment(\.fmPalette) private var fm

    let symbol: String
    var tint: Color? = nil
    let title: String
    let detail: String
    var unread: Bool = false
    @ViewBuilder var trailing: () -> Trailing

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(tint ?? fm.accent)
                .frame(width: 30, height: 30)
                .background((tint ?? fm.accent).opacity(0.14), in: RoundedRectangle(cornerRadius: 9, style: .continuous))
            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .font(.system(size: 14, weight: unread ? .semibold : .medium))
                    .foregroundStyle(fm.textPrimary)
                    .lineLimit(1)
                Text(detail)
                    .font(.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
            trailing()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .padding(.horizontal, 12)
    }
}

struct TourPill: View {
    @Environment(\.fmPalette) private var fm

    let text: String
    var filled: Bool = true
    var tint: Color? = nil
    var lit: Bool = false

    var body: some View {
        Text(text)
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(filled ? Color.white : (tint ?? fm.accent))
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(filled ? (tint ?? fm.accent) : (tint ?? fm.accent).opacity(0.14), in: Capsule())
            .tourTarget(lit)
    }
}
