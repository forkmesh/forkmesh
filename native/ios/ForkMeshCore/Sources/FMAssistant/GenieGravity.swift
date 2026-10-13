import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public struct GenieArena: Equatable, Sendable {
    public var box: CGRect

    public init(box: CGRect) {
        self.box = box
    }

    public init(full: CGRect, ceiling: CGFloat, floor: CGFloat, side: CGFloat) {
        let stand: CGFloat = side * GenieGravity.stand
        let top: CGFloat = max(ceiling, full.minY) + stand
        let bottom: CGFloat = min(floor, full.maxY) - stand
        let left: CGFloat = full.minX + stand
        let right: CGFloat = full.maxX - stand
        let midX: CGFloat = (left + right) / 2
        let midY: CGFloat = (top + bottom) / 2
        let width: CGFloat = max(right - left, 1)
        let height: CGFloat = max(bottom - top, 1)
        box = CGRect(
            x: right > left ? left : midX - 0.5, y: bottom > top ? top : midY - 0.5, width: width, height: height)
    }

    public var perimeter: Double { Double(box.width + box.height) * 2 }

    public static func normal(_ surface: GenieEdge) -> CGSize {
        switch surface {
        case .left: return CGSize(width: -1, height: 0)
        case .right: return CGSize(width: 1, height: 0)
        case .top: return CGSize(width: 0, height: -1)
        case .bottom: return CGSize(width: 0, height: 1)
        }
    }

    public func on(_ surface: GenieEdge, near point: CGPoint) -> CGPoint {
        let x: CGFloat = min(max(point.x, box.minX), box.maxX)
        let y: CGFloat = min(max(point.y, box.minY), box.maxY)
        switch surface {
        case .left: return CGPoint(x: box.minX, y: y)
        case .right: return CGPoint(x: box.maxX, y: y)
        case .top: return CGPoint(x: x, y: box.minY)
        case .bottom: return CGPoint(x: x, y: box.maxY)
        }
    }

    public func nearest(_ point: CGPoint) -> GenieEdge {
        let gaps: [(GenieEdge, CGFloat)] = [
            (.bottom, abs(box.maxY - point.y)), (.left, abs(point.x - box.minX)), (.right, abs(box.maxX - point.x)),
            (.top, abs(point.y - box.minY)),
        ]
        return gaps.min { $0.1 < $1.1 }?.0 ?? .bottom
    }

    public func touches(_ point: CGPoint, _ surface: GenieEdge, within slack: CGFloat = 1.5) -> Bool {
        let snapped: CGPoint = on(surface, near: point)
        return abs(snapped.x - point.x) <= slack && abs(snapped.y - point.y) <= slack
    }

    public func distance(_ point: CGPoint, on surface: GenieEdge) -> Double {
        let p: CGPoint = on(surface, near: point)
        let w: Double = Double(box.width)
        let h: Double = Double(box.height)
        switch surface {
        case .top: return Double(p.x - box.minX)
        case .right: return w + Double(p.y - box.minY)
        case .bottom: return w + h + Double(box.maxX - p.x)
        case .left: return w * 2 + h + Double(box.maxY - p.y)
        }
    }

    public func corners(clockwise: Bool, from surface: GenieEdge, to target: GenieEdge) -> [(CGPoint, GenieEdge)] {
        let ring: [GenieEdge] = [.top, .right, .bottom, .left]
        let ends: [GenieEdge: (CGPoint, CGPoint)] = [
            .top: (CGPoint(x: box.minX, y: box.minY), CGPoint(x: box.maxX, y: box.minY)),
            .right: (CGPoint(x: box.maxX, y: box.minY), CGPoint(x: box.maxX, y: box.maxY)),
            .bottom: (CGPoint(x: box.maxX, y: box.maxY), CGPoint(x: box.minX, y: box.maxY)),
            .left: (CGPoint(x: box.minX, y: box.maxY), CGPoint(x: box.minX, y: box.minY)),
        ]
        guard var index = ring.firstIndex(of: surface) else { return [] }
        var out: [(CGPoint, GenieEdge)] = []
        var current: GenieEdge = surface
        for _ in 0..<4 where current != target {
            let corner: CGPoint = clockwise ? (ends[current]?.1 ?? .zero) : (ends[current]?.0 ?? .zero)
            index = (index + (clockwise ? 1 : 3)) % 4
            out.append((corner, current))
            current = ring[index]
        }
        return out
    }
}

