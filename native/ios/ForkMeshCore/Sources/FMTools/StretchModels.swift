import FMApi
import FMBase
import Foundation
import Observation

public struct StretchAPI {
    public var call: ToolsApiCall

    public init(call: @escaping ToolsApiCall) {
        self.call = call
    }

    @MainActor
    public init(api: ApiService) {
        call = { method, suffix, body in try await api.stretchApi(method, suffix, body) }
    }
}

extension KeyedDecodingContainer {
    fileprivate func stretch<T: Decodable>(_ key: Key, _ fallback: T) -> T {
        (try? decodeIfPresent(T.self, forKey: key)) ?? fallback
    }
}

public enum StretchJSON {
    public static func decode<T: Decodable>(_ type: T.Type, _ object: JSONObject) -> T? {
        try? JSONDecoder().decode(type, from: JSON.encodeData(.object(object)))
    }
}

public struct StretchFigure: Codable, Equatable, Sendable {
    public var width: Double = 200
    public var height: Double = 264
    public var hip: [Double] = [100, 158]
    public var hipShift: Double = 16
    public var kneeDip: Double = 14
    public var torso: Double = 80
    public var shoulderHalf: Double = 30
    public var shrug: Double = 7
    public var neck: Double = 24
    public var head: Double = 16
    public var upperArm: Double = 36
    public var forearm: Double = 32
    public var hipHalf: Double = 12
    public var feet: [[Double]] = [[78, 254], [122, 254]]
    public var tapLift: Double = 4
    public var tapHz: Double = 3

    public init() {}

    public var hipPoint: StretchPoint { StretchPoint(hip.count > 1 ? hip[0] : 100, hip.count > 1 ? hip[1] : 158) }

    public func foot(_ index: Int) -> StretchPoint {
        guard index < feet.count, feet[index].count > 1 else {
            return index == 0 ? StretchPoint(78, 254) : StretchPoint(122, 254)
        }
        return StretchPoint(feet[index][0], feet[index][1])
    }
}

extension StretchFigure {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = StretchFigure()
        width = c.stretch(.width, d.width)
        height = c.stretch(.height, d.height)
        hip = c.stretch(.hip, d.hip)
        hipShift = c.stretch(.hipShift, d.hipShift)
        kneeDip = c.stretch(.kneeDip, d.kneeDip)
        torso = c.stretch(.torso, d.torso)
        shoulderHalf = c.stretch(.shoulderHalf, d.shoulderHalf)
        shrug = c.stretch(.shrug, d.shrug)
        neck = c.stretch(.neck, d.neck)
        head = c.stretch(.head, d.head)
        upperArm = c.stretch(.upperArm, d.upperArm)
        forearm = c.stretch(.forearm, d.forearm)
        hipHalf = c.stretch(.hipHalf, d.hipHalf)
        feet = c.stretch(.feet, d.feet)
        tapLift = c.stretch(.tapLift, d.tapLift)
        tapHz = c.stretch(.tapHz, d.tapHz)
    }
}

public struct StretchMotion: Codable, Equatable, Sendable {
    public var period: Double
    public var tap: String
    public var frames: [[String: Double]]

    public init(period: Double, tap: String = "", frames: [[String: Double]]) {
        self.period = period
        self.tap = tap
        self.frames = frames
    }
}

extension StretchMotion {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        period = c.stretch(.period, 1)
        tap = c.stretch(.tap, "")
        frames = c.stretch(.frames, [])
    }
}

public struct StretchStep: Codable, Equatable, Identifiable, Sendable {
    public var id: String
    public var title: String
    public var seconds: Int
    public var say: String
    public var half: String
    public var motion: String

    public init(id: String, title: String, seconds: Int, say: String = "", half: String = "", motion: String = "") {
        self.id = id
        self.title = title
        self.seconds = seconds
        self.say = say
        self.half = half
        self.motion = motion
    }
}

extension StretchStep {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.stretch(.id, "")
        title = c.stretch(.title, "")
        seconds = max(1, c.stretch(.seconds, 1))
        say = c.stretch(.say, "")
        half = c.stretch(.half, "")
        motion = c.stretch(.motion, "")
    }
}

public struct StretchRoutine: Codable, Equatable, Identifiable, Sendable {
    public var id: String
    public var title: String
    public var summary: String
    public var workout: String
    public var seconds: Int
    public var steps: [StretchStep]

    public init(id: String, title: String, summary: String = "", workout: String = "flexibility", steps: [StretchStep]) {
        self.id = id
        self.title = title
        self.summary = summary
        self.workout = workout
        self.steps = steps
        seconds = steps.reduce(0) { $0 + $1.seconds }
    }

    public var workoutKind: StretchWorkoutKind { StretchWorkoutKind(rawValue: workout) ?? .flexibility }
    public var durationLabel: String { StretchText.duration(seconds) }
}

