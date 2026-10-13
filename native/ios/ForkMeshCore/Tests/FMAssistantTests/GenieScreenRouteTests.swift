import Foundation
import XCTest
@testable import FMAssistant

final class GenieScreenRouteTests: XCTestCase {
    private final class Dice {
        var state: UInt64
        init(_ seed: UInt64) { state = seed }
        func next() -> Double {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return Double(state >> 11) / Double(1 << 53)
        }
    }

    private let full = CGRect(x: 0, y: 0, width: 400, height: 900)
    private var arena: GenieArena { GenieArena(full: full, ceiling: 50, floor: 800, side: 92) }
    private let t0 = Date(timeIntervalSinceReferenceDate: 500)

    private var tabs: [GenieLandmark] {
        (0..<4).map { GenieLandmark(kind: .tab, rect: CGRect(x: 20 + CGFloat($0) * 90, y: 420, width: 70, height: 32), text: "Tab \($0)") }
    }

    private var marks: [GenieLandmark] {
        [
            GenieLandmark(kind: .button, rect: CGRect(x: 150, y: 300, width: 90, height: 40), text: "Merge"),
            GenieLandmark(kind: .heading, rect: CGRect(x: 60, y: 180, width: 220, height: 30), text: "Agents"),
            GenieLandmark(kind: .text, rect: CGRect(x: 60, y: 560, width: 260, height: 20), text: "Three agents are running"),
            GenieLandmark(kind: .image, rect: CGRect(x: 120, y: 640, width: 120, height: 90), text: "Avatar"),
        ] + tabs
    }

    private func choice(_ act: GenieScreenAct, _ mark: GenieLandmark) -> GenieScreenChoice {
        GenieScreenChoice(
            act: act, mark: mark, line: act.line(mark.text), group: act == .tabHop ? GenieScreenPlay.row(mark, in: marks) : [])
    }

    func testTravellingActsOnlyComeWhenTheSpiderMayMove() {
        let travelling = GenieScreenAct.allCases.filter(\.travels)
        XCTAssertEqual(travelling.count, 9)
        let dice = Dice(3)
        var still = GenieScreenPlay()
        for _ in 0..<40 {
            guard let picked = still.choose(marks, from: CGPoint(x: 300, y: 754), random: dice.next) else { continue }
            XCTAssertFalse(picked.act.travels)
        }
        var moving = GenieScreenPlay()
        var seen: Set<GenieScreenAct> = []
        for _ in 0..<GenieScreenAct.allCases.count * 3 {
            guard let picked = moving.choose(marks, from: CGPoint(x: 300, y: 754), travels: true, random: dice.next) else { continue }
            seen.insert(picked.act)
            if picked.act == .tabHop { XCTAssertGreaterThanOrEqual(picked.group.count, GenieScreenPlay.fewestTabs) }
        }
        XCTAssertTrue(Set(travelling).isSubset(of: seen), "missing \(Set(travelling).subtracting(seen))")
    }

    func testTabsInARowAreFoundLeftToRight() {
        let row = GenieScreenPlay.row(tabs[2], in: marks)
        XCTAssertEqual(row.map(\.text), ["Tab 0", "Tab 1", "Tab 2", "Tab 3"])
        XCTAssertTrue(GenieScreenPlay.row(marks[1], in: marks).isEmpty)
        XCTAssertEqual(GenieScreenPlay.row(marks[0], in: marks).count, 1)
    }

    func testEveryTravellingActHasARouteInsideTheScreen() {
        for act in GenieScreenAct.allCases where act.travels {
            guard let mark = marks.first(where: { GenieScreenPlay.fits(act, $0) && (act != .tabHop || $0.kind == .tab) }) else {
                XCTFail("no mark for \(act)")
                continue
            }
            guard let route = GenieScreenPlay.route(for: choice(act, mark), from: CGPoint(x: 330, y: 758), arena: arena, side: 92) else {
                XCTFail("no route for \(act)")
                continue
            }
            XCTAssertGreaterThan(route.path.count, 1, act.rawValue)
            XCTAssertEqual(route.path.first, route.approach, act.rawValue)
            for point in route.path {
                XCTAssertTrue(arena.box.insetBy(dx: -0.001, dy: -0.001).contains(point), "\(act) \(point)")
            }
            XCTAssertFalse(route.rects.isEmpty, act.rawValue)
            XCTAssertEqual(route.beats.count == 0 || route.beats.count == route.rects.count, true, act.rawValue)
        }
    }

