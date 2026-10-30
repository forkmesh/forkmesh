import FMAccount
import FMBase
import SwiftUI

struct PermissionsIntroView: View {
    @Environment(\.fmPalette) private var fm
    let settings: SettingsService

    private func row(_ item: PermissionReason) -> some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            Image(systemName: item.icon)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(fm.accent)
                .frame(width: 34, height: 34)
                .background(fm.accentSubtle, in: RoundedRectangle(cornerRadius: FmRadius.lg))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(item.title)
                    .fmText(.titleSmall)
                Text(item.reason)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("permissions-intro-\(item.id)")
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x5) {
                    PermissionsIllustration()
                        .frame(maxWidth: .infinity)
                        .padding(.top, FmSpace.x2)
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        Text(PermissionPacing.title)
                            .fmText(.displaySmall)
                        Text(PermissionPacing.summary)
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    VStack(alignment: .leading, spacing: FmSpace.x4) {
                        ForEach(PermissionPacing.reasons) { item in row(item) }
                    }
                    HStack(alignment: .top, spacing: FmSpace.x2) {
                        Image(systemName: "lock.fill")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(fm.textTertiary)
                            .accessibilityHidden(true)
                        Text(PermissionPacing.footnote)
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .frame(maxWidth: 520, alignment: .leading)
                .padding(.horizontal, FmSpace.x5)
                .padding(.top, FmSpace.x6)
                .frame(maxWidth: .infinity)
            }
            FmFilledButton("Sounds good", minHeight: 52, fillWidth: true, onPressed: { settings.finishPermissionsIntro() })
                .accessibilityIdentifier("permissions-intro-continue")
                .frame(maxWidth: 520)
                .padding(.horizontal, FmSpace.x5)
                .padding(.vertical, FmSpace.x4)
        }
        .background(fm.canvas.ignoresSafeArea())
    }
}

private struct PermissionsIllustration: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var floating = false

    private static let badges: [(icon: String, angle: Double)] = [
        ("bell.fill", -150), ("dot.radiowaves.left.and.right", -30), ("video.fill", 30), ("photo.fill", 150),
    ]

    private func badge(_ icon: String, angle: Double, index: Int) -> some View {
        let radians: Double = angle * .pi / 180
        let lift: CGFloat = floating ? (index.isMultiple(of: 2) ? -4 : 4) : 0
        return Image(systemName: icon)
            .font(.system(size: 14, weight: .semibold))
            .foregroundStyle(fm.accent)
            .frame(width: 36, height: 36)
            .background(fm.bgRaised, in: Circle())
            .overlay { Circle().strokeBorder(fm.border, lineWidth: 1) }
            .shadow(color: Color.black.opacity(fm.dark ? 0.3 : 0.08), radius: 6, y: 2)
            .offset(x: CGFloat(cos(radians)) * 82, y: CGFloat(sin(radians)) * 52 + lift)
    }

    var body: some View {
        ZStack {
            Circle()
                .fill(fm.accentSubtle)
                .frame(width: 150, height: 150)
            Ellipse()
                .strokeBorder(fm.accent.opacity(0.18), style: StrokeStyle(lineWidth: 1, dash: [4, 5]))
                .frame(width: 196, height: 128)
            Image(systemName: "checkmark.shield.fill")
                .font(.system(size: 64, weight: .regular))
                .foregroundStyle(fm.accent)
                .scaleEffect(floating ? 1.03 : 1)
            ForEach(Array(Self.badges.enumerated()), id: \.offset) { index, item in
                badge(item.icon, angle: item.angle, index: index)
            }
        }
        .frame(height: 170)
        .accessibilityHidden(true)
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.easeInOut(duration: 2.4).repeatForever(autoreverses: true)) { floating = true }
        }
    }
}
