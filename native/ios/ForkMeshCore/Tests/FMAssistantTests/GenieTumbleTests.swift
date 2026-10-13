import Foundation
import XCTest
@testable import FMAssistant

final class GenieTumbleTests: XCTestCase {
    private let full = CGRect(x: 0, y: 0, width: 400, height: 900)
    private var arena: GenieArena { GenieArena(full: full, ceiling: 50, floor: 800, side: 100) }

    private func standing(x: CGFloat = 200) -> GenieTumble {
        GenieTumble(point: CGPoint(x: x, y: 758), velocity: .zero, turn: 0, touching: .bottom)
    }

    private func run(_ tumble: inout GenieTumble, push: CGSize, seconds: Double, gravity: CGSize = GenieGravity.down) -> [GenieKnock] {
        var knocks: [GenieKnock] = []
        var left: Double = seconds
        while left > 1e-9 {
            let dt: Double = min(left, 1.0 / 30.0)
            knocks += tumble.advance(by: dt, push: push, gravity: gravity, arena: arena)
            left -= dt
        }
        return knocks
    }

    func testOnlyARealJoltShakesItLoose() {
        XCTAssertFalse(GenieTumble.loosens(CGSize(width: 0.1, height: 0.1)))
        XCTAssertFalse(GenieTumble.loosens(CGSize(width: 0, height: -0.2)))
        XCTAssertTrue(GenieTumble.loosens(CGSize(width: 0.3, height: 0)))
        XCTAssertTrue(GenieTumble.loosens(CGSize(width: 0.2, height: 0.2)))
    }

    func testMovingThePhoneDownLiftsItAndItFloatsBackDown() {
        var tumble = standing()
        _ = run(&tumble, push: CGSize(width: 0, height: 0.7), seconds: 0.4)
        XCTAssertNil(tumble.touching, "it floats off the floor")
        XCTAssertLessThan(tumble.point.y, 758 - 20)
        XCTAssertLessThan(tumble.velocity.height, 0)
        XCTAssertGreaterThan(tumble.gait.air, 0.9)
        _ = run(&tumble, push: .zero, seconds: 5)
        XCTAssertEqual(tumble.touching, .bottom)
        XCTAssertEqual(tumble.point.y, 758, accuracy: 0.001)
        XCTAssertEqual(tumble.turn, 0, accuracy: 2)
        XCTAssertTrue(tumble.done)
    }

    func testMovingThePhoneUpPressesItIntoTheFloor() {
        var tumble = standing()
        let knocks = run(&tumble, push: CGSize(width: 0, height: -0.6), seconds: 0.5)
        XCTAssertTrue(knocks.isEmpty)
        XCTAssertEqual(tumble.touching, .bottom)
        XCTAssertEqual(tumble.point.y, 758, accuracy: 0.001)
        XCTAssertEqual(tumble.point.x, 200, accuracy: 2)
    }

    func testItSinksSlowlyThroughTheLiquid() {
        var tumble = GenieTumble(point: CGPoint(x: 200, y: 300), velocity: .zero, turn: 0, touching: nil)
        _ = run(&tumble, push: .zero, seconds: 0.5)
        let fell: CGFloat = tumble.point.y - 300
        XCTAssertGreaterThan(fell, 5, "it still sinks")
        XCTAssertLessThan(fell, 0.5 * GenieGravity.pull * 0.25 * 0.4, "much slower than a free fall")
        _ = run(&tumble, push: .zero, seconds: 2)
        XCTAssertLessThan(tumble.speed, 300, "drag holds it near a gentle sinking speed")
    }

    func testASidewaysJerkCarriesItToTheFarWallAndTheLiquidSloshesBack() {
        var tumble = standing()
        _ = run(&tumble, push: CGSize(width: 1, height: 0), seconds: 0.3)
        XCTAssertLessThan(tumble.flow.width, 0, "the liquid lags behind the phone")
        var nearest: CGFloat = tumble.point.x
        var turned: Bool = false
        for _ in 0..<60 {
            _ = run(&tumble, push: .zero, seconds: 1.0 / 30.0)
            nearest = min(nearest, tumble.point.x)
            if tumble.flow.width > 0 { turned = true }
        }
        XCTAssertLessThanOrEqual(nearest, arena.box.minX + 2, "it reaches the left wall")
        XCTAssertTrue(turned, "the slosh rolls back the other way")
        XCTAssertGreaterThan(tumble.point.x, nearest + 10, "and carries it back off the wall")
    }

    func testAViolentShakeKnocksItDizzy() {
        var tumble = standing()
        let knocks = run(&tumble, push: CGSize(width: 3, height: 0), seconds: 0.4)
        XCTAssertTrue(knocks.contains { $0.surface == .left && $0.dizzying })
    }

