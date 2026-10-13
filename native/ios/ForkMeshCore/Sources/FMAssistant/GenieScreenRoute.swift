import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieScreenMark: String, Equatable, Sendable {
    case none, outline, underline, highlight, rings, tape, cobweb, lantern
}

public struct GenieScreenRoute: Equatable, Sendable {
    public var approach: CGPoint
    public var path: [CGPoint]
    public var aim: Double?
    public var rest: Double?
    public var mark: GenieScreenMark
    public var rects: [CGRect]
    public var beats: [Double]
    public var corner: CGPoint?
    public var label: String

    public init(
        approach: CGPoint, path: [CGPoint], aim: Double? = nil, rest: Double? = nil, mark: GenieScreenMark, rects: [CGRect],
        beats: [Double] = [], corner: CGPoint? = nil, label: String = ""
    ) {
        self.approach = approach
        self.path = path
        self.aim = aim
        self.rest = rest
        self.mark = mark
        self.rects = rects
        self.beats = beats
        self.corner = corner
        self.label = label
    }

    public var end: CGPoint { path.last ?? approach }
}

public struct GenieScreenEffect: Equatable, Sendable {
    public nonisolated static let fade: Double = 0.8
    public nonisolated static let ring: Double = 0.16

    public var route: GenieScreenRoute
    public var start: Date
    public var duration: Double
    public var hold: Double

    public init(route: GenieScreenRoute, start: Date, duration: Double, hold: Double) {
        self.route = route
        self.start = start
        self.duration = max(duration, 0.01)
        self.hold = max(hold, 0)
    }

    public var end: Date { start.addingTimeInterval(duration + hold + GenieScreenEffect.fade) }

    public func progress(at now: Date) -> Double {
        GenieWalk.ease(min(max(now.timeIntervalSince(start) / duration, 0), 1))
    }

    public func strength(at now: Date) -> Double {
        let elapsed: Double = now.timeIntervalSince(start)
        guard elapsed >= 0 else { return 0 }
        let gone: Double = elapsed - duration - hold
        guard gone > 0 else { return 1 }
        return max(1 - gone / GenieScreenEffect.fade, 0)
    }

    public func rings(at now: Date) -> [(rect: CGRect, age: Double)] {
        let u: Double = progress(at: now)
        return zip(route.rects, route.beats).compactMap { (pair: (CGRect, Double)) -> (rect: CGRect, age: Double)? in
            let age: Double = (u - pair.1) / GenieScreenEffect.ring
            guard age >= 0, age <= 1 else { return nil }
            return (pair.0, age)
        }
    }

    public func reach(at now: Date) -> CGFloat {
        guard let rect = route.rects.first else { return 0 }
        return rect.width * CGFloat(progress(at: now))
    }
}

extension GenieScreenPlay {
    public nonisolated static let bump: CGFloat = 12
    public nonisolated static let bumps: Int = 3

