import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieMagic: String, Sendable, CaseIterable {
    case poof, levitate, lamp, wishes, smokeRings, shimmer, stardust, carpet, vortex, crystal, runes, halo, fireflies, whirl, grow, bottle
    case rainbow, fireworks, bubbles, glitter, jewelBeam, ripple, moons, constellation, crescent, mirage, coins, flourish, incense, whirlwind

    public nonisolated static let crown: Double = 0.14
    public nonisolated static let mouth: Double = -0.22
    public nonisolated static let feet: Double = -0.42
    public nonisolated static let swap: Double = 0.5

    public var trick: String {
        var out: String = "genie_"
        for character in rawValue {
            if character.isUppercase {
                out += "_" + character.lowercased()
            } else {
                out.append(character)
            }
        }
        return out
    }

    public var aims: Bool {
        switch self {
        case .poof, .levitate, .carpet, .crystal, .whirl, .grow, .bottle, .incense, .whirlwind: return false
        default: return true
        }
    }

    public static func glimmers(_ magic: GenieMagic, frame: GenieTrickFrame, stage: GenieMagicStage) -> [GenieGlimmer] {
        var sky: GenieSky = GenieSky(frame: frame, stage: stage)
        sky.paint(magic)
        return sky.out
    }
}

public enum GenieGlimmerShape: String, Sendable, CaseIterable {
    case puff, orb, spark, star, dust, ring, band, beam, arc, line, lamp, carpet, crystal, rune, bubble, coin, moon, bottle, outline, wisp, glow
}

public enum GenieGlimmerTone: String, Sendable, CaseIterable {
    case glow, lilac, violet, plum, gold, mist, spectrum
}

public struct GenieGlimmer: Equatable, Sendable {
    public var shape: GenieGlimmerShape
    public var x: Double
    public var y: Double
    public var size: Double
    public var opacity: Double
    public var tone: GenieGlimmerTone
    public var front: Bool
    public var angle: Double
    public var squash: Double
    public var hue: Double
    public var weight: Double
    public var variant: Int
    public var x2: Double
    public var y2: Double
    public var bend: Double
    public var reveal: Double
    public var box: CGRect

    public init(
        _ shape: GenieGlimmerShape, x: Double, y: Double, size: Double, opacity: Double, tone: GenieGlimmerTone = .glow, front: Bool = true,
        angle: Double = 0, squash: Double = 1, hue: Double = 0, weight: Double = 1, variant: Int = 0, x2: Double = 0, y2: Double = 0,
        bend: Double = 0, reveal: Double = 1, box: CGRect = CGRect.zero
    ) {
        self.shape = shape
        self.x = x
        self.y = y
        self.size = size
        self.opacity = opacity
        self.tone = tone
        self.front = front
        self.angle = angle
        self.squash = squash
        self.hue = hue
        self.weight = weight
        self.variant = variant
        self.x2 = x2
        self.y2 = y2
        self.bend = bend
        self.reveal = reveal
        self.box = box
    }
}

public struct GenieMagicStage: Equatable, Sendable {
    public var spider: CGPoint
    public var side: Double
    public var turn: Double
    public var target: CGRect

    public init(spider: CGPoint, side: Double, turn: Double = 0, target: CGRect? = nil, full: CGRect = CGRect.zero) {
        self.spider = spider
        self.side = max(side, 1)
        self.turn = turn
        self.target = target ?? GenieMagicStage.standIn(spider: spider, side: side, turn: turn, full: full)
    }

    public var up: CGPoint {
        let radians: Double = turn * Double.pi / 180
        return CGPoint(x: sin(radians), y: -cos(radians))
    }

    public var right: CGPoint {
        let radians: Double = turn * Double.pi / 180
        return CGPoint(x: cos(radians), y: sin(radians))
    }

    public func at(_ across: Double, _ rise: Double) -> CGPoint {
        let r: CGPoint = right
        let u: CGPoint = up
        let x: Double = Double(spider.x) + (Double(r.x) * across + Double(u.x) * rise) * side
        let y: Double = Double(spider.y) + (Double(r.y) * across + Double(u.y) * rise) * side
        return CGPoint(x: x, y: y)
    }

    public var crown: CGPoint { at(0, GenieMagic.crown) }

    public static func standIn(spider: CGPoint, side: Double, turn: Double, full: CGRect) -> CGRect {
        let probe: GenieMagicStage = GenieMagicStage(spider: spider, side: side, turn: turn, target: CGRect.zero)
        let middle: CGPoint = full.isEmpty ? spider : CGPoint(x: full.midX, y: full.midY)
        let a: CGPoint = probe.at(1.3, 1.1)
        let b: CGPoint = probe.at(-1.3, 1.1)
        let near: CGPoint = hypot(a.x - middle.x, a.y - middle.y) <= hypot(b.x - middle.x, b.y - middle.y) ? a : b
        let width: Double = max(side, 1) * 0.9
        let height: Double = max(side, 1) * 0.5
        var x: Double = Double(near.x) - width / 2
        var y: Double = Double(near.y) - height / 2
        if !full.isEmpty {
            let room: CGRect = full.insetBy(dx: 8, dy: 8)
            x = min(max(x, Double(room.minX)), max(Double(room.maxX) - width, Double(room.minX)))
            y = min(max(y, Double(room.minY)), max(Double(room.maxY) - height, Double(room.minY)))
        }
        return CGRect(x: x, y: y, width: width, height: height)
    }
}