public struct GenieGait: Equatable, Sendable {
    public var phase: Double = 0
    public var walking: Double = 0
    public var facing: Double = 0
    public var air: Double = 0
    public var land: Double = 0

    public init(phase: Double = 0, walking: Double = 0, facing: Double = 0, air: Double = 0, land: Double = 0) {
        self.phase = phase
        self.walking = walking
        self.facing = facing
        self.air = air
        self.land = land
    }

    public static let still: GenieGait = GenieGait()
}

public struct GenieMove: Equatable, Sendable {
    public enum Kind: String, Equatable, Sendable {
        case crawl, fly, hop, land, silk, slide, roam
    }

    public var kind: Kind
    public var from: CGPoint
    public var to: CGPoint
    public var surface: GenieEdge?
    public var start: Date
    public var duration: Double
    public var turnFrom: Double
    public var turnTo: Double
    public var path: [CGPoint]
    public var before: Double
    public var anchor: CGPoint?
    public var aim: Double?

    public init(
        kind: Kind, from: CGPoint, to: CGPoint, surface: GenieEdge?, start: Date, duration: Double, turnFrom: Double, turnTo: Double,
        path: [CGPoint] = [], before: Double = 0, anchor: CGPoint? = nil, aim: Double? = nil
    ) {
        self.kind = kind
        self.from = from
        self.to = to
        self.surface = surface
        self.start = start
        self.duration = max(duration, 0.01)
        self.turnFrom = turnFrom
        self.turnTo = turnTo
        self.path = path
        self.before = before
        self.anchor = anchor
        self.aim = aim
    }

    public var end: Date { start.addingTimeInterval(duration) }

    public var length: Double { Double(hypot(to.x - from.x, to.y - from.y)) }

    public func progress(at now: Date) -> Double {
        min(max(now.timeIntervalSince(start) / duration, 0), 1)
    }

    public func point(at now: Date) -> CGPoint {
        let u: Double = progress(at: now)
        switch kind {
        case .fly, .slide:
            guard path.count > 1 else { return to }
            let spot: Double = u * Double(path.count - 1)
            let index: Int = min(Int(spot), path.count - 2)
            let k: CGFloat = CGFloat(spot - Double(index))
            let a: CGPoint = path[index]
            let b: CGPoint = path[index + 1]
            return CGPoint(x: a.x + (b.x - a.x) * k, y: a.y + (b.y - a.y) * k)
        case .roam:
            return along(GenieWalk.ease(u)).point
        case .land:
            return to
        case .crawl, .hop:
            let k: CGFloat = CGFloat(GenieWalk.ease(u))
            return CGPoint(x: from.x + (to.x - from.x) * k, y: from.y + (to.y - from.y) * k)
        case .silk:
            let k: CGFloat = CGFloat(GenieWalk.ease(u))
            let p: CGPoint = CGPoint(x: from.x + (to.x - from.x) * k, y: from.y + (to.y - from.y) * k)
            guard let anchor = anchor else { return p }
            let angle: CGFloat = CGFloat(swing(at: now) * Double.pi / 180)
            let dx: CGFloat = p.x - anchor.x
            let dy: CGFloat = p.y - anchor.y
            return CGPoint(x: anchor.x + dx * cos(angle) - dy * sin(angle), y: anchor.y + dx * sin(angle) + dy * cos(angle))
        }
    }

    public var hanging: Bool { kind == .silk && from == to }

