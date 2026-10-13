import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieScreenAct: String, Sendable, CaseIterable {
    case look, read, waveAt, pointAt, eureka, cheer, sniff, poke, silk, giggle, salute, blowKiss, highFive
    case trace, underline, boop, highlight, perch, tabHop, measure, cobweb, lantern
    case poof, lamp, wishes, smokeRings, shimmer, stardust, vortex, crystal, runes, halo, fireflies, rainbow, fireworks, bubbles, glitter
    case jewelBeam, ripple, moons, constellation, crescent, mirage, coins, flourish

    public var magic: GenieMagic? { GenieMagic(rawValue: rawValue) }

    public var title: String {
        switch self {
        case .look: return "Look at it"
        case .read: return "Read it"
        case .waveAt: return "Wave at a button"
        case .pointAt: return "Point at a picture"
        case .eureka: return "Eureka at a text box"
        case .cheer: return "Cheer a progress bar"
        case .sniff: return "Sniff it"
        case .poke: return "Poke the air beside it"
        case .silk: return "Silk thread to it"
        case .giggle: return "Giggle at a link"
        case .salute: return "Salute a tab"
        case .blowKiss: return "Blow it a kiss"
        case .highFive: return "High-five a switch"
        case .poof: return "Poof over to it"
        case .lamp: return "Lamp smoke to it"
        case .wishes: return "Send it three wishes"
        case .smokeRings: return "Blow smoke rings at it"
        case .shimmer: return "Shimmer it gold"
        case .stardust: return "Pour stardust on it"
        case .vortex: return "Spin sparks round it"
        case .crystal: return "Gaze at it in a crystal ball"
        case .runes: return "Circle it with runes"
        case .halo: return "Give it a halo"
        case .fireflies: return "Send fireflies to it"
        case .rainbow: return "Rainbow to it"
        case .fireworks: return "Fireworks over it"
        case .bubbles: return "Bubbles from it"
        case .glitter: return "Glitter on it"
        case .jewelBeam: return "Jewel beam at it"
        case .ripple: return "Ripple it"
        case .moons: return "Send it little moons"
        case .constellation: return "Stars round it"
        case .crescent: return "Crescent moon over it"
        case .mirage: return "Mirage of it"
        case .coins: return "Coins beside it"
        case .flourish: return "Flourish to it"
        case .trace: return "Trace round it"
        case .underline: return "Underline it"
        case .boop: return "Boop it"
        case .highlight: return "Highlight it"
        case .perch: return "Nap on top"
        case .tabHop: return "Hop along the tabs"
        case .measure: return "Measure it"
        case .cobweb: return "Cobweb in the corner"
        case .lantern: return "Light it up"
        }
    }

    public var fits: Set<GenieLandmarkKind> {
        switch self {
        case .look: return Set(GenieLandmarkKind.allCases)
        case .read: return [.heading, .text]
        case .waveAt: return [.button, .tab]
        case .pointAt: return [.image]
        case .eureka: return [.field]
        case .cheer: return [.progress, .slider]
        case .sniff: return [.button, .image, .text]
        case .poke: return [.button, .toggle, .tab]
        case .silk: return [.button, .heading, .image, .field]
        case .giggle: return [.link]
        case .salute: return [.tab, .heading]
        case .blowKiss: return [.image, .heading]
        case .highFive: return [.toggle, .slider]
        case .poof: return [.button, .image, .heading, .tab]
        case .lamp: return [.field, .button, .image]
        case .wishes: return [.button, .image, .heading]
        case .smokeRings: return [.button, .heading, .text, .image]
        case .shimmer: return [.button, .heading, .tab, .progress]
        case .stardust: return [.heading, .image, .field, .text]
        case .vortex: return [.button, .toggle, .image]
        case .crystal: return [.image, .text, .heading, .field]
        case .runes: return [.button, .field, .image]
        case .halo: return [.image, .heading, .button, .tab]
        case .fireflies: return [.text, .heading, .link, .image]
        case .rainbow: return [.heading, .image, .progress, .slider]
        case .fireworks: return [.button, .heading, .progress]
        case .bubbles: return [.field, .text, .image, .toggle]
        case .glitter: return [.heading, .image, .tab, .link]
        case .jewelBeam: return [.button, .link, .toggle, .field]
        case .ripple: return [.button, .toggle, .slider, .tab]
        case .moons: return [.image, .heading, .text]
        case .constellation: return [.image, .heading, .button]
        case .crescent: return [.heading, .tab, .image]
        case .mirage: return [.button, .image, .field, .text]
        case .coins: return [.progress, .slider, .button, .link]
        case .flourish: return [.link, .heading, .text, .button]
        case .trace: return [.button, .image, .field, .toggle]
        case .underline: return [.heading, .text, .link]
        case .boop: return [.button, .tab, .toggle]
        case .highlight: return [.heading, .text]
        case .perch: return [.heading, .button]
        case .tabHop: return [.tab, .button]
        case .measure: return Set(GenieLandmarkKind.allCases)
        case .cobweb: return [.image, .field]
        case .lantern: return [.heading, .text]
        }
    }

    public var trick: String {
        if let magic = magic { return magic.trick }
        switch self {
        case .look: return "gasp"
        case .read: return "nod"
        case .waveAt: return "wave"
        case .pointAt: return "ponder"
        case .eureka: return "eureka"
        case .cheer: return "applause"
        case .sniff: return "sneeze"
        case .poke: return "fist_bump"
        case .silk: return "spin_web"
        case .giggle: return "giggle"
        case .salute: return "salute"
        case .blowKiss: return "blow_kiss"
        case .highFive: return "high_five"
        case .trace: return "dust_off"
        case .underline: return "stretch"
        case .boop: return "bounce"
        case .highlight: return "sparkle"
        case .perch: return "nap"
        case .tabHop: return "jump"
        case .measure: return "lookout"
        case .cobweb: return "catch_fly"
        case .lantern: return "glow_up"
        default: return "gasp"
        }
    }

    public var sweeps: Bool { self == .read || self == .lantern || self == .highlight }

    public var threads: Bool { self == .silk }

    public var blinks: Bool { self == .poof }

    public var needsText: Bool { self == .read || self == .highlight || self == .lantern }

    public var travels: Bool {
        switch self {
        case .trace, .underline, .boop, .highlight, .perch, .tabHop, .measure, .cobweb, .lantern: return true
        default: return false
        }
    }

    public var widths: ClosedRange<CGFloat> {
        switch self {
        case .measure: return 80...900
        case .underline, .highlight, .lantern: return 40...900
        case .trace, .perch, .cobweb: return 24...600
        default: return 0...CGFloat.greatestFiniteMagnitude
        }
    }

    public func line(_ text: String) -> String {
        let name: String = GenieScreenPlay.short(text)
        let has: Bool = !name.isEmpty
        switch self {
        case .look: return has ? "Ooh, “\(name)”!" : "Ooh, what's that?"
        case .read: return "“\(name)”… mm-hmm."
        case .waveAt: return has ? "Hi, \(name)!" : "Hi there, button!"
        case .pointAt: return has ? "Look, \(name)!" : "Look at that picture!"
        case .eureka: return "You could type something here!"
        case .cheer: return "Go, go, go!"
        case .sniff: return "*sniff sniff*… ACHOO!"
        case .poke: return "Poke! Missed on purpose."
        case .silk: return "Thwip! Hanging a thread on it."
        case .giggle: return has ? "Hee-hee, “\(name)” is a link!" : "Hee-hee, a link!"
        case .salute: return has ? "Aye aye, \(name)!" : "Aye aye!"
        case .blowKiss: return has ? "Mwah! Lovely \(name)." : "Mwah!"
        case .highFive: return "Up top, switch!"
        case .poof: return "Poof! Here I am."
        case .lamp: return has ? "A wisp of lamp smoke for \(name)." : "A wisp of lamp smoke!"
        case .wishes: return has ? "Three wishes for \(name)!" : "Three wishes for you!"
        case .smokeRings: return "Puff, puff, puff."
        case .shimmer: return has ? "Shiny \(name)!" : "Shiny!"
        case .stardust: return "A sprinkle of stardust."
        case .vortex: return "Round and round… flash!"
        case .crystal: return has ? "I see \(name) in your future." : "I see something wonderful."
        case .runes: return "Ancient spider magic."
        case .halo: return has ? "Angelic, \(name)." : "Angelic."
        case .fireflies: return "Off you go, little lights."
        case .rainbow: return has ? "A rainbow to \(name)!" : "A rainbow!"
        case .fireworks: return "Ooh! Aah!"
        case .bubbles: return "Pop, pop, pop!"
        case .glitter: return "Everything is better with glitter."
        case .jewelBeam: return has ? "Zap, \(name)!" : "Zap!"
        case .ripple: return "Ripples of light."
        case .moons: return "My little moons, off you go!"
        case .constellation: return has ? "The constellation of \(name)." : "Join the stars…"
        case .crescent: return "Moonrise."
        case .mirage: return has ? "Is \(name) really there?" : "Is it really there?"
        case .coins: return "Treasure!"
        case .flourish: return "Ta-da!"
        case .trace: return has ? "Round and round “\(name)”." : "Round and round."
        case .underline: return has ? "“\(name)”: important!" : "This bit is important!"
        case .boop: return has ? "Boop, boop, boop, \(name)!" : "Boop, boop, boop!"
        case .highlight: return "“\(name)”, highlighted."
        case .perch: return has ? "A nap on \(name)… Zzz." : "Zzz…"
        case .tabHop: return "Hop, hop, hop!"
        case .measure: return "Let me measure that."
        case .cobweb: return "A cosy corner for a web."
        case .lantern: return "“\(name)”, all lit up."
        }
    }
}