    public static func route(for choice: GenieScreenChoice, from spider: CGPoint, arena: GenieArena, side: CGFloat) -> GenieScreenRoute? {
        let rect: CGRect = choice.mark.rect
        let stand: CGFloat = side * GenieGravity.stand
        let box: CGRect = arena.box
        func inside(_ point: CGPoint) -> CGPoint { GenieCrawl.clamp(point, to: box) }
        func line(_ a: CGPoint, _ b: CGPoint) -> [CGPoint] { GenieGlass.even([inside(a), inside(b)]) }
        func usable(_ path: [CGPoint], least: Double) -> [CGPoint]? { GenieGlass.length(path) >= least ? path : nil }
        let label: String = "\(Int(rect.width.rounded())) pt"
        switch choice.act {
        case .trace:
            let ring: [CGPoint] = loop(around: rect.insetBy(dx: -stand * 0.7, dy: -stand * 0.7), radius: stand * 0.6, from: spider)
                .map(inside)
            guard let path = usable(ring, least: 60), let first = path.first else { return nil }
            return GenieScreenRoute(
                approach: first, path: path, mark: .outline, rects: [rect.insetBy(dx: -3, dy: -3)], label: label)
        case .underline:
            let y: CGFloat = rect.maxY + stand
            guard let path = usable(line(CGPoint(x: rect.minX, y: y), CGPoint(x: rect.maxX, y: y)), least: 30), let first = path.first
            else { return nil }
            return GenieScreenRoute(approach: first, path: path, rest: 0, mark: .underline, rects: [rect])
        case .highlight, .lantern, .measure:
            let y: CGFloat = rect.minY - stand
            guard let path = usable(line(CGPoint(x: rect.minX, y: y), CGPoint(x: rect.maxX, y: y)), least: 30), let first = path.first
            else { return nil }
            let mark: GenieScreenMark = choice.act == .highlight ? .highlight : (choice.act == .lantern ? .lantern : .tape)
            return GenieScreenRoute(approach: first, path: path, rest: 0, mark: mark, rects: [rect], label: label)
        case .boop:
            let fromLeft: Bool = spider.x <= rect.midX
            let reach: CGFloat = side * 0.45
            let start: CGPoint = inside(CGPoint(x: fromLeft ? rect.minX - reach : rect.maxX + reach, y: rect.midY))
            let toward: CGFloat = fromLeft ? GenieScreenPlay.bump : -GenieScreenPlay.bump
            var points: [CGPoint] = [start]
            for _ in 0..<GenieScreenPlay.bumps {
                points.append(CGPoint(x: start.x + toward, y: start.y))
                points.append(start)
            }
            let beats: [Double] = (0..<GenieScreenPlay.bumps).map { Double(2 * $0 + 1) / Double(2 * GenieScreenPlay.bumps) }
            return GenieScreenRoute(
                approach: start, path: GenieGlass.even(points.map(inside), spacing: 4), aim: fromLeft ? 90 : -90,
                mark: .rings, rects: Array(repeating: rect.insetBy(dx: -4, dy: -4), count: GenieScreenPlay.bumps), beats: beats)
        case .perch:
            let y: CGFloat = rect.minY - stand
            let from: CGPoint = CGPoint(x: spider.x <= rect.midX ? rect.minX : rect.maxX, y: y)
            let path: [CGPoint] = line(from, CGPoint(x: rect.midX, y: y))
            guard let first = path.first else { return nil }
            return GenieScreenRoute(approach: first, path: path, rest: 0, mark: .none, rects: [rect])
        case .tabHop:
            let tabs: [CGRect] = (choice.group.isEmpty ? [choice.mark] : choice.group).map(\.rect)
            guard tabs.count >= GenieScreenPlay.fewestTabs else { return nil }
            let leftFirst: Bool = abs(spider.x - (tabs.first?.midX ?? 0)) <= abs(spider.x - (tabs.last?.midX ?? 0))
            let order: [CGRect] = leftFirst ? tabs : tabs.reversed()
            let tops: [CGPoint] = order.map { inside(CGPoint(x: $0.midX, y: $0.minY - stand)) }
            guard let first = tops.first else { return nil }
            var points: [CGPoint] = [first]
            for index in 1..<tops.count {
                points += arc(from: tops[index - 1], to: tops[index], height: side * 0.5).dropFirst().map(inside)
            }
            let path: [CGPoint] = GenieGlass.even(points, spacing: 6)
            let total: Double = GenieGlass.length(path)
            guard total > 20 else { return nil }
            var beats: [Double] = [0.02]
            var walked: Double = 0
            for index in 1..<tops.count {
                walked += GenieGlass.length(arc(from: tops[index - 1], to: tops[index], height: side * 0.5))
                beats.append(min(walked / max(GenieGlass.length(points), 1), 0.98))
            }
            return GenieScreenRoute(approach: first, path: path, aim: 0, rest: 0, mark: .rings, rects: order, beats: beats)
        case .cobweb:
            let right: Bool = spider.x > rect.midX
            let corner: CGPoint = CGPoint(x: right ? rect.maxX : rect.minX, y: rect.minY)
            let out: CGFloat = right ? 1 : -1
            let start: CGPoint = inside(CGPoint(x: corner.x + out * stand, y: corner.y - stand))
            let points: [CGPoint] = [
                start, CGPoint(x: corner.x + out * stand * 0.4, y: corner.y - stand * 0.4), start,
                CGPoint(x: corner.x + out * stand * 0.6, y: corner.y - stand * 0.2), start,
            ].map(inside)
            return GenieScreenRoute(
                approach: start, path: GenieGlass.even(points, spacing: 4), aim: right ? -135 : 135, mark: .cobweb, rects: [rect],
                corner: corner)
        default:
            return nil
        }
    }

    public static func beside(for mark: GenieLandmark, arena: GenieArena, side: CGFloat, blockers: [CGRect]) -> CGPoint {
        let rect: CGRect = mark.rect
        let gap: CGFloat = side * (GenieClearance.body + 0.1)
        let box: CGRect = arena.box
        let below: CGPoint = CGPoint(x: rect.midX, y: rect.maxY + gap)
        let above: CGPoint = CGPoint(x: rect.midX, y: rect.minY - gap)
        let start: CGPoint = GenieCrawl.clamp(below.y <= box.maxY ? below : above, to: box)
        return GenieGlass.place(start, arena: arena, side: side, buttons: blockers + [rect])
    }

    public static func loop(around rect: CGRect, radius: CGFloat, from spider: CGPoint) -> [CGPoint] {
        let r: CGFloat = max(min(radius, rect.width / 2, rect.height / 2), 0)
        var raw: [CGPoint] = []
        let corners: [(CGPoint, Double)] = [
            (CGPoint(x: rect.maxX - r, y: rect.minY + r), -90), (CGPoint(x: rect.maxX - r, y: rect.maxY - r), 0),
            (CGPoint(x: rect.minX + r, y: rect.maxY - r), 90), (CGPoint(x: rect.minX + r, y: rect.minY + r), 180),
        ]
        for (center, from) in corners {
            for step in 0...4 {
                let angle: Double = (from + Double(step) * 22.5) * Double.pi / 180
                raw.append(CGPoint(x: center.x + r * CGFloat(cos(angle)), y: center.y + r * CGFloat(sin(angle))))
            }
        }
        guard let first = raw.first else { return [] }
        raw.append(first)
        let ring: [CGPoint] = GenieGlass.even(raw)
        guard ring.count > 2 else { return ring }
        let open: [CGPoint] = Array(ring.dropLast())
        let nearest: Int = open.indices.min {
            hypot(open[$0].x - spider.x, open[$0].y - spider.y) < hypot(open[$1].x - spider.x, open[$1].y - spider.y)
        } ?? 0
        let turned: [CGPoint] = Array(open[nearest...] + open[..<nearest])
        return turned + [turned[0]]
    }

    public static func arc(from a: CGPoint, to b: CGPoint, height: CGFloat) -> [CGPoint] {
        let count: Int = 12
        return (0...count).map { (index: Int) -> CGPoint in
            let t: CGFloat = CGFloat(index) / CGFloat(count)
            return CGPoint(x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t - height * 4 * t * (1 - t))
        }
    }
}
