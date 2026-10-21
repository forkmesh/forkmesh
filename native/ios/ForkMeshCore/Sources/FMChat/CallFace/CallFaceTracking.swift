import Foundation

public struct CallFaceBlendShapes: Equatable, Sendable {
    public var eyeBlinkLeft = 0.0
    public var eyeBlinkRight = 0.0
    public var eyeLookInLeft = 0.0
    public var eyeLookOutLeft = 0.0
    public var eyeLookUpLeft = 0.0
    public var eyeLookDownLeft = 0.0
    public var eyeLookInRight = 0.0
    public var eyeLookOutRight = 0.0
    public var eyeLookUpRight = 0.0
    public var eyeLookDownRight = 0.0
    public var tongueOut = 0.0

    public init() {}
}

public enum CallFaceBlendMapping {
    public static let blinkOpen = 0.2
    public static let blinkClosed = 0.75
    public static let tongueStart = 0.15
    public static let tongueFull = 0.5
    public static let defaultSkinLuma = 0.5

    static func ramp(_ value: Double, _ low: Double, _ high: Double) -> Double {
        guard value.isFinite else { return 0 }
        return CallFaceGeometry.clampValue((value - low) / (high - low), 0, 1)
    }

    public static func close(_ shapes: CallFaceBlendShapes) -> [Double] {
        [ramp(shapes.eyeBlinkRight, blinkOpen, blinkClosed), ramp(shapes.eyeBlinkLeft, blinkOpen, blinkClosed)]
    }

    public static func look(_ shapes: CallFaceBlendShapes) -> [CallFacePoint] {
        let limit = CallFaceExpression.lookLimit
        func clamp(_ v: Double) -> Double { CallFaceGeometry.clampValue(v.isFinite ? v : 0, -limit, limit) }
        return [
            CallFacePoint(clamp((shapes.eyeLookInRight - shapes.eyeLookOutRight) * limit), clamp((shapes.eyeLookDownRight - shapes.eyeLookUpRight) * limit)),
            CallFacePoint(clamp((shapes.eyeLookOutLeft - shapes.eyeLookInLeft) * limit), clamp((shapes.eyeLookDownLeft - shapes.eyeLookUpLeft) * limit)),
        ]
    }

    public static func tongue(_ shapes: CallFaceBlendShapes) -> Double {
        ramp(shapes.tongueOut, tongueStart, tongueFull)
    }

    public static func reading(_ shapes: CallFaceBlendShapes, probe: CallFaceExpressionReading?) -> CallFaceExpressionReading {
        CallFaceExpressionReading(
            close: close(shapes),
            look: look(shapes),
            tongue: tongue(shapes),
            skinRed: probe?.skinRed ?? CallFaceDrawPlan.defaultSkinRed,
            skinLuma: probe?.skinLuma ?? defaultSkinLuma
        )
    }
}

public struct CallFaceMeshAnchor: Equatable, Sendable {
    public let triangle: Int
    public let weights: [Double]
}

public final class CallFaceMeshBinding {
    public static let learnFrames = 3
    public static let refreshMs = 2000.0
    public static let tolerance = 0.1
    public static let maxMisses = 3
    public static let keepShare = 0.8

    public let triangles: [Int]
    public private(set) var anchors: [CallFaceMeshAnchor]?
    public private(set) var misses = 0
    public private(set) var learnedAt = -Double.infinity
    public private(set) var lastError: Double?
    private var pending: [(landmarks: [CallFacePoint], vertices: [CallFacePoint])] = []

    public init(triangles: [Int]) {
        self.triangles = triangles
    }

    public var ready: Bool { anchors != nil && misses < Self.maxMisses }
    public var unreliable: Bool { misses >= Self.maxMisses }

    public func due(time: Double) -> Bool {
        if unreliable { return false }
        return anchors == nil || !pending.isEmpty || time - learnedAt >= Self.refreshMs
    }

    public func reset() {
        anchors = nil
        pending = []
        misses = 0
        learnedAt = -.infinity
        lastError = nil
    }

    func usable(_ landmarks: [CallFacePoint], _ vertices: [CallFacePoint]) -> Bool {
        guard landmarks.count >= CallFaceGeometry.landmarkCount, let top = triangles.max(), top < vertices.count else { return false }
        return triangles.count % 3 == 0 && !triangles.isEmpty
    }

