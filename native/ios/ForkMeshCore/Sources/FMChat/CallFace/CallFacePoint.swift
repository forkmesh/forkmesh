import Foundation

public struct CallFacePoint: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var z: Double?

    public init(_ x: Double, _ y: Double, _ z: Double? = nil) {
        self.x = x
        self.y = y
        self.z = z
    }

    public init(_ values: [Double]) {
        x = values.count > 0 ? values[0] : .nan
        y = values.count > 1 ? values[1] : .nan
        z = values.count > 2 ? values[2] : nil
    }

    public var dims: Int { z == nil ? 2 : 3 }
    public var flat: CallFacePoint { CallFacePoint(x, y) }
    public var values: [Double] { z.map { [x, y, $0] } ?? [x, y] }
    var depth: Double { z ?? .nan }
}

enum CallFaceJS {
    static func hypot(_ a: Double, _ b: Double) -> Double {
        if a.isNaN || b.isNaN { return a.isInfinite || b.isInfinite ? .infinity : .nan }
        let x = abs(a)
        let y = abs(b)
        let top = x > y ? x : y
        if top == .infinity { return .infinity }
        if top == 0 { return 0 }
        var sum = 0.0
        var compensation = 0.0
        for value in [x, y] {
            let n = value / top
            let summand = n * n - compensation
            let preliminary = sum + summand
            compensation = (preliminary - sum) - summand
            sum = preliminary
        }
        return sum.squareRoot() * top
    }

    static func hypot(_ a: Double, _ b: Double, _ c: Double) -> Double {
        let values = [abs(a), abs(b), abs(c)]
        var top = 0.0
        var nan = false
        for value in values {
            if value.isNaN { nan = true } else if value > top { top = value }
        }
        if top == .infinity { return .infinity }
        if nan { return .nan }
        if top == 0 { return 0 }
        var sum = 0.0
        var compensation = 0.0
        for value in values {
            let n = value / top
            let summand = n * n - compensation
            let preliminary = sum + summand
            compensation = (preliminary - sum) - summand
            sum = preliminary
        }
        return sum.squareRoot() * top
    }

    static func round(_ x: Double) -> Double {
        guard x.isFinite else { return x }
        let floor = x.rounded(.down)
        return x - floor >= 0.5 ? floor + 1 : floor
    }

    static func min(_ a: Double, _ b: Double) -> Double {
        if a.isNaN || b.isNaN { return .nan }
        return a < b ? a : b
    }

    static func max(_ a: Double, _ b: Double) -> Double {
        if a.isNaN || b.isNaN { return .nan }
        return a > b ? a : b
    }

    static func max(_ values: Double...) -> Double {
        var out = -Double.infinity
        for value in values { out = max(out, value) }
        return out
    }

    static func or(_ value: Double, _ fallback: Double) -> Double {
        value == 0 || value.isNaN ? fallback : value
    }

    static func truthy(_ value: Double?) -> Bool {
        guard let value else { return false }
        return value != 0 && !value.isNaN
    }

    static func sign(_ value: Double) -> Double {
        value > 0 ? 1 : value < 0 ? -1 : value
    }

    static func sorted<T>(_ items: [T], by compare: (T, T) -> Double) -> [T] {
        items.enumerated().sorted { left, right in
            let order = compare(left.element, right.element)
            if order < 0 { return true }
            if order > 0 { return false }
            return left.offset < right.offset
        }.map(\.element)
    }
}
