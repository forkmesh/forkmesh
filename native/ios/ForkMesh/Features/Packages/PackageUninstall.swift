import FMPackages
import SwiftUI

struct PackageUninstallRequest: Identifiable {
    let packageId: String
    let name: String
    let uninstall: MarketUninstall
    var organization = ""
    var account: AccountPackageAction?

    var id: String { "uninstall-" + packageId + "/" + organization }

    @MainActor
    init(_ package: MarketPackage, _ uninstall: MarketUninstall, organization: String = "", account store: AccountPackagesStore?) {
        packageId = package.id
        name = package.name
        self.uninstall = uninstall
        self.organization = organization
        if uninstall == .account, let store {
            account = AccountPackageAction(
                packageId: package.id, name: package.name, install: false, others: store.dependents(of: package.id).map(\.name),
                project: store.ownsInstalls)
        }
    }

    var title: String {
        switch uninstall {
        case .account: return account?.title ?? "Remove \(name) from all your devices?"
        case .device, .phone: return "Uninstall \(name) from this phone?"
        case .organization: return "Remove \(name) from \(organization)?"
        }
    }

    var message: String {
        switch uninstall {
        case .account:
            return account?.message ?? "It goes from your phone, your desktop and the web."
        case .device:
            if packageId == PhonePackages.signIn {
                return "Your avatar and Sign in leave this phone's top bar and menu. Settings, Account always keeps Sign in and Sign out."
            }
            return "It stays on your other devices. Install it again here from the Market."
        case .phone:
            return "It disappears from your home menu and shortcuts on this phone until you install it again here. Your data is kept."
        case .organization:
            return "Everyone in the organization loses it. Its data is deleted from the server; you get a copy to save right after."
        }
    }

    static func exportFile(_ removal: PackageRemoval) -> URL? {
        guard let data = removal.export else { return nil }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(removal.filename)
        do {
            try data.write(to: url, options: [.atomic, .completeFileProtection])
        } catch {
            return nil
        }
        return url
    }
}

struct PackageUninstallMenuItems: View {
    let package: MarketPackage
    let uninstall: MarketUninstall
    let onChoose: (String) -> Void

    var body: some View {
        if case .organization(let organizations) = uninstall {
            if organizations.count == 1 {
                Button(role: .destructive) { onChoose(organizations[0]) } label: {
                    Label("Uninstall from \(organizations[0])", systemImage: "trash")
                }
            } else {
                Section("Uninstall from") {
                    ForEach(organizations, id: \.self) { (organization: String) in
                        Button(organization, role: .destructive) { onChoose(organization) }
                    }
                }
            }
        } else {
            Button(role: .destructive) { onChoose("") } label: {
                Label(uninstall == .account ? "Uninstall everywhere" : "Uninstall", systemImage: "trash")
            }
        }
    }
}

private struct PackageUninstallFlow: ViewModifier {
    @Binding var request: PackageUninstallRequest?
    let market: MarketModel
    let onRemoved: (PackageRemoval) -> Void

    func body(content: Content) -> some View {
        content.confirmationDialog(
            request?.title ?? "",
            isPresented: Binding(get: { request != nil }, set: { if !$0 { request = nil } }),
            titleVisibility: .visible, presenting: request
        ) { (request: PackageUninstallRequest) in
            Button("Uninstall", role: .destructive) {
                Task { await run(request) }
            }
            Button("Cancel", role: .cancel) {}
        } message: { (request: PackageUninstallRequest) in
            Text(request.message)
        }
    }

    private func run(_ request: PackageUninstallRequest) async {
        switch request.uninstall {
        case .account:
            guard let account = market.account, await account.uninstall(request.packageId) != nil else { return }
            PackageGateSync.publish()
        case .device:
            PackageGateSync.setOnDevice(request.packageId, on: false)
        case .phone:
            PhonePackages.setRemoved(request.packageId, removed: true)
            PackageGateSync.publish()
        case .organization:
            guard let package = market.package(request.packageId),
                  await market.uninstall(package, organization: request.organization) else { return }
            await HubPackageState.shared.refresh()
            if let removal = market.removed { onRemoved(removal) }
        }
        FmHaptics.commit()
    }
}

extension View {
    func packageUninstall(
        _ request: Binding<PackageUninstallRequest?>, market: MarketModel, onRemoved: @escaping (PackageRemoval) -> Void = { _ in }
    ) -> some View {
        modifier(PackageUninstallFlow(request: request, market: market, onRemoved: onRemoved))
    }
}