    func testTraceWalksAllTheWayRoundWithoutStandingOnIt() {
        let button = marks[0]
        let route = GenieScreenPlay.route(for: choice(.trace, button), from: CGPoint(x: 330, y: 758), arena: arena, side: 92)!
        XCTAssertEqual(route.mark, .outline)
        XCTAssertEqual(route.path.first!.x, route.path.last!.x, accuracy: 0.5)
        XCTAssertEqual(route.path.first!.y, route.path.last!.y, accuracy: 0.5)
        XCTAssertFalse(route.path.contains { button.rect.contains($0) })
        XCTAssertGreaterThan(GenieGlass.length(route.path), Double(button.rect.width + button.rect.height) * 2)
        let start = route.path.first!
        XCTAssertGreaterThan(start.y, button.rect.midY, "it starts on the side nearest the spider")
    }

    func testBoopBumpsThreeTimesFacingTheButton() {
        let button = marks[0]
        let route = GenieScreenPlay.route(for: choice(.boop, button), from: CGPoint(x: 60, y: 320), arena: arena, side: 92)!
        XCTAssertEqual(route.mark, .rings)
        XCTAssertEqual(route.beats.count, 3)
        XCTAssertEqual(route.aim, 90, "it comes from the left and faces right")
        XCTAssertLessThan(route.approach.x, button.rect.minX)
        let effect = GenieScreenEffect(route: route, start: t0, duration: 2, hold: 1)
        var flashes = 0
        var wasOn = false
        for k in 0...200 {
            let on = !effect.rings(at: t0.addingTimeInterval(2 * Double(k) / 200)).isEmpty
            if on && !wasOn { flashes += 1 }
            wasOn = on
        }
        XCTAssertEqual(flashes, 3)
    }

    func testTabHopLandsOnEveryTab() {
        let route = GenieScreenPlay.route(for: choice(.tabHop, tabs[0]), from: CGPoint(x: 30, y: 758), arena: arena, side: 92)!
        XCTAssertEqual(route.rects.count, 4)
        XCTAssertEqual(route.beats, route.beats.sorted())
        XCTAssertEqual(route.rects.first, tabs[0].rect)
        XCTAssertTrue(route.path.allSatisfy { $0.y <= tabs[0].rect.minY })
        let back = GenieScreenPlay.route(for: choice(.tabHop, tabs[0]), from: CGPoint(x: 380, y: 758), arena: arena, side: 92)!
        XCTAssertEqual(back.rects.first, tabs[3].rect, "it starts at the end nearest the spider")
    }

    func testWalkAlongActsFollowTheText() {
        let heading = marks[1]
        let under = GenieScreenPlay.route(for: choice(.underline, heading), from: .zero, arena: arena, side: 92)!
        XCTAssertTrue(under.path.allSatisfy { $0.y > heading.rect.maxY })
        let over = GenieScreenPlay.route(for: choice(.measure, heading), from: .zero, arena: arena, side: 92)!
        XCTAssertTrue(over.path.allSatisfy { $0.y < heading.rect.minY })
        XCTAssertEqual(over.label, "220 pt")
        XCTAssertEqual(over.mark, .tape)
        XCTAssertLessThan(over.path.first!.x, over.path.last!.x)
    }

    func testEffectsGrowWithTheWalkAndFadeAfterTheHold() {
        let route = GenieScreenPlay.route(for: choice(.highlight, marks[1]), from: .zero, arena: arena, side: 92)!
        let effect = GenieScreenEffect(route: route, start: t0, duration: 2, hold: 1)
        XCTAssertEqual(effect.reach(at: t0), 0)
        XCTAssertEqual(effect.reach(at: t0.addingTimeInterval(1)), 110, accuracy: 0.001)
        XCTAssertEqual(effect.reach(at: t0.addingTimeInterval(2)), 220, accuracy: 0.001)
        XCTAssertEqual(effect.strength(at: t0.addingTimeInterval(2.9)), 1)
        XCTAssertEqual(effect.strength(at: t0.addingTimeInterval(3.4)), 0.5, accuracy: 0.001)
        XCTAssertEqual(effect.strength(at: effect.end), 0)
    }

    func testOnTheGlassItPlaysBesideTheElementNotOnIt() {
        let button = marks[0]
        let spot = GenieScreenPlay.beside(for: button, arena: arena, side: 92, blockers: [])
        XCTAssertFalse(GenieClearance.covers(spot, side: 92, buttons: [button.rect]))
        XCTAssertLessThan(hypot(spot.x - button.rect.midX, spot.y - button.rect.midY), 120)
    }
}
