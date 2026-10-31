import FMApi
import FMBase
import SwiftUI

private extension StarterPack {
    var icon: String {
        switch self {
        case .minimal: return "chevron.left.forwardslash.chevron.right"
        case .standard: return "sparkles"
        case .everything: return "square.grid.2x2.fill"
        }
    }

    var orb: [Color] {
        switch self {
        case .minimal:
            return [Color(fmARGB: 0xFF27C76F), Color(fmARGB: 0xFF0B6B46), Color(fmARGB: 0xFF1456E0), Color(fmARGB: 0xFF0A2BC2)]
        case .standard:
            return [Color(fmARGB: 0xFF2F6BFF), Color(fmARGB: 0xFF16C8A8), Color(fmARGB: 0xFF0C2CD6), Color(fmARGB: 0xFF6B3DF2)]
        case .everything:
            return [Color(fmARGB: 0xFFFF5A2A), Color(fmARGB: 0xFFD8146A), Color(fmARGB: 0xFF7A1FE0), Color(fmARGB: 0xFFFFA42E)]
        }
    }
}

struct StarterPackPage: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Binding var selection: StarterPack
    let onContinue: () -> Void
    @GestureState private var drag: CGFloat = 0

    private static let cardWidth: CGFloat = 214
    private static let cardHeight: CGFloat = 238
    private static let gap: CGFloat = 16
    private var enter: Double { FmOnboarding.enter }
    private var packs: [StarterPack] { StarterPack.allCases }
    private var selectedIndex: Int { packs.firstIndex(of: selection) ?? 0 }

    private func select(_ pack: StarterPack) {
        guard pack != selection else { return }
        FmHaptics.tap()
        withAnimation(FmMotion.easeOutCubic(0.32, reduceMotion: reduceMotion)) { selection = pack }
    }

    private func chip(_ pack: StarterPack, index: Int) -> some View {
        let selected: Bool = pack == selection
        return Button { select(pack) } label: {
            HStack(spacing: 6) {
                Image(systemName: pack.icon)
                    .font(.system(size: 11, weight: .semibold))
                Text(pack.label)
                    .fmText(.labelMedium, color: selected ? Color.white : FmOnboarding.secondaryText)
            }
            .foregroundStyle(selected ? Color.white : FmOnboarding.secondaryText)
            .padding(.horizontal, 13)
            .frame(height: 36)
            .background(selected ? FmOnboarding.raised : FmOnboarding.surface, in: Capsule())
            .overlay { Capsule().strokeBorder(FmOnboarding.hairline, lineWidth: 1) }
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isSelected] : [])
        .fmReveal(delay: enter + 0.3 + Double(index) * 0.09, rise: 0, scale: 0.94)
    }

    private func card(_ pack: StarterPack) -> some View {
        let selected: Bool = pack == selection
        return VStack(spacing: FmSpace.x3) {
            FmAgentOrb(size: 112, colors: pack.orb)
                .frame(maxWidth: .infinity, minHeight: 172)
                .background(Color.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 24, style: .continuous))
                .overlay {
                    RoundedRectangle(cornerRadius: 24, style: .continuous)
                        .strokeBorder(FmOnboarding.hairline, lineWidth: 1)
                }
            HStack(spacing: 6) {
                Image(systemName: pack.icon)
                    .font(.system(size: 11, weight: .semibold))
                Text(pack.label)
                    .fmText(.labelMedium, color: FmOnboarding.secondaryText)
            }
            .foregroundStyle(FmOnboarding.secondaryText)
            .padding(.horizontal, 12)
            .frame(height: 30)
            .background(Color.white.opacity(0.05), in: Capsule())
            .overlay { Capsule().strokeBorder(FmOnboarding.hairline, lineWidth: 1) }
        }
        .padding(EdgeInsets(top: 8, leading: 8, bottom: 12, trailing: 8))
        .frame(width: Self.cardWidth)
        .background(FmOnboarding.surface, in: RoundedRectangle(cornerRadius: 30, style: .continuous))
        .scaleEffect(selected ? 1 : 0.88)
        .opacity(selected ? 1 : 0.42)
        .onTapGesture { select(pack) }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("\(pack.label). \(pack.summary)"))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
        .accessibilityAction { select(pack) }
        .accessibilityIdentifier("starter-pack-\(pack.rawValue)")
    }

    private var carousel: some View {
        Color.clear
            .frame(maxWidth: .infinity, minHeight: Self.cardHeight, maxHeight: Self.cardHeight)
            .overlay {
                HStack(spacing: Self.gap) {
                    ForEach(packs, id: \.self) { pack in card(pack) }
                }
                .fixedSize()
                .offset(x: (CGFloat(packs.count - 1) / 2 - CGFloat(selectedIndex)) * (Self.cardWidth + Self.gap) + drag)
            }
            .clipped()
            .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 12)
                .updating($drag) { value, state, _ in state = value.translation.width * 0.6 }
                .onEnded { value in
                    let shift: Int = value.translation.width < -40 ? 1 : (value.translation.width > 40 ? -1 : 0)
                    let next: Int = min(max(selectedIndex + shift, 0), packs.count - 1)
                    select(packs[next])
                })
    }

    private var dots: some View {
        HStack(spacing: 4) {
            ForEach(packs, id: \.self) { pack in
                Capsule()
                    .fill(pack == selection ? Color.white : Color.white.opacity(0.28))
                    .frame(width: pack == selection ? 14 : 5, height: 4)
            }
        }
        .accessibilityHidden(true)
    }

    private var title: some View {
        VStack(alignment: .leading, spacing: 2) {
            FmRevealLine(text: "Choose how", delay: enter)
            HStack(spacing: 7) {
                FmRevealLine(text: "to start", delay: enter + 0.16)
                FmAgentOrb(size: 26, colors: StarterPack.standard.orb)
                    .fmReveal(delay: enter + 0.34, rise: 6, scale: 0.3)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Choose how to start"))
        .accessibilityAddTraits(.isHeader)
    }

    var body: some View {
        VStack(spacing: 0) {
            title
                .padding(.horizontal, 28)
                .padding(.top, FmSpace.x3)
            HStack(spacing: FmSpace.x2) {
                ForEach(Array(packs.enumerated()), id: \.element) { index, pack in chip(pack, index: index) }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 28)
            .padding(.top, FmSpace.x4)
            Spacer(minLength: FmSpace.x3)
            carousel
                .fmReveal(delay: enter + 0.04, rise: 24, scale: 0.95)
            Text(selection.summary)
                .fmText(.bodyMedium, color: Color.white)
                .multilineTextAlignment(.center)
                .frame(minHeight: 44, alignment: .top)
                .padding(.horizontal, 28)
                .padding(.top, FmSpace.x4)
                .id(selection)
                .transition(AnyTransition.opacity)
                .fmReveal(delay: enter + 0.2, rise: 6, scale: 1)
            dots
                .padding(.top, FmSpace.x2)
            Spacer(minLength: FmSpace.x3)
            FmRevealLine(
                text: "Add or remove any package later in the Market.", role: .bodySmall,
                color: FmOnboarding.secondaryText, delay: enter + 0.5, duration: 0.6
            )
            .multilineTextAlignment(.center)
            .padding(.horizontal, 28)
            FmPillButton(label: "Start with \(selection.label)", onPressed: onContinue)
                .accessibilityIdentifier("starter-pack-continue")
                .fmBrighten(delay: enter + 0.1)
                .frame(maxWidth: 520)
                .padding(.horizontal, 28)
                .padding(.top, FmSpace.x3)
                .padding(.bottom, FmSpace.x3)
        }
    }
}