extension GenieTrick {
    public func glimmers(age: Double, stage: GenieMagicStage) -> [GenieGlimmer] {
        guard let magic = magic, let frame = frame(age: age) else { return [] }
        return GenieMagic.glimmers(magic, frame: frame, stage: stage)
    }
}

struct GenieVec: Equatable {
    var x: Double
    var y: Double

    init(_ x: Double, _ y: Double) {
        self.x = x
        self.y = y
    }

    init(_ point: CGPoint) {
        x = Double(point.x)
        y = Double(point.y)
    }

    static func + (a: GenieVec, b: GenieVec) -> GenieVec { GenieVec(a.x + b.x, a.y + b.y) }

    static func - (a: GenieVec, b: GenieVec) -> GenieVec { GenieVec(a.x - b.x, a.y - b.y) }

    static func * (a: GenieVec, s: Double) -> GenieVec { GenieVec(a.x * s, a.y * s) }

    var length: Double { (x * x + y * y).squareRoot() }

    var unit: GenieVec {
        let size: Double = max(length, 0.0001)
        return GenieVec(x / size, y / size)
    }

    var normal: GenieVec { GenieVec(-y, x) }

    func mix(_ b: GenieVec, _ t: Double) -> GenieVec { GenieVec(x + (b.x - x) * t, y + (b.y - y) * t) }

    static func curve(_ a: GenieVec, _ c: GenieVec, _ b: GenieVec, _ t: Double) -> GenieVec {
        a.mix(c, t).mix(c.mix(b, t), t)
    }
}

struct GenieSky {
    let u: Double
    let age: Double
    let strength: Double
    let side: Double
    let ground: GenieMagicStage
    let body: GenieMagicStage
    let box: CGRect
    let center: GenieVec
    let top: GenieVec
    let crown: GenieVec
    let face: Double
    var out: [GenieGlimmer] = []

    init(frame: GenieTrickFrame, stage: GenieMagicStage) {
        u = frame.progress
        age = frame.age
        strength = frame.rise
        side = stage.side
        ground = stage
        body = GenieMagicStage(
            spider: stage.at(frame.dx / 100, -frame.dy / 100), side: stage.side, turn: stage.turn, target: stage.target)
        box = stage.target
        center = GenieVec(Double(stage.target.midX), Double(stage.target.midY))
        top = GenieVec(Double(stage.target.midX), Double(stage.target.minY))
        crown = GenieVec(body.crown)
        let toward: GenieVec = center - GenieVec(stage.spider)
        let right: CGPoint = stage.right
        face = toward.x * Double(right.x) + toward.y * Double(right.y) >= 0 ? 1 : -1
    }

    static func window(_ u: Double, _ a: Double, _ b: Double) -> Double {
        min(max((u - a) / max(b - a, 0.0001), 0), 1)
    }

    static func bell(_ v: Double) -> Double { sin(Double.pi * min(max(v, 0), 1)) }

    static func ease(_ v: Double) -> Double {
        let c: Double = min(max(v, 0), 1)
        return c * c * (3 - 2 * c)
    }

    static func seed(_ k: Int) -> Double {
        let s: Double = sin(Double(k) * 12.9898 + 78.233) * 43_758.5453
        return s - s.rounded(.down)
    }

    static func live(_ w: Double) -> Bool { w > 0 && w < 1 }

    var bw: Double { Double(box.width) }

    var bh: Double { Double(box.height) }

    func at(_ stage: GenieMagicStage, _ across: Double, _ rise: Double) -> GenieVec { GenieVec(stage.at(across, rise)) }

    mutating func put(_ glimmer: GenieGlimmer) {
        var next: GenieGlimmer = glimmer
        next.opacity = min(max(glimmer.opacity * strength, 0), 1)
        guard next.opacity > 0.01, next.size > 0.05, next.x.isFinite, next.y.isFinite else { return }
        out.append(next)
    }

    mutating func dot(
        _ shape: GenieGlimmerShape, _ at: GenieVec, size: Double, opacity: Double, tone: GenieGlimmerTone, front: Bool = true, angle: Double = 0,
        squash: Double = 1, hue: Double = 0, weight: Double = 1, variant: Int = 0
    ) {
        put(GenieGlimmer(
            shape, x: at.x, y: at.y, size: size, opacity: opacity, tone: tone, front: front, angle: angle, squash: squash, hue: hue, weight: weight,
            variant: variant))
    }

    mutating func stroke(
        _ shape: GenieGlimmerShape, from a: GenieVec, to b: GenieVec, weight: Double, opacity: Double, tone: GenieGlimmerTone, bend: Double = 0,
        reveal: Double = 1, hue: Double = 0
    ) {
        put(GenieGlimmer(
            shape, x: a.x, y: a.y, size: max(weight, 0.1), opacity: opacity, tone: tone, hue: hue, weight: weight, x2: b.x, y2: b.y, bend: bend,
            reveal: reveal))
    }

    func lift(_ a: GenieVec, _ b: GenieVec, _ share: Double) -> Double {
        let chord: GenieVec = b - a
        let normal: GenieVec = chord.normal.unit
        let reach: Double = chord.length * share
        return normal.y > 0 ? -reach : reach
    }

    func bowed(_ a: GenieVec, _ b: GenieVec, _ share: Double, _ t: Double) -> GenieVec {
        let bend: Double = lift(a, b, share)
        let control: GenieVec = a.mix(b, 0.5) + (b - a).normal.unit * bend
        return GenieVec.curve(a, control, b, t)
    }

