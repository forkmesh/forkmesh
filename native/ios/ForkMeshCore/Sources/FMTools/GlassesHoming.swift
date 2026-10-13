import Foundation

public enum GlassesTrend: String, Sendable {
    case closer
    case farther
    case steady

    public var label: String {
        switch self {
        case .closer: return "Getting closer"
        case .farther: return "Moving away"
        case .steady: return "Holding steady"
        }
    }
}

public struct GlassesHomingReading: Equatable, Sendable {
    public let id: String
    public let label: String
    public let rssi: Int
    public let closeness: Double
    public let trend: GlassesTrend?
    public let bearing: Double?
    public let confidence: Double
    public let live: Bool
    public let compass: Bool

    public init(
        id: String, label: String, rssi: Int, closeness: Double, trend: GlassesTrend?, bearing: Double?, confidence: Double,
        live: Bool, compass: Bool
    ) {
        self.id = id
        self.label = label
        self.rssi = rssi
        self.closeness = closeness
        self.trend = trend
        self.bearing = bearing
        self.confidence = confidence
        self.live = live
        self.compass = compass
    }

    public var distance: String { GlassesSignatures.distance(rssi) }

    public func arrow(heading: Double?) -> Double? {
        guard let bearing = bearing, let heading = heading else { return nil }
        return GlassesHoming.normalize(bearing - heading)
    }

    public var hint: String {
        if !live { return "Signal lost. Walk around to pick it up again." }
        if !compass { return "No compass reading, so only closeness is shown." }
        if bearing == nil { return "Hold the phone in front of you and turn slowly to find the direction." }
        if confidence < 0.5 { return "Rough direction. Keep turning slowly to sharpen it." }
        return "Strongest signal this way."
    }

    public var summary: String {
        var parts: [String] = [live ? distance : "Signal lost", "\(rssi) dBm"]
        if live, let trend = trend {
            parts.append(trend.label)
        }
        return parts.joined(separator: " · ")
    }
}

public struct GlassesHoming: Sendable {
    public static let smoothing: Double = 0.3
    public static let recent: TimeInterval = 3
    public static let trendDb: Double = 3
    public static let minSectorSamples: Int = 2
    public static let minSectors: Int = 3
    public static let minSpreadDb: Double = 3
    public static let fullSpreadDb: Double = 12
    public static let farRssi: Double = -95
    public static let nearRssi: Double = -45

    private struct Sample: Sendable {
        let at: Date
        let rssi: Int
        let heading: Double?
    }

    public let window: TimeInterval
    public let liveFor: TimeInterval
    public let sticky: TimeInterval
    public let sectors: Int
    public private(set) var target: GlassesSighting?
    private var samples: [Sample] = []
    private var smoothed: Double?

    public init(window: TimeInterval = 20, liveFor: TimeInterval = 8, sticky: TimeInterval = 15, sectors: Int = 12) {
        self.window = window
        self.liveFor = liveFor
        self.sticky = sticky
        self.sectors = sectors
    }

    public mutating func follow(_ nearby: [GlassesSighting], now: Date) {
        let current: GlassesSighting? = target
        if let current = current, let kept = nearby.first(where: { $0.id == current.id }),
            now.timeIntervalSince(kept.seenAt) <= sticky
        {
            target = kept
            return
        }
        let next: GlassesSighting? = nearby.first { now.timeIntervalSince($0.seenAt) <= sticky }
        if next?.id != current?.id {
            samples.removeAll()
            smoothed = nil
        }
        target = next
    }

    public mutating func hear(_ id: String, rssi: Int, heading: Double?, now: Date) {
        guard id == target?.id, rssi < 0, rssi >= -127 else { return }
        samples.append(Sample(at: now, rssi: rssi, heading: heading.map(GlassesHoming.normalize)))
        if let previous = smoothed {
            smoothed = previous + (Double(rssi) - previous) * GlassesHoming.smoothing
        } else {
            smoothed = Double(rssi)
        }
        prune(now)
    }

    public mutating func reset() {
        samples.removeAll()
        smoothed = nil
        target = nil
    }

    public mutating func reading(_ now: Date) -> GlassesHomingReading? {
        guard let sighting = target else { return nil }
        prune(now)
        let rssi: Int = smoothed.map { Int($0.rounded()) } ?? sighting.rssi
        let last: Date = samples.last?.at ?? sighting.seenAt
        let found: (angle: Double, confidence: Double)? = bearing()
        return GlassesHomingReading(
            id: sighting.id, label: sighting.label, rssi: rssi, closeness: GlassesHoming.closeness(rssi), trend: trend(now),
            bearing: found?.angle, confidence: found?.confidence ?? 0, live: now.timeIntervalSince(last) <= liveFor,
            compass: samples.contains { $0.heading != nil })
    }

    private mutating func prune(_ now: Date) {
        if let cut = samples.firstIndex(where: { now.timeIntervalSince($0.at) <= window }) {
            if cut > 0 { samples.removeFirst(cut) }
        } else {
            samples.removeAll()
        }
    }

