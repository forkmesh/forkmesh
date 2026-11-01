import Foundation

public typealias DragonPoint = SIMD2<Double>

public struct DragonAffine: Equatable, Sendable {
    public var a: Double = 1
    public var b: Double = 0
    public var c: Double = 0
    public var d: Double = 1
    public var tx: Double = 0
    public var ty: Double = 0

    public init() {}

    public init(a: Double, b: Double, c: Double, d: Double, tx: Double, ty: Double) {
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.tx = tx
        self.ty = ty
    }

    public static let identity: DragonAffine = DragonAffine()

    public func translated(_ x: Double, _ y: Double) -> DragonAffine {
        DragonAffine(a: a, b: b, c: c, d: d, tx: a * x + c * y + tx, ty: b * x + d * y + ty)
    }

    public func translated(_ p: DragonPoint) -> DragonAffine { translated(p.x, p.y) }

    public func scaled(_ sx: Double, _ sy: Double) -> DragonAffine {
        DragonAffine(a: a * sx, b: b * sx, c: c * sy, d: d * sy, tx: tx, ty: ty)
    }

    public func rotated(_ angle: Double) -> DragonAffine {
        let cs: Double = cos(angle)
        let sn: Double = sin(angle)
        return DragonAffine(a: a * cs + c * sn, b: b * cs + d * sn, c: -a * sn + c * cs, d: -b * sn + d * cs, tx: tx, ty: ty)
    }

    public func concatenated(_ o: DragonAffine) -> DragonAffine {
        DragonAffine(
            a: a * o.a + c * o.b, b: b * o.a + d * o.b, c: a * o.c + c * o.d, d: b * o.c + d * o.d,
            tx: a * o.tx + c * o.ty + tx, ty: b * o.tx + d * o.ty + ty)
    }

    public func apply(_ p: DragonPoint) -> DragonPoint {
        DragonPoint(a * p.x + c * p.y + tx, b * p.x + d * p.y + ty)
    }

    public var isFinite: Bool { [a, b, c, d, tx, ty].allSatisfy { $0.isFinite } }
}

public enum DragonLayer: String, CaseIterable, Sendable {
    case armFar, armNear, body, ear, eyeFar, eyeNear, footFar, footNear, head, pawFar, pawNear, tailStrip
    case thighFar, thighNear, wingFar, wingNear
}

public struct DragonLayerFrame: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var w: Double
    public var h: Double
    public var ox: Double
    public var oy: Double
    public var s: Double
}

public struct DragonPropFrame: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var w: Double
    public var h: Double
    public var k: Double
}

public struct DragonTailMeta: Equatable, Sendable {
    public var curve: [DragonPoint]
    public var inner: Double
    public var outer: Double
    public var length: Double
}

public struct DragonJoints: Equatable, Sendable {
    public var axis: DragonPoint
    public var ear: DragonPoint
    public var footFar: DragonPoint
    public var footNear: DragonPoint
    public var hip: DragonPoint
    public var hipFar: DragonPoint
    public var hipNear: DragonPoint
    public var mouth: DragonPoint
    public var neck: DragonPoint
    public var pawFar: DragonPoint
    public var pawNear: DragonPoint
    public var shoulderFar: DragonPoint
    public var shoulderNear: DragonPoint
    public var wingFar: DragonPoint
    public var wingNear: DragonPoint
}

public struct DragonEye: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var rx: Double
    public var ry: Double
}

public struct DragonAtlas: Equatable, Sendable {
    public var joints: DragonJoints
    public var eyeNear: DragonEye
    public var eyeFar: DragonEye
    public var layers: [DragonLayer: DragonLayerFrame]
    public var props: [String: DragonPropFrame]
    public var rigScale: Double
    public var units: Double
    public var tail: DragonTailMeta
    public var version: Int

    public enum Failure: Error, Equatable {
        case malformed(String)
    }