public struct GenieScreenChoice: Equatable, Sendable {
    public var act: GenieScreenAct
    public var mark: GenieLandmark
    public var line: String
    public var group: [GenieLandmark]

    public init(act: GenieScreenAct, mark: GenieLandmark, line: String, group: [GenieLandmark] = []) {
        self.act = act
        self.mark = mark
        self.line = line
        self.group = group
    }
}

public struct GenieScreenPlay: Sendable {
    public nonisolated static let gap: ClosedRange<Double> = 16...34
    public nonisolated static let firstGap: ClosedRange<Double> = 8...14
    public nonisolated static let approach: Double = 8
    public nonisolated static let shortest: Int = 18
    public nonisolated static let fewestTabs: Int = 3
    public nonisolated static let mostTabs: Int = 6
    public nonisolated static let rowSlack: CGFloat = 10

    public private(set) var bag: GenieBag<GenieScreenAct> = GenieBag(GenieScreenAct.allCases)
    public private(set) var lastMark: GenieLandmark?

    public init() {}

    public mutating func choose(
        _ marks: [GenieLandmark], from spider: CGPoint, travels: Bool = false, random: () -> Double
    ) -> GenieScreenChoice? {
        let usable: [GenieLandmark] = marks.filter { $0 != lastMark }
        guard !usable.isEmpty else { return nil }
        func fits(_ act: GenieScreenAct, _ mark: GenieLandmark) -> Bool {
            guard travels || !act.travels, GenieScreenPlay.fits(act, mark) else { return false }
            return act != .tabHop || GenieScreenPlay.row(mark, in: marks).count >= GenieScreenPlay.fewestTabs
        }
        guard let act = bag.next(random: random, fits: { (act: GenieScreenAct) -> Bool in
            usable.contains { fits(act, $0) }
        }) else { return nil }
        let fitting: [GenieLandmark] = usable.filter { fits(act, $0) }
        let scored: [(GenieLandmark, Double)] = fitting.map { (mark: GenieLandmark) -> (GenieLandmark, Double) in
            let apart: Double = Double(hypot(mark.rect.midX - spider.x, mark.rect.midY - spider.y))
            return (mark, apart * (0.5 + random()))
        }
        guard let mark = scored.min(by: { $0.1 < $1.1 })?.0 else { return nil }
        lastMark = mark
        let group: [GenieLandmark] = act == .tabHop ? GenieScreenPlay.row(mark, in: marks) : []
        return GenieScreenChoice(act: act, mark: mark, line: act.line(mark.text), group: group)
    }

