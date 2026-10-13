import FMAssistant
import FMBase
import Foundation
import Observation
import SwiftUI
import UIKit

@MainActor
@Observable
final class GenieCrawler {
    static let shared: GenieCrawler = GenieCrawler()
    nonisolated static let spotXKey: String = "genie/spot_x"
    nonisolated static let spotYKey: String = "genie/spot_y"
    nonisolated static let surfaceKey: String = "genie/surface"
    nonisolated static let parkedKey: String = "genie/parked"
    nonisolated static let wandersKey: String = "genie/wanders"
    nonisolated static let home: CGPoint = CGPoint(x: 0.86, y: 1)
    nonisolated static let rest: ClosedRange<Double> = 5...13
    nonisolated static let tuckSpeed: CGFloat = 260
    nonisolated static let base: CGFloat = 92
    nonisolated static let silkOdds: Double = 0.35
    nonisolated static let silkToFloor: Double = 0.35
    nonisolated static let silkDepth: ClosedRange<Double> = 0.3...0.7
    nonisolated static let silkHang: ClosedRange<Double> = 1.4...3.2
    nonisolated static let swirlWindow: Double = 0.2
    nonisolated static let hopPace: Double = 2.2
    nonisolated static let actPace: Double = 0.8

    private(set) var full: CGRect = .zero
    private(set) var safe: CGRect = .zero
    private(set) var floor: CGFloat?
    private(set) var side: CGFloat = GenieCrawler.base
    private(set) var spot: CGPoint = .zero
    private(set) var surface: GenieEdge?
    private(set) var turn: Double = 0
    private(set) var track: GenieTrack?
    private(set) var parked: GenieEdge?
    private(set) var wanders: Bool
    private(set) var held: Bool = false
    private(set) var tumbling: Bool = false
    private(set) var glass: Bool = false

    @ObservationIgnored private let prefs: KeyValueStore?
    @ObservationIgnored private var fraction: CGPoint
    @ObservationIgnored private var savedSurface: GenieEdge?
    @ObservationIgnored private var grabbed: CGPoint = .zero
    @ObservationIgnored private var arrival: Task<Void, Never>?
    @ObservationIgnored private var tumble: GenieTumble?
    @ObservationIgnored private var tumbleClock: Date = .distantPast
    @ObservationIgnored private var lying: Bool = false
    @ObservationIgnored private var tiltMark: (tilt: CGSize, at: Date)?
    @ObservationIgnored var hidden: Bool = false
    @ObservationIgnored var still: @MainActor () -> Bool = { false }
    @ObservationIgnored var vitality: @MainActor () -> Double = { 0.7 }
    @ObservationIgnored var lean: @MainActor () -> CGSize = { .zero }
    @ObservationIgnored var gravity: @MainActor () -> CGSize = { GenieGravity.down }
    @ObservationIgnored var tilt: @MainActor () -> CGSize = { GenieGravity.down }
    @ObservationIgnored var flat: @MainActor () -> Bool = { false }
    @ObservationIgnored var floats: @MainActor () -> Bool = { false }
    @ObservationIgnored var avoid: @MainActor () -> [CGRect] = { [] }
    @ObservationIgnored var knocked: @MainActor (GenieKnock) -> Void = { _ in }
    @ObservationIgnored var loose: @MainActor (Bool) -> Void = { _ in }

    init(prefs: KeyValueStore? = Services.get(KeyValueStore.self)) {
        self.prefs = prefs
        let x: Double? = prefs?.double(forKey: GenieCrawler.spotXKey)
        let y: Double? = prefs?.double(forKey: GenieCrawler.spotYKey)
        fraction = x.flatMap { (sx: Double) in y.map { CGPoint(x: sx, y: $0) } } ?? GenieCrawler.home
        savedSurface = GenieEdge(rawValue: prefs?.string(forKey: GenieCrawler.surfaceKey) ?? "") ?? .bottom
        parked = GenieEdge(rawValue: prefs?.string(forKey: GenieCrawler.parkedKey) ?? "")
        wanders = prefs?.bool(forKey: GenieCrawler.wandersKey) ?? true
    }