extension StretchRoutine {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.stretch(.id, "")
        title = c.stretch(.title, "")
        summary = c.stretch(.summary, "")
        workout = c.stretch(.workout, "flexibility")
        steps = c.stretch(.steps, [])
        let total = c.stretch(.seconds, 0)
        seconds = total > 0 ? total : steps.reduce(0) { $0 + $1.seconds }
    }
}

public struct StretchLibrary: Codable, Equatable, Sendable {
    public var version: Int = 1
    public var figure: StretchFigure = StretchFigure()
    public var poseFields: [String] = StretchPose.fields
    public var motions: [String: StretchMotion] = [:]
    public var routines: [StretchRoutine] = []

    public init() {}

    public func routine(_ id: String) -> StretchRoutine? { routines.first { $0.id == id } }
}

extension StretchLibrary {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = c.stretch(.version, 1)
        figure = c.stretch(.figure, StretchFigure())
        let fields = c.stretch(.poseFields, [String]())
        poseFields = fields.isEmpty ? StretchPose.fields : fields
        motions = c.stretch(.motions, [:])
        routines = c.stretch(.routines, [])
    }
}

public struct StretchProfile: Codable, Equatable, Sendable {
    public var voice: Bool = true
    public var routine: String = "desk"
    public var tzOffsetMinutes: Int = 0

    public init() {}
}

extension StretchProfile {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        voice = c.stretch(.voice, true)
        routine = c.stretch(.routine, "desk")
        tzOffsetMinutes = c.stretch(.tzOffsetMinutes, 0)
    }
}

public struct StretchDayTotal: Codable, Equatable, Identifiable, Sendable {
    public var id: String { day }
    public var day: String = ""
    public var sessions: Int = 0
    public var seconds: Int = 0

    public init(day: String = "", sessions: Int = 0, seconds: Int = 0) {
        self.day = day
        self.sessions = sessions
        self.seconds = seconds
    }
}

extension StretchDayTotal {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        day = c.stretch(.day, "")
        sessions = c.stretch(.sessions, 0)
        seconds = c.stretch(.seconds, 0)
    }
}

public struct StretchSummary: Codable, Equatable, Sendable {
    public var streak: Int = 0
    public var bestStreak: Int = 0
    public var sessions: Int = 0
    public var weekSeconds: Int = 0
    public var weekSessions: Int = 0

    public init() {}
}

extension StretchSummary {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        streak = c.stretch(.streak, 0)
        bestStreak = c.stretch(.bestStreak, 0)
        sessions = c.stretch(.sessions, 0)
        weekSeconds = c.stretch(.weekSeconds, 0)
        weekSessions = c.stretch(.weekSessions, 0)
    }
}

public struct StretchSessionRecord: Codable, Equatable, Identifiable, Sendable {
    public var id: String = ""
    public var day: String = ""
    public var loggedAt: Int64 = 0
    public var routine: String = "desk"
    public var seconds: Int = 0
    public var steps: Int = 0
    public var stepsTotal: Int = 0
    public var completed: Bool = false
    public var at: Int64 = 0
    public var source: String = "web"
    public var health: Bool = false

    public init() {}
}

extension StretchSessionRecord {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.stretch(.id, "")
        day = c.stretch(.day, "")
        loggedAt = c.stretch(.loggedAt, 0)
        routine = c.stretch(.routine, "desk")
        seconds = c.stretch(.seconds, 0)
        steps = c.stretch(.steps, 0)
        stepsTotal = c.stretch(.stepsTotal, 0)
        completed = c.stretch(.completed, false)
        at = c.stretch(.at, 0)
        source = c.stretch(.source, "web")
        health = c.stretch(.health, false)
    }
}

public struct StretchOverview: Codable, Equatable, Sendable {
    public var installed: Bool = false
    public var day: String = ""
    public var profile: StretchProfile = StretchProfile()
    public var library: StretchLibrary = StretchLibrary()
    public var today: StretchDayTotal = StretchDayTotal()
    public var summary: StretchSummary = StretchSummary()
    public var days: [StretchDayTotal] = []
    public var recent: [StretchSessionRecord] = []

    public init() {}
}

extension StretchOverview {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        installed = c.stretch(.installed, false)
        day = c.stretch(.day, "")
        profile = c.stretch(.profile, StretchProfile())
        library = c.stretch(.library, StretchLibrary())
        today = c.stretch(.today, StretchDayTotal())
        summary = c.stretch(.summary, StretchSummary())
        days = c.stretch(.days, [])
        recent = c.stretch(.recent, [])
    }
}

public struct StretchPoint: Equatable, Sendable {
    public var x: Double
    public var y: Double

    public init(_ x: Double, _ y: Double) {
        self.x = x
        self.y = y
    }

