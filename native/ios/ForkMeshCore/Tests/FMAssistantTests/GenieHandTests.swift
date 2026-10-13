import FMApi
import FMBase
import FMLocalAI
import XCTest
@testable import FMAssistant

@MainActor
private final class HandActions: GenieActions {
    var me = "ada"
    var paletteContext = PaletteContext()
    var commands: [PaletteCommand] = []

    func run(_ command: PaletteCommand) {
        commands.append(command)
    }

    func startAgent(prompt: String, repository: String?) async -> String {
        "Started an agent."
    }
}

private func hand(
    _ cx: Double, _ cy: Double, size s: Double = 0.17, fingers: Set<GenieFinger> = [.thumb, .index, .middle, .ring, .little],
    pinched: Bool = false, thumbUp: Bool = false, depth: Double? = 0.4
) -> GenieHandPose {
    var joints: [GenieJoint: CGPoint] = [.wrist: CGPoint(x: cx, y: cy + s * 0.5)]
    let spread: [(GenieFinger, Double)] = [(.index, -0.3), (.middle, -0.1), (.ring, 0.1), (.little, 0.3)]
    for (finger, dx) in spread {
        let knuckle = CGPoint(x: cx + dx * s, y: cy - s * 0.5)
        joints[finger.knuckle] = knuckle
        joints[finger.tip] = fingers.contains(finger)
            ? CGPoint(x: knuckle.x, y: knuckle.y - s * 0.8) : CGPoint(x: knuckle.x, y: knuckle.y + s * 0.35)
    }
    joints[.thumbKnuckle] = CGPoint(x: cx - s * 0.45, y: cy)
    if pinched {
        joints[.thumbTip] = joints[.indexTip]
    } else if thumbUp {
        joints[.thumbTip] = CGPoint(x: cx - s * 0.6, y: cy - s * 1.2)
    } else if fingers.contains(.thumb) {
        joints[.thumbTip] = CGPoint(x: cx - s * 0.9, y: cy - s * 0.1)
    } else {
        joints[.thumbTip] = CGPoint(x: cx - s * 0.25, y: cy - s * 0.3)
    }
    return GenieHandPose(joints: joints, depth: depth)
}

@MainActor
final class GenieHandTests: XCTestCase {
    private let start = Date(timeIntervalSince1970: 2_000_000)

    private func at(_ seconds: Double) -> Date {
        start.addingTimeInterval(seconds)
    }

    private func feed(_ reader: inout GenieHandReader, _ frames: [(Double, [GenieHandPose])]) -> [GenieHandEvent] {
        frames.flatMap { reader.read($0.1, at: at($0.0)) }
    }

