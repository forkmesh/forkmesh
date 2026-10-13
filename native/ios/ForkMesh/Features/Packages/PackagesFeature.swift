import FMAccount
import FMApi
import FMBase
import FMPackages
import SwiftUI

@MainActor
enum PackagesFeature {
    static var client: PackagesClient? {
        if let existing = Services.get(PackagesClient.self) { return existing }
        guard let services = AppServices.current else { return nil }
        let client = PackagesClient(transport: ApiPackagesTransport(api: services.api))
        Services.put(client)
        return client
    }

    static var accountPackages: AccountPackagesStore? {
        if let existing = Services.get(AccountPackagesStore.self) { return existing }
        guard let client = client, let settings = Services.get(SettingsService.self) else { return nil }
        let store = AccountPackagesStore(
            client: client, prefs: settings.prefs, signedIn: { PackagesFeature.signedIn },
            legacyProject: { [weak settings] in settings?.legacyProject ?? true },
            projectNamespace: { [weak settings] in settings?.scope.namespace ?? "" })
        store.onChange = { PackageGateSync.publish() }
        Services.put(store)
        PackageGateSync.publish()
        return store
    }

    static var personalLedger: PersonalLedgerStore? {
        if let existing = Services.get(PersonalLedgerStore.self) { return existing }
        guard let services = AppServices.current else { return nil }
        let store = PersonalLedgerStore(store: services.secure)
        Services.put(store)
        return store
    }

    static var signedIn: Bool { AppServices.current?.api.hasSession ?? false }

    static func removeFromAccount(_ packageId: String) async {
        guard signedIn, let store = accountPackages else { return }
        _ = await store.uninstall(packageId, everywhere: true)
        PackageGateSync.publish()
    }

    static func organizationNames() async -> [String] {
        guard let api = AppServices.current?.api else { return [] }
        return ((try? await api.myOrgs()) ?? []).map(\.name)
    }
}

struct PackagesGate<Content: View>: View {
    let feature: String
    @ViewBuilder var content: (PackagesClient) -> Content

    var body: some View {
        if let client = PackagesFeature.client, PackagesFeature.signedIn {
            content(client)
        } else {
            ContentUnavailableView(
                "Sign in to use \(feature)", systemImage: "person.crop.circle.badge.questionmark",
                description: Text("\(feature) keeps its data in your account on the server."))
        }
    }
}

struct PackagesErrorBanner: View {
    let text: String
    var retry: (() async -> Void)?

    var body: some View {
        if !text.isEmpty {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Label(text, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(text)
                Spacer(minLength: 0)
                if let retry {
                    Button("Retry") { Task { await retry() } }
                        .font(.footnote.weight(.semibold))
                }
            }
            .accessibilityElement(children: .combine)
        }
    }
}

struct OrganizationPicker: View {
    let organizations: [PackageOrganization]
    @Binding var selection: String
    let onChange: () async -> Void

    var body: some View {
        if organizations.count > 1 {
            Picker("Organization", selection: $selection) {
                ForEach(organizations) { org in
                    Text(org.installed ? org.name : "\(org.name) (not installed)").tag(org.name)
                }
            }
            .onChange(of: selection) { _, _ in Task { await onChange() } }
        }
    }
}

struct PackageNotInstalledView: View {
    let package: String
    let organizations: [PackageOrganization]

    var body: some View {
        ContentUnavailableView {
            Label("\(package) is not installed", systemImage: "shippingbox")
        } description: {
            Text(
                organizations.isEmpty
                    ? "Join or create an organization, then install \(package) for it."
                    : "An owner or admin installs \(package) for an organization from the Market.")
        } actions: {
            if organizations.isEmpty {
                NavigationLink(value: Route.orgs) {
                    Text("Create an organization")
                }
                .accessibilityIdentifier("package-create-organization")
            }
            NavigationLink(value: Route.market(filter: MarketFilter.notInstalled.rawValue)) {
                Text("Open the Market")
            }
        }
    }
}

enum PackagesFormat {
    static func date(_ ms: Int64) -> String {
        guard ms > 0 else { return "" }
        return Date(timeIntervalSince1970: TimeInterval(ms) / 1000).formatted(date: .abbreviated, time: .omitted)
    }

    static func relative(_ ms: Int64) -> String {
        guard ms > 0 else { return "" }
        return Date(timeIntervalSince1970: TimeInterval(ms) / 1000).formatted(.relative(presentation: .named))
    }

    static func count(_ value: Int64) -> String { value.formatted(.number.notation(.compactName)) }
}
