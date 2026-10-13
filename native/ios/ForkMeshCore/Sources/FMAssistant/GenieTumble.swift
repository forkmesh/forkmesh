import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public struct GenieKnock: Equatable, Sendable {
    public var surface: GenieEdge
    public var speed: Double

    public init(surface: GenieEdge, speed: Double) {
        self.surface = surface
        self.speed = speed
    }

    public var dizzying: Bool { speed >= GenieTumble.dizzy }
}

public struct GenieTumble: Equatable, Sendable {
    public nonisolated static let gain: Double = 3
    public nonisolated static let loosen: Double = 0.25
    public nonisolated static let calm: Double = 0.12
    public nonisolated static let calmFor: Double = 2.5
    public nonisolated static let restFor: Double = 0.8
    public nonisolated static let longest: Double = 20
    public nonisolated static let sinking: Double = 0.18
    public nonisolated static let inertia: Double = 0.4
    public nonisolated static let viscosity: Double = 3.5
    public nonisolated static let thickness: Double = 0.0015
    public nonisolated static let sloshShare: Double = 0.25
    public nonisolated static let sloshPeriod: Double = 1.6
    public nonisolated static let sloshDamping: Double = 1.6
    public nonisolated static let eddy: Double = 22
    public nonisolated static let stirred: Double = 0.3
    public nonisolated static let maxStir: Double = 80
    public nonisolated static let eddySize: Double = 120
    public nonisolated static let grip: Double = 0.5
    public nonisolated static let bounce: Double = 0.3
    public nonisolated static let glance: Double = 0.85
    public nonisolated static let thud: Double = 160
    public nonisolated static let maxSpeed: Double = 3200
    public nonisolated static let dizzy: Double = 800
    public nonisolated static let spinKick: Double = 0.35
    public nonisolated static let spinDecay: Double = 1.4
    public nonisolated static let swirl: Double = 0.6
    public nonisolated static let righting: Double = 1.6
    public nonisolated static let weightless: Double = 0.3
    public nonisolated static let swirlRate: Double = 120
    public nonisolated static let lookahead: Double = 0.05

    public var point: CGPoint
    public var velocity: CGSize
    public var turn: Double
    public var spin: Double = 0
    public var touching: GenieEdge?
    public var quiet: Double = 0
    public var rested: Double = 0
    public var since: Double = .infinity
    public var age: Double = 0
    public var air: Double
    public var force: CGSize = .zero
    public var slosh: CGSize
    public var flow: CGSize = .zero
    public var current: CGSize = .zero
    public var floating: Bool = false

    public init(point: CGPoint, velocity: CGSize, turn: Double, touching: GenieEdge?, gravity: CGSize = GenieGravity.down) {
        self.point = point
        self.velocity = velocity
        self.turn = turn
        self.touching = touching
        air = touching == nil ? 1 : 0
        slosh = GenieTumble.level(gravity)
        floating = GenieTumble.weightless(gravity)
    }

    public static func strength(_ push: CGSize) -> Double {
        Double(hypot(push.width, push.height))
    }

    public static func loosens(_ push: CGSize) -> Bool {
        strength(push) > loosen
    }

    public static func weightless(_ gravity: CGSize) -> Bool {
        strength(gravity) < weightless
    }

    public static func felt(push: CGSize, gravity: CGSize) -> CGSize {
        CGSize(width: Double(gravity.width) - Double(push.width) * gain, height: Double(gravity.height) - Double(push.height) * gain)
    }

    public static func force(push: CGSize, gravity: CGSize) -> CGSize {
        let pull: Double = GenieGravity.pull
        let shove: Double = gain * inertia
        return CGSize(
            width: (Double(gravity.width) * sinking - Double(push.width) * shove) * pull,
            height: (Double(gravity.height) * sinking - Double(push.height) * shove) * pull)
    }

    static var stiffness: Double {
        let w: Double = 2 * Double.pi / sloshPeriod
        return w * w
    }

    static func level(_ gravity: CGSize) -> CGSize {
        let k: Double = GenieGravity.pull * sloshShare / stiffness
        return CGSize(width: Double(gravity.width) * k, height: Double(gravity.height) * k)
    }