    public func along(_ k: Double) -> (point: CGPoint, heading: Double?) {
        guard path.count > 1 else { return (to, nil) }
        let spot: Double = min(max(k, 0), 1) * Double(path.count - 1)
        let index: Int = min(Int(spot), path.count - 2)
        let f: CGFloat = CGFloat(spot - Double(index))
        let a: CGPoint = path[index]
        let b: CGPoint = path[index + 1]
        let point: CGPoint = CGPoint(x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f)
        let back: CGPoint = path[max(index - 1, 0)]
        let ahead: CGPoint = path[min(index + 2, path.count - 1)]
        let dx: CGFloat = (b.x - a.x) * 2 + (ahead.x - back.x) * 0.5
        let dy: CGFloat = (b.y - a.y) * 2 + (ahead.y - back.y) * 0.5
        guard hypot(dx, dy) > 0.01 else { return (point, nil) }
        return (point, GenieGlass.heading(dx: dx, dy: dy))
    }

    public func swing(at now: Date) -> Double {
        guard hanging else { return 0 }
        let elapsed: Double = now.timeIntervalSince(start)
        return sin(elapsed * 2 * Double.pi / GenieGravity.swingPeriod) * GenieGravity.swingDegrees * sin(Double.pi * progress(at: now))
    }

    public func rotation(at now: Date) -> Double {
        let u: Double = progress(at: now)
        let delta: Double = GenieGravity.turn(from: turnFrom, to: turnTo)
        switch kind {
        case .fly:
            let tumble: Double = sin(u * Double.pi * 3) * 14 * (1 - u)
            return turnFrom + delta * GenieWalk.ease(u) + tumble
        case .crawl:
            let blend: Double = min(0.3, duration * 0.45)
            let k: Double = min(max(now.timeIntervalSince(start) / blend, 0), 1)
            return turnFrom + delta * GenieWalk.ease(k)
        case .hop, .land, .slide:
            return turnFrom + delta * u
        case .roam:
            let blend: Double = min(GenieGlass.pivot, duration * 0.5)
            let k: Double = GenieWalk.ease(min(max(now.timeIntervalSince(start) / blend, 0), 1))
            let walk: (point: CGPoint, heading: Double?) = along(GenieWalk.ease(u))
            guard let heading = aim ?? walk.heading else { return turnFrom + delta * GenieWalk.ease(u) }
            return turnFrom + GenieGravity.turn(from: turnFrom, to: heading) * k
        case .silk:
            let blend: Double = min(0.6, duration * 0.5)
            let k: Double = min(max(now.timeIntervalSince(start) / blend, 0), 1)
            return turnFrom + delta * GenieWalk.ease(k) + swing(at: now)
        }
    }

    public func gait(at now: Date) -> GenieGait {
        let u: Double = progress(at: now)
        switch kind {
        case .fly:
            return GenieGait(phase: now.timeIntervalSince(start) * 14, air: min(u / 0.1, 1))
        case .land:
            return GenieGait(land: sin(Double.pi * u))
        case .slide:
            let elapsed: Double = now.timeIntervalSince(start)
            let land: Double = elapsed < GenieGravity.landing ? sin(Double.pi * elapsed / GenieGravity.landing) : 0
            let walking: Double = u < 1 ? min((1 - u) / 0.2, 1) : 0
            guard path.count > 1 else { return GenieGait(phase: elapsed * 16, walking: walking, land: land) }
            let index: Int = min(Int(u * Double(path.count - 1)), path.count - 2)
            let angle: Double = -turnTo * Double.pi / 180
            let dx: Double = Double(path[index + 1].x - path[index].x)
            let dy: Double = Double(path[index + 1].y - path[index].y)
            let local: Double = dx * cos(angle) - dy * sin(angle)
            let facing: Double = abs(local) < 0.05 ? 0 : (local > 0 ? 1 : -1)
            return GenieGait(phase: elapsed * 16, walking: walking, facing: facing, land: land)
        case .roam:
            let walked: Double = GenieGlass.length(path) * GenieWalk.ease(u)
            let walking: Double = u > 0 && u < 1 && walked > 0 ? min(u / 0.12, (1 - u) / 0.12, 1) : 0
            return GenieGait(phase: GenieCrawl.phase(travelled: before + walked), walking: walking)
        case .crawl, .hop:
            let travelled: Double = before + length * GenieWalk.ease(u)
            let walking: Double = u > 0 && u < 1 ? min(u / 0.12, (1 - u) / 0.12, 1) : 0
            let angle: Double = -rotation(at: now) * Double.pi / 180
            let dx: Double = Double(to.x - from.x)
            let dy: Double = Double(to.y - from.y)
            let local: Double = dx * cos(angle) - dy * sin(angle)
            let facing: Double = abs(local) < 4 ? 0 : (local > 0 ? 1 : -1)
            return GenieGait(phase: GenieCrawl.phase(travelled: travelled), walking: walking, facing: facing)
        case .silk:
            let elapsed: Double = now.timeIntervalSince(start)
            guard let anchor = anchor, !hanging else { return GenieGait(phase: elapsed * 2, air: GenieGravity.dangle) }
            let climbing: Bool = hypot(to.x - anchor.x, to.y - anchor.y) < hypot(from.x - anchor.x, from.y - anchor.y)
            guard climbing else { return GenieGait(phase: elapsed * 3, air: GenieGravity.dangle) }
            let walking: Double = u > 0 && u < 1 ? min(u / 0.12, (1 - u) / 0.12, 1) : 0
            return GenieGait(phase: GenieCrawl.phase(travelled: before + length * GenieWalk.ease(u)), walking: walking)
        }
    }
}

