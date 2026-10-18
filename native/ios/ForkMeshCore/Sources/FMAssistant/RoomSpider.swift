import Foundation

public enum RoomSpiderMode: Equatable, Sendable {
    case hidden
    case crawling
    case flying
    case held
    case silk(anchor: RoomPoint, onto: String)
}

public struct RoomSpiderGoal: Equatable, Sendable {
    public var point: RoomPoint
    public var surface: String
    public var asked: Bool
}

public struct RoomSpider: Sendable {
    public static let stand: Float = 0.045
    public static let pace: Float = 0.22
    public static let turnRate: Float = 4.5
    public static let gravity: Float = 9.8
    public static let drag: Float = 0.25
    public static let mostThrow: Float = 7
    public static let bounceFloor: Float = 1.8
    public static let keep: Float = 0.35
    public static let dizzySpeed: Float = 3.6
    public static let leapReach: Float = 0.8
    public static let leapRise: Float = 1.3
    public static let silkSpeed: Float = 0.35
    public static let mostWebs: Int = 6
    public static let tick: Float = 1.0 / 120.0
    public static let giveUp: Double = 14
    public static let mostFlight: Double = 6

    public private(set) var mode: RoomSpiderMode = .hidden
    public private(set) var position: RoomPoint = .zero
    public private(set) var velocity: RoomPoint = .zero
    public private(set) var up: RoomPoint = RoomMath.up
    public private(set) var heading: RoomPoint = RoomPoint(0, 0, 1)
    public private(set) var surface: String?
    public private(set) var goal: RoomSpiderGoal?
    public private(set) var act: RoomSpiderAct?
    public private(set) var clock: Double = 0
    public private(set) var stride: Float = 0
    public private(set) var walking: Float = 0
    public private(set) var spin: Float = 0
    public private(set) var webs: [RoomWeb] = []
    public private(set) var thumps: Int = 0
    public private(set) var visited: Set<String> = []
    private var actStart: Double = 0
    private var actUntil: Double = 0
    private var restUntil: Double = 0
    private var goalSince: Double = 0
    private var flightStart: Double = 0
    private var bounces: Int = 0
    private var line: String = ""
    private var lineUntil: Double = 0
    private var samples: [(RoomPoint, Double)] = []
    private var nextWeb: Int = 1
    private var dice: RoomDice

    public init(seed: UInt64 = 0x5EED) {
        dice = RoomDice(seed: seed)
    }

    public var caption: String? { clock < lineUntil && !line.isEmpty ? line : nil }

    public var actProgress: Float {
        guard act != nil, actUntil > actStart else { return 0 }
        return Float(max(0, min(1, (clock - actStart) / (actUntil - actStart))))
    }

    public var silkAnchor: RoomPoint? {
        if case .silk(let anchor, _) = mode { return anchor }
        return nil
    }

    public var visible: Bool { mode != .hidden }

    public mutating func appear(at point: RoomPoint, in room: RoomMap) {
        let target: RoomSurface = room.below(point, besides: "") ?? room.floor
        position = point
        velocity = .zero
        up = RoomMath.up
        goal = nil
        act = nil
        spin = 0
        surface = nil
        mode = .silk(anchor: point, onto: target.id)
        let toward: RoomPoint = RoomMath.flat(room.viewer - point, on: RoomMath.up)
        heading = RoomMath.normalized(toward, or: heading)
        say(RoomSpiderAct.hello.line, for: 2.4)
    }

    public mutating func grab(at point: RoomPoint, time: Double) {
        guard visible else { return }
        mode = .held
        position = point
        velocity = .zero
        surface = nil
        goal = nil
        act = nil
        samples = [(point, time)]
        say("Whoa, put me down!", for: 1.6)
    }

    public mutating func drag(to point: RoomPoint, time: Double) {
        guard mode == .held else { return }
        position = point
        samples.append((point, time))
        if samples.count > 8 { samples.removeFirst(samples.count - 8) }
    }