    var ready: Bool { full != .zero }

    var moving: Bool { track != nil || tumbling }

    var arena: GenieArena {
        GenieArena(full: full, ceiling: safe.minY, floor: floor ?? safe.maxY, side: side)
    }

    var roamArea: CGRect { arena.box }

    func layout(full next: CGRect, safe area: CGRect, floor ground: CGFloat?, side size: CGFloat) {
        guard next != full || area != safe || ground != floor || abs(size - side) > 0.5 else { return }
        let first: Bool = full == .zero
        if tumble != nil { spot = point(at: Date()) }
        let wanted: CGPoint = first ? GenieCrawl.point(fraction, in: area) : spot
        let on: GenieEdge? = first ? savedSurface : (track?.surface ?? surface)
        arrival?.cancel()
        stopTumbling()
        track = nil
        full = next
        safe = area
        floor = ground
        side = size
        if let parked = parked {
            spot = GenieCrawl.parked(parked, near: wanted, full: full, safe: safe, side: side)
            return
        }
        if glass {
            spot = GenieCrawl.clamp(wanted, to: arena.box)
            return
        }
        let edge: GenieEdge = on ?? arena.nearest(wanted)
        surface = edge
        turn = edge.degrees
        spot = arena.on(edge, near: wanted)
    }

    func point(at now: Date) -> CGPoint {
        if let tumble = tumble, !held { return tumble.point(after: now.timeIntervalSince(tumbleClock), in: arena) }
        guard let track = track, !held else { return spot }
        return track.point(at: now) ?? spot
    }

    func rotation(at now: Date) -> Double {
        if let parked = parked { return parked.degrees }
        if held { return GenieGravity.upright(gravity()) }
        if let tumble = tumble { return tumble.rotation(after: now.timeIntervalSince(tumbleClock)) }
        guard let track = track else { return turn }
        return track.rotation(at: now) ?? turn
    }

    func stride(at now: Date) -> GenieStride {
        guard !held else { return .still }
        guard let gait = tumble?.gait ?? track?.gait(at: now) else { return .still }
        return GenieStride(phase: gait.phase, walking: gait.walking, facing: gait.facing, air: gait.air, land: gait.land)
    }

    func airborne(at now: Date) -> Bool {
        if let tumble = tumble { return tumble.touching == nil }
        return track?.airborne(at: now) ?? false
    }

    func silk(at now: Date) -> CGPoint? {
        guard !held, tumble == nil else { return nil }
        return track?.silk(at: now)
    }

    func run() async {
        while !Task.isCancelled {
            let pace: Double = 0.55 + 0.45 * min(max(vitality(), 0), 1)
            try? await Task.sleep(for: .seconds(Double.random(in: GenieCrawler.rest) / pace))
            if Task.isCancelled { return }
            wander()
        }
    }

    func wander() {
        guard ready, wanders, parked == nil, !held, track == nil, tumble == nil, !still() else { return }
        guard !UIAccessibility.isReduceMotionEnabled, !UIAccessibility.isVoiceOverRunning else { return }
        if glass {
            let wanted: CGPoint = GenieGlass.wander(from: spot, arena: arena, random: { Double.random(in: 0..<1) })
            roam(to: GenieGlass.place(wanted, arena: arena, side: side, buttons: avoid()))
            return
        }
        guard let on = surface else {
            fall()
            return
        }
        if GenieGravity.lowness(on, gravity: gravity()) < GenieGravity.ceiling, Double.random(in: 0..<1) < GenieCrawler.silkOdds,
            dangle(from: on)
        {
            return
        }
        let (wanted, edge): (CGPoint, GenieEdge) = GenieGravity.wander(
            from: spot, on: on, arena: arena, gravity: gravity(), random: { Double.random(in: 0..<1) })
        let (target, goal): (CGPoint, GenieEdge) = GenieClearance.place(
            wanted, on: edge, arena: arena, side: side, buttons: avoid())
        crawl(to: target, on: goal)
    }

