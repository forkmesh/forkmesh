import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieJoint: String, CaseIterable, Sendable {
    case wrist, thumbTip, thumbKnuckle, indexTip, indexKnuckle, middleTip, middleKnuckle, ringTip, ringKnuckle, littleTip, littleKnuckle
}

public enum GenieFinger: String, CaseIterable, Sendable {
    case thumb, index, middle, ring, little

    var tip: GenieJoint {
        switch self {
        case .thumb: return .thumbTip
        case .index: return .indexTip
        case .middle: return .middleTip
        case .ring: return .ringTip
        case .little: return .littleTip
        }
    }

    var knuckle: GenieJoint {
        switch self {
        case .thumb: return .thumbKnuckle
        case .index: return .indexKnuckle
        case .middle: return .middleKnuckle
        case .ring: return .ringKnuckle
        case .little: return .littleKnuckle
        }
    }
}

public enum GenieHandShape: String, CaseIterable, Sendable {
    case open, fist, point, peace, three, four, thumbsUp, thumbsDown, rock, callMe, ok, gun, pinch, other
}

public enum GenieSwipe: String, CaseIterable, Sendable {
    case left, right, up, down
}

public struct GenieHandPose: Equatable, Sendable {
    public var joints: [GenieJoint: CGPoint]
    public var depth: Double?
    public var confidence: Double

    public init(joints: [GenieJoint: CGPoint], depth: Double? = nil, confidence: Double = 1) {
        self.joints = joints
        self.depth = depth
        self.confidence = confidence
    }

    public func point(_ joint: GenieJoint) -> CGPoint {
        joints[joint] ?? joints[.wrist] ?? .zero
    }

    public static func gap(_ a: CGPoint, _ b: CGPoint) -> Double {
        Double(hypot(a.x - b.x, a.y - b.y))
    }

    public var size: Double {
        max(GenieHandPose.gap(point(.wrist), point(.middleKnuckle)), 0.01)
    }

    public var palm: CGPoint {
        let parts: [GenieJoint] = [.wrist, .indexKnuckle, .middleKnuckle, .ringKnuckle, .littleKnuckle]
        let xs = parts.map { point($0).x }.reduce(0, +)
        let ys = parts.map { point($0).y }.reduce(0, +)
        return CGPoint(x: xs / CGFloat(parts.count), y: ys / CGFloat(parts.count))
    }

    public var pinchGap: Double {
        GenieHandPose.gap(point(.thumbTip), point(.indexTip)) / size
    }

    public var indexReach: Double {
        GenieHandPose.gap(point(.indexTip), point(.wrist)) / max(GenieHandPose.gap(point(.indexKnuckle), point(.wrist)), 0.001)
    }

    public var pinched: Bool {
        pinchGap < GenieHandReader.pinchOn && indexReach > 0.95
    }

    public var pinchPoint: CGPoint {
        let a = point(.thumbTip)
        let b = point(.indexTip)
        return CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
    }

    public var distance: Double {
        if let depth, depth > 0.03, depth < 2 { return depth }
        return GenieHandReader.sizeAtArmsLength * GenieHandReader.armsLength / size
    }

    public func extended(_ finger: GenieFinger) -> Bool {
        let wrist = point(.wrist)
        if finger == .thumb {
            return GenieHandPose.gap(point(.thumbTip), point(.indexKnuckle)) / size > 0.62
        }
        let tip = GenieHandPose.gap(point(finger.tip), wrist)
        let knuckle = max(GenieHandPose.gap(point(finger.knuckle), wrist), 0.001)
        return tip / knuckle > 1.38
    }

    public var fingerCount: Int {
        GenieFinger.allCases.filter { extended($0) }.count
    }

    public var shape: GenieHandShape {
        let fingers = [GenieFinger.index, .middle, .ring, .little].filter { extended($0) }
        let thumb = extended(.thumb)
        if pinchGap < GenieHandReader.pinchOn {
            if fingers.filter({ $0 != .index }).count >= 2 { return .ok }
            if indexReach > 0.95 { return .pinch }
        }
        switch fingers {
        case []:
            guard thumb else { return .fist }
            let lift = Double(palm.y - point(.thumbTip).y) / size
            if lift > 0.45 { return .thumbsUp }
            if lift < -0.45 { return .thumbsDown }
            return .fist
        case [.index]: return thumb ? .gun : .point
        case [.index, .middle]: return .peace
        case [.index, .middle, .ring]: return .three
        case [.index, .little]: return .rock
        case [.little]: return thumb ? .callMe : .other
        case [.index, .middle, .ring, .little]: return thumb ? .open : .four
        default: return .other
        }
    }
}

