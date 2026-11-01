import Foundation

public enum GenieMascot: String, CaseIterable, Sendable {
    case spider, dragon

    public nonisolated static let key: String = "genie/mascot"

    public init(stored: String?) {
        self = stored.flatMap { GenieMascot(rawValue: $0) } ?? .spider
    }

    public var title: String {
        switch self {
        case .spider: return "Genie spider"
        case .dragon: return "Cloud Dragon"
        }
    }

    public var name: String {
        switch self {
        case .spider: return "Genie"
        case .dragon: return "Cloud Dragon"
        }
    }

    public var base: Double {
        switch self {
        case .spider: return 92
        case .dragon: return 96
        }
    }

    public var other: GenieMascot { self == .spider ? .dragon : .spider }
}

public struct DragonNap: Equatable, Sendable {
    public nonisolated static let quiet: Double = 45

    public private(set) var ambient: DragonActivity?
    public private(set) var age: Double = 0
    public private(set) var quietSince: Double

    public init(now: Double) {
        quietSince = now
    }

    public mutating func step(now: Double, dt: Double, poked: Bool, awake: Bool, calm: Bool) {
        if ambient == .stretch {
            age += dt
            if age >= DragonActivity.stretch.duration {
                ambient = nil
                quietSince = now
            }
            return
        }
        if ambient == .sleep {
            age += dt
            if poked || !calm {
                ambient = poked || calm ? .stretch : nil
                age = 0
                if ambient == nil { quietSince = now }
            }
            return
        }
        if poked || awake {
            quietSince = now
        } else if calm && now - quietSince > DragonNap.quiet {
            ambient = .sleep
            age = 0
        }
    }

    public mutating func rouse(now: Double) {
        ambient = nil
        age = 0
        quietSince = now
    }

    public mutating func lullaby(now: Double) {
        ambient = .sleep
        age = 0
        quietSince = now
    }

    public var asleep: Bool { ambient == .sleep }
}

public enum DragonTrick {
    public static func overlay(_ frame: GenieTrickFrame) -> DragonTarget {
        typealias M = DragonMath
        let k: Double = M.clamp01(frame.rise)
        guard k > 0.01 else { return [:] }
        let age: Double = frame.age
        var p: DragonTarget = [:]
        switch frame.legs {
        case .rest:
            break
        case .wave:
            p.put(.pawF, 225 + sin(age * 13) * 40, -310)
            p[.pawFa] = sin(age * 13) * 0.4
            p[.front] = 1
        case .cheer:
            p.put(.pawN, -210, -320)
            p.put(.pawF, 210, -320)
            p[.front] = 1
        case .dance:
            let lift: Double = max(0, sin(age * 9))
            p.put(.footN, 0, -lift * 34)
            p.put(.footF, 0, -max(0, -sin(age * 9)) * 34)
            p[.y] = -abs(sin(age * 9)) * 14
            p[.lean] = sin(age * 4.5) * 0.06
        case .march:
            p.put(.footN, 0, -max(0, sin(age * 12)) * 30)
            p.put(.footF, 0, -max(0, -sin(age * 12)) * 30)
        case .drum:
            p.put(.pawN, 150, 40 - max(0, sin(age * 22)) * 40)
            p.put(.pawF, 100, 60 - max(0, -sin(age * 22)) * 40)
        case .swim:
            p.put(.pawN, 150 + cos(age * 6) * 70, -40 + sin(age * 6) * 60)
            p.put(.pawF, 120 + cos(age * 6 + 3) * 70, -40 + sin(age * 6 + 3) * 60)
        case .splay:
            p.put(.pawN, -210, -130)
            p.put(.pawF, 230, -130)
            p[.front] = 1
        case .tuck:
            p[.crouch] = 0.8
            p.put(.pawN, 60, 40)
            p.put(.pawF, 60, 40)
        case .coverEyes:
            p[.reach] = 1
            p.put(.pawN, 60, -140)
            p.put(.pawF, 200, -150)
            p[.front] = 1
        case .clap:
            let together: Double = abs(sin(age * 8))
            p.put(.pawN, 120 + 120 * together, -90)
            p.put(.pawF, 120 + 120 * together, -110)
            p[.front] = 1
        case .flex:
            p.put(.pawN, -90, -330)
            p[.wing] = 0.2
            p[.front] = 1
        case .flap:
            p[.flap] = 1
            p.put(.pawN, -180, -200)
            p.put(.pawF, 180, -200)
            p[.front] = 1
        case .reach:
            p.put(.pawN, 200, -420 + sin(age * 5) * 20)
            p[.front] = 1
        case .shrug:
            p.put(.pawN, -120, -120 + sin(age * 6) * 12)
            p.put(.pawF, 150, -120 + sin(age * 6) * 12)
            p[.head] = 0.08
            p[.front] = 1
        case .typing:
            p.put(.pawN, 120, 40 - max(sin(age * 19), 0) * 22)
            p.put(.pawF, 85, 70 - max(sin(age * 19 + 3.1), 0) * 22)
            p[.crouch] = 0.2
        case .guitar:
            p.put(.pawN, 160, -10)
            p.put(.pawF, 120 + sin(age * 18) * 40, 60)
        case .conduct:
            p.put(.pawF, 200 + sin(age * 5) * 80, -300 + sin(age * 10) * 40)
            p[.front] = 1
        case .salute:
            p.put(.pawF, 170, -380)
            p[.front] = 1
        case .point:
            p.put(.pawN, 440, -90)
            p[.front] = 1
        case .wiggle:
            p.put(.pawN, 40 + sin(age * 16) * 40, -40 + cos(age * 13) * 30)
            p.put(.pawF, 30 + sin(age * 15 + 1) * 40, -40 + cos(age * 12) * 30)
        case .hug:
            p.put(.pawN, 150, -60)
            p.put(.pawF, 40, -80)
            p[.front] = 1
        }
        switch frame.eyes {
        case .normal:
            break
        case .wide:
            p[.oval] = 0.8
            p[.open] = max(p[.open] ?? 0, 0.3)
            p[.gy] = -0.2
        case .closed:
            p[.lid] = 1
        case .wink:
            p[.lidN] = 1
        case .hearts, .stars:
            p[.lid] = 1
            p[.happy] = 1
        case .spiral, .cross:
            p[.head] = sin(age * 9) * 0.1
            p[.gx] = sin(age * 9)
            p[.gy] = cos(age * 9)
        case .squint:
            p[.lid] = 0.55
        case .roll:
            p[.gx] = cos(age * 6)
            p[.gy] = sin(age * 6)
        case .sleepy:
            p[.lid] = 0.62
        case .up:
            p[.gy] = -1
        case .down:
            p[.gy] = 1
        }
        switch frame.mouth {
        case .normal, .grin, .frown, .flat:
            break
        case .o:
            p[.open] = 0.5
            p[.oval] = 1
        case .whistle:
            p[.open] = 0.25
            p[.oval] = 1
        case .tongue:
            p[.open] = 0.3
        case .yawn:
            p[.open] = 1
            p[.oval] = 0.55
            p[.head] = -0.2
        case .chatter:
            p[.open] = abs(sin(age * 24)) * 0.5
        case .open:
            p[.open] = 0.8
        }
        for (key, value) in p {
            switch key {
            case .front, .reach, .oval:
                p[key] = value * k > 0.5 ? value : 0
            case .sway:
                break
            default:
                p[key] = value * k
            }
        }
        return p
    }
}
