import Foundation

public enum GlassesMaker: Int, CaseIterable, Sendable {
    case meta = 0x01AB
    case metaTechnologies = 0x058E
    case luxottica = 0x0D53

    public var companyId: Int { rawValue }

    public var label: String {
        switch self {
        case .meta: return "Meta Platforms"
        case .metaTechnologies: return "Meta Platforms Technologies"
        case .luxottica: return "Luxottica (Ray-Ban, Oakley)"
        }
    }
}

public enum GlassesRange: String, CaseIterable, Sendable {
    case near
    case room
    case wide

    public static let defaultRange: GlassesRange = .room

    public static func of(_ wire: String?) -> GlassesRange {
        GlassesRange(rawValue: wire ?? "") ?? defaultRange
    }

    public var label: String {
        switch self {
        case .near: return "Close by"
        case .room: return "Same room"
        case .wide: return "Anywhere in range"
        }
    }

    public var minRssi: Int {
        switch self {
        case .near: return -65
        case .room: return -75
        case .wide: return -90
        }
    }
}

public struct GlassesAdvert: Equatable, Sendable {
    public let identifier: String
    public let name: String?
    public let rssi: Int
    public let companyIds: [Int]
    public let serviceUuids: [Int]
    public let appearance: Int?

    public init(identifier: String, name: String?, rssi: Int, companyIds: [Int], serviceUuids: [Int] = [], appearance: Int? = nil) {
        self.identifier = identifier
        self.name = name
        self.rssi = rssi
        self.companyIds = companyIds
        self.serviceUuids = serviceUuids
        self.appearance = appearance
    }
}

public struct GlassesSighting: Equatable, Identifiable, Sendable {
    public let id: String
    public let label: String
    public let maker: GlassesMaker?
    public let namedAsGlasses: Bool
    public let rssi: Int
    public let seenAt: Date

    public init(id: String, label: String, maker: GlassesMaker?, namedAsGlasses: Bool, rssi: Int, seenAt: Date) {
        self.id = id
        self.label = label
        self.maker = maker
        self.namedAsGlasses = namedAsGlasses
        self.rssi = rssi
        self.seenAt = seenAt
    }

    public var distance: String { GlassesSignatures.distance(rssi) }
}

public enum GlassesSignatures {
    public static let companyIds: Set<Int> = Set(GlassesMaker.allCases.map(\.companyId))
    public static let nameKeywords: [String] = ["ray-ban", "rayban", "ray ban", "oakley meta", "meta ray", "meta glasses"]

    public static func companyId(manufacturerData: [UInt8]) -> Int? {
        guard manufacturerData.count >= 2 else { return nil }
        return Int(manufacturerData[0]) | (Int(manufacturerData[1]) << 8)
    }

    public static func nameMatches(_ name: String?) -> Bool {
        guard let lowered = name?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased(), !lowered.isEmpty else {
            return false
        }
        return nameKeywords.contains { lowered.contains($0) }
    }

    public static func match(_ advert: GlassesAdvert, range: GlassesRange, now: Date) -> GlassesSighting? {
        guard advert.rssi >= range.minRssi, advert.rssi < 0 else { return nil }
        let maker: GlassesMaker? = advert.companyIds.lazy.compactMap { GlassesMaker(rawValue: $0) }.first
        let named: Bool = nameMatches(advert.name)
        guard maker != nil || named else { return nil }
        let label: String
        if named, let name = advert.name {
            label = name.trimmingCharacters(in: .whitespacesAndNewlines)
        } else if maker == .luxottica {
            label = "Ray-Ban or Oakley Meta glasses"
        } else {
            label = "Meta glasses or headset"
        }
        return GlassesSighting(
            id: advert.identifier, label: label, maker: maker, namedAsGlasses: named, rssi: advert.rssi, seenAt: now)
    }

    public static func distance(_ rssi: Int) -> String {
        if rssi >= -60 { return "Very close" }
        if rssi >= -70 { return "Close" }
        return "In the area"
    }

    public static func alertText(_ sighting: GlassesSighting) -> String {
        "\(sighting.label) · \(sighting.distance.lowercased()) (\(sighting.rssi) dBm)"
    }
}

public struct GlassesTracker: Sendable {
    public let forgetAfter: TimeInterval
    public let cooldown: TimeInterval
    private var seen: [String: GlassesSighting] = [:]
    private var lastAlert: Date?

    public init(forgetAfter: TimeInterval = 120, cooldown: TimeInterval = 30) {
        self.forgetAfter = forgetAfter
        self.cooldown = cooldown
    }

    public mutating func observe(_ sighting: GlassesSighting) -> Bool {
        prune(sighting.seenAt)
        let fresh: Bool = seen[sighting.id] == nil
        seen[sighting.id] = sighting
        guard fresh else { return false }
        if let last = lastAlert, sighting.seenAt.timeIntervalSince(last) < cooldown { return false }
        lastAlert = sighting.seenAt
        return true
    }

    public mutating func nearby(_ now: Date) -> [GlassesSighting] {
        prune(now)
        return seen.values.sorted { $0.rssi == $1.rssi ? $0.id < $1.id : $0.rssi > $1.rssi }
    }

    public mutating func reset() {
        seen.removeAll()
        lastAlert = nil
    }

    private mutating func prune(_ now: Date) {
        seen = seen.filter { now.timeIntervalSince($0.value.seenAt) <= forgetAfter }
    }
}

public enum GlassesAlertPrefs {
    public static let range: String = "glassesAlert.range"
    public static let watching: String = "glassesAlert.watching"
    public static let keepAwake: String = "glassesAlert.keepAwake"
}