    func tone(_ k: Int, _ tones: [GenieGlimmerTone]) -> GenieGlimmerTone { tones[k % tones.count] }

    mutating func paint(_ magic: GenieMagic) {
        switch magic {
        case .poof: poof()
        case .levitate: levitate()
        case .lamp: lamp()
        case .wishes: wishes()
        case .smokeRings: smokeRings()
        case .shimmer: shimmer()
        case .stardust: stardust()
        case .carpet: carpet()
        case .vortex: vortex()
        case .crystal: crystal()
        case .runes: runes()
        case .halo: halo()
        case .fireflies: fireflies()
        case .whirl: whirl()
        case .grow: grow()
        case .bottle: bottle()
        case .rainbow: rainbow()
        case .fireworks: fireworks()
        case .bubbles: bubbles()
        case .glitter: glitter()
        case .jewelBeam: jewelBeam()
        case .ripple: ripple()
        case .moons: moons()
        case .constellation: constellation()
        case .crescent: crescent()
        case .mirage: mirage()
        case .coins: coins()
        case .flourish: flourish()
        case .incense: incense()
        case .whirlwind: whirlwind()
        }
    }

    mutating func poof() {
        let middle: GenieVec = at(ground, 0, -0.12)
        for cloud in 0..<2 {
            let w: Double = cloud == 0 ? GenieSky.window(u, 0.02, GenieMagic.swap) : GenieSky.window(u, GenieMagic.swap, 0.98)
            guard GenieSky.live(w) else { continue }
            dot(.glow, middle, size: side * (0.25 + 0.3 * w), opacity: GenieSky.bell(w) * 0.5, tone: .violet, front: false)
            for j in 0..<8 {
                let angle: Double = Double(j) / 8 * Double.pi * 2 + GenieSky.seed(j + cloud * 8) * 0.7
                let reach: Double = side * (0.1 + 0.36 * GenieSky.ease(w))
                let spot: GenieVec = middle + GenieVec(cos(angle), sin(angle) * 0.8) * reach
                let size: Double = side * (0.09 + 0.12 * w) * (0.8 + 0.4 * GenieSky.seed(j + 20))
                dot(.puff, spot, size: size, opacity: GenieSky.bell(w) * 0.9, tone: tone(j, [.lilac, .violet, .plum]))
            }
        }
    }

    mutating func levitate() {
        let feet: GenieVec = at(body, 0, GenieMagic.feet)
        let floor: GenieVec = at(ground, 0, GenieMagic.feet)
        let height: Double = min((feet - floor).length / (side * 0.08), 1)
        let right: GenieVec = GenieVec(ground.right)
        for j in 0..<9 {
            let t: Double = Double(j) / 8
            let curl: Double = sin(age * 3.2 + t * 5) * side * 0.1 * t
            let spot: GenieVec = feet.mix(floor, t) + right * curl
            dot(.puff, spot, size: side * (0.035 + 0.05 * t), opacity: height * (1 - t * 0.55) * 0.75, tone: tone(j, [.lilac, .violet]), front: false)
        }
    }

    mutating func lamp() {
        let appear: Double = GenieSky.ease(GenieSky.window(u, 0, 0.15))
        let leave: Double = GenieSky.window(u, 0.88, 1)
        let size: Double = side * 0.2 * (0.4 + 0.6 * appear)
        let base: GenieVec = at(ground, face * 0.62, -0.31)
        dot(.lamp, base, size: size, opacity: appear * (1 - leave), tone: .gold, front: false, angle: ground.turn, squash: face)
        let spout: GenieVec = at(ground, face * (0.62 + 0.22), -0.25)
        let flow: Double = GenieSky.bell(GenieSky.window(u, 0.16, 0.9))
        let control: GenieVec = spout.mix(center, 0.5) + GenieVec(0, -max((center - spout).length * 0.35, 30))
        let across: GenieVec = (center - spout).normal.unit
        for j in 0..<11 {
            let t: Double = (age * 0.42 + Double(j) / 11).truncatingRemainder(dividingBy: 1)
            let curl: Double = sin(t * Double.pi * 4 + age * 2) * 10 * (1 - t * 0.6)
            let spot: GenieVec = GenieVec.curve(spout, control, center, t) + across * curl
            dot(.puff, spot, size: 3 + 8 * t, opacity: flow * GenieSky.bell(t) * 0.8, tone: tone(j, [.violet, .lilac, .plum]))
        }
        let idea: Double = GenieSky.window(u, 0.72, 0.98)
        if GenieSky.live(idea) {
            for j in 0..<8 {
                let angle: Double = Double(j) / 8 * Double.pi * 2
                let spot: GenieVec = crown + GenieVec(cos(angle), sin(angle)) * (side * (0.12 + 0.14 * idea))
                dot(.spark, spot, size: 3.5, opacity: GenieSky.bell(idea), tone: .gold, angle: age * 90)
            }
        }
    }

