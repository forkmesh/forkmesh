import Foundation
import XCTest
@testable import FMAssistant

final class GenieGravityTests: XCTestCase {
    private let start = Date(timeIntervalSince1970: 1_790_000_000)
    private let full = CGRect(x: 0, y: 0, width: 400, height: 900)
    private var arena: GenieArena { GenieArena(full: full, ceiling: 50, floor: 800, side: 100) }

    func testArenaStandsOnTheBarAndUnderTheTop() {
        XCTAssertEqual(arena.box.minX, 42, accuracy: 0.001)
        XCTAssertEqual(arena.box.maxX, 358, accuracy: 0.001)
        XCTAssertEqual(arena.box.minY, 92, accuracy: 0.001)
        XCTAssertEqual(arena.box.maxY, 758, accuracy: 0.001)
        XCTAssertEqual(arena.on(.bottom, near: CGPoint(x: 200, y: 300)), CGPoint(x: 200, y: 758))
        XCTAssertEqual(arena.nearest(CGPoint(x: 50, y: 400)), .left)
        XCTAssertEqual(GenieGravity.floor(CGSize(width: 0, height: 1)), .bottom)
        XCTAssertEqual(GenieGravity.floor(CGSize(width: -0.9, height: 0.2)), .left)
        XCTAssertEqual(GenieGravity.floor(.zero), .bottom)
        XCTAssertEqual(GenieGravity.upright(CGSize(width: 0, height: 1)), 0, accuracy: 0.001)
        XCTAssertEqual(GenieGravity.upright(CGSize(width: -1, height: 0)), GenieEdge.left.degrees, accuracy: 0.001)
        XCTAssertEqual(GenieGravity.turn(from: 170, to: -90), 100, accuracy: 0.001)
    }

    func testADroppedSpiderFallsBouncesAndLandsOnTheFloor() {
        let track = GenieGravity.fly(
            from: CGPoint(x: 200, y: 200), velocity: .zero, gravity: CGSize(width: 0, height: 1), arena: arena, at: start, turn: 0)
        XCTAssertEqual(track.surface, .bottom)
        XCTAssertEqual(track.final?.y ?? 0, 758, accuracy: 0.001)
        XCTAssertTrue(track.flying)
        XCTAssertTrue(track.airborne(at: start.addingTimeInterval(0.1)))
        let path = track.moves[0].path
        let lowest = path.map(\.y).max() ?? 0
        XCTAssertEqual(lowest, 758, accuracy: 0.001)
        let firstFloor = path.firstIndex { $0.y >= 757.9 } ?? 0
        XCTAssertLessThan(path[firstFloor...].map(\.y).min() ?? 758, 740, "it bounces before it settles")
        XCTAssertEqual(track.moves.last?.kind, .land)
        XCTAssertGreaterThan(track.gait(at: track.moves[1].start.addingTimeInterval(GenieGravity.landing / 2)).land, 0.9)
        let midFall = track.point(at: start.addingTimeInterval(0.2))?.y ?? 0
        XCTAssertGreaterThan(midFall, 200)
    }

    func testFlightGravityApproachesTerminalSpeedAndSlowsAFlick() {
        var point = CGPoint.zero
        var velocity = CGSize.zero
        for _ in 0..<1200 {
            let next = GenieGravity.flightStep(point, velocity: velocity, gravity: GenieGravity.down)
            point = next.point
            velocity = next.velocity
        }
        XCTAssertGreaterThan(velocity.height, 1800)
        XCTAssertLessThan(velocity.height, 2200)
        XCTAssertLessThanOrEqual(hypot(velocity.width, velocity.height), GenieGravity.maxSpeed)
        let capped = GenieGravity.flightStep(.zero, velocity: CGSize(width: 0, height: -20_000), gravity: GenieGravity.down)
        XCTAssertLessThanOrEqual(hypot(capped.velocity.width, capped.velocity.height), GenieGravity.maxSpeed)
        XCTAssertLessThanOrEqual(abs(capped.point.y), CGFloat(GenieGravity.maxSpeed * GenieGravity.step))

        point = .zero
        velocity = CGSize(width: 3200, height: -400)
        for _ in 0..<120 {
            let next = GenieGravity.flightStep(point, velocity: velocity, gravity: GenieGravity.down)
            point = next.point
            velocity = next.velocity
        }
        XCTAssertGreaterThan(point.x, 1000)
        XCTAssertGreaterThan(velocity.width, 0)
        XCTAssertLessThan(velocity.width, 1800)
    }

