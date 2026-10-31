import Foundation

public enum SightingKind: String, Sendable {
    case glasses
    case camera

    public var title: String {
        switch self {
        case .glasses: return "Smart glasses nearby"
        case .camera: return "Camera nearby"
        }
    }

    public var icon: String {
        switch self {
        case .glasses: return "visibility"
        case .camera: return "photo_camera"
        }
    }
}

public enum GlassesMaker: String, CaseIterable, Sendable {
    case meta
    case metaTechnologies
    case luxottica
    case vuzix
    case snap
    case gopro
    case insta360
    case dji
    case canon
    case nikon
    case fujifilm
    case olympus
    case omDigital
    case ricoh
    case leica
    case hasselblad
    case polaroid
    case blincam
    case opal
    case axon
    case seventyMai
    case arlo
    case hikvision

    public init?(companyId: Int) {
        guard let maker = GlassesMaker.allCases.first(where: { $0.companyId == companyId }) else { return nil }
        self = maker
    }

    public init?(service: Int) {
        guard let maker = GlassesMaker.allCases.first(where: { $0.services.contains(service) }) else { return nil }
        self = maker
    }

    public var companyId: Int? {
        switch self {
        case .meta: return 0x01AB
        case .metaTechnologies: return 0x058E
        case .luxottica: return 0x0D53
        case .vuzix: return 0x060C
        case .snap: return nil
        case .gopro: return 0x02F2
        case .insta360: return 0x10D7
        case .dji: return 0x08AA
        case .canon: return 0x01A9
        case .nikon: return 0x0399
        case .fujifilm: return 0x04D8
        case .olympus: return 0x04D0
        case .omDigital: return 0x09F1
        case .ricoh: return 0x065F
        case .leica: return 0x051A
        case .hasselblad: return 0x10D4
        case .polaroid: return 0x02A0
        case .blincam: return 0x04D7
        case .opal: return 0x0F30
        case .axon: return nil
        case .seventyMai: return 0x0909
        case .arlo: return 0x0C19
        case .hikvision: return 0x0E25
        }
    }

    public var services: [Int] {
        switch self {
        case .snap: return [0xFE45]
        case .gopro: return [0xFEA5, 0xFEA6]
        case .insta360: return [0xFC2D, 0xFC2E, 0xFC2F, 0xFC30]
        case .axon: return [0xFC81]
        case .seventyMai: return [0xFD4D, 0xFD4E]
        default: return []
        }
    }

    public var kind: SightingKind {
        switch self {
        case .meta, .metaTechnologies, .luxottica, .vuzix, .snap: return .glasses
        default: return .camera
        }
    }

    public var label: String {
        switch self {
        case .meta: return "Meta Platforms"
        case .metaTechnologies: return "Meta Platforms Technologies"
        case .luxottica: return "Luxottica (Ray-Ban, Oakley)"
        case .vuzix: return "Vuzix"
        case .snap: return "Snap"
        case .gopro: return "GoPro"
        case .insta360: return "Arashi Vision (Insta360)"
        case .dji: return "DJI"
        case .canon: return "Canon"
        case .nikon: return "Nikon"
        case .fujifilm: return "Fujifilm"
        case .olympus: return "Olympus"
        case .omDigital: return "OM Digital Solutions"
        case .ricoh: return "Ricoh (Pentax, Theta)"
        case .leica: return "Leica Camera"
        case .hasselblad: return "Hasselblad"
        case .polaroid: return "Impossible Camera (Polaroid)"
        case .blincam: return "Blincam"
        case .opal: return "Opal Camera"
        case .axon: return "Axon"
        case .seventyMai: return "70mai"
        case .arlo: return "Arlo"
        case .hikvision: return "Hikvision"
        }
    }

