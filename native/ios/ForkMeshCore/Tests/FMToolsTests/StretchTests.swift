import FMApi
import FMBase
import XCTest
@testable import FMTools

@MainActor
final class FakeStretchHealth: StretchBridge {
    var available = true
    var allow = true
    var fail = false
    var saved: [StretchWorkout] = []
    var minutes: [StretchWorkoutKind: Double] = [:]

    func authorizeStretch() async -> Bool { allow }

    func saveStretchWorkout(_ workout: StretchWorkout) async throws {
        if fail { throw OrgApiException("health", "denied") }
        saved.append(workout)
    }

    func stretchMinutes(from: Date, to: Date) async -> [StretchWorkoutKind: Double] { minutes }
}

@MainActor
final class FakeStretchSpeaker: StretchSpeaker {
    var said: [(text: String, interrupt: Bool)] = []
    var paused = 0
    var resumed = 0
    var stopped = 0

    func say(_ text: String, interrupt: Bool) { said.append((text, interrupt)) }
    func pause() { paused += 1 }
    func resume() { resumed += 1 }
    func stop() { stopped += 1 }
}

@MainActor
final class StretchTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!
    let figure = StretchFigure()

    func close(_ a: StretchPoint, _ b: StretchPoint, accuracy: Double = 1e-6, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(a.x, b.x, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(a.y, b.y, accuracy: accuracy, file: file, line: line)
    }

    var breathe: StretchMotion {
        StretchMotion(period: 8, frames: [
            ["t": 0, "lArm": 10, "rArm": 10, "headNod": 0],
            ["t": 0.5, "lArm": 168, "rArm": 168, "headNod": -0.3],
            ["t": 1, "lArm": 10, "rArm": 10, "headNod": 0],
        ])
    }

    func routine(_ id: String = "desk", workout: String = "flexibility") -> StretchRoutine {
        StretchRoutine(id: id, title: id == "lymph" ? "Lymph tapping" : "Desk stretch", workout: workout, steps: [
            StretchStep(id: "a", title: "Hip circles", seconds: 20, say: "Hands on hips.", half: "Other way.", motion: "breathe"),
            StretchStep(id: "b", title: "Twist", seconds: 16, say: "Rotate.", half: "", motion: "breathe"),
            StretchStep(id: "c", title: "Shake out", seconds: 10, say: "Shake.", half: "Ignored.", motion: "breathe"),
        ])
    }

    func play(_ steps: Double, by dt: Double = 0.5, _ state: inout StretchRun) -> [StretchCue] {
        var cues: [StretchCue] = []
        var done = 0.0
        while done < steps - 1e-9 {
            cues += state.advance(by: dt, now: Date(timeIntervalSince1970: 1000 + done))
            done += dt
        }
        return cues
    }

    func testInterpolationEasesBetweenFrames() {
        XCTAssertEqual(StretchMath.pose(breathe, at: 0).lArm, 10, accuracy: 1e-9)
        XCTAssertEqual(StretchMath.pose(breathe, at: 2).lArm, 89, accuracy: 1e-9)
        XCTAssertEqual(StretchMath.pose(breathe, at: 4).lArm, 168, accuracy: 1e-9)
        XCTAssertEqual(StretchMath.pose(breathe, at: 1).lArm, 10 + 158 * 0.15625, accuracy: 1e-9)
        XCTAssertEqual(StretchMath.pose(breathe, at: 10).lArm, 89, accuracy: 1e-9)
        XCTAssertEqual(StretchMath.pose(breathe, at: 6).headNod, -0.15, accuracy: 1e-9)
        XCTAssertEqual(StretchMath.pose(breathe, at: 3).twist, 0)
        XCTAssertEqual(StretchMath.smooth(0.25), 0.15625, accuracy: 1e-12)
    }

    func testGeometryAtRest() {
        let body = StretchMath.body(figure, pose: StretchPose())
        close(body.hip, StretchPoint(100, 158))
        close(body.chest, StretchPoint(100, 78))
        close(body.leftShoulder, StretchPoint(70, 78))
        close(body.rightShoulder, StretchPoint(130, 78))
        close(body.head, StretchPoint(100, 54))
        close(body.eyes[0], StretchPoint(95, 52))
        close(body.eyes[1], StretchPoint(105, 52))
        close(body.leftElbow, StretchPoint(70, 114))
        close(body.leftHand, StretchPoint(70, 146))
        close(body.rightHand, StretchPoint(130, 146))
        close(body.leftHip, StretchPoint(88, 158))
        close(body.leftKnee, StretchPoint(83, 206))
        close(body.rightKnee, StretchPoint(117, 206))
        close(body.leftFoot, StretchPoint(78, 254))
        XCTAssertEqual(body.headRadius, 16)
        XCTAssertEqual(body.segments.map(\.width), [14, 10, 9, 9, 8, 8, 10, 10, 10, 10])
    }

    func testGeometryFollowsTheReadmeArithmetic() {
        let pose = StretchPose([
            "hipX": 0.5, "knees": 0.5, "lean": 90, "twist": 0.5, "fold": 0.5, "shrug": 1, "headTilt": -90,
            "headTurn": 1, "headNod": 1, "lArm": 90, "lElbow": 90, "rArm": 180, "rElbow": 0,
        ])
        let body = StretchMath.body(figure, pose: pose, seconds: 1.0 / 12, tap: "left")
        close(body.hip, StretchPoint(108, 165))
        close(body.chest, StretchPoint(168, 165))
        close(body.leftShoulder, StretchPoint(168, 134))
        close(body.rightShoulder, StretchPoint(168, 182))
        close(body.head, StretchPoint(169.5, 147))
        close(body.eyes[0], StretchPoint(169.5, 148))
        close(body.eyes[1], StretchPoint(179.5, 148))
        close(body.leftElbow, StretchPoint(168, 98))
        close(body.leftHand, StretchPoint(200, 94))
        close(body.rightElbow, StretchPoint(204, 182))
        close(body.rightHand, StretchPoint(236, 182))
        close(body.leftKnee, StretchPoint(82, 209.5))
        close(body.rightKnee, StretchPoint(126, 209.5))
    }

    func testLibraryDecodesFromTheApi() {
        let payload: JSONObject = [
            "version": 1,
            "figure": ["width": 200, "height": 264, "hip": [100, 158], "torso": 80, "feet": [[78, 254], [122, 254]]],
            "poseFields": ["lArm", "rArm"],
            "motions": ["breathe": ["period": 8, "tap": "", "frames": [["t": 0, "lArm": 10], ["t": 1, "lArm": 10]]]],
            "routines": [
                ["id": "lymph", "title": "Lymph tapping", "summary": "s", "workout": "mindAndBody", "seconds": 30,
                 "steps": [["id": "x", "title": "Belly breaths", "seconds": 30, "say": "Breathe.", "half": "", "motion": "breathe"]]],
            ],
        ]
        let library = StretchJSON.decode(StretchLibrary.self, payload)
        XCTAssertEqual(library?.routines.first?.workoutKind, .mindAndBody)
        XCTAssertEqual(library?.routines.first?.steps.first?.title, "Belly breaths")
        XCTAssertEqual(library?.motions["breathe"]?.period, 8)
        XCTAssertEqual(library?.figure.shoulderHalf, 30)
        XCTAssertEqual(library?.poseFields, ["lArm", "rArm"])
    }

    func testCueSchedule() {
        var state = StretchRun(routine: routine(), startedAt: Date(timeIntervalSince1970: 1000))
        let opening = state.begin()
        XCTAssertEqual(opening.map(\.text), ["Hip circles. Hands on hips."])
        let early = play(9.5, &state)
        XCTAssertTrue(early.isEmpty)
        let half = play(0.5, &state)
        XCTAssertEqual(half.map(\.text), ["Other way."])
        let quiet = play(6.5, &state)
        XCTAssertTrue(quiet.isEmpty)
        let next = play(0.5, &state)
        XCTAssertEqual(next.map(\.text), ["Next: Twist"])
        let second = play(3, &state)
        XCTAssertEqual(second.map(\.text), ["Twist. Rotate."])
        let twist = play(16, &state)
        XCTAssertEqual(twist.map(\.kind), [.next, .start])
        XCTAssertEqual(twist.map(\.text), ["Next: Shake out", "Shake out. Shake."])
        let last = play(10, &state)
        XCTAssertEqual(last.map(\.text), ["Well done. That is your Desk stretch finished."])
        XCTAssertTrue(state.finished)
        XCTAssertTrue(state.completed)
        XCTAssertEqual(state.stepsDone, 3)
        XCTAssertEqual(state.activeSeconds, 46, accuracy: 1e-9)
        XCTAssertTrue(state.shouldLog)
    }

    func testPauseBackAndSkip() {
        var state = StretchRun(routine: routine(), startedAt: Date(timeIntervalSince1970: 1000))
        _ = state.begin()
        _ = play(5, &state)
        state.pause(at: Date(timeIntervalSince1970: 1005))
        let paused = state.advance(by: 30, now: Date(timeIntervalSince1970: 1035))
        XCTAssertTrue(paused.isEmpty)
        XCTAssertEqual(state.activeSeconds, 5, accuracy: 1e-9)
        state.resume(at: Date(timeIntervalSince1970: 1035))
        XCTAssertEqual(state.pauses, [DateInterval(start: Date(timeIntervalSince1970: 1005), end: Date(timeIntervalSince1970: 1035))])
        let skipped = state.skip(now: Date(timeIntervalSince1970: 1035))
        XCTAssertEqual(skipped.map(\.text), ["Twist. Rotate."])
        XCTAssertEqual(state.index, 1)
        let previous = state.back()
        XCTAssertEqual(previous.map(\.text), ["Hip circles. Hands on hips."])
        _ = play(4, &state)
        let restarted = state.back()
        XCTAssertEqual(restarted.map(\.text), ["Hip circles. Hands on hips."])
        XCTAssertEqual(state.index, 0)
        XCTAssertEqual(state.stepElapsed, 0)
    }

    func testLoggingRules() {
        var short = StretchRun(routine: routine(), startedAt: Date(timeIntervalSince1970: 1000))
        _ = short.begin()
        _ = play(29.5, &short)
        short.stop(at: Date(timeIntervalSince1970: 1030))
        XCTAssertTrue(short.finished)
        XCTAssertFalse(short.completed)
        XCTAssertFalse(short.shouldLog)

        var enough = StretchRun(routine: routine(), startedAt: Date(timeIntervalSince1970: 1000))
        _ = enough.begin()
        _ = play(30, &enough)
        enough.stop(at: Date(timeIntervalSince1970: 1040))
        XCTAssertTrue(enough.shouldLog)
        let body = enough.logBody(day: "2026-10-08", health: true)
        XCTAssertEqual(body["seconds"]?.intValue, 30)
        XCTAssertEqual(body["steps"]?.intValue, 1)
        XCTAssertEqual(body["stepsTotal"]?.intValue, 3)
        XCTAssertEqual(body["completed"]?.boolValue, false)
        XCTAssertEqual(body["source"]?.stringValue, "ios")
        XCTAssertEqual(body["health"]?.boolValue, true)
        XCTAssertEqual(body["at"]?.intValue, 1_000_000)
        XCTAssertEqual(enough.workout.end, Date(timeIntervalSince1970: 1040))
    }

    func overview(installed: Bool = true) -> JSONObject {
        let lymph: JSONObject = [
            "id": "lymph", "title": "Lymph tapping", "summary": "", "workout": "mindAndBody",
            "steps": [["id": "x", "title": "Belly breaths", "seconds": 40, "say": "Breathe.", "half": "Slowly.", "motion": "breathe"]],
        ]
        return [
            "ok": true, "installed": .bool(installed), "day": "2026-10-08",
            "profile": ["voice": true, "routine": "lymph", "tzOffsetMinutes": 0],
            "library": ["version": 1, "motions": [:], "routines": [.object(lymph)]],
            "today": ["day": "2026-10-08", "sessions": 1, "seconds": 300],
            "summary": ["streak": 3, "bestStreak": 5, "sessions": 9, "weekSeconds": 1200, "weekSessions": 4],
            "days": [], "recent": [
                ["id": "s1", "day": "2026-10-08", "loggedAt": 0, "routine": "lymph", "seconds": 270, "steps": 11,
                 "stepsTotal": 11, "completed": true, "at": 0, "source": "web", "health": false],
            ],
        ]
    }

    func testSessionLogsToHealthAndTheApi() async {
        let api = FakePackageApi()
        api.replies["GET"] = overview()
        api.replies["POST /sessions"] = ["ok": true, "session": ["id": "s2", "routine": "lymph", "seconds": 40, "health": true]]
        let health = FakeStretchHealth()
        health.minutes = [.mindAndBody: 12.4, .flexibility: 5]
        let prefs = InMemoryKeyValueStore()
        var clock = Date(timeIntervalSince1970: 1_791_460_800)
        let model = StretchModel(api: StretchAPI(call: api.call), health: health, prefs: prefs, timeZone: utc, now: { clock })
        let speaker = FakeStretchSpeaker()
        model.speaker = speaker
        await model.load()
        XCTAssertEqual(model.selectedRoutine, "lymph")
        XCTAssertEqual(model.stats.first?.value, "1 session · 5 min")
        XCTAssertEqual(model.recent.first?.detail, "4 min 30 s · completed · web")
        await model.enableHealth()
        XCTAssertTrue(prefs.bool(forKey: HealthPrefs.stretch, or: false))
        XCTAssertTrue(model.healthLine.contains("Mind and Body 12 min"))
        await model.start()
        XCTAssertEqual(speaker.said.first?.text, "Belly breaths. Breathe.")
        for _ in 0..<40 {
            clock = clock.addingTimeInterval(1)
            await model.tick(by: 1)
        }
        XCTAssertEqual(speaker.said.map(\.text), ["Belly breaths. Breathe.", "Slowly.", "Well done. That is your Lymph tapping finished."])
        XCTAssertEqual(health.saved.first?.kind, .mindAndBody)
        XCTAssertEqual(health.saved.first?.seconds, 40)
        let body = api.bodies("POST", "/sessions").first
        XCTAssertEqual(body?["health"]?.boolValue, true)
        XCTAssertEqual(body?["completed"]?.boolValue, true)
        XCTAssertEqual(body?["routine"]?.stringValue, "lymph")
        XCTAssertEqual(body?["day"]?.stringValue, "2026-10-08")
        XCTAssertEqual(model.logged?.id, "s2")
    }

    func testShortStopIsNotLoggedAndPausedSpeechWaits() async {
        let api = FakePackageApi()
        api.replies["GET"] = overview()
        var clock = Date(timeIntervalSince1970: 1_791_460_800)
        let model = StretchModel(api: StretchAPI(call: api.call), health: nil, timeZone: utc, now: { clock })
        let speaker = FakeStretchSpeaker()
        model.speaker = speaker
        await model.load()
        await model.start()
        model.pause()
        XCTAssertEqual(speaker.paused, 1)
        await model.tick(by: 1)
        XCTAssertEqual(model.run?.activeSeconds, 0)
        model.back()
        model.resume()
        XCTAssertEqual(speaker.said.last?.text, "Belly breaths. Breathe.")
        XCTAssertEqual(speaker.said.last?.interrupt, true)
        clock = clock.addingTimeInterval(10)
        await model.tick(by: 10)
        await model.stop()
        XCTAssertTrue(api.bodies("POST", "/sessions").isEmpty)
        XCTAssertFalse(model.logNote.isEmpty)
    }

    func testVoiceToggleSavesTheProfile() async {
        let api = FakePackageApi()
        api.replies["GET"] = overview()
        let model = StretchModel(api: StretchAPI(call: api.call), health: nil, timeZone: utc)
        let speaker = FakeStretchSpeaker()
        model.speaker = speaker
        await model.load()
        await model.setVoice(false)
        XCTAssertEqual(api.bodies("PUT", "/profile").first?["voice"]?.boolValue, false)
        XCTAssertEqual(speaker.stopped, 1)
        await model.start()
        XCTAssertTrue(speaker.said.isEmpty)
    }

    func testNotInstalledShowsInstall() async {
        let api = FakePackageApi()
        api.replies["GET"] = overview(installed: false)
        let model = StretchModel(api: StretchAPI(call: api.call), health: nil, timeZone: utc)
        await model.load()
        XCTAssertTrue(model.showsInstall)
        XCTAssertEqual(model.routines.count, 1)
        await model.install()
        XCTAssertEqual(api.bodies("POST", "/install").first?["voice"]?.boolValue, true)
    }

    func testText() {
        XCTAssertEqual(StretchText.duration(330), "5 min 30 s")
        XCTAssertEqual(StretchText.duration(240), "4 min")
        XCTAssertEqual(StretchText.duration(45), "45 s")
        XCTAssertEqual(StretchText.countdown(75), "1:15")
    }
}