    func testAFlickSidewaysBouncesOffAWallButAGentleOneClings() {
        let hard = GenieGravity.fly(
            from: CGPoint(x: 200, y: 400), velocity: CGSize(width: -3000, height: 0), gravity: GenieGravity.down, arena: arena, at: start, turn: 0)
        let hardPath = hard.moves[0].path
        let wall = hardPath.firstIndex { $0.x <= 42.01 }
        XCTAssertNotNil(wall, "it reaches the left wall")
        XCTAssertGreaterThan(hardPath[(wall ?? 0)...].map(\.x).max() ?? 0, 300, "and bounces off it")
        let gentle = GenieGravity.fly(
            from: CGPoint(x: 120, y: 400), velocity: CGSize(width: -900, height: -200), gravity: GenieGravity.down, arena: arena, at: start, turn: 0)
        XCTAssertEqual(gentle.surface, .left)
        XCTAssertEqual(gentle.turn, GenieEdge.left.degrees)
        let upward = GenieGravity.fly(
            from: CGPoint(x: 200, y: 300), velocity: CGSize(width: 0, height: -1600), gravity: GenieGravity.down, arena: arena, at: start, turn: 0)
        XCTAssertEqual(upward.surface, .top)
    }

    func testAThrowKeepsItsMomentumAndSkidsAlongTheFloor() {
        let track = GenieGravity.fly(
            from: CGPoint(x: 60, y: 700), velocity: CGSize(width: 400, height: 0), gravity: GenieGravity.down, arena: arena, at: start, turn: 0)
        XCTAssertEqual(track.moves.map(\.kind), [.fly, .slide])
        XCTAssertEqual(track.surface, .bottom)
        let flight = track.moves[0]
        let slide = track.moves[1]
        XCTAssertGreaterThan(flight.to.x, 180, "it keeps flying sideways after the first bounce")
        XCTAssertGreaterThan(slide.to.x, slide.from.x + 15, "it skids on after landing")
        XCTAssertEqual(track.final?.y ?? 0, 758, accuracy: 0.001)
        XCTAssertTrue(slide.path.allSatisfy { abs($0.y - 758) < 0.001 })
        let mid = slide.start.addingTimeInterval(slide.duration / 2)
        XCTAssertFalse(track.airborne(at: mid))
        XCTAssertTrue(track.sliding(at: mid))
        XCTAssertEqual(track.gait(at: mid).facing, 1)
        XCTAssertGreaterThan(track.gait(at: slide.start.addingTimeInterval(GenieGravity.landing / 2)).land, 0.9)
        XCTAssertFalse(track.sliding(at: track.end.addingTimeInterval(0.1)))
    }

    func testAFastFlickIsCappedAtTheTopSpeed() {
        let track = GenieGravity.fly(
            from: CGPoint(x: 200, y: 400), velocity: CGSize(width: 0, height: -20000), gravity: GenieGravity.down, arena: arena, at: start, turn: 0)
        let path = track.moves[0].path
        let first = Double(path[0].y - path[1].y) / GenieGravity.step
        XCTAssertLessThanOrEqual(first, GenieGravity.maxSpeed)
        XCTAssertGreaterThan(first, GenieGravity.maxSpeed - 40)
    }

