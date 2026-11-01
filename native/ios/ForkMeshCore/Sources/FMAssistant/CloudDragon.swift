import Foundation

public enum DragonMath {
    public nonisolated static let tau: Double = Double.pi * 2

    public static func clamp(_ v: Double, _ lo: Double, _ hi: Double) -> Double { min(max(v, lo), hi) }

    public static func clamp01(_ v: Double) -> Double { clamp(v, 0, 1) }

    public static func smooth(_ v: Double) -> Double {
        let u: Double = clamp01(v)
        return u * u * (3 - 2 * u)
    }

    public static func back(_ v: Double) -> Double {
        let u: Double = clamp01(v) - 1
        return 1 + 2.7 * u * u * u + 1.7 * u * u
    }

    public static func arc(_ v: Double) -> Double { sin(Double.pi * clamp01(v)) }

    public static func seed(_ n: Double) -> Double {
        let x: Double = sin(n * 127.1 + 311.7) * 43758.5453
        return x - x.rounded(.down)
    }

    public static func seed(_ n: Int) -> Double { seed(Double(n)) }

    public static func span(_ u: Double, _ from: Double, _ to: Double) -> Double { clamp01((u - from) / (to - from)) }

    public static func wave(_ age: Double, _ speed: Double, _ phase: Double = 0) -> Double { sin(age * speed + phase) }

    public static func bump(_ u: Double, _ from: Double, _ to: Double) -> Double { arc(span(u, from, to)) }

    public static func mixed(_ a: Double, _ b: Double, _ k: Double) -> Double { a + (b - a) * k }

    public static func wrapped(_ angle: Double) -> Double { atan2(sin(angle), cos(angle)) }

    public static func fraction(_ v: Double) -> Double { v - v.rounded(.down) }
}

public enum DragonKey: Int, CaseIterable, Sendable {
    case x, y, lean, crouch, chest, head, hx, hy, wing, flap, tail, tailBase, sway
    case open, oval, lid, lidN, lidF, happy, gx, gy, gh, gv, ear, jitter, pawNa, pawFa, reach
    case pawNx, pawNy, pawFx, pawFy, footNx, footNy, footFx, footFy, ground, front

    public var initial: Double { self == .sway ? 1 : 0 }

    public var stiffness: (k: Double, c: Double) {
        switch self {
        case .lid, .lidN, .lidF, .happy, .open, .jitter, .pawNa, .pawFa, .gx, .gy: return (520, 36)
        case .tail, .tailBase, .wing, .flap, .sway, .gh, .gv, .ear: return (58, 7.5)
        case .pawNx, .pawNy, .pawFx, .pawFy, .footNx, .footNy, .footFx, .footFy: return (250, 20)
        default: return (190, 15.5)
        }
    }
}

public typealias DragonTarget = [DragonKey: Double]

public enum DragonPair: Sendable {
    case pawN, pawF, footN, footF

    public var keys: (x: DragonKey, y: DragonKey) {
        switch self {
        case .pawN: return (.pawNx, .pawNy)
        case .pawF: return (.pawFx, .pawFy)
        case .footN: return (.footNx, .footNy)
        case .footF: return (.footFx, .footFy)
        }
    }
}

extension Dictionary where Key == DragonKey, Value == Double {
    public mutating func put(_ pair: DragonPair, _ x: Double, _ y: Double) {
        self[pair.keys.x] = x
        self[pair.keys.y] = y
    }

    public mutating func overlay(_ other: [DragonKey: Double]) {
        for (key, value) in other { self[key] = value }
    }
}

public struct DragonPose: Equatable, Sendable {
    public private(set) var values: [Double]

    public static let sit: DragonPose = DragonPose(values: DragonKey.allCases.map { $0.initial })

    public init(values: [Double]) {
        self.values = values
    }

    public subscript(key: DragonKey) -> Double {
        get { values[key.rawValue] }
        set { values[key.rawValue] = newValue }
    }

    public mutating func overlay(_ other: [DragonKey: Double]) {
        for (key, value) in other { values[key.rawValue] = value }
    }
}