    func keepClear() {
        guard ready, !held, track == nil, tumble == nil, !still() else { return }
        let buttons: [CGRect] = avoid()
        guard !buttons.isEmpty else { return }
        if let parked = parked {
            let next: CGPoint = GenieClearance.tucked(parked, near: spot, full: full, safe: safe, side: side, buttons: buttons)
            guard hypot(next.x - spot.x, next.y - spot.y) > 1 else { return }
            slide(to: next, along: parked)
            return
        }
        if glass {
            roam(to: GenieGlass.place(spot, arena: arena, side: side, buttons: buttons), least: 1)
            return
        }
        guard let on = surface else { return }
        let (target, goal): (CGPoint, GenieEdge) = GenieClearance.place(spot, on: on, arena: arena, side: side, buttons: buttons)
        crawl(to: target, on: goal, least: 1)
    }

    @discardableResult
    func visit(_ target: CGPoint, on goal: GenieEdge) -> Bool {
        guard ready, wanders, parked == nil, !held, track == nil, tumble == nil, !still(), surface != nil else { return false }
        guard !UIAccessibility.isReduceMotionEnabled else { return true }
        crawl(to: target, on: goal, least: 2)
        return true
    }

    @discardableResult
    func visit(_ target: CGPoint) -> Bool {
        guard ready, wanders, glass, parked == nil, !held, track == nil, tumble == nil, !still() else { return false }
        guard !UIAccessibility.isReduceMotionEnabled else { return true }
        roam(to: target, least: 2)
        return true
    }

    func act(_ route: GenieScreenRoute, hold: Double) -> (start: Date, duration: Double)? {
        guard ready, wanders, parked == nil, !held, track == nil, tumble == nil, !still(), route.path.count > 1 else { return nil }
        guard !UIAccessibility.isReduceMotionEnabled, !UIAccessibility.isVoiceOverRunning else { return nil }
        let now: Date = Date()
        let speed: Double = GenieCrawl.speed(vitality: vitality())
        var approach: GenieTrack
        if glass {
            approach = GenieGlass.walk(
                to: route.approach, from: spot, arena: arena, speed: speed, at: now, turn: turn,
                bend: Double.random(in: -GenieGlass.bend...GenieGlass.bend))
        } else {
            let gap: Double = Double(hypot(route.approach.x - spot.x, route.approach.y - spot.y))
            approach = GenieGravity.hops(
                through: [route.approach], from: spot, seconds: max(gap / (speed * GenieCrawler.hopPace), 0.5), at: now, turn: turn)
        }
        if approach.moves.isEmpty { approach = GenieTrack([GenieGlass.stand(at: spot, turn: turn, at: now, seconds: 0.2)]) }
        let walk: GenieMove = GenieGlass.roam(
            route.path, speed: speed * GenieCrawler.actPace, at: approach.end, turn: approach.turn, aim: route.aim, least: 0.8)
        let rest: GenieMove = GenieGlass.stand(at: route.end, turn: walk.turnTo, to: route.rest, at: walk.end, seconds: hold)
        surface = nil
        play(GenieTrack(approach.moves + [walk, rest]))
        return (walk.start, walk.duration)
    }

    @discardableResult
    func blink(to target: CGPoint, on goal: GenieEdge?) -> Bool {
        guard ready, wanders, parked == nil, !held, track == nil, tumble == nil, !still() else { return false }
        guard glass || (surface != nil && goal != nil), !UIAccessibility.isReduceMotionEnabled else { return false }
        arrival?.cancel()
        spot = target
        if !glass, let goal = goal {
            surface = goal
            turn = goal.degrees
        }
        save()
        return true
    }

    var settled: Bool { ready && !held && track == nil && tumble == nil }