    func testAGentleNudgeDoesNotMakeItDizzy() {
        var tumble = standing(x: 70)
        let knocks = run(&tumble, push: CGSize(width: 0.4, height: 0), seconds: 0.25)
        XCTAssertFalse(knocks.contains { $0.dizzying })
    }

    func testItSettlesOnceThePhoneIsStill() {
        var tumble = standing()
        _ = run(&tumble, push: CGSize(width: -0.9, height: 0.4), seconds: 0.3)
        XCTAssertFalse(tumble.done)
        _ = run(&tumble, push: .zero, seconds: 0.3)
        XCTAssertFalse(tumble.done)
        _ = run(&tumble, push: .zero, seconds: 6)
        XCTAssertTrue(tumble.done)
    }

    func testOnAFlatPhoneItFloatsWithTheEddiesUntilTheLiquidCalms() {
        var tumble = GenieTumble(point: CGPoint(x: 200, y: 400), velocity: .zero, turn: 30, touching: nil, gravity: .zero)
        XCTAssertTrue(tumble.floating)
        _ = run(&tumble, push: .zero, seconds: 2, gravity: .zero)
        XCTAssertNil(tumble.touching)
        XCTAssertGreaterThan(hypot(tumble.point.x - 200, tumble.point.y - 400), 5, "the eddies move it about")
        XCTAssertFalse(tumble.done)
        _ = run(&tumble, push: .zero, seconds: 1, gravity: .zero)
        XCTAssertTrue(tumble.done)
    }

    func testTheEddiesHaveNoSourcesOrSinks() {
        let step: CGFloat = 0.5
        for (x, y, age) in [(40.0, 90.0, 0.0), (210.0, 333.0, 1.7), (377.0, 610.0, 9.2)] {
            let p = CGPoint(x: x, y: y)
            let right = GenieTumble.eddies(at: CGPoint(x: p.x + step, y: p.y), age: age, strength: 22).flow
            let left = GenieTumble.eddies(at: CGPoint(x: p.x - step, y: p.y), age: age, strength: 22).flow
            let down = GenieTumble.eddies(at: CGPoint(x: p.x, y: p.y + step), age: age, strength: 22).flow
            let up = GenieTumble.eddies(at: CGPoint(x: p.x, y: p.y - step), age: age, strength: 22).flow
            let divergence = (right.width - left.width) / (2 * step) + (down.height - up.height) / (2 * step)
            XCTAssertEqual(divergence, 0, accuracy: 1e-3)
            let curl = (right.height - left.height) / (2 * step) - (down.width - up.width) / (2 * step)
            XCTAssertEqual(Double(curl), GenieTumble.eddies(at: p, age: age, strength: 22).curl, accuracy: 1e-3)
        }
    }

    func testTiltingQuicklySwirlsItLoose() {
        XCTAssertTrue(GenieTumble.swirls(from: CGSize(width: 0, height: 1), to: CGSize(width: 1, height: 0), seconds: 0.2))
        XCTAssertFalse(GenieTumble.swirls(from: CGSize(width: 0, height: 1), to: CGSize(width: 0.34, height: 0.94), seconds: 0.5))
        XCTAssertFalse(GenieTumble.swirls(from: CGSize(width: 0, height: 0.1), to: CGSize(width: 0.1, height: 0), seconds: 0.2), "flat")
    }

    func testItStaysInTheBoxWhateverYouDo() {
        var tumble = standing()
        let pushes: [CGSize] = [
            CGSize(width: 2.5, height: -1), CGSize(width: -3, height: 2), CGSize(width: 0.2, height: 3), CGSize(width: -1, height: -3),
        ]
        for step in 0..<240 {
            let push = pushes[step / 7 % pushes.count]
            _ = tumble.advance(by: 1.0 / 60.0, push: push, gravity: GenieGravity.down, arena: arena)
            XCTAssertTrue(arena.box.insetBy(dx: -0.001, dy: -0.001).contains(tumble.point))
            XCTAssertLessThanOrEqual(tumble.speed, GenieTumble.maxSpeed + 0.001)
            XCTAssertTrue(arena.box.insetBy(dx: -0.001, dy: -0.001).contains(tumble.point(after: 1, in: arena)))
        }
    }

    func testTheShakeFollowsTheRoomsGravity() {
        var tumble = GenieTumble(point: CGPoint(x: 200, y: 400), velocity: .zero, turn: 0, touching: nil)
        _ = run(&tumble, push: .zero, seconds: 3, gravity: CGSize(width: -1, height: 0))
        XCTAssertEqual(tumble.touching, .left)
        XCTAssertEqual(tumble.turn, GenieEdge.left.degrees, accuracy: 2)
    }
}