    public static func + (lhs: StretchPoint, rhs: StretchPoint) -> StretchPoint { StretchPoint(lhs.x + rhs.x, lhs.y + rhs.y) }
    public static func - (lhs: StretchPoint, rhs: StretchPoint) -> StretchPoint { StretchPoint(lhs.x - rhs.x, lhs.y - rhs.y) }
    public static func * (lhs: StretchPoint, rhs: Double) -> StretchPoint { StretchPoint(lhs.x * rhs, lhs.y * rhs) }
}

public struct StretchSegment: Equatable, Sendable {
    public let from: StretchPoint
    public let to: StretchPoint
    public let width: Double

    public init(_ from: StretchPoint, _ to: StretchPoint, width: Double) {
        self.from = from
        self.to = to
        self.width = width
    }
}

public struct StretchPose: Equatable, Sendable {
    public static let fields = [
        "hipX", "knees", "lean", "twist", "fold", "shrug", "headTilt", "headTurn", "headNod",
        "lArm", "lElbow", "rArm", "rElbow",
    ]

    public var values: [String: Double]

    public init(_ values: [String: Double] = [:]) {
        self.values = values
    }

    public subscript(_ field: String) -> Double { values[field] ?? 0 }

    public var hipX: Double { self["hipX"] }
    public var knees: Double { self["knees"] }
    public var lean: Double { self["lean"] }
    public var twist: Double { self["twist"] }
    public var fold: Double { self["fold"] }
    public var shrug: Double { self["shrug"] }
    public var headTilt: Double { self["headTilt"] }
    public var headTurn: Double { self["headTurn"] }
    public var headNod: Double { self["headNod"] }
    public var lArm: Double { self["lArm"] }
    public var lElbow: Double { self["lElbow"] }
    public var rArm: Double { self["rArm"] }
    public var rElbow: Double { self["rElbow"] }
}

public struct StretchBody: Equatable, Sendable {
    public let hip: StretchPoint
    public let chest: StretchPoint
    public let leftShoulder: StretchPoint
    public let rightShoulder: StretchPoint
    public let leftElbow: StretchPoint
    public let rightElbow: StretchPoint
    public let leftHand: StretchPoint
    public let rightHand: StretchPoint
    public let leftHip: StretchPoint
    public let rightHip: StretchPoint
    public let leftKnee: StretchPoint
    public let rightKnee: StretchPoint
    public let leftFoot: StretchPoint
    public let rightFoot: StretchPoint
    public let head: StretchPoint
    public let headRadius: Double
    public let eyes: [StretchPoint]
    public let eyeRadius: Double

    public var segments: [StretchSegment] {
        [
            StretchSegment(hip, chest, width: 14),
            StretchSegment(leftShoulder, rightShoulder, width: 10),
            StretchSegment(leftShoulder, leftElbow, width: 9),
            StretchSegment(rightShoulder, rightElbow, width: 9),
            StretchSegment(leftElbow, leftHand, width: 8),
            StretchSegment(rightElbow, rightHand, width: 8),
            StretchSegment(leftHip, leftKnee, width: 10),
            StretchSegment(leftKnee, leftFoot, width: 10),
            StretchSegment(rightHip, rightKnee, width: 10),
            StretchSegment(rightKnee, rightFoot, width: 10),
        ]
    }
}

public enum StretchMath {
    public static func smooth(_ u: Double) -> Double { u * u * (3 - 2 * u) }

    public static func rotate(_ degrees: Double, _ x: Double, _ y: Double) -> StretchPoint {
        let radians = degrees * .pi / 180
        return StretchPoint(x * cos(radians) - y * sin(radians), x * sin(radians) + y * cos(radians))
    }

    public static func phase(_ seconds: Double, period: Double) -> Double {
        guard period > 0 else { return 0 }
        var value = seconds.truncatingRemainder(dividingBy: period) / period
        if value < 0 { value += 1 }
        return value
    }

    public static func pose(_ motion: StretchMotion, at seconds: Double, fields: [String] = StretchPose.fields) -> StretchPose {
        let frames = motion.frames
        guard let first = frames.first else { return StretchPose() }
        guard frames.count > 1, motion.period > 0 else { return StretchPose(first) }
        let position = phase(seconds, period: motion.period)
        var a = frames[frames.count - 2]
        var b = frames[frames.count - 1]
        for index in 0..<(frames.count - 1) {
            let low = frames[index]["t"] ?? 0
            let high = frames[index + 1]["t"] ?? 1
            if low <= position && position <= high {
                a = frames[index]
                b = frames[index + 1]
                break
            }
        }
        let start = a["t"] ?? 0
        let span = (b["t"] ?? 1) - start
        let u = smooth(span > 0 ? min(1, max(0, (position - start) / span)) : 0)
        var values: [String: Double] = [:]
        for field in fields {
            let from = a[field] ?? 0
            values[field] = from + ((b[field] ?? 0) - from) * u
        }
        return StretchPose(values)
    }