public struct GenieTrack: Equatable, Sendable {
    public var moves: [GenieMove]

    public init(_ moves: [GenieMove]) {
        self.moves = moves
    }

    public var start: Date { moves.first?.start ?? .distantPast }

    public var end: Date { moves.last?.end ?? .distantPast }

    public var duration: Double { end.timeIntervalSince(start) }

    public var final: CGPoint? { moves.last?.to }

    public var surface: GenieEdge? { moves.last?.surface }

    public var turn: Double { moves.last?.turnTo ?? 0 }

    public var flying: Bool { moves.contains { $0.kind == .fly } }

    public func move(at now: Date) -> GenieMove? {
        moves.first { now < $0.end } ?? moves.last
    }

    public func airborne(at now: Date) -> Bool {
        move(at: now)?.kind == .fly && now < end
    }

    public func sliding(at now: Date) -> Bool {
        move(at: now)?.kind == .slide && now < end
    }

    public func silk(at now: Date) -> CGPoint? {
        guard now < end, let move = move(at: now), move.kind == .silk else { return nil }
        return move.anchor
    }

    public func point(at now: Date) -> CGPoint? { move(at: now)?.point(at: now) }

    public func rotation(at now: Date) -> Double? { move(at: now)?.rotation(at: now) }

    public func gait(at now: Date) -> GenieGait { move(at: now)?.gait(at: now) ?? .still }

    public func then(_ next: GenieTrack) -> GenieTrack {
        guard let last = moves.last else { return next }
        let shift: Double = last.end.timeIntervalSince(next.start)
        let shifted: [GenieMove] = next.moves.map { (move: GenieMove) -> GenieMove in
            var moved: GenieMove = move
            moved.start = move.start.addingTimeInterval(shift)
            return moved
        }
        return GenieTrack(moves + shifted)
    }
}

public enum GenieGravity {
    public nonisolated static let stand: CGFloat = 0.42
    public nonisolated static let pull: Double = 2200
    public nonisolated static let drag: Double = 0.00055
    public nonisolated static let settle: Double = 240
    public nonisolated static let grip: Double = 1500
    public nonisolated static let floorBounce: Double = 0.3
    public nonisolated static let wallBounce: Double = 0.42
    public nonisolated static let glance: Double = 0.78
    public nonisolated static let friction: Double = 2400
    public nonisolated static let rest: Double = 20
    public nonisolated static let maxSpeed: Double = 4200
    public nonisolated static let maxFlight: Double = 6
    public nonisolated static let step: Double = 1.0 / 120.0
    public nonisolated static let landing: Double = 0.24
    public nonisolated static let down: CGSize = CGSize(width: 0, height: 1)
    public nonisolated static let ceiling: Double = -0.6
    public nonisolated static let shortestSilk: Double = 60
    public nonisolated static let swingPeriod: Double = 1.7
    public nonisolated static let swingDegrees: Double = 7
    public nonisolated static let dangle: Double = 0.3

