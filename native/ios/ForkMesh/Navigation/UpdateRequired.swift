import FMAccount
import FMApi
import FMBase
import Observation
import SwiftUI

@MainActor
@Observable
final class UpdateRequiredCheck {
    static let shared = UpdateRequiredCheck()

    private(set) var requirement: UpdateRequirement?
    private(set) var available: AvailableUpdate?
    @ObservationIgnored private var checkedOrigin: String = ""
    @ObservationIgnored private var storeCheckedAt: Date?

    var blocksUse: Bool { requirement != nil || available != nil }

    func check() async {
        guard let services = AppServices.current else { return }
        let origin: String = services.settings.relays.active.httpOrigin
        if origin == checkedOrigin { return }
        checkedOrigin = origin
        let running: String = services.config.build.version
        requirement = await ClientVersionGate.check(http: services.http, origin: origin, running: running)
    }

    func checkStore() async {
        guard available == nil, let services = AppServices.current else { return }
        if let last = storeCheckedAt, Date().timeIntervalSince(last) < ClientVersionGate.storeRecheckInterval { return }
        storeCheckedAt = Date()
        let running: String = services.config.build.version
        if let found = await ClientVersionGate.availableUpdate(http: services.http, running: running) {
            available = found
        }
    }
}

struct UpdateRequiredView: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let requirement: UpdateRequirement?
    var available: AvailableUpdate? = nil
    var onClose: (() -> Void)? = nil

    private var summary: String {
        requirement?.summary ?? available?.summary ?? "This version of ForkMesh is no longer supported."
    }

    var body: some View {
        VStack(spacing: FmSpace.x4) {
            Spacer(minLength: 0)
            Image(systemName: "arrow.down.app")
                .font(.system(size: 56, weight: .regular))
                .foregroundStyle(fm.accent)
                .accessibilityHidden(true)
            Text("Update required")
                .fmText(.titleLarge)
                .multilineTextAlignment(.center)
                .accessibilityAddTraits(.isHeader)
            Text(summary)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            Text(UpdateRequirement.advice)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            if let store = requirement?.storeURL ?? available?.storeURL ?? ClientVersionGate.defaultStoreURL {
                FmFilledButton("Open the App Store", icon: "open_in_new", onPressed: { openURL(store) })
                    .accessibilityIdentifier("update-required-store")
            }
            if let onClose = onClose {
                Button("Not now", action: onClose)
                    .frame(minHeight: 44)
            }
            Spacer(minLength: 0)
        }
        .padding(FmSpace.x6)
        .frame(maxWidth: 520)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(fm.canvas.ignoresSafeArea())
        .interactiveDismissDisabled(true)
        .accessibilityIdentifier("update-required")
    }
}