    public static func body(_ figure: StretchFigure, pose p: StretchPose, seconds: Double = 0, tap: String = "") -> StretchBody {
        let base = figure.hipPoint
        let hip = StretchPoint(base.x + p.hipX * figure.hipShift, base.y + p.knees * figure.kneeDip)
        let length = figure.torso * (1 - 0.5 * p.fold)
        let chest = hip + rotate(p.lean, 0, -1) * length
        let halfWidth = figure.shoulderHalf * (1 - 0.4 * abs(p.twist))
        let side = rotate(p.lean, 1, 0)
        let lift = StretchPoint(0, -p.shrug * figure.shrug)
        let leftShoulder = chest - side * halfWidth + lift
        let rightShoulder = chest + side * halfWidth + lift
        let headAngle = p.lean + p.headTilt
        let head = chest + rotate(headAngle, 0, -1) * figure.neck + StretchPoint(p.twist * 3, p.headNod * 6)
        let eyes = [-1.0, 1.0].map { sign in
            head + rotate(headAngle, sign * 5 + p.headTurn * 5, -2 + p.headNod * 3)
        }
        let tapOffset = figure.tapLift * abs(sin(2 * .pi * figure.tapHz * seconds))
        func arm(_ shoulder: StretchPoint, sign: Double, angle: Double, elbow: Double, tapping: Bool) -> (StretchPoint, StretchPoint) {
            let a = angle * .pi / 180
            let e = (angle + elbow) * .pi / 180
            let joint = shoulder + rotate(p.lean, sign * sin(a), cos(a)) * figure.upperArm
            var hand = joint + rotate(p.lean, sign * sin(e), cos(e)) * figure.forearm
            if tapping { hand.y -= tapOffset }
            return (joint, hand)
        }
        let left = arm(leftShoulder, sign: -1, angle: p.lArm, elbow: p.lElbow, tapping: tap == "left" || tap == "both")
        let right = arm(rightShoulder, sign: 1, angle: p.rArm, elbow: p.rElbow, tapping: tap == "right" || tap == "both")
        let leftHip = hip - StretchPoint(figure.hipHalf, 0)
        let rightHip = hip + StretchPoint(figure.hipHalf, 0)
        let leftFoot = figure.foot(0)
        let rightFoot = figure.foot(1)
        let leftKnee = StretchPoint((leftHip.x + leftFoot.x) / 2 - p.knees * 10, (leftHip.y + leftFoot.y) / 2)
        let rightKnee = StretchPoint((rightHip.x + rightFoot.x) / 2 + p.knees * 10, (rightHip.y + rightFoot.y) / 2)
        return StretchBody(
            hip: hip, chest: chest, leftShoulder: leftShoulder, rightShoulder: rightShoulder,
            leftElbow: left.0, rightElbow: right.0, leftHand: left.1, rightHand: right.1,
            leftHip: leftHip, rightHip: rightHip, leftKnee: leftKnee, rightKnee: rightKnee,
            leftFoot: leftFoot, rightFoot: rightFoot, head: head, headRadius: figure.head, eyes: eyes, eyeRadius: 2)
    }

    public static func body(_ library: StretchLibrary, step: StretchStep?, seconds: Double) -> StretchBody {
        guard let step, let motion = library.motions[step.motion] else {
            return body(library.figure, pose: StretchPose(["lArm": 8, "rArm": 8]))
        }
        let current = pose(motion, at: seconds, fields: library.poseFields)
        return body(library.figure, pose: current, seconds: seconds, tap: motion.tap)
    }
}

public enum StretchCueKind: String, Equatable, Sendable {
    case start
    case half
    case next
    case finish
}

public struct StretchCue: Equatable, Sendable {
    public let kind: StretchCueKind
    public let step: Int
    public let text: String

    public init(kind: StretchCueKind, step: Int, text: String) {
        self.kind = kind
        self.step = step
        self.text = text
    }

    public var interrupts: Bool { kind == .start || kind == .finish }
}

public struct StretchRun: Equatable, Sendable {
    public static let halfMinimum = 20
    public static let nextMinimum = 15
    public static let nextLead: Double = 3
    public static let logMinimum: Double = 30
    public static let backRestart: Double = 3

    public let routine: StretchRoutine
    public let startedAt: Date
    public private(set) var index = 0
    public private(set) var stepElapsed: Double = 0
    public private(set) var activeSeconds: Double = 0
    public private(set) var paused = false
    public private(set) var pausedAt: Date?
    public private(set) var pauses: [DateInterval] = []
    public private(set) var finished = false
    public private(set) var completed = false
    public private(set) var endedAt: Date?
    private var halfSpoken = false
    private var nextSpoken = false

    public init(routine: StretchRoutine, startedAt: Date) {
        self.routine = routine
        self.startedAt = startedAt
    }

    public var steps: [StretchStep] { routine.steps }
    public var step: StretchStep? { steps.indices.contains(index) ? steps[index] : nil }
    public var nextStep: StretchStep? { steps.indices.contains(index + 1) ? steps[index + 1] : nil }
    public var stepsDone: Int { completed ? steps.count : min(index, steps.count) }
    public var stepRemaining: Int { max(0, Int((Double(step?.seconds ?? 0) - stepElapsed).rounded(.up))) }