public struct DragonFlips: Equatable, Sendable {
    public var head: Double = 1
    public var body: Double = 1
    public var wing: Double = 1
    public var tail: Double = 1

    public init() {}

    public static func seen(_ f: Double) -> Double { (f < 0 ? -1 : 1) * (0.56 + 0.44 * abs(f)) }
}

public enum DragonReaction: String, CaseIterable, Sendable {
    case wave, cheer, nod, eureka, giggle, surprise, dizzy

    public var duration: Double { 1.4 }

    public func pose(age: Double) -> DragonTarget {
        var p: DragonTarget = [:]
        switch self {
        case .wave:
            p.put(.pawF, 225 + sin(age * 13) * 40, -310)
            p[.pawFa] = sin(age * 13) * 0.4
            p[.open] = 0.5
            p[.lean] = -0.03
            p[.gx] = 1
            p[.front] = 1
        case .cheer:
            p.put(.pawN, -210, -320)
            p.put(.pawF, 210, -320)
            p[.y] = -90 * abs(sin(age * 8))
            p[.open] = 1
            p[.lid] = 1
            p[.happy] = 1
            p[.wing] = 0.3
            p[.front] = 1
        case .nod:
            p[.head] = sin(age * 11) * 0.12 * (1 - DragonMath.clamp01(age))
        case .eureka:
            p.put(.pawF, 215, -330)
            p[.gy] = -1
            p[.open] = 0.7
            p[.y] = -50 * DragonMath.arc(age / 0.5)
            p[.front] = 1
        case .giggle:
            p[.lid] = 1
            p[.happy] = 1
            p[.open] = 0.8
            p[.head] = sin(age * 22) * 0.05
            p[.y] = -abs(sin(age * 17)) * 20
            p.put(.pawN, 60, -120)
        case .surprise:
            p[.y] = -110 * DragonMath.arc(age / 0.34)
            p[.open] = 0.7
            p[.oval] = 1
            p[.wing] = 0.35
            p.put(.pawN, 40, -60)
            p.put(.pawF, 40, -60)
        case .dizzy:
            p[.head] = sin(age * 9) * 0.14
            p[.hx] = cos(age * 9) * 16
            p[.gx] = sin(age * 9)
            p[.gy] = cos(age * 9)
            p[.lean] = sin(age * 5) * 0.05
        }
        return p
    }
}

public struct DragonHabit: Equatable, Sendable {
    public let name: String
    public let time: Double
    public let sneeze: Double?

    public static let all: [DragonHabit] = [
        DragonHabit(name: "yawn", time: 2.1, sneeze: nil),
        DragonHabit(name: "sneeze", time: 1.3, sneeze: 0.56),
        DragonHabit(name: "look", time: 2.6, sneeze: nil),
        DragonHabit(name: "tilt", time: 1.7, sneeze: nil),
        DragonHabit(name: "wings", time: 1.7, sneeze: nil),
        DragonHabit(name: "flick", time: 1.2, sneeze: nil),
        DragonHabit(name: "lick", time: 2.4, sneeze: nil),
        DragonHabit(name: "shift", time: 1, sneeze: nil),
        DragonHabit(name: "hop", time: 0.9, sneeze: nil),
    ]

    public static func named(_ name: String) -> DragonHabit? { all.first { $0.name == name } }

    public static func title(_ name: String) -> String {
        switch name {
        case "yawn": return "Yawns"
        case "sneeze": return "Sneezes a puff of smoke"
        case "look": return "Looks around"
        case "tilt": return "Tilts its head"
        case "wings": return "Stretches its wings"
        case "flick": return "Flicks its tail"
        case "lick": return "Licks its paw"
        case "shift": return "Shifts its weight"
        default: return "Hops a little"
        }
    }

