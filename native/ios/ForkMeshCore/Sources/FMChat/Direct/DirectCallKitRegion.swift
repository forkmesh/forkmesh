import Foundation

public enum DirectCallKitRegion {
    public static let blockedStorefronts: Set<String> = ["CHN"]
    public static let blockedRegions: Set<String> = ["CN"]

    public static func allowed(storefront: String?, region: String?) -> Bool {
        let store: String = (storefront ?? "").trimmingCharacters(in: .whitespaces).uppercased()
        let place: String = (region ?? "").trimmingCharacters(in: .whitespaces).uppercased()
        return !blockedStorefronts.contains(store) && !blockedRegions.contains(place)
    }
}
