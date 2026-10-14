import FMAccount
import FMApi
import FMBase

@MainActor
public enum PhonePackages {
    public static let designSystem = "design-system"
    public static let tour = "tour"
    public static let aquarium = "aquarium"
    public static let signIn = "sign-in"
    nonisolated public static let deviceSwitched: Set<String> = ["aquarium", "sign-in"]

    public static func isOn(_ id: String, settings: SettingsService? = Services.get(SettingsService.self)) -> Bool? {
        switch id {
        case aquarium: return settings?.aquariumOn ?? false
        case signIn: return settings?.signInOn ?? true
        default: return nil
        }
    }

    public static func starterPack(settings: SettingsService? = Services.get(SettingsService.self)) -> StarterPack {
        StarterPack.fromName(settings?.starterPack)
    }

    public static func chooseStarterPack(_ pack: StarterPack, settings: SettingsService? = Services.get(SettingsService.self)) {
        settings?.setStarterPack(pack.rawValue)
    }

    public static func isRemoved(_ id: String, settings: SettingsService? = Services.get(SettingsService.self)) -> Bool {
        settings?.removedPackages.contains(id) ?? false
    }

    public static func setRemoved(_ id: String, removed: Bool, settings: SettingsService? = Services.get(SettingsService.self)) {
        settings?.setPackageRemoved(id, removed: removed)
    }

    public static func canRemove(_ package: MarketPackage) -> Bool {
        !package.accountSync && isOn(package.id, settings: nil) == nil && PackageRoutes.route(for: package) != nil
    }

    public static func set(_ id: String, on: Bool, settings: SettingsService? = Services.get(SettingsService.self)) {
        switch id {
        case aquarium: settings?.setAquariumOn(on)
        case signIn: settings?.setSignInOn(on)
        default: return
        }
    }

    public static func installed(
        account: AccountPackagesStore?, settings: SettingsService?, organization: Set<String>
    ) -> Set<String> {
        var out: Set<String> = account?.installedGatedPackages ?? []
        if account == nil, settings?.legacyProject ?? true {
            out = PackageGate.packages.subtracting(PackageGate.organizationPackages)
        } else if account == nil {
            out = starterPack(settings: settings).packages
        }
        for id in PackageGate.devicePackages {
            if isOn(id, settings: settings) ?? true { out.insert(id) } else { out.remove(id) }
        }
        out.formUnion(organization.intersection(PackageGate.organizationPackages))
        out.subtract(settings?.removedPackages ?? [])
        return out
    }

    public static func isInstalled(_ id: String, account: AccountPackagesStore?, settings: SettingsService?) -> Bool {
        if let on = isOn(id, settings: settings) { return on }
        if isRemoved(id, settings: settings) { return false }
        return account?.isInstalled(id) ?? ((settings?.legacyProject ?? true) || PackageGate.isStarter(id, pack: starterPack(settings: settings)))
    }
}