    public mutating func release(time: Double, in room: RoomMap) {
        guard mode == .held else { return }
        var thrown: RoomPoint = .zero
        if let last = samples.last, let first = samples.first(where: { last.1 - $0.1 <= 0.1 }), last.1 - first.1 > 0.005 {
            thrown = (last.0 - first.0) / Float(last.1 - first.1)
        }
        let speed: Float = RoomMath.length(thrown)
        if speed > RoomSpider.mostThrow { thrown *= RoomSpider.mostThrow / speed }
        samples = []
        if speed < 0.3, let near = placed(in: room) {
            settle(on: near, at: position, room: room)
            return
        }
        velocity = thrown
        mode = .flying
        flightStart = clock
        bounces = 0
        if speed > 1.5 { say(RoomSpiderAct.swing.line, for: 1.2) }
    }

    public mutating func poke() {
        guard mode == .crawling else { return }
        perform(.giggle)
    }

    public mutating func come(to point: RoomPoint, on id: String, in room: RoomMap) {
        guard mode == .crawling || mode == .flying || silkAnchor != nil, let target = room.surface(id) else { return }
        goal = RoomSpiderGoal(point: target.project(point), surface: id, asked: true)
        goalSince = clock
        restUntil = 0
        if let current = act, current != .dizzy { act = nil; actUntil = clock }
        say("On my way!", for: 1.4)
    }

    public mutating func clearWebs() {
        webs = []
    }

    public mutating func hide() {
        mode = .hidden
        goal = nil
        act = nil
        surface = nil
    }

    public mutating func step(_ dt: Double, in room: RoomMap) {
        let span: Double = max(0, min(dt, 0.1))
        guard span > 0 else { return }
        clock += span
        if let current = act, clock >= actUntil { finish(current, room: room) }
        switch mode {
        case .hidden:
            return
        case .held:
            walking = max(0, walking - Float(span) * 3)
            stride += Float(span) * 14
        case .flying:
            var left: Float = Float(span)
            while left > 1e-5, mode == .flying {
                let h: Float = min(RoomSpider.tick, left)
                fly(h, room: room)
                left -= h
            }
        case .silk(let anchor, let onto):
            dangle(Float(span), anchor: anchor, onto: onto, room: room)
        case .crawling:
            crawl(Float(span), room: room)
        }
    }

    private mutating func fly(_ h: Float, room: RoomMap) {
        velocity.y -= RoomSpider.gravity * h
        velocity *= max(0, 1 - RoomSpider.drag * h)
        let next: RoomPoint = position + velocity * h
        spin += RoomMath.length(velocity) * h * 5
        walking = max(0, walking - h * 3)
        if let hit = room.hit(from: position, to: next, lift: RoomSpider.stand) {
            land(hit, room: room)
            return
        }
        position = next
        if clock - flightStart > RoomSpider.mostFlight || position.y < room.floorY - 1 {
            let floor: RoomSurface = room.floor
            settle(on: floor, at: floor.project(position), room: room)
        }
    }

    private mutating func land(_ hit: RoomMap.Hit, room: RoomMap) {
        let into: Float = -RoomMath.dot(velocity, hit.surface.normal)
        let speed: Float = RoomMath.length(velocity)
        thumps += 1
        if hit.surface.upright, into > RoomSpider.bounceFloor, bounces < 3 {
            let along: RoomPoint = RoomMath.flat(velocity, on: hit.surface.normal) * 0.7
            velocity = along + hit.surface.normal * (into * RoomSpider.keep)
            position = hit.point
            bounces += 1
            return
        }
        let wasThrown: Bool = clock - flightStart > 0.25 || bounces > 0
        settle(on: hit.surface, at: hit.point, room: room)
        if speed > RoomSpider.dizzySpeed {
            perform(.dizzy)
        } else if wasThrown, speed > 1.6, dice.next() < 0.5 {
            perform(.cheer)
        }
    }

    private func placed(in room: RoomMap) -> RoomSurface? {
        let level: Float = room.floorY
        return room.all
            .filter { abs($0.height(position) - RoomSpider.stand) < 0.09 && $0.covers(position, margin: 0.02, floor: level) }
            .min { abs($0.height(position)) < abs($1.height(position)) }
    }