    public static func unit(_ gravity: CGSize) -> CGSize {
        let length: CGFloat = hypot(gravity.width, gravity.height)
        guard length > 0.05 else { return down }
        return CGSize(width: gravity.width / length, height: gravity.height / length)
    }

    public static func upright(_ gravity: CGSize) -> Double {
        let g: CGSize = unit(gravity)
        return atan2(Double(-g.width), Double(g.height)) * 180 / Double.pi
    }

    public static func turn(from: Double, to: Double) -> Double {
        var delta: Double = (to - from).truncatingRemainder(dividingBy: 360)
        if delta > 180 { delta -= 360 }
        if delta < -180 { delta += 360 }
        return delta
    }

    public static func lowness(_ surface: GenieEdge, gravity: CGSize) -> Double {
        let g: CGSize = unit(gravity)
        let n: CGSize = GenieArena.normal(surface)
        return Double(n.width * g.width + n.height * g.height)
    }

    public static func floor(_ gravity: CGSize) -> GenieEdge {
        GenieEdge.allCases.max { lowness($0, gravity: gravity) < lowness($1, gravity: gravity) } ?? .bottom
    }

    public static func pace(along direction: CGSize, on surface: GenieEdge, gravity: CGSize) -> Double {
        let length: CGFloat = hypot(direction.width, direction.height)
        guard length > 0.01 else { return 1 }
        let g: CGSize = unit(gravity)
        let climb: Double = -Double((direction.width * g.width + direction.height * g.height) / length)
        var pace: Double = 1 - 0.45 * max(climb, 0) + 0.2 * max(-climb, 0)
        if lowness(surface, gravity: gravity) < -0.5 { pace *= 0.82 }
        return pace
    }

    static func flightStep(_ point: CGPoint, velocity: CGSize, gravity: CGSize, seconds: Double = step) -> (point: CGPoint, velocity: CGSize) {
        let dt: Double = min(max(seconds, 0), 0.05)
        let g: CGSize = unit(gravity)
        var beforeX: Double = Double(velocity.width)
        var beforeY: Double = Double(velocity.height)
        let launchSpeed: Double = hypot(beforeX, beforeY)
        if launchSpeed > maxSpeed {
            beforeX *= maxSpeed / launchSpeed
            beforeY *= maxSpeed / launchSpeed
        }
        var vx: Double = beforeX
        var vy: Double = beforeY + Double(g.height) * pull * dt
        let slow: (Double) -> Double = { velocity in velocity / (1 + drag * abs(velocity) * dt) }
        vx = slow(vx)
        vy = slow(vy)
        let speed: Double = hypot(vx, vy)
        if speed > maxSpeed {
            vx *= maxSpeed / speed
            vy *= maxSpeed / speed
        }
        return (
            point: CGPoint(x: point.x + CGFloat((beforeX + vx) * dt / 2), y: point.y + CGFloat((beforeY + vy) * dt / 2)),
            velocity: CGSize(width: vx, height: vy))
    }

