import FMAccount
import FMApi
import FMPackages
import SwiftUI

struct OnboardingFlowView: View {
    private enum Step {
        case permissions
        case starterPack
    }

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let settings: SettingsService
    private let steps: [Step]
    @State private var index = 0
    @State private var selection: StarterPack = .fallback

    init(settings: SettingsService) {
        self.settings = settings
        var pending: [Step] = []
        if settings.needsPermissionsIntro { pending.append(.permissions) }
        if settings.needsStarterPack { pending.append(.starterPack) }
        steps = pending
    }

    private func go(to next: Int) {
        withAnimation(FmMotion.easeOutCubic(FmOnboarding.exit, reduceMotion: reduceMotion)) { index = next }
    }

    private func advance() {
        if index + 1 < steps.count {
            go(to: index + 1)
        } else {
            finish(selection)
        }
    }

    private func finish(_ pack: StarterPack) {
        if settings.needsStarterPack {
            PhonePackages.chooseStarterPack(pack, settings: settings)
            PackageGateSync.publish()
        }
        if settings.needsPermissionsIntro {
            settings.finishPermissionsIntro()
        }
    }

    private var header: some View {
        ZStack(alignment: .leading) {
            HStack {
                Spacer(minLength: 0)
                FmPillTextButton(label: "Skip") { finish(.fallback) }
                    .accessibilityIdentifier("onboarding-skip")
            }
            HStack(spacing: 0) {
                FmOnboardingBackButton { go(to: max(index - 1, 0)) }
                    .frame(width: 44, height: 44, alignment: .leading)
                    .frame(width: index > 0 ? 56 : 0, alignment: .leading)
                    .opacity(index > 0 ? 1 : 0)
                    .clipped()
                    .disabled(index == 0)
                    .accessibilityHidden(index == 0)
                FmOnboardingProgress(count: max(steps.count, 1), index: index)
            }
            .frame(minHeight: 44)
            .animation(FmMotion.easeOutCubic(0.45, reduceMotion: reduceMotion), value: index)
        }
        .padding(.horizontal, 20)
        .padding(.top, FmSpace.x2)
    }

    @ViewBuilder private var page: some View {
        if steps.indices.contains(index) {
            switch steps[index] {
            case .permissions:
                PermissionsIntroPage(onContinue: advance, onSkip: { finish(.fallback) })
            case .starterPack:
                StarterPackPage(selection: $selection, onContinue: advance)
            }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            ZStack {
                page
                    .id(index)
                    .transition(
                        AnyTransition.asymmetric(
                            insertion: AnyTransition.opacity.animation(
                                .linear(duration: 0.05).delay(reduceMotion ? 0 : FmOnboarding.exit)),
                            removal: .opacity))
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background {
            ZStack {
                FmOnboarding.canvas
                RadialGradient(
                    colors: [Color.white.opacity(0.035), Color.clear],
                    center: UnitPoint(x: 0.5, y: 1),
                    startRadius: 0,
                    endRadius: 380)
            }
            .ignoresSafeArea()
        }
        .fmForcedDark()
    }
}