    mutating func wishes() {
        let tones: [GenieGlimmerTone] = [.glow, .lilac, .gold]
        for i in 0..<3 {
            let start: Double = 0.08 + Double(i) * 0.2
            let flight: Double = GenieSky.window(u, start, start + 0.32)
            if GenieSky.live(flight) {
                let sway: Double = Double(i - 1) * 0.12
                let t: Double = GenieSky.ease(flight)
                let spot: GenieVec = bowed(crown, center, 0.3 + sway, t)
                dot(.orb, spot, size: 6, opacity: 1, tone: tones[i])
                for trail in 1...3 {
                    let back: Double = max(t - Double(trail) * 0.05, 0)
                    dot(.dust, bowed(crown, center, 0.3 + sway, back), size: 1.8, opacity: 0.8 - Double(trail) * 0.2, tone: tones[i])
                }
            }
            let pop: Double = GenieSky.window(u, start + 0.32, start + 0.48)
            if GenieSky.live(pop) {
                for j in 0..<7 {
                    let angle: Double = Double(j) / 7 * Double.pi * 2 + Double(i)
                    let spot: GenieVec = center + GenieVec(cos(angle), sin(angle)) * (6 + 26 * GenieSky.ease(pop))
                    dot(.spark, spot, size: 4 * (1 - pop * 0.5), opacity: 1 - pop, tone: tones[i], angle: age * 120)
                }
            }
        }
    }

    mutating func smokeRings() {
        let mouth: GenieVec = at(body, 0, GenieMagic.mouth)
        let heading: GenieVec = center - mouth
        let angle: Double = atan2(heading.y, heading.x) * 180 / Double.pi
        for i in 0..<3 {
            let start: Double = 0.1 + Double(i) * 0.2
            let w: Double = GenieSky.window(u, start, start + 0.5)
            guard GenieSky.live(w) else { continue }
            let spot: GenieVec = mouth.mix(center, GenieSky.ease(w) * 0.94)
            let size: Double = side * 0.05 + w * side * 0.22
            dot(
                .ring, spot, size: size, opacity: (1 - w) * min(1, w * 6) * 0.85, tone: i == 1 ? .lilac : .violet, angle: angle, squash: 0.55,
                weight: max(size * 0.22, 1.5))
        }
    }

    mutating func shimmer() {
        let w: Double = GenieSky.window(u, 0.12, 0.88)
        guard GenieSky.live(w) else { return }
        let band: Double = max(min(bw * 0.35, 60), 18)
        let x: Double = Double(box.minX) - band + (bw + 2 * band) * GenieSky.ease(w)
        put(GenieGlimmer(
            .band, x: x, y: center.y, size: band, opacity: GenieSky.bell(w) * 0.85 + 0.1, tone: .gold, angle: 18,
            box: box.insetBy(dx: -3, dy: -3)))
        for j in 0..<3 {
            let spot: GenieVec = GenieVec(x + Double(j - 1) * band * 0.3, Double(box.minY) + Double(j % 2) * bh)
            dot(.spark, spot, size: 3.5, opacity: GenieSky.bell(w) * (0.6 + 0.4 * sin(age * 9 + Double(j))), tone: .gold, angle: age * 60)
        }
    }

    mutating func stardust() {
        let pour: Double = GenieSky.window(u, 0.05, 0.62)
        if GenieSky.live(pour) {
            for j in 0..<16 {
                let t: Double = (u * 2.2 + GenieSky.seed(j)).truncatingRemainder(dividingBy: 1)
                let jitter: GenieVec = GenieVec(GenieSky.seed(j + 3) - 0.5, GenieSky.seed(j + 4) - 0.5) * 6
                let spot: GenieVec = bowed(crown, top, 0.45, t) + jitter
                let shape: GenieGlimmerShape = j % 4 == 0 ? .spark : .dust
                dot(shape, spot, size: 1.6 + GenieSky.seed(j + 5) * 1.6, opacity: GenieSky.bell(t) * GenieSky.bell(pour), tone: tone(j, [.gold, .glow, .mist]), angle: age * 80)
            }
        }
        let fade: Double = 1 - GenieSky.window(u, 0.88, 1)
        for j in 0..<9 {
            let shown: Double = GenieSky.window(u, 0.35 + Double(j) * 0.03, 0.45 + Double(j) * 0.03)
            guard shown > 0 else { continue }
            let spot: GenieVec = GenieVec(Double(box.minX) + bw * (Double(j) + 0.5) / 9, Double(box.minY) - 1)
            dot(.spark, spot, size: 3, opacity: shown * fade * (0.5 + 0.5 * sin(age * 7 + Double(j) * 1.7)), tone: tone(j, [.gold, .mist, .glow]), angle: 45)
        }
    }

    mutating func carpet() {
        var across: Double = 0
        if u < 0.18 { across = face * (1 - GenieSky.ease(u / 0.18)) * 0.9 }
        if u > 0.85 { across = -face * GenieSky.ease((u - 0.85) / 0.15) * 0.9 }
        let spot: GenieVec = at(body, across, GenieMagic.feet - 0.02)
        let edge: Double = min(GenieSky.ease(u / 0.12), 1 - GenieSky.window(u, 0.9, 1))
        put(GenieGlimmer(
            .carpet, x: spot.x, y: spot.y, size: side * 1.05, opacity: max(edge, 0.2), tone: .violet, front: false,
            angle: ground.turn + sin(age * 2) * 3, bend: age * 6))
        for j in 0..<4 {
            let t: Double = (age * 0.8 + Double(j) / 4).truncatingRemainder(dividingBy: 1)
            let below: GenieVec = at(body, across + (GenieSky.seed(j) - 0.5) * 0.9, GenieMagic.feet - 0.06 - t * 0.2)
            dot(.dust, below, size: 1.6, opacity: GenieSky.bell(t) * edge, tone: tone(j, [.gold, .glow]), front: false)
        }
    }