    public func pose(_ u: Double) -> DragonTarget {
        typealias M = DragonMath
        var p: DragonTarget = [:]
        switch name {
        case "yawn":
            p[.open] = M.bump(u, 0.1, 0.85)
            p[.oval] = 0.55
            p[.lid] = M.bump(u, 0.1, 0.9)
            p[.head] = -0.2 * M.bump(u, 0.05, 0.9)
            p[.wing] = 0.14 * M.bump(u, 0.1, 0.9)
            p[.chest] = 0.04 * M.bump(u, 0.1, 0.8)
            p[.ear] = -0.1 * M.bump(u, 0.1, 0.9)
        case "sneeze":
            p[.head] = -0.18 * M.bump(u, 0, 0.56) + 0.24 * M.bump(u, 0.54, 0.8)
            p[.open] = 0.4 * M.bump(u, 0, 0.56) + 0.5 * M.bump(u, 0.54, 0.74)
            p[.oval] = 1
            p[.lid] = M.span(u, 0.42, 0.56) * (1 - M.span(u, 0.72, 0.84))
            p[.y] = 30 * M.bump(u, 0.54, 0.8)
            p[.wing] = 0.2 * M.bump(u, 0.54, 0.8)
            p[.ear] = 0.14 * M.bump(u, 0.54, 0.8)
        case "look":
            let side: Double = u < 0.5 ? -0.9 : 0.8
            p[.gx] = side
            p[.gy] = -0.2
            p[.gh] = side * M.bump(u, 0, 1) * 1.1
            p[.ear] = 0.08
        case "tilt":
            p[.head] = 0.17 * M.bump(u, 0, 1)
            p[.gy] = 0.1
            p[.ear] = 0.13 * M.bump(u, 0, 1)
        case "wings":
            p[.wing] = 0.5 * M.bump(u, 0, 0.7)
            p[.lean] = -0.03 * M.bump(u, 0, 0.7)
            p[.flap] = 0.32 * M.bump(u, 0.55, 1)
        case "flick":
            p[.tail] = 0.55 * M.bump(u, 0, 1)
            p[.sway] = 2 + 3 * M.bump(u, 0, 1)
        case "lick":
            p.put(.pawN, 78 * M.bump(u, 0, 1), -285 * M.bump(u, 0, 1))
            p[.head] = 0.1 * M.bump(u, 0, 1)
            p[.lid] = M.bump(u, 0.15, 0.9)
            p[.open] = 0.32 * max(M.wave(u * 2.4, 16), 0) * M.bump(u, 0.2, 0.85)
            p[.front] = M.bump(u, 0, 1)
        case "shift":
            p.put(.footN, 0, -34 * M.bump(u, 0, 1))
            p[.x] = 16 * M.bump(u, 0, 1)
            p[.lean] = 0.03 * M.bump(u, 0, 1)
        default:
            let hop: Double = M.bump(u, 0.15, 0.8)
            p[.y] = -58 * hop
            p[.crouch] = 0.3 * M.bump(u, 0, 0.3) + 0.25 * M.bump(u, 0.75, 1)
            p[.open] = 0.5 * hop
            p.put(.footN, 0, -18 * hop)
            p.put(.footF, 0, -18 * hop)
            p[.ear] = 0.1 * hop
        }
        return p
    }

    public static func at(_ t: Double) -> (habit: DragonHabit, u: Double)? {
        let n: Double = (t / 5.5).rounded(.down)
        let start: Double = n * 5.5 + DragonMath.seed(n + 70) * 2
        let habit: DragonHabit = all[Int(DragonMath.seed(n + 90) * Double(all.count))]
        let u: Double = (t - start) / habit.time
        return u > 0 && u < 1 ? (habit, u) : nil
    }

    public static func blink(at t: Double) -> Double {
        let period: Double = 3.4
        let n: Double = (t / period).rounded(.down)
        let start: Double = n * period + DragonMath.seed(n) * 2.2
        func one(_ at: Double) -> Double {
            let u: Double = (t - at) / 0.17
            return u > 0 && u < 1 ? sin(Double.pi * u) : 0
        }
        return max(one(start), DragonMath.seed(n + 50) < 0.3 ? one(start + 0.3) : 0)
    }
}