public struct GenieManipulation: Equatable, Sendable {
    public var rotation: Double
    public var scale: Double
    public var yaw: Double
    public var pitch: Double

    public init(rotation: Double = 0, scale: Double = 1, yaw: Double = 0, pitch: Double = 0) {
        self.rotation = rotation
        self.scale = scale
        self.yaw = yaw
        self.pitch = pitch
    }

    public static let identity = GenieManipulation()
}

public enum GenieHandEvent: Equatable, Sendable {
    case appeared(Int)
    case vanished
    case returned(after: Double)
    case pinchBegan(CGPoint)
    case pinchEnded(CGPoint, velocity: CGSize)
    case push(Double)
    case pull(Double)
    case swipe(GenieSwipe)
    case wave
    case shape(GenieHandShape)
    case circle(clockwise: Bool)
    case wiggle
    case clap
    case twoHandsBegan
    case twoHandsEnded(GenieManipulation)

    public var key: String {
        switch self {
        case .appeared: return "appeared"
        case .vanished: return "vanished"
        case .returned: return "returned"
        case .pinchBegan: return "pinch"
        case .pinchEnded: return "release"
        case .push: return "push"
        case .pull: return "pull"
        case .swipe(let way): return "swipe_\(way.rawValue)"
        case .wave: return "wave"
        case .shape(let shape): return "shape_\(shape.rawValue)"
        case .circle: return "circle"
        case .wiggle: return "wiggle"
        case .clap: return "clap"
        case .twoHandsBegan: return "two_hands"
        case .twoHandsEnded: return "two_hands_done"
        }
    }
}

public struct GenieHandState: Equatable, Sendable {
    public var hands: Int = 0
    public var cursor: CGPoint?
    public var second: CGPoint?
    public var distance: Double?
    public var nearness: Double = 0
    public var pinching: Bool = false
    public var speed: Double = 0
    public var velocity: CGSize = .zero
    public var shape: GenieHandShape?
    public var manipulation: GenieManipulation?
    public var depthSensed: Bool = false

    public init() {}
}

public struct GenieHandReader: Sendable {
    public nonisolated static let pinchOn: Double = 0.32
    public nonisolated static let pinchOff: Double = 0.52
    public nonisolated static let smoothing: Double = 0.5
    public nonisolated static let vanishAfter: Double = 0.5
    public nonisolated static let peekabooAfter: Double = 1.2
    public nonisolated static let peekabooUntil: Double = 10
    public nonisolated static let armsLength: Double = 0.35
    public nonisolated static let sizeAtArmsLength: Double = 0.17
    public nonisolated static let nearDistance: Double = 0.12
    public nonisolated static let farDistance: Double = 0.55
    public nonisolated static let pushDistance: Double = 0.07
    public nonisolated static let pullDistance: Double = 0.09
    public nonisolated static let thrustWindow: Double = 0.35
    public nonisolated static let swipeDistance: Double = 0.3
    public nonisolated static let swipeWindow: Double = 0.35
    public nonisolated static let shapeHold: Double = 0.45
    public nonisolated static let cooldown: Double = 0.6

    struct Sample: Sendable {
        var at: Date
        var palm: CGPoint
        var tip: CGPoint
        var distance: Double
        var fingers: [CGPoint]
    }

    public private(set) var state = GenieHandState()

    var history: [Sample] = []
    var cursorTrail: [(at: Date, point: CGPoint)] = []
    var lastSeen: Date?
    var hiddenSince: Date?
    var vanishedSaid = true
    var pinchFrames = 0
    var releaseFrames = 0
    var shapeSeen: (shape: GenieHandShape, since: Date)?
    var shapeSaid: GenieHandShape?
    var quietUntil: Date = .distantPast
    var waveUntil: Date = .distantPast
    var wiggleUntil: Date = .distantPast
    var clapUntil: Date = .distantPast
    var twoStart: (angle: Double, gap: Double, depth: Double, mid: CGPoint)?
    var twoLost = 0
    var apartAt: Date?

    public init() {}

    public static func nearness(_ distance: Double) -> Double {
        min(max((farDistance - distance) / (farDistance - nearDistance), 0), 1)
    }

