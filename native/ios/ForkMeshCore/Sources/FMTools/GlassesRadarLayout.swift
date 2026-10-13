import Foundation

public struct GlassesRadarBlip: Equatable, Sendable {
    public let id: String
    public let angle: Double
    public let distance: Double
    public let glasses: Bool

    public init(id: String, angle: Double, distance: Double, glasses: Bool) {
        self.id = id
        self.angle = angle
        self.distance = distance
        self.glasses = glasses
    }
}

public enum GlassesRadarLayout {
    public static let nearest: Double = 0.2
    public static let farthest: Double = 0.92

    public static func angle(_ id: String) -> Double {
        var hash: UInt32 = 2_166_136_261
        for byte in id.utf8 {
            hash ^= UInt32(byte)
            hash = hash &* 16_777_619
        }
        return Double(hash % 3600) / 10
    }

    public static func distance(_ rssi: Int) -> Double {
        min(farthest, max(nearest, 1 - GlassesHoming.closeness(rssi)))
    }

    public static func blips(
        others: [NearbyDevice], glasses: [GlassesSighting], homing: GlassesHomingReading?, heading: Double?
    ) -> [GlassesRadarBlip] {
        let devices: [GlassesRadarBlip] = others.map {
            GlassesRadarBlip(id: $0.id, angle: angle($0.id), distance: distance($0.rssi), glasses: false)
        }
        let found: [GlassesRadarBlip] = glasses.map { (sighting: GlassesSighting) -> GlassesRadarBlip in
            let followed: GlassesHomingReading? = homing?.id == sighting.id && homing?.live == true ? homing : nil
            return GlassesRadarBlip(
                id: sighting.id, angle: followed?.arrow(heading: heading) ?? angle(sighting.id),
                distance: distance(followed?.rssi ?? sighting.rssi), glasses: true)
        }
        return devices + found
    }
}
