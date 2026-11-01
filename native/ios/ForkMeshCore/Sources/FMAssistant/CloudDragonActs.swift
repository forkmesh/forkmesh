import Foundation

public enum DragonKind: String, CaseIterable, Sendable {
    case heading, short, button, link, nav, image, logo, input, code, card

    public init(_ kind: GenieLandmarkKind) {
        switch kind {
        case .button, .toggle: self = .button
        case .link: self = .link
        case .tab: self = .nav
        case .field, .slider: self = .input
        case .image: self = .image
        case .heading: self = .heading
        case .text: self = .short
        case .progress: self = .card
        }
    }
}

public struct DragonBeat: Sendable {
    public var age: Double
    public var duration: Double
    public var t: Double

    public init(age: Double, duration: Double, t: Double) {
        self.age = age
        self.duration = duration
        self.t = t
    }

    public var u: Double { duration > 0 ? DragonMath.clamp01(age / duration) : 0 }

    public func hold(_ from: Double = 0.08, _ to: Double = 0.92) -> Double {
        DragonMath.smooth(DragonMath.span(u, 0, from)) * (1 - DragonMath.smooth(DragonMath.span(u, to, 1)))
    }
}

public enum DragonAnchor: String, Sendable {
    case ground, pawN, pawF, torso, chest
}

public enum DragonOrder: String, Sendable {
    case back, under, mid, hug, front
}

public struct DragonGear: Equatable, Sendable {
    public let prop: String
    public let at: DragonAnchor
    public let x: Double
    public let y: Double
    public let scale: Double
    public let order: DragonOrder
    public let beat: Bool

    public init(_ prop: String, at: DragonAnchor, x: Double, y: Double, scale: Double, order: DragonOrder = .mid, beat: Bool = false) {
        self.prop = prop
        self.at = at
        self.x = x
        self.y = y
        self.scale = scale
        self.order = order
        self.beat = beat
    }
}

public enum DragonActivity: String, CaseIterable, Sendable {
    case breatheFire = "breathe-fire"
    case land, terminal, coffee, book, magnify, celebrate, headphones
    case codeFast = "code-fast"
    case upload, protect, build, share, binoculars, travel, sleep, stretch, star, globe, heart

    public var title: String {
        switch self {
        case .breatheFire: return "Breathes a little fire"
        case .land: return "Drops in with a dusty landing"
        case .terminal: return "Taps away at a little terminal"
        case .coffee: return "Sips a warm cup of coffee"
        case .book: return "Reads a page or two"
        case .magnify: return "Inspects it through a magnifying glass"
        case .celebrate: return "Throws confetti"
        case .headphones: return "Vibes to a tune on headphones"
        case .codeFast: return "Codes at full speed"
        case .upload: return "Hugs a cloud while it syncs"
        case .protect: return "Stands guard with a shield"
        case .build: return "Stacks up some blocks"
        case .share: return "Sends off a paper plane"
        case .binoculars: return "Scans the page through binoculars"
        case .travel: return "Sets off with a backpack"
        case .sleep: return "Naps on a cloud"
        case .stretch: return "Wakes up with a big stretch"
        case .star: return "Cuddles a shiny star"
        case .globe: return "Surfs the network on a globe"
        case .heart: return "Hugs a big heart"
        }
    }

    public var duration: Double {
        switch self {
        case .breatheFire: return 3
        case .land: return 2.4
        case .terminal: return 3.6
        case .coffee: return 4
        case .book: return 4
        case .magnify: return 3.6
        case .celebrate: return 3.2
        case .headphones: return 4.2
        case .codeFast: return 2.8
        case .upload: return 3.8
        case .protect: return 3.4
        case .build: return 3.2
        case .share: return 3.4
        case .binoculars: return 3.6
        case .travel: return 3.4
        case .sleep: return 6
        case .stretch: return 2.4
        case .star: return 3.2
        case .globe: return 4.2
        case .heart: return 3.2
        }
    }

    public var fits: [DragonKind] {
        switch self {
        case .breatheFire: return [.heading, .logo, .card, .image]
        case .terminal: return [.code, .input, .card]
        case .book: return [.heading, .code, .card]
        case .magnify: return [.code, .image, .heading, .card]
        case .celebrate: return [.button, .heading, .card, .logo]
        case .codeFast: return [.code, .input]
        case .protect: return [.button, .input, .logo, .card]
        case .build: return [.card, .button, .code]
        case .share: return [.link, .button, .nav]
        case .star: return [.button, .heading, .logo]
        case .globe: return [.link, .nav, .image, .logo]
        case .heart: return [.image, .heading, .card, .logo]
        default: return DragonKind.allCases
        }
    }

