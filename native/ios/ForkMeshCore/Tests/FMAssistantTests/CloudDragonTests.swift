import Foundation
import XCTest
@testable import FMAssistant

final class CloudDragonTests: XCTestCase {
    private static let here: URL = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    private static let resources: URL = here.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        .appendingPathComponent("ForkMesh/Resources/CloudDragon")

    private static let atlas: DragonAtlas = {
        guard let data = try? Data(contentsOf: resources.appendingPathComponent("cloud-dragon-atlas.json")),
            let atlas = try? DragonAtlas(json: data)
        else { fatalError("the Cloud Dragon atlas is missing") }
        return atlas
    }()

    private var atlas: DragonAtlas { CloudDragonTests.atlas }

    private func scene(_ t: Double = 3) -> DragonScene {
        var s: DragonScene = DragonScene()
        s.t = t
        return s
    }

    private func run(_ body: inout DragonBody, _ s: inout DragonScene, frames: Int, each: (inout DragonScene) -> Void = { _ in })
        -> DragonFrame
    {
        var last: DragonFrame = body.frame(s, atlas: atlas)
        for _ in 0..<frames {
            s.t += 1.0 / 60
            each(&s)
            last = body.frame(s, atlas: atlas)
        }
        return last
    }

    private func finite(_ frame: DragonFrame, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertTrue(frame.q.allSatisfy { $0.isFinite }, file: file, line: line)
        for step in frame.steps {
            switch step {
            case .layer(_, let xf, let alpha):
                XCTAssertTrue(xf.isFinite && alpha.isFinite, file: file, line: line)
            case .tail(let part):
                XCTAssertTrue(part.xf.isFinite, file: file, line: line)
            case .gear(let gear):
                XCTAssertTrue(gear.x.isFinite && gear.y.isFinite && gear.scale.isFinite, file: file, line: line)
            case .head(let head):
                XCTAssertTrue(head.xf.isFinite && head.earXf.isFinite, file: file, line: line)
            }
        }
    }

    func testTheAtlasHasEveryLayerPropAndJoint() {
        for layer in DragonLayer.allCases {
            let frame: DragonLayerFrame = atlas.layer(layer)
            XCTAssertTrue(frame.x >= 0 && frame.y >= 0 && frame.w > 0 && frame.h > 0, layer.rawValue)
        }
        XCTAssertGreaterThan(atlas.tail.curve.count, 8)
        for act in DragonActivity.allCases {
            for gear in act.gear { XCTAssertNotNil(atlas.props[gear.prop], "\(act.rawValue): \(gear.prop)") }
        }
        for name in ["flame", "magnifier", "binoculars", "globe", "upcloud", "arrow", "bed", "plane", "zee", "loveheart", "sparkle", "cup"] {
            XCTAssertNotNil(atlas.props[name], name)
        }
        XCTAssertEqual(atlas.props.count, 20)
        XCTAssertEqual(atlas.units, 0.07243, accuracy: 0.00001)
    }

    func testTheAtlasRefusesBrokenJSON() {
        XCTAssertThrowsError(try DragonAtlas(json: Data("[]".utf8)))
        XCTAssertThrowsError(try DragonAtlas(json: Data("{\"joints\":{}}".utf8)))
    }

    func testTheTwentyActivitiesKeepTheirNamesAndTimes() {
        let names: [String] = [
            "breathe-fire", "land", "terminal", "coffee", "book", "magnify", "celebrate", "headphones", "code-fast", "upload",
            "protect", "build", "share", "binoculars", "travel", "sleep", "stretch", "star", "globe", "heart",
        ]
        XCTAssertEqual(DragonActivity.allCases.map(\.rawValue), names)
        let seconds: [Double] = [3, 2.4, 3.6, 4, 4, 3.6, 3.2, 4.2, 2.8, 3.8, 3.4, 3.2, 3.4, 3.6, 3.4, 6, 2.4, 3.2, 4.2, 3.2]
        XCTAssertEqual(DragonActivity.allCases.map(\.duration), seconds)
        for act in DragonActivity.allCases {
            XCTAssertGreaterThan(act.title.count, 8, act.rawValue)
            XCTAssertFalse(act.fits.isEmpty, act.rawValue)
        }
        XCTAssertEqual(DragonActivity.travel.stride(DragonBeat(age: 2, duration: 3.4, t: 0)), 18)
        XCTAssertNil(DragonActivity.coffee.stride(DragonBeat(age: 2, duration: 4, t: 0)))
    }

