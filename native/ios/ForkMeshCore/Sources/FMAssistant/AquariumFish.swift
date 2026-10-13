import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public struct AquariumSwimmer: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var model: String
    public var waiting: Bool

    public init(id: String, name: String, model: String, waiting: Bool = false) {
        self.id = id
        self.name = name
        self.model = model
        self.waiting = waiting
    }
}

public enum AquariumSpecies: String, Sendable, CaseIterable {
    case goldfish, clownfish, angelfish, puffer, tetra, betta

    public var height: Double {
        switch self {
        case .goldfish: return 0.56
        case .clownfish: return 0.5
        case .angelfish: return 0.95
        case .puffer: return 0.8
        case .tetra: return 0.4
        case .betta: return 0.62
        }
    }
}

public struct AquariumColor: Equatable, Sendable {
    public var red: Double
    public var green: Double
    public var blue: Double

    public init(_ red: Double, _ green: Double, _ blue: Double) {
        self.red = min(max(red, 0), 1)
        self.green = min(max(green, 0), 1)
        self.blue = min(max(blue, 0), 1)
    }

    public func shifted(_ amount: Double, _ seed: UInt64) -> AquariumColor {
        let a: Double = amount * (Double(seed & 0xff) / 255 - 0.5)
        let b: Double = amount * (Double((seed >> 8) & 0xff) / 255 - 0.5)
        let c: Double = amount * (Double((seed >> 16) & 0xff) / 255 - 0.5)
        return AquariumColor(red + a, green + b, blue + c)
    }

    public static let gold: AquariumColor = AquariumColor(1, 0.82, 0.3)
    public static let white: AquariumColor = AquariumColor(1, 1, 1)
}

public struct AquariumLook: Equatable, Sendable {
    public var species: AquariumSpecies
    public var body: AquariumColor
    public var fin: AquariumColor
    public var accent: AquariumColor
    public var size: Double

    public init(species: AquariumSpecies, body: AquariumColor, fin: AquariumColor, accent: AquariumColor, size: Double) {
        self.species = species
        self.body = body
        self.fin = fin
        self.accent = accent
        self.size = size
    }

    public static func hash(_ text: String) -> UInt64 {
        var value: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in text.utf8 {
            value ^= UInt64(byte)
            value = value &* 0x0000_0100_0000_01b3
        }
        return value
    }

    public static func of(name: String, model: String) -> AquariumLook {
        let seed: UInt64 = hash(name + "|" + model)
        let species: AquariumSpecies = AquariumSpecies.allCases[Int(seed % UInt64(AquariumSpecies.allCases.count))]
        let base: AquariumLook = palette(species)
        let size: Double = 0.85 + Double((seed >> 40) % 1000) / 1000 * 0.35
        return AquariumLook(
            species: species, body: base.body.shifted(0.16, seed >> 8), fin: base.fin.shifted(0.12, seed >> 24),
            accent: base.accent, size: size)
    }

    public static func palette(_ species: AquariumSpecies) -> AquariumLook {
        switch species {
        case .goldfish:
            return AquariumLook(species: species, body: AquariumColor(1, 0.55, 0.15), fin: AquariumColor(1, 0.76, 0.32), accent: AquariumColor(1, 0.9, 0.6), size: 1)
        case .clownfish:
            return AquariumLook(species: species, body: AquariumColor(1, 0.45, 0.1), fin: AquariumColor(0.95, 0.35, 0.08), accent: .white, size: 1)
        case .angelfish:
            return AquariumLook(species: species, body: AquariumColor(0.86, 0.88, 0.93), fin: AquariumColor(0.98, 0.85, 0.35), accent: AquariumColor(0.18, 0.18, 0.24), size: 1)
        case .puffer:
            return AquariumLook(species: species, body: AquariumColor(0.88, 0.8, 0.52), fin: AquariumColor(0.75, 0.68, 0.42), accent: AquariumColor(0.42, 0.3, 0.16), size: 1)
        case .tetra:
            return AquariumLook(species: species, body: AquariumColor(0.62, 0.72, 0.85), fin: AquariumColor(0.9, 0.18, 0.22), accent: AquariumColor(0.18, 0.62, 1), size: 1)
        case .betta:
            return AquariumLook(species: species, body: AquariumColor(0.55, 0.25, 0.85), fin: AquariumColor(0.86, 0.22, 0.56), accent: AquariumColor(0.35, 0.55, 1), size: 1)
        }
    }
}