    public static func fits(_ act: GenieScreenAct, _ mark: GenieLandmark) -> Bool {
        guard act.fits.contains(mark.kind), act.widths.contains(mark.rect.width) else { return false }
        return !act.needsText || !short(mark.text).isEmpty
    }

    public static func row(_ mark: GenieLandmark, in marks: [GenieLandmark]) -> [GenieLandmark] {
        guard mark.kind == .tab || mark.kind == .button else { return [] }
        let line: [GenieLandmark] = marks.filter { (other: GenieLandmark) -> Bool in
            other.kind == mark.kind && abs(other.rect.midY - mark.rect.midY) <= rowSlack
                && abs(other.rect.height - mark.rect.height) <= max(mark.rect.height * 0.5, rowSlack)
        }
        let sorted: [GenieLandmark] = line.sorted { $0.rect.midX < $1.rect.midX }
        var kept: [GenieLandmark] = []
        for item in sorted where kept.last.map({ item.rect.minX >= $0.rect.maxX - 2 }) ?? true {
            kept.append(item)
        }
        return Array(kept.prefix(mostTabs))
    }

    public static func short(_ text: String) -> String {
        let clean: String = GenieLandmarks.clean(text)
        guard clean.count > shortest else { return clean }
        return String(clean.prefix(shortest - 1)).trimmingCharacters(in: .whitespaces) + "…"
    }

