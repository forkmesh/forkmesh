import Foundation
import XCTest
@testable import FMAssistant

final class GenieMagicTests: XCTestCase {
    private let ids: [String] = [
        "genie_poof", "genie_levitate", "genie_lamp", "genie_wishes", "genie_smoke_rings", "genie_shimmer", "genie_stardust", "genie_carpet",
        "genie_vortex", "genie_crystal", "genie_runes", "genie_halo", "genie_fireflies", "genie_whirl", "genie_grow", "genie_bottle",
        "genie_rainbow", "genie_fireworks", "genie_bubbles", "genie_glitter", "genie_jewel_beam", "genie_ripple", "genie_moons",
        "genie_constellation", "genie_crescent", "genie_mirage", "genie_coins", "genie_flourish", "genie_incense", "genie_whirlwind",
    ]

    private let full = CGRect(x: 0, y: 0, width: 400, height: 900)
    private let target = CGRect(x: 60, y: 300, width: 160, height: 44)

    private func stage(turn: Double = 0, spider: CGPoint = CGPoint(x: 300, y: 760), target: CGRect? = nil) -> GenieMagicStage {
        GenieMagicStage(spider: spider, side: 92, turn: turn, target: target ?? self.target, full: full)
    }

    private func trick(_ magic: GenieMagic) throws -> GenieTrick {
        try XCTUnwrap(GenieTricks.named(magic.trick), magic.rawValue)
    }

    func testThirtyGenieTricksJoinTheCatalog() throws {
        XCTAssertEqual(GenieMagic.allCases.count, 30)
        XCTAssertEqual(GenieTricks.all.count, 172)
        XCTAssertEqual(GenieMagic.allCases.map(\.trick), ids)
        XCTAssertEqual(GenieTricks.all.suffix(30).map(\.id), ids)
        XCTAssertEqual(GenieTricks.all.compactMap(\.magic), GenieMagic.allCases)
        for id in ["levitate", "teleport", "rainbow", "fireworks", "bubbles", "grow", "tornado", "vanish"] {
            XCTAssertNotNil(GenieTricks.named(id), id)
            XCTAssertNil(GenieTricks.named(id)?.magic, id)
        }
        for magic in GenieMagic.allCases {
            let trick = try trick(magic)
            XCTAssertEqual(trick.magic, magic)
            XCTAssertEqual(trick.travel, GenieTravel.none, trick.id)
            XCTAssertTrue((2.2...3.5).contains(trick.duration), trick.id)
            XCTAssertEqual(GenieTricks.find(trick.id)?.id, trick.id)
            XCTAssertEqual(GenieTricks.find(trick.title)?.id, trick.id)
            XCTAssertEqual(GenieTricks.find(trick.words[0])?.id, trick.id, trick.words[0])
            XCTAssertEqual(trick.frame(age: 1)?.magic, magic)
        }
        let recipes = Set(GenieTricks.all.map { "\($0.legs)|\($0.eyes)|\($0.mouth)|\($0.body)|\($0.effect)|\($0.travel)" })
        XCTAssertEqual(recipes.count, GenieTricks.all.count)
    }

    @MainActor
    func testGenieEffectsAreSpokenLikeOtherTricks() {
        XCTAssertEqual(GenieSpotter.spot("magic lamp")?.arguments, ["trick": "genie_lamp"])
        XCTAssertEqual(GenieSpotter.spot("smoke rings")?.arguments, ["trick": "genie_smoke_rings"])
        XCTAssertEqual(GenieSpotter.spot("show me the crystal ball")?.arguments, ["trick": "genie_crystal"])
        XCTAssertEqual(GenieSpotter.spot("do a magic carpet ride")?.arguments, ["trick": "genie_carpet"])
        XCTAssertEqual(GenieSpotter.spot("do a magic trick")?.arguments, ["trick": "magic"])
        XCTAssertEqual(GenieSpotter.spot("levitate")?.arguments, ["trick": "levitate"])
        XCTAssertEqual(GenieSpotter.spot("rainbow")?.arguments, ["trick": "rainbow"])
        XCTAssertEqual(GenieSpotter.spot("tornado")?.arguments, ["trick": "tornado"])
        XCTAssertEqual(GenieSpotter.spot("poof")?.arguments, ["trick": "vanish"])
    }

