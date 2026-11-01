import Foundation

public struct DragonHabitCue: Equatable, Sendable {
    public var name: String
    public var u: Double

    public init(name: String, u: Double) {
        self.name = name
        self.u = u
    }
}

public struct DragonScene: Sendable {
    public var t: Double = 0
    public var gaze: DragonPoint?
    public var held: Bool = false
    public var hover: Bool = false
    public var reduced: Bool = false
    public var air: Double = 0
    public var land: Double = 0
    public var walking: Double = 0
    public var vx: Double = 0
    public var vy: Double = 0
    public var stridePhase: Double = 0
    public var facing: Double = 0
    public var rest: Double = 0
    public var activity: DragonActivity?
    public var activityAge: Double = 0
    public var ambient: DragonActivity?
    public var ambientAge: Double = 0
    public var reaction: DragonReaction?
    public var reactionAge: Double = 0
    public var habit: DragonHabitCue?
    public var overlay: DragonTarget = [:]
    public var tired: Double = 0

    public init() {}

    public var current: (activity: DragonActivity, age: Double)? {
        if let ambient = ambient { return (ambient, ambientAge) }
        if let activity = activity { return (activity, activityAge) }
        return nil
    }
}

public struct DragonBody: Sendable {
    public private(set) var x: [Double]
    public private(set) var v: [Double]
    public private(set) var flips: DragonFlips = DragonFlips()
    public private(set) var facing: Double = 1
    public private(set) var turnAt: Double = -9
    public private(set) var last: Double?
    public private(set) var dt: Double = 0
    public private(set) var sneeze: Double?
    var tailState: DragonTailState = DragonTailState()

    public var tailAim: [Double] { tailState.aim }

    public var tailRest: [Double] { tailState.rest }

    public init() {
        x = DragonKey.allCases.map { $0.initial }
        v = [Double](repeating: 0, count: DragonKey.allCases.count)
    }

    public func q(_ key: DragonKey) -> Double { x[key.rawValue] }

    public func speed(_ key: DragonKey) -> Double { v[key.rawValue] }

    public static func target(for s: DragonScene, facing: Double) -> (target: DragonPose, sneeze: Double?) {
        typealias M = DragonMath
        var pose: DragonPose = DragonPose.sit
        let t: Double = s.t
        pose[.chest] = sin(t * 1.7) * 0.012
        pose[.head] = sin(t * 0.41) * 0.02
        pose[.wing] = sin(t * 1.05) * 0.03
        let n: Double = (t / 6).rounded(.down)
        let flutterAt: Double = n * 6 + M.seed(n + 3) * 4
        if t > flutterAt && t < flutterAt + 0.7 { pose[.wing] += sin((t - flutterAt) * 26) * 0.1 }
        let glance: Double = (t / 1.8).rounded(.down)
        if let gaze = s.gaze {
            pose[.gx] = M.clamp(gaze.x, -1, 1) * facing
            pose[.gy] = M.clamp(gaze.y, -1, 1)
            pose[.ear] = 0.06
            pose[.head] += 0.04
            pose[.sway] = 1.8
        } else {
            pose[.gx] = (M.seed(glance + 13) - 0.5) * 1.1
            pose[.gy] = (M.seed(glance + 41) - 0.5) * 0.5
        }
        if s.tired > 0 { pose[.lid] = s.tired }
        var sneeze: Double?
        let walk: Double = M.clamp01(s.walking)
        let act: (activity: DragonActivity, age: Double)? = s.current
        if s.held {
            var p: DragonTarget = [:]
            p[.y] = -30
            p[.head] = -0.06
            p.put(.pawN, 40, -90)
            p.put(.pawF, 20, -90)
            p.put(.footN, sin(t * 6) * 20 - 20, -60)
            p.put(.footF, sin(t * 6 + 1.5) * 20 + 10, -60)
            p[.open] = 0.5
            p[.tail] = 0.4
            p[.tailBase] = 0.15
            p[.wing] = 0.25 + sin(t * 9) * 0.06
            p[.lean] = M.clamp(s.vx / 2200, -0.4, 0.4)
            pose.overlay(p)
        } else if s.air > 0.3 {
            let pitch: Double = M.clamp(s.vy / 1800, -0.5, 0.5)
            var p: DragonTarget = [:]
            p[.lean] = 0.85 + pitch * 0.4
            p[.head] = -0.4 - pitch * 0.4
            p[.flap] = 1
            p[.tail] = 0.5
            p[.tailBase] = 0.2
            p[.open] = 0.55
            p.put(.pawN, 70, -60)
            p.put(.pawF, 70, -60)
            p.put(.footN, -110, -60)
            p.put(.footF, -140, -80)
            pose.overlay(p)
        } else if let act = act {
            pose.overlay(act.activity.pose(DragonBeat(age: act.age, duration: act.activity.duration, t: t)))
        } else if walk > 0.05 {
            var p: DragonTarget = [:]
            p[.ground] = walk
            p[.tail] = 0.86 * walk
            p[.tailBase] = 0.03 * walk
            p[.sway] = 2.2
            p[.wing] = 0.1 + sin(s.stridePhase * 2) * 0.1
            pose.overlay(p)
        } else if let reaction = s.reaction {
            pose.overlay(reaction.pose(age: s.reactionAge))
        } else if !s.reduced {
            let now: (habit: DragonHabit, u: Double)?
            if let cue = s.habit, let forced = DragonHabit.named(cue.name) {
                now = (forced, cue.u)
            } else {
                now = DragonHabit.at(t)
            }
            if let now = now {
                pose.overlay(now.habit.pose(now.u))
                if let at = now.habit.sneeze, now.u > at { sneeze = (now.u - at) / (1 - at) }
            }
        }
        if let act = act {
            pose[.crouch] += 0.24 * M.bump(act.age, 0, 0.26)
        }
        if pose[.gh] == 0 { pose[.gh] = pose[.gx] * 0.75 }
        if pose[.gv] == 0 { pose[.gv] = pose[.gy] }
        if s.hover {
            pose[.chest] += sin(t * 38) * 0.006
            pose[.ear] += 0.05
        }
        if s.land > 0 {
            pose[.crouch] = max(pose[.crouch], s.land * 0.85)
            pose[.wing] += s.land * 0.4
        }
        pose.overlay(s.overlay)
        return (pose, sneeze)
    }