    static func smooth(_ old: CGPoint?, _ new: CGPoint, _ k: Double) -> CGPoint {
        guard let old else { return new }
        return CGPoint(x: old.x + (new.x - old.x) * CGFloat(k), y: old.y + (new.y - old.y) * CGFloat(k))
    }

    func primary(_ hands: [GenieHandPose]) -> (main: GenieHandPose, other: GenieHandPose?)? {
        guard let first = hands.first else { return nil }
        guard hands.count > 1 else { return (first, nil) }
        let ranked = hands.sorted { a, b in
            guard let cursor = state.cursor else { return a.confidence > b.confidence }
            return GenieHandPose.gap(a.pinchPoint, cursor) < GenieHandPose.gap(b.pinchPoint, cursor)
        }
        return (ranked[0], ranked[1])
    }

    public mutating func read(_ raw: [GenieHandPose], at now: Date) -> [GenieHandEvent] {
        let hands = Array(raw.filter { $0.confidence >= 0.3 }.prefix(2))
        var events: [GenieHandEvent] = []
        guard let (main, other) = primary(hands) else {
            missing(at: now, into: &events)
            return events
        }
        if vanishedSaid {
            events.append(.appeared(hands.count))
            if let gone = hiddenSince ?? lastSeen {
                let away = now.timeIntervalSince(gone)
                if away >= GenieHandReader.peekabooAfter, away <= GenieHandReader.peekabooUntil {
                    events.append(.returned(after: away))
                }
            }
        }
        hiddenSince = nil
        vanishedSaid = false
        lastSeen = now
        state.hands = hands.count
        state.depthSensed = main.depth != nil
        let cursor = GenieHandReader.smooth(state.cursor, main.pinchPoint, GenieHandReader.smoothing)
        state.cursor = cursor
        state.second = other.map { GenieHandReader.smooth(state.second, $0.pinchPoint, GenieHandReader.smoothing) }
        let distance = state.distance.map { $0 + (main.distance - $0) * 0.5 } ?? main.distance
        state.distance = distance
        state.nearness = GenieHandReader.nearness(distance)
        cursorTrail = cursorTrail.filter { now.timeIntervalSince($0.at) < 0.16 } + [(now, cursor)]
        if let first = cursorTrail.first, let last = cursorTrail.last, last.at > first.at {
            let dt = last.at.timeIntervalSince(first.at)
            state.velocity = CGSize(width: Double(last.point.x - first.point.x) / dt, height: Double(last.point.y - first.point.y) / dt)
        } else {
            state.velocity = .zero
        }
        state.speed = Double(hypot(state.velocity.width, state.velocity.height))
        let fingers = [GenieJoint.indexTip, .middleTip, .ringTip, .littleTip].map { main.point($0) }
        history = history.filter { now.timeIntervalSince($0.at) < 2.2 }
            + [Sample(at: now, palm: main.palm, tip: main.point(.indexTip), distance: main.distance, fingers: fingers)]

        if let other {
            two(main, other, at: now, into: &events)
        } else if twoStart != nil {
            twoLost += 1
            if twoLost >= 3 { endTwo(into: &events) }
        }
        if twoStart == nil {
            pinch(main, at: now, into: &events)
        }
        let shape = main.shape
        state.shape = shape
        if !state.pinching, twoStart == nil {
            shapes(shape, at: now, into: &events)
            thrust(at: now, into: &events)
            swipe(at: now, into: &events)
            wave(shape, at: now, into: &events)
            circle(shape, at: now, into: &events)
            wiggle(main, at: now, into: &events)
        }
        if let other, twoStart == nil {
            clap(main, other, at: now, into: &events)
        } else {
            apartAt = nil
        }
        return events
    }

    mutating func missing(at now: Date, into events: inout [GenieHandEvent]) {
        if hiddenSince == nil, lastSeen != nil, !vanishedSaid { hiddenSince = now }
        state.hands = 0
        if let hidden = hiddenSince, !vanishedSaid, now.timeIntervalSince(hidden) >= GenieHandReader.vanishAfter {
            if state.pinching {
                pinchFrames = 0
                events.append(.pinchEnded(state.cursor ?? .zero, velocity: .zero))
            }
            if twoStart != nil { endTwo(into: &events) }
            vanishedSaid = true
            history = []
            cursorTrail = []
            shapeSeen = nil
            shapeSaid = nil
            state = GenieHandState()
            events.append(.vanished)
        }
    }