    private func loaded() async -> FakeLocalChatEngine {
        let engine = FakeLocalChatEngine()
        try? await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: LocalModelCatalog.gemma4E2B)
        return engine
    }

    func testCatalogHasOverAHundredDistinctTricks() {
        XCTAssertGreaterThanOrEqual(GenieTricks.all.count, 100)
        XCTAssertEqual(Set(GenieTricks.all.map(\.id)).count, GenieTricks.all.count)
        XCTAssertEqual(Set(GenieTricks.all.map(\.title)).count, GenieTricks.all.count)
        for trick in GenieTricks.all {
            XCTAssertFalse(trick.line.isEmpty, trick.id)
            XCTAssertEqual(GenieTricks.find(trick.id)?.id, trick.id, trick.id)
            XCTAssertEqual(GenieTricks.find(trick.title)?.id, trick.id, trick.title)
            XCTAssertNotNil(trick.frame(age: 0), trick.id)
            XCTAssertNil(trick.frame(age: trick.duration), trick.id)
            let middle = trick.frame(age: trick.duration / 2)
            XCTAssertEqual(middle?.envelope ?? 0, 1, accuracy: 0.001, trick.id)
            XCTAssertGreaterThan(middle?.scaleX ?? 0, 0, trick.id)
            XCTAssertGreaterThan(middle?.opacity ?? 0, 0, trick.id)
        }
        let recipes = Set(GenieTricks.all.map { "\($0.legs)|\($0.eyes)|\($0.mouth)|\($0.body)|\($0.effect)|\($0.travel)" })
        XCTAssertEqual(recipes.count, GenieTricks.all.count)
        XCTAssertGreaterThanOrEqual(Set(GenieTricks.all.map(\.legs)).count, GenieLegMove.allCases.count - 1)
        XCTAssertEqual(Set(GenieTricks.all.map(\.effect)).count, GenieEffect.allCases.count)
        for pool in GenieHandPlay.pools.values.joined() {
            XCTAssertNotNil(GenieTricks.named(pool), pool)
        }
    }

    func testTrickFramesMove() async throws {
        let flip = try XCTUnwrap(GenieTricks.named("backflip"))
        let top = try XCTUnwrap(flip.frame(age: flip.duration / 2))
        XCTAssertEqual(top.dy, -28, accuracy: 0.01)
        XCTAssertEqual(top.rotation, -180, accuracy: 0.5)
        XCTAssertEqual(flip.frame(age: flip.duration - 0.0001)?.rotation ?? 0, -360, accuracy: 0.5)
        let vanish = try XCTUnwrap(GenieTricks.named("vanish")?.frame(age: 0.9))
        XCTAssertLessThan(vanish.opacity, 0.2)
        let hearts = try XCTUnwrap(GenieTricks.named("love")?.frame(age: 1))
        XCTAssertEqual(hearts.particles.count, 5)
        XCTAssertTrue(hearts.particles.allSatisfy { ["💖", "💕", "💗"].contains($0.glyph) })
        XCTAssertTrue(GenieTricks.named("web_swing")?.frame(age: 1)?.particles.isEmpty ?? false)
    }

    func testFindingAndPickingTricks() {
        XCTAssertEqual(GenieTricks.find("do a back flip")?.id, "backflip")
        XCTAssertEqual(GenieTricks.find("Air Guitar")?.id, "air_guitar")
        XCTAssertEqual(GenieTricks.find("moonwalk")?.id, "moonwalk")
        XCTAssertNil(GenieTricks.find("surprise"))
        XCTAssertTrue(GenieTricks.isSurprise("surprise me"))
        XCTAssertTrue(GenieTricks.isSurprise(""))
        XCTAssertFalse(GenieTricks.isSurprise("tap dance"))
        var taste = GenieTrickTaste()
        taste.rate("moonwalk", 5)
        taste.rate("sulk", -3)
        taste.rate("not_a_trick", 3)
        XCTAssertEqual(taste.favourites, ["moonwalk"])
        XCTAssertEqual(taste.dislikes, ["sulk"])
        XCTAssertEqual(GenieTrickTaste(encoded: taste.encoded), taste)
        var counts: [String: Int] = [:]
        var seed: UInt64 = 42
        for _ in 0..<2_000 {
            let trick = GenieTricks.pick(taste: taste) {
                seed = seed &* 6_364_136_223_846_793_005 &+ 1
                return Double(seed >> 11) / Double(1 << 53)
            }
            counts[trick.id, default: 0] += 1
        }
        XCTAssertGreaterThan(counts["moonwalk"] ?? 0, (counts["dance"] ?? 0) * 3)
        XCTAssertLessThan(counts["sulk"] ?? 0, (counts["dance"] ?? 0) + 5)
        let ids = GenieTricks.all.map(\.id)
        XCTAssertEqual(GenieTricks.pick(recent: Array(ids.prefix(8))) { 0 }.id, ids[8])
    }

    func testHandShapes() {
        XCTAssertEqual(hand(0.5, 0.5).shape, .open)
        XCTAssertEqual(hand(0.5, 0.5, fingers: []).shape, .fist)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.index]).shape, .point)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.thumb, .index]).shape, .gun)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.index, .middle]).shape, .peace)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.index, .little]).shape, .rock)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.thumb, .little]).shape, .callMe)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.index, .middle, .ring, .little]).shape, .four)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [], thumbUp: true).shape, .thumbsUp)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.index], pinched: true).shape, .pinch)
        XCTAssertEqual(hand(0.5, 0.5, fingers: [.index, .middle, .ring, .little], pinched: true).shape, .ok)
        XCTAssertEqual(hand(0.5, 0.5).fingerCount, 5)
        XCTAssertEqual(hand(0.5, 0.5, size: 0.17, depth: nil).distance, GenieHandReader.armsLength, accuracy: 0.005)
        XCTAssertGreaterThan(
            GenieHandReader.nearness(hand(0.5, 0.5, size: 0.3, depth: nil).distance), GenieHandReader.nearness(hand(0.5, 0.5, size: 0.1, depth: nil).distance))
    }

    func testPinchDragAndRelease() {
        var reader = GenieHandReader()
        var events = feed(&reader, [(0, [hand(0.3, 0.5, fingers: [.index])]), (0.033, [hand(0.3, 0.5, fingers: [.index], pinched: true)])])
        XCTAssertEqual(events, [.appeared(1)])
        events = feed(&reader, [(0.066, [hand(0.3, 0.5, fingers: [.index], pinched: true)])])
        guard case .pinchBegan = events.first else { return XCTFail("\(events)") }
        XCTAssertTrue(reader.state.pinching)
        var frames: [(Double, [GenieHandPose])] = []
        for k in 1...8 { frames.append((0.066 + Double(k) * 0.033, [hand(0.3 + Double(k) * 0.05, 0.5, fingers: [.index], pinched: true)])) }
        events = feed(&reader, frames)
        XCTAssertTrue(events.isEmpty, "\(events)")
        XCTAssertGreaterThan(Double(reader.state.cursor?.x ?? 0), 0.55)
        events = feed(&reader, [(0.4, [hand(0.72, 0.5, fingers: [.index])]), (0.433, [hand(0.74, 0.5, fingers: [.index])])])
        guard case .pinchEnded(_, let velocity) = events.last else { return XCTFail("\(events)") }
        XCTAssertGreaterThan(Double(velocity.width), 0.5)
        XCTAssertFalse(reader.state.pinching)
    }

    func testPushAndPullUseTheDepth() {
        var reader = GenieHandReader()
        var events = feed(&reader, (0...6).map { k in (Double(k) * 0.04, [hand(0.5, 0.5, depth: 0.42 - Double(k) * 0.02)]) })
        XCTAssertTrue(events.contains { if case .push(let amount) = $0 { return amount >= 0.07 } else { return false } }, "\(events)")
        XCTAssertTrue(reader.state.depthSensed)
        XCTAssertGreaterThan(reader.state.nearness, 0.2)
        events = feed(&reader, (0...6).map { k in (1.5 + Double(k) * 0.04, [hand(0.5, 0.5, depth: 0.3 + Double(k) * 0.025)]) })
        XCTAssertTrue(events.contains { if case .pull = $0 { return true } else { return false } }, "\(events)")

        var flat = GenieHandReader()
        events = feed(&flat, (0...6).map { k in (Double(k) * 0.04, [hand(0.5, 0.5, size: 0.15 + Double(k) * 0.012, depth: nil)]) })
        XCTAssertTrue(events.contains { if case .push = $0 { return true } else { return false } }, "\(events)")
        XCTAssertFalse(flat.state.depthSensed)
    }

    func testSwipeWaveAndHeldShapes() {
        var reader = GenieHandReader()
        var events = feed(&reader, (0...6).map { k in (Double(k) * 0.04, [hand(0.2 + Double(k) * 0.07, 0.5)]) })
        XCTAssertTrue(events.contains(.swipe(.right)), "\(events)")

        reader = GenieHandReader()
        events = feed(&reader, (0...40).map { k in
            let t = Double(k) / 30
            return (t, [hand(0.5 + sin(t * Double.pi * 2 * 2.5) * 0.06, 0.5)])
        })
        XCTAssertTrue(events.contains(.wave), "\(events)")
        XCTAssertFalse(events.contains { if case .swipe = $0 { return true } else { return false } }, "\(events)")

        reader = GenieHandReader()
        events = feed(&reader, (0...20).map { k in (Double(k) / 30, [hand(0.5, 0.5, fingers: [], thumbUp: true)]) })
        XCTAssertEqual(events.filter { $0 == .shape(.thumbsUp) }.count, 1, "\(events)")
    }

    func testCircleClapAndPeekaboo() {
        var reader = GenieHandReader()
        var events = feed(&reader, (0...45).map { k in
            let angle = Double(k) / 45 * Double.pi * 2.2
            return (Double(k) / 30, [hand(0.5 + cos(angle) * 0.15, 0.5 + sin(angle) * 0.15, fingers: [.index])])
        })
        XCTAssertTrue(events.contains(.circle(clockwise: true)), "\(events)")

        reader = GenieHandReader()
        events = feed(&reader, [
            (0, [hand(0.15, 0.5), hand(0.85, 0.5)]), (0.1, [hand(0.3, 0.5), hand(0.7, 0.5)]), (0.2, [hand(0.47, 0.5), hand(0.53, 0.5)]),
        ])
        XCTAssertTrue(events.contains(.clap), "\(events)")

        reader = GenieHandReader()
        events = feed(&reader, [(0, [hand(0.5, 0.5)]), (0.1, []), (0.7, []), (2.5, [hand(0.5, 0.5)])])
        XCTAssertEqual(events.first, .appeared(1))
        XCTAssertTrue(events.contains(.vanished))
        XCTAssertTrue(events.contains { if case .returned(let after) = $0 { return after > 2 } else { return false } }, "\(events)")
    }

    func testTwoHandsTurnAndResize() async throws {
        var reader = GenieHandReader()
        var events = feed(&reader, [
            (0, [hand(0.3, 0.5, fingers: [.index], pinched: true, depth: 0.4), hand(0.7, 0.5, fingers: [.index], pinched: true, depth: 0.4)]),
        ])
        XCTAssertTrue(events.contains(.twoHandsBegan), "\(events)")
        for k in 1...12 {
            let gap = 0.2 + Double(k) * 0.02
            _ = reader.read([
                hand(0.5 - gap, 0.5 + Double(k) * 0.01, fingers: [.index], pinched: true, depth: 0.4 - Double(k) * 0.005),
                hand(0.5 + gap, 0.5 - Double(k) * 0.01, fingers: [.index], pinched: true, depth: 0.4),
            ], at: at(Double(k) * 0.033))
        }
        let turned = try XCTUnwrap(reader.state.manipulation)
        XCTAssertGreaterThan(turned.scale, 1.6)
        XCTAssertLessThan(turned.rotation, -5)
        XCTAssertGreaterThan(turned.yaw, 10)
        events = feed(&reader, [(0.5, [hand(0.3, 0.5)]), (0.53, [hand(0.3, 0.5)]), (0.56, [hand(0.3, 0.5)])])
        guard case .twoHandsEnded(let done)? = events.first(where: { if case .twoHandsEnded = $0 { return true } else { return false } }) else {
            return XCTFail("\(events)")
        }
        XCTAssertGreaterThan(done.scale, 1.6)
        XCTAssertNil(reader.state.manipulation)
    }

    func testPlayGrabsCarriesAndFlings() {
        var play = GenieHandPlay()
        let scene = GenieHandScene(spider: CGPoint(x: 0.5, y: 0.5), reach: 0.12)
        var state = GenieHandState()
        state.hands = 1
        state.cursor = CGPoint(x: 0.52, y: 0.5)
        XCTAssertEqual(play.react(.pinchBegan(CGPoint(x: 0.52, y: 0.5)), state: state, scene: scene, at: start), [.grab])
        state.pinching = true
        state.cursor = CGPoint(x: 0.7, y: 0.6)
        XCTAssertEqual(play.tick(state: state, scene: scene, at: at(0.2)), [.carry(CGPoint(x: 0.7, y: 0.6))])
        let thrown = play.react(.pinchEnded(CGPoint(x: 0.7, y: 0.6), velocity: CGSize(width: 2, height: 0)), state: state, scene: scene, at: at(0.3))
        XCTAssertEqual(thrown.first, .drop(fling: CGSize(width: 2, height: 0)))
        XCTAssertEqual(thrown.last, .trick("dizzy", cause: "throw"))
        XCTAssertFalse(play.grabbed)
        XCTAssertEqual(play.react(.pinchBegan(CGPoint(x: 0.1, y: 0.1)), state: state, scene: scene, at: at(1)), [.crawl(to: CGPoint(x: 0.1, y: 0.1))])
        state.cursor = CGPoint(x: 0.51, y: 0.5)
        let boop = play.react(.push(0.08), state: state, scene: scene, at: at(2))
        XCTAssertEqual(boop.first, .recoil(from: CGPoint(x: 0.51, y: 0.5)))
        XCTAssertEqual(boop.last, .trick("giggle", cause: "boop"))
    }

    func testPlayVariesRepeatsAndLearnsWhatIsLiked() {
        var play = GenieHandPlay()
        let scene = GenieHandScene(spider: CGPoint(x: 0.5, y: 0.5), reach: 0.12)
        var state = GenieHandState()
        state.hands = 1
        state.cursor = CGPoint(x: 0.9, y: 0.9)
        let first = play.react(.swipe(.up), state: state, scene: scene, at: start)
        XCTAssertEqual(first, [.trick("jump", cause: "swipe up")])
        play.played("jump", at: start)
        let second = play.react(.swipe(.up), state: state, scene: scene, at: at(2))
        XCTAssertEqual(second, [.trick("blast_off", cause: "swipe up")])
        play.played("blast_off", at: at(2))
        let third = play.react(.swipe(.up), state: state, scene: scene, at: at(4))
        guard case .improvise(let why)? = third.first else { return XCTFail("\(third)") }
        XCTAssertTrue(why.contains("swipe up"), why)
        let liked = play.react(.shape(.thumbsUp), state: state, scene: scene, at: at(5))
        XCTAssertEqual(liked.first, .rate("blast_off", 1))
        XCTAssertEqual(liked.last, .trick("dance", cause: "a thumbs up"))
        let booed = play.react(.shape(.thumbsDown), state: state, scene: scene, at: at(30))
        XCTAssertFalse(booed.contains { if case .rate = $0 { return true } else { return false } })
    }

    func testPettingAndIdleImprovisation() {
        var play = GenieHandPlay()
        let scene = GenieHandScene(spider: CGPoint(x: 0.5, y: 0.5), reach: 0.12)
        var state = GenieHandState()
        state.hands = 1
        state.cursor = CGPoint(x: 0.5, y: 0.47)
        state.shape = .open
        XCTAssertEqual(play.tick(state: state, scene: scene, at: start), [])
        XCTAssertEqual(play.tick(state: state, scene: scene, at: at(1.3)), [.trick("purr", cause: "pet")])
        state.cursor = CGPoint(x: 0.9, y: 0.1)
        state.nearness = 0.4
        _ = play.react(.shape(.open), state: state, scene: scene, at: at(1.4))
        XCTAssertEqual(play.tick(state: state, scene: scene, at: at(5)), [])
        let idle = play.tick(state: state, scene: scene, at: at(9))
        guard case .improvise(let why)? = idle.first else { return XCTFail("\(idle)") }
        XCTAssertTrue(why.contains("open palm"), why)
        XCTAssertTrue(why.contains("40%"), why)
        XCTAssertEqual(play.tick(state: state, scene: GenieHandScene(spider: scene.spider, reach: 0.12, busy: true), at: at(30)), [])
    }

    func testImproviserParsesAndFallsBack() async {
        XCTAssertEqual(GenieImproviser.parse("moonwalk: Smooth criminal!")?.trick.id, "moonwalk")
        XCTAssertEqual(GenieImproviser.parse("moonwalk: Smooth criminal!")?.line, "Smooth criminal!")
        XCTAssertEqual(GenieImproviser.parse("{\"trick\": \"air_guitar\", \"line\": \"Shred!\"}")?.line, "Shred!")
        XCTAssertEqual(GenieImproviser.parse("I think I'll do a **backflip** now")?.trick.id, "backflip")
        XCTAssertEqual(GenieImproviser.parse("Air guitar: \"Wah!\"")?.trick.id, "air_guitar")
        XCTAssertNil(GenieImproviser.parse("Hello there."))

        let engine = await loaded()
        engine.enqueueReply(["jug", "gle: Watch my eight legs!"])
        let model = GenieModel(name: "Fake", engine: engine)
        var taste = GenieTrickTaste()
        taste.rate("moonwalk", 3)
        let done = await GenieImproviser.improvise(scene: "They waved.", recent: ["wave"], taste: taste, vitals: nil, model: model)
        XCTAssertEqual(done, GenieImprovisation(trick: GenieTricks.named("juggle")!, line: "Watch my eight legs!", byModel: true))
        let asked = engine.requests.last?.messages ?? []
        XCTAssertTrue(asked.first?.content.contains("They loved: moonwalk.") ?? false)
        XCTAssertTrue(asked.last?.content.contains("You just did: wave.") ?? false)
        XCTAssertEqual(engine.requests.last?.parameters.maxTokens, 48)

        engine.enqueueReply(["I would rather not."])
        let fallback = await GenieImproviser.improvise(scene: "x", recent: [], taste: GenieTrickTaste(), vitals: nil, model: model) { 0 }
        XCTAssertFalse(fallback.byModel)
        XCTAssertEqual(fallback.trick.id, GenieTricks.all.first?.id)
        let none = await GenieImproviser.improvise(scene: "x", recent: [], taste: GenieTrickTaste(), vitals: nil, model: nil) { 0 }
        XCTAssertFalse(none.byModel)

        let slow = await loaded()
        slow.enqueue([.hang])
        let late = await GenieImproviser.improvise(scene: "x", recent: [], taste: GenieTrickTaste(), vitals: nil, model: GenieModel(name: "Slow", engine: slow), timeout: 0.2) { 0 }
        XCTAssertFalse(late.byModel)
    }

    func testSpotsTricksWithoutStealingOtherRequests() {
        XCTAssertEqual(GenieSpotter.spot("do a backflip")?.arguments, ["trick": "backflip"])
        XCTAssertEqual(GenieSpotter.spot("do a backflip")?.tool, GenieTricks.tool)
        XCTAssertEqual(GenieSpotter.spot("dance")?.arguments, ["trick": "dance"])
        XCTAssertEqual(GenieSpotter.spot("can you dance for me")?.arguments, ["trick": "dance"])
        XCTAssertEqual(GenieSpotter.spot("genie, give me a hug")?.arguments, ["trick": "hug"])
        XCTAssertTrue(GenieSpotter.spot("genie, give me a hug")?.addressed ?? false)
        XCTAssertEqual(GenieSpotter.spot("show me a trick")?.arguments, ["trick": "surprise"])
        XCTAssertEqual(GenieSpotter.spot("surprise me")?.arguments, ["trick": "surprise"])
        XCTAssertEqual(GenieSpotter.spot("play air guitar please")?.arguments, ["trick": "air_guitar"])
        XCTAssertEqual(GenieSpotter.spot("show me your moves")?.arguments, ["trick": "dance"])
        XCTAssertNotEqual(GenieSpotter.spot("jump to settings")?.tool, GenieTricks.tool)
        XCTAssertNotEqual(GenieSpotter.spot("show me settings")?.tool, GenieTricks.tool)
        XCTAssertEqual(GenieSpotter.spot("spin up an agent to fix the tests")?.tool, "start_agent")
        XCTAssertNotEqual(GenieSpotter.spot("hide the keyboard")?.tool, GenieTricks.tool)
        XCTAssertNotEqual(GenieSpotter.spot("what can you do")?.tool, GenieTricks.tool)
        XCTAssertNotEqual(GenieSpotter.spot("how are you feeling")?.tool, GenieTricks.tool)
    }

    func testToolboxPerformsTricks() async {
        let toolbox = GenieToolbox(data: nil, actions: HandActions())
        let offscreen = await toolbox.performTrick("dance")
        XCTAssertEqual(offscreen, "I can only do tricks while I'm on screen.")
        var played: [String?] = []
        toolbox.tricks = { wanted in
            played.append(wanted?.id)
            let trick = wanted ?? GenieTricks.named("juggle")!
            return GenieImprovisation(trick: trick, line: trick.line, byModel: wanted == nil)
        }
        let danced = await toolbox.invoke(LocalToolCall(id: "1", name: "do_a_trick", arguments: ["move": "moonwalk"]))
        XCTAssertEqual(danced.text, "Hee-hee!")
        let surprise = await toolbox.invoke(LocalToolCall(id: "2", name: GenieTricks.tool, arguments: ["trick": "surprise me"]))
        XCTAssertEqual(surprise.text, "Three balls, eight legs. Easy.")
        let unknown = await toolbox.invoke(LocalToolCall(id: "3", name: GenieTricks.tool, arguments: ["trick": "quantum tango"]))
        XCTAssertTrue(unknown.text.hasPrefix("I don't know quantum tango yet, so here's my juggle."), unknown.text)
        XCTAssertEqual(played, ["moonwalk", nil, nil])
        let approval = await toolbox.approval(for: LocalToolCall(id: "4", name: GenieTricks.tool, arguments: [:]))
        XCTAssertNil(approval)
        let tricks = GenieCapabilities.describe(topic: "tricks", toolNames: toolbox.toolNames)
        XCTAssertTrue(tricks.contains("Hand play"), tricks)
    }

    func testBrainDoesSpokenTricksAtOnce() async {
        let engine = await loaded()
        let toolbox = GenieToolbox(data: nil, actions: HandActions())
        var played: [String] = []
        toolbox.tricks = { wanted in
            played.append(wanted?.id ?? "?")
            return wanted.map { GenieImprovisation(trick: $0, line: $0.line, byModel: false) }
        }
        let brain = GenieBrain(
            toolbox: toolbox, memory: GenieMemory(store: InMemoryKeyValueStore()),
            model: GenieModel(name: "Fake", engine: engine))
        let outcome = await brain.begin("do a cartwheel")
        XCTAssertEqual(outcome, .reply("Look at me go!"))
        XCTAssertEqual(played, ["cartwheel"])
        XCTAssertTrue(engine.requests.isEmpty)
    }

    func testToursStayOnScreen() {
        let roam = CGRect(x: 50, y: 100, width: 300, height: 500)
        let from = CGPoint(x: 200, y: 300)
        for travel in GenieTravel.allCases {
            let points = GenieCrawl.tour(travel, from: from, roam: roam, toward: CGPoint(x: 120, y: 200))
            XCTAssertEqual(points.isEmpty, travel == .none, "\(travel)")
            XCTAssertTrue(points.allSatisfy { roam.insetBy(dx: -0.01, dy: -0.01).contains($0) }, "\(travel)")
        }
        XCTAssertEqual(GenieCrawl.tour(.rope, from: from, roam: roam).last, from)
        XCTAssertEqual(GenieCrawl.tour(.toHand, from: from, roam: roam, toward: CGPoint(x: 0, y: 0)), [CGPoint(x: 50, y: 100)])
        XCTAssertEqual(GenieCrawl.tour(.flee, from: from, roam: roam, toward: CGPoint(x: 100, y: 300)).first?.x, 350)
        let bounced = GenieCrawl.landing(from: CGPoint(x: 300, y: 300), velocity: CGSize(width: 400, height: 0), in: roam)
        XCTAssertEqual(bounced.x, 260, accuracy: 0.01)
        XCTAssertTrue(roam.contains(GenieCrawl.landing(from: from, velocity: CGSize(width: -5000, height: 9000), in: roam)))
    }
}