public enum AquariumTrick: String, Sendable, CaseIterable {
    case peek, boop, read, underline, polish, bubbleRing, measure, highlight, quote, count, startle, perch, trace
    case hello, noseBoop, tag, orbit, bubbleCrown, highFive, race, serenade, legCount, photo, kiss

    public static let element: [AquariumTrick] = allCases.filter { !$0.withSpider }
    public static let spider: [AquariumTrick] = allCases.filter(\.withSpider)

    public var withSpider: Bool {
        switch self {
        case .hello, .noseBoop, .tag, .orbit, .bubbleCrown, .highFive, .race, .serenade, .legCount, .photo, .kiss: return true
        default: return false
        }
    }

    public var title: String {
        switch self {
        case .peek: return "peeks at"
        case .boop: return "boops"
        case .read: return "reads"
        case .underline: return "underlines"
        case .polish: return "polishes"
        case .bubbleRing: return "blows a bubble ring round"
        case .measure: return "measures"
        case .highlight: return "highlights"
        case .quote: return "quotes"
        case .count: return "counts the letters of"
        case .startle: return "gets startled by"
        case .perch: return "naps beside"
        case .trace: return "traces"
        case .hello: return "says hello to the spider"
        case .noseBoop: return "boops the spider's nose"
        case .tag: return "plays tag with the spider"
        case .orbit: return "swims rings round the spider"
        case .bubbleCrown: return "gives the spider a bubble crown"
        case .highFive: return "high-fives the spider"
        case .race: return "races the spider"
        case .serenade: return "sings to the spider"
        case .legCount: return "counts the spider's legs"
        case .photo: return "takes the spider's photo"
        case .kiss: return "kisses the spider"
        }
    }

    public var fits: Set<GenieLandmarkKind> {
        switch self {
        case .peek: return [.button, .tab, .toggle, .image, .link]
        case .boop: return [.button, .tab, .link]
        case .read: return [.heading, .text]
        case .underline: return [.text, .heading]
        case .polish: return [.button]
        case .bubbleRing: return [.button, .toggle, .image]
        case .measure: return Set(GenieLandmarkKind.allCases)
        case .highlight: return [.text, .heading]
        case .quote: return [.text, .heading, .button]
        case .count: return [.text, .heading, .button]
        case .startle: return [.button, .link]
        case .perch: return [.button, .heading]
        case .trace: return [.button, .image, .field]
        default: return []
        }
    }

    public func duration(_ text: String) -> Double {
        let letters: Int = Swift.max(AquariumTrick.letters(text).count, 1)
        switch self {
        case .peek: return 2.4
        case .boop: return 2.2
        case .read: return Swift.min(0.8 + Double(Swift.min(letters, 24)) * 0.14, 4.2)
        case .underline: return 2
        case .polish: return 2.4
        case .bubbleRing: return 2.4
        case .measure: return 2.6
        case .highlight: return 2.2
        case .quote: return 2.6
        case .count: return Swift.min(0.9 + Double(letters) * 0.18, 5.4)
        case .startle: return 1.9
        case .perch: return 3
        case .trace: return 2.6
        case .hello: return 2.2
        case .noseBoop: return 1.8
        case .tag: return 2.6
        case .orbit: return 3.2
        case .bubbleCrown: return 2.6
        case .highFive: return 1.8
        case .race: return 3.6
        case .serenade: return 2.6
        case .legCount: return 3.6
        case .photo: return 2.6
        case .kiss: return 2
        }
    }

    public func fits(_ mark: GenieLandmark) -> Bool {
        guard fits.contains(mark.kind) else { return false }
        let letters: Int = AquariumTrick.letters(mark.text).count
        switch self {
        case .read, .underline, .highlight, .quote: return letters > 0
        case .count: return (1...24).contains(letters)
        case .measure: return (60...900).contains(mark.rect.width)
        case .bubbleRing: return mark.kind != .button || mark.rect.width <= 90
        default: return true
        }
    }

    public static func letters(_ text: String) -> [Character] {
        text.filter { $0.isLetter || $0.isNumber }
    }
}

public enum AquariumPhase: String, Sendable {
    case entering, wandering, going, acting, leaving
}

