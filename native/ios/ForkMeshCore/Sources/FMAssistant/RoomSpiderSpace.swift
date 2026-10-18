import Foundation

public typealias RoomPoint = SIMD3<Float>

public enum RoomMath {
    public static let up: RoomPoint = RoomPoint(0, 1, 0)

    public static func dot(_ a: RoomPoint, _ b: RoomPoint) -> Float { (a * b).sum() }

    public static func cross(_ a: RoomPoint, _ b: RoomPoint) -> RoomPoint {
        RoomPoint(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)
    }

    public static func length(_ a: RoomPoint) -> Float { dot(a, a).squareRoot() }

    public static func normalized(_ a: RoomPoint, or fallback: RoomPoint = up) -> RoomPoint {
        let size: Float = length(a)
        return size > 1e-6 ? a / size : fallback
    }

    public static func flat(_ a: RoomPoint, on normal: RoomPoint) -> RoomPoint { a - normal * dot(a, normal) }

    public static func perpendicular(to a: RoomPoint) -> RoomPoint {
        let pick: RoomPoint = abs(a.y) < 0.9 ? up : RoomPoint(1, 0, 0)
        return normalized(cross(a, pick))
    }

    public static func rotate(_ v: RoomPoint, from a: RoomPoint, to b: RoomPoint) -> RoomPoint {
        let axis: RoomPoint = cross(a, b)
        let sine: Float = length(axis)
        let cosine: Float = dot(a, b)
        if sine < 1e-5 {
            if cosine > 0 { return v }
            let pivot: RoomPoint = perpendicular(to: a)
            return pivot * (2 * dot(v, pivot)) - v
        }
        let k: RoomPoint = axis / sine
        return v * cosine + cross(k, v) * sine + k * (dot(k, v) * (1 - cosine))
    }

    public static func turn(_ heading: RoomPoint, toward wanted: RoomPoint, on normal: RoomPoint, by most: Float) -> RoomPoint {
        let from: RoomPoint = normalized(flat(heading, on: normal), or: perpendicular(to: normal))
        let to: RoomPoint = normalized(flat(wanted, on: normal), or: from)
        let angle: Float = atan2(dot(cross(from, to), normal), dot(from, to))
        let step: Float = max(-most, min(most, angle))
        return normalized(from * cos(step) + cross(normal, from) * sin(step), or: from)
    }
}

public enum RoomSurfaceKind: String, Sendable, CaseIterable {
    case floor, wall, ceiling, table, seat, door, window, other
}

public struct RoomSurface: Sendable, Equatable, Identifiable {
    public let id: String
    public var kind: RoomSurfaceKind
    public var center: RoomPoint
    public var normal: RoomPoint
    public var across: RoomPoint
    public var half: SIMD2<Float>
    public var bounded: Bool

    public init(id: String, kind: RoomSurfaceKind, center: RoomPoint, normal: RoomPoint, across: RoomPoint, half: SIMD2<Float>, bounded: Bool = true) {
        let n: RoomPoint = RoomMath.normalized(normal)
        self.id = id
        self.kind = kind
        self.center = center
        self.normal = n
        self.across = RoomMath.normalized(RoomMath.flat(across, on: n), or: RoomMath.perpendicular(to: n))
        self.half = SIMD2<Float>(max(half.x, 0.01), max(half.y, 0.01))
        self.bounded = bounded
    }

    public var along: RoomPoint { RoomMath.cross(normal, across) }

    public var upright: Bool { normal.y > 0.7 }

    public var vertical: Bool { abs(normal.y) < 0.35 }

    public var overhead: Bool { normal.y < -0.7 }

    public var furniture: Bool {
        switch kind {
        case .table, .seat: return true
        case .other: return upright
        default: return false
        }
    }

    public func height(_ p: RoomPoint) -> Float { RoomMath.dot(p - center, normal) }

    public func local(_ p: RoomPoint) -> SIMD2<Float> {
        let d: RoomPoint = p - center
        return SIMD2<Float>(RoomMath.dot(d, across), RoomMath.dot(d, along))
    }

    public func point(_ l: SIMD2<Float>, lift: Float = 0) -> RoomPoint {
        center + across * l.x + along * l.y + normal * lift
    }

    public func project(_ p: RoomPoint, lift: Float = 0) -> RoomPoint {
        p - normal * (height(p) - lift)
    }

