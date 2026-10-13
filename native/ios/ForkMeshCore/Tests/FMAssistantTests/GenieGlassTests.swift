import Foundation
import XCTest
@testable import FMAssistant

final class GenieGlassTests: XCTestCase {
    private let full = CGRect(x: 0, y: 0, width: 400, height: 900)
    private var arena: GenieArena { GenieArena(full: full, ceiling: 50, floor: 800, side: 100) }
    private let t0 = Date(timeIntervalSinceReferenceDate: 1000)

    func testAPhoneLyingFaceUpIsFlatWithSomeSlack() {
        XCTAssertTrue(GenieGlass.flat(plane: CGSize(width: 0.1, height: 0.1), z: -0.98, was: false))
        XCTAssertFalse(GenieGlass.flat(plane: CGSize(width: 0.35, height: 0), z: -0.93, was: false))
        XCTAssertTrue(GenieGlass.flat(plane: CGSize(width: 0.35, height: 0), z: -0.93, was: true), "it stays flat until tilted further")
        XCTAssertFalse(GenieGlass.flat(plane: CGSize(width: 0.6, height: 0), z: -0.8, was: true))
        XCTAssertFalse(GenieGlass.flat(plane: CGSize(width: 0, height: 0.1), z: 0.99, was: false), "face down is not a table top")
    }

    func testHeadingPointsTheHeadWhereItWalks() {
        XCTAssertEqual(GenieGlass.heading(dx: 0, dy: -1), 0, accuracy: 0.001)
        XCTAssertEqual(GenieGlass.heading(dx: 1, dy: 0), 90, accuracy: 0.001)
        XCTAssertEqual(GenieGlass.heading(dx: -1, dy: 0), -90, accuracy: 0.001)
        XCTAssertEqual(abs(GenieGlass.heading(dx: 0, dy: 1)), 180, accuracy: 0.001)
        XCTAssertEqual(GenieGlass.heading(dx: 1, dy: 0), GenieEdge.left.degrees, "a left-wall spider's head points right too")
    }

    func testPathsAreEvenlySpaced() {
        let path = GenieGlass.even([CGPoint(x: 0, y: 0), CGPoint(x: 100, y: 0), CGPoint(x: 100, y: 37)])
        XCTAssertEqual(path.first, CGPoint(x: 0, y: 0))
        XCTAssertEqual(path.last!.x, 100, accuracy: 0.001)
        XCTAssertEqual(path.last!.y, 37, accuracy: 0.001)
        let gaps = zip(path, path.dropFirst()).map { hypot($1.x - $0.x, $1.y - $0.y) }
        XCTAssertEqual(gaps.max()! - gaps.min()!, 0, accuracy: 3)
    }

    func testItWalksAcrossTheGlassOnACurveFacingTheWayItGoes() {
        let from = CGPoint(x: 100, y: 400)
        let to = CGPoint(x: 300, y: 400)
        let track = GenieGlass.walk(to: to, from: from, arena: arena, speed: 80, at: t0, turn: 0, bend: 0.25)
        XCTAssertEqual(track.moves.count, 1)
        let move = track.moves[0]
        XCTAssertEqual(move.kind, .roam)
        XCTAssertNil(track.surface)
        XCTAssertEqual(track.point(at: t0)!, from)
        XCTAssertEqual(track.point(at: track.end)!.x, to.x, accuracy: 0.001)
        XCTAssertEqual(track.point(at: track.end)!.y, to.y, accuracy: 0.001)
        let middle = t0.addingTimeInterval(track.duration / 2)
        XCTAssertGreaterThan(abs(track.point(at: middle)!.y - 400), 10, "it bends")
        XCTAssertEqual(track.rotation(at: middle)!, 90, accuracy: 25)
        XCTAssertGreaterThan(track.gait(at: middle).walking, 0.5)
        XCTAssertEqual(track.gait(at: middle).air, 0)
        XCTAssertEqual(track.turn, track.rotation(at: track.end)!, accuracy: 0.001)
        for k in 0...20 {
            let p = track.point(at: t0.addingTimeInterval(track.duration * Double(k) / 20))!
            XCTAssertTrue(arena.box.insetBy(dx: -0.001, dy: -0.001).contains(p))
        }
    }