    mutating func vortex() {
        let reach: Double = min(max(bw, bh) / 2 + 16, 140)
        let aspect: Double = min(max((bh + 32) / (bw + 32), 0.35), 1)
        let w: Double = GenieSky.window(u, 0.05, 0.78)
        if GenieSky.live(w) {
            for j in 0..<14 {
                let angle: Double = Double(j) / 14 * Double.pi * 2 + age * (5 + 4 * w)
                let r: Double = reach * (1 - GenieSky.ease(w)) * (0.85 + 0.3 * GenieSky.seed(j))
                let spot: GenieVec = center + GenieVec(cos(angle) * r, sin(angle) * r * aspect)
                dot(.spark, spot, size: 3.2 * (1 - w * 0.4), opacity: min(1, w * 8), tone: tone(j, [.glow, .lilac, .gold]), angle: age * 200)
            }
        }
        let flash: Double = GenieSky.window(u, 0.76, 0.98)
        if GenieSky.live(flash) {
            dot(.glow, center, size: 10 + 34 * flash, opacity: GenieSky.bell(flash), tone: .mist)
            dot(.ring, center, size: 6 + 30 * flash, opacity: 1 - flash, tone: .glow, weight: 1.4)
        }
    }

    mutating func crystal() {
        let grow: Double = GenieSky.ease(GenieSky.window(u, 0, 0.15)) * (1 - GenieSky.window(u, 0.88, 1))
        let ball: GenieVec = at(body, 0, -0.27)
        let size: Double = side * 0.17 * grow
        dot(.crystal, ball, size: size, opacity: grow, tone: .lilac, angle: age * 2.2 * 180 / Double.pi)
        for j in 0..<3 {
            let angle: Double = -Double.pi * 0.75 + Double(j) * 0.5
            let spot: GenieVec = ball + GenieVec(cos(angle), sin(angle)) * (size * 0.95)
            dot(.spark, spot, size: 2.6, opacity: grow * (0.5 + 0.5 * sin(age * 5 + Double(j) * 2)), tone: .mist, angle: 45)
        }
    }

    mutating func runes() {
        let rx: Double = bw / 2 + 18
        let ry: Double = max(bh / 2 + 18, 24)
        let fade: Double = 1 - GenieSky.window(u, 0.78, 1)
        for j in 0..<6 {
            let shown: Double = GenieSky.window(u, 0.05 + Double(j) * 0.06, 0.2 + Double(j) * 0.06)
            guard shown > 0 else { continue }
            let angle: Double = Double(j) / 6 * Double.pi * 2 + age * 1.1
            let spot: GenieVec = center + GenieVec(cos(angle) * rx, sin(angle) * ry)
            dot(
                .rune, spot, size: 7, opacity: shown * fade, tone: j % 2 == 0 ? .glow : .lilac, angle: sin(age * 2 + Double(j)) * 15, weight: 1.4,
                variant: j)
        }
    }

    mutating func halo() {
        let w: Double = GenieSky.window(u, 0.05, 0.45)
        guard w > 0 else { return }
        let fade: Double = 1 - GenieSky.window(u, 0.75, 1)
        let y: Double = center.y + (Double(box.minY) - 14 - center.y) * GenieSky.ease(w) + sin(age * 3) * 2
        let size: Double = min(max(bw * 0.32, 14), 60)
        let spot: GenieVec = GenieVec(center.x, y)
        dot(.glow, spot, size: size * 1.2, opacity: min(1, w * 3) * fade * 0.35, tone: .gold, front: false)
        dot(.ring, spot, size: size, opacity: min(1, w * 3) * fade, tone: .gold, squash: 0.28, weight: 3)
    }

    mutating func fireflies() {
        let fade: Double = 1 - GenieSky.window(u, 0.85, 1)
        for j in 0..<7 {
            let launch: Double = 0.04 + Double(j) * 0.07
            let w: Double = GenieSky.window(u, launch, launch + 0.4)
            guard w > 0 else { continue }
            let home: GenieVec = GenieVec(
                Double(box.minX) + bw * GenieSky.seed(j + 20), Double(box.minY) + bh * GenieSky.seed(j + 30))
            let hover: GenieVec = home + GenieVec(sin(age * 2.3 + Double(j)) * 8, cos(age * 1.9 + Double(j)) * 6)
            let path: GenieVec = crown.mix(hover, GenieSky.ease(w))
            let wander: GenieVec = (hover - crown).normal.unit * (sin(w * Double.pi * 3 + Double(j)) * 14 * (1 - w))
            let blink: Double = 0.55 + 0.45 * sin(age * 6 + Double(j) * 2)
            dot(.orb, path + wander, size: 3.2, opacity: min(1, w * 6) * blink * fade, tone: j % 2 == 0 ? .glow : .gold)
        }
    }

    mutating func whirl() {
        for j in 0..<10 {
            let angle: Double = age * 7 + Double(j) / 10 * Double.pi * 2
            let spot: GenieVec = at(ground, cos(angle) * 0.45, -0.3 + sin(angle) * 0.1 + Double(j % 3) * 0.04)
            dot(.puff, spot, size: side * 0.07, opacity: 0.7 * GenieSky.bell(u), tone: tone(j, [.lilac, .violet, .plum]), front: sin(angle) > 0)
        }
    }

