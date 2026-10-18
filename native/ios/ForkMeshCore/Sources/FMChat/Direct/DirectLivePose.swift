import Foundation

public struct DirectLivePlanePoint: Equatable, Sendable {
    public let x: Double
    public let y: Double

    public init(x: Double, y: Double) {
        self.x = x
        self.y = y
    }
}

public struct DirectLiveFaceMap: Equatable, Sendable {
    public let a: Double
    public let b: Double
    public let c: Double
    public let d: Double
    public let tx: Double
    public let ty: Double
}

public struct DirectLivePhonePose: Equatable, Sendable {
    public static let width = 20.0
    public static let height = 32.0
    public static let depth = 1.8
    public static let corner = 5.0
    public static let bezel = 1.3
    public static let kickHoldMs: Int64 = 260
    public static let kickReach = 3.2
    public static let maxKick = 2.0
    public static let still = DirectLivePhonePose(roll: 0, pitch: 0)

    public let roll: Double
    public let pitch: Double
    public let kickX: Double
    public let kickY: Double
    public let kickZ: Double

    public init(roll: Double, pitch: Double, push: (x: Double, y: Double, z: Double) = (0, 0, 0)) {
        self.roll = roll.isFinite ? roll : 0
        self.pitch = pitch.isFinite ? min(max(pitch, -.pi / 2), .pi / 2) : 0
        let reach = sqrt(push.x * push.x + push.y * push.y + push.z * push.z)
        let scale = reach > DirectLivePhonePose.maxKick ? DirectLivePhonePose.maxKick / reach : 1
        let turned = DirectLivePhonePose.turn(push.x * scale, push.y * scale, push.z * scale, roll: self.roll, pitch: self.pitch)
        kickX = turned.x
        kickY = turned.y
        kickZ = turned.z
    }

    public init(_ sample: DirectLiveMotionSample, push: (x: Double, y: Double, z: Double)? = nil) {
        self.init(roll: sample.steadyRoll, pitch: sample.pitch, push: push ?? (sample.ax, sample.ay, sample.az))
    }

    public static func live(_ track: DirectLiveMotionTrack, at now: Int64) -> DirectLivePhonePose? {
        guard let current = track.sample(at: now) else { return nil }
        var push = (x: current.ax, y: current.ay, z: current.az)
        var strongest = current.jolt
        for sample in track.samples where sample.at <= now && now - sample.at < kickHoldMs {
            let fade = 1 - Double(now - sample.at) / Double(kickHoldMs)
            let felt = sample.jolt * fade
            if felt > strongest {
                strongest = felt
                push = (sample.ax * fade, sample.ay * fade, sample.az * fade)
            }
        }
        return DirectLivePhonePose(current, push: push)
    }

    public var jolt: Double { sqrt(kickX * kickX + kickY * kickY + kickZ * kickZ) }

    public var offset: DirectLivePlanePoint {
        DirectLivePlanePoint(x: kickX * DirectLivePhonePose.kickReach, y: kickY * DirectLivePhonePose.kickReach)
    }

    public var lift: Double { 1 + kickZ * 0.06 }

    public var faceShowing: Bool { cos(pitch) > 0.015 }

    public func project(_ u: Double, _ v: Double, _ w: Double) -> DirectLivePlanePoint {
        let turned = DirectLivePhonePose.turn(u, v, w, roll: roll, pitch: pitch)
        return DirectLivePlanePoint(x: turned.x, y: turned.y)
    }

    public var faceMap: DirectLiveFaceMap {
        let s = sin(roll)
        let c = cos(roll)
        let tilt = cos(pitch)
        return DirectLiveFaceMap(
            a: c, b: s * tilt, c: -s, d: c * tilt, tx: 0, ty: -DirectLivePhonePose.depth / 2 * sin(pitch))
    }

    public func outline(steps: Int = 5) -> [DirectLivePlanePoint] {
        let halfWidth = DirectLivePhonePose.width / 2 - DirectLivePhonePose.corner
        let halfHeight = DirectLivePhonePose.height / 2 - DirectLivePhonePose.corner
        let half = DirectLivePhonePose.depth / 2
        let centres: [(Double, Double, Double)] = [
            (halfWidth, halfHeight, 0), (-halfWidth, halfHeight, .pi / 2), (-halfWidth, -halfHeight, .pi),
            (halfWidth, -halfHeight, .pi * 1.5),
        ]
        var points: [DirectLivePlanePoint] = []
        let count = max(1, steps)
        for (cx, cy, start) in centres {
            for step in 0...count {
                let angle = start + Double(step) / Double(count) * .pi / 2
                let u = cx + DirectLivePhonePose.corner * cos(angle)
                let v = cy + DirectLivePhonePose.corner * sin(angle)
                points.append(project(u, v, half))
                points.append(project(u, v, -half))
            }
        }
        return DirectLivePhonePose.hull(points)
    }

    static func turn(_ u: Double, _ v: Double, _ w: Double, roll: Double, pitch: Double) -> (
        x: Double, y: Double, z: Double
    ) {
        let s = sin(roll)
        let c = cos(roll)
        let across = u * c + v * s
        let up = -u * s + v * c
        let high = up * cos(pitch) + w * sin(pitch)
        let near = -up * sin(pitch) + w * cos(pitch)
        return (across, -high, near)
    }

    static func hull(_ points: [DirectLivePlanePoint]) -> [DirectLivePlanePoint] {
        let sorted = points.sorted { $0.x == $1.x ? $0.y < $1.y : $0.x < $1.x }
        guard sorted.count > 2 else { return sorted }
        func cross(_ o: DirectLivePlanePoint, _ a: DirectLivePlanePoint, _ b: DirectLivePlanePoint) -> Double {
            (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x)
        }
        var lower: [DirectLivePlanePoint] = []
        for point in sorted {
            while lower.count >= 2, cross(lower[lower.count - 2], lower[lower.count - 1], point) <= 0 { lower.removeLast() }
            lower.append(point)
        }
        var upper: [DirectLivePlanePoint] = []
        for point in sorted.reversed() {
            while upper.count >= 2, cross(upper[upper.count - 2], upper[upper.count - 1], point) <= 0 { upper.removeLast() }
            upper.append(point)
        }
        return Array(lower.dropLast() + upper.dropLast())
    }
}