    func halt() {
        guard let track = track, !held else { return }
        let now: Date = Date()
        if track.airborne(at: now) || track.sliding(at: now) || track.silk(at: now) != nil { return }
        arrival?.cancel()
        let move: GenieMove? = track.move(at: now)
        spot = track.point(at: now) ?? spot
        turn = track.rotation(at: now) ?? turn
        self.track = nil
        if move?.kind == .roam || move?.kind == .hop {
            surface = nil
            if !glass { drift(from: spot, velocity: .zero) }
            return
        }
        surface = move?.surface
        if surface == nil { fall() }
    }

    func grab() {
        guard !held else { return }
        let now: Date = Date()
        if let track = track {
            spot = track.point(at: now) ?? spot
        }
        if tumble != nil {
            spot = point(at: now)
            stopTumbling()
        }
        arrival?.cancel()
        track = nil
        grabbed = spot
        held = true
        parked = nil
        surface = nil
        glass = false
    }

    func carry(to point: CGPoint) {
        grab()
        spot = GenieCrawl.clamp(point, to: full)
    }

    func drag(by translation: CGSize) {
        grab()
        spot = GenieCrawl.clamp(CGPoint(x: grabbed.x + translation.width, y: grabbed.y + translation.height), to: full)
    }

    @discardableResult
    func release(velocity: CGSize) -> GenieEdge? {
        held = false
        let speed: CGFloat = hypot(velocity.width, velocity.height)
        let reach: CGFloat = side * 0.2
        let edge: GenieEdge? = spot.x < full.minX + reach ? .left : (spot.x > full.maxX - reach ? .right : nil)
        if let edge = edge, speed < GenieCrawler.tuckSpeed {
            parked = edge
            spot = GenieCrawl.parked(edge, near: spot, full: full, safe: safe, side: side)
            save()
            return edge
        }
        if flat() {
            glide(velocity)
            return nil
        }
        throwFrom(spot, velocity: velocity)
        return nil
    }

    @discardableResult
    func drop() -> GenieEdge? {
        release(velocity: .zero)
    }

    func fling(_ velocity: CGSize) {
        held = false
        parked = nil
        if flat() {
            glide(velocity)
            return
        }
        throwFrom(spot, velocity: velocity)
    }

    func stroll(to point: CGPoint) {
        guard ready, !held, tumble == nil else { return }
        halt()
        parked = nil
        guard track == nil else { return }
        if glass {
            roam(to: point, least: 2)
            return
        }
        let goal: GenieEdge = arena.nearest(point)
        crawl(to: arena.on(goal, near: point), on: goal)
    }

    func recoil(from point: CGPoint) {
        guard ready, !held, parked == nil, tumble == nil else { return }
        halt()
        guard track == nil else { return }
        let dx: CGFloat = spot.x - point.x
        let dy: CGFloat = spot.y - point.y
        let length: CGFloat = max(hypot(dx, dy), 1)
        if glass {
            play(GenieGlass.glide(
                from: spot, velocity: CGSize(width: dx / length * 520, height: dy / length * 520), arena: arena, at: Date(), turn: turn))
            return
        }
        let up: CGSize = GenieGravity.unit(gravity())
        throwFrom(spot, velocity: CGSize(width: dx / length * 520 - up.width * 620, height: dy / length * 520 - up.height * 620))
    }

    func tour(_ points: [CGPoint], seconds: Double) {
        guard ready, !held, parked == nil, tumble == nil, !points.isEmpty else { return }
        halt()
        let now: Date = Date()
        let hops: GenieTrack = GenieGravity.hops(through: points, from: point(at: now), seconds: seconds, at: now, turn: turn)
        guard let last = hops.final else { return }
        if glass {
            play(hops.then(GenieTrack([GenieGlass.stand(at: last, turn: turn, at: now)])))
            return
        }
        let landing: GenieTrack = arena.touches(last, arena.nearest(last))
            ? GenieTrack([
                GenieMove(
                    kind: .crawl, from: last, to: last, surface: arena.nearest(last), start: now, duration: 0.3, turnFrom: turn,
                    turnTo: arena.nearest(last).degrees)
            ])
            : GenieGravity.fall(from: last, on: nil, gravity: gravity(), arena: arena, at: now, turn: turn)
        play(hops.then(landing))
    }