    public var shown: String {
        switch self {
        case .meta, .metaTechnologies: return "Meta glasses or headset"
        case .luxottica: return "Ray-Ban or Oakley Meta glasses"
        case .vuzix: return "Vuzix camera glasses"
        case .snap: return "Snap Spectacles"
        case .gopro: return "GoPro camera"
        case .insta360: return "Insta360 camera"
        case .dji: return "DJI camera or drone"
        case .canon: return "Canon camera"
        case .nikon: return "Nikon camera"
        case .fujifilm: return "Fujifilm camera"
        case .olympus: return "Olympus camera"
        case .omDigital: return "OM System camera"
        case .ricoh: return "Ricoh, Pentax or Theta camera"
        case .leica: return "Leica camera"
        case .hasselblad: return "Hasselblad camera"
        case .polaroid: return "Polaroid camera"
        case .blincam: return "Blincam wearable camera"
        case .opal: return "Opal webcam"
        case .axon: return "Axon body camera"
        case .seventyMai: return "70mai dash cam"
        case .arlo: return "Arlo security camera"
        case .hikvision: return "Hikvision security camera"
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
    public let kind: SightingKind

    public init(
        id: String, label: String, maker: GlassesMaker?, namedAsGlasses: Bool, rssi: Int, seenAt: Date, kind: SightingKind = .glasses
    ) {
        self.id = id
        self.label = label
        self.maker = maker
        self.namedAsGlasses = namedAsGlasses
        self.rssi = rssi
        self.seenAt = seenAt
        self.kind = kind
    }

    public var distance: String { GlassesSignatures.distance(rssi) }
}

public enum GlassesSignatures {
    public static let companyIds: Set<Int> = Set(GlassesMaker.allCases.compactMap(\.companyId))
    public static let serviceUuids: Set<Int> = Set(GlassesMaker.allCases.flatMap(\.services))
    public static let nameKeywords: [String] = ["ray-ban", "rayban", "ray ban", "oakley meta", "meta ray", "meta glasses"]
    public static let cameraKeywords: [String] = [
        "camera", "webcam", "dashcam", "dash cam", "bodycam", "body cam", "body-cam", "trail cam", "eufycam", "wyze cam",
        "gopro", "insta360", "insta 360", "osmo action", "osmo pocket", "osmo 360", "osmo nano", "dji action", "dji pocket",
        "canon", "powershot", "eos r", "eos m", "nikon", "coolpix", "fujifilm", "lumix", "olympus", "om system", "ricoh",
        "pentax", "theta", "leica", "hasselblad", "ilce-", "zv-e", "zv-1", "dsc-rx", "dsc-hx", "fdr-x", "hdr-as", "kodak",
        "polaroid", "instax mini evo", "instax pal", "akaso", "sjcam", "yi action", "blincam", "spectacles", "axon body",
        "nextbase", "viofo", "70mai", "blackvue", "thinkware", "vantrue", "garmin dash", "brinno", "tactacam", "spypoint",
        "mavic", "dji mini", "dji air", "dji neo", "dji flip", "avata", "skydio", "anafi", "hover x1", "vuzix", "rokid",
    ]
    public static let printerKeywords: [String] = [
        "printer", "pixma", "maxify", "selphy", "imageclass", "instax link", "instax mini link", "instax square link",
        "instax wide link", "instax share",
    ]

    public static func companyId(manufacturerData: [UInt8]) -> Int? {
        guard manufacturerData.count >= 2 else { return nil }
        return Int(manufacturerData[0]) | (Int(manufacturerData[1]) << 8)
    }

    public static func nameKind(_ name: String?) -> SightingKind? {
        guard let lowered = name?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased(), !lowered.isEmpty else {
            return nil
        }
        if nameKeywords.contains(where: { lowered.contains($0) }) { return .glasses }
        if cameraKeywords.contains(where: { lowered.contains($0) }) && !printerName(lowered) { return .camera }
        return nil
    }

    public static func nameMatches(_ name: String?) -> Bool {
        nameKind(name) != nil
    }

    public static func printerName(_ name: String?) -> Bool {
        guard let lowered = name?.lowercased() else { return false }
        return printerKeywords.contains { lowered.contains($0) }
    }

    public static func maker(_ advert: GlassesAdvert) -> GlassesMaker? {
        advert.companyIds.lazy.compactMap { GlassesMaker(companyId: $0) }.first
            ?? advert.serviceUuids.lazy.compactMap { GlassesMaker(service: $0) }.first
    }

    public static func kind(_ advert: GlassesAdvert) -> SightingKind? {
        guard let kind = maker(advert)?.kind else { return nameKind(advert.name) }
        return kind == .camera && printerName(advert.name) ? nil : kind
    }

    public static func match(_ advert: GlassesAdvert, range: GlassesRange, now: Date) -> GlassesSighting? {
        guard advert.rssi >= range.minRssi, advert.rssi < 0, let kind = kind(advert) else { return nil }
        let maker: GlassesMaker? = maker(advert)
        let named: Bool = nameMatches(advert.name)
        let label: String
        if named, let name = advert.name {
            label = name.trimmingCharacters(in: .whitespacesAndNewlines)
        } else {
            label = maker?.shown ?? kind.title
        }
        return GlassesSighting(
            id: advert.identifier, label: label, maker: maker, namedAsGlasses: named, rssi: advert.rssi, seenAt: now, kind: kind)
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
