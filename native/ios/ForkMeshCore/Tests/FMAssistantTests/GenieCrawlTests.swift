import Foundation
import XCTest
@testable import FMAssistant

final class GenieCrawlTests: XCTestCase {
    private let start = Date(timeIntervalSince1970: 1_000)
    private let full = CGRect(x: 0, y: 0, width: 400, height: 800)
    private let safe = CGRect(x: 0, y: 50, width: 400, height: 716)

    func testWalkEasesAndStrides() {
        let walk = GenieCrawl.walk(from: CGPoint(x: 0, y: 0), to: CGPoint(x: 160, y: 0), start: start, vitality: 0.4)
        XCTAssertEqual(walk.duration, 160 / GenieCrawl.speed(vitality: 0.4), accuracy: 0.001)
        XCTAssertEqual(walk.point(at: start), CGPoint(x: 0, y: 0))
        XCTAssertEqual(walk.point(at: walk.end), CGPoint(x: 160, y: 0))
        XCTAssertEqual(walk.point(at: start.addingTimeInterval(walk.duration / 2)).x, 80, accuracy: 0.001)
        XCTAssertEqual(walk.walking(at: start), 0)
        XCTAssertEqual(walk.walking(at: start.addingTimeInterval(walk.duration / 2)), 1)
        XCTAssertEqual(walk.facing, 1)
        XCTAssertGreaterThan(GenieCrawl.speed(vitality: 1), GenieCrawl.speed(vitality: 0))
        XCTAssertEqual(GenieWalk(from: .zero, to: CGPoint(x: 2, y: 90), start: start, duration: 1).facing, 0)
    }

    func testEdgesAndParking() {
        XCTAssertEqual(GenieCrawl.edge(near: CGPoint(x: 380, y: 400), in: full, margin: 40), .right)
        XCTAssertEqual(GenieCrawl.edge(near: CGPoint(x: 200, y: 10), in: full, margin: 40), .top)
        XCTAssertNil(GenieCrawl.edge(near: CGPoint(x: 200, y: 400), in: full, margin: 40))
        let tucked = GenieCrawl.parked(.right, near: CGPoint(x: 390, y: 900), full: full, safe: safe, side: 100)
        XCTAssertEqual(tucked.x, 400 - 100 * GenieCrawl.tuck, accuracy: 0.001)
        XCTAssertEqual(tucked.y, safe.maxY - 50, accuracy: 0.001)
        XCTAssertEqual(GenieEdge.left.degrees, 90)
        XCTAssertEqual(GenieEdge.right.degrees, -90)
    }

    func testTargetsStayInsideTheRoamAndFollowTheTilt() {
        let roam = CGRect(x: 60, y: 120, width: 280, height: 520)
        var seed: UInt64 = 7
        let random: () -> Double = {
            seed = seed &* 6_364_136_223_846_793_005 &+ 1
            return Double(seed >> 11) / Double(1 << 53)
        }
        var rightward = 0
        for _ in 0..<200 {
            let next = GenieCrawl.target(from: CGPoint(x: 200, y: 400), roam: roam, lean: CGSize(width: 1, height: 0), random: random)
            XCTAssertTrue(roam.insetBy(dx: -0.001, dy: -0.001).contains(next), "\(next)")
            if next.x > 200 { rightward += 1 }
        }
        XCTAssertGreaterThan(rightward, 150)
        let back = GenieCrawl.point(GenieCrawl.fraction(CGPoint(x: 100, y: 300), in: safe), in: safe)
        XCTAssertEqual(back.x, 100, accuracy: 0.001)
        XCTAssertEqual(back.y, 300, accuracy: 0.001)
    }
}