    public static func fly(
        from start: CGPoint, velocity: CGSize, gravity: CGSize, arena: GenieArena, at time: Date, turn: Double
    ) -> GenieTrack {
        let box: CGRect = arena.box
        let g: CGSize = unit(gravity)
        var p: CGPoint = GenieCrawl.clamp(start, to: box)
        let speed: Double = Double(hypot(velocity.width, velocity.height))
        let cap: Double = speed > maxSpeed ? maxSpeed / speed : 1
        var vx: Double = Double(velocity.width) * cap
        var vy: Double = Double(velocity.height) * cap
        var samples: [CGPoint] = [p]
        var landed: GenieEdge?
        var t: Double = 0
        while t < maxFlight, landed == nil {
            let next = flightStep(p, velocity: CGSize(width: vx, height: vy), gravity: g)
            p = next.point
            vx = Double(next.velocity.width)
            vy = Double(next.velocity.height)
            t += step
            let hits: [(GenieEdge, Bool)] = [
                (.left, p.x < box.minX), (.right, p.x > box.maxX), (.top, p.y < box.minY), (.bottom, p.y > box.maxY),
            ]
            for (surface, hit) in hits where hit && landed == nil {
                let n: CGSize = GenieArena.normal(surface)
                let into: Double = vx * Double(n.width) + vy * Double(n.height)
                p = GenieCrawl.clamp(p, to: box)
                let floorLike: Bool = lowness(surface, gravity: g) > 0.6
                if into < (floorLike ? settle : grip) {
                    landed = surface
                    vx -= into * Double(n.width)
                    vy -= into * Double(n.height)
                    if !floorLike {
                        vx = 0
                        vy = 0
                    }
                    break
                }
                let bounce: Double = floorLike ? floorBounce : wallBounce
                let tx: Double = vx - into * Double(n.width)
                let ty: Double = vy - into * Double(n.height)
                vx = tx * glance - into * bounce * Double(n.width)
                vy = ty * glance - into * bounce * Double(n.height)
            }
            samples.append(p)
        }
        let surface: GenieEdge = landed ?? arena.nearest(p)
        let touch: CGPoint = arena.on(surface, near: p)
        if samples.last != touch { samples.append(touch) }
        let flight: GenieMove = GenieMove(
            kind: .fly, from: samples.first ?? touch, to: touch, surface: surface, start: time,
            duration: Double(samples.count - 1) * step, turnFrom: turn, turnTo: surface.degrees, path: samples)
        let trail: [CGPoint] = landed == nil ? [touch] : skid(from: touch, velocity: CGSize(width: vx, height: vy), arena: arena)
        if trail.count > 2, let last = trail.last {
            let slide: GenieMove = GenieMove(
                kind: .slide, from: touch, to: last, surface: surface, start: flight.end, duration: Double(trail.count - 1) * step,
                turnFrom: surface.degrees, turnTo: surface.degrees, path: trail)
            return GenieTrack([flight, slide])
        }
        let bump: GenieMove = GenieMove(
            kind: .land, from: touch, to: touch, surface: surface, start: flight.end, duration: landing, turnFrom: surface.degrees,
            turnTo: surface.degrees)
        return GenieTrack([flight, bump])
    }

    public static func skid(from start: CGPoint, velocity: CGSize, arena: GenieArena) -> [CGPoint] {
        let box: CGRect = arena.box
        var p: CGPoint = start
        var vx: Double = Double(velocity.width)
        var vy: Double = Double(velocity.height)
        var path: [CGPoint] = [p]
        var t: Double = 0
        while t < maxFlight {
            let speed: Double = hypot(vx, vy)
            let slower: Double = speed - friction * step
            if slower < rest { break }
            vx *= slower / speed
            vy *= slower / speed
            p.x += CGFloat(vx * step)
            p.y += CGFloat(vy * step)
            t += step
            if p.x < box.minX || p.x > box.maxX {
                vx = -vx * wallBounce
            }
            if p.y < box.minY || p.y > box.maxY {
                vy = -vy * wallBounce
            }
            p = GenieCrawl.clamp(p, to: box)
            path.append(p)
        }
        return path
    }

    public static func fall(from point: CGPoint, on surface: GenieEdge?, gravity: CGSize, arena: GenieArena, at time: Date, turn: Double)
        -> GenieTrack
    {
        let push: CGSize
        if let surface {
            let n: CGSize = GenieArena.normal(surface)
            push = CGSize(width: -n.width * 140, height: -n.height * 140)
        } else {
            push = .zero
        }
        return fly(from: point, velocity: push, gravity: gravity, arena: arena, at: time, turn: turn)
    }