public enum AquariumBit: Equatable, Sendable {
    case bubble, sparkle, heart, star, note, zzz, flash, photo, speed, splash
    case glyph(String)
}

public struct AquariumParticle: Equatable, Sendable {
    public var bit: AquariumBit
    public var point: CGPoint
    public var velocity: CGSize
    public var age: Double = 0
    public var life: Double
    public var size: Double
    public var center: CGPoint?
    public var radius: Double = 0
    public var angle: Double = 0
    public var spin: Double = 0

    public init(_ bit: AquariumBit, at point: CGPoint, velocity: CGSize = .zero, life: Double, size: Double) {
        self.bit = bit
        self.point = point
        self.velocity = velocity
        self.life = Swift.max(life, 0.05)
        self.size = size
    }

    public var progress: Double { Swift.min(Swift.max(age / life, 0), 1) }
}

public enum AquariumMarkKind: Equatable, Sendable {
    case ring, underline, highlight, trace
    case tape(String)
}

public struct AquariumMark: Equatable, Sendable {
    public var kind: AquariumMarkKind
    public var rect: CGRect
    public var age: Double = 0
    public var life: Double
    public var tint: AquariumColor

    public init(_ kind: AquariumMarkKind, rect: CGRect, life: Double, tint: AquariumColor) {
        self.kind = kind
        self.rect = rect
        self.life = Swift.max(life, 0.05)
        self.tint = tint
    }

    public var progress: Double { Swift.min(Swift.max(age / life, 0), 1) }
}

public struct AquariumAct: Equatable, Sendable {
    public var trick: AquariumTrick
    public var target: CGRect
    public var text: String
    public var spot: CGPoint
    public var start: Double = 0
    public var duration: Double
    public var fired: Set<Int> = []
    public var away: CGPoint?
    public var angle: Double = 0

    public init(trick: AquariumTrick, target: CGRect, text: String, spot: CGPoint) {
        self.trick = trick
        self.target = target
        self.text = text
        self.spot = spot
        self.duration = trick.duration(text)
    }

    public mutating func beat(_ index: Int, at seconds: Double, age: Double) -> Bool {
        guard age >= seconds, !fired.contains(index) else { return false }
        fired.insert(index)
        return true
    }
}

public struct AquariumFish: Equatable, Sendable, Identifiable {
    public nonisolated static let baseLength: CGFloat = 34
    public nonisolated static let labelHeight: CGFloat = 14
    public nonisolated static let longestLabel: Int = 16

    public let id: String
    public var name: String
    public var look: AquariumLook
    public var waiting: Bool
    public var point: CGPoint
    public var velocity: CGSize = .zero
    public var goal: CGPoint
    public var phase: AquariumPhase = .entering
    public var act: AquariumAct?
    public var restUntil: Double = 0
    public var deadline: Double = 0
    public var speech: String?
    public var speechUntil: Double = 0
    public var speechBox: CGRect?
    public var opacity: Double = 1
    public var appear: Double = 0
    public var facingLeft: Bool = false
    public var swim: Double = 0
    public var nextCall: Double = 0
    public var nudge: CGSize = .zero

    public init(id: String, name: String, model: String, waiting: Bool, at point: CGPoint) {
        self.id = id
        self.name = name
        self.look = AquariumLook.of(name: name, model: model)
        self.waiting = waiting
        self.point = point
        self.goal = point
    }

    public var length: CGFloat { AquariumFish.baseLength * CGFloat(look.size) }

    public var height: CGFloat { length * CGFloat(look.species.height) }

    public var label: String {
        let clean: String = GenieLandmarks.clean(name)
        guard clean.count > AquariumFish.longestLabel else { return clean }
        return String(clean.prefix(AquariumFish.longestLabel - 1)).trimmingCharacters(in: .whitespaces) + "…"
    }

    public var drawn: CGPoint { CGPoint(x: point.x + nudge.width, y: point.y + nudge.height) }

    public func body(at center: CGPoint) -> CGRect {
        let width: CGFloat = length * 1.2 + 6
        let above: CGFloat = height / 2 + 4 + (waiting ? 16 : 0)
        let below: CGFloat = height / 2 + 2 + AquariumFish.labelHeight
        return CGRect(x: center.x - width / 2, y: center.y - above, width: width, height: above + below)
    }

    public var box: CGRect { body(at: drawn) }
}

