import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieGlass {
    public nonisolated static let enter: Double = 0.3
    public nonisolated static let leave: Double = 0.45
    public nonisolated static let faceUp: Double = -0.75
    public nonisolated static let settleFor: Double = 0.5
    public nonisolated static let pivot: Double = 0.35
    public nonisolated static let pace: Double = 1.15
    public nonisolated static let bend: Double = 0.3
    public nonisolated static let spacing: CGFloat = 10
    public nonisolated static let grid: CGFloat = 14
    public nonisolated static let near: ClosedRange<Double> = 0.18...0.55
    public nonisolated static let stepOff: CGFloat = 0.9
    public nonisolated static let tries: Int = 8
    public nonisolated static let standFor: Double = 0.3

    public static func flat(plane: CGSize, z: Double, was: Bool) -> Bool {
        let lean: Double = Double(hypot(plane.width, plane.height))
        guard z < faceUp || (was && z < 0) else { return false }
        return lean < (was ? leave : enter)
    }

    public static func heading(dx: CGFloat, dy: CGFloat) -> Double {
        atan2(Double(dx), Double(-dy)) * 180 / Double.pi
    }

    public static func length(_ path: [CGPoint]) -> Double {
        guard path.count > 1 else { return 0 }
        var total: Double = 0
        for index in 1..<path.count {
            total += Double(hypot(path[index].x - path[index - 1].x, path[index].y - path[index - 1].y))
        }
        return total
    }

    public static func even(_ points: [CGPoint], spacing: CGFloat = GenieGlass.spacing) -> [CGPoint] {
        guard points.count > 1, let first = points.first else { return points }
        let total: Double = length(points)
        let count: Int = max(Int((total / Double(spacing)).rounded(.up)), 1)
        var out: [CGPoint] = [first]
        var leg: Int = 1
        var legStart: Double = 0
        for step in 1...count {
            let want: Double = total * Double(step) / Double(count)
            while leg < points.count - 1 {
                let span: Double = Double(hypot(points[leg].x - points[leg - 1].x, points[leg].y - points[leg - 1].y))
                if legStart + span >= want { break }
                legStart += span
                leg += 1
            }
            let a: CGPoint = points[leg - 1]
            let b: CGPoint = points[leg]
            let span: Double = max(Double(hypot(b.x - a.x, b.y - a.y)), 1e-9)
            let f: CGFloat = CGFloat(min(max((want - legStart) / span, 0), 1))
            out.append(CGPoint(x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f))
        }
        return out
    }

    public static func curve(from a: CGPoint, to b: CGPoint, bend: Double) -> [CGPoint] {
        let dx: CGFloat = b.x - a.x
        let dy: CGFloat = b.y - a.y
        let span: CGFloat = hypot(dx, dy)
        guard span > 1 else { return [a, b] }
        let mid: CGPoint = CGPoint(x: (a.x + b.x) / 2 - dy * CGFloat(bend), y: (a.y + b.y) / 2 + dx * CGFloat(bend))
        let count: Int = max(Int((span / spacing).rounded(.up)), 2)
        let raw: [CGPoint] = (0...count).map { (index: Int) -> CGPoint in
            let t: CGFloat = CGFloat(index) / CGFloat(count)
            let u: CGFloat = 1 - t
            return CGPoint(x: u * u * a.x + 2 * u * t * mid.x + t * t * b.x, y: u * u * a.y + 2 * u * t * mid.y + t * t * b.y)
        }
        return even(raw)
    }

    public static func roam(
        _ path: [CGPoint], surface: GenieEdge? = nil, speed: Double, at time: Date, turn: Double, before: Double = 0, aim: Double? = nil,
        least: Double = 0.3
    ) -> GenieMove {
        let walk: Double = length(path)
        let from: CGPoint = path.first ?? .zero
        let to: CGPoint = path.last ?? from
        let duration: Double = max(walk / max(speed, 1), least)
        let move: GenieMove = GenieMove(
            kind: .roam, from: from, to: to, surface: surface, start: time, duration: duration, turnFrom: turn, turnTo: turn,
            path: path.count > 1 ? path : [from, to], before: before, aim: aim)
        var done: GenieMove = move
        done.turnTo = move.rotation(at: move.end)
        return done
    }

    public static func stand(at point: CGPoint, turn: Double, to facing: Double? = nil, at time: Date, seconds: Double = standFor)
        -> GenieMove
    {
        GenieMove(
            kind: .roam, from: point, to: point, surface: nil, start: time, duration: seconds, turnFrom: turn, turnTo: facing ?? turn,
            path: [point, point], aim: facing ?? turn)
    }

    public static func walk(
        to target: CGPoint, from start: CGPoint, arena: GenieArena, speed: Double, at time: Date, turn: Double, bend: Double
    ) -> GenieTrack {
        let goal: CGPoint = GenieCrawl.clamp(target, to: arena.box)
        let path: [CGPoint] = curve(from: start, to: goal, bend: bend).map { GenieCrawl.clamp($0, to: arena.box) }
        guard length(path) > 1 else { return GenieTrack([stand(at: start, turn: turn, at: time)]) }
        let move: GenieMove = roam(path, speed: speed * pace, at: time, turn: turn)
        return GenieTrack([move])
    }

    public static func through(
        _ legs: [[CGPoint]], arena: GenieArena, speed: Double, at time: Date, turn: Double, aims: [Double?] = []
    ) -> GenieTrack {
        var moves: [GenieMove] = []
        var clock: Date = time
        var facing: Double = turn
        var walked: Double = 0
        for (index, leg) in legs.enumerated() where !leg.isEmpty {
            let path: [CGPoint] = leg.map { GenieCrawl.clamp($0, to: arena.box) }
            let aim: Double? = index < aims.count ? aims[index] : nil
            let move: GenieMove = roam(path, speed: speed * pace, at: clock, turn: facing, before: walked, aim: aim)
            moves.append(move)
            walked += length(path)
            clock = move.end
            facing = move.turnTo
        }
        return GenieTrack(moves)
    }

    public static func glide(from start: CGPoint, velocity: CGSize, arena: GenieArena, at time: Date, turn: Double) -> GenieTrack {
        let p: CGPoint = GenieCrawl.clamp(start, to: arena.box)
        let trail: [CGPoint] = GenieGravity.skid(from: p, velocity: velocity, arena: arena)
        guard trail.count > 2, let last = trail.last else { return GenieTrack([stand(at: p, turn: turn, at: time)]) }
        let slide: GenieMove = GenieMove(
            kind: .slide, from: p, to: last, surface: nil, start: time, duration: Double(trail.count - 1) * GenieGravity.step,
            turnFrom: turn, turnTo: turn, path: trail)
        return GenieTrack([slide, stand(at: last, turn: turn, at: slide.end)])
    }

    public static func wander(from point: CGPoint, arena: GenieArena, random: () -> Double) -> CGPoint {
        let box: CGRect = arena.box
        let diagonal: Double = Double(hypot(box.width, box.height))
        let least: Double = diagonal * near.lowerBound
        let most: Double = diagonal * near.upperBound
        var best: CGPoint = point
        var bestGap: Double = -1
        for _ in 0..<tries {
            let spot: CGPoint = CGPoint(x: box.minX + box.width * CGFloat(random()), y: box.minY + box.height * CGFloat(random()))
            let gap: Double = Double(hypot(spot.x - point.x, spot.y - point.y))
            if gap >= least && gap <= most { return spot }
            if gap <= most && gap > bestGap {
                best = spot
                bestGap = gap
            }
        }
        return best
    }

    public static func inward(from point: CGPoint, arena: GenieArena, side: CGFloat) -> CGPoint {
        let box: CGRect = arena.box
        let step: CGFloat = side * stepOff
        let edge: GenieEdge = arena.nearest(point)
        let n: CGSize = GenieArena.normal(edge)
        return GenieCrawl.clamp(CGPoint(x: point.x - n.width * step, y: point.y - n.height * step), to: box)
    }

    public static func spots(_ arena: GenieArena) -> [CGPoint] {
        let box: CGRect = arena.box
        let across: Int = max(Int((box.width / grid).rounded(.up)), 1)
        let down: Int = max(Int((box.height / grid).rounded(.up)), 1)
        var out: [CGPoint] = []
        for row in 0...down {
            for column in 0...across {
                out.append(CGPoint(
                    x: box.minX + box.width * CGFloat(column) / CGFloat(across), y: box.minY + box.height * CGFloat(row) / CGFloat(down)))
            }
        }
        return out
    }

    public static func place(_ point: CGPoint, arena: GenieArena, side: CGFloat, buttons: [CGRect]) -> CGPoint {
        let here: Double = GenieClearance.covered(point, side: side, buttons: buttons)
        guard here > GenieClearance.touching else { return point }
        let options: [(CGPoint, GenieEdge)] = spots(arena).map { ($0, GenieEdge.bottom) }
        let found: (CGPoint, GenieEdge)? = GenieClearance.pick(options, here: here, side: side, buttons: buttons) {
            (spot: CGPoint, _: GenieEdge) -> Double in
            Double(hypot(spot.x - point.x, spot.y - point.y))
        }
        return found?.0 ?? point
    }
}