    public static func crawl(
        from point: CGPoint, on surface: GenieEdge, to target: CGPoint, on goal: GenieEdge, arena: GenieArena, gravity: CGSize,
        speed: Double, at time: Date, turn: Double
    ) -> GenieTrack {
        let a: CGPoint = arena.on(surface, near: point)
        let b: CGPoint = arena.on(goal, near: target)
        let options: [[(CGPoint, GenieEdge)]] = [true, false].map { (clockwise: Bool) -> [(CGPoint, GenieEdge)] in
            var path: [(CGPoint, GenieEdge)] = arena.corners(clockwise: clockwise, from: surface, to: goal)
            path.append((b, goal))
            return path
        }
        func cost(_ path: [(CGPoint, GenieEdge)]) -> Double {
            var from: CGPoint = a
            var total: Double = 0
            for (to, on) in path {
                let length: Double = Double(hypot(to.x - from.x, to.y - from.y))
                total += length / pace(along: CGSize(width: to.x - from.x, height: to.y - from.y), on: on, gravity: gravity)
                from = to
            }
            return total
        }
        let sameSide: Bool = surface == goal
        let chosen: [(CGPoint, GenieEdge)] = sameSide ? [(b, goal)] : (options.min { cost($0) < cost($1) } ?? [(b, goal)])
        var moves: [GenieMove] = []
        var from: CGPoint = a
        var clock: Date = time
        var facing: Double = turn
        var walked: Double = 0
        for (to, on) in chosen {
            let length: Double = Double(hypot(to.x - from.x, to.y - from.y))
            let direction: CGSize = CGSize(width: to.x - from.x, height: to.y - from.y)
            if length < 1 && on.degrees == facing { continue }
            let duration: Double = max(length / (speed * pace(along: direction, on: on, gravity: gravity)), 0.3)
            let move: GenieMove = GenieMove(
                kind: .crawl, from: from, to: to, surface: on, start: clock, duration: duration, turnFrom: facing, turnTo: on.degrees,
                before: walked)
            moves.append(move)
            walked += length
            clock = move.end
            facing = on.degrees
            from = to
        }
        if let last = moves.last, last.surface != goal {
            moves.append(GenieMove(
                kind: .crawl, from: b, to: b, surface: goal, start: clock, duration: 0.3, turnFrom: facing, turnTo: goal.degrees, before: walked))
        }
        return GenieTrack(moves)
    }

    public static func silk(
        from point: CGPoint, on surface: GenieEdge, arena: GenieArena, gravity: CGSize, depth: Double, hang: Double, down: Bool,
        speed: Double, at time: Date, turn: Double
    ) -> GenieTrack? {
        guard lowness(surface, gravity: gravity) < ceiling, speed > 0 else { return nil }
        let g: CGSize = unit(gravity)
        let anchor: CGPoint = arena.on(surface, near: point)
        guard let hit = ray(from: anchor, along: g, in: arena.box), hit.reach >= shortestSilk else { return nil }
        let (ground, edge, reach): (CGPoint, GenieEdge, Double) = (hit.end, hit.edge, hit.reach)
        let length: Double = reach * min(max(depth, 0.15), 0.85)
        let low: CGPoint = CGPoint(x: anchor.x + g.width * CGFloat(length), y: anchor.y + g.height * CGFloat(length))
        let hanging: Double = upright(g)
        let lower: GenieMove = GenieMove(
            kind: .silk, from: anchor, to: low, surface: nil, start: time, duration: max(length / (speed * 0.9), 0.6), turnFrom: turn,
            turnTo: hanging, anchor: anchor)
        let wait: GenieMove = GenieMove(
            kind: .silk, from: low, to: low, surface: nil, start: lower.end, duration: max(hang, 0.5), turnFrom: hanging, turnTo: hanging,
            anchor: anchor)
        if down {
            let rest: GenieMove = GenieMove(
                kind: .silk, from: low, to: ground, surface: nil, start: wait.end, duration: max((reach - length) / (speed * 1.1), 0.4),
                turnFrom: hanging, turnTo: edge.degrees, anchor: anchor)
            let bump: GenieMove = GenieMove(
                kind: .land, from: ground, to: ground, surface: edge, start: rest.end, duration: landing, turnFrom: edge.degrees,
                turnTo: edge.degrees)
            return GenieTrack([lower, wait, rest, bump])
        }
        let climb: GenieMove = GenieMove(
            kind: .silk, from: low, to: anchor, surface: nil, start: wait.end, duration: max(length / (speed * 0.6), 0.6), turnFrom: hanging,
            turnTo: hanging, anchor: anchor)
        let grip: GenieMove = GenieMove(
            kind: .crawl, from: anchor, to: anchor, surface: surface, start: climb.end, duration: 0.3, turnFrom: hanging,
            turnTo: surface.degrees)
        return GenieTrack([lower, wait, climb, grip])
    }