    func testPosesStayFiniteFromFirstMomentToLast() {
        for act in DragonActivity.allCases {
            var u: Double = 0
            while u <= 1.0001 {
                let pose: DragonTarget = act.pose(DragonBeat(age: act.duration * u, duration: act.duration, t: 3))
                XCTAssertFalse(pose.isEmpty, act.rawValue)
                XCTAssertTrue(pose.values.allSatisfy { $0.isFinite }, act.rawValue)
                u += 0.05
            }
        }
        for reaction in DragonReaction.allCases {
            XCTAssertTrue(reaction.pose(age: 0.4).values.allSatisfy { $0.isFinite }, reaction.rawValue)
        }
        for habit in DragonHabit.all {
            var u: Double = 0
            while u < 1 {
                XCTAssertTrue(habit.pose(u).values.allSatisfy { $0.isFinite }, habit.name)
                u += 0.1
            }
        }
    }

    func testEveryActivityPaintsFromItsFirstMomentToItsLast() {
        for act in DragonActivity.allCases {
            var body: DragonBody = DragonBody()
            var u: Double = 0
            while u <= 1.0001 {
                var s: DragonScene = scene(2 + u)
                s.activity = act
                s.activityAge = act.duration * u
                finite(body.frame(s, atlas: atlas))
                u += 0.1
            }
        }
    }

    func testIdleWalkingFlyingHeldAndReactionsAllPaint() {
        var shapes: [(inout DragonScene) -> Void] = [
            { _ in }, { $0.walking = 1 }, { $0.air = 1; $0.vy = 900 }, { $0.held = true; $0.vx = 400 }, { $0.land = 0.8 },
            { $0.gaze = DragonPoint(0.5, -0.2) }, { $0.reduced = true }, { $0.hover = true }, { $0.ambient = .sleep; $0.ambientAge = 3 },
            { $0.ambient = .stretch; $0.ambientAge = 1 },
        ]
        for reaction in DragonReaction.allCases {
            shapes.append({ $0.reaction = reaction; $0.reactionAge = 0.3 })
        }
        for habit in DragonHabit.all {
            shapes.append({ $0.habit = DragonHabitCue(name: habit.name, u: 0.5) })
        }
        for shape in shapes {
            var body: DragonBody = DragonBody()
            var s: DragonScene = scene()
            s.stridePhase = 1
            shape(&s)
            var frame: DragonFrame = body.frame(s, atlas: atlas)
            for _ in 0..<30 {
                s.t += 1.0 / 60
                frame = body.frame(s, atlas: atlas)
            }
            finite(frame)
        }
    }