    func testGravityFromTheTiltDecidesWhereItFalls() {
        let sideways = GenieGravity.fly(
            from: CGPoint(x: 200, y: 400), velocity: .zero, gravity: CGSize(width: 1, height: 0), arena: arena, at: start, turn: 0)
        XCTAssertEqual(sideways.surface, .right)
        let upsideDown = GenieGravity.fall(
            from: CGPoint(x: 200, y: 758), on: .bottom, gravity: CGSize(width: 0, height: -1), arena: arena, at: start, turn: 0)
        XCTAssertEqual(upsideDown.surface, .top)
    }

    func testClimbingToTheTopGoesUpAWallAndIsSlowerThanComingDown() {
        let up = GenieGravity.crawl(
            from: CGPoint(x: 300, y: 758), on: .bottom, to: CGPoint(x: 200, y: 92), on: .top, arena: arena, gravity: GenieGravity.down,
            speed: 80, at: start, turn: 0)
        XCTAssertEqual(up.moves.map(\.surface), [.bottom, .right, .top])
        XCTAssertEqual(up.final, CGPoint(x: 200, y: 92))
        XCTAssertEqual(up.turn, GenieEdge.top.degrees)
        let down = GenieGravity.crawl(
            from: CGPoint(x: 200, y: 92), on: .top, to: CGPoint(x: 300, y: 758), on: .bottom, arena: arena, gravity: GenieGravity.down,
            speed: 80, at: start, turn: 180)
        XCTAssertGreaterThan(up.duration, down.duration * 1.3)
        let climbing = up.moves[1]
        let halfway = climbing.start.addingTimeInterval(climbing.duration / 2)
        XCTAssertEqual(up.rotation(at: halfway) ?? 0, GenieEdge.right.degrees, accuracy: 0.001)
        XCTAssertEqual(up.point(at: halfway)?.x ?? 0, 358, accuracy: 0.001)
        XCTAssertGreaterThan(up.gait(at: halfway).walking, 0.9)
        let along = GenieGravity.crawl(
            from: CGPoint(x: 100, y: 758), on: .bottom, to: CGPoint(x: 300, y: 758), on: .bottom, arena: arena, gravity: GenieGravity.down,
            speed: 80, at: start, turn: 0)
        XCTAssertEqual(along.moves.count, 1)
        XCTAssertEqual(along.gait(at: start.addingTimeInterval(along.duration / 2)).facing, 1)
    }

    func testFromTheCeilingItLowersOnSilkHangsAndClimbsBack() {
        let track = GenieGravity.silk(
            from: CGPoint(x: 200, y: 92), on: .top, arena: arena, gravity: GenieGravity.down, depth: 0.5, hang: 2, down: false, speed: 80,
            at: start, turn: 180)
        guard let track = track else { return XCTFail("a spider on the ceiling can lower itself") }
        let anchor = CGPoint(x: 200, y: 92)
        XCTAssertEqual(track.moves.map(\.kind), [.silk, .silk, .silk, .crawl])
        XCTAssertEqual(track.moves[0].to.y, 92 + 666 * 0.5, accuracy: 0.001)
        XCTAssertEqual(track.moves[0].to.x, 200, accuracy: 0.001)
        XCTAssertEqual(track.final, anchor)
        XCTAssertEqual(track.surface, .top)
        XCTAssertEqual(track.turn, GenieEdge.top.degrees)
        XCTAssertFalse(track.flying)
        let lowering = track.moves[0]
        XCTAssertEqual(track.silk(at: lowering.start.addingTimeInterval(lowering.duration / 2)), anchor)
        XCTAssertEqual(track.rotation(at: lowering.end.addingTimeInterval(-0.01)) ?? 180, 0, accuracy: 0.5, "it hangs upright")
        XCTAssertGreaterThan(track.gait(at: lowering.start.addingTimeInterval(lowering.duration / 2)).air, 0)
        let hang = track.moves[1]
        XCTAssertTrue(hang.hanging)
        let swinging = hang.start.addingTimeInterval(hang.duration * 0.4)
        let swung = track.point(at: swinging) ?? anchor
        XCTAssertEqual(hypot(swung.x - anchor.x, swung.y - anchor.y), 333, accuracy: 0.01, "it swings on a fixed length of silk")
        XCTAssertNotEqual(swung.x, 200, accuracy: 0.5)
        XCTAssertEqual(track.point(at: hang.end)?.x ?? 0, 200, accuracy: 0.001, "the swing settles before it climbs")
        let climb = track.moves[2]
        XCTAssertGreaterThan(climb.duration, lowering.duration, "climbing the silk is slower than lowering")
        XCTAssertGreaterThan(track.gait(at: climb.start.addingTimeInterval(climb.duration / 2)).walking, 0.9)
        XCTAssertNil(track.silk(at: track.moves[3].start.addingTimeInterval(0.1)))
        XCTAssertNil(track.silk(at: track.end.addingTimeInterval(1)))
    }