    public var gear: [DragonGear] {
        switch self {
        case .terminal: return [DragonGear("laptop", at: .ground, x: 330, y: 1135, scale: 1)]
        case .coffee: return [DragonGear("mug", at: .pawN, x: 88, y: -18, scale: 1.05, order: .hug)]
        case .book: return [DragonGear("book", at: .pawN, x: 118, y: -20, scale: 1, order: .hug)]
        case .codeFast: return [DragonGear("laptop", at: .ground, x: 340, y: 1135, scale: 1)]
        case .upload: return [DragonGear("upcloud", at: .ground, x: 190, y: 1040, scale: 0.86, order: .hug)]
        case .protect: return [DragonGear("shield", at: .ground, x: 285, y: 925, scale: 0.6, order: .hug)]
        case .build: return [DragonGear("blocks", at: .ground, x: 400, y: 1115, scale: 1)]
        case .travel: return [DragonGear("backpack", at: .torso, x: 545, y: 830, scale: 1, order: .under)]
        case .star: return [DragonGear("star", at: .chest, x: 190, y: 860, scale: 1)]
        case .globe: return [DragonGear("globe", at: .ground, x: 0, y: 760, scale: 1, order: .hug)]
        case .heart: return [DragonGear("heart", at: .chest, x: 180, y: 860, scale: 1, beat: true)]
        default: return []
        }
    }

    public func stride(_ beat: DragonBeat) -> Double? { self == .travel ? beat.age * 9 : nil }

    public var props: [String] { gear.map(\.prop) }

    public static func drawn(from bag: inout [DragonActivity], kinds: [DragonKind], random: () -> Double) -> DragonActivity? {
        let wanted: Set<DragonKind> = Set(kinds)
        let fitting: (DragonActivity) -> Bool = { (act: DragonActivity) -> Bool in act.fits.contains { wanted.contains($0) } }
        if !bag.contains(where: fitting) {
            var fresh: [DragonActivity] = allCases
            var k: Int = fresh.count - 1
            while k > 0 {
                let j: Int = min(Int(random() * Double(k + 1)), k)
                fresh.swapAt(k, j)
                k -= 1
            }
            bag = fresh
        }
        guard let index = bag.firstIndex(where: fitting) else { return nil }
        return bag.remove(at: index)
    }