    private mutating func settle(on target: RoomSurface, at point: RoomPoint, room: RoomMap) {
        let carried: RoomPoint = RoomMath.flat(RoomMath.length(velocity) > 0.05 ? velocity : heading, on: target.normal)
        heading = RoomMath.normalized(carried, or: RoomMath.perpendicular(to: target.normal))
        up = target.normal
        position = target.project(point, lift: RoomSpider.stand)
        velocity = .zero
        spin = 0
        bounces = 0
        mode = .crawling
        surface = target.id
        restUntil = clock + 0.6
        arrive(on: target)
    }

    private mutating func arrive(on target: RoomSurface) {
        let first: Bool = visited.insert(target.id).inserted
        guard act == nil, first || dice.next() < 0.3 else { return }
        if let reaction = RoomSpiderAct.arriving(on: target.kind, upright: target.upright) {
            perform(reaction)
        }
    }

    private mutating func dangle(_ dt: Float, anchor: RoomPoint, onto id: String, room: RoomMap) {
        walking = max(0, walking - dt * 3)
        guard let target = room.surface(id) else {
            mode = .flying
            flightStart = clock
            return
        }
        position = RoomPoint(anchor.x, position.y - RoomSpider.silkSpeed * dt, anchor.z)
        if target.height(position) <= RoomSpider.stand {
            settle(on: target, at: position, room: room)
        }
    }

    private mutating func crawl(_ dt: Float, room: RoomMap) {
        guard let id = surface, let here = room.surface(id) else {
            mode = .flying
            flightStart = clock
            return
        }
        up = here.normal
        position = here.project(position, lift: RoomSpider.stand)
        heading = RoomMath.normalized(RoomMath.flat(heading, on: up), or: RoomMath.perpendicular(to: up))
        if let current = act {
            walking = max(0, walking - dt * 4)
            if !current.still { heading = RoomMath.turn(heading, toward: RoomMath.cross(up, heading), on: up, by: dt * 6) }
            return
        }
        if clock < restUntil {
            walking = max(0, walking - dt * 4)
            heading = RoomMath.turn(heading, toward: room.viewer - position, on: up, by: dt * 2)
            return
        }
        if let current = goal, !current.asked, clock - goalSince > RoomSpider.giveUp { goal = nil }
        if let current = goal, current.asked, clock - goalSince > RoomSpider.giveUp * 2 { goal = nil }
        if goal == nil { think(on: here, room: room) }
        guard goal != nil, let aim = steer(on: here, room: room) else { return }
        let toward: RoomPoint = RoomMath.flat(aim - position, on: up)
        if RoomMath.length(toward) < 0.04 {
            reached(on: here, room: room)
            return
        }
        heading = RoomMath.turn(heading, toward: toward, on: up, by: RoomSpider.turnRate * dt)
        let climbing: Bool = here.vertical && heading.y > 0.5
        let speed: Float = RoomSpider.pace * (climbing ? 0.7 : 1) * (heading.y < -0.5 ? 1.2 : 1)
        let next: RoomPoint = position + heading * (speed * dt)
        stride += speed * dt * 38
        walking = min(1, walking + dt * 4)
        if let wall = room.wall(ahead: next + heading * 0.02, from: position, on: here, lift: RoomSpider.stand) {
            move(onto: wall, at: next)
            return
        }
        if !here.covers(next, floor: room.floorY) {
            edge(of: here, at: next, room: room)
            return
        }
        position = next
    }

    private mutating func move(onto next: RoomSurface, at point: RoomPoint) {
        let turned: RoomPoint = RoomMath.rotate(heading, from: up, to: next.normal)
        heading = RoomMath.normalized(RoomMath.flat(turned, on: next.normal), or: RoomMath.perpendicular(to: next.normal))
        up = next.normal
        surface = next.id
        position = next.project(point, lift: RoomSpider.stand)
        arrive(on: next)
    }

    private mutating func edge(of here: RoomSurface, at point: RoomPoint, room: RoomMap) {
        if let next = room.neighbor(of: here, at: point, lift: RoomSpider.stand) {
            move(onto: next, at: point)
            return
        }
        if here.upright || heading.y < -0.3, let below = room.below(point, besides: here.id) {
            position = point
            mode = .silk(anchor: point, onto: below.id)
            surface = nil
            if dice.next() < 0.5 { say(RoomSpiderAct.swing.line, for: 1.4) }
            return
        }
        if let target = goal.flatMap({ room.surface($0.surface) }), target.overhead, here.vertical, heading.y > 0.3,
           let top = room.above(point, besides: here.id) {
            leap(to: top.project(point, lift: RoomSpider.stand) + here.normal * 0.15, rise: 0.5)
            return
        }
        heading = -heading
        if goal?.asked != true { goal = nil }
    }