    private func reach(floor: Float) -> (low: SIMD2<Float>, high: SIMD2<Float>) {
        var low: SIMD2<Float> = -half
        var high: SIMD2<Float> = half
        guard vertical, floor < center.y else { return (low, high) }
        if abs(across.y) > abs(along.y), abs(across.y) > 0.5 {
            let toFloor: Float = (floor - center.y) / across.y
            if toFloor < 0 { low.x = min(low.x, toFloor) } else { high.x = max(high.x, toFloor) }
        } else if abs(along.y) > 0.5 {
            let toFloor: Float = (floor - center.y) / along.y
            if toFloor < 0 { low.y = min(low.y, toFloor) } else { high.y = max(high.y, toFloor) }
        }
        return (low, high)
    }

    public func covers(_ p: RoomPoint, margin: Float = 0, floor: Float) -> Bool {
        guard bounded else { return true }
        let l: SIMD2<Float> = local(p)
        let box = reach(floor: floor)
        return l.x >= box.low.x - margin && l.x <= box.high.x + margin && l.y >= box.low.y - margin && l.y <= box.high.y + margin
    }

    public func clamped(_ p: RoomPoint, inset: Float, floor: Float, lift: Float = 0) -> RoomPoint {
        guard bounded else { return project(p, lift: lift) }
        let box = reach(floor: floor)
        var l: SIMD2<Float> = local(p)
        let lowX: Float = min(box.low.x + inset, 0)
        let highX: Float = max(box.high.x - inset, 0)
        let lowY: Float = min(box.low.y + inset, 0)
        let highY: Float = max(box.high.y - inset, 0)
        l = SIMD2<Float>(max(lowX, min(highX, l.x)), max(lowY, min(highY, l.y)))
        return point(l, lift: lift)
    }

    public func matches(_ other: String, at p: RoomPoint, in room: RoomMap) -> Bool {
        if other == id { return true }
        guard let that = room.surface(other) else { return false }
        return RoomMath.dot(that.normal, normal) > 0.95 && abs(height(p)) < 0.05
    }

    public func distance(to p: RoomPoint, floor: Float) -> Float {
        RoomMath.length(clamped(p, inset: 0, floor: floor) - p)
    }
}

public struct RoomMap: Sendable {
    public static let floorId: String = "room-floor"
    public static let fallbackDrop: Float = 1.35

    public private(set) var surfaces: [String: RoomSurface] = [:]
    public private(set) var viewer: RoomPoint = .zero
    public private(set) var viewerSeen: Bool = false
    private var order: [String] = []

    public init() {}

    public mutating func look(from viewer: RoomPoint) {
        self.viewer = viewer
        viewerSeen = true
    }

    public mutating func put(_ next: RoomSurface) {
        var surface: RoomSurface = next
        if let old = surfaces[next.id] {
            if RoomMath.dot(old.normal, next.normal) < 0 { surface.normal = -next.normal }
        } else {
            if RoomMath.dot(viewer - next.center, next.normal) < 0 { surface.normal = -next.normal }
            if surface.kind == .ceiling, surface.normal.y > 0 { surface.normal = -surface.normal }
            if surface.kind == .floor, surface.normal.y < 0 { surface.normal = -surface.normal }
            order.append(next.id)
        }
        surface.across = RoomMath.normalized(RoomMath.flat(surface.across, on: surface.normal), or: RoomMath.perpendicular(to: surface.normal))
        surfaces[next.id] = surface
    }

    public mutating func remove(_ id: String) {
        surfaces[id] = nil
        order.removeAll { $0 == id }
    }

    public var detected: [RoomSurface] { order.compactMap { surfaces[$0] } }

    public var floorY: Float {
        let floors: [Float] = detected.filter { $0.kind == .floor }.map(\.center.y)
        if let lowest = floors.min() { return lowest }
        let tops: [Float] = detected.filter { $0.upright && viewer.y - $0.center.y > 0.9 }.map(\.center.y)
        if let lowest = tops.min() { return lowest }
        return viewer.y - RoomMap.fallbackDrop
    }

    public var floorFound: Bool {
        detected.contains { $0.kind == .floor || ($0.upright && viewer.y - $0.center.y > 0.9) }
    }