    func shakeLoose() {
        guard ready, !held, parked == nil, track == nil, tumble == nil, let on = surface else { return }
        guard GenieGravity.lowness(on, gravity: gravity()) < 0.6 else { return }
        fall()
    }

    func park(_ edge: GenieEdge? = nil) {
        halt()
        if tumble != nil {
            spot = point(at: Date())
            stopTumbling()
        }
        arrival?.cancel()
        track = nil
        glass = false
        parked = edge ?? GenieCrawl.edge(near: spot, in: full, margin: .greatestFiniteMagnitude) ?? .right
        if let parked = parked { spot = GenieCrawl.parked(parked, near: spot, full: full, safe: safe, side: side) }
        save()
    }

    func unpark() {
        guard let edge = parked else { return }
        parked = nil
        let step: CGFloat = side * 0.9
        let out: CGPoint
        switch edge {
        case .left: out = CGPoint(x: spot.x + step, y: spot.y)
        case .right: out = CGPoint(x: spot.x - step, y: spot.y)
        case .top: out = CGPoint(x: spot.x, y: spot.y + step)
        case .bottom: out = CGPoint(x: spot.x, y: spot.y - step)
        }
        spot = GenieCrawl.clamp(out, to: arena.box)
        surface = nil
        turn = edge.degrees
        if flat() {
            stepOnGlass()
            return
        }
        fall()
    }

    func setWanders(_ on: Bool) {
        wanders = on
        prefs?.set(on, forKey: GenieCrawler.wandersKey)
        if !on { halt() }
    }

    func reset() {
        arrival?.cancel()
        stopTumbling()
        track = nil
        held = false
        parked = nil
        glass = false
        surface = .bottom
        turn = 0
        spot = arena.on(.bottom, near: GenieCrawl.point(GenieCrawler.home, in: safe))
        save()
        if flat() { stepOnGlass() }
    }

    func jostle(_ push: CGSize) {
        let now: Date = Date()
        let swirled: Bool = sense(now)
        guard var live = tumble else {
            if GenieTumble.loosens(push) || swirled { shake(push, at: now) }
            return
        }
        guard ready, !held else {
            stopTumbling()
            return
        }
        guard !hidden else {
            calmDown(live)
            return
        }
        let seconds: Double = min(max(now.timeIntervalSince(tumbleClock), 0), 0.1)
        tumbleClock = now
        let knocks: [GenieKnock] = live.advance(by: seconds, push: push, gravity: tilt(), arena: arena)
        tumble = live
        for knock in knocks { knocked(knock) }
        if live.done { calmDown(live) }
    }

    private func shake(_ push: CGSize, at now: Date) {
        guard ready, !held, !hidden, parked == nil, !still() else { return }
        guard !UIAccessibility.isReduceMotionEnabled, !UIAccessibility.isVoiceOverRunning else { return }
        var from: CGPoint = spot
        var velocity: CGSize = .zero
        var facing: Double = turn
        var touching: GenieEdge? = surface
        if let track = track {
            let step: Double = 1.0 / 60.0
            from = track.point(at: now) ?? spot
            let before: CGPoint = track.point(at: now.addingTimeInterval(-step)) ?? from
            velocity = CGSize(width: (from.x - before.x) / step, height: (from.y - before.y) / step)
            facing = track.rotation(at: now) ?? turn
            touching = track.airborne(at: now) || track.silk(at: now) != nil ? nil : track.move(at: now)?.surface
        }
        arrival?.cancel()
        track = nil
        let start: CGPoint = GenieCrawl.clamp(from, to: arena.box)
        if let on = touching, !arena.touches(start, on) { touching = nil }
        begin(GenieTumble(point: start, velocity: velocity, turn: facing, touching: touching, gravity: tilt()), at: now)
        jostle(push)
    }

