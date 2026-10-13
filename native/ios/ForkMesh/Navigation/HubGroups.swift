import FMAccount
import FMApi
import FMBase
import FMPackages
import Observation
import SwiftUI

@MainActor
@Observable
final class HubPackageState {
    static let shared = HubPackageState()

    private(set) var installed: Set<String> = []
    private(set) var adReviewer = false
    @ObservationIgnored private var refreshing = false
    @ObservationIgnored private var epoch = 0

    func reset() {
        epoch &+= 1
        refreshing = false
        installed = []
        adReviewer = false
    }

    func refresh() async {
        guard !refreshing, PackagesFeature.signedIn, let client = PackagesFeature.client else { return }
        let started = epoch
        refreshing = true
        defer { if started == epoch { refreshing = false } }
        var found: Set<String> = []
        for name in await PackagesFeature.organizationNames().filter(PackagesText.isOrgName).prefix(20) {
            if started != epoch { return }
            guard let state = try? await client.organizationPackages(name) else { continue }
            for id in PackageRoutes.businessPackageIds where state.isInstalled(id) {
                found.insert(id)
            }
        }
        let reviewer: Bool = (try? await client.isAdReviewer()) ?? false
        if started != epoch { return }
        installed = found
        adReviewer = reviewer
        PackageGateSync.publish()
    }
}

@MainActor
enum PackageGateSync {
    static var installed: Set<String> {
        PhonePackages.installed(
            account: PackagesFeature.accountPackages, settings: Services.get(SettingsService.self),
            organization: HubPackageState.shared.installed)
    }

    @discardableResult
    static func publish() -> Set<String> {
        let current: Set<String> = installed
        if PackageGate.installed != current { PackageGate.publish(current) }
        return current
    }

    static func isInstalled(_ id: String) -> Bool {
        guard PackageGate.isGated(id) else { return true }
        return publish().contains(id)
    }

    static func setOnDevice(_ id: String, on: Bool) {
        PhonePackages.set(id, on: on)
        publish()
    }
}

@MainActor
enum HubGroups {
    static func current(guest: Bool) -> [HubGroup] {
        let admin: Bool = Services.get(AuthService.self)?.session?.isAdmin ?? false
        let groups: [HubGroup] = HubCatalog.groups(guest: guest, admin: admin) { (kind: Route.Kind) -> Bool in
            AppServices.current?.routes.isRegistered(kind) ?? false
        }
        let state: HubPackageState = HubPackageState.shared
        let business: [HubItem] = PackageRoutes.hubRoutes(installed: state.installed, adReviewer: state.adReviewer).map {
            (entry: (id: String, title: String, route: Route)) -> HubItem in
            HubItem(
                id: entry.id, title: entry.title, symbol: RouteCatalog.symbol(entry.route.kind), action: .route(entry.route))
        }
        let installed: Set<String> = PackageGateSync.publish()
        let arranged: [HubGroup] = groups.map { (group: HubGroup) -> HubGroup in
            group.id == "business" ? HubGroup(id: group.id, title: group.title, items: business) : group
        }
        return PackageGate.filter(arranged, installed: installed)
    }
}