    mutating func turning(_ s: DragonScene) -> Double {
        let want: Double = abs(s.facing) > 0.2 ? (s.facing < 0 ? -1 : 1) : (s.rest != 0 ? s.rest : facing)
        if want != facing {
            facing = want
            turnAt = s.t
        }
        let age: Double = s.t - turnAt
        let delays: [(Int, Double)] = [(0, 0), (1, 0.09), (2, 0.14), (3, 0.2)]
        for (part, delay) in delays where age >= delay {
            let f: Double
            switch part {
            case 0: f = flips.head
            case 1: f = flips.body
            case 2: f = flips.wing
            default: f = flips.tail
            }
            let step: Double = DragonMath.clamp(facing - f, -dt * 8.5, dt * 8.5)
            let next: Double = DragonMath.clamp(f + step, -1, 1)
            switch part {
            case 0: flips.head = next
            case 1: flips.body = next
            case 2: flips.wing = next
            default: flips.tail = next
            }
        }
        return age < 0.5 ? DragonMath.arc(age / 0.5) : 0
    }

    mutating func settle(_ target: DragonPose, reduced: Bool) {
        let steps: Int = max(1, Int((dt * 120).rounded(.up)))
        let h: Double = dt / Double(steps)
        for key in DragonKey.allCases {
            let i: Int = key.rawValue
            let goal: Double = target[key]
            if reduced {
                x[i] = goal
                v[i] = 0
                continue
            }
            let (k, c): (Double, Double) = key.stiffness
            for _ in 0..<steps {
                v[i] += (k * (goal - x[i]) - c * v[i]) * h
                x[i] += v[i] * h
            }
        }
    }

    public mutating func advance(_ s: DragonScene) {
        dt = last == nil ? 0 : DragonMath.clamp(s.t - (last ?? s.t), 0, 0.1)
        last = s.t
        let hopping: Double = turning(s)
        let made: (target: DragonPose, sneeze: Double?) = DragonBody.target(for: s, facing: facing)
        var target: DragonPose = made.target
        sneeze = made.sneeze
        if hopping > 0 {
            target[.y] -= 60 * hopping
            target[.footNy] -= 26 * hopping
            target[.footFy] -= 26 * hopping
            target[.crouch] += 0.12 * hopping
            target[.wing] += 0.2 * hopping
        }
        settle(target, reduced: s.reduced)
    }
}

struct DragonTailState: Sendable {
    var rest: [Double] = []
    var length: [Double] = []
    var aim: [Double] = []
    var total: Double = 0

    mutating func prepare(_ atlas: DragonAtlas) {
        guard rest.isEmpty else { return }
        let curve: [DragonPoint] = atlas.tail.curve
        for i in 0..<(curve.count - 1) {
            let d: DragonPoint = curve[i + 1] - curve[i]
            rest.append(atan2(d.y, d.x))
            length.append(hypot(d.x, d.y))
        }
        aim = rest
        total = length.reduce(0, +)
    }

    mutating func bend(_ atlas: DragonAtlas, root: DragonPoint, base: Double, tail: Double, sway: Double, dt: Double, s: DragonScene) -> [DragonPoint] {
        prepare(atlas)
        let n: Int = atlas.tail.curve.count - 1
        var points: [DragonPoint] = [root]
        let curl: Double = 1 - DragonMath.clamp(tail, -0.4, 1)
        let reach: Double = DragonMath.mixed(1, atlas.tail.length * 1.16 / total, DragonMath.clamp01(tail))
        let swing: Double = DragonMath.clamp(s.vx * -0.00012, -0.03, 0.03)
        for i in 0..<n {
            let k: Double = Double(i) / Double(n - 1)
            let parent: Double = i > 0 ? aim[i - 1] : rest[0] + base - 0.17 * DragonMath.clamp01(tail)
            let rel: Double = i > 0 ? DragonMath.wrapped(rest[i] - rest[i - 1]) : 0
            let wag: Double = sin(s.t * 2.4 - Double(i) * 0.3) * 0.009 * sway * (0.5 + k * 1.8)
            let want: Double = parent + rel * curl + wag + swing
            let rate: Double = s.reduced || dt == 0 ? 1 : 1 - exp(-(36 - 25 * k) * dt)
            aim[i] += DragonMath.wrapped(want - aim[i]) * rate
            let p: DragonPoint = points[i]
            points.append(DragonPoint(p.x + cos(aim[i]) * length[i] * reach, p.y + sin(aim[i]) * length[i] * reach))
        }
        let lowest: Double = points.map { $0.y }.max() ?? 0
        if lowest > 1214 {
            for i in 0..<points.count {
                points[i].y -= (lowest - 1214) * DragonMath.smooth(Double(i) / Double(n) * 1.6)
            }
        }
        return points
    }
}