    private func begin(_ next: GenieTumble, at now: Date) {
        tumble = next
        tumbleClock = now
        surface = nil
        glass = false
        tumbling = true
        loose(true)
    }

    private func sense(_ now: Date) -> Bool {
        let tilted: CGSize = tilt()
        var swirled: Bool = false
        if let mark = tiltMark {
            let seconds: Double = now.timeIntervalSince(mark.at)
            if seconds >= GenieCrawler.swirlWindow {
                swirled = !lying && GenieTumble.swirls(from: mark.tilt, to: tilted, seconds: seconds)
                tiltMark = (tilted, now)
            }
        } else {
            tiltMark = (tilted, now)
        }
        let lies: Bool = flat()
        guard ready, lies != lying else { return swirled }
        lying = lies
        if lies {
            enterGlass()
        } else {
            leaveGlass()
        }
        return false
    }

    private func enterGlass() {
        guard ready, parked == nil, !held, tumble == nil, !glass else { return }
        if let track = track {
            let now: Date = Date()
            if track.airborne(at: now) || track.sliding(at: now) || track.silk(at: now) != nil { return }
            halt()
            guard self.track == nil, tumble == nil else { return }
        }
        stepOnGlass()
    }

    private func leaveGlass() {
        guard glass else { return }
        glass = false
        guard ready, !held, parked == nil, tumble == nil else { return }
        let now: Date = Date()
        if let track = track {
            spot = track.point(at: now) ?? spot
            turn = track.rotation(at: now) ?? turn
        }
        arrival?.cancel()
        track = nil
        drift(from: spot, velocity: .zero)
    }

    private func stepOnGlass() {
        glass = true
        surface = nil
        guard wanders, !hidden, !still(), !UIAccessibility.isReduceMotionEnabled, !UIAccessibility.isVoiceOverRunning else {
            save()
            return
        }
        roam(to: GenieGlass.place(GenieGlass.inward(from: spot, arena: arena, side: side), arena: arena, side: side, buttons: avoid()))
    }

    private func drift(from point: CGPoint, velocity: CGSize) {
        surface = nil
        guard ready, floats(), !hidden, !UIAccessibility.isReduceMotionEnabled, !UIAccessibility.isVoiceOverRunning else {
            throwFrom(point, velocity: velocity)
            return
        }
        arrival?.cancel()
        track = nil
        begin(
            GenieTumble(point: GenieCrawl.clamp(point, to: arena.box), velocity: velocity, turn: turn, touching: nil, gravity: tilt()),
            at: Date())
    }

    private func glide(_ velocity: CGSize) {
        glass = true
        surface = nil
        play(GenieGlass.glide(from: spot, velocity: velocity, arena: arena, at: Date(), turn: turn))
    }

    private func roam(to target: CGPoint, least: CGFloat = 12) {
        guard hypot(target.x - spot.x, target.y - spot.y) > least else { return }
        play(GenieGlass.walk(
            to: target, from: spot, arena: arena, speed: GenieCrawl.speed(vitality: vitality()), at: Date(), turn: turn,
            bend: Double.random(in: -GenieGlass.bend...GenieGlass.bend)))
    }

    private func calmDown(_ done: GenieTumble) {
        let now: Date = Date()
        stopTumbling()
        spot = done.point
        turn = done.turn
        if flat(), parked == nil {
            glass = true
            surface = nil
            play(GenieTrack([GenieGlass.stand(at: spot, turn: turn, at: now)]))
            return
        }
        if let on = done.touching, done.speed < GenieGravity.rest * 3 {
            let rest: CGPoint = arena.on(on, near: spot)
            play(GenieTrack([
                GenieMove(kind: .crawl, from: rest, to: rest, surface: on, start: now, duration: 0.3, turnFrom: turn, turnTo: on.degrees)
            ]))
            return
        }
        play(GenieGravity.fly(from: spot, velocity: done.velocity, gravity: gravity(), arena: arena, at: now, turn: turn))
    }