    public var elapsedTotal: Double {
        let before = steps.prefix(index).reduce(0) { $0 + $1.seconds }
        return Double(before) + stepElapsed
    }

    public var progress: Double {
        guard routine.seconds > 0 else { return 0 }
        return completed ? 1 : min(1, elapsedTotal / Double(routine.seconds))
    }

    public var shouldLog: Bool { finished && (completed || activeSeconds >= Self.logMinimum) }
    public var loggedSeconds: Int { min(14400, max(1, Int(activeSeconds.rounded()))) }

    public static func startText(_ step: StretchStep) -> String {
        step.say.isEmpty ? "\(step.title)." : "\(step.title). \(step.say)"
    }

    public static func nextText(_ step: StretchStep) -> String { "Next: \(step.title)" }

    public static func finishText(_ routine: StretchRoutine) -> String {
        "Well done. That is your \(routine.title) finished."
    }

    public mutating func begin() -> [StretchCue] {
        guard let step else {
            return finish(at: startedAt, completed: true)
        }
        return [StretchCue(kind: .start, step: index, text: Self.startText(step))]
    }

    public mutating func advance(by seconds: Double, now: Date) -> [StretchCue] {
        guard !finished, !paused, seconds > 0 else { return [] }
        var cues: [StretchCue] = []
        var left = seconds
        while left > 0, !finished, let step {
            let length = Double(step.seconds)
            let used = min(left, max(0, length - stepElapsed))
            let before = stepElapsed
            stepElapsed += used
            activeSeconds += used
            left -= used
            cues += crossings(step, from: before, to: stepElapsed)
            if stepElapsed >= length {
                cues += moveOn(now: now)
            }
        }
        return cues
    }

    private mutating func crossings(_ step: StretchStep, from before: Double, to after: Double) -> [StretchCue] {
        var cues: [StretchCue] = []
        let length = Double(step.seconds)
        if !halfSpoken && step.seconds >= Self.halfMinimum && !step.half.isEmpty && before < length / 2 && after >= length / 2 {
            halfSpoken = true
            cues.append(StretchCue(kind: .half, step: index, text: step.half))
        }
        if !nextSpoken, step.seconds >= Self.nextMinimum, let next = nextStep,
            before < length - Self.nextLead, after >= length - Self.nextLead {
            nextSpoken = true
            cues.append(StretchCue(kind: .next, step: index, text: Self.nextText(next)))
        }
        return cues
    }

    private mutating func moveOn(now: Date) -> [StretchCue] {
        if index + 1 >= steps.count {
            index = steps.count
            return finish(at: now, completed: true)
        }
        return go(to: index + 1)
    }

    private mutating func go(to target: Int) -> [StretchCue] {
        index = max(0, min(steps.count - 1, target))
        stepElapsed = 0
        halfSpoken = false
        nextSpoken = false
        guard let step else { return [] }
        return [StretchCue(kind: .start, step: index, text: Self.startText(step))]
    }

    private mutating func finish(at date: Date, completed: Bool) -> [StretchCue] {
        closePause(at: date)
        finished = true
        self.completed = completed
        endedAt = date
        stepElapsed = 0
        return completed ? [StretchCue(kind: .finish, step: steps.count, text: Self.finishText(routine))] : []
    }

    private mutating func closePause(at date: Date) {
        if let start = pausedAt {
            pauses.append(DateInterval(start: start, end: max(start, date)))
        }
        pausedAt = nil
        paused = false
    }

    public mutating func pause(at date: Date) {
        guard !finished, !paused else { return }
        paused = true
        pausedAt = date
    }

    public mutating func resume(at date: Date) {
        guard !finished, paused else { return }
        closePause(at: date)
    }

    public mutating func skip(now: Date) -> [StretchCue] {
        guard !finished else { return [] }
        return moveOn(now: now)
    }

    public mutating func back() -> [StretchCue] {
        guard !finished, !steps.isEmpty else { return [] }
        if stepElapsed >= Self.backRestart || index == 0 {
            return go(to: index)
        }
        return go(to: index - 1)
    }

    public mutating func stop(at date: Date) {
        guard !finished else { return }
        _ = finish(at: date, completed: false)
    }

    public func logBody(day: String, health: Bool, source: String = "ios") -> JSONObject {
        [
            "routine": .string(routine.id),
            "seconds": .int(Int64(loggedSeconds)),
            "steps": .int(Int64(stepsDone)),
            "stepsTotal": .int(Int64(steps.count)),
            "completed": .bool(completed),
            "at": .int(Int64((startedAt.timeIntervalSince1970 * 1000).rounded())),
            "day": .string(day),
            "source": .string(source),
            "health": .bool(health),
        ]
    }