    mutating func grow() {
        let swell: Double = GenieSky.bell(u)
        let middle: GenieVec = at(ground, 0, -0.06)
        for j in 0..<12 {
            let angle: Double = Double(j) / 12 * Double.pi * 2 + GenieSky.seed(j) * 0.5 + age * 0.4
            let r: Double = side * (0.35 + 0.25 * swell)
            let spot: GenieVec = middle + GenieVec(cos(angle) * r, sin(angle) * r * 0.8)
            dot(.puff, spot, size: side * (0.08 + 0.08 * swell), opacity: swell * 0.6, tone: tone(j, [.violet, .lilac, .plum]), front: j % 2 == 0)
        }
    }

    mutating func bottle() {
        let shown: Double = GenieSky.window(u, 0.1, 0.28) * (1 - GenieSky.window(u, 0.72, 0.86))
        let base: GenieVec = at(ground, 0, -0.3)
        let neck: GenieVec = at(ground, 0, -0.09)
        dot(.bottle, base, size: side * 0.42, opacity: shown, tone: .violet, front: false, angle: ground.turn)
        let inward: Double = GenieSky.window(u, 0.08, 0.45)
        if GenieSky.live(inward) {
            for j in 0..<10 {
                let angle: Double = age * 8 + Double(j) / 10 * Double.pi * 2
                let spot: GenieVec = neck + GenieVec(cos(angle), sin(angle)) * (side * 0.5 * (1 - inward))
                dot(.spark, spot, size: 3, opacity: GenieSky.bell(inward), tone: tone(j, [.glow, .lilac]), angle: age * 150)
            }
        }
        let pop: Double = GenieSky.window(u, 0.58, 0.78)
        if GenieSky.live(pop) {
            dot(.puff, neck, size: side * (0.08 + 0.14 * pop), opacity: GenieSky.bell(pop) * 0.8, tone: .lilac)
            for j in 0..<10 {
                let angle: Double = Double(j) / 10 * Double.pi * 2
                let spot: GenieVec = neck + GenieVec(cos(angle), sin(angle)) * (side * (0.05 + 0.45 * pop))
                dot(.spark, spot, size: 3.4 * (1 - pop * 0.5), opacity: 1 - pop, tone: tone(j, [.gold, .glow]), angle: 45)
            }
        }
    }

    mutating func rainbow() {
        let reveal: Double = GenieSky.ease(GenieSky.window(u, 0.05, 0.45))
        let fade: Double = 1 - GenieSky.window(u, 0.75, 1)
        let normal: GenieVec = (top - crown).normal.unit
        let flip: Double = normal.y > 0 ? -1 : 1
        for j in 0..<6 {
            let shift: GenieVec = normal * (flip * Double(j) * 3)
            let a: GenieVec = crown + shift
            let b: GenieVec = top + shift
            stroke(
                .arc, from: a, to: b, weight: 3, opacity: fade * (0.75 + 0.25 * sin(age * 5 + Double(j))), tone: .spectrum,
                bend: lift(a, b, 0.4), reveal: reveal, hue: Double(j) / 6 * 0.8)
        }
    }

    mutating func fireworks() {
        let bursts: [(Double, GenieVec)] = [
            (0.05, GenieVec(center.x - bw * 0.15, Double(box.minY) - 34)),
            (0.35, GenieVec(center.x + bw * 0.18, Double(box.minY) - 22)),
        ]
        for (index, burst) in bursts.enumerated() {
            let rocket: Double = GenieSky.window(u, burst.0, burst.0 + 0.15)
            if GenieSky.live(rocket) {
                let from: GenieVec = GenieVec(burst.1.x, Double(box.minY))
                dot(.dust, from.mix(burst.1, rocket), size: 2, opacity: 1, tone: .gold)
            }
            let w: Double = GenieSky.window(u, burst.0 + 0.15, burst.0 + 0.6)
            guard GenieSky.live(w) else { continue }
            for j in 0..<12 {
                let angle: Double = Double(j) / 12 * Double.pi * 2 + Double(index) * 0.26
                let r: Double = 4 + 30 * GenieSky.ease(w)
                let spot: GenieVec = burst.1 + GenieVec(cos(angle) * r, sin(angle) * r + 10 * w * w)
                dot(.spark, spot, size: 3 * (1 - w * 0.6), opacity: 1 - w, tone: j % 4 == 0 ? .mist : .gold, angle: angle * 180 / Double.pi)
            }
        }
    }

    mutating func bubbles() {
        for j in 0..<8 {
            let start: Double = GenieSky.seed(j) * 0.55
            let w: Double = GenieSky.window(u, start, start + 0.4)
            guard GenieSky.live(w) else { continue }
            let x: Double = Double(box.minX) + bw * (0.1 + 0.8 * GenieSky.seed(j + 40)) + sin(age * 3 + Double(j)) * 4
            let y: Double = center.y - w * (bh / 2 + 46)
            let size: Double = 4 + GenieSky.seed(j + 41) * 6
            let hue: Double = (age * 0.3 + Double(j) * 0.13).truncatingRemainder(dividingBy: 1)
            if w < 0.85 {
                dot(.bubble, GenieVec(x, y), size: size, opacity: min(1, w * 5), tone: .spectrum, hue: hue)
            } else {
                let pop: Double = (w - 0.85) / 0.15
                dot(.ring, GenieVec(x, y), size: size * (1 + pop), opacity: 1 - pop, tone: .mist, weight: 0.8)
            }
        }
    }

