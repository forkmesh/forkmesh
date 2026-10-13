import Foundation
import XCTest
@testable import FMAssistant

final class GenieScreenPlayTests: XCTestCase {
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

    private var marks: [GenieLandmark] {
        [
            GenieLandmark(kind: .button, rect: CGRect(x: 340, y: 60, width: 44, height: 36), text: "Settings"),
            GenieLandmark(kind: .tab, rect: CGRect(x: 0, y: 810, width: 130, height: 50), text: "Home"),
            GenieLandmark(kind: .heading, rect: CGRect(x: 20, y: 130, width: 180, height: 30), text: "Agents"),
            GenieLandmark(kind: .text, rect: CGRect(x: 20, y: 230, width: 260, height: 20), text: "Three agents are running"),
            GenieLandmark(kind: .image, rect: CGRect(x: 16, y: 60, width: 36, height: 36), text: "ForkMesh logo"),
            GenieLandmark(kind: .field, rect: CGRect(x: 20, y: 450, width: 350, height: 36), text: "Search"),
            GenieLandmark(kind: .link, rect: CGRect(x: 20, y: 560, width: 120, height: 22), text: "Docs"),
            GenieLandmark(kind: .toggle, rect: CGRect(x: 300, y: 330, width: 51, height: 31), text: "Wander"),
            GenieLandmark(kind: .progress, rect: CGRect(x: 20, y: 640, width: 300, height: 4), text: "50%"),
        ]
    }

    func testEveryActIsAStillTrickTheSpiderKnows() {
        XCTAssertGreaterThanOrEqual(GenieScreenAct.allCases.count, 10)
        for act in GenieScreenAct.allCases {
            let trick = GenieTricks.named(act.trick)
            XCTAssertNotNil(trick, act.rawValue)
            XCTAssertEqual(trick?.travel, GenieTravel.none, act.rawValue)
            XCTAssertFalse(act.line("Settings").isEmpty)
            XCTAssertFalse(act.fits.isEmpty)
        }
        XCTAssertEqual(Set(GenieScreenAct.allCases.map(\.trick)).count, GenieScreenAct.allCases.count)
    }

    func testChoicesFitTheirElementAndUseEveryActBeforeRepeating() {
        let dice = Dice(1)
        var play = GenieScreenPlay()
        var seen: [GenieScreenAct] = []
        let still = GenieScreenAct.allCases.filter { !$0.travels }
        for _ in 0..<still.count {
            guard let choice = play.choose(marks, from: CGPoint(x: 300, y: 754), random: dice.next) else {
                XCTFail("no choice")
                return
            }
            XCTAssertTrue(GenieScreenPlay.fits(choice.act, choice.mark))
            seen.append(choice.act)
        }
        XCTAssertEqual(Set(seen).count, still.count)
        var empty = GenieScreenPlay()
        XCTAssertNil(empty.choose([], from: .zero, random: dice.next))
        var lonely = GenieScreenPlay()
        let only = [GenieLandmark(kind: .link, rect: CGRect(x: 10, y: 10, width: 40, height: 20), text: "Docs")]
        let first = lonely.choose(only, from: .zero, random: dice.next)
        XCTAssertNotNil(first)
        XCTAssertNil(lonely.choose(only, from: .zero, random: dice.next))
    }

    func testPerchKeepsOffButtonsAndTheElementItPlaysWith() {
        let blockers = GenieLandmarks.blockers(marks)
        for mark in marks {
            let (spot, edge) = GenieScreenPlay.perch(for: mark, arena: arena, side: 92, blockers: blockers)
            XCTAssertTrue(arena.touches(spot, edge), "\(mark.text)")
            XCTAssertFalse(GenieClearance.covers(spot, side: 92, buttons: blockers + [mark.rect]), "\(mark.text) at \(spot)")
        }
    }

    func testReadingSweepsTheEyesAlongTheText() {
        let text = CGRect(x: 20, y: 130, width: 300, height: 30)
        let spider = CGPoint(x: 170, y: 400)
        let start = GenieScreenPlay.gaze(from: spider, to: text, progress: 0, sweep: true)
        let end = GenieScreenPlay.gaze(from: spider, to: text, progress: 1, sweep: true)
        XCTAssertLessThan(start.x, 0)
        XCTAssertGreaterThan(end.x, 0)
        XCTAssertLessThan(start.y, 0)
        let fixed = GenieScreenPlay.gaze(from: spider, to: text, progress: 0, sweep: false)
        XCTAssertEqual(fixed, GenieScreenPlay.gaze(from: spider, to: text, progress: 1, sweep: false))
        XCTAssertLessThanOrEqual(hypot(fixed.x, fixed.y), 1.0001)
    }