    private func trend(_ now: Date) -> GlassesTrend? {
        let recent: [Int] = samples.filter { now.timeIntervalSince($0.at) <= GlassesHoming.recent }.map(\.rssi)
        let earlier: [Int] = samples.filter {
            let age: TimeInterval = now.timeIntervalSince($0.at)
            return age >= GlassesHoming.recent + 1 && age <= GlassesHoming.recent + 9
        }.map(\.rssi)
        guard recent.count >= 2, earlier.count >= 2 else { return nil }
        let change: Double = GlassesHoming.mean(recent) - GlassesHoming.mean(earlier)
        if change >= GlassesHoming.trendDb { return .closer }
        if change <= -GlassesHoming.trendDb { return .farther }
        return .steady
    }

    private func bearing() -> (angle: Double, confidence: Double)? {
        let width: Double = 360 / Double(sectors)
        var sums: [Double] = Array(repeating: 0, count: sectors)
        var counts: [Int] = Array(repeating: 0, count: sectors)
        for sample in samples {
            guard let heading = sample.heading else { continue }
            let index: Int = GlassesHoming.wrap(Int((heading / width).rounded(.down)), sectors)
            sums[index] += Double(sample.rssi)
            counts[index] += 1
        }
        let averages: [Double?] = (0..<sectors).map {
            counts[$0] >= GlassesHoming.minSectorSamples ? sums[$0] / Double(counts[$0]) : nil
        }
        let known: [Double] = averages.compactMap { $0 }
        guard known.count >= GlassesHoming.minSectors, let weakest = known.min(),
            let best = averages.indices.max(by: { (averages[$0] ?? -.infinity) < (averages[$1] ?? -.infinity) }),
            let strongest = averages[best]
        else { return nil }
        let spread: Double = strongest - weakest
        guard spread >= GlassesHoming.minSpreadDb else { return nil }
        var x: Double = 0
        var y: Double = 0
        for offset in -1...1 {
            let index: Int = GlassesHoming.wrap(best + offset, sectors)
            guard let average = averages[index] else { continue }
            let weight: Double = (average - weakest) * (average - weakest)
            let angle: Double = (Double(index) + 0.5) * width * .pi / 180
            x += weight * sin(angle)
            y += weight * cos(angle)
        }
        let angle: Double = GlassesHoming.normalize(atan2(x, y) * 180 / .pi)
        let confidence: Double = min(1, spread / GlassesHoming.fullSpreadDb) * min(1, Double(known.count) / (Double(sectors) / 2))
        return (angle, confidence)
    }

    private static func wrap(_ value: Int, _ count: Int) -> Int {
        ((value % count) + count) % count
    }

    private static func mean(_ values: [Int]) -> Double {
        Double(values.reduce(0, +)) / Double(values.count)
    }

    public static func normalize(_ degrees: Double) -> Double {
        let r: Double = degrees.truncatingRemainder(dividingBy: 360)
        return r < 0 ? r + 360 : r
    }

    public static func closeness(_ rssi: Int) -> Double {
        min(1, max(0, (Double(rssi) - farRssi) / (nearRssi - farRssi)))
    }

    public static func clock(_ arrow: Double) -> String {
        let hour: Int = Int((normalize(arrow) / 30).rounded()) % 12
        return "at \(hour == 0 ? 12 : hour) o'clock"
    }

    public static func unwrap(_ shown: Double, _ next: Double) -> Double {
        let delta: Double = normalize(next - shown + 180) - 180
        return shown + delta
    }

    public static func smoothHeading(_ previous: Double?, _ next: Double, factor: Double = 0.2) -> Double {
        guard let previous = previous else { return normalize(next) }
        let a: Double = previous * .pi / 180
        let b: Double = next * .pi / 180
        let x: Double = sin(a) * (1 - factor) + sin(b) * factor
        let y: Double = cos(a) * (1 - factor) + cos(b) * factor
        return normalize(atan2(x, y) * 180 / .pi)
    }

    public static func heading(rotation: [Double]) -> Double? {
        guard rotation.count >= 9 else { return nil }
        let east: Double = rotation[1] - rotation[2]
        let north: Double = rotation[4] - rotation[5]
        guard (east * east + north * north).squareRoot() >= 0.2 else { return nil }
        return normalize(atan2(east, north) * 180 / .pi)
    }

    public static func rotation(up: [Double], magnetic: [Double]) -> [Double]? {
        guard up.count == 3, magnetic.count == 3 else { return nil }
        var h: [Double] = [
            magnetic[1] * up[2] - magnetic[2] * up[1],
            magnetic[2] * up[0] - magnetic[0] * up[2],
            magnetic[0] * up[1] - magnetic[1] * up[0],
        ]
        let hLength: Double = (h[0] * h[0] + h[1] * h[1] + h[2] * h[2]).squareRoot()
        let aLength: Double = (up[0] * up[0] + up[1] * up[1] + up[2] * up[2]).squareRoot()
        guard hLength > 0.1, aLength > 0.1 else { return nil }
        h = h.map { $0 / hLength }
        let a: [Double] = up.map { $0 / aLength }
        let m: [Double] = [
            a[1] * h[2] - a[2] * h[1],
            a[2] * h[0] - a[0] * h[2],
            a[0] * h[1] - a[1] * h[0],
        ]
        return h + m + a
    }
}
