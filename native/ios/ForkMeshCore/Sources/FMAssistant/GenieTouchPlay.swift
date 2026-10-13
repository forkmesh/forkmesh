import Foundation

public enum GenieTouchGesture: String, CaseIterable, Sendable {
    case hover, loop, shake, speed, high, floor, toss, setDown, drop, again
}

public struct GenieTouchPlay: Sendable {
    public nonisolated static let pools: [GenieTouchGesture: [String]] = [
        .hover: ["lullaby", "daydream", "kick_legs"],
        .loop: ["loop_de_loop", "merry_go_round", "tornado"],
        .shake: ["maracas", "jelly", "rattled"],
        .speed: ["superhero", "rollercoaster", "windswept"],
        .high: ["vertigo", "lookout", "stargaze"],
        .floor: ["skate", "surf", "sled"],
        .toss: ["cannonball", "skydive", "starfish"],
        .setDown: ["dust_off", "thanks_lift", "comfy"],
        .drop: ["parachute", "umbrella", "feather_fall"],
        .again: ["seasick", "giddy", "pick_me"],
    ]

    public nonisolated static let gap: Double = 2.5
    public nonisolated static let hoverAfter: Double = 1.4
    public nonisolated static let hoverSlack: Double = 14
    public nonisolated static let loopWindow: Double = 2.4
    public nonisolated static let loopTurn: Double = 2 * Double.pi * 0.85
    public nonisolated static let loopStep: Double = 8
    public nonisolated static let shakeWindow: Double = 0.9
    public nonisolated static let shakeLeg: Double = 16
    public nonisolated static let shakeFlips: Int = 3
    public nonisolated static let speed: Double = 1700
    public nonisolated static let speedWindow: Double = 0.1
    public nonisolated static let high: Double = 0.18
    public nonisolated static let floor: Double = 0.82
    public nonisolated static let floorRun: Double = 160
    public nonisolated static let toss: Double = 900
    public nonisolated static let gentle: Double = 260
    public nonisolated static let settleAfter: Double = 0.35
    public nonisolated static let dropAbove: Double = 0.45
    public nonisolated static let againCount: Int = 3
    public nonisolated static let againWindow: Double = 12

    struct Sample: Equatable, Sendable {
        var x: Double
        var y: Double
        var t: Double
    }

    public private(set) var holding = false
    var samples: [Sample] = []
    var started: Double = 0
    var height: Double = 1
    var rest = Sample(x: 0, y: 0, t: 0)
    var floorFrom: Double?
    var fired: Set<GenieTouchGesture> = []
    var pickups: [Double] = []
    var lastTrick: Double = -Double.infinity
    var turns: [GenieTouchGesture: Int] = [:]

    public init() {}

    public mutating func begin(x: Double, y: Double, at t: Double, height: Double) -> GenieTouchGesture? {
        holding = true
        started = t
        self.height = max(height, 1)
        samples = [Sample(x: x, y: y, t: t)]
        rest = Sample(x: x, y: y, t: t)
        floorFrom = y >= self.height * GenieTouchPlay.floor ? x : nil
        fired = []
        pickups = pickups.filter { t - $0 < GenieTouchPlay.againWindow } + [t]
        guard pickups.count >= GenieTouchPlay.againCount else { return nil }
        pickups = []
        return fire(.again, at: t)
    }

    public mutating func move(x: Double, y: Double, at t: Double) -> GenieTouchGesture? {
        guard holding else { return nil }
        samples.append(Sample(x: x, y: y, t: t))
        samples.removeAll { t - $0.t > GenieTouchPlay.loopWindow }
        if hypot(x - rest.x, y - rest.y) > GenieTouchPlay.hoverSlack { rest = Sample(x: x, y: y, t: t) }
        if y >= height * GenieTouchPlay.floor {
            let from = floorFrom ?? x
            floorFrom = from
            if abs(x - from) >= GenieTouchPlay.floorRun, let gesture = fire(.floor, at: t) { return gesture }
        } else {
            floorFrom = nil
        }
        if shaken(at: t), let gesture = fire(.shake, at: t) { return gesture }
        if looped(), let gesture = fire(.loop, at: t) { return gesture }
        if sped(at: t), let gesture = fire(.speed, at: t) { return gesture }
        if y <= height * GenieTouchPlay.high, let gesture = fire(.high, at: t) { return gesture }
        return nil
    }