public struct AquariumSpider: Equatable, Sendable {
    public var point: CGPoint
    public var side: CGFloat
    public var free: Bool

    public init(point: CGPoint, side: CGFloat, free: Bool) {
        self.point = point
        self.side = side
        self.free = free
    }

    public var rect: CGRect {
        let half: CGFloat = side * 0.4
        return CGRect(x: point.x - half, y: point.y - half, width: half * 2, height: half * 2)
    }
}

public enum AquariumSpiderAsk: Equatable, Sendable {
    case wave, cheer, giggle, surprise, eureka, dizzy, nod
    case walk(CGPoint)
    case look(CGPoint)
}

public enum AquariumEvent: Equatable, Sendable {
    case spider(AquariumSpiderAsk)
    case log(String)
}

public enum AquariumClear {
    public nonisolated static let gaps: [CGFloat] = [10, 22, 40]
    public nonisolated static let room: CGFloat = 8

    public static func fits(_ box: CGRect, in water: CGRect) -> Bool {
        box.minX >= water.minX && box.maxX <= water.maxX && box.minY >= water.minY && box.maxY <= water.maxY
    }

    public static func clear(_ box: CGRect, blockers: [CGRect], water: CGRect, room: CGFloat = 0) -> Bool {
        fits(box, in: water) && !GenieLandmarks.overlaps(box.insetBy(dx: -room, dy: -room), blockers)
    }

    public static func spot(
        beside target: CGRect, body: (CGPoint) -> CGRect, blockers: [CGRect], water: CGRect, prefer: CGPoint
    ) -> CGPoint? {
        let sample: CGRect = body(.zero)
        let left: CGFloat = -sample.minX
        let right: CGFloat = sample.maxX
        let up: CGFloat = -sample.minY
        let down: CGFloat = sample.maxY
        var options: [CGPoint] = []
        for gap in gaps {
            let xs: [CGFloat] = [target.minX - gap - right, target.maxX + gap + left]
            let ys: [CGFloat] = [target.minY - gap - down, target.maxY + gap + up]
            for x in xs { options.append(CGPoint(x: x, y: target.midY)) }
            for y in ys {
                options.append(CGPoint(x: target.midX, y: y))
                if target.width > sample.width * 2 {
                    options.append(CGPoint(x: target.minX + right, y: y))
                    options.append(CGPoint(x: target.maxX - left, y: y))
                }
            }
            for x in xs {
                for y in ys { options.append(CGPoint(x: x, y: y)) }
            }
        }
        let avoid: [CGRect] = blockers + [target]
        let clear: [CGPoint] = options.filter { AquariumClear.clear(body($0), blockers: avoid, water: water, room: room) }
        return clear.min { hypot($0.x - prefer.x, $0.y - prefer.y) < hypot($1.x - prefer.x, $1.y - prefer.y) }
    }

    public static func waypoint(
        in area: CGRect, body: (CGPoint) -> CGRect, blockers: [CGRect], water: CGRect, random: () -> Double
    ) -> CGPoint {
        var last: CGPoint = CGPoint(x: area.midX, y: area.midY)
        for _ in 0..<18 {
            let point: CGPoint = CGPoint(x: area.minX + area.width * CGFloat(random()), y: area.minY + area.height * CGFloat(random()))
            last = point
            if clear(body(point), blockers: blockers, water: water, room: room) { return point }
        }
        return last
    }

    public static func speech(_ text: String, body: CGRect, blockers: [CGRect], screen: CGRect) -> CGRect? {
        let width: CGFloat = CGFloat(min(max(Double(text.count) * 6.6 + 20, 32), 200))
        let height: CGFloat = 24
        let options: [CGRect] = [
            CGRect(x: body.midX - width / 2, y: body.minY - 4 - height, width: width, height: height),
            CGRect(x: body.maxX + 4, y: body.midY - height / 2, width: width, height: height),
            CGRect(x: body.minX - 4 - width, y: body.midY - height / 2, width: width, height: height),
            CGRect(x: body.midX - width / 2, y: body.maxY + 4, width: width, height: height),
        ]
        return options.first { clear($0, blockers: blockers, water: screen) }
    }

    public static func opacity(_ box: CGRect, blockers: [CGRect]) -> Double {
        GenieLandmarks.overlaps(box, blockers) ? AquariumTank.crossing : 1
    }
}