    public init(json: Data) throws {
        guard let root = try JSONSerialization.jsonObject(with: json) as? [String: Any] else { throw Failure.malformed("root") }
        func numbers(_ value: Any?, _ count: Int, _ name: String) throws -> [Double] {
            guard let list = value as? [Any] else { throw Failure.malformed(name) }
            let values: [Double] = list.compactMap { ($0 as? NSNumber)?.doubleValue }
            guard values.count == list.count, values.count >= count else { throw Failure.malformed(name) }
            return values
        }
        func number(_ dictionary: [String: Any], _ key: String, _ name: String) throws -> Double {
            guard let value = (dictionary[key] as? NSNumber)?.doubleValue else { throw Failure.malformed("\(name).\(key)") }
            return value
        }
        guard let joints = root["joints"] as? [String: Any] else { throw Failure.malformed("joints") }
        func joint(_ key: String) throws -> DragonPoint {
            let pair: [Double] = try numbers(joints[key], 2, "joints.\(key)")
            return DragonPoint(pair[0], pair[1])
        }
        self.joints = DragonJoints(
            axis: try joint("axis"), ear: try joint("ear"), footFar: try joint("footFar"), footNear: try joint("footNear"),
            hip: try joint("hip"), hipFar: try joint("hipFar"), hipNear: try joint("hipNear"), mouth: try joint("mouth"),
            neck: try joint("neck"), pawFar: try joint("pawFar"), pawNear: try joint("pawNear"),
            shoulderFar: try joint("shoulderFar"), shoulderNear: try joint("shoulderNear"), wingFar: try joint("wingFar"),
            wingNear: try joint("wingNear"))
        guard let eyes = root["eyes"] as? [String: Any] else { throw Failure.malformed("eyes") }
        func eye(_ key: String) throws -> DragonEye {
            let v: [Double] = try numbers(eyes[key], 4, "eyes.\(key)")
            return DragonEye(x: v[0], y: v[1], rx: v[2], ry: v[3])
        }
        self.eyeNear = try eye("eyeNear")
        self.eyeFar = try eye("eyeFar")
        guard let rig = root["rig"] as? [String: Any] else { throw Failure.malformed("rig") }
        var layers: [DragonLayer: DragonLayerFrame] = [:]
        for layer in DragonLayer.allCases {
            guard let entry = rig[layer.rawValue] as? [String: Any] else { throw Failure.malformed("rig.\(layer.rawValue)") }
            layers[layer] = DragonLayerFrame(
                x: try number(entry, "x", layer.rawValue), y: try number(entry, "y", layer.rawValue),
                w: try number(entry, "w", layer.rawValue), h: try number(entry, "h", layer.rawValue),
                ox: (entry["ox"] as? NSNumber)?.doubleValue ?? 0, oy: (entry["oy"] as? NSNumber)?.doubleValue ?? 0,
                s: (entry["s"] as? NSNumber)?.doubleValue ?? 1)
        }
        self.layers = layers
        guard let props = root["props"] as? [String: Any] else { throw Failure.malformed("props") }
        var table: [String: DragonPropFrame] = [:]
        for (name, value) in props {
            guard let entry = value as? [String: Any] else { throw Failure.malformed("props.\(name)") }
            table[name] = DragonPropFrame(
                x: try number(entry, "x", name), y: try number(entry, "y", name), w: try number(entry, "w", name),
                h: try number(entry, "h", name), k: try number(entry, "k", name))
        }
        self.props = table
        self.rigScale = try number(root, "rigScale", "atlas")
        self.units = try number(root, "units", "atlas")
        self.version = Int((root["version"] as? NSNumber)?.intValue ?? 0)
        guard let tail = root["tail"] as? [String: Any], let curve = tail["curve"] as? [Any] else { throw Failure.malformed("tail") }
        let points: [DragonPoint] = try curve.map { (item: Any) -> DragonPoint in
            let pair: [Double] = try numbers(item, 2, "tail.curve")
            return DragonPoint(pair[0], pair[1])
        }
        guard points.count > 8 else { throw Failure.malformed("tail.curve") }
        self.tail = DragonTailMeta(
            curve: points, inner: try number(tail, "inner", "tail"), outer: try number(tail, "outer", "tail"),
            length: try number(tail, "length", "tail"))
    }

    public func layer(_ name: DragonLayer) -> DragonLayerFrame {
        layers[name] ?? DragonLayerFrame(x: 0, y: 0, w: 1, h: 1, ox: 0, oy: 0, s: 1)
    }
}