    public mutating func still(at t: Double) -> GenieTouchGesture? {
        guard holding, t - rest.t >= GenieTouchPlay.hoverAfter else { return nil }
        return fire(.hover, at: t)
    }

    public mutating func end(x: Double, y: Double, vx: Double, vy: Double, at t: Double, parked: Bool) -> GenieTouchGesture? {
        guard holding else { return nil }
        holding = false
        samples = []
        guard !parked else { return nil }
        if -vy >= GenieTouchPlay.toss { return fire(.toss, at: t) }
        guard hypot(vx, vy) < GenieTouchPlay.gentle else { return nil }
        if y < height * GenieTouchPlay.dropAbove { return fire(.drop, at: t) }
        return t - started >= GenieTouchPlay.settleAfter ? fire(.setDown, at: t) : nil
    }

    public mutating func trick(for gesture: GenieTouchGesture) -> GenieTrick? {
        guard let options = GenieTouchPlay.pools[gesture], !options.isEmpty else { return nil }
        let turn = turns[gesture] ?? 0
        turns[gesture] = turn + 1
        return GenieTricks.named(options[turn % options.count])
    }

    mutating func fire(_ gesture: GenieTouchGesture, at t: Double) -> GenieTouchGesture? {
        guard !fired.contains(gesture), t - lastTrick >= GenieTouchPlay.gap else { return nil }
        fired.insert(gesture)
        lastTrick = t
        return gesture
    }

    func shaken(at t: Double) -> Bool {
        let recent = samples.filter { t - $0.t <= GenieTouchPlay.shakeWindow }
        guard recent.count >= 4 else { return false }
        let pairs = Array(zip(recent, recent.dropFirst()))
        let across = pairs.reduce(0) { $0 + abs($1.1.x - $1.0.x) }
        let along = pairs.reduce(0) { $0 + abs($1.1.y - $1.0.y) }
        var sign = 0.0
        var run = 0.0
        var flips = 0
        for (a, b) in pairs {
            let d = across >= along ? b.x - a.x : b.y - a.y
            guard d != 0 else { continue }
            let s: Double = d > 0 ? 1 : -1
            if s == sign {
                run += abs(d)
            } else {
                if sign != 0, run >= GenieTouchPlay.shakeLeg { flips += 1 }
                sign = s
                run = abs(d)
            }
        }
        return flips >= GenieTouchPlay.shakeFlips
    }

    func looped() -> Bool {
        guard var anchor = samples.first else { return false }
        var heading: Double?
        var total = 0.0
        for sample in samples.dropFirst() {
            let dx = sample.x - anchor.x
            let dy = sample.y - anchor.y
            guard hypot(dx, dy) >= GenieTouchPlay.loopStep else { continue }
            let now = atan2(dy, dx)
            if let before = heading {
                var turn = now - before
                while turn > Double.pi { turn -= 2 * Double.pi }
                while turn < -Double.pi { turn += 2 * Double.pi }
                total = abs(turn) > Double.pi * 0.75 ? 0 : total + turn
            }
            heading = now
            anchor = sample
            if abs(total) >= GenieTouchPlay.loopTurn { return true }
        }
        return false
    }

    func sped(at t: Double) -> Bool {
        guard let last = samples.last, let first = samples.first(where: { t - $0.t <= GenieTouchPlay.speedWindow }) else { return false }
        let seconds = last.t - first.t
        guard seconds >= 0.03 else { return false }
        return hypot(last.x - first.x, last.y - first.y) / seconds >= GenieTouchPlay.speed
    }
}
