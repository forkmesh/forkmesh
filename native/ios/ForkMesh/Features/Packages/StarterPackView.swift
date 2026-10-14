import FMAccount
import FMApi
import FMBase
import FMPackages
import SwiftUI

struct StarterPackView: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var selection: StarterPack = .fallback
    let settings: SettingsService

    private func choose() {
        PhonePackages.chooseStarterPack(selection, settings: settings)
        PackageGateSync.publish()
    }

    private func option(_ pack: StarterPack) -> some View {
        let selected = pack == selection
        return Button {
            withAnimation(FmMotion.easeOutCubic(FmMotion.fast, reduceMotion: reduceMotion)) { selection = pack }
        } label: {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .font(.title3)
                    .foregroundStyle(selected ? fm.accent : fm.textTertiary)
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text(pack.label)
                        .fmText(.titleSmall)
                    Text(pack.summary)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
            .padding(FmSpace.x4)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(selected ? fm.accentSubtle : fm.bgRaised)
            .overlay {
                RoundedRectangle(cornerRadius: FmRadius.lg)
                    .strokeBorder(selected ? fm.accent : fm.border, lineWidth: selected ? 1.5 : 1)
            }
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg))
            .contentShape(RoundedRectangle(cornerRadius: FmRadius.lg))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isSelected] : [])
        .accessibilityIdentifier("starter-pack-\(pack.rawValue)")
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x5) {
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        Text("Choose how to start")
                            .fmText(.displaySmall)
                        Text("Pick what this project starts with. You can add or remove any package later in the Market.")
                            .fmText(.bodyMedium, color: fm.textSecondary)
                    }
                    VStack(spacing: FmSpace.x3) {
                        ForEach(StarterPack.allCases, id: \.self) { pack in option(pack) }
                    }
                }
                .frame(maxWidth: 520, alignment: .leading)
                .padding(.horizontal, FmSpace.x5)
                .padding(.top, FmSpace.x7)
                .frame(maxWidth: .infinity)
            }
            FmFilledButton("Continue", minHeight: 52, fillWidth: true, onPressed: choose)
                .accessibilityIdentifier("starter-pack-continue")
                .frame(maxWidth: 520)
                .padding(.horizontal, FmSpace.x5)
                .padding(.vertical, FmSpace.x4)
        }
        .background(fm.canvas.ignoresSafeArea())
    }
}