    public var workout: StretchWorkout {
        StretchWorkout(
            kind: routine.workoutKind, routine: routine.id, start: startedAt, end: max(startedAt, endedAt ?? startedAt),
            pauses: pauses, seconds: loggedSeconds)
    }
}

@MainActor
public protocol StretchSpeaker: AnyObject {
    func say(_ text: String, interrupt: Bool)
    func pause()
    func resume()
    func stop()
}

public struct StretchStat: Equatable, Identifiable, Sendable {
    public var id: String { label }
    public let label: String
    public let value: String
}

public struct StretchRecentRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let detail: String
    public let when: String
    public let completed: Bool
    public let health: Bool
}

@MainActor
@Observable
public final class StretchModel {
    public static let title = "Stretch Break"
    public static let installTitle = "Take a stretch break"
    public static let installMessage =
        "Stretch Break talks you through a short stretch for your back and posture after a long day at the computer, and a gentle lymph tapping routine, while an example person does each move with you. Sessions are private to your ForkMesh account and show here, on the web, and on your desktop. With Apple Health turned on, each session is saved to Health as a workout."
    public static let installButton = "Install Stretch Break"
    public static let unavailableTitle = "Stretch Break is unavailable"
    public static let disclaimer = "General wellness guidance, not medical advice. Move gently and stop if anything hurts."

    @ObservationIgnored public let api: StretchAPI
    @ObservationIgnored public let health: StretchBridge?
    @ObservationIgnored public let prefs: KeyValueStore
    @ObservationIgnored public let timeZone: TimeZone
    @ObservationIgnored public weak var speaker: StretchSpeaker?
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored public private(set) var lastTick: Date?
    @ObservationIgnored private var pendingCue: StretchCue?

    public private(set) var overview: StretchOverview?
    public private(set) var library: StretchLibrary?
    public private(set) var installed = false
    public private(set) var loading = true
    public private(set) var error = ""
    public private(set) var busy = false
    public private(set) var voice = true
    public private(set) var healthEnabled: Bool
    public private(set) var healthMinutes: [StretchWorkoutKind: Double] = [:]
    public private(set) var run: StretchRun?
    public private(set) var logged: StretchSessionRecord?
    public private(set) var logNote = ""
    public var selectedRoutine = "desk"
    public var notice: ToolsNotice?

    public init(
        api: StretchAPI, health: StretchBridge?, prefs: KeyValueStore = InMemoryKeyValueStore(),
        timeZone: TimeZone = .current, now: (() -> Date)? = nil
    ) {
        self.api = api
        self.health = health
        self.prefs = prefs
        self.timeZone = timeZone
        self.now = now ?? { Date() }
        healthEnabled = prefs.bool(forKey: HealthPrefs.stretch, or: false)
    }

    public var today: String { CaloriesText.isoDay(now(), timeZone: timeZone) }
    public var healthAvailable: Bool { health?.available == true }

    public func load() async {
        loading = true
        error = ""
        do {
            try await fetch()
            loading = false
            await readHealth()
        } catch let failure as OrgApiException where failure.code == "plugin_not_installed" {
            installed = false
            overview = nil
            loading = false
        } catch {
            loading = false
            self.error = message(error)
        }
    }

    private func fetch() async throws {
        let first = overview == nil
        let payload = try await api.call("GET", "?day=\(today)", nil)
        let decoded = StretchJSON.decode(StretchOverview.self, payload) ?? StretchOverview()
        installed = payload["installed"]?.boolValue == true
        if let raw = payload["library"]?.objectValue, let parsed = StretchJSON.decode(StretchLibrary.self, raw) {
            library = parsed
        }
        overview = installed ? decoded : nil
        if installed {
            voice = decoded.profile.voice
            if first || routine(selectedRoutine) == nil {
                selectedRoutine = decoded.profile.routine
            }
        }
    }

    func message(_ error: Error) -> String {
        guard let failure = error as? OrgApiException else { return "Stretch Break is unavailable right now." }
        switch failure.code {
        case "invalid_routine": return "Stretch Break did not recognise that routine."
        case "invalid_seconds": return "That session was too short to log."
        case "day_full": return "Today already holds as many sessions as it can."
        case "session_not_found": return "That session was already removed."
        default: return failure.message
        }
    }

    func show(_ text: String, error: Bool = false) { notice = ToolsNotice(text, isError: error) }

    public var showsSpinner: Bool { loading && overview == nil && error.isEmpty }
    public var showsError: Bool { !error.isEmpty && overview == nil }
    public var showsInstall: Bool { !loading && error.isEmpty && !installed }
    public var inSession: Bool { run != nil }

    public var routines: [StretchRoutine] { library?.routines ?? [] }
    public func routine(_ id: String) -> StretchRoutine? { library?.routine(id) }
    public var chosenRoutine: StretchRoutine? { routine(selectedRoutine) ?? routines.first }

