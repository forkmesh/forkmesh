import Foundation
import XCTest
@testable import FMAssistant

final class AquariumTests: XCTestCase {
    private final class Dice {
        var state: UInt64
        init(_ seed: UInt64) { state = seed }
        func next() -> Double {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return Double(state >> 11) / Double(1 << 53)
        }
    }

    private let screen = CGRect(x: 0, y: 0, width: 390, height: 844)

    private var page: [GenieLandmark] {
        [
            GenieLandmark(kind: .image, rect: CGRect(x: 16, y: 60, width: 36, height: 36), text: "ForkMesh logo"),
            GenieLandmark(kind: .button, rect: CGRect(x: 330, y: 60, width: 44, height: 36), text: "Settings"),
            GenieLandmark(kind: .heading, rect: CGRect(x: 20, y: 130, width: 180, height: 30), text: "Agents"),
            GenieLandmark(kind: .text, rect: CGRect(x: 20, y: 230, width: 260, height: 20), text: "Three agents are running"),
            GenieLandmark(kind: .button, rect: CGRect(x: 20, y: 330, width: 70, height: 40), text: "Merge"),
            GenieLandmark(kind: .toggle, rect: CGRect(x: 300, y: 330, width: 51, height: 31), text: "Wander"),
            GenieLandmark(kind: .field, rect: CGRect(x: 20, y: 450, width: 350, height: 36), text: "Search"),
            GenieLandmark(kind: .link, rect: CGRect(x: 20, y: 560, width: 120, height: 22), text: "Read the docs"),
            GenieLandmark(kind: .tab, rect: CGRect(x: 0, y: 760, width: 130, height: 50), text: "Home"),
            GenieLandmark(kind: .tab, rect: CGRect(x: 130, y: 760, width: 130, height: 50), text: "Agents"),
            GenieLandmark(kind: .tab, rect: CGRect(x: 260, y: 760, width: 130, height: 50), text: "Chat"),
        ]
    }

    private func swimmers(_ count: Int, waiting: Set<Int> = []) -> [AquariumSwimmer] {
        (0..<count).map { AquariumSwimmer(id: "a\($0)", name: "Agent \($0)", model: $0 % 2 == 0 ? "opus" : "sonnet", waiting: waiting.contains($0)) }
    }

    private func tank(_ swimmers: [AquariumSwimmer], dice: Dice, spider: AquariumSpider? = nil) -> AquariumTank {
        var tank = AquariumTank(screen: screen, top: 50, floor: 750)
        tank.see(page)
        tank.spider = spider
        tank.roster(swimmers, random: dice.next)
        return tank
    }

    func testLookComesFromNameAndModel() {
        XCTAssertEqual(AquariumLook.of(name: "Fix login", model: "opus"), AquariumLook.of(name: "Fix login", model: "opus"))
        var species = Set<AquariumSpecies>()
        for index in 0..<60 {
            let look = AquariumLook.of(name: "Task \(index)", model: "model-\(index % 3)")
            species.insert(look.species)
            XCTAssertTrue((0.85...1.2).contains(look.size))
        }
        XCTAssertEqual(species.count, AquariumSpecies.allCases.count)
    }

    func testThreeGuestsWhenNoAgentRuns() {
        let dice = Dice(1)
        var tank = tank([], dice: dice)
        XCTAssertEqual(tank.fish.map(\.name), ["Bubbles", "Finn", "Coral"])
        tank.roster(swimmers(4), random: dice.next)
        for _ in 0..<300 { tank.step(1.0 / 30, random: dice.next) }
        XCTAssertEqual(Set(tank.fish.map(\.id)), Set(swimmers(4).map(\.id)))
    }

    func testOneFishPerAgentAndTheyLeaveWithTheirAgent() {
        let dice = Dice(2)
        var tank = tank(swimmers(3), dice: dice)
        for _ in 0..<90 { tank.step(1.0 / 30, random: dice.next) }
        XCTAssertEqual(tank.fish.count, 3)
        let events = tank.roster(Array(swimmers(3).prefix(2)), random: dice.next)
        XCTAssertTrue(events.contains(.log("Agent 2 swims off")))
        XCTAssertEqual(tank.fish.first { $0.id == "a2" }?.speech, "Bye!")
        for _ in 0..<300 { tank.step(1.0 / 30, random: dice.next) }
        XCTAssertEqual(tank.fish.map(\.id).sorted(), ["a0", "a1"])
        let many: AquariumTank = self.tank(swimmers(20), dice: dice)
        XCTAssertEqual(many.fish.count, AquariumTank.most)
    }

