import XCTest
@testable import FMAssistant

final class GenieTouchPlayTests: XCTestCase {
    private let height = 800.0

    func testThirtyNewTricksAllReachableByTouch() {
        let ids = GenieTouchPlay.pools.values.flatMap { $0 }
        XCTAssertEqual(ids.count, 30)
        XCTAssertEqual(Set(ids).count, 30)
        XCTAssertEqual(Set(GenieTouchPlay.pools.keys), Set(GenieTouchGesture.allCases))
        for id in ids {
            let trick = GenieTricks.named(id)
            XCTAssertNotNil(trick, id)
            XCTAssertEqual(trick?.travel, GenieTravel.none, id)
        }
        XCTAssertTrue(Set(ids).isDisjoint(with: GenieHandPlay.pools.values.joined()))
        var play = GenieTouchPlay()
        for gesture in GenieTouchGesture.allCases {
            let pool = GenieTouchPlay.pools[gesture] ?? []
            let picked = (0..<pool.count * 2).compactMap { _ in play.trick(for: gesture)?.id }
            XCTAssertEqual(picked, pool + pool, gesture.rawValue)
        }
    }

    func testHoldingStillHovers() {
        var play = GenieTouchPlay()
        XCTAssertNil(play.begin(x: 200, y: 400, at: 0, height: height))
        XCTAssertNil(play.move(x: 205, y: 403, at: 0.3))
        XCTAssertNil(play.still(at: 1.0))
        XCTAssertEqual(play.still(at: 1.5), .hover)
        XCTAssertNil(play.still(at: 5))
        XCTAssertNil(play.end(x: 205, y: 403, vx: 400, vy: 0, at: 5.1, parked: false))
        XCTAssertNil(play.still(at: 9))
    }

    func testCircleShakeSpeedHighAndFloor() {
        var play = GenieTouchPlay()
        _ = play.begin(x: 300, y: 400, at: 0, height: height)
        var loop: GenieTouchGesture?
        for step in 1...40 {
            let angle = Double(step) / 40 * 2 * Double.pi
            loop = loop ?? play.move(x: 200 + cos(angle) * 100, y: 400 + sin(angle) * 100, at: Double(step) * 0.05)
        }
        XCTAssertEqual(loop, .loop)

        var shaker = GenieTouchPlay()
        _ = shaker.begin(x: 200, y: 400, at: 0, height: height)
        var shake: GenieTouchGesture?
        for step in 1...8 {
            shake = shake ?? shaker.move(x: step % 2 == 0 ? 200 : 240, y: 400, at: Double(step) * 0.08)
        }
        XCTAssertEqual(shake, .shake)

        var racer = GenieTouchPlay()
        _ = racer.begin(x: 0, y: 400, at: 0, height: height)
        XCTAssertNil(racer.move(x: 20, y: 400, at: 0.02))
        XCTAssertEqual(racer.move(x: 140, y: 400, at: 0.08), .speed)

        var climber = GenieTouchPlay()
        _ = climber.begin(x: 200, y: 400, at: 0, height: height)
        XCTAssertNil(climber.move(x: 200, y: 300, at: 0.4))
        XCTAssertEqual(climber.move(x: 200, y: 120, at: 0.8), .high)
        XCTAssertNil(climber.move(x: 200, y: 100, at: 4), "once per drag")

        var skater = GenieTouchPlay()
        _ = skater.begin(x: 50, y: 700, at: 0, height: height)
        XCTAssertNil(skater.move(x: 150, y: 700, at: 0.4))
        XCTAssertEqual(skater.move(x: 230, y: 700, at: 0.8), .floor)
    }

    func testReleases() {
        var play = GenieTouchPlay()
        _ = play.begin(x: 200, y: 500, at: 0, height: height)
        XCTAssertEqual(play.end(x: 200, y: 500, vx: 100, vy: -1200, at: 0.5, parked: false), .toss)
        _ = play.begin(x: 200, y: 200, at: 10, height: height)
        XCTAssertEqual(play.end(x: 200, y: 200, vx: 30, vy: 20, at: 11, parked: false), .drop)
        _ = play.begin(x: 200, y: 600, at: 30, height: height)
        XCTAssertEqual(play.end(x: 200, y: 600, vx: 30, vy: 20, at: 31, parked: false), .setDown)
        _ = play.begin(x: 200, y: 600, at: 50, height: height)
        XCTAssertNil(play.end(x: 200, y: 600, vx: 30, vy: 20, at: 50.1, parked: false), "a quick nudge is not set down")
        _ = play.begin(x: 10, y: 600, at: 70, height: height)
        XCTAssertNil(play.end(x: 10, y: 600, vx: 0, vy: -2000, at: 71, parked: true))
    }

    func testPickingItUpAgainAndAgainAndTheGap() {
        var play = GenieTouchPlay()
        XCTAssertNil(play.begin(x: 200, y: 600, at: 0, height: height))
        _ = play.end(x: 200, y: 600, vx: 500, vy: 0, at: 0.2, parked: false)
        XCTAssertNil(play.begin(x: 200, y: 600, at: 1, height: height))
        _ = play.end(x: 200, y: 600, vx: 500, vy: 0, at: 1.2, parked: false)
        XCTAssertEqual(play.begin(x: 200, y: 600, at: 2, height: height), .again)
        XCTAssertNil(play.end(x: 200, y: 600, vx: 0, vy: -1500, at: 3, parked: false), "too soon after the last trick")
        XCTAssertNil(play.begin(x: 200, y: 600, at: 20, height: height))
        XCTAssertEqual(play.end(x: 200, y: 600, vx: 0, vy: -1500, at: 21, parked: false), .toss)
    }
}