    public var summary: String {
        var parts: [String] = []
        if floorFound { parts.append("the floor") }
        let named: [(RoomSurfaceKind, String, String)] = [
            (.wall, "a wall", "walls"), (.table, "a table", "tables"), (.seat, "a seat", "seats"),
            (.window, "a window", "windows"), (.door, "a door", "doors"),
        ]
        for (kind, one, many) in named {
            let count: Int = detected.filter { $0.kind == kind }.count
            if count == 1 { parts.append(one) } else if count > 1 { parts.append("\(count) \(many)") }
        }
        if detected.contains(where: { $0.kind == .ceiling }) { parts.append("the ceiling") }
        let level: Float = floorY
        let ledges: Int = detected.filter { $0.kind == .other && $0.upright && $0.center.y > level + 0.15 }.count
        if ledges == 1 { parts.append("a ledge") } else if ledges > 1 { parts.append("\(ledges) ledges") }
        guard let last = parts.last else { return "" }
        if parts.count == 1 { return last }
        return parts.dropLast().joined(separator: ", ") + " and " + last
    }

    public var floor: RoomSurface {
        RoomSurface(id: RoomMap.floorId, kind: .floor, center: RoomPoint(viewer.x, floorY, viewer.z), normal: RoomMath.up,
                    across: RoomPoint(1, 0, 0), half: SIMD2<Float>(50, 50), bounded: false)
    }

    public var all: [RoomSurface] { detected + [floor] }

    public func surface(_ id: String) -> RoomSurface? {
        id == RoomMap.floorId ? floor : surfaces[id]
    }

    public struct Hit: Sendable, Equatable {
        public var surface: RoomSurface
        public var point: RoomPoint
        public var fraction: Float
    }

    public func hit(from a: RoomPoint, to b: RoomPoint, lift: Float) -> Hit? {
        var best: Hit?
        let level: Float = floorY
        for surface in all {
            let before: Float = surface.height(a) - lift
            let after: Float = surface.height(b) - lift
            guard after < 0, before >= -0.02, before > after else { continue }
            let fraction: Float = max(0, min(1, before / (before - after)))
            let at: RoomPoint = a + (b - a) * fraction
            guard surface.covers(at, margin: 0.03, floor: level) else { continue }
            if let current = best, current.fraction < fraction - 1e-4 { continue }
            if let current = best, abs(current.fraction - fraction) <= 1e-4, current.surface.bounded || !surface.bounded { continue }
            best = Hit(surface: surface, point: surface.project(at, lift: lift), fraction: fraction)
        }
        return best
    }

    public func wall(ahead p: RoomPoint, from start: RoomPoint, on current: RoomSurface, lift: Float) -> RoomSurface? {
        let level: Float = floorY
        var best: (RoomSurface, Float)?
        for surface in all where surface.id != current.id && RoomMath.dot(surface.normal, current.normal) < 0.6 {
            let height: Float = surface.height(p)
            guard height < lift, height > -0.12, surface.height(start) >= lift - 0.01, surface.covers(p, margin: 0.04, floor: level) else { continue }
            if best == nil || height < best!.1 { best = (surface, height) }
        }
        return best?.0
    }

    public func neighbor(of current: RoomSurface, at p: RoomPoint, lift: Float) -> RoomSurface? {
        let level: Float = floorY
        var best: (RoomSurface, Float)?
        for surface in all where surface.id != current.id {
            let gap: Float = abs(surface.height(p) - lift)
            guard gap < 0.09, surface.covers(p, margin: 0.06, floor: level) else { continue }
            if best == nil || gap < best!.1 { best = (surface, gap) }
        }
        return best?.0
    }

    public func below(_ p: RoomPoint, besides id: String) -> RoomSurface? {
        let level: Float = floorY
        var best: (RoomSurface, Float)?
        for surface in all where surface.id != id && surface.upright {
            let height: Float = surface.height(p)
            guard height > 0.05, surface.covers(p, margin: 0, floor: level) else { continue }
            if best == nil || height < best!.1 { best = (surface, height) }
        }
        return best?.0
    }

    public func above(_ p: RoomPoint, besides id: String) -> RoomSurface? {
        let level: Float = floorY
        var best: (RoomSurface, Float)?
        for surface in detected where surface.id != id && surface.overhead {
            let height: Float = surface.height(p)
            guard height > 0.2, surface.covers(p, margin: 0, floor: level) else { continue }
            if best == nil || height < best!.1 { best = (surface, height) }
        }
        return best?.0
    }

    public func nearest(_ p: RoomPoint, where keep: (RoomSurface) -> Bool) -> RoomSurface? {
        let level: Float = floorY
        return detected.filter(keep).min { $0.distance(to: p, floor: level) < $1.distance(to: p, floor: level) }
    }
}