    static func ray(from point: CGPoint, along g: CGSize, in box: CGRect) -> (end: CGPoint, edge: GenieEdge, reach: Double)? {
        var hits: [(Double, GenieEdge)] = []
        if g.width > 0.001 { hits.append((Double((box.maxX - point.x) / g.width), .right)) }
        if g.width < -0.001 { hits.append((Double((box.minX - point.x) / g.width), .left)) }
        if g.height > 0.001 { hits.append((Double((box.maxY - point.y) / g.height), .bottom)) }
        if g.height < -0.001 { hits.append((Double((box.minY - point.y) / g.height), .top)) }
        guard let first = hits.filter({ $0.0 > 0.5 }).min(by: { $0.0 < $1.0 }) else { return nil }
        let end: CGPoint = CGPoint(x: point.x + g.width * CGFloat(first.0), y: point.y + g.height * CGFloat(first.0))
        return (GenieCrawl.clamp(end, to: box), first.1, first.0)
    }

    public static func hops(through points: [CGPoint], from start: CGPoint, seconds: Double, at time: Date, turn: Double) -> GenieTrack {
        var from: CGPoint = start
        let total: Double = points.reduce((0.0, start)) { (sum: (Double, CGPoint), next: CGPoint) in
            (sum.0 + Double(hypot(next.x - sum.1.x, next.y - sum.1.y)), next)
        }.0
        guard total > 1 else { return GenieTrack([]) }
        var clock: Date = time
        var walked: Double = 0
        var moves: [GenieMove] = []
        for point in points {
            let length: Double = Double(hypot(point.x - from.x, point.y - from.y))
            let move: GenieMove = GenieMove(
                kind: .hop, from: from, to: point, surface: nil, start: clock, duration: max(seconds * length / total, 0.12), turnFrom: turn,
                turnTo: turn, before: walked)
            moves.append(move)
            walked += length
            clock = move.end
            from = point
        }
        return GenieTrack(moves)
    }

    public static func wander(
        from point: CGPoint, on surface: GenieEdge, arena: GenieArena, gravity: CGSize, random: () -> Double
    ) -> (CGPoint, GenieEdge) {
        let box: CGRect = arena.box
        let roll: Double = random()
        let goal: GenieEdge
        if roll < 0.55 {
            goal = surface
        } else {
            let low: GenieEdge = floor(gravity)
            let pick: Double = random()
            let walls: [GenieEdge] = GenieEdge.allCases.filter { abs(lowness($0, gravity: gravity)) < 0.6 }
            if pick < 0.5 || walls.isEmpty {
                goal = low
            } else if pick < 0.85 {
                goal = walls[min(Int(random() * Double(walls.count)), walls.count - 1)]
            } else {
                goal = GenieEdge.allCases.min { lowness($0, gravity: gravity) < lowness($1, gravity: gravity) } ?? .top
            }
        }
        let along: CGFloat = CGFloat(random())
        var target: CGPoint
        switch goal {
        case .top, .bottom: target = CGPoint(x: box.minX + box.width * along, y: point.y)
        case .left, .right: target = CGPoint(x: point.x, y: box.minY + box.height * along)
        }
        if goal == surface {
            let reach: CGFloat = GenieCrawl.shortHop * CGFloat(0.35 + 0.65 * random()) * (random() < 0.5 ? -1 : 1)
            switch goal {
            case .top, .bottom: target = CGPoint(x: point.x + reach, y: point.y)
            case .left, .right: target = CGPoint(x: point.x, y: point.y + reach)
            }
        }
        return (arena.on(goal, near: target), goal)
    }
}
