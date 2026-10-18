import Foundation

public struct SynapseVector: Sendable, Equatable {
    public var x: Float
    public var y: Float
    public var z: Float

    public init(_ x: Float = 0, _ y: Float = 0, _ z: Float = 0) {
        self.x = x
        self.y = y
        self.z = z
    }

    public static let zero = SynapseVector()

    @inline(__always) public static func + (a: SynapseVector, b: SynapseVector) -> SynapseVector {
        SynapseVector(a.x + b.x, a.y + b.y, a.z + b.z)
    }

    @inline(__always) public static func - (a: SynapseVector, b: SynapseVector) -> SynapseVector {
        SynapseVector(a.x - b.x, a.y - b.y, a.z - b.z)
    }

    @inline(__always) public static prefix func - (a: SynapseVector) -> SynapseVector {
        SynapseVector(-a.x, -a.y, -a.z)
    }

    @inline(__always) public static func * (a: SynapseVector, s: Float) -> SynapseVector {
        SynapseVector(a.x * s, a.y * s, a.z * s)
    }

    @inline(__always) public static func / (a: SynapseVector, s: Float) -> SynapseVector {
        SynapseVector(a.x / s, a.y / s, a.z / s)
    }

    @inline(__always) public static func += (a: inout SynapseVector, b: SynapseVector) {
        a.x += b.x
        a.y += b.y
        a.z += b.z
    }

    @inline(__always) public static func -= (a: inout SynapseVector, b: SynapseVector) {
        a.x -= b.x
        a.y -= b.y
        a.z -= b.z
    }

    @inline(__always) public static func *= (a: inout SynapseVector, s: Float) {
        a.x *= s
        a.y *= s
        a.z *= s
    }

    @inline(__always) public var lengthSquared: Float { x * x + y * y + z * z }

    public var length: Float { SynapseMath.hypot(x, y, z) }

    public var normalized: SynapseVector {
        let len = length
        if SynapseMath.fuzzyIsNull(len - 1) { return self }
        if SynapseMath.fuzzyIsNull(len) { return SynapseVector() }
        return SynapseVector(x / len, y / len, z / len)
    }

    public mutating func normalize() {
        let len = length
        if SynapseMath.fuzzyIsNull(len - 1) || SynapseMath.fuzzyIsNull(len) { return }
        x /= len
        y /= len
        z /= len
    }

    @inline(__always) public static func dot(_ a: SynapseVector, _ b: SynapseVector) -> Float {
        a.x * b.x + a.y * b.y + a.z * b.z
    }

    @inline(__always) public static func cross(_ a: SynapseVector, _ b: SynapseVector) -> SynapseVector {
        SynapseVector(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)
    }
}

enum SynapseMath {
    static let goldenAngle: Float = 2.39996323

    @inline(__always) static func fuzzyIsNull(_ value: Float) -> Bool { abs(value) <= 0.00001 }

    static func hypot(_ a: Float, _ b: Float, _ c: Float) -> Float {
        let x = abs(a)
        let y = abs(b)
        let z = abs(c)
        let m = x < y ? (y < z ? z : y) : (x < z ? z : x)
        if m == 0 { return 0 }
        let p = x / m
        let q = y / m
        let r = z / m
        return m * (p * p + q * q + r * r).squareRoot()
    }

    @inline(__always) static func sin(_ x: Float) -> Float { sinf(x) }
    @inline(__always) static func cos(_ x: Float) -> Float { cosf(x) }
    @inline(__always) static func tan(_ x: Float) -> Float { tanf(x) }
    @inline(__always) static func atan2(_ y: Float, _ x: Float) -> Float { atan2f(y, x) }
    @inline(__always) static func asin(_ x: Float) -> Float { asinf(x) }
    @inline(__always) static func log(_ x: Float) -> Float { logf(x) }
    @inline(__always) static func log1p(_ x: Float) -> Float { log1pf(x) }
    @inline(__always) static func cbrt(_ x: Float) -> Float { cbrtf(x) }
    @inline(__always) static func pow(_ x: Float, _ y: Float) -> Float { powf(x, y) }

    @inline(__always) static func clamp(_ value: Float, _ low: Float, _ high: Float) -> Float {
        value < low ? low : (high < value ? high : value)
    }

    static func mixHash(_ input: UInt32) -> UInt32 {
        var value = input
        value ^= value >> 16
        value = value &* 0x7feb352d
        value ^= value >> 15
        value = value &* 0x846ca68b
        value ^= value >> 16
        return value
    }

    @inline(__always) static func unitFromHash(_ value: UInt32) -> Float {
        Float(value & 0xffffff) / Float(0xffffff)
    }

    static func jitter(_ seed: UInt32) -> SynapseVector {
        let u = unitFromHash(mixHash(seed ^ 0x9e3779b9))
        let v = unitFromHash(mixHash(seed ^ 0x85ebca6b))
        let w = unitFromHash(mixHash(seed ^ 0xc2b2ae35))
        let theta = u * 6.2831853
        let z = v * 2.0 - 1.0
        let r = max(0, 1 - z * z).squareRoot()
        let radius = cbrt(max(0.02, w))
        return SynapseVector(r * cos(theta), z, r * sin(theta)) * radius
    }
}