    public func learn(landmarks: [CallFacePoint], vertices: [CallFacePoint], time: Double) {
        guard usable(landmarks, vertices) else { return }
        if let current = anchors, pending.isEmpty {
            let error = Self.error(current, triangles: triangles, landmarks: landmarks, vertices: vertices)
            lastError = error
            if error <= Self.tolerance {
                misses = 0
                learnedAt = time
                anchors = current.enumerated().map { i, anchor in
                    let fresh = Self.weights(landmarks[i], anchor.triangle, triangles: triangles, vertices: vertices) ?? anchor.weights
                    return CallFaceMeshAnchor(triangle: anchor.triangle, weights: zip(anchor.weights, fresh).map { $0 * Self.keepShare + $1 * (1 - Self.keepShare) })
                }
                return
            }
            misses += 1
        }
        pending.append((Array(landmarks.prefix(CallFaceGeometry.landmarkCount)), vertices))
        guard pending.count >= Self.learnFrames else { return }
        let frames = pending
        pending = []
        var built: [CallFaceMeshAnchor] = []
        for i in 0..<CallFaceGeometry.landmarkCount {
            var votes: [Int: Int] = [:]
            var order: [Int] = []
            for frame in frames {
                let chosen = Self.anchor(frame.landmarks[i], triangles: triangles, vertices: frame.vertices).triangle
                if votes[chosen] == nil { order.append(chosen) }
                votes[chosen, default: 0] += 1
            }
            let triangle = order.max { (votes[$0] ?? 0) < (votes[$1] ?? 0) } ?? order[0]
            var sum = [0.0, 0.0, 0.0]
            var counted = 0
            for frame in frames {
                guard let w = Self.weights(frame.landmarks[i], triangle, triangles: triangles, vertices: frame.vertices) else { continue }
                for k in 0..<3 { sum[k] += w[k] }
                counted += 1
            }
            guard counted > 0 else { return }
            built.append(CallFaceMeshAnchor(triangle: triangle, weights: sum.map { $0 / Double(counted) }))
        }
        let error = frames.map { Self.error(built, triangles: triangles, landmarks: $0.landmarks, vertices: $0.vertices) }.reduce(0, +) / Double(frames.count)
        lastError = error
        if error <= Self.tolerance {
            anchors = built
            misses = 0
            learnedAt = time
        } else {
            misses += 1
            if anchors == nil { learnedAt = time }
        }
    }

    public func place(_ vertices: [CallFacePoint], focal: Double = 0) -> [CallFacePoint]? {
        guard ready, let anchors else { return nil }
        return Self.place(anchors, triangles: triangles, vertices: vertices, focal: focal)
    }

    public static func place(_ anchors: [CallFaceMeshAnchor], triangles: [Int], vertices: [CallFacePoint], focal: Double = 0) -> [CallFacePoint]? {
        var points: [CallFacePoint] = []
        var depths: [Double] = []
        for anchor in anchors {
            let base = anchor.triangle * 3
            guard base + 2 < triangles.count else { return nil }
            var x = 0.0
            var y = 0.0
            var z = 0.0
            var deep = true
            for k in 0..<3 {
                let v = triangles[base + k]
                guard v < vertices.count else { return nil }
                x += anchor.weights[k] * vertices[v].x
                y += anchor.weights[k] * vertices[v].y
                if let d = vertices[v].z { z += anchor.weights[k] * d } else { deep = false }
            }
            points.append(CallFacePoint(x, y))
            depths.append(deep ? z : .nan)
        }
        let usable = depths.filter(CallFaceDepth.usable)
        guard focal > 0, focal.isFinite, usable.count == depths.count, let face = CallFaceDepth.median(usable) else { return points }
        return zip(points, depths).map { point, depth in CallFacePoint(point.x, point.y, CallFaceDepth.z(depth: depth, faceDepth: face, focal: focal)) }
    }

    public static func error(_ anchors: [CallFaceMeshAnchor], triangles: [Int], landmarks: [CallFacePoint], vertices: [CallFacePoint]) -> Double {
        guard let placed = place(anchors, triangles: triangles, vertices: vertices), placed.count == CallFaceGeometry.landmarkCount else { return .infinity }
        let span = CallFaceGeometry.faceFrame(landmarks).span
        var total = 0.0
        for i in 0..<placed.count { total += CallFaceJS.hypot(placed[i].x - landmarks[i].x, placed[i].y - landmarks[i].y) }
        return total / Double(placed.count) / max(span, 1e-6)
    }