    mutating func glitter() {
        let tones: [GenieGlimmerTone] = [.gold, .lilac, .glow, .mist]
        for j in 0..<22 {
            let phase: Double = (u * 1.3 + GenieSky.seed(j)).truncatingRemainder(dividingBy: 1)
            let x: Double = Double(box.minX) - 6 + (bw + 12) * GenieSky.seed(j + 50) + sin(age * 2 + Double(j)) * 3
            let y: Double = Double(box.minY) - 36 + phase * (bh + 48)
            let shape: GenieGlimmerShape = j % 3 == 0 ? .spark : .dust
            dot(
                shape, GenieVec(x, y), size: 1.5 + GenieSky.seed(j + 51) * 1.8,
                opacity: GenieSky.bell(phase) * (0.6 + 0.4 * sin(age * 9 + Double(j))), tone: tones[j % tones.count], angle: age * 170 + Double(j) * 20)
        }
    }

    mutating func jewelBeam() {
        let reach: Double = GenieSky.ease(GenieSky.window(u, 0.05, 0.3))
        let fade: Double = 1 - GenieSky.window(u, 0.8, 1)
        guard reach > 0 else { return }
        let end: GenieVec = crown.mix(center, reach)
        stroke(.beam, from: crown, to: end, weight: 2.2, opacity: fade * (0.8 + 0.2 * sin(age * 10)), tone: .glow)
        dot(.spark, crown, size: 5, opacity: fade, tone: .mist, angle: age * 90)
        let lit: Double = GenieSky.window(u, 0.25, 0.4) * fade
        if lit > 0 {
            put(GenieGlimmer(
                .glow, x: center.x, y: center.y, size: max(bw, bh) / 2, opacity: lit * (0.7 + 0.3 * sin(age * 4)), tone: .glow,
                box: box.insetBy(dx: -6, dy: -6)))
        }
    }

    mutating func ripple() {
        let aspect: Double = min(max((bh + 24) / (bw + 24), 0.4), 1)
        for j in 0..<4 {
            let start: Double = 0.04 + Double(j) * 0.17
            let w: Double = GenieSky.window(u, start, start + 0.5)
            guard GenieSky.live(w) else { continue }
            let r: Double = 6 + w * (max(bw, bh) * 0.6 + 24)
            dot(.ring, center, size: r, opacity: (1 - w) * min(1, w * 8) * 0.9, tone: j % 2 == 0 ? .glow : .lilac, squash: aspect, weight: 1.6)
        }
    }

    mutating func moons() {
        let tones: [GenieGlimmerTone] = [.mist, .gold, .lilac]
        for j in 0..<3 {
            let angle: Double = age * 4 + Double(j) / 3 * Double.pi * 2
            let orbit: GenieVec = at(body, cos(angle) * 0.34, 0.24 + sin(angle) * 0.08)
            let start: Double = 0.48 + Double(j) * 0.06
            let fly: Double = GenieSky.window(u, start, start + 0.3)
            let landing: GenieVec = center + GenieVec(cos(Double(j) * 2.1) * 12, sin(Double(j) * 2.1) * 8)
            let spot: GenieVec = orbit.mix(landing, GenieSky.ease(fly))
            let arrive: Double = GenieSky.window(u, start + 0.3, min(start + 0.45, 1))
            let front: Bool = fly > 0 || sin(angle) > 0
            dot(.moon, spot, size: side * 0.045 + 2 * fly, opacity: 1 - arrive, tone: tones[j], front: front, angle: -30 + age * 20)
            if GenieSky.live(arrive) {
                for k in 0..<4 {
                    let a: Double = Double(k) / 4 * Double.pi * 2 + Double(j)
                    dot(.spark, landing + GenieVec(cos(a), sin(a)) * (4 + 14 * arrive), size: 3, opacity: 1 - arrive, tone: tones[j], angle: 45)
                }
            }
        }
    }

    mutating func constellation() {
        let fade: Double = 1 - GenieSky.window(u, 0.82, 1)
        var stars: [GenieVec] = []
        for j in 0..<7 {
            let angle: Double = Double(j) / 7 * Double.pi * 2 + GenieSky.seed(j) * 0.5
            let rx: Double = bw / 2 + 10 + GenieSky.seed(j + 60) * 14
            let ry: Double = bh / 2 + 10 + GenieSky.seed(j + 61) * 12
            stars.append(center + GenieVec(cos(angle) * rx, sin(angle) * ry))
        }
        for j in 0..<7 {
            let reveal: Double = GenieSky.ease(GenieSky.window(u, 0.38 + Double(j) * 0.04, 0.5 + Double(j) * 0.04))
            if reveal > 0 {
                stroke(.line, from: stars[j], to: stars[(j + 1) % 7], weight: 0.8, opacity: fade * 0.55, tone: .glow, reveal: reveal)
            }
        }
        for j in 0..<7 {
            let shown: Double = GenieSky.window(u, 0.04 + Double(j) * 0.05, 0.14 + Double(j) * 0.05)
            guard shown > 0 else { continue }
            dot(
                .star, stars[j], size: 3.2 + GenieSky.seed(j + 62) * 1.6, opacity: shown * fade * (0.7 + 0.3 * sin(age * 6 + Double(j))),
                tone: j % 2 == 0 ? .mist : .gold)
        }
    }