    func testTheBodyTurnsRoundInStagesAndEndsMirrored() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene(1)
        s.facing = 1
        _ = body.frame(s, atlas: atlas)
        s.facing = -1
        s.t += 0.08
        _ = body.frame(s, atlas: atlas)
        XCTAssertLessThan(body.flips.head, body.flips.tail)
        var narrowest: Double = 1
        for _ in 0..<90 {
            s.t += 1.0 / 60
            _ = body.frame(s, atlas: atlas)
            narrowest = min(narrowest, abs(DragonFlips.seen(body.flips.body)))
        }
        var mirrored: DragonFlips = DragonFlips()
        mirrored.head = -1
        mirrored.body = -1
        mirrored.wing = -1
        mirrored.tail = -1
        XCTAssertEqual(body.flips, mirrored)
        XCTAssertGreaterThanOrEqual(narrowest, 0.56 - 0.0001)
        XCTAssertEqual(DragonFlips.seen(0), 0.56, accuracy: 0.0001)
        XCTAssertEqual(DragonFlips.seen(-1), -1, accuracy: 0.0001)
    }

    func testTurningHopsAndBlinks() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene(1)
        s.facing = 1
        _ = body.frame(s, atlas: atlas)
        s.facing = -1
        var highest: Double = 0
        var blink: Double = 0
        for _ in 0..<20 {
            s.t += 1.0 / 60
            let frame: DragonFrame = body.frame(s, atlas: atlas)
            highest = max(highest, -frame.value(.y))
            for case .head(let head) in frame.steps { blink = max(blink, head.blink) }
        }
        XCTAssertGreaterThan(highest, 10)
        XCTAssertGreaterThan(blink, 0.3)
    }

    func testTheTailCurlsWhenSittingAndTrailsOutStraightWhenWalking() {
        func spread(_ walking: Double) -> Double {
            var body: DragonBody = DragonBody()
            var s: DragonScene = scene(1)
            s.walking = walking
            _ = run(&body, &s, frames: 180)
            let aim: [Double] = body.tailAim
            return aim.map { abs(DragonMath.wrapped($0 - aim[0])) }.max() ?? 0
        }
        XCTAssertGreaterThan(spread(0), 2)
        XCTAssertLessThan(spread(1), 1.2)
    }

    func testTheTailBendsAlongItsWholeLength() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene(1)
        s.walking = 1
        _ = run(&body, &s, frames: 120) { $0.stridePhase += 0.2 }
        XCTAssertEqual(body.tailAim.count, atlas.tail.curve.count - 1)
        XCTAssertTrue(zip(body.tailAim, body.tailRest).contains { abs($0 - $1) > 0.05 })
        XCTAssertTrue(body.tailAim.allSatisfy { $0.isFinite })
    }

    func testTheTailIsOneStripOnTwentyEightSegmentsTuckedBehindTheBody() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        let frame: DragonFrame = run(&body, &s, frames: 30)
        XCTAssertEqual(frame.tail.segments, 28)
        XCTAssertEqual(frame.tail.triangles(from: 0, to: 28).count, 56)
        var order: [String] = []
        var tuck: Int = 0
        for step in frame.steps {
            switch step {
            case .tail(let part):
                order.append(part.from == 0 ? "tail-back" : "tail-front")
                if part.from == 0 { tuck = part.to }
            case .layer(.body, _, _): order.append("body")
            case .layer(.footNear, _, _): order.append("foot")
            default: break
            }
        }
        XCTAssertEqual(order, ["tail-back", "body", "foot", "tail-front"])
        XCTAssertEqual(tuck, 12)
        XCTAssertEqual(Double(tuck) / 28, 0.42, accuracy: 0.02)
    }

    func testTheTailRootStaysAtTheBodyWhateverThePose() {
        let root: DragonPoint = atlas.tail.curve[0]
        for walking in [0.0, 1.0] {
            var body: DragonBody = DragonBody()
            var s: DragonScene = scene()
            s.walking = walking
            let frame: DragonFrame = run(&body, &s, frames: 120) { $0.stridePhase += 0.15 }
            let first: DragonPoint = frame.tail.points[0]
            XCTAssertLessThan(abs(first.x - root.x), 80)
            XCTAssertLessThan(abs(first.y - root.y), 80)
            XCTAssertEqual(frame.tail.outer.count, 29)
            XCTAssertEqual(frame.tail.inner.count, 29)
            XCTAssertLessThanOrEqual(frame.tail.points.map { $0.y }.max() ?? 0, 1214.001)
        }
    }

    func testTheTailMeshStaysFiniteAndCoversTheStrip() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.walking = 1
        let frame: DragonFrame = run(&body, &s, frames: 90) { $0.stridePhase += 0.2 }
        for triangle in frame.tail.triangles(from: 0, to: 28) {
            XCTAssertEqual(triangle.clip.count, 3)
            XCTAssertTrue(triangle.xf.isFinite)
            XCTAssertTrue(triangle.clip.allSatisfy { $0.x.isFinite && $0.y.isFinite })
        }
        for i in 0..<28 {
            let column: (x: Double, w: Double) = frame.tail.column(i)
            XCTAssertGreaterThanOrEqual(column.x, 0)
            XCTAssertGreaterThan(column.w, 0)
            XCTAssertLessThanOrEqual(column.x + column.w, atlas.layer(.tailStrip).w + 0.0001)
        }
    }

    func testPlantedFeetMoveBackAtExactlyBodySpeedAndLiftedFeetSwingForward() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.walking = 1
        _ = run(&body, &s, frames: 120)
        func foot(_ phase: Double) -> DragonPoint {
            s.stridePhase = phase
            s.t += 1.0 / 60
            let frame: DragonFrame = body.frame(s, atlas: atlas)
            for case .layer(.footNear, let xf, _) in frame.steps { return xf.apply(atlas.joints.footNear) }
            return DragonPoint(0, 0)
        }
        let bodyPerRadian: Double = 236 / Double.pi
        let a: DragonPoint = foot(Double.pi * 1.4)
        let b: DragonPoint = foot(Double.pi * 1.4 + 0.1)
        XCTAssertEqual((b.x - a.x) / 0.1, -bodyPerRadian, accuracy: 3)
        XCTAssertEqual(b.y, a.y, accuracy: 0.5)
        let c: DragonPoint = foot(Double.pi * 0.4)
        let d: DragonPoint = foot(Double.pi * 0.4 + 0.1)
        XCTAssertGreaterThan(d.x, c.x)
    }

    func testTheStrideIsAFunctionOfPhaseAndNotOfTime() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.walking = 1
        _ = run(&body, &s, frames: 60)
        var feet: [DragonPoint] = []
        for time in [0.0, 5.0] {
            s.stridePhase = Double.pi * 1.6
            s.t += time + 1.0 / 60
            let frame: DragonFrame = body.frame(s, atlas: atlas)
            for case .layer(.footNear, let xf, _) in frame.steps { feet.append(xf.apply(atlas.joints.footNear)) }
        }
        XCTAssertEqual(feet.count, 2)
        XCTAssertEqual(feet[0].x, feet[1].x, accuracy: 2)
    }

    func testWalkingStaysUprightAndTheTailTrailsStraight() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.walking = 1
        s.facing = 1
        let frame: DragonFrame = run(&body, &s, frames: 120) { $0.stridePhase += 0.15 }
        XCTAssertEqual(frame.value(.ground), 1, accuracy: 0.05)
        XCTAssertLessThan(abs(frame.lean), 0.3)
        let bend: Double = frame.tail.aim.map { abs(DragonMath.wrapped($0 - frame.tail.aim[0])) }.max() ?? 9
        XCTAssertLessThan(bend, 1.2)
    }

    func testSpringsCarryTheBodyBetweenPoses() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene(1)
        _ = body.frame(s, atlas: atlas)
        s.activity = .stretch
        s.activityAge = 1
        s.t += 1.0 / 30
        _ = body.frame(s, atlas: atlas)
        let early: Double = body.q(.pawNy)
        XCTAssertLessThan(early, 0)
        XCTAssertGreaterThan(early, -200)
        var lowest: Double = 0
        for _ in 0..<120 {
            s.t += 1.0 / 60
            _ = body.frame(s, atlas: atlas)
            lowest = min(lowest, body.q(.pawNy))
        }
        XCTAssertLessThan(lowest, -300)
    }

    func testEyesMoveFirstAndTheHeadFollows() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene(1)
        _ = body.frame(s, atlas: atlas)
        s.gaze = DragonPoint(1, 0)
        s.t += 0.05
        _ = body.frame(s, atlas: atlas)
        XCTAssertGreaterThan(body.q(.gx), body.q(.gh) / 0.75)
        XCTAssertGreaterThan(DragonKey.gx.stiffness.k, DragonKey.head.stiffness.k)
        XCTAssertGreaterThan(DragonKey.head.stiffness.k, DragonKey.tail.stiffness.k)
    }

    func testReducedMotionSnapsToThePoseAndPlaysNoHabits() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene(5.4)
        s.reduced = true
        _ = body.frame(s, atlas: atlas)
        let goal: DragonPose = DragonBody.target(for: s, facing: 1).target
        for key in DragonKey.allCases { XCTAssertEqual(body.q(key), goal[key], accuracy: 1e-9, "\(key)") }
        for t in stride(from: 0.0, to: 120.0, by: 0.37) {
            var calm: DragonScene = scene(t)
            calm.reduced = true
            let made: (target: DragonPose, sneeze: Double?) = DragonBody.target(for: calm, facing: 1)
            XCTAssertNil(made.sneeze)
            XCTAssertEqual(made.target[.open], 0, accuracy: 1e-9)
        }
    }

    func testNineHabitsComeOneEveryFiveAndAHalfSeconds() {
        XCTAssertEqual(DragonHabit.all.map(\.name), ["yawn", "sneeze", "look", "tilt", "wings", "flick", "lick", "shift", "hop"])
        var seen: Set<String> = []
        var starts: Int = 0
        var was: Bool = false
        var t: Double = 0
        while t < 600 {
            let now: (habit: DragonHabit, u: Double)? = DragonHabit.at(t)
            if let now = now { seen.insert(now.habit.name) }
            if now != nil && !was { starts += 1 }
            was = now != nil
            t += 0.05
        }
        XCTAssertEqual(seen.count, 9)
        XCTAssertGreaterThan(starts, 100)
        XCTAssertLessThanOrEqual(starts, 110)
        for habit in DragonHabit.all { XCTAssertFalse(DragonHabit.title(habit.name).isEmpty) }
        XCTAssertNotNil(DragonHabit.named("sneeze")?.sneeze)
    }

    func testASneezePutsOutAPuffOfSmoke() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene(2)
        s.habit = DragonHabitCue(name: "sneeze", u: 0.8)
        let frame: DragonFrame = body.frame(s, atlas: atlas)
        XCTAssertNotNil(frame.sneeze)
        XCTAssertEqual(frame.sneeze ?? 0, (0.8 - 0.56) / 0.44, accuracy: 0.001)
        s.habit = DragonHabitCue(name: "yawn", u: 0.5)
        XCTAssertNil(body.frame(s, atlas: atlas).sneeze)
    }

    func testBlinksComeEveryFewSecondsAndSometimesTwice() {
        var blinks: Int = 0
        var shut: Bool = false
        var double: Bool = false
        var t: Double = 0
        var gap: Double = 1
        while t < 120 {
            let b: Double = DragonHabit.blink(at: t)
            if b > 0.5 && !shut {
                blinks += 1
                if gap < 0.6 { double = true }
                gap = 0
            }
            shut = b > 0.5
            gap += 0.01
            t += 0.01
        }
        XCTAssertGreaterThan(blinks, 25)
        XCTAssertTrue(double)
    }

    func testReactionsAreShortPoses() {
        XCTAssertEqual(DragonReaction.allCases.map(\.rawValue), ["wave", "cheer", "nod", "eureka", "giggle", "surprise", "dizzy"])
        XCTAssertGreaterThan(DragonReaction.wave.pose(age: 0.2)[.pawFx] ?? 0, 150)
        XCTAssertEqual(DragonReaction.nod.pose(age: 1.2)[.head] ?? 1, 0, accuracy: 0.0001)
    }

    func testPropsAreHeldInThePawsAndPopInAndOut() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.activity = .coffee
        var sizes: [Double] = []
        for step in 0...10 {
            s.activityAge = DragonActivity.coffee.duration * Double(step) / 10
            s.t += 0.1
            let frame: DragonFrame = body.frame(s, atlas: atlas)
            for case .gear(let gear) in frame.steps {
                XCTAssertEqual(gear.prop, "mug")
                sizes.append(gear.scale)
                if step == 5 {
                    XCTAssertEqual(gear.x, frame.pawN.x + 88, accuracy: 0.5)
                    XCTAssertEqual(gear.y, frame.pawN.y - 18, accuracy: 0.5)
                }
            }
        }
        XCTAssertEqual(sizes.count, 11)
        XCTAssertEqual(sizes.first ?? 1, 0, accuracy: 0.001)
        XCTAssertEqual(sizes.last ?? 1, 0, accuracy: 0.001)
        XCTAssertGreaterThan(sizes.max() ?? 0, 0.9)
    }

    func testTheHeartAndTheBackpackAppearInTheirActivities() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.activity = .heart
        s.activityAge = 1.5
        var frame: DragonFrame = body.frame(s, atlas: atlas)
        XCTAssertTrue(frame.steps.contains { if case .gear(let g) = $0 { return g.prop == "heart" }; return false })
        s.activity = .travel
        frame = run(&body, &s, frames: 60) { $0.activity = .travel; $0.activityAge = 1.5 }
        XCTAssertTrue(frame.steps.contains { if case .gear(let g) = $0 { return g.prop == "backpack" }; return false })
        XCTAssertEqual(frame.value(.ground), 1, accuracy: 0.1)
    }

    func testASittingDragonStandsOnTheGroundWithItsShadow() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        let frame: DragonFrame = run(&body, &s, frames: 120)
        XCTAssertEqual(frame.lift, 0, accuracy: 0.5)
        XCTAssertEqual(frame.shadow.width, 44, accuracy: 0.5)
        for case .layer(.footNear, let xf, _) in frame.steps {
            XCTAssertLessThanOrEqual(xf.apply(atlas.joints.footNear).y, 1236.001)
        }
    }

    func testFlyingAndHeldHaveTheirOwnPoses() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.air = 1
        s.vy = 900
        var frame: DragonFrame = run(&body, &s, frames: 60)
        XCTAssertEqual(frame.value(.flap), 1, accuracy: 0.05)
        XCTAssertGreaterThan(frame.value(.lean), 0.6)
        s.air = 0
        s.held = true
        s.vy = 0
        frame = run(&body, &s, frames: 90)
        XCTAssertLessThan(frame.value(.y), -15)
        XCTAssertLessThan(frame.value(.footNy), -40)
        s.held = false
        s.land = 1
        frame = run(&body, &s, frames: 6)
        XCTAssertGreaterThan(frame.value(.crouch), 0.2)
    }

    func testNapsStartAfterFortyFiveQuietSecondsAndWakeWithAStretch() {
        var nap: DragonNap = DragonNap(now: 0)
        var now: Double = 0
        for _ in 0..<440 {
            now += 0.1
            nap.step(now: now, dt: 0.1, poked: false, awake: false, calm: true)
        }
        XCTAssertNil(nap.ambient)
        for _ in 0..<80 {
            now += 0.1
            nap.step(now: now, dt: 0.1, poked: false, awake: false, calm: true)
        }
        XCTAssertEqual(nap.ambient, .sleep)
        XCTAssertTrue(nap.asleep)
        now += 0.1
        nap.step(now: now, dt: 0.1, poked: true, awake: true, calm: true)
        XCTAssertEqual(nap.ambient, .stretch)
        for _ in 0..<30 {
            now += 0.1
            nap.step(now: now, dt: 0.1, poked: false, awake: false, calm: true)
        }
        XCTAssertNil(nap.ambient)
        XCTAssertGreaterThan(nap.quietSince, 54)
    }

    func testNothingNapsWhileAnythingIsGoingOn() {
        var nap: DragonNap = DragonNap(now: 0)
        var now: Double = 0
        for _ in 0..<2000 {
            now += 0.1
            nap.step(now: now, dt: 0.1, poked: false, awake: true, calm: true)
        }
        XCTAssertNil(nap.ambient)
        for _ in 0..<2000 {
            now += 0.1
            nap.step(now: now, dt: 0.1, poked: false, awake: false, calm: false)
        }
        XCTAssertNil(nap.ambient)
        nap.lullaby(now: now)
        XCTAssertTrue(nap.asleep)
        nap.step(now: now + 1, dt: 1, poked: false, awake: true, calm: false)
        XCTAssertNil(nap.ambient)
        nap.lullaby(now: now)
        nap.rouse(now: now)
        XCTAssertNil(nap.ambient)
    }

    func testTheBagPlaysEveryActivityBeforeAnyRepeats() {
        var seed: UInt64 = 11
        let random: () -> Double = {
            seed = seed &* 6_364_136_223_846_793_005 &+ 1
            return Double(seed >> 11) / Double(1 << 53)
        }
        var bag: [DragonActivity] = []
        var seen: Set<DragonActivity> = []
        for _ in 0..<20 {
            guard let next = DragonActivity.drawn(from: &bag, kinds: DragonKind.allCases, random: random) else { return XCTFail("empty") }
            seen.insert(next)
        }
        XCTAssertEqual(seen.count, 20)
    }

    func testADrawnActivityRespectsTheKindsOnThePage() {
        var bag: [DragonActivity] = []
        var seen: Set<DragonActivity> = []
        for _ in 0..<90 {
            guard let next = DragonActivity.drawn(from: &bag, kinds: [.code], random: { Double.random(in: 0..<1) }) else {
                return XCTFail("empty")
            }
            XCTAssertTrue(next.fits.contains(.code), next.rawValue)
            seen.insert(next)
        }
        XCTAssertGreaterThan(seen.count, 8)
        XCTAssertNil(DragonActivity.drawn(from: &bag, kinds: [], random: { 0.5 }))
    }

    func testPageKindsMapOntoTheDragonsKinds() {
        for kind in GenieLandmarkKind.allCases { XCTAssertTrue(DragonKind.allCases.contains(DragonKind(kind))) }
        XCTAssertEqual(DragonKind(.tab), .nav)
        XCTAssertEqual(DragonKind(.field), .input)
        XCTAssertEqual(DragonKind(.text), .short)
    }

    func testTheMascotIsChosenFromTheSavedValueAndTheSpiderStaysTheDefault() {
        XCTAssertEqual(GenieMascot(stored: nil), .spider)
        XCTAssertEqual(GenieMascot(stored: ""), .spider)
        XCTAssertEqual(GenieMascot(stored: "dragon"), .dragon)
        XCTAssertEqual(GenieMascot(stored: "spider"), .spider)
        XCTAssertEqual(GenieMascot(stored: "kraken"), .spider)
        XCTAssertEqual(GenieMascot.key, "genie/mascot")
        XCTAssertEqual(GenieMascot.dragon.other, .spider)
        XCTAssertEqual(GenieMascot.spider.base, 92)
        XCTAssertEqual(GenieMascot.dragon.title, "Cloud Dragon")
    }

    func testTrickPosesCoverEveryLegEyeAndMouthMove() {
        func frame(_ legs: GenieLegMove, _ eyes: GenieEyeMove, _ mouth: GenieMouthMove, rise: Double = 1) -> GenieTrickFrame {
            GenieTrickFrame(
                age: 0.7, progress: 0.5, envelope: 1, rise: rise, rotation: 0, dx: 0, dy: 0, scaleX: 1, scaleY: 1, opacity: 1, hue: 0,
                legs: legs, eyes: eyes, mouth: mouth, effect: .none, magic: nil, particles: [])
        }
        for legs in GenieLegMove.allCases {
            let pose: DragonTarget = DragonTrick.overlay(frame(legs, .normal, .normal))
            XCTAssertTrue(pose.values.allSatisfy { $0.isFinite }, legs.rawValue)
            if legs != .rest { XCTAssertFalse(pose.isEmpty, legs.rawValue) }
        }
        for eyes in GenieEyeMove.allCases {
            XCTAssertTrue(DragonTrick.overlay(frame(.rest, eyes, .normal)).values.allSatisfy { $0.isFinite }, eyes.rawValue)
        }
        for mouth in GenieMouthMove.allCases {
            XCTAssertTrue(DragonTrick.overlay(frame(.rest, .normal, mouth)).values.allSatisfy { $0.isFinite }, mouth.rawValue)
        }
        XCTAssertTrue(DragonTrick.overlay(frame(.wave, .closed, .o, rise: 0)).isEmpty)
        XCTAssertEqual(DragonTrick.overlay(frame(.rest, .closed, .normal))[.lid], 1)
    }

    func testAnOverlayLandsOnTopOfEveryOtherPose() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.overlay = [.lid: 1, .open: 0.8]
        let frame: DragonFrame = run(&body, &s, frames: 90)
        XCTAssertEqual(frame.value(.lid), 1, accuracy: 0.05)
        XCTAssertEqual(frame.value(.open), 0.8, accuracy: 0.05)
    }

    func testTiredEyelidsDroopForAPoorlyPet() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.tired = 0.45
        let frame: DragonFrame = run(&body, &s, frames: 90)
        XCTAssertEqual(frame.value(.lid), 0.45, accuracy: 0.05)
    }

    func testGearFollowsTheBodyWhenItTurnsRound() {
        var body: DragonBody = DragonBody()
        var s: DragonScene = scene()
        s.activity = .terminal
        s.activityAge = 1.5
        s.facing = 1
        func laptopX(_ frame: DragonFrame) -> Double {
            for case .gear(let gear) in frame.steps { return gear.x }
            return 0
        }
        let right: Double = laptopX(run(&body, &s, frames: 90) { $0.activityAge = 1.5 })
        s.facing = -1
        let left: Double = laptopX(run(&body, &s, frames: 90) { $0.activityAge = 1.5 })
        XCTAssertGreaterThan(right, atlas.joints.axis.x)
        XCTAssertLessThan(left, atlas.joints.axis.x)
        XCTAssertEqual(right - atlas.joints.axis.x, atlas.joints.axis.x - left, accuracy: 3)
    }

    func testAffineTransformsComposeInCanvasOrder() {
        let m: DragonAffine = DragonAffine.identity.translated(10, 20).scaled(2, 3).rotated(Double.pi / 2)
        let p: DragonPoint = m.apply(DragonPoint(1, 0))
        XCTAssertEqual(p.x, 10, accuracy: 1e-9)
        XCTAssertEqual(p.y, 23, accuracy: 1e-9)
        let q: DragonPoint = m.apply(DragonPoint(0, 1))
        XCTAssertEqual(q.x, 10 - 2, accuracy: 1e-9)
        XCTAssertEqual(q.y, 20, accuracy: 1e-9)
        let o: DragonAffine = DragonAffine.identity.translated(5, 5).concatenated(DragonAffine.identity.scaled(2, 2))
        XCTAssertEqual(o.apply(DragonPoint(1, 1)), DragonPoint(7, 7))
    }
}