    func testWaitingFishHoverNearTheTopAndAskForYou() {
        let dice = Dice(3)
        var tank = tank(swimmers(1, waiting: [0]), dice: dice)
        var asked = false
        for frame in 0..<(30 * 40) {
            tank.step(1.0 / 30, random: dice.next)
            let fish = tank.fish[0]
            XCTAssertNil(fish.act)
            if fish.speech == "I need you!" { asked = true }
            if frame > 30 * 8 { XCTAssertLessThan(fish.point.y, tank.water.minY + 190) }
        }
        XCTAssertTrue(asked)
    }

    func testEveryElementTrickPlaysBesideItsTargetAndNeverRestsOnAButton() {
        for seed: UInt64 in [4, 11, 23, 57] { elementTricks(seed: seed) }
    }

    private func elementTricks(seed: UInt64) {
        let dice = Dice(seed)
        var tank = tank(swimmers(3), dice: dice)
        var played = Set<AquariumTrick>()
        var logs = 0
        var overlapFor: [String: Double] = [:]
        let dt = 1.0 / 30
        for _ in 0..<(30 * 400) {
            logs += tank.step(dt, random: dice.next).filter { if case .log = $0 { return true } else { return false } }.count
            for fish in tank.fish {
                let box = fish.box
                let covers = GenieLandmarks.overlaps(box, tank.blockers)
                if let act = fish.act, fish.phase == .acting {
                    played.insert(act.trick)
                    if !(act.trick == .startle && tank.time - act.start >= 1) {
                        XCTAssertFalse(covers, "\(act.trick) covers a button at \(box)")
                        XCTAssertFalse(GenieLandmarks.overlaps(box, [act.target]), "\(act.trick) covers its target")
                    }
                }
                if fish.phase == .wandering, hypot(fish.point.x - fish.goal.x, fish.point.y - fish.goal.y) < 4,
                    hypot(fish.velocity.width, fish.velocity.height) < 2
                {
                    XCTAssertFalse(covers, "\(fish.name) rests on a button at \(box)")
                }
                if let speech = fish.speechBox {
                    XCTAssertFalse(GenieLandmarks.overlaps(speech, tank.blockers), "speech covers a button")
                }
                overlapFor[fish.id] = covers ? (overlapFor[fish.id] ?? 0) + dt : 0
                if (overlapFor[fish.id] ?? 0) > 0.6 { XCTAssertLessThan(fish.opacity, 0.3) }
                if fish.phase != .entering && fish.phase != .leaving {
                    XCTAssertTrue(tank.screen.contains(fish.point))
                }
            }
        }
        XCTAssertEqual(played, Set(AquariumTrick.element))
        XCTAssertGreaterThanOrEqual(AquariumTrick.element.count, 12)
        XCTAssertGreaterThan(logs, 13)
    }

    func testSpiderTricksMakeTheSpiderReact() {
        spiderTricks(seed: 5, spider: AquariumSpider(point: CGPoint(x: 300, y: 711), side: 92, free: true))
        spiderTricks(seed: 6, spider: AquariumSpider(point: CGPoint(x: 39, y: 400), side: 92, free: true))
    }

    private func spiderTricks(seed: UInt64, spider: AquariumSpider) {
        let dice = Dice(seed)
        var tank = tank(swimmers(2), dice: dice, spider: spider)
        var played = Set<AquariumTrick>()
        var asks: [AquariumSpiderAsk] = []
        for _ in 0..<(30 * 600) {
            for event in tank.step(1.0 / 30, random: dice.next) {
                if case .spider(let ask) = event { asks.append(ask) }
            }
            for fish in tank.fish where fish.phase == .acting {
                if let act = fish.act { played.insert(act.trick) }
            }
            XCTAssertLessThanOrEqual(tank.fish.filter { $0.act?.trick.withSpider == true && $0.phase == .acting }.count, 1)
        }
        XCTAssertEqual(played.intersection(AquariumTrick.spider), Set(AquariumTrick.spider))
        XCTAssertGreaterThanOrEqual(AquariumTrick.spider.count, 10)
        for wanted: AquariumSpiderAsk in [.wave, .cheer, .giggle, .surprise, .dizzy, .nod] {
            XCTAssertTrue(asks.contains(wanted), "never asked \(wanted)")
        }
        XCTAssertTrue(asks.contains { if case .walk = $0 { return true } else { return false } })
        XCTAssertTrue(asks.contains { if case .look = $0 { return true } else { return false } })
    }