    func testEveryEffectDrawsWhileItRunsAndNothingAfter() throws {
        for magic in GenieMagic.allCases {
            let trick = try trick(magic)
            var drawn = 0
            var shapes = Set<GenieGlimmerShape>()
            for step in 1..<20 {
                let glimmers = trick.glimmers(age: trick.duration * Double(step) / 20, stage: stage())
                drawn += glimmers.count
                for glimmer in glimmers {
                    shapes.insert(glimmer.shape)
                    XCTAssertTrue(glimmer.x.isFinite && glimmer.y.isFinite && glimmer.size.isFinite, magic.rawValue)
                    XCTAssertTrue((0...1).contains(glimmer.opacity), magic.rawValue)
                    XCTAssertGreaterThan(glimmer.size, 0, magic.rawValue)
                }
            }
            XCTAssertGreaterThan(drawn, 10, magic.rawValue)
            XCTAssertTrue(trick.glimmers(age: trick.duration, stage: stage()).isEmpty, magic.rawValue)
            XCTAssertTrue(trick.glimmers(age: -0.1, stage: stage()).isEmpty, magic.rawValue)
            XCTAssertFalse(shapes.isEmpty, magic.rawValue)
        }
        XCTAssertTrue(GenieTricks.named("backflip")?.glimmers(age: 0.5, stage: stage()).isEmpty ?? false)
    }

    func testDrawnShapesMatchTheirDescriptions() throws {
        func shapes(_ magic: GenieMagic) throws -> Set<GenieGlimmerShape> {
            let trick = try trick(magic)
            return Set((1..<40).flatMap { trick.glimmers(age: trick.duration * Double($0) / 40, stage: stage()).map(\.shape) })
        }
        let wanted: [GenieMagic: GenieGlimmerShape] = [
            .poof: .puff, .levitate: .puff, .lamp: .lamp, .wishes: .orb, .smokeRings: .ring, .shimmer: .band, .stardust: .dust, .carpet: .carpet,
            .vortex: .spark, .crystal: .crystal, .runes: .rune, .halo: .ring, .fireflies: .orb, .whirl: .puff, .grow: .puff, .bottle: .bottle,
            .rainbow: .arc, .fireworks: .spark, .bubbles: .bubble, .glitter: .dust, .jewelBeam: .beam, .ripple: .ring, .moons: .moon,
            .constellation: .line, .crescent: .moon, .mirage: .outline, .coins: .coin, .flourish: .line, .incense: .wisp, .whirlwind: .puff,
        ]
        XCTAssertEqual(Set(wanted.keys), Set(GenieMagic.allCases))
        for (magic, shape) in wanted {
            XCTAssertTrue(try shapes(magic).contains(shape), "\(magic.rawValue) draws no \(shape.rawValue)")
        }
        XCTAssertTrue(try shapes(.constellation).contains(.star))
        XCTAssertTrue(try shapes(.incense).contains(.glow))
        let rainbow = try trick(.rainbow).glimmers(age: 1.5, stage: stage()).filter { $0.shape == .arc }
        XCTAssertEqual(Set(rainbow.map(\.hue)).count, 6)
        let runes = try trick(.runes).glimmers(age: 1.6, stage: stage()).filter { $0.shape == .rune }
        XCTAssertEqual(Set(runes.map(\.variant)), Set(0..<6))
    }

    func testAimedEffectsReachTheTargetAndTheOthersStayNearTheSpider() throws {
        let spider = CGPoint(x: 300, y: 760)
        let near = target.insetBy(dx: -60, dy: -70)
        for magic in GenieMagic.allCases {
            let trick = try trick(magic)
            let all = (1..<40).flatMap { trick.glimmers(age: trick.duration * Double($0) / 40, stage: stage(spider: spider)) }
            let reached = all.contains { near.contains(CGPoint(x: $0.x, y: $0.y)) || (($0.shape == .beam || $0.shape == .arc) && near.contains(CGPoint(x: $0.x2, y: $0.y2))) }
            if magic.aims {
                XCTAssertTrue(reached, magic.rawValue)
            } else {
                XCTAssertFalse(reached, magic.rawValue)
                XCTAssertTrue(all.allSatisfy { hypot($0.x - Double(spider.x), $0.y - Double(spider.y)) < 92 * 1.6 }, magic.rawValue)
            }
        }
        XCTAssertEqual(GenieMagic.allCases.filter(\.aims).count, 21)
    }

    func testEffectsTurnWithTheSurfaceTheSpiderStandsOn() throws {
        let carpet = try trick(.carpet)
        let wall = CGPoint(x: 46, y: 400)
        let onLeft = carpet.glimmers(age: 1.6, stage: stage(turn: GenieEdge.left.degrees, spider: wall)).first { $0.shape == .carpet }
        XCTAssertLessThan(try XCTUnwrap(onLeft).x, Double(wall.x))
        let floor = CGPoint(x: 200, y: 800)
        let onFloor = carpet.glimmers(age: 1.6, stage: stage(turn: 0, spider: floor)).first { $0.shape == .carpet }
        XCTAssertGreaterThan(try XCTUnwrap(onFloor).y, Double(floor.y))
        let ceiling = carpet.glimmers(age: 1.6, stage: stage(turn: GenieEdge.top.degrees, spider: CGPoint(x: 200, y: 100))).first { $0.shape == .carpet }
        XCTAssertLessThan(try XCTUnwrap(ceiling).y, 100)
        let up = stage(turn: 0)
        XCTAssertLessThan(up.crown.y, up.spider.y)
        XCTAssertEqual(stage(turn: 90).up.x, 1, accuracy: 0.0001)
    }