    public static func swirls(from old: CGSize, to new: CGSize, seconds: Double) -> Bool {
        guard seconds > 0, strength(old) > 0.4, strength(new) > 0.4 else { return false }
        let a: Double = atan2(Double(old.height), Double(old.width))
        let b: Double = atan2(Double(new.height), Double(new.width))
        let turned: Double = abs(GenieGravity.turn(from: a * 180 / Double.pi, to: b * 180 / Double.pi))
        return turned / seconds > swirlRate
    }

    public static func eddies(at point: CGPoint, age: Double, strength: Double) -> (flow: CGSize, curl: Double) {
        let x: Double = Double(point.x)
        let y: Double = Double(point.y)
        let a1: Double = 1 / eddySize
        let b1: Double = 1 / (eddySize * 1.3)
        let a2: Double = 1 / (eddySize * 0.7)
        let b2: Double = 1 / (eddySize * 0.9)
        let p1: Double = x * a1 + age * 0.31
        let q1: Double = y * b1 - age * 0.23
        let p2: Double = x * a2 - age * 0.17 + 1.7
        let q2: Double = y * b2 + age * 0.29 + 0.6
        let amp1: Double = strength / a1
        let amp2: Double = strength * 0.6 / a2
        let u: Double = -amp1 * b1 * sin(p1) * sin(q1) - amp2 * b2 * sin(p2) * sin(q2)
        let v: Double = -amp1 * a1 * cos(p1) * cos(q1) - amp2 * a2 * cos(p2) * cos(q2)
        let curl: Double = amp1 * (a1 * a1 + b1 * b1) * sin(p1) * cos(q1) + amp2 * (a2 * a2 + b2 * b2) * sin(p2) * cos(q2)
        return (CGSize(width: u, height: v), curl)
    }

    public var speed: Double { Double(hypot(velocity.width, velocity.height)) }

    public var done: Bool {
        rested >= GenieTumble.restFor || (floating && quiet >= GenieTumble.calmFor) || age >= GenieTumble.longest
    }

    public var gait: GenieGait {
        let land: Double = since < GenieGravity.landing ? sin(Double.pi * since / GenieGravity.landing) : 0
        return GenieGait(phase: age * 7, air: air, land: land)
    }

    public func point(after seconds: Double, in arena: GenieArena) -> CGPoint {
        let t: CGFloat = CGFloat(min(max(seconds, 0), GenieTumble.lookahead))
        let ahead: CGPoint = CGPoint(
            x: point.x + velocity.width * t + force.width * t * t / 2, y: point.y + velocity.height * t + force.height * t * t / 2)
        return GenieCrawl.clamp(ahead, to: arena.box)
    }

    public func rotation(after seconds: Double) -> Double {
        turn + spin * min(max(seconds, 0), GenieTumble.lookahead)
    }

    public mutating func advance(by seconds: Double, push: CGSize, gravity: CGSize, arena: GenieArena) -> [GenieKnock] {
        let felt: CGSize = GenieTumble.felt(push: push, gravity: gravity)
        let body: CGSize = GenieTumble.force(push: push, gravity: gravity)
        let total: Double = max(seconds, 0)
        floating = GenieTumble.weightless(gravity)
        quiet = GenieTumble.strength(push) < GenieTumble.calm ? quiet + total : 0
        var knocks: [GenieKnock] = []
        var left: Double = total
        while left > 1e-6 {
            let dt: Double = min(left, GenieGravity.step)
            left -= dt
            if let knock = tick(dt, felt: felt, body: body, gravity: gravity, arena: arena) { knocks.append(knock) }
        }
        let still: Bool = touching != nil && speed < GenieGravity.rest * 2 && !GenieTumble.loosens(push)
        rested = still ? rested + total : 0
        return knocks
    }

    private mutating func stir(_ dt: Double, felt: CGSize) {
        let drive: Double = GenieGravity.pull * GenieTumble.sloshShare
        let k: Double = GenieTumble.stiffness
        let c: Double = GenieTumble.sloshDamping
        var fx: Double = Double(flow.width)
        var fy: Double = Double(flow.height)
        fx += (Double(felt.width) * drive - k * Double(slosh.width) - c * fx) * dt
        fy += (Double(felt.height) * drive - k * Double(slosh.height) - c * fy) * dt
        flow = CGSize(width: fx, height: fy)
        slosh = CGSize(width: Double(slosh.width) + fx * dt, height: Double(slosh.height) + fy * dt)
    }

