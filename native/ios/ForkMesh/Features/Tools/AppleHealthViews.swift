import FMAccount
import FMTools
import SwiftUI

struct AppleHealthMark: View {
    var size: CGFloat = 22

    var body: some View {
        Image(systemName: "heart.fill")
            .font(Font.system(size: size * 0.55, weight: .semibold))
            .foregroundStyle(Color(red: 1.0, green: 0.18, blue: 0.33))
            .frame(width: size, height: size)
            .background(RoundedRectangle(cornerRadius: size * 0.24, style: .continuous).fill(Color.white))
            .overlay(RoundedRectangle(cornerRadius: size * 0.24, style: .continuous).stroke(Color.black.opacity(0.12), lineWidth: 0.5))
            .accessibilityHidden(true)
    }
}

struct AppleHealthDisclosure: View {
    @Environment(\.fmPalette) private var fm
    let tool: AppleHealthTool
    @State private var showing: Bool = false

    var body: some View {
        let use: AppleHealthUse = AppleHealthGuide.use(tool)
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmHairline()
            HStack(spacing: FmSpace.x2) {
                AppleHealthMark()
                VStack(alignment: .leading, spacing: 1) {
                    Text(AppleHealthGuide.badge)
                        .fmText(.labelLarge)
                    Text(AppleHealthGuide.framework)
                        .fmText(.labelSmall, color: fm.textTertiary)
                }
            }
            .accessibilityElement(children: .combine)
            AppleHealthLine(label: AppleHealthGuide.readsLabel, text: use.reads)
            AppleHealthLine(label: AppleHealthGuide.writesLabel, text: use.writes)
            FmTextButton(AppleHealthGuide.learnMore, onPressed: { showing = true })
                .accessibilityIdentifier("apple-health-learn-more")
        }
        .accessibilityIdentifier("apple-health-disclosure")
        .sheet(isPresented: $showing) {
            AppleHealthGuideSheet()
        }
    }
}

private struct AppleHealthLine: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(label)
                .fmText(.labelSmall, color: fm.textTertiary)
            Text(text)
                .fmText(.bodySmall, color: fm.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .accessibilityElement(children: .combine)
    }
}

struct AppleHealthGuideSheet: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            AppleHealthGuideScreen()
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                    }
                }
        }
        .presentationDetents([.large])
    }
}

struct AppleHealthGuideScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                HStack(spacing: FmSpace.x3) {
                    AppleHealthMark(size: 44)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(AppleHealthGuide.badge)
                            .fmText(.titleMedium)
                        Text(AppleHealthGuide.framework)
                            .fmText(.bodySmall, color: fm.textTertiary)
                    }
                }
                .accessibilityElement(children: .combine)
                Text(AppleHealthGuide.summary)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                FmSectionHeader(title: AppleHealthGuide.toolsHeader)
                ForEach(AppleHealthGuide.uses) { (use: AppleHealthUse) in
                    FmCard {
                        VStack(alignment: .leading, spacing: FmSpace.x2) {
                            Text(use.name)
                                .fmText(.titleSmall)
                            AppleHealthLine(label: AppleHealthGuide.readsLabel, text: use.reads)
                            AppleHealthLine(label: AppleHealthGuide.writesLabel, text: use.writes)
                            AppleHealthLine(label: AppleHealthGuide.accountLabel, text: use.account)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .accessibilityIdentifier("apple-health-use-\(use.id)")
                }
                FmSectionHeader(title: AppleHealthGuide.promisesHeader)
                FmCard {
                    VStack(alignment: .leading, spacing: FmSpace.x3) {
                        ForEach(AppleHealthGuide.promises, id: \.self) { (promise: String) in
                            Text(promise)
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                FmTextButton(AppleHealthGuide.privacyLabel, icon: "open_in_new", onPressed: { openURL(LegalLinks.appleHealth) })
                    .accessibilityIdentifier("apple-health-privacy-link")
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas)
        .navigationTitle(AppleHealthGuide.title)
        .navigationBarTitleDisplayMode(.inline)
        .accessibilityIdentifier("apple-health-guide")
    }
}