    func testNoSpiderTricksWhileTheSpiderIsBusyOrAway() {
        let dice = Dice(6)
        var tank = tank(swimmers(3), dice: dice, spider: AquariumSpider(point: CGPoint(x: 300, y: 711), side: 92, free: false))
        for frame in 0..<(30 * 120) {
            if frame == 30 * 60 { tank.spider = nil }
            for event in tank.step(1.0 / 30, random: dice.next) {
                if case .spider = event { XCTFail("asked the spider while it was not free") }
            }
            XCTAssertFalse(tank.spiderPlaying)
        }
    }

    func testCalmTankSwimsSlowlyWithoutParticles() {
        let dice = Dice(7)
        var tank = tank(swimmers(3), dice: dice)
        tank.calm = true
        var last: [String: CGPoint] = [:]
        for frame in 0..<(30 * 120) {
            tank.step(1.0 / 30, random: dice.next)
            XCTAssertTrue(tank.particles.isEmpty)
            for fish in tank.fish {
                if let before = last[fish.id], frame > 30 * 3 {
                    XCTAssertLessThanOrEqual(hypot(fish.point.x - before.x, fish.point.y - before.y) * 30, 240 * AquariumTank.calmPace + 1)
                }
                last[fish.id] = fish.point
            }
        }
    }

    func testBesideSpotsKeepTheFishAndItsTargetApart() {
        let dice = Dice(8)
        let fish = AquariumFish(id: "f", name: "Finn", model: "x", waiting: false, at: .zero)
        let water = AquariumTank.water(screen: screen, top: 50, floor: 750)
        for mark in page {
            guard let spot = AquariumClear.spot(
                beside: mark.rect, body: { fish.body(at: $0) }, blockers: GenieLandmarks.blockers(page), water: water,
                prefer: CGPoint(x: 200, y: 400))
            else { continue }
            XCTAssertFalse(GenieLandmarks.overlaps(fish.body(at: spot), GenieLandmarks.blockers(page) + [mark.rect]))
        }
        let point = AquariumClear.waypoint(
            in: water, body: { fish.body(at: $0) }, blockers: GenieLandmarks.blockers(page), water: water, random: dice.next)
        XCTAssertTrue(AquariumClear.clear(fish.body(at: point), blockers: GenieLandmarks.blockers(page), water: water))
        XCTAssertEqual(AquariumClear.opacity(CGRect(x: 25, y: 335, width: 20, height: 20), blockers: GenieLandmarks.blockers(page)), AquariumTank.crossing)
        XCTAssertEqual(AquariumClear.opacity(CGRect(x: 150, y: 600, width: 20, height: 20), blockers: GenieLandmarks.blockers(page)), 1)
    }

    func testTricksFitTheirElements() {
        let short = GenieLandmark(kind: .text, rect: CGRect(x: 0, y: 0, width: 100, height: 20), text: "Agents")
        let long = GenieLandmark(kind: .text, rect: CGRect(x: 0, y: 0, width: 100, height: 20), text: String(repeating: "word ", count: 10))
        let blank = GenieLandmark(kind: .heading, rect: CGRect(x: 0, y: 0, width: 40, height: 20), text: "")
        XCTAssertTrue(AquariumTrick.count.fits(short))
        XCTAssertFalse(AquariumTrick.count.fits(long))
        XCTAssertFalse(AquariumTrick.read.fits(blank))
        XCTAssertFalse(AquariumTrick.measure.fits(blank))
        XCTAssertFalse(AquariumTrick.polish.fits(short))
        XCTAssertTrue(AquariumTrick.polish.fits(GenieLandmark(kind: .button, rect: CGRect(x: 0, y: 0, width: 60, height: 30))))
        for trick in AquariumTrick.element { XCTAssertFalse(trick.fits.isEmpty, "\(trick)") }
    }
}