    private mutating func tick(_ dt: Double, felt: CGSize, body: CGSize, gravity: CGSize, arena: GenieArena) -> GenieKnock? {
        stir(dt, felt: felt)
        let box: CGRect = arena.box
        let churn: Double = min(Double(hypot(flow.width, flow.height)) * GenieTumble.stirred, GenieTumble.maxStir)
        let (swirl, curl): (CGSize, Double) = GenieTumble.eddies(at: point, age: age, strength: GenieTumble.eddy + churn)
        current = CGSize(width: flow.width + swirl.width, height: flow.height + swirl.height)
        let rx: Double = Double(current.width - velocity.width)
        let ry: Double = Double(current.height - velocity.height)
        let rel: Double = hypot(rx, ry)
        let drag: Double = GenieTumble.viscosity + GenieTumble.thickness * rel
        let ax: Double = Double(body.width) + rx * drag
        let ay: Double = Double(body.height) + ry * drag
        force = CGSize(width: ax, height: ay)
        let ease: Double = min(drag * dt, 1)
        var vx: Double = Double(velocity.width) + Double(body.width) * dt + rx * ease
        var vy: Double = Double(velocity.height) + Double(body.height) * dt + ry * ease
        if let on = touching {
            let n: CGSize = GenieArena.normal(on)
            let press: Double = ax * Double(n.width) + ay * Double(n.height)
            if press <= 0 || !arena.touches(point, on) {
                touching = nil
            } else {
                let into: Double = vx * Double(n.width) + vy * Double(n.height)
                if into > 0 {
                    vx -= into * Double(n.width)
                    vy -= into * Double(n.height)
                }
                let along: Double = hypot(vx, vy)
                if along > 0 {
                    let slower: Double = max(along - GenieTumble.grip * press * dt, 0)
                    vx *= slower / along
                    vy *= slower / along
                }
            }
        }
        let fast: Double = hypot(vx, vy)
        if fast > GenieTumble.maxSpeed {
            vx *= GenieTumble.maxSpeed / fast
            vy *= GenieTumble.maxSpeed / fast
        }
        var p: CGPoint = CGPoint(x: point.x + CGFloat(vx * dt), y: point.y + CGFloat(vy * dt))
        age += dt
        since += dt
        var knock: GenieKnock?
        let hits: [(GenieEdge, Bool)] = [
            (.left, p.x < box.minX), (.right, p.x > box.maxX), (.top, p.y < box.minY), (.bottom, p.y > box.maxY),
        ]
        for (surface, hit) in hits where hit {
            let n: CGSize = GenieArena.normal(surface)
            let into: Double = vx * Double(n.width) + vy * Double(n.height)
            p = GenieCrawl.clamp(p, to: box)
            guard into > 0 else { continue }
            if into > GenieTumble.thud {
                let tx: Double = vx - into * Double(n.width)
                let ty: Double = vy - into * Double(n.height)
                vx = tx * GenieTumble.glance - into * GenieTumble.bounce * Double(n.width)
                vy = ty * GenieTumble.glance - into * GenieTumble.bounce * Double(n.height)
                spin += (Double(n.width) * ty - Double(n.height) * tx) * GenieTumble.spinKick
                touching = nil
                since = 0
                if into > (knock?.speed ?? 0) { knock = GenieKnock(surface: surface, speed: into) }
            } else {
                vx -= into * Double(n.width)
                vy -= into * Double(n.height)
                touching = surface
            }
        }
        point = p
        velocity = CGSize(width: vx, height: vy)
        if touching == nil { spin += curl * 180 / Double.pi * GenieTumble.swirl * dt }
        spin *= exp(-GenieTumble.spinDecay * dt)
        turn += spin * dt
        if let on = touching {
            turn += GenieGravity.turn(from: turn, to: on.degrees) * min(GenieTumble.righting * 3 * dt, 1)
        } else if !floating {
            turn += GenieGravity.turn(from: turn, to: GenieGravity.upright(gravity)) * min(GenieTumble.righting * dt, 1)
        }
        air += ((touching == nil ? 1 : 0) - air) * min(10 * dt, 1)
        return knock
    }
}