    func testPoofSwapsCloudsHalfWay() throws {
        let poof = try trick(.poof)
        let before = poof.glimmers(age: poof.duration * 0.3, stage: stage())
        let after = poof.glimmers(age: poof.duration * 0.7, stage: stage())
        XCTAssertFalse(before.isEmpty)
        XCTAssertFalse(after.isEmpty)
        let gone = try XCTUnwrap(poof.frame(age: poof.duration * GenieMagic.swap))
        XCTAssertLessThan(gone.opacity, 0.1)
    }

    func testStandInTargetWithoutAnElement() {
        for (spider, turn) in [(CGPoint(x: 340, y: 800), 0.0), (CGPoint(x: 46, y: 420), 90.0), (CGPoint(x: 200, y: 96), 180.0), (CGPoint(x: 360, y: 300), -90.0)] {
            let staged = GenieMagicStage(spider: spider, side: 92, turn: turn, full: full)
            XCTAssertTrue(full.contains(staged.target), "\(spider)")
            XCTAssertFalse(staged.target.contains(spider), "\(spider)")
            XCTAssertGreaterThan(staged.target.width, 40)
            let middle = CGPoint(x: full.midX, y: full.midY)
            XCTAssertLessThan(hypot(staged.target.midX - middle.x, staged.target.midY - middle.y), hypot(spider.x - middle.x, spider.y - middle.y))
        }
    }

    func testGenieScreenActsPlayTheirEffectAtALandmark() throws {
        let genie = GenieScreenAct.allCases.filter { $0.magic != nil }
        XCTAssertEqual(genie.count, 23)
        XCTAssertEqual(Set(genie.compactMap(\.magic)), Set(GenieMagic.allCases.filter(\.aims) + [.poof, .crystal]))
        for act in genie {
            let magic = try XCTUnwrap(act.magic)
            XCTAssertEqual(act.trick, magic.trick)
            XCTAssertNotNil(GenieTricks.named(act.trick), act.rawValue)
            XCTAssertGreaterThanOrEqual(act.fits.count, 3, act.rawValue)
            XCTAssertFalse(act.needsText, act.rawValue)
            XCTAssertFalse(act.title.isEmpty)
            XCTAssertFalse(act.line("Settings").isEmpty)
            XCTAssertFalse(act.line("").isEmpty)
            for kind in act.fits {
                XCTAssertTrue(GenieScreenPlay.fits(act, GenieLandmark(kind: kind, rect: target, text: "")), "\(act.rawValue) \(kind)")
            }
        }
        XCTAssertEqual(GenieScreenAct.allCases.filter(\.blinks), [.poof])
        for kind in GenieLandmarkKind.allCases {
            XCTAssertTrue(genie.contains { $0.fits.contains(kind) }, kind.rawValue)
        }
    }

    func testScreenPlayBagHoldsEveryActOnceAndGenieActsComeUp() {
        let play = GenieScreenPlay()
        XCTAssertEqual(play.bag.items, GenieScreenAct.allCases)
        XCTAssertEqual(Array(GenieScreenAct.allCases.prefix(13)), [.look, .read, .waveAt, .pointAt, .eureka, .cheer, .sniff, .poke, .silk, .giggle, .salute, .blowKiss, .highFive])
        var state: UInt64 = 5
        let random: () -> Double = {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return Double(state >> 11) / Double(1 << 53)
        }
        let marks: [GenieLandmark] = [
            GenieLandmark(kind: .button, rect: CGRect(x: 340, y: 60, width: 44, height: 36), text: "Settings"),
            GenieLandmark(kind: .heading, rect: CGRect(x: 20, y: 130, width: 180, height: 30), text: "Agents"),
            GenieLandmark(kind: .image, rect: CGRect(x: 16, y: 60, width: 36, height: 36), text: "Logo"),
            GenieLandmark(kind: .text, rect: CGRect(x: 20, y: 230, width: 260, height: 20), text: "Three agents are running"),
        ]
        var bag = GenieScreenPlay()
        var seen: [GenieScreenAct] = []
        for _ in 0..<60 {
            if let choice = bag.choose(marks, from: CGPoint(x: 300, y: 754), random: random) {
                XCTAssertTrue(GenieScreenPlay.fits(choice.act, choice.mark))
                seen.append(choice.act)
            }
        }
        XCTAssertGreaterThanOrEqual(Set(seen.compactMap(\.magic)).count, 18)
    }
}
