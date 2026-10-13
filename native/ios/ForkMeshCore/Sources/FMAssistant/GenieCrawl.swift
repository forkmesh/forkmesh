import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieEdge: String, Equatable, Sendable, CaseIterable {
    case left, right, top, bottom

    public var degrees: Double {
        switch self {
        case .left: return 90
        case .right: return -90
        case .top: return 180
        case .bottom: return 0
        }
    }
}

public struct GenieWalk: Equatable, Sendable {
    public var from: CGPoint
    public var to: CGPoint
    public var start: Date
    public var duration: Double

    public init(from: CGPoint, to: CGPoint, start: Date, duration: Double) {
        self.from = from
        self.to = to
        self.start = start
        self.duration = max(duration, 0.01)
    }

    public var length: Double {
        Double(hypot(to.x - from.x, to.y - from.y))
    }

    public var end: Date { start.addingTimeInterval(duration) }

    public func progress(at now: Date) -> Double {
        min(max(now.timeIntervalSince(start) / duration, 0), 1)
    }

    public static func ease(_ u: Double) -> Double {
        u * u * (3 - 2 * u)
    }

    public func point(at now: Date) -> CGPoint {
        let k: CGFloat = CGFloat(GenieWalk.ease(progress(at: now)))
        return CGPoint(x: from.x + (to.x - from.x) * k, y: from.y + (to.y - from.y) * k)
    }

    public func travelled(at now: Date) -> Double {
        length * GenieWalk.ease(progress(at: now))
    }

    public func walking(at now: Date) -> Double {
        let u: Double = progress(at: now)
        guard u > 0, u < 1 else { return 0 }
        return min(u / 0.12, (1 - u) / 0.12, 1)
    }

    public var facing: Double {
        let dx: Double = Double(to.x - from.x)
        return abs(dx) < 4 ? 0 : (dx > 0 ? 1 : -1)
    }
}

public enum GenieCrawl {
    public nonisolated static let speed: Double = 80
    public nonisolated static let strideLength: Double = 11
    public nonisolated static let tuck: CGFloat = 0.14
    public nonisolated static let shortHop: CGFloat = 170

    public static func speed(vitality: Double) -> Double {
        speed * (0.5 + 0.75 * min(max(vitality, 0), 1))
    }

    public static func walk(from: CGPoint, to: CGPoint, start: Date, vitality: Double) -> GenieWalk {
        let distance: Double = Double(hypot(to.x - from.x, to.y - from.y))
        return GenieWalk(from: from, to: to, start: start, duration: max(distance / speed(vitality: vitality), 0.4))
    }

    public static func phase(travelled: Double) -> Double {
        travelled / strideLength * Double.pi
    }

    public static func clamp(_ point: CGPoint, to rect: CGRect) -> CGPoint {
        guard rect.width >= 0, rect.height >= 0 else { return point }
        return CGPoint(x: min(max(point.x, rect.minX), rect.maxX), y: min(max(point.y, rect.minY), rect.maxY))
    }

    public static func edge(near point: CGPoint, in full: CGRect, margin: CGFloat) -> GenieEdge? {
        let gaps: [(GenieEdge, CGFloat)] = [
            (.left, point.x - full.minX), (.right, full.maxX - point.x), (.top, point.y - full.minY), (.bottom, full.maxY - point.y),
        ]
        guard let nearest = gaps.min(by: { $0.1 < $1.1 }), nearest.1 <= margin else { return nil }
        return nearest.0
    }

    public static func parked(_ edge: GenieEdge, near point: CGPoint, full: CGRect, safe: CGRect, side: CGFloat) -> CGPoint {
        let inset: CGFloat = side * tuck
        let along: CGRect = safe.insetBy(dx: min(side / 2, safe.width / 2), dy: min(side / 2, safe.height / 2))
        switch edge {
        case .left: return CGPoint(x: full.minX + inset, y: min(max(point.y, along.minY), along.maxY))
        case .right: return CGPoint(x: full.maxX - inset, y: min(max(point.y, along.minY), along.maxY))
        case .top: return CGPoint(x: min(max(point.x, along.minX), along.maxX), y: full.minY + inset)
        case .bottom: return CGPoint(x: min(max(point.x, along.minX), along.maxX), y: full.maxY - inset)
        }
    }

    public static func target(from point: CGPoint, roam: CGRect, lean: CGSize, random: () -> Double) -> CGPoint {
        guard roam.width > 0, roam.height > 0 else { return point }
        let near: Bool = random() < 0.6
        var x: CGFloat
        var y: CGFloat
        if near {
            let angle: Double = random() * Double.pi * 2
            let reach: CGFloat = shortHop * CGFloat(0.4 + 0.6 * random())
            x = point.x + CGFloat(cos(angle)) * reach
            y = point.y + CGFloat(sin(angle)) * reach
        } else {
            x = roam.minX + roam.width * CGFloat(random())
            y = roam.minY + roam.height * CGFloat(random())
        }
        x += CGFloat(min(max(lean.width, -1), 1)) * roam.width * 0.35
        y += CGFloat(min(max(lean.height, -1), 1)) * roam.height * 0.2
        return clamp(CGPoint(x: x, y: y), to: roam)
    }