    mutating func crescent() {
        let w: Double = GenieSky.window(u, 0.04, 0.42)
        guard w > 0 else { return }
        let fade: Double = 1 - GenieSky.window(u, 0.82, 1)
        let moon: GenieVec = GenieVec(center.x, center.y + (Double(box.minY) - 26 - center.y) * GenieSky.ease(w))
        dot(.glow, moon, size: 22, opacity: min(1, w * 3) * fade * 0.4, tone: .gold, front: false)
        dot(.moon, moon, size: 13, opacity: min(1, w * 3) * fade, tone: .gold, angle: -20)
        let offsets: [GenieVec] = [GenieVec(-30, -8), GenieVec(24, -18), GenieVec(34, 6), GenieVec(-18, -26), GenieVec(8, -34)]
        let stars: Double = GenieSky.window(u, 0.3, 0.5)
        for (j, offset) in offsets.enumerated() where stars > 0 {
            dot(
                .spark, moon + offset, size: 2.5 + Double(j % 3) * 0.5, opacity: stars * fade * (0.5 + 0.5 * sin(age * 7 + Double(j) * 1.3)),
                tone: j % 2 == 0 ? .mist : .glow, angle: 45)
        }
    }

    mutating func mirage() {
        for j in 0..<2 {
            let start: Double = Double(j) * 0.3
            let w: Double = GenieSky.window(u, start, start + 0.65)
            guard GenieSky.live(w) else { continue }
            let ghost: CGRect = box.offsetBy(dx: CGFloat(sin(age * 3 + Double(j)) * 3), dy: CGFloat(-w * 44))
            put(GenieGlimmer(
                .outline, x: Double(ghost.midX), y: Double(ghost.midY), size: 1.4, opacity: (1 - w) * min(1, w * 5) * 0.8,
                tone: j == 0 ? .lilac : .glow, angle: age * 7, weight: 1.4, bend: 2.5 + w * 3, box: ghost))
        }
    }

    mutating func coins() {
        let x0: Double = Double(ground.spider.x) < center.x ? Double(box.minX) - 14 : Double(box.maxX) + 14
        let fade: Double = 1 - GenieSky.window(u, 0.85, 1)
        for j in 0..<8 {
            let start: Double = GenieSky.seed(j + 70) * 0.4
            let w: Double = GenieSky.window(u, start, start + 0.32)
            guard w > 0 else { continue }
            let x: Double = x0 + (GenieSky.seed(j + 71) - 0.5) * 14
            let floor: Double = Double(box.maxY) - 5 - Double(j % 3) * 4
            let y: Double = Double(box.minY) - 50 + (floor - Double(box.minY) + 50) * w * w
            let tumble: Double = w < 1 ? cos(age * 9 + Double(j)) : 1
            dot(.coin, GenieVec(x, y), size: 7, opacity: fade, tone: .gold, squash: tumble)
            let glint: Double = GenieSky.window(u, start + 0.32, start + 0.5)
            if GenieSky.live(glint) {
                dot(.spark, GenieVec(x + 2, y - 2), size: 3.5, opacity: GenieSky.bell(glint) * fade, tone: .mist, angle: 45)
            }
        }
    }

    mutating func flourish() {
        let head: Double = GenieSky.ease(GenieSky.window(u, 0.05, 0.6))
        let tail: Double = GenieSky.ease(GenieSky.window(u, 0.55, 0.95))
        guard head > tail else { return }
        let chord: GenieVec = center - crown
        let across: GenieVec = chord.normal.unit
        let curl: Double = min(30, chord.length * 0.15)
        func point(_ t: Double) -> GenieVec {
            let loop: GenieVec = across * (sin(t * Double.pi * 3) * curl) + chord.unit * (cos(t * Double.pi * 3) - 1) * curl * 0.4 * sin(Double.pi * t)
            return bowed(crown, center, 0.3, t) + loop
        }
        let pieces: Int = 32
        for j in 0..<pieces {
            let a: Double = Double(j) / Double(pieces)
            let b: Double = Double(j + 1) / Double(pieces)
            guard a >= tail, b <= head + 0.0001 else { continue }
            let fresh: Double = (a - tail) / max(head - tail, 0.01)
            stroke(.line, from: point(a), to: point(b), weight: 2.2, opacity: 0.4 + 0.6 * fresh, tone: fresh > 0.7 ? .glow : .lilac)
        }
        if head < 1 { dot(.spark, point(head), size: 4, opacity: 1, tone: .gold, angle: age * 120) }
    }

    mutating func incense() {
        dot(.glow, at(ground, 0, -0.06), size: side * 0.62, opacity: 0.45, tone: .violet, front: false)
        for j in 0..<5 {
            let phase: Double = (u * 1.4 + Double(j) / 5).truncatingRemainder(dividingBy: 1)
            let spot: GenieVec = at(ground, -0.5 + Double(j) * 0.25, -0.36 + phase * 0.6)
            dot(
                .wisp, spot, size: side * 0.3, opacity: GenieSky.bell(phase) * 0.65, tone: tone(j, [.lilac, .violet, .mist]), front: j % 2 == 0,
                angle: age * 2 + Double(j), weight: 1.6)
        }
    }

    mutating func whirlwind() {
        let scatter: Double = GenieSky.window(u, 0.8, 1)
        for j in 0..<14 {
            let h: Double = (Double(j) / 14 + u * 0.9).truncatingRemainder(dividingBy: 1)
            let angle: Double = age * 8 + Double(j) * 2.4
            let radius: Double = (0.18 + 0.38 * h) * (1 + scatter * 1.5)
            let spot: GenieVec = at(ground, cos(angle) * radius, GenieMagic.feet + h * 0.95 + sin(angle) * 0.05)
            dot(
                .puff, spot, size: side * (0.04 + 0.05 * h), opacity: (1 - scatter) * GenieSky.bell(h) * 0.75, tone: tone(j, [.violet, .lilac, .plum]),
                front: sin(angle) > 0)
        }
    }
}
