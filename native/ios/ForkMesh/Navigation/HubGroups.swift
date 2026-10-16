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
    private(set) var refreshing = false
    @ObservationIgnored private var epoch = 0

    func reset() {
        epoch &+= 1
        refreshing = false
        installed = []
        adReviewer = false
        OrganizationPackagesLedger.shared.reset()
    }

    func refresh() async {
        guard !refreshing, PackagesFeature.signedIn, let client = PackagesFeature.client else { return }
        let started = epoch
        refreshing = true
        defer { if started == epoch { refreshing = false } }
        async let reviewing: Bool? = try? await client.isAdReviewer()
        let names: [String] = Array(await PackagesFeature.organizationNames().filter(PackagesText.isOrgName).prefix(20))
        if started != epoch { return }
        let read: [OrgPackageState?] = await client.organizationPackages(names)
        let states: [OrgPackageState] = read.compactMap { $0 }
        let complete: Bool = states.count == read.count
        var found: Set<String> = []
        for state in states {
            for id in PackageRoutes.businessPackageIds where state.isInstalled(id) {
                found.insert(id)
            }
        }
        let reviewer: Bool = await reviewing ?? false
        if started != epoch { return }
        installed = found
        adReviewer = reviewer
        if complete { OrganizationPackagesLedger.shared.replace(states) }
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
    static func current(guest: Bool, installed known: Set<String>? = nil) -> [HubGroup] {
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
        let installed: Set<String> = known ?? PackageGateSync.publish()
        let arranged: [HubGroup] = groups.map { (group: HubGroup) -> HubGroup in
            group.id == "business" ? HubGroup(id: group.id, title: group.title, items: business) : group
        }
        return PackageGate.filter(arranged, installed: installed)
    }
}