    private mutating func reached(on here: RoomSurface, room: RoomMap) {
        guard let current = goal else { return }
        if here.matches(current.surface, at: current.point, in: room) {
            let asked: Bool = current.asked
            goal = nil
            restUntil = clock + Double(dice.between(1.2, 3.5))
            if asked { perform(dice.next() < 0.5 ? .wave : .cheer) }
            return
        }
        if let target = room.surface(current.surface), target.upright, RoomMath.length(target.project(current.point) - position) < RoomSpider.leapReach {
            leap(to: target.project(current.point, lift: RoomSpider.stand), rise: 0.35)
            return
        }
        if here.upright {
            goal = nil
            return
        }
        mode = .flying
        flightStart = clock
        velocity = here.normal * 0.4
        surface = nil
    }

    private mutating func think(on here: RoomSurface, room: RoomMap) {
        let roll: Float = dice.next()
        let level: Float = room.floorY
        let walls: [RoomSurface] = room.detected.filter(\.vertical)
        let tops: [RoomSurface] = room.detected.filter { $0.upright && $0.center.y > level + 0.15 }
        let ceilings: [RoomSurface] = room.detected.filter(\.overhead)
        if here.id == RoomMap.floorId || here.kind == .floor {
            if roll < 0.25, let wall = pick(walls) {
                let height: Float = dice.between(0.25, 1.2)
                let spot: RoomPoint = wall.clamped(wall.point(SIMD2<Float>(dice.between(-wall.half.x, wall.half.x), 0)), inset: 0.1, floor: level)
                aim(RoomPoint(spot.x, level + height, spot.z), on: wall)
            } else if roll < 0.5, let top = pick(tops) {
                aim(top.point(SIMD2<Float>(dice.between(-top.half.x, top.half.x) * 0.6, dice.between(-top.half.y, top.half.y) * 0.6)), on: top)
            } else if roll < 0.9 {
                let around: RoomPoint = RoomPoint(room.viewer.x, level, room.viewer.z)
                let angle: Float = dice.between(0, 2 * .pi)
                let reach: Float = dice.between(0.5, 1.4)
                aim(around + RoomPoint(cos(angle) * reach, 0, sin(angle) * reach), on: here)
            } else {
                restUntil = clock + Double(dice.between(1.5, 3))
                perform(.wave)
            }
        } else if here.vertical {
            if roll < 0.3, webs.count < RoomSpider.mostWebs, !webs.contains(where: { RoomMath.length($0.center - position) < 0.35 }) {
                perform(.weave)
            } else if roll < 0.6 {
                aim(here.clamped(here.point(SIMD2<Float>(dice.between(-here.half.x, here.half.x), dice.between(-here.half.y, here.half.y))), inset: 0.08, floor: level), on: here)
            } else if roll < 0.75, let top = pick(ceilings) {
                aim(top.project(position), on: top)
            } else {
                let floor: RoomSurface = room.floor
                aim(floor.project(position) + here.normal * dice.between(0.2, 0.8), on: floor)
            }
        } else if here.overhead {
            if roll < 0.5 {
                aim(here.clamped(here.point(SIMD2<Float>(dice.between(-here.half.x, here.half.x), dice.between(-here.half.y, here.half.y))), inset: 0.1, floor: level), on: here)
            } else if let below = room.below(position, besides: here.id) {
                mode = .silk(anchor: position, onto: below.id)
                surface = nil
                say(RoomSpiderAct.swing.line, for: 1.4)
            }
        } else {
            if roll < 0.45 {
                aim(here.clamped(here.point(SIMD2<Float>(dice.between(-here.half.x, here.half.x), dice.between(-here.half.y, here.half.y))), inset: 0.06, floor: level), on: here)
            } else {
                let floor: RoomSurface = room.floor
                let away: RoomPoint = RoomMath.normalized(RoomMath.flat(position - here.center, on: RoomMath.up), or: heading)
                aim(floor.project(position + away * (max(here.half.x, here.half.y) + 0.3)), on: floor)
            }
        }
    }

