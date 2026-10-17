import Foundation

public enum DeviceHeat: Int, Comparable, Sendable {
    case nominal, fair, serious, critical

    public static func < (left: DeviceHeat, right: DeviceHeat) -> Bool { left.rawValue < right.rawValue }
}

public enum NavigatorCountPlan {
    public static let width = 3
    public static let calmFreshFor: TimeInterval = 60
    public static let warmFreshFor: TimeInterval = 300

    public static func freshFor(heat: DeviceHeat, lowPower: Bool) -> TimeInterval? {
        switch heat {
        case .critical: return nil
        case .serious: return warmFreshFor
        case .nominal, .fair: return lowPower ? warmFreshFor : calmFreshFor
        }
    }

    public static func due(loadedAt: Date?, now: Date, heat: DeviceHeat, lowPower: Bool, force: Bool = false) -> Bool {
        guard let fresh = freshFor(heat: heat, lowPower: lowPower) else { return false }
        guard !force, let loadedAt else { return true }
        return now.timeIntervalSince(loadedAt) >= fresh
    }

    public static func shownKinds(_ groups: [HubGroup]) -> Set<Route.Kind> {
        var kinds: Set<Route.Kind> = [.repos]
        for group in groups {
            for item in group.items {
                if let route = item.route { kinds.insert(route.kind) }
            }
        }
        return kinds
    }

    public static func wanted(_ fetchable: [Route.Kind], shown: Set<Route.Kind>) -> [Route.Kind] {
        fetchable.filter(shown.contains)
    }

    public static func run(
        _ kinds: [Route.Kind], width: Int = NavigatorCountPlan.width,
        _ load: @escaping @MainActor @Sendable (Route.Kind) async -> Int?
    ) async -> [Route.Kind: Int] {
        let counts: [Int?] = await ApiService.parallelMap(kinds, width: width, load)
        var out: [Route.Kind: Int] = [:]
        for (kind, count) in zip(kinds, counts) {
            if let count { out[kind] = count }
        }
        return out
    }
}