    public func install() async {
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("POST", "/install", [
                "voice": .bool(true), "routine": .string(selectedRoutine),
                "tzOffsetMinutes": .int(Int64(CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now()))),
            ])
            show("Stretch Break installed.")
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    @discardableResult
    public func uninstall() async -> Bool {
        busy = true
        defer { busy = false }
        do {
            _ = try await api.call("POST", "/uninstall", [:])
            prefs.set(false, forKey: HealthPrefs.stretch)
            healthEnabled = false
            healthMinutes = [:]
            show("Stretch Break uninstalled from your account.")
            await load()
            return true
        } catch {
            show(message(error), error: true)
            return false
        }
    }

    public func setVoice(_ on: Bool) async {
        voice = on
        if !on { speaker?.stop() }
        do {
            _ = try await api.call("PUT", "/profile", ["voice": .bool(on)])
        } catch {
            show(message(error), error: true)
        }
    }

    public func start() async {
        guard let routine = chosenRoutine, !routine.steps.isEmpty else {
            show("That routine has no steps yet.", error: true)
            return
        }
        logged = nil
        logNote = ""
        pendingCue = nil
        var fresh = StretchRun(routine: routine, startedAt: now())
        let cues = fresh.begin()
        run = fresh
        lastTick = now()
        speak(cues)
        if overview?.profile.routine != routine.id {
            _ = try? await api.call("PUT", "/profile", ["routine": .string(routine.id)])
        }
    }

    public func tick() async {
        let current = now()
        let dt = min(1, max(0, current.timeIntervalSince(lastTick ?? current)))
        await tick(by: dt)
    }

    public func tick(by seconds: Double) async {
        lastTick = now()
        guard var current = run, !current.finished, !current.paused else { return }
        let cues = current.advance(by: seconds, now: now())
        run = current
        speak(cues)
        if current.finished {
            await log()
        }
    }

    public func figureSeconds(at date: Date) -> Double {
        guard let current = run, !current.finished else { return 0 }
        guard !current.paused, let lastTick else { return current.stepElapsed }
        let extra = min(1, max(0, date.timeIntervalSince(lastTick)))
        return min(Double(current.step?.seconds ?? 0), current.stepElapsed + extra)
    }

    public func pause() {
        guard var current = run, !current.finished, !current.paused else { return }
        current.pause(at: now())
        run = current
        speaker?.pause()
    }

    public func resume() {
        guard var current = run, current.paused else { return }
        current.resume(at: now())
        run = current
        lastTick = now()
        guard voice else { return }
        if let cue = pendingCue {
            pendingCue = nil
            speaker?.say(cue.text, interrupt: true)
        } else {
            speaker?.resume()
        }
    }

    public func togglePause() {
        if run?.paused == true { resume() } else { pause() }
    }

    public func skip() async {
        guard var current = run, !current.finished else { return }
        let cues = current.skip(now: now())
        run = current
        lastTick = now()
        speak(cues)
        if current.finished {
            await log()
        }
    }

    public func back() {
        guard var current = run, !current.finished else { return }
        let cues = current.back()
        run = current
        lastTick = now()
        speak(cues)
    }

    public func stop() async {
        guard var current = run, !current.finished else {
            close()
            return
        }
        current.stop(at: now())
        run = current
        speaker?.stop()
        await log()
    }

    public func close() {
        if run?.finished == false {
            speaker?.stop()
        }
        run = nil
        lastTick = nil
        pendingCue = nil
    }

    private func speak(_ cues: [StretchCue]) {
        guard voice, let speaker else { return }
        if run?.paused == true {
            if let cue = cues.last(where: { $0.interrupts }) {
                pendingCue = cue
                speaker.stop()
            }
            return
        }
        pendingCue = nil
        for cue in cues {
            speaker.say(cue.text, interrupt: cue.interrupts)
        }
    }

    public func log() async {
        guard let current = run, current.finished else { return }
        guard current.shouldLog else {
            logNote = "Not logged: a session counts once 30 seconds are done."
            return
        }
        busy = true
        defer { busy = false }
        var saved = false
        if healthEnabled, let health, health.available {
            do {
                try await health.saveStretchWorkout(current.workout)
                saved = true
            } catch {
                show("Apple Health did not save this session. Allow workouts in Settings > Health > Data Access & Devices.", error: true)
            }
        }
        do {
            let reply = try await api.call("POST", "/sessions", current.logBody(day: today, health: saved))
            if let raw = reply["session"]?.objectValue {
                logged = StretchJSON.decode(StretchSessionRecord.self, raw)
            }
            logNote = saved ? "Logged and saved to Apple Health." : "Logged on your account."
            try? await fetch()
            await readHealth()
        } catch {
            logNote = saved ? "Saved to Apple Health, but not logged on your account." : "Not logged."
            show(message(error), error: true)
        }
    }

