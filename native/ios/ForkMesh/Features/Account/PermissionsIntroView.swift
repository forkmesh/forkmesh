import FMAccount
import FMBase
import SwiftUI

struct PermissionsIntroPage: View {
    let onContinue: () -> Void
    let onSkip: () -> Void

    private var enter: Double { FmOnboarding.enter }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView(showsIndicators: false) {
                VStack(spacing: 0) {
                    PermissionsHeroCard(enter: enter)
                        .padding(.top, FmSpace.x3)
                        .fmReveal(delay: enter, rise: 26, scale: 0.96)
                    FmRevealTitle(lines: PermissionPacing.titleLines, delay: enter + 0.12)
                        .padding(.top, FmSpace.x5)
                    FmRevealLine(
                        text: PermissionPacing.footnote, role: .bodySmall, color: FmOnboarding.secondaryText,
                        delay: enter + 0.42, duration: 0.6
                    )
                    .multilineTextAlignment(.center)
                    .padding(.top, FmSpace.x3)
                }
                .frame(maxWidth: 520)
                .padding(.horizontal, 28)
                .frame(maxWidth: .infinity)
            }
            VStack(spacing: FmSpace.x1) {
                FmPillButton(label: "Let's Get Started", onPressed: onContinue)
                    .accessibilityIdentifier("permissions-intro-continue")
                    .fmBrighten(delay: enter + 0.1)
                FmPillTextButton(label: "skip and start", onPressed: onSkip)
                    .accessibilityIdentifier("permissions-intro-skip")
                    .fmReveal(delay: enter + 0.6, rise: 0, scale: 1)
            }
            .frame(maxWidth: 520)
            .padding(.horizontal, 28)
            .padding(.top, FmSpace.x3)
        }
    }
}

private struct PermissionsHeroCard: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let enter: Double
    @State private var asked: Int = PermissionPacing.reasons.count
    @State private var floating = false

    private static let band: [Color] = [
        Color(fmARGB: 0xFF5B6CFF), Color(fmARGB: 0xFF8E8BFF), Color(fmARGB: 0xFFC39BF2), Color(fmARGB: 0xFF7FE3D8),
    ]

    private var top: some View {
        HStack(spacing: FmSpace.x2) {
            Image(systemName: "checkmark.shield.fill")
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Color.white)
            FmRevealLine(text: "Asked one at a time", role: .labelLarge, delay: enter + 0.16, duration: 0.5)
            Spacer(minLength: 0)
            Image(systemName: "lock.fill")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Color.white)
                .frame(width: 28, height: 28)
                .background(Color.white.opacity(0.08), in: Circle())
                .overlay { Circle().strokeBorder(FmOnboarding.hairline, lineWidth: 1) }
        }
        .padding(EdgeInsets(top: 12, leading: 14, bottom: 10, trailing: 12))
        .accessibilityHidden(true)
    }

    private var badges: some View {
        HStack(spacing: -7) {
            ForEach(Array(PermissionPacing.reasons.enumerated()), id: \.element.id) { index, item in
                Image(systemName: item.icon)
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(Color.white)
                    .frame(width: 26, height: 26)
                    .background(Color(fmARGB: 0xFF17171A), in: Circle())
                    .overlay { Circle().strokeBorder(Color.white, lineWidth: 1.5) }
                    .fmReveal(delay: enter + 0.26 + Double(index) * 0.1, rise: 0, scale: 0.4)
            }
        }
        .accessibilityHidden(true)
    }

    private var strip: some View {
        HStack(spacing: FmSpace.x2) {
            Text("Only when needed")
                .fmText(.labelMedium, color: Color.white, weight: .medium)
                .lineLimit(1)
            Spacer(minLength: 0)
            badges
        }
        .padding(EdgeInsets(top: 6, leading: 10, bottom: 4, trailing: 6))
    }

    private var counter: some View {
        HStack(spacing: FmSpace.x3) {
            Image(systemName: "hand.raised.fill")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Color.white)
                .frame(width: 40, height: 40)
                .background(Color.white.opacity(0.1), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            VStack(alignment: .leading, spacing: 0) {
                Text("\(asked)")
                    .fmText(.titleLarge, color: Color.white, weight: .semibold, tabular: true)
                    .contentTransition(.numericText(value: Double(asked)))
                Text("Asked up front")
                    .fmText(.labelSmall, color: FmOnboarding.secondaryText)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 66)
        .background(Color.black, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(PermissionPacing.summary))
    }

    private func row(_ item: PermissionReason, index: Int) -> some View {
        HStack(spacing: FmSpace.x3) {
            Image(systemName: item.icon)
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(Color.white)
                .frame(width: 28, height: 28)
                .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 9, style: .continuous))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 0) {
                Text(item.title)
                    .fmText(.labelLarge, color: Color.white, weight: .medium)
                    .lineLimit(1)
                Text(item.hint)
                    .fmText(.labelSmall, color: FmOnboarding.secondaryText)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
        }
        .frame(minHeight: 40)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("\(item.title). \(item.reason)"))
        .accessibilityIdentifier("permissions-intro-\(item.id)")
        .fmReveal(delay: enter + 0.34 + Double(index) * 0.07, rise: 8, scale: 1)
    }

    private var list: some View {
        VStack(spacing: 3) {
            ForEach(Array(PermissionPacing.reasons.enumerated()), id: \.element.id) { index, item in
                row(item, index: index)
            }
        }
        .padding(EdgeInsets(top: 10, leading: 12, bottom: 10, trailing: 12))
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.black, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }

    private var band: some View {
        VStack(spacing: 6) {
            strip
            counter
            list
        }
        .padding(5)
        .background {
            LinearGradient(colors: Self.band, startPoint: UnitPoint.topLeading, endPoint: UnitPoint.bottomTrailing)
                .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            top
            band
        }
        .padding(5)
        .frame(maxWidth: 316)
        .background(FmOnboarding.surface, in: RoundedRectangle(cornerRadius: 27, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 27, style: .continuous)
                .strokeBorder(FmOnboarding.hairline, lineWidth: 1)
        }
        .rotation3DEffect(Angle.degrees(floating ? 2.4 : -2.4), axis: (x: 0.3, y: 1, z: 0), perspective: 0.5)
        .onAppear {
            guard !reduceMotion else {
                asked = 0
                return
            }
            withAnimation(.easeInOut(duration: 3.4).repeatForever(autoreverses: true)) { floating = true }
        }
        .task {
            guard !reduceMotion else { return }
            try? await Task.sleep(for: .seconds(enter + 0.5))
            while asked > 0, !Task.isCancelled {
                withAnimation(.easeOut(duration: 0.18)) { asked -= 1 }
                try? await Task.sleep(for: .seconds(0.17))
            }
        }
    }
}
