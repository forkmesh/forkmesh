import Foundation

public enum ReticulumMapGroup: Int, CaseIterable, Sendable, Equatable {
    case peer
    case node
    case propagation
    case transport
    case other

    public init(kind: String) {
        switch kind {
        case "peer", "forkmesh": self = .peer
        case "node", "service": self = .node
        case "propagation": self = .propagation
        case "transport", "discovery": self = .transport
        default: self = .other
        }
    }

    public var label: String {
        switch self {
        case .peer: return "People"
        case .node: return "Nodes and services"
        case .propagation: return "Propagation nodes"
        case .transport: return "Transport"
        case .other: return "Other"
        }
    }

    public var seriesSlot: Int? {
        switch self {
        case .peer: return 0
        case .node: return 1
        case .propagation: return 2
        case .transport: return 3
        case .other: return nil
        }
    }
}

public enum ReticulumFormat {
    public static func bytes(_ value: Double) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var amount = value
        var unit = 0
        while amount >= 1000 && unit < 4 {
            amount /= 1000
            unit += 1
        }
        if unit == 0 { return "\(Int64(amount)) B" }
        return String(format: amount < 10 ? "%.1f" : "%.0f", amount) + " " + units[unit]
    }

    public static func rate(_ bytesPerSecond: Double) -> String { bytes(bytesPerSecond) + "/s" }

    public static func duration(_ seconds: Int64) -> String {
        if seconds < 60 { return "\(seconds) s" }
        if seconds < 3600 { return "\(seconds / 60) min" }
        if seconds < 86400 { return "\(seconds / 3600) h \((seconds % 3600) / 60) min" }
        return "\(seconds / 86400) d \((seconds % 86400) / 3600) h"
    }

    public static func age(_ when: Double?, now: Double) -> String {
        guard let when, when > 0 else { return "never" }
        let seconds = max(0, Int64(now - when))
        if seconds < 5 { return "just now" }
        return duration(seconds) + " ago"
    }

    public static func hops(_ count: Int) -> String { count == 1 ? "1 hop" : "\(count) hops" }

    public static func niceCeiling(_ value: Double) -> Double {
        guard value > 0 else { return 1 }
        let exponent = pow(10, floor(log10(value)))
        let fraction = value / exponent
        let nice: Double = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10
        return nice * exponent
    }

    public static func chartTop(_ samples: [ReticulumTrafficSample]) -> Double {
        let maximum = samples.reduce(0.0) { max($0, Double($1.rx), Double($1.tx)) }
        return niceCeiling(max(maximum * 1.1, 512))
    }

    public static func clock(_ when: Double) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter.string(from: Date(timeIntervalSince1970: when))
    }

    public static func shortDate(_ when: Double) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "d MMM HH:mm"
        return formatter.string(from: Date(timeIntervalSince1970: when))
    }

    public static func timestamp(_ seconds: Int64) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return formatter.string(from: Date(timeIntervalSince1970: Double(seconds)))
    }
}

public struct ReticulumPoint: Equatable, Sendable {
    public var x: Double
    public var y: Double

    public init(_ x: Double, _ y: Double) {
        self.x = x
        self.y = y
    }
}

public struct ReticulumMapSpoke: Equatable, Sendable {
    public let id: String
    public let name: String
    public let online: Bool

    public init(id: String, name: String, online: Bool) {
        self.id = id
        self.name = name
        self.online = online
    }
}

public struct ReticulumMapSector: Equatable, Sendable {
    public let id: String
    public let start: Double
    public let span: Double
    public var middle: Double { start + span / 2 }
}

public struct ReticulumMapLayout: Equatable, Sendable {
    public var width: Double
    public var height: Double
    public var maxRing = 4
    public var sectors: [ReticulumMapSector] = []
    public var positions: [[UInt8]: ReticulumPoint] = [:]

    public init(width: Double, height: Double) {
        self.width = width
        self.height = height
    }

    public var center: ReticulumPoint { ReticulumPoint(width / 2, height / 2 - 6) }

    public var outerRadius: Double { max(40, min(width, height) / 2 - 44) }

    public func ringRadius(_ hops: Int) -> Double {
        let outer = outerRadius
        let inner = outer * 0.24
        let ring = min(max(hops, 1), maxRing)
        if maxRing <= 1 { return outer }
        return inner + (outer - inner) * Double(ring - 1) / Double(maxRing - 1)
    }

    static func fraction(_ hash: [UInt8], _ offset: Int) -> Double {
        var value: UInt32 = 0
        var index = 0
        while index < 4 && offset + index < hash.count {
            value = (value << 8) | UInt32(hash[offset + index])
            index += 1
        }
        return Double(value) / 4_294_967_296.0
    }

    public static func compute(width: Double, height: Double, spokes: [ReticulumMapSpoke], destinations: [ReticulumDestination]) -> ReticulumMapLayout {
        var layout = ReticulumMapLayout(width: width, height: height)
        layout.maxRing = min(destinations.reduce(2) { max($0, $1.hops) }, 8)
        let known = Set(spokes.map(\.id))
        var counts: [String: Int] = [:]
        for record in destinations { counts[known.contains(record.interfaceId) ? record.interfaceId : "", default: 0] += 1 }
        var order = spokes.map(\.id)
        if counts[""] != nil { order.append("") }
        guard !order.isEmpty else { return layout }
        let total = order.reduce(0.0) { $0 + Double(max(1, counts[$1] ?? 0) + 2) }
        var angle = -Double.pi / 2
        var byId: [String: ReticulumMapSector] = [:]
        for id in order {
            let span = 2 * Double.pi * Double(max(1, counts[id] ?? 0) + 2) / total
            let sector = ReticulumMapSector(id: id, start: angle, span: span)
            layout.sectors.append(sector)
            byId[id] = sector
            angle += span
        }
        let middle = layout.center
        let ringGap = layout.maxRing > 1 ? layout.ringRadius(2) - layout.ringRadius(1) : 30
        for record in destinations {
            let id = known.contains(record.interfaceId) ? record.interfaceId : ""
            guard let sector = byId[id] else { continue }
            let margin = min(0.12, sector.span * 0.15)
            let theta = sector.start + margin + (sector.span - 2 * margin) * fraction(record.hash, 0)
            let jitter = (fraction(record.hash, 4) - 0.5) * ringGap * 0.5
            let radius = layout.ringRadius(record.hops) + jitter
            layout.positions[record.hash] = ReticulumPoint(middle.x + cos(theta) * radius, middle.y + sin(theta) * radius)
        }
        return layout
    }

    public func hash(at point: ReticulumPoint, within distance: Double = 22) -> [UInt8]? {
        var best: [UInt8]?
        var bestDistance = distance * distance
        for (hash, position) in positions {
            let dx = position.x - point.x
            let dy = position.y - point.y
            let squared = dx * dx + dy * dy
            if squared < bestDistance {
                bestDistance = squared
                best = hash
            }
        }
        return best
    }

    public func ringLabel(_ ring: Int) -> String {
        if ring == maxRing && maxRing == 8 { return "8+ hops" }
        return ring == 1 ? "1 hop" : "\(ring) hops"
    }
}