    public func pose(_ c: DragonBeat) -> DragonTarget {
        typealias M = DragonMath
        func span(_ from: Double, _ to: Double) -> Double { M.span(c.u, from, to) }
        func smooth(_ v: Double) -> Double { M.smooth(v) }
        func bump(_ from: Double, _ to: Double) -> Double { M.bump(c.u, from, to) }
        func wave(_ speed: Double, _ phase: Double = 0) -> Double { M.wave(c.age, speed, phase) }
        func hold(_ from: Double = 0.08, _ to: Double = 0.92) -> Double { c.hold(from, to) }
        var p: DragonTarget = [:]
        switch self {
        case .breatheFire:
            let inhale: Double = smooth(span(0.02, 0.2)) * (1 - smooth(span(0.2, 0.28)))
            let blow: Double = smooth(span(0.22, 0.3)) * (1 - smooth(span(0.82, 0.94)))
            p[.head] = -0.26 * inhale - 0.05 * blow
            p[.lean] = -0.1 * inhale + 0.1 * blow
            p[.x] = -22 * blow
            p[.chest] = 0.05 * inhale
            p[.open] = 0.25 * inhale + blow
            p[.wing] = 0.25 * inhale - 0.1 * blow
            p[.lid] = blow
            p[.happy] = blow
            p[.jitter] = blow * 3
            p.put(.pawN, -30 * blow, -40 * blow)
            p.put(.pawF, -20 * blow, -30 * blow)
        case .land:
            let rise: Double = M.span(c.age, 0, 0.5)
            let fall: Double = M.span(c.age, 0.5, 0.9)
            let hit: Double = M.span(c.age, 0.9, 1.5)
            let up: Double = rise < 1 ? smooth(rise) : 1 - fall * fall
            p[.y] = -620 * up
            p[.flap] = up * (1 - fall)
            p[.lean] = 0.3 * up
            p[.tail] = up * 0.8
            p[.crouch] = (hit > 0 && hit < 1 ? 1 - hit : 0) + 0.3 * M.bump(c.age, 0, 0.14)
            p.put(.pawN, 30 * up, -130 * up)
            p.put(.pawF, 30 * up, -130 * up)
            p.put(.footN, 0, -40 * up)
            p.put(.footF, 0, -40 * up)
            p[.lid] = hit > 0 && hit < 0.6 ? 1 : 0
            p[.happy] = hit > 0 && hit < 0.6 ? 1 : 0
            p[.wing] = 0.5 * (hit > 0 ? 1 - hit : 0)
        case .terminal:
            let on: Double = hold()
            p[.crouch] = 0.22 * on
            p[.lean] = 0.14 * on
            p[.head] = 0.1 * on + 0.015 * wave(2.2)
            p[.gx] = 0.4
            p[.gy] = 0.5
            p.put(.pawN, 120 * on, (40 + max(wave(19), 0) * -22) * on)
            p.put(.pawF, 85 * on, (70 + max(wave(19, 3.1), 0) * -22) * on)
        case .coffee:
            let on: Double = hold()
            let sip: Double = bump(0.25, 0.48) + bump(0.6, 0.82)
            p[.head] = 0.06 * on - 0.2 * sip
            p[.lean] = -0.05 * sip
            p[.lid] = sip
            p[.happy] = sip
            p[.wing] = 0.03 * sip
            p[.reach] = on
            p.put(.pawN, 150 + 30 * sip, -30 - 150 * sip)
            p.put(.pawF, 255 + 20 * sip, -10 - 150 * sip)
        case .book:
            let on: Double = hold()
            p[.head] = 0.14 * on + wave(1.1) * 0.02
            p[.gx] = wave(2.1) * 0.6
            p[.gy] = 0.75 * on
            p[.gh] = 0.2
            p[.gv] = 0.6
            p[.reach] = on
            p.put(.pawN, 75, 140)
            p.put(.pawF, 335, 115)
        case .magnify:
            let on: Double = hold()
            let sweep: Double = wave(1.6)
            p[.y] = -64 * on
            p[.lean] = 0.72 * on
            p[.crouch] = on
            p[.head] = 0.05 * on + sweep * 0.035
            p[.hy] = 44 * on
            p[.x] = sweep * 14
            p[.gx] = 0.9
            p[.gy] = 0.35
            p[.gh] = 0.1
            p[.reach] = on
            p[.wing] = 0.14
            p.put(.pawN, 205, 235)
            p.put(.pawF, 80, 250)
            p.put(.footN, -40, 64)
            p.put(.footF, -30, 64)
        case .celebrate:
            let h: Double = abs(wave(5.2)) * hold()
            p[.y] = -110 * h
            p[.crouch] = 0.2 * (1 - h)
            p[.open] = 1
            p[.lid] = 1
            p[.happy] = 1
            p[.head] = wave(2.6) * 0.1
            p[.wing] = 0.25
            p[.flap] = 0.3
            p.put(.pawN, -210, -340)
            p.put(.pawF, 210, -340)
            p.put(.footN, 0, -26 * h)
            p.put(.footF, 0, -26 * h)
            p[.front] = 1
        case .headphones:
            let on: Double = hold()
            let beat: Double = wave(7.5)
            p[.head] = wave(3.75) * 0.1 * on
            p[.y] = -abs(beat) * 16 * on
            p[.lid] = on
            p[.lean] = wave(3.75) * 0.03
            p.put(.footN, 0, -max(beat, 0) * 34 * on)
            p.put(.pawN, 10, -30 + wave(7.5) * 18)
            p.put(.pawF, 10, -30 - wave(7.5) * 18)
            p[.tail] = 0.2
            p[.sway] = 2
        case .codeFast:
            let on: Double = hold()
            p[.crouch] = 0.25 * on
            p[.lean] = 0.24 * on
            p[.head] = 0.08 * on
            p[.jitter] = 5 * on
            p[.gx] = 0.4
            p[.gy] = 0.5
            p[.tail] = 0.7
            p[.sway] = 3
            p[.open] = 0.3 * on
            p[.wing] = -0.1
            p.put(.pawN, 125 * on, (40 + max(wave(34), 0) * -26) * on)
            p.put(.pawF, 88 * on, (70 + max(wave(34, 3.1), 0) * -26) * on)
        case .upload:
            let on: Double = hold()
            p[.y] = wave(2.4) * 10
            p[.head] = 0.06 * on + wave(1.3) * 0.03
            p[.lid] = on
            p[.happy] = on
            p[.lean] = 0.05 * on
            p[.front] = 1
            p.put(.pawN, 150 * on, -60 * on)
            p.put(.pawF, 40 * on, -80 * on)
        case .protect:
            let on: Double = hold()
            let brace: Double = bump(0.08, 0.3)
            let nuzzle: Double = wave(1.5)
            p[.lean] = (0.14 + 0.05 * brace) * on
            p[.crouch] = 0.2 * on
            p[.head] = (0.2 + nuzzle * 0.02) * on
            p[.hx] = 18 * on
            p[.hy] = 56 * on
            p[.x] = 22 * brace
            p[.gx] = 0.7
            p[.gy] = -0.2
            p[.gh] = 0.1
            p[.wing] = 0.16 * brace
            p[.reach] = on
            p.put(.pawN, 150, 215)
            p.put(.pawF, 330, 230)
        case .build:
            let on: Double = hold()
            let tap: Double = max(0, wave(6))
            p[.lean] = 0.14 * on
            p[.crouch] = 0.14 * on
            p[.head] = 0.1 * on + tap * 0.03
            p[.gx] = 0.8
            p[.gy] = 0.5
            p.put(.pawF, 230 * on, (-30 - tap * 70) * on)
            p.put(.pawN, 140 * on, (40 - max(0, wave(6, 3.1)) * 60) * on)
        case .share:
            let wind: Double = bump(0, 0.24)
            let throwIt: Double = smooth(span(0.2, 0.28))
            let rest: Double = 1 - smooth(span(0.6, 0.86))
            p[.lean] = -0.1 * wind + 0.14 * throwIt * rest
            p[.head] = -0.1 * wind - 0.1 * rest * throwIt
            p[.gx] = 1
            p[.gy] = -0.7
            p[.open] = 0.6 * throwIt * rest
            p[.x] = -16 * wind + 16 * throwIt * rest
            p.put(.pawF, 250 * wind + 250 * throwIt * rest, -350 * wind - 200 * throwIt * rest)
            p[.front] = 1
            p.put(.footN, 0, -24 * bump(0.2, 0.34))
        case .binoculars:
            let on: Double = hold()
            let pan: Double = wave(1.3)
            p[.y] = -64 * on
            p[.lean] = 0.72 * on
            p[.crouch] = on
            p[.head] = -0.04 * on + pan * 0.07
            p[.hy] = 30 * on
            p[.x] = pan * 18
            p[.gx] = 1
            p[.gh] = 0.1
            p[.reach] = on
            p[.wing] = 0.16
            p.put(.pawN, 120, -40)
            p.put(.pawF, 440, -20)
            p.put(.footN, -40, 64)
            p.put(.footF, -30, 64)
        case .travel:
            let on: Double = hold()
            p[.ground] = on
            p[.lid] = on
            p[.happy] = on
            p[.tail] = 0.86 * on
            p[.tailBase] = 0.03 * on
            p[.sway] = 2.2
        case .sleep:
            let on: Double = smooth(M.span(c.age, 0, 0.8))
            p[.y] = -150 * on + wave(1.2) * 12
            p[.crouch] = on
            p[.lean] = 0.62 * on
            p[.head] = 0.7 * on
            p[.hy] = 150 * on
            p[.hx] = 40 * on
            p[.lid] = on
            p[.wing] = -0.25 * on
            p[.chest] = wave(1.6) * 0.035
            p.put(.pawN, 70 * on, 90 * on)
            p.put(.pawF, 10 * on, 90 * on)
            p[.tail] = -0.25 * on
            p[.sway] = 0.25
        case .stretch:
            let up: Double = bump(0, 0.82)
            p[.y] = -40 * up
            p[.chest] = 0.05 * up
            p[.lean] = -0.1 * up
            p[.head] = -0.2 * up
            p[.open] = up
            p[.oval] = 0.4
            p[.lid] = up
            p[.happy] = up
            p[.wing] = 0.42 * up
            p[.tail] = 0.5 * up
            p.put(.pawN, -230 * up, -420 * up)
            p.put(.pawF, 230 * up, -420 * up)
            p[.front] = 1
        case .star:
            let on: Double = hold()
            p[.head] = 0.14 * on + wave(2.4) * 0.05
            p[.lean] = wave(2.4) * 0.05
            p[.lid] = on
            p[.happy] = on
            p[.gx] = 0.3
            p.put(.pawN, 150 * on, -110 * on)
            p.put(.pawF, 40 * on, -120 * on)
            p[.x] = wave(2.4) * 12
        case .globe:
            let on: Double = hold(0.16, 0.86)
            let rock: Double = wave(2.2)
            p[.y] = -610 * on + wave(4.4) * 8 * on
            p[.x] = (-90 + rock * 26) * on
            p[.lean] = (0.55 + rock * 0.05) * on
            p[.crouch] = 0.8 * on
            p[.head] = (-0.3 - rock * 0.05) * on
            p[.hy] = 40 * on
            p[.open] = 0.35
            p[.wing] = 0.22 + rock * 0.08
            p[.tail] = 0.25 * on
            p[.sway] = 2
            p[.reach] = on
            p[.front] = on
            p[.gx] = 0.8
            p[.gy] = -0.7
            p.put(.pawN, 150, 210)
            p.put(.pawF, 330, 200)
        case .heart:
            let on: Double = hold()
            p[.head] = 0.12 * on + wave(2) * 0.04
            p[.lean] = wave(2) * 0.04
            p[.lid] = on
            p.put(.pawN, 150 * on, -100 * on)
            p.put(.pawF, 30 * on, -110 * on)
        }
        return p
    }
}
