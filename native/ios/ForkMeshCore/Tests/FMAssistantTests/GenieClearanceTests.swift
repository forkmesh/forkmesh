import Foundation
import XCTest
@testable import FMAssistant

final class GenieClearanceTests: XCTestCase {
    private let full = CGRect(x: 0, y: 0, width: 400, height: 900)
    private let safe = CGRect(x: 0, y: 50, width: 400, height: 816)
    private var arena: GenieArena { GenieArena(full: full, ceiling: 50, floor: 800, side: 100) }

    func testStandingOnTheBarDoesNotCoverIt() {
        let bar = CGRect(x: 0, y: 800, width: 400, height: 100)
        let spot = CGPoint(x: 200, y: arena.box.maxY)
        XCTAssertFalse(GenieClearance.covers(spot, side: 100, buttons: [bar]))
        let (point, edge) = GenieClearance.place(spot, on: .bottom, arena: arena, side: 100, buttons: [bar])
        XCTAssertEqual(point, spot)
        XCTAssertEqual(edge, .bottom)
    }

    func testStepsAsideFromAButtonAlongTheSameSurface() {
        let button = CGRect(x: 180, y: 700, width: 40, height: 40)
        let spot = CGPoint(x: 200, y: arena.box.maxY)
        XCTAssertTrue(GenieClearance.covers(spot, side: 100, buttons: [button]))
        let (point, edge) = GenieClearance.place(spot, on: .bottom, arena: arena, side: 100, buttons: [button])
        XCTAssertEqual(edge, .bottom)
        XCTAssertFalse(GenieClearance.covers(point, side: 100, buttons: [button]))
        XCTAssertLessThanOrEqual(abs(point.x - spot.x), 66)
        XCTAssertEqual(point.y, spot.y, accuracy: 0.001)
    }

    func testClimbsToAnotherSurfaceWhenTheFloorIsTaken() {
        let dock = CGRect(x: 0, y: 690, width: 400, height: 110)
        let spot = CGPoint(x: 330, y: arena.box.maxY)
        let (point, edge) = GenieClearance.place(spot, on: .bottom, arena: arena, side: 100, buttons: [dock])
        XCTAssertEqual(edge, .right)
        XCTAssertFalse(GenieClearance.covers(point, side: 100, buttons: [dock]))
        XCTAssertLessThan(point.y, 690)
    }

    func testStaysWhenNowhereIsClearer() {
        let spot = CGPoint(x: 200, y: arena.box.maxY)
        let (point, edge) = GenieClearance.place(spot, on: .bottom, arena: arena, side: 100, buttons: [full])
        XCTAssertEqual(point, spot)
        XCTAssertEqual(edge, .bottom)
    }

    func testTuckedSpiderSlidesAlongItsEdge() {
        let start = GenieCrawl.parked(.right, near: CGPoint(x: 400, y: 400), full: full, safe: safe, side: 100)
        let button = CGRect(x: 340, y: 380, width: 60, height: 44)
        XCTAssertTrue(GenieClearance.covers(start, side: 100, buttons: [button]))
        let moved = GenieClearance.tucked(.right, near: start, full: full, safe: safe, side: 100, buttons: [button])
        XCTAssertEqual(moved.x, start.x, accuracy: 0.001)
        XCTAssertFalse(GenieClearance.covers(moved, side: 100, buttons: [button]))
        XCTAssertLessThan(abs(moved.y - start.y), 100)
        XCTAssertEqual(GenieClearance.tucked(.right, near: start, full: full, safe: safe, side: 100, buttons: []), start)
    }
}