    mutating func pinch(_ hand: GenieHandPose, at now: Date, into events: inout [GenieHandEvent]) {
        let gap = hand.pinchGap
        if state.pinching {
            releaseFrames = gap > GenieHandReader.pinchOff || hand.indexReach < 0.85 ? releaseFrames + 1 : 0
            if releaseFrames >= 2 {
                state.pinching = false
                releaseFrames = 0
                pinchFrames = 0
                events.append(.pinchEnded(state.cursor ?? hand.pinchPoint, velocity: state.velocity))
                quietUntil = now.addingTimeInterval(0.3)
            }
        } else {
            pinchFrames = hand.pinched ? pinchFrames + 1 : 0
            if pinchFrames >= 2 {
                state.pinching = true
                releaseFrames = 0
                events.append(.pinchBegan(state.cursor ?? hand.pinchPoint))
            }
        }
    }

    mutating func shapes(_ shape: GenieHandShape, at now: Date, into events: inout [GenieHandEvent]) {
        guard shape != .pinch, shape != .other else {
            shapeSeen = nil
            return
        }
        if shapeSeen?.shape != shape {
            shapeSeen = (shape, now)
            if shapeSaid != shape { shapeSaid = nil }
            return
        }
        guard let seen = shapeSeen, shapeSaid != shape, now.timeIntervalSince(seen.since) >= GenieHandReader.shapeHold, state.speed < 0.6 else { return }
        shapeSaid = shape
        events.append(.shape(shape))
    }

    mutating func thrust(at now: Date, into events: inout [GenieHandEvent]) {
        guard now >= quietUntil, let latest = history.last else { return }
        let window = history.filter { now.timeIntervalSince($0.at) <= GenieHandReader.thrustWindow }
        guard let farthest = window.max(by: { $0.distance < $1.distance }), let nearest = window.min(by: { $0.distance < $1.distance }) else { return }
        let drift = GenieHandPose.gap(window.first?.palm ?? latest.palm, latest.palm)
        guard drift < 0.18 else { return }
        if farthest.at < latest.at, farthest.distance - latest.distance >= GenieHandReader.pushDistance {
            events.append(.push(farthest.distance - latest.distance))
            quietUntil = now.addingTimeInterval(GenieHandReader.cooldown)
            history = [latest]
        } else if nearest.at < latest.at, latest.distance - nearest.distance >= GenieHandReader.pullDistance {
            events.append(.pull(latest.distance - nearest.distance))
            quietUntil = now.addingTimeInterval(GenieHandReader.cooldown)
            history = [latest]
        }
    }

    mutating func swipe(at now: Date, into events: inout [GenieHandEvent]) {
        guard now >= quietUntil, let latest = history.last else { return }
        guard let start = history.first(where: { now.timeIntervalSince($0.at) <= GenieHandReader.swipeWindow }) else { return }
        let dx = Double(latest.palm.x - start.palm.x)
        let dy = Double(latest.palm.y - start.palm.y)
        let way: GenieSwipe
        if abs(dx) >= GenieHandReader.swipeDistance, abs(dy) < abs(dx) * 0.6 {
            way = dx > 0 ? .right : .left
        } else if abs(dy) >= GenieHandReader.swipeDistance, abs(dx) < abs(dy) * 0.6 {
            way = dy > 0 ? .down : .up
        } else {
            return
        }
        events.append(.swipe(way))
        quietUntil = now.addingTimeInterval(GenieHandReader.cooldown)
        waveUntil = now.addingTimeInterval(1)
        history = [latest]
    }

    mutating func wave(_ shape: GenieHandShape, at now: Date, into events: inout [GenieHandEvent]) {
        guard now >= waveUntil, shape == .open || shape == .four else { return }
        let window = history.filter { now.timeIntervalSince($0.at) <= 1.6 }
        guard window.count >= 6 else { return }
        var turns = 0
        var direction = 0
        var anchor = Double(window[0].palm.x)
        for sample in window.dropFirst() {
            let x = Double(sample.palm.x)
            let moved = x - anchor
            if abs(moved) >= 0.035 {
                let now = moved > 0 ? 1 : -1
                if direction != 0, now != direction { turns += 1 }
                direction = now
                anchor = x
            }
        }
        guard turns >= 3 else { return }
        events.append(.wave)
        waveUntil = now.addingTimeInterval(2)
        quietUntil = now.addingTimeInterval(0.8)
    }