    public static func perch(
        for mark: GenieLandmark, arena: GenieArena, side: CGFloat, blockers: [CGRect]
    ) -> (CGPoint, GenieEdge) {
        let center: CGPoint = mark.center
        let edge: GenieEdge = arena.nearest(center)
        let near: CGPoint = arena.on(edge, near: center)
        return GenieClearance.place(near, on: edge, arena: arena, side: side, buttons: blockers + [mark.rect])
    }

    public static func gaze(from spider: CGPoint, to rect: CGRect, progress: Double, sweep: Bool) -> CGPoint {
        let u: Double = min(max(progress, 0), 1)
        let target: CGPoint = sweep
            ? CGPoint(x: rect.minX + rect.width * CGFloat(0.5 - 0.5 * cos(Double.pi * min(u * 1.25, 1))), y: rect.midY)
            : CGPoint(x: rect.midX, y: rect.midY)
        let dx: CGFloat = target.x - spider.x
        let dy: CGFloat = target.y - spider.y
        let length: CGFloat = max(hypot(dx, dy), 90)
        return CGPoint(x: dx / length, y: dy / length)
    }

    public static func thread(from spider: CGPoint, to rect: CGRect, gap: CGFloat = 3) -> CGPoint {
        let inside: CGPoint = CGPoint(x: min(max(spider.x, rect.minX), rect.maxX), y: min(max(spider.y, rect.minY), rect.maxY))
        let dx: CGFloat = inside.x - spider.x
        let dy: CGFloat = inside.y - spider.y
        let length: CGFloat = hypot(dx, dy)
        guard length > gap else { return spider }
        return CGPoint(x: inside.x - dx / length * gap, y: inside.y - dy / length * gap)
    }
}