    func testASilkDropCanReachTheFloorAndOnlyStartsFromTheCeiling() {
        let track = GenieGravity.silk(
            from: CGPoint(x: 120, y: 92), on: .top, arena: arena, gravity: GenieGravity.down, depth: 0.4, hang: 1, down: true, speed: 80,
            at: start, turn: 180)
        XCTAssertEqual(track?.moves.map(\.kind), [.silk, .silk, .silk, .land])
        XCTAssertEqual(track?.final, CGPoint(x: 120, y: 758))
        XCTAssertEqual(track?.surface, .bottom)
        XCTAssertNotNil(track?.silk(at: track?.moves[2].start.addingTimeInterval(0.1) ?? start))
        XCTAssertNil(track?.silk(at: track?.moves[3].start.addingTimeInterval(0.05) ?? start), "the silk is let go on landing")
        XCTAssertNil(GenieGravity.silk(
            from: CGPoint(x: 200, y: 758), on: .bottom, arena: arena, gravity: GenieGravity.down, depth: 0.5, hang: 1, down: false, speed: 80,
            at: start, turn: 0))
        XCTAssertNil(GenieGravity.silk(
            from: CGPoint(x: 42, y: 400), on: .left, arena: arena, gravity: GenieGravity.down, depth: 0.5, hang: 1, down: false, speed: 80,
            at: start, turn: 90))
        let sideways = GenieGravity.silk(
            from: CGPoint(x: 42, y: 400), on: .left, arena: arena, gravity: CGSize(width: 1, height: 0), depth: 0.5, hang: 1, down: true,
            speed: 80, at: start, turn: 90)
        XCTAssertEqual(sideways?.surface, .right, "with the phone on its side the left edge is the ceiling")
    }

    func testWanderStaysOnSurfacesAndTracksChain() {
        var seed: UInt64 = 7
        let random: () -> Double = {
            seed = seed &* 6364136223846793005 &+ 1442695040888963407
            return Double(seed >> 11) / Double(1 << 53)
        }
        var surfaces: Set<GenieEdge> = []
        for _ in 0..<200 {
            let (point, surface) = GenieGravity.wander(from: CGPoint(x: 200, y: 758), on: .bottom, arena: arena, gravity: GenieGravity.down, random: random)
            XCTAssertTrue(arena.touches(point, surface), "\(point) on \(surface)")
            surfaces.insert(surface)
        }
        XCTAssertEqual(surfaces, Set(GenieEdge.allCases))
        let hop = GenieGravity.hops(through: [CGPoint(x: 150, y: 300)], from: CGPoint(x: 200, y: 758), seconds: 1, at: start, turn: 0)
        let fall = GenieGravity.fly(from: CGPoint(x: 150, y: 300), velocity: .zero, gravity: GenieGravity.down, arena: arena, at: start, turn: 0)
        let chained = hop.then(fall)
        XCTAssertEqual(chained.moves[1].start, hop.end)
        XCTAssertEqual(chained.surface, .bottom)
        XCTAssertEqual(chained.end.timeIntervalSince(start), hop.duration + fall.duration, accuracy: 0.001)
    }
}