    private func stopTumbling() {
        guard tumble != nil || tumbling else { return }
        tumble = nil
        tumbling = false
        loose(false)
    }

    private func fall() {
        let now: Date = Date()
        play(GenieGravity.fall(from: spot, on: surface, gravity: gravity(), arena: arena, at: now, turn: turn))
    }

    private func throwFrom(_ point: CGPoint, velocity: CGSize) {
        guard ready else { return }
        if surface == nil { turn = GenieGravity.upright(gravity()) }
        surface = nil
        let now: Date = Date()
        play(GenieGravity.fly(from: point, velocity: velocity, gravity: gravity(), arena: arena, at: now, turn: turn))
    }

    private func crawl(to target: CGPoint, on goal: GenieEdge, least: CGFloat = 12) {
        guard let on = surface else { return }
        guard hypot(target.x - spot.x, target.y - spot.y) > least || goal != on else { return }
        let now: Date = Date()
        play(GenieGravity.crawl(
            from: spot, on: on, to: target, on: goal, arena: arena, gravity: gravity(), speed: GenieCrawl.speed(vitality: vitality()),
            at: now, turn: turn))
    }

    private func dangle(from on: GenieEdge) -> Bool {
        guard let silk = GenieGravity.silk(
            from: spot, on: on, arena: arena, gravity: gravity(), depth: Double.random(in: GenieCrawler.silkDepth),
            hang: Double.random(in: GenieCrawler.silkHang), down: Double.random(in: 0..<1) < GenieCrawler.silkToFloor,
            speed: GenieCrawl.speed(vitality: vitality()), at: Date(), turn: turn)
        else { return false }
        play(silk)
        return true
    }

    private func slide(to point: CGPoint, along edge: GenieEdge) {
        let length: Double = Double(hypot(point.x - spot.x, point.y - spot.y))
        let move: GenieMove = GenieMove(
            kind: .crawl, from: spot, to: point, surface: edge, start: Date(),
            duration: max(length / GenieCrawl.speed(vitality: vitality()), 0.3), turnFrom: edge.degrees, turnTo: edge.degrees)
        play(GenieTrack([move]))
    }

    private func play(_ next: GenieTrack) {
        arrival?.cancel()
        stopTumbling()
        guard !next.moves.isEmpty else {
            track = nil
            return
        }
        if UIAccessibility.isReduceMotionEnabled {
            settle(next)
            return
        }
        track = next
        arrival = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(max(next.end.timeIntervalSinceNow, 0)))
            guard !Task.isCancelled, let self = self, self.track == next else { return }
            self.settle(next)
        }
    }

    private func settle(_ done: GenieTrack) {
        spot = done.final ?? spot
        turn = done.turn
        track = nil
        if done.moves.last?.kind == .roam {
            surface = nil
            if flat(), parked == nil {
                glass = true
                save()
                keepClear()
                return
            }
            glass = false
            save()
            drift(from: spot, velocity: .zero)
            return
        }
        glass = false
        surface = done.surface ?? arena.nearest(spot)
        save()
        if flat(), parked == nil, !held {
            stepOnGlass()
            return
        }
        keepClear()
    }

    private func save() {
        guard ready else { return }
        fraction = GenieCrawl.fraction(spot, in: safe)
        prefs?.set(Double(fraction.x), forKey: GenieCrawler.spotXKey)
        prefs?.set(Double(fraction.y), forKey: GenieCrawler.spotYKey)
        prefs?.set(surface?.rawValue ?? "", forKey: GenieCrawler.surfaceKey)
        prefs?.set(parked?.rawValue ?? "", forKey: GenieCrawler.parkedKey)
    }
}