    public static func barycentric(_ p: CallFacePoint, _ a: CallFacePoint, _ b: CallFacePoint, _ c: CallFacePoint) -> [Double]? {
        let v0x = b.x - a.x, v0y = b.y - a.y
        let v1x = c.x - a.x, v1y = c.y - a.y
        let v2x = p.x - a.x, v2y = p.y - a.y
        let det = v0x * v1y - v1x * v0y
        guard abs(det) > 1e-9 else { return nil }
        let u = (v2x * v1y - v1x * v2y) / det
        let v = (v0x * v2y - v2x * v0y) / det
        return [1 - u - v, u, v]
    }

    static func weights(_ p: CallFacePoint, _ triangle: Int, triangles: [Int], vertices: [CallFacePoint]) -> [Double]? {
        let base = triangle * 3
        return barycentric(p, vertices[triangles[base]], vertices[triangles[base + 1]], vertices[triangles[base + 2]])
    }

    static func segmentDistance(_ p: CallFacePoint, _ a: CallFacePoint, _ b: CallFacePoint) -> Double {
        let dx = b.x - a.x
        let dy = b.y - a.y
        let length = dx * dx + dy * dy
        let t = length > 0 ? CallFaceGeometry.clampValue(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0, 1) : 0
        return CallFaceJS.hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy))
    }

    public static func anchor(_ p: CallFacePoint, triangles: [Int], vertices: [CallFacePoint]) -> CallFaceMeshAnchor {
        var best = 0
        var bestDistance = Double.infinity
        var bestDepth = Double.infinity
        for t in 0..<(triangles.count / 3) {
            let a = vertices[triangles[t * 3]], b = vertices[triangles[t * 3 + 1]], c = vertices[triangles[t * 3 + 2]]
            guard let w = barycentric(p, a, b, c) else { continue }
            let inside = w.allSatisfy { $0 >= 0 }
            let distance = inside ? 0 : min(segmentDistance(p, a, b), segmentDistance(p, b, c), segmentDistance(p, c, a))
            let depth = (a.z ?? 0) * w[0] + (b.z ?? 0) * w[1] + (c.z ?? 0) * w[2]
            if distance < bestDistance || (distance == 0 && bestDistance == 0 && depth < bestDepth) {
                best = t
                bestDistance = distance
                bestDepth = depth
            }
        }
        let base = best * 3
        let w = barycentric(p, vertices[triangles[base]], vertices[triangles[base + 1]], vertices[triangles[base + 2]]) ?? [1, 0, 0]
        return CallFaceMeshAnchor(triangle: best, weights: w)
    }
}

public struct CallFaceVideoFormat: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let fps: Int

    public init(width: Int, height: Int, fps: Int) {
        self.width = width
        self.height = height
        self.fps = fps
    }
}

public enum CallFaceCapture {
    public static let startupMs = 1500.0
    public static let restartDelayMs = 300.0

    public static func rotation(deviceOrientation: Int, front: Bool, last: Int) -> Int {
        switch deviceOrientation {
        case 1: return 90
        case 2: return 270
        case 3: return front ? 180 : 0
        case 4: return front ? 0 : 180
        default: return last
        }
    }

    public static func pick(_ formats: [CallFaceVideoFormat], width: Int, height: Int, fps: Int) -> Int? {
        guard !formats.isEmpty else { return nil }
        let fast = formats.indices.filter { formats[$0].fps >= fps }
        let pool = fast.isEmpty ? Array(formats.indices) : fast
        let rate = fast.isEmpty ? pool.map { formats[$0].fps }.max()! : pool.map { formats[$0].fps }.min()!
        let rated = pool.filter { formats[$0].fps == rate }
        let long = max(width, height)
        let short = min(width, height)
        func covers(_ f: CallFaceVideoFormat) -> Bool { max(f.width, f.height) >= long && min(f.width, f.height) >= short }
        let covering = rated.filter { covers(formats[$0]) }
        if let smallest = covering.min(by: { formats[$0].width * formats[$0].height < formats[$1].width * formats[$1].height }) { return smallest }
        return rated.max { formats[$0].width * formats[$0].height < formats[$1].width * formats[$1].height }
    }
}

public final class CallFaceFrameGate {
    private var due: Int64?

    public init() {}

    public func reset() {
        due = nil
    }

    public func admit(timeNs: Int64, fps: Int) -> Bool {
        let interval = Int64(1_000_000_000 / max(1, fps))
        let slack = interval / 4
        guard let next = due else {
            due = timeNs + interval
            return true
        }
        guard timeNs >= next - slack else { return false }
        due = timeNs > next + interval ? timeNs + interval : next + interval
        return true
    }
}