    func testAStandTurnsToFaceAndStaysPut() {
        let move = GenieGlass.stand(at: CGPoint(x: 200, y: 300), turn: 90, to: 0, at: t0, seconds: 1)
        XCTAssertEqual(move.point(at: t0.addingTimeInterval(0.5)), CGPoint(x: 200, y: 300))
        XCTAssertEqual(move.rotation(at: t0.addingTimeInterval(1)), 0, accuracy: 0.001)
        XCTAssertEqual(move.gait(at: t0.addingTimeInterval(0.5)).walking, 0)
    }

    func testAFlickGlidesItAcrossTheTableAndItStops() {
        let start = CGPoint(x: 200, y: 400)
        let track = GenieGlass.glide(from: start, velocity: CGSize(width: 900, height: -300), arena: arena, at: t0, turn: 0)
        XCTAssertEqual(track.moves.first?.kind, .slide)
        XCTAssertEqual(track.moves.last?.kind, .roam)
        XCTAssertNil(track.surface)
        let end = track.final!
        XCTAssertGreaterThan(end.x, start.x + 50)
        XCTAssertTrue(arena.box.insetBy(dx: -0.001, dy: -0.001).contains(end))
        let still = GenieGlass.glide(from: start, velocity: .zero, arena: arena, at: t0, turn: 0)
        XCTAssertEqual(still.final, start)
    }

    func testWanderingGoesAnywhereOnTheScreen() {
        var seed: UInt64 = 7
        func dice() -> Double {
            seed = seed &* 6364136223846793005 &+ 1442695040888963407
            return Double(seed >> 11) / Double(UInt64(1) << 53)
        }
        var middle = 0
        let start = CGPoint(x: 200, y: 400)
        let diagonal = Double(hypot(arena.box.width, arena.box.height))
        for _ in 0..<200 {
            let target = GenieGlass.wander(from: start, arena: arena, random: dice)
            XCTAssertTrue(arena.box.insetBy(dx: -0.001, dy: -0.001).contains(target))
            XCTAssertLessThanOrEqual(Double(hypot(target.x - start.x, target.y - start.y)), diagonal * GenieGlass.near.upperBound + 0.001)
            if arena.box.insetBy(dx: 40, dy: 40).contains(target) { middle += 1 }
        }
        XCTAssertGreaterThan(middle, 100, "most spots are away from the edges")
    }

    func testItStepsOffTheEdgeOntoTheGlass() {
        let spot = arena.on(.bottom, near: CGPoint(x: 200, y: 900))
        let next = GenieGlass.inward(from: spot, arena: arena, side: 100)
        XCTAssertEqual(next.x, spot.x, accuracy: 0.001)
        XCTAssertEqual(next.y, spot.y - 90, accuracy: 0.001)
    }

    func testOnTheGlassItKeepsOffButtons() {
        let button = CGRect(x: 150, y: 350, width: 100, height: 100)
        let spot = CGPoint(x: 200, y: 400)
        XCTAssertTrue(GenieClearance.covers(spot, side: 100, buttons: [button]))
        let moved = GenieGlass.place(spot, arena: arena, side: 100, buttons: [button])
        XCTAssertFalse(GenieClearance.covers(moved, side: 100, buttons: [button]))
        XCTAssertLessThan(hypot(moved.x - spot.x, moved.y - spot.y), 120, "it steps aside, not across the screen")
        XCTAssertEqual(GenieGlass.place(CGPoint(x: 100, y: 700), arena: arena, side: 100, buttons: [button]), CGPoint(x: 100, y: 700))
    }
}