    func testThreadStopsJustShortOfTheElement() {
        let rect = CGRect(x: 100, y: 100, width: 80, height: 40)
        let end = GenieScreenPlay.thread(from: CGPoint(x: 140, y: 400), to: rect)
        XCTAssertFalse(rect.contains(end))
        XCTAssertEqual(end.x, 140, accuracy: 0.01)
        XCTAssertEqual(end.y, 143, accuracy: 0.01)
    }

    func testLandmarkKindsFromAccessibilityTraits() {
        XCTAssertEqual(GenieLandmarks.kind(.button), .button)
        XCTAssertEqual(GenieLandmarks.kind([.button, .image]), .button)
        XCTAssertEqual(GenieLandmarks.kind(.button, inTabBar: true), .tab)
        XCTAssertEqual(GenieLandmarks.kind(.button, value: "1"), .toggle)
        XCTAssertEqual(GenieLandmarks.kind(.toggle), .toggle)
        XCTAssertEqual(GenieLandmarks.kind(.searchField), .field)
        XCTAssertEqual(GenieLandmarks.kind(.textEntry), .field)
        XCTAssertEqual(GenieLandmarks.kind(.adjustable), .slider)
        XCTAssertEqual(GenieLandmarks.kind([.link, .staticText]), .link)
        XCTAssertEqual(GenieLandmarks.kind([.header, .staticText]), .heading)
        XCTAssertEqual(GenieLandmarks.kind(.image), .image)
        XCTAssertEqual(GenieLandmarks.kind(.staticText), .text)
        XCTAssertEqual(GenieLandmarks.kind(.updates, value: "45%"), .progress)
        XCTAssertNil(GenieLandmarks.kind([]))
        for kind: GenieLandmarkKind in [.button, .link, .tab, .field, .toggle, .slider, .image] { XCTAssertTrue(kind.keepsClear) }
        for kind: GenieLandmarkKind in [.heading, .text, .progress] { XCTAssertFalse(kind.keepsClear) }
    }

    func testTidyClipsDropsContainersDedupesAndKeepsButtonsWithinTheCap() {
        let screen = CGRect(x: 0, y: 0, width: 400, height: 900)
        let raw = [
            GenieLandmark(kind: .text, rect: CGRect(x: 0, y: 0, width: 50, height: 20), text: "Line\none"),
            GenieLandmark(kind: .button, rect: CGRect(x: 380, y: 10, width: 60, height: 30), text: "Edge"),
            GenieLandmark(kind: .button, rect: CGRect(x: 381, y: 11, width: 59, height: 29), text: "Edge again"),
            GenieLandmark(kind: .button, rect: CGRect(x: 0, y: 0, width: 400, height: 800), text: "Whole page"),
            GenieLandmark(kind: .image, rect: CGRect(x: 10, y: 10, width: 3, height: 3)),
            GenieLandmark(kind: .image, rect: CGRect(x: 10, y: 500, width: 40, height: 40), text: "Logo"),
        ]
        let tidy = GenieLandmarks.tidy(raw, in: screen)
        XCTAssertEqual(tidy.map(\.text), ["Edge", "Logo", "Line one"])
        XCTAssertEqual(tidy[0].rect, CGRect(x: 380, y: 10, width: 20, height: 30))
        XCTAssertEqual(GenieLandmarks.blockers(tidy).count, 2)
        var many: [GenieLandmark] = []
        for index in 0..<500 {
            let kind: GenieLandmarkKind = index % 2 == 0 ? .text : .button
            let x: CGFloat = CGFloat(index % 20) * 20
            let y: CGFloat = CGFloat(index / 20) * 30
            many.append(GenieLandmark(kind: kind, rect: CGRect(x: x, y: y, width: 18, height: 20)))
        }
        let capped: [GenieLandmark] = GenieLandmarks.tidy(many, in: screen)
        XCTAssertEqual(capped.count, GenieLandmarks.cap)
        XCTAssertTrue(capped.prefix(250).allSatisfy { (mark: GenieLandmark) -> Bool in mark.keepsClear })
    }

    func testBagShufflesWithoutRepeatsAndSkipsWhatDoesNotFit() {
        let dice = Dice(9)
        var bag = GenieBag(Array(0..<8))
        let first = (0..<8).compactMap { _ in bag.next(random: dice.next) }
        XCTAssertEqual(Set(first).count, 8)
        let again = bag.next(random: dice.next)
        XCTAssertNotEqual(again, first.last)
        var picky = GenieBag(Array(0..<8))
        for _ in 0..<5 { XCTAssertEqual(picky.next(random: dice.next, fits: { $0 % 4 == 0 }).map { $0 % 4 }, 0) }
        XCTAssertNil(picky.next(random: dice.next, fits: { $0 > 99 }))
    }
}