    public static func fraction(_ point: CGPoint, in rect: CGRect) -> CGPoint {
        guard rect.width > 0, rect.height > 0 else { return CGPoint(x: 0.85, y: 0.7) }
        return CGPoint(x: (point.x - rect.minX) / rect.width, y: (point.y - rect.minY) / rect.height)
    }

    public static func point(_ fraction: CGPoint, in rect: CGRect) -> CGPoint {
        CGPoint(x: rect.minX + rect.width * fraction.x, y: rect.minY + rect.height * fraction.y)
    }
}

extension GenieCrawl {
    public static func tour(_ travel: GenieTravel, from start: CGPoint, roam: CGRect, toward hand: CGPoint? = nil) -> [CGPoint] {
        guard roam.width > 0, roam.height > 0 else { return [] }
        func at(_ dx: CGFloat, _ dy: CGFloat) -> CGPoint { clamp(CGPoint(x: start.x + dx, y: start.y + dy), to: roam) }
        let reach: CGFloat = min(70, roam.width / 3, roam.height / 3)
        switch travel {
        case .none:
            return []
        case .toHand:
            guard let hand else { return [] }
            return [clamp(hand, to: roam)]
        case .circle:
            return (1...9).map { k in
                let angle = Double(k) / 8 * Double.pi * 2
                return at(CGFloat(sin(angle)) * reach, CGFloat(1 - cos(angle)) * -reach * 0.8)
            }
        case .zigzag:
            return [at(-reach, -reach * 0.5), at(reach, -reach), at(-reach, -reach * 1.5), start].map { clamp($0, to: roam) }
        case .hopAround:
            return [at(reach * 0.7, 0), at(reach * 0.7, -reach * 0.7), at(0, -reach * 0.7), start].map { clamp($0, to: roam) }
        case .flee:
            let from = hand ?? CGPoint(x: roam.midX, y: roam.midY)
            let dx = start.x - from.x
            let dy = start.y - from.y
            let length = hypot(dx, dy)
            let unit = length < 1 ? CGPoint(x: 1, y: 0) : CGPoint(x: dx / length, y: dy / length)
            return [clamp(CGPoint(x: start.x + unit.x * 170, y: start.y + unit.y * 170), to: roam)]
        case .hide:
            let gaps: [(CGPoint, CGFloat)] = [
                (CGPoint(x: roam.minX, y: start.y), start.x - roam.minX), (CGPoint(x: roam.maxX, y: start.y), roam.maxX - start.x),
                (CGPoint(x: start.x, y: roam.minY), start.y - roam.minY), (CGPoint(x: start.x, y: roam.maxY), roam.maxY - start.y),
            ]
            return [gaps.min { $0.1 < $1.1 }?.0 ?? start, start]
        case .rope:
            let drop = min(roam.maxY - start.y, 220)
            let down = drop > 60 ? drop : -min(start.y - roam.minY, 220)
            return [at(0, down), start]
        case .zoom:
            let far: CGFloat = start.x > roam.midX ? roam.minX : roam.maxX
            return [clamp(CGPoint(x: far, y: start.y), to: roam), start]
        case .figureEight:
            return (1...12).map { k in
                let angle = Double(k) / 12 * Double.pi * 2
                return at(CGFloat(sin(angle)) * reach, CGFloat(sin(angle * 2)) * reach * 0.45)
            }
        case .backAndForth:
            return [at(-reach * 0.8, 0), at(reach * 0.8, 0), start].map { clamp($0, to: roam) }
        }
    }

    public static func landing(from start: CGPoint, velocity: CGSize, in roam: CGRect, glide: Double = 0.35) -> CGPoint {
        guard roam.width > 0, roam.height > 0 else { return start }
        func bounce(_ value: CGFloat, _ low: CGFloat, _ high: CGFloat) -> CGFloat {
            let span = high - low
            guard span > 0 else { return low }
            var offset = (value - low).truncatingRemainder(dividingBy: span * 2)
            if offset < 0 { offset += span * 2 }
            return low + (offset <= span ? offset : span * 2 - offset)
        }
        let x = start.x + CGFloat(velocity.width * glide)
        let y = start.y + CGFloat(velocity.height * glide)
        return CGPoint(x: bounce(x, roam.minX, roam.maxX), y: bounce(y, roam.minY, roam.maxY))
    }
}