    private mutating func aim(_ point: RoomPoint, on target: RoomSurface) {
        goal = RoomSpiderGoal(point: point, surface: target.id, asked: false)
        goalSince = clock
    }

    private mutating func pick(_ list: [RoomSurface]) -> RoomSurface? {
        guard !list.isEmpty else { return nil }
        return list[min(Int(dice.next() * Float(list.count)), list.count - 1)]
    }

    private mutating func steer(on here: RoomSurface, room: RoomMap) -> RoomPoint? {
        guard let current = goal else { return nil }
        let level: Float = room.floorY
        if here.matches(current.surface, at: current.point, in: room) {
            if current.surface != here.id { return here.project(current.point, lift: RoomSpider.stand) }
            return here.clamped(current.point, inset: 0.04, floor: level, lift: RoomSpider.stand)
        }
        guard let target = room.surface(current.surface) else {
            goal = nil
            return nil
        }
        let spot: RoomPoint = target.clamped(current.point, inset: 0.04, floor: level)
        if here.upright {
            if target.upright {
                let rise: Float = target.center.y - position.y
                let gap: Float = RoomMath.length(RoomMath.flat(spot - position, on: RoomMath.up))
                if rise > -0.1, rise < RoomSpider.leapRise, gap < RoomSpider.leapReach {
                    leap(to: target.project(spot, lift: RoomSpider.stand), rise: 0.35)
                    return nil
                }
                return here.project(spot, lift: RoomSpider.stand)
            }
            if target.vertical {
                return here.project(target.project(spot) - target.normal * 0.15, lift: RoomSpider.stand)
            }
            if let wall = room.nearest(position, where: \.vertical) {
                let base: RoomPoint = wall.clamped(position, inset: 0.05, floor: level)
                return here.project(base - wall.normal * 0.15, lift: RoomSpider.stand)
            }
            goal = nil
            return nil
        }
        if here.overhead {
            if let below = room.below(position, besides: here.id) {
                mode = .silk(anchor: position, onto: below.id)
                surface = nil
            }
            return nil
        }
        if target.upright, target.center.y > level + 0.15, RoomMath.length(target.project(spot) - position) < 1.0 {
            leap(to: target.project(spot, lift: RoomSpider.stand), rise: 0.3)
            return nil
        }
        if target.overhead {
            return here.project(position + RoomMath.up * 2, lift: RoomSpider.stand)
        }
        if target.upright, target.center.y < position.y {
            return here.project(spot - RoomMath.up * 0.2, lift: RoomSpider.stand)
        }
        return here.project(spot, lift: RoomSpider.stand)
    }

    private mutating func leap(to point: RoomPoint, rise: Float) {
        let travel: Float = RoomMath.length(point - position)
        let time: Float = max(0.35, min(0.9, 0.3 + travel * 0.35 + rise * 0.3))
        velocity = (point - position) / time + RoomPoint(0, 0.5 * RoomSpider.gravity * time, 0)
        position += up * 0.01
        mode = .flying
        flightStart = clock
        surface = nil
        bounces = 0
    }

    private mutating func perform(_ next: RoomSpiderAct) {
        act = next
        actStart = clock
        actUntil = clock + next.seconds
        say(next.line, for: max(next.seconds, 1.8))
    }

    private mutating func finish(_ done: RoomSpiderAct, room: RoomMap) {
        act = nil
        if done == .weave, mode == .crawling, let id = surface, let here = room.surface(id), webs.count < RoomSpider.mostWebs {
            let size: Float = dice.between(0.16, 0.26)
            let center: RoomPoint = here.project(position + heading * (size * 0.6), lift: 0.004)
            webs.append(RoomWeb(id: nextWeb, center: center, normal: here.normal, across: heading, size: size, surface: id))
            nextWeb += 1
        }
        if done == .nap || done == .dizzy { restUntil = clock + 0.8 }
    }

    private mutating func say(_ text: String, for seconds: Double) {
        line = text
        lineUntil = clock + seconds
    }
}