    mutating func circle(_ shape: GenieHandShape, at now: Date, into events: inout [GenieHandEvent]) {
        guard now >= quietUntil, shape == .point || shape == .gun else { return }
        let window = history.filter { now.timeIntervalSince($0.at) <= 2.2 }
        var points: [CGPoint] = []
        for sample in window where points.last.map({ GenieHandPose.gap($0, sample.tip) >= 0.012 }) ?? true {
            points.append(sample.tip)
        }
        guard points.count >= 8 else { return }
        var turned = 0.0
        for k in 2..<points.count {
            let a = atan2(Double(points[k - 1].y - points[k - 2].y), Double(points[k - 1].x - points[k - 2].x))
            let b = atan2(Double(points[k].y - points[k - 1].y), Double(points[k].x - points[k - 1].x))
            var step = b - a
            while step > Double.pi { step -= 2 * Double.pi }
            while step < -Double.pi { step += 2 * Double.pi }
            turned += step
        }
        guard abs(turned) >= Double.pi * 1.8 else { return }
        events.append(.circle(clockwise: turned > 0))
        quietUntil = now.addingTimeInterval(1)
        history = history.suffix(1)
    }

    mutating func wiggle(_ hand: GenieHandPose, at now: Date, into events: inout [GenieHandEvent]) {
        guard now >= wiggleUntil, state.speed < 0.35 else { return }
        let window = history.filter { now.timeIntervalSince($0.at) <= 0.7 }
        guard window.count >= 6 else { return }
        var energy = 0.0
        for k in 1..<window.count {
            let before = window[k - 1]
            let after = window[k]
            for f in 0..<min(before.fingers.count, after.fingers.count) {
                let a = CGPoint(x: before.fingers[f].x - before.palm.x, y: before.fingers[f].y - before.palm.y)
                let b = CGPoint(x: after.fingers[f].x - after.palm.x, y: after.fingers[f].y - after.palm.y)
                energy += GenieHandPose.gap(a, b)
            }
        }
        guard energy / hand.size >= 4.5 else { return }
        events.append(.wiggle)
        wiggleUntil = now.addingTimeInterval(1.4)
    }

    mutating func clap(_ a: GenieHandPose, _ b: GenieHandPose, at now: Date, into events: inout [GenieHandEvent]) {
        let gap = GenieHandPose.gap(a.palm, b.palm)
        if gap > 0.25 { apartAt = now }
        guard now >= clapUntil, gap < 0.1, let apart = apartAt, now.timeIntervalSince(apart) <= 0.6 else { return }
        events.append(.clap)
        clapUntil = now.addingTimeInterval(1)
        apartAt = nil
    }

    mutating func two(_ a: GenieHandPose, _ b: GenieHandPose, at now: Date, into events: inout [GenieHandEvent]) {
        let both = a.pinchGap < 0.4 && b.pinchGap < 0.4 && a.indexReach > 0.95 && b.indexReach > 0.95
        guard both || (twoStart != nil && twoLost < 3) else {
            if twoStart != nil { endTwo(into: &events) }
            return
        }
        twoLost = both ? 0 : twoLost + 1
        let left = a.pinchPoint.x <= b.pinchPoint.x ? a : b
        let right = a.pinchPoint.x <= b.pinchPoint.x ? b : a
        let l = left.pinchPoint
        let r = right.pinchPoint
        let angle = atan2(Double(r.y - l.y), Double(r.x - l.x)) * 180 / Double.pi
        let gap = max(GenieHandPose.gap(l, r), 0.02)
        let depth = right.distance - left.distance
        let mid = CGPoint(x: (l.x + r.x) / 2, y: (l.y + r.y) / 2)
        guard let start = twoStart else {
            if state.pinching {
                state.pinching = false
                pinchFrames = 0
                events.append(.pinchEnded(state.cursor ?? mid, velocity: .zero))
            }
            twoStart = (angle, gap, depth, mid)
            state.manipulation = .identity
            events.append(.twoHandsBegan)
            return
        }
        var turn = angle - start.angle
        while turn > 180 { turn -= 360 }
        while turn < -180 { turn += 360 }
        state.manipulation = GenieManipulation(
            rotation: turn, scale: min(max(gap / start.gap, 0.3), 3),
            yaw: min(max((depth - start.depth) * 420, -70), 70), pitch: min(max(Double(mid.y - start.mid.y) * -200, -60), 60))
    }

    mutating func endTwo(into events: inout [GenieHandEvent]) {
        let done = state.manipulation ?? .identity
        twoStart = nil
        twoLost = 0
        state.manipulation = nil
        pinchFrames = 0
        events.append(.twoHandsEnded(done))
    }
}