    public func deleteSession(_ id: String) async {
        guard !id.isEmpty else { return }
        do {
            _ = try await api.call("DELETE", "/sessions/\(id)", nil)
            show("Session removed. Apple Health is not changed.")
            try? await fetch()
        } catch {
            show(message(error), error: true)
        }
    }

    public func enableHealth() async {
        guard let health, health.available else {
            show("Apple Health is not available on this device.", error: true)
            return
        }
        guard await health.authorizeStretch() else {
            show("ForkMesh could not save workouts to Apple Health. Allow it in Settings > Health > Data Access & Devices.", error: true)
            return
        }
        prefs.set(true, forKey: HealthPrefs.stretch)
        healthEnabled = true
        await readHealth()
        show("Each Stretch Break session is now saved to Apple Health as a workout.")
    }

    public func disableHealth() {
        prefs.set(false, forKey: HealthPrefs.stretch)
        healthEnabled = false
        healthMinutes = [:]
        show("Apple Health is off for Stretch Break.")
    }

    public var healthRange: (from: Date, to: Date)? {
        guard let midnight = HealthDates.startOfDay(today, timeZone: timeZone),
            let start = CaloriesText.calendar(timeZone).date(byAdding: .day, value: -6, to: midnight)
        else { return nil }
        let end = now()
        return start < end ? (start, end) : nil
    }

    public func readHealth() async {
        guard healthEnabled, let health, health.available, let range = healthRange else {
            healthMinutes = [:]
            return
        }
        healthMinutes = await health.stretchMinutes(from: range.from, to: range.to)
    }

    public var profile: StretchProfile { overview?.profile ?? StretchProfile() }
    public var summary: StretchSummary { overview?.summary ?? StretchSummary() }
    public var todayTotal: StretchDayTotal { overview?.today ?? StretchDayTotal() }
    public var days: [StretchDayTotal] { overview?.days ?? [] }

    public var stats: [StretchStat] {
        [
            StretchStat(label: "Today", value: StretchText.sessions(todayTotal.sessions, seconds: todayTotal.seconds)),
            StretchStat(label: "Streak", value: StretchText.days(summary.streak)),
            StretchStat(label: "Last 7 days", value: StretchText.sessions(summary.weekSessions, seconds: summary.weekSeconds)),
            StretchStat(label: "Best streak", value: StretchText.days(summary.bestStreak)),
            StretchStat(label: "All sessions", value: String(summary.sessions)),
        ]
    }

    public var healthWeekMinutes: Double { healthMinutes.values.reduce(0, +) }

    public var healthLine: String {
        if !healthAvailable { return "Apple Health is not available on this device." }
        if !healthEnabled {
            return "Turn on Apple Health to save each session as a workout (Flexibility, or Mind and Body for lymph tapping)."
        }
        let flexibility = Int((healthMinutes[.flexibility] ?? 0).rounded())
        let mind = Int((healthMinutes[.mindAndBody] ?? 0).rounded())
        let ours = Int((Double(summary.weekSeconds) / 60).rounded())
        return "Apple Health, last 7 days: Flexibility \(flexibility) min, Mind and Body \(mind) min. Stretch Break logged \(ours) min."
    }

    public var recent: [StretchRecentRow] {
        (overview?.recent ?? []).map { session in
            let title = routine(session.routine)?.title ?? session.routine
            var parts = [StretchText.duration(session.seconds)]
            parts.append(session.completed ? "completed" : "stopped early")
            parts.append(StretchText.source(session.source))
            if session.health { parts.append("Apple Health") }
            var when = CaloriesText.dayLabel(session.day, timeZone: timeZone, now: now())
            let at = session.at > 0 ? session.at : session.loggedAt
            if at > 0 {
                when += " " + StepsText.clock(Date(timeIntervalSince1970: TimeInterval(at) / 1000), timeZone: timeZone)
            }
            return StretchRecentRow(
                id: session.id, title: title, detail: parts.joined(separator: " · "), when: when,
                completed: session.completed, health: session.health)
        }
    }
}

public enum StretchText {
    public static func duration(_ seconds: Int) -> String {
        let total = max(0, seconds)
        let minutes = total / 60
        let rest = total % 60
        if minutes == 0 { return "\(rest) s" }
        return rest == 0 ? "\(minutes) min" : "\(minutes) min \(rest) s"
    }

    public static func countdown(_ seconds: Int) -> String {
        let total = max(0, seconds)
        return String(format: "%d:%02d", total / 60, total % 60)
    }

    public static func days(_ count: Int) -> String { count == 1 ? "1 day" : "\(count) days" }

    public static func sessions(_ count: Int, seconds: Int) -> String {
        let label = count == 1 ? "1 session" : "\(count) sessions"
        return count == 0 ? label : "\(label) · \(duration(seconds))"
    }

    public static func source(_ value: String) -> String {
        switch value {
        case "ios": return "iPhone"
        case "android": return "Android"
        case "desktop": return "desktop"
        default: return "web"
        }
    }
}
