import Foundation

public typealias CallFaceRegions = [String: [[Double]]]

public struct CallFaceFrame: Equatable, Sendable {
    public let left: CallFacePoint
    public let right: CallFacePoint
    public let mid: CallFacePoint
    public let span: Double
    public let ux: CallFacePoint
    public let uy: CallFacePoint
    public let roll: Double
}

public struct CallFaceAnchors: Equatable, Sendable {
    public var eyeL: CallFacePoint
    public var eyeR: CallFacePoint
    public var mouth: CallFacePoint?
    public var mouthWidth: Double?
    public var open: Double?
    public var yaw: Double?

    public init(eyeL: CallFacePoint, eyeR: CallFacePoint, mouth: CallFacePoint? = nil, mouthWidth: Double? = nil, open: Double? = nil, yaw: Double? = nil) {
        self.eyeL = eyeL
        self.eyeR = eyeR
        self.mouth = mouth
        self.mouthWidth = mouthWidth
        self.open = open
        self.yaw = yaw
    }
}

public struct CallFacePose: Equatable, Sendable {
    public let scale: Double
    public let rows: [[Double]]
    public let t: [Double]
    public let roll: Double
    public let yaw: Double
    public let pitch: Double
    public let depth: Bool
}

public struct CallFaceTriangle: Equatable, Sendable {
    public let v: [Int]
    public let hole: [Double]
    public let mouth: Bool
    public let cull: Bool
}

public struct CallFaceHeadMesh: Equatable, Sendable {
    public let triangles: [CallFaceTriangle]
    public let count: Int
    public let extraStart: Int
    public let mouthStart: Int
}

public struct CallFaceLiveHead: Equatable, Sendable {
    public let points: [CallFacePoint]
    public let pose: CallFacePose
    public let jawTrust: [Double]
}

public enum CallFaceGeometry {
    public static let landmarkCount = 68
    public static let jaw = Array(0..<17)
    public static let leftEye = Array(36..<42)
    public static let rightEye = Array(42..<48)
    public static let outerLips = Array(48..<60)
    public static let innerLips = Array(60..<68)
    public static let foreheadLift = 0.5
    public static let ringStart = 78

    static let mirror = [
        16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
        26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
        27, 28, 29, 30, 35, 34, 33, 32, 31,
        45, 44, 43, 42, 47, 46, 39, 38, 37, 36, 41, 40,
        54, 53, 52, 51, 50, 49, 48, 59, 58, 57, 56, 55,
        64, 63, 62, 61, 60, 67, 66, 65,
    ]

    static let depthTable: [Double] = [
        -0.95, -0.9, -0.8, -0.65, -0.45, -0.25, -0.08, 0.05, 0.1, 0.05, -0.08, -0.25, -0.45, -0.65, -0.8, -0.9, -0.95,
        -0.25, -0.08, 0.02, 0.05, 0.05, 0.05, 0.05, 0.02, -0.08, -0.25,
        0, 0.12, 0.25, 0.38, 0.12, 0.2, 0.25, 0.2, 0.12,
        -0.12, -0.04, -0.04, -0.03, -0.04, -0.04, -0.03, -0.04, -0.04, -0.12, -0.04, -0.04,
        0.02, 0.12, 0.17, 0.19, 0.17, 0.12, 0.02, 0.12, 0.15, 0.16, 0.15, 0.12,
        0.04, 0.13, 0.14, 0.13, 0.04, 0.13, 0.14, 0.13,
    ]

    public static let poseWeights: [Double] = (0..<68).map { i in i <= 16 ? 0.4 : i <= 26 ? 0.8 : i <= 35 ? 1.4 : i <= 47 ? 1.4 : 0 }
    static let residualSigma = 0.55
    static let residualAnchor = 0.08
    static let foreheadCurve = 1.5
    static let foreheadBack = -0.12
    static let ringDepth = -1.0
    static let headCenter = CallFacePoint(0, -0.2, -1.1)
    static let sideReach = 0.4
    static let sideBack = 0.35
    static let jawFollow = 0.7
    static let jawFacing = (0.08, 0.38)
    static let jawNormalZ = 0.55
    static let fieldSources: [(Int, Double)] = (17..<36).map { ($0, 1.0) } + [(36, 1), (39, 1), (42, 1), (45, 1)]
    static let cheekSources: [(Int, Double)] = fieldSources + [(48, 0.6), (54, 0.6), (57, 0.5)]
    static let cheekSide: [([(Int, Double)], Double)] = [
        ([(2, 0.5), (31, 0.5)], 0.2),
        ([(1, 0.5), (41, 0.5)], 0.16),
        ([(4, 0.5), (48, 0.5)], 0.18),
        ([(3, 0.4), (31, 0.3), (48, 0.3)], 0.2),
        ([(6, 0.5), (59, 0.5)], 0.1),
        ([(40, 0.5), (31, 0.5)], 0.08),
        ([(0, 0.55), (36, 0.45)], 0.1),
    ]
    static let cheekMiddle: [([(Int, Double)], Double)] = [([(8, 0.5), (57, 0.5)], 0.08)]
    static let chinDrop: [(Int, Double)] = [(5, 0.3), (6, 0.6), (7, 0.85), (8, 1), (9, 0.85), (10, 0.6), (11, 0.3)]
    static let maxOpen = 0.42
    static let noseTrust = 0.06
    static let ringGap = 0.05
    static let nose = Array(27..<36)
    static let noseRigid = 0.85
    static let depthSpread = 0.04
    static let lipSteps = 4
    static let mouthFeather = 0.14
    static let mouthDeep = 0.4
    static let upperInner = [60, 61, 62, 63, 64]
    static let lowerInner = [60, 67, 66, 65, 64]
    static let upperOuter = [48, 49, 50, 51, 52, 53, 54]
    static let lowerOuter = [48, 59, 58, 57, 56, 55, 54]
    static let lipKeep = 0.65
    static let cornerTaper = 0.35
    static let openProfile: [(Int, Double)] = [(55, 0.45), (56, 0.8), (57, 1), (58, 0.8), (59, 0.45), (65, 0.8), (66, 1), (67, 0.8)]
    static let liftProfile: [(Int, Double)] = [(61, 0.7), (62, 1), (63, 0.7)]

    typealias Weights = [(Int, Double)]

    struct MouthRef {
        let landmark: Int?
        let extra: Int?
        let weights: Weights
    }

    struct Mouth {
        var extras: [Weights] = []
        var upper: [MouthRef] = []
        var lower: [MouthRef] = []
        var innerUpper: [MouthRef] = []
        var innerLower: [MouthRef] = []
        var deepUpper: [MouthRef] = []
        var deepLower: [MouthRef] = []
        var contourCount = 0
    }

    static let mouth: Mouth = buildMouth()
    public static let mouthExtraCount = mouth.extras.count
    static let cheeks: [(Weights, Double)] = cheekSide + cheekSide.map { weights, lift in (weights.map { (mirror[$0.0], $0.1) }, lift) } + cheekMiddle
    public static let cheekCount = cheeks.count
    public static let extraCount = cheekCount + mouthExtraCount

    public static func pointsFromFlat(_ flat: [Double]) -> [CallFacePoint] {
        var points: [CallFacePoint] = []
        var i = 0
        while i + 1 < flat.count {
            points.append(CallFacePoint(flat[i], flat[i + 1]))
            i += 2
        }
        return points
    }

    public static func centroid(_ points: [CallFacePoint], _ indices: [Int]) -> CallFacePoint {
        var x = 0.0
        var y = 0.0
        for i in indices {
            x += points[i].x
            y += points[i].y
        }
        return CallFacePoint(x / Double(indices.count), y / Double(indices.count))
    }

    public static func faceFrame(_ points: [CallFacePoint]) -> CallFaceFrame {
        let left = centroid(points, leftEye)
        let right = centroid(points, rightEye)
        let dx = right.x - left.x
        let dy = right.y - left.y
        let span = CallFaceJS.or(CallFaceJS.hypot(dx, dy), 1)
        let ux = CallFacePoint(dx / span, dy / span)
        return CallFaceFrame(
            left: left,
            right: right,
            mid: CallFacePoint((left.x + right.x) / 2, (left.y + right.y) / 2),
            span: span,
            ux: ux,
            uy: CallFacePoint(-ux.y, ux.x),
            roll: atan2(dy, dx)
        )
    }

    static func toFrame(_ frame: CallFaceFrame, _ point: CallFacePoint) -> CallFacePoint {
        let x = point.x - frame.mid.x
        let y = point.y - frame.mid.y
        return CallFacePoint(x * frame.ux.x + y * frame.ux.y, x * frame.uy.x + y * frame.uy.y)
    }

    static func fromFrame(_ frame: CallFaceFrame, _ a: Double, _ b: Double) -> CallFacePoint {
        CallFacePoint(frame.mid.x + a * frame.ux.x + b * frame.uy.x, frame.mid.y + a * frame.ux.y + b * frame.uy.y)
    }

    public static func normalizeShape(_ points: [CallFacePoint]) -> [CallFacePoint] {
        let frame = faceFrame(points)
        return points.map { point in
            let local = toFrame(frame, point)
            return CallFacePoint(local.x / frame.span, local.y / frame.span)
        }
    }

    public static func mouthGap(_ points: [CallFacePoint]) -> Double {
        let frame = faceFrame(points)
        let top = toFrame(frame, points[62])
        let bottom = toFrame(frame, points[66])
        return (bottom.y - top.y) / frame.span
    }

    public static func buildTemplate(_ shapes: [[CallFacePoint]]) -> [CallFacePoint] {
        let normalized = shapes.map(normalizeShape)
        let closed = normalized.filter { $0[66].y - $0[62].y < 0.05 }
        let mouthSource = closed.isEmpty ? normalized : closed
        var mean: [CallFacePoint] = []
        for i in 0..<landmarkCount {
            let pool = i >= 48 ? mouthSource : normalized
            var x = 0.0
            var y = 0.0
            for shape in pool {
                x += shape[i].x
                y += shape[i].y
            }
            mean.append(CallFacePoint(x / Double(pool.count), y / Double(pool.count)))
        }
        var symmetric = mean.enumerated().map { i, point in
            let twin = mean[mirror[i]]
            return CallFacePoint((point.x - twin.x) / 2, (point.y + twin.y) / 2)
        }
        for (upper, lower) in [(61, 67), (62, 66), (63, 65)] {
            let y = (symmetric[upper].y + symmetric[lower].y) / 2
            symmetric[upper].y = y
            symmetric[lower].y = y
        }
        return symmetric.enumerated().map { i, point in CallFacePoint(point.x, point.y, depthTable[i]) }
    }

    public static func fitTemplate(_ template: [CallFacePoint], _ anchors: CallFaceAnchors) -> [CallFacePoint] {
        let yaw = clampValue(CallFaceJS.truthy(anchors.yaw) ? anchors.yaw! : 0, -0.7, 0.7)
        let cosine = cos(yaw)
        let sine = sin(yaw)
        let turned = template.map { CallFacePoint($0.x * cosine + $0.depth * sine, $0.y) }
        let frame = faceFrame(turned)
        let eyeL = anchors.eyeL
        let eyeR = anchors.eyeR
        let dx = eyeR.x - eyeL.x
        let dy = eyeR.y - eyeL.y
        let span = CallFaceJS.or(CallFaceJS.hypot(dx, dy), 1)
        let scale = span / frame.span
        let angle = atan2(dy, dx) - frame.roll
        let c = cos(angle) * scale
        let s = sin(angle) * scale
        let mid = CallFacePoint((eyeL.x + eyeR.x) / 2, (eyeL.y + eyeR.y) / 2)
        let placed = turned.map { point -> CallFacePoint in
            let px = point.x - frame.mid.x
            let py = point.y - frame.mid.y
            return CallFacePoint(mid.x + px * c - py * s, mid.y + px * s + py * c)
        }
        let live = faceFrame(placed)
        var local = placed.map { toFrame(live, $0) }
        let mouthAt = CallFacePoint((local[48].x + local[54].x) / 2, (local[48].y + local[54].y) / 2)
        if let mouth = anchors.mouth {
            let target = toFrame(live, mouth)
            let stretch = clampValue(target.y / CallFaceJS.or(mouthAt.y, 1), 0.7, 1.35)
            let shift = clampValue(target.x - mouthAt.x, -0.35 * live.span, 0.35 * live.span)
            for k in local.indices {
                if local[k].y > 0 {
                    local[k].x += shift * clampValue(local[k].y / mouthAt.y, 0, 1.4)
                    local[k].y *= stretch
                } else {
                    local[k].y *= 1 + (stretch - 1) * 0.5
                }
            }
        }
        let center = CallFacePoint((local[48].x + local[54].x) / 2, (local[48].y + local[54].y) / 2)
        if CallFaceJS.truthy(anchors.mouthWidth) {
            let width = CallFaceJS.or(CallFaceJS.hypot(local[54].x - local[48].x, local[54].y - local[48].y), 1)
            let ratio = pow(clampValue(anchors.mouthWidth! / width, 0.75, 1.4), 0.8)
            for i in 48..<68 {
                local[i].x = center.x + (local[i].x - center.x) * ratio
            }
        }
        let drop = clampValue(CallFaceJS.truthy(anchors.open) ? anchors.open! : 0, 0, 1) * maxOpen * live.span
        if drop > 0 {
            for (i, weight) in openProfile { local[i].y += drop * weight }
            for (i, weight) in liftProfile { local[i].y -= drop * 0.08 * weight }
            for (i, weight) in chinDrop { local[i].y += drop * 0.85 * weight }
        }
        return local.map { fromFrame(live, $0.x, $0.y) }
    }

    public static func delaunay(_ points: [CallFacePoint]) -> [[Int]] {
        var minX = Double.infinity
        var minY = Double.infinity
        var maxX = -Double.infinity
        var maxY = -Double.infinity
        for point in points {
            minX = CallFaceJS.min(minX, point.x)
            minY = CallFaceJS.min(minY, point.y)
            maxX = CallFaceJS.max(maxX, point.x)
            maxY = CallFaceJS.max(maxY, point.y)
        }
        let size = CallFaceJS.or(CallFaceJS.max(maxX - minX, maxY - minY), 1)
        let midX = (minX + maxX) / 2
        let midY = (minY + maxY) / 2
        let all = points + [
            CallFacePoint(midX - 20 * size, midY - size),
            CallFacePoint(midX, midY + 20 * size),
            CallFacePoint(midX + 20 * size, midY - size),
        ]
        let n = points.count
        struct Circle {
            let x: Double
            let y: Double
            let r: Double
        }
        struct Triangle {
            let v: [Int]
            let c: Circle
        }
        func circle(_ a: Int, _ b: Int, _ c: Int) -> Circle {
            let ax = all[a].x, ay = all[a].y
            let bx = all[b].x, by = all[b].y
            let cx = all[c].x, cy = all[c].y
            let d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
            if abs(d) < 1e-12 { return Circle(x: 0, y: 0, r: .infinity) }
            let a2 = ax * ax + ay * ay
            let b2 = bx * bx + by * by
            let c2 = cx * cx + cy * cy
            let x = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
            let y = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
            return Circle(x: x, y: y, r: (x - ax) * (x - ax) + (y - ay) * (y - ay))
        }
        var triangles = [Triangle(v: [n, n + 1, n + 2], c: circle(n, n + 1, n + 2))]
        for i in 0..<n {
            let px = all[i].x
            let py = all[i].y
            var bad: [Triangle] = []
            var keep: [Triangle] = []
            for t in triangles {
                if (px - t.c.x) * (px - t.c.x) + (py - t.c.y) * (py - t.c.y) < t.c.r { bad.append(t) } else { keep.append(t) }
            }
            var order: [Int64] = []
            var edges: [Int64: (Int, Int)?] = [:]
            for t in bad {
                for (a, b) in [(t.v[0], t.v[1]), (t.v[1], t.v[2]), (t.v[2], t.v[0])] {
                    let key = a < b ? Int64(a) << 32 | Int64(b) : Int64(b) << 32 | Int64(a)
                    if edges[key] != nil {
                        edges[key] = .some(nil)
                    } else {
                        edges[key] = .some((a, b))
                        order.append(key)
                    }
                }
            }
            for key in order {
                if let edge = edges[key] ?? nil {
                    keep.append(Triangle(v: [edge.0, edge.1, i], c: circle(edge.0, edge.1, i)))
                }
            }
            triangles = keep
        }
        return triangles.filter { $0.v.allSatisfy { $0 < n } }.map(\.v)
    }

    static func catmullWeights(_ t: Double) -> [Double] {
        let t2 = t * t
        let t3 = t2 * t
        return [0.5 * (-t + 2 * t2 - t3), 0.5 * (2 - 5 * t2 + 3 * t3), 0.5 * (t + 4 * t2 - 3 * t3), 0.5 * (t3 - t2)]
    }

    static func addWeight(_ weights: inout Weights, _ index: Int, _ value: Double) {
        if let at = weights.firstIndex(where: { $0.0 == index }) {
            weights[at].1 = weights[at].1 + value
        } else {
            weights.append((index, 0 + value))
        }
    }

    static func lipSamples(_ control: [Int]) -> [Weights] {
        let last = control.count - 1
        func at(_ j: Int) -> Weights {
            if j < 0 { return [(control[0], 2), (control[1], -1)] }
            if j > last { return [(control[last], 2), (control[last - 1], -1)] }
            return [(control[j], 1)]
        }
        var samples: [Weights] = []
        for j in 0..<last {
            samples.append([(control[j], 1)])
            for s in 1..<lipSteps {
                let w = catmullWeights(Double(s) / Double(lipSteps))
                var weights: Weights = []
                for (n, k) in [j - 1, j, j + 1, j + 2].enumerated() {
                    for (index, scale) in at(k) { addWeight(&weights, index, w[n] * scale) }
                }
                samples.append(weights)
            }
        }
        samples.append([(control[last], 1)])
        return samples
    }

    static func blendWeights(_ a: Weights, _ b: Weights, _ amount: Double) -> Weights {
        var weights: Weights = []
        for (index, w) in a { addWeight(&weights, index, w * (1 - amount)) }
        for (index, w) in b { addWeight(&weights, index, w * amount) }
        return weights
    }

    static func buildMouth() -> Mouth {
        var mouth = Mouth()
        func reference(_ weights: Weights) -> MouthRef {
            if weights.count == 1 && weights[0].1 == 1 { return MouthRef(landmark: weights[0].0, extra: nil, weights: weights) }
            mouth.extras.append(weights)
            return MouthRef(landmark: nil, extra: mouth.extras.count - 1, weights: weights)
        }
        let upper = lipSamples(upperInner).map(reference)
        let lower = lipSamples(lowerInner).map(reference)
        let contourCount = mouth.extras.count
        let last = upper.count - 1
        func inner(_ from: [MouthRef], _ to: [MouthRef], _ amount: Double) -> [MouthRef] {
            from.enumerated().map { k, ref in k == 0 || k == last ? ref : reference(blendWeights(ref.weights, to[k].weights, amount)) }
        }
        let innerUpper = inner(upper, lower, mouthFeather)
        let innerLower = inner(lower, upper, mouthFeather)
        let deepUpper = inner(upper, lower, mouthDeep)
        let deepLower = inner(lower, upper, mouthDeep)
        mouth.upper = upper
        mouth.lower = lower
        mouth.contourCount = contourCount
        mouth.innerUpper = innerUpper
        mouth.innerLower = innerLower
        mouth.deepUpper = deepUpper
        mouth.deepLower = deepLower
        return mouth
    }

    static func combine(_ points: [CallFacePoint], _ weights: Weights) -> CallFacePoint {
        var dims = Int.max
        for (i, _) in weights { dims = Swift.min(dims, points[i].dims) }
        var out = [Double](repeating: 0, count: dims)
        for (i, w) in weights {
            let values = points[i].values
            for k in 0..<dims { out[k] += w * values[k] }
        }
        return CallFacePoint(out)
    }

    public static func mouthExtras(_ points: [CallFacePoint]) -> [CallFacePoint] {
        mouth.extras.map { combine(points, $0) }
    }

    public static func cheekExtras(_ points: [CallFacePoint], lift: Bool = false) -> [CallFacePoint] {
        cheeks.map { weights, bulge in
            var point = combine(points, weights)
            if lift, let z = point.z { point.z = z + bulge }
            return point
        }
    }

    public static func mouthContour(_ points: [CallFacePoint]) -> [CallFacePoint] {
        func pick(_ ref: MouthRef) -> CallFacePoint { ref.landmark.map { points[$0] } ?? combine(points, ref.weights) }
        return mouth.upper.map(pick) + mouth.lower[1..<(mouth.lower.count - 1)].reversed().map(pick)
    }

    public static func insidePolygon(_ polygon: [CallFacePoint], _ point: CallFacePoint) -> Bool {
        var inside = false
        var j = polygon.count - 1
        for i in 0..<polygon.count {
            let xi = polygon[i].x, yi = polygon[i].y
            let xj = polygon[j].x, yj = polygon[j].y
            if (yi > point.y) != (yj > point.y) && point.x < ((xj - xi) * (point.y - yi)) / (yj - yi) + xi { inside.toggle() }
            j = i
        }
        return inside
    }

    public static func headLayout(_ points: [CallFacePoint], _ ring: [CallFacePoint], forehead: Double = foreheadLift) -> [CallFacePoint] {
        let frame = faceFrame(points)
        let lift = forehead * frame.span
        var out = points.map(\.flat)
        for i in 17...26 {
            out.append(CallFacePoint(points[i].x - frame.uy.x * lift, points[i].y - frame.uy.y * lift))
        }
        for point in ring { out.append(point.flat) }
        for (anchor, point) in sideAnchors(out, ring.count) {
            out.append(CallFacePoint(out[anchor].x + (point.x - out[anchor].x) * sideReach, out[anchor].y + (point.y - out[anchor].y) * sideReach))
        }
        return out + cheekExtras(out) + mouthExtras(out)
    }

    static func sideAnchors(_ layout: [CallFacePoint], _ count: Int) -> [(Int, CallFacePoint)] {
        let edge = jaw + Array(landmarkCount..<(landmarkCount + 10))
        return layout[ringStart..<(ringStart + count)].map { point in
            var best = edge[0]
            var near = Double.infinity
            for i in edge {
                let dx = layout[i].x - point.x
                let dy = layout[i].y - point.y
                let d = dx * dx + dy * dy
                if d < near {
                    near = d
                    best = i
                }
            }
            return (best, point)
        }
    }

    public static func ringCount(_ length: Int) -> Int {
        (length - extraCount - ringStart) / 2
    }

    static func vertexDepth(_ i: Int, _ count: Int, _ anchors: [(Int, CallFacePoint)]) -> Double {
        if i < landmarkCount { return depthTable[i] }
        if i < ringStart { return depthTable[i - landmarkCount + 17] * foreheadCurve + foreheadBack }
        if i < ringStart + count { return ringDepth }
        return vertexDepth(anchors[i - ringStart - count].0, count, anchors) - sideBack
    }

    public static func headModel(_ points: [CallFacePoint], _ ring: [CallFacePoint]) -> [CallFacePoint] {
        let frame = faceFrame(points)
        let layout = headLayout(points, ring)
        let anchors = sideAnchors(layout, ring.count)
        let base = layout[0..<(layout.count - extraCount)].enumerated().map { i, point -> CallFacePoint in
            let local = toFrame(frame, point)
            return CallFacePoint(local.x / frame.span, local.y / frame.span, vertexDepth(i, ring.count, anchors))
        }
        return base + cheekExtras(base, lift: true) + mouthExtras(base)
    }

    static func triangleArea(_ points: [CallFacePoint], _ v: [Int]) -> Double {
        let a = points[v[0]], b = points[v[1]], c = points[v[2]]
        return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)
    }

    public static func buildHeadMesh(_ model: [CallFacePoint], _ template: [CallFacePoint]) -> CallFaceHeadMesh {
        let open = fitTemplate(template, CallFaceAnchors(eyeL: CallFacePoint(-0.5, 0), eyeR: CallFacePoint(0.5, 0), open: 0.6))
        let start = model.count - extraCount
        let mouthStart = start + cheekCount
        let base = model[0..<start].enumerated().map { i, point in i < landmarkCount ? open[i] : point.flat }
        let layout = base + cheekExtras(base) + mouthExtras(open)
        func vertex(_ ref: MouthRef) -> Int { ref.landmark ?? mouthStart + ref.extra! }
        let contour = mouth.upper.map(vertex) + mouth.lower[1..<(mouth.lower.count - 1)].reversed().map(vertex)
        let onContour = Set(contour)
        let polygon = contour.map { layout[$0] }
        var triangles = delaunay(Array(layout[0..<(mouthStart + mouth.contourCount)]))
            .filter { v in !(v.allSatisfy { onContour.contains($0) } && insidePolygon(polygon, centroid(layout, v))) }
            .filter { abs(triangleArea(model, $0)) > 1e-9 }
            .map { v in CallFaceTriangle(v: v, hole: [0, 0, 0], mouth: false, cull: !v.contains { ($0 >= 48 && $0 < landmarkCount) || $0 >= mouthStart }) }
        let corners: Set<Int> = [upperInner[0], upperInner[upperInner.count - 1]]
        func add(_ v: [Int], _ hole: [Double]) {
            if Set(v).count < 3 { return }
            let holes = v.enumerated().map { k, index in corners.contains(index) ? CallFaceJS.min(hole[k], 0.5) : hole[k] }
            triangles.append(CallFaceTriangle(v: v, hole: holes, mouth: true, cull: false))
        }
        func strip(_ a: [MouthRef], _ b: [MouthRef], _ ha: Double, _ hb: Double) {
            var k = 0
            while k + 1 < a.count {
                let p = vertex(a[k])
                let q = vertex(a[k + 1])
                let r = vertex(b[k + 1])
                let s = vertex(b[k])
                add([p, q, r], [ha, ha, hb])
                add([p, r, s], [ha, hb, hb])
                k += 1
            }
        }
        strip(mouth.upper, mouth.innerUpper, 0, 1)
        strip(mouth.lower, mouth.innerLower, 0, 1)
        strip(mouth.innerUpper, mouth.deepUpper, 1, 2)
        strip(mouth.innerLower, mouth.deepLower, 1, 2)
        strip(mouth.deepUpper, mouth.deepLower, 2, 2)
        return CallFaceHeadMesh(triangles: triangles, count: layout.count, extraStart: start, mouthStart: mouthStart)
    }

    public static func hasDepth(_ live: [CallFacePoint]) -> Bool {
        var low = Double.infinity
        var high = -Double.infinity
        for i in 0..<landmarkCount {
            if poseWeights[i] == 0 { continue }
            guard i < live.count, let z = live[i].z, z.isFinite else { return false }
            low = CallFaceJS.min(low, z)
            high = CallFaceJS.max(high, z)
        }
        let span = CallFaceJS.or(CallFaceJS.hypot(live[45].x - live[36].x, live[45].y - live[36].y), 1)
        return high - low > depthSpread * span
    }

    static func dominantEigenvector(_ matrix: [[Double]]) -> [Double] {
        var a = matrix
        var v = (0..<4).map { i in (0..<4).map { j in i == j ? 1.0 : 0.0 } }
        for _ in 0..<24 {
            var off = 0.0
            for p in 0..<4 { for q in (p + 1)..<4 { off += a[p][q] * a[p][q] } }
            if off < 1e-18 { break }
            for p in 0..<4 {
                for q in (p + 1)..<4 {
                    if abs(a[p][q]) < 1e-20 { continue }
                    let theta = (a[q][q] - a[p][p]) / (2 * a[p][q])
                    let t = CallFaceJS.sign(CallFaceJS.or(theta, 1)) / (abs(theta) + (theta * theta + 1).squareRoot())
                    let c = 1 / (t * t + 1).squareRoot()
                    let s = t * c
                    for k in 0..<4 {
                        let akp = a[k][p]
                        let akq = a[k][q]
                        a[k][p] = c * akp - s * akq
                        a[k][q] = s * akp + c * akq
                    }
                    for k in 0..<4 {
                        let apk = a[p][k]
                        let aqk = a[q][k]
                        a[p][k] = c * apk - s * aqk
                        a[q][k] = s * apk + c * aqk
                    }
                    for k in 0..<4 {
                        let vkp = v[k][p]
                        let vkq = v[k][q]
                        v[k][p] = c * vkp - s * vkq
                        v[k][q] = s * vkp + c * vkq
                    }
                }
            }
        }
        var best = 0
        for i in 1..<4 where a[i][i] > a[best][best] { best = i }
        return [v[0][best], v[1][best], v[2][best], v[3][best]]
    }

    static func component(_ point: CallFacePoint, _ k: Int) -> Double {
        k == 0 ? point.x : k == 1 ? point.y : point.depth
    }

    public static func solvePoseWithDepth(_ model: [CallFacePoint], _ live: [CallFacePoint], weights: [Double] = poseWeights) -> CallFacePose? {
        var total = 0.0
        var mm = [0.0, 0.0, 0.0]
        var lm = [0.0, 0.0, 0.0]
        for i in 0..<landmarkCount {
            let w = weights[i]
            if !CallFaceJS.truthy(w) { continue }
            total += w
            for k in 0..<3 {
                mm[k] += w * component(model[i], k)
                lm[k] += w * component(live[i], k)
            }
        }
        for k in 0..<3 {
            mm[k] /= total
            lm[k] /= total
        }
        var S = [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
        for i in 0..<landmarkCount {
            let w = weights[i]
            if !CallFaceJS.truthy(w) { continue }
            for a in 0..<3 {
                for b in 0..<3 { S[a][b] += w * (component(model[i], a) - mm[a]) * (component(live[i], b) - lm[b]) }
            }
        }
        let sxx = S[0][0], sxy = S[0][1], sxz = S[0][2]
        let syx = S[1][0], syy = S[1][1], syz = S[1][2]
        let szx = S[2][0], szy = S[2][1], szz = S[2][2]
        let q = dominantEigenvector([
            [sxx + syy + szz, syz - szy, szx - sxz, sxy - syx],
            [syz - szy, sxx - syy - szz, sxy + syx, szx + sxz],
            [szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy],
            [sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz],
        ])
        let qw = q[0], qx = q[1], qy = q[2], qz = q[3]
        let r1 = [qw * qw + qx * qx - qy * qy - qz * qz, 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)]
        let r2 = [2 * (qx * qy + qw * qz), qw * qw - qx * qx + qy * qy - qz * qz, 2 * (qy * qz - qw * qx)]
        let r3 = [2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), qw * qw - qx * qx - qy * qy + qz * qz]
        var num = 0.0
        var den = 0.0
        for i in 0..<landmarkCount {
            let w = weights[i]
            if !CallFaceJS.truthy(w) { continue }
            let m = [model[i].x - mm[0], model[i].y - mm[1], model[i].depth - mm[2]]
            let px = dot3(r1, m)
            let py = dot3(r2, m)
            num += w * ((live[i].x - lm[0]) * px + (live[i].y - lm[1]) * py)
            den += w * (px * px + py * py)
        }
        if !(den > 0) || !(num > 0) { return nil }
        let scale = num / den
        return CallFacePose(
            scale: scale,
            rows: [r1, r2, r3],
            t: [lm[0] - scale * dot3(r1, mm), lm[1] - scale * dot3(r2, mm)],
            roll: atan2(r2[0], r1[0]),
            yaw: asin(clampValue(-r3[0], -1, 1)),
            pitch: asin(clampValue(r3[1], -1, 1)),
            depth: true
        )
    }

    public static func solvePose(_ model: [CallFacePoint], _ live: [CallFacePoint], weights: [Double] = poseWeights) -> CallFacePose {
        if hasDepth(live), let pose = solvePoseWithDepth(model, live, weights: weights) { return pose }
        var total = 0.0
        var mm = [0.0, 0.0, 0.0]
        var lm = [0.0, 0.0]
        for i in 0..<landmarkCount {
            let w = weights[i]
            if !CallFaceJS.truthy(w) { continue }
            total += w
            for k in 0..<3 { mm[k] += w * component(model[i], k) }
            lm[0] += w * live[i].x
            lm[1] += w * live[i].y
        }
        for k in 0..<3 { mm[k] /= total }
        lm[0] /= total
        lm[1] /= total
        var mtm = [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
        var ltm = [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
        for i in 0..<landmarkCount {
            let w = weights[i]
            if !CallFaceJS.truthy(w) { continue }
            let m = [model[i].x - mm[0], model[i].y - mm[1], model[i].depth - mm[2]]
            let l = [live[i].x - lm[0], live[i].y - lm[1]]
            for a in 0..<3 {
                for b in 0..<3 { mtm[a][b] += w * m[a] * m[b] }
                ltm[0][a] += w * l[0] * m[a]
                ltm[1][a] += w * l[1] * m[a]
            }
        }
        for a in 0..<3 { mtm[a][a] += 1e-4 }
        let inverse = invert3(mtm)
        let rows = ltm.map { row in (0..<3).map { b in row[0] * inverse[0][b] + row[1] * inverse[1][b] + row[2] * inverse[2][b] } }
        let s1 = CallFaceJS.or(CallFaceJS.hypot(rows[0][0], rows[0][1], rows[0][2]), 1)
        let s2 = CallFaceJS.or(CallFaceJS.hypot(rows[1][0], rows[1][1], rows[1][2]), 1)
        let a = rows[0].map { $0 / s1 }
        let b = rows[1].map { $0 / s2 }
        let c = normalize3((0..<3).map { a[$0] + b[$0] })
        let d = normalize3((0..<3).map { a[$0] - b[$0] })
        var r1 = (0..<3).map { (c[$0] + d[$0]) / 2.0.squareRoot() }
        var r2 = (0..<3).map { (c[$0] - d[$0]) / 2.0.squareRoot() }
        let scale = (s1 + s2) / 2
        if noseDisagrees(model, live, r1, r2, scale) {
            r1[2] = -r1[2]
            r2[2] = -r2[2]
        }
        let r3 = [r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0]]
        let t = [lm[0] - scale * dot3(r1, mm), lm[1] - scale * dot3(r2, mm)]
        return CallFacePose(
            scale: scale,
            rows: [r1, r2, r3],
            t: t,
            roll: atan2(r2[0], r1[0]),
            yaw: asin(clampValue(-r3[0], -1, 1)),
            pitch: asin(clampValue(r3[1], -1, 1)),
            depth: false
        )
    }

    public static func noseTurn(_ points: [CallFacePoint]) -> Double {
        let frame = faceFrame(points)
        let tip = toFrame(frame, points[30])
        let bridge = toFrame(frame, points[27])
        return (tip.x - bridge.x) / frame.span
    }

    static func noseDisagrees(_ model: [CallFacePoint], _ live: [CallFacePoint], _ r1: [Double], _ r2: [Double], _ scale: Double) -> Bool {
        let seen = noseTurn(live) - (model[30].x - model[27].x)
        if abs(seen) < noseTrust { return false }
        let turn = r1[2] * (model[30].depth - model[27].depth)
        return CallFaceJS.sign(turn) != CallFaceJS.sign(seen) && abs(turn) > noseTrust * 0.5
    }

    public static func projectPose(_ pose: CallFacePose, _ point: CallFacePoint) -> CallFacePoint {
        let p = [point.x, point.y, point.depth]
        return CallFacePoint(pose.t[0] + pose.scale * dot3(pose.rows[0], p), pose.t[1] + pose.scale * dot3(pose.rows[1], p))
    }

    public static func projectOutline(_ pose: CallFacePose, _ point: CallFacePoint) -> CallFacePoint {
        let center = projectPose(pose, headCenter)
        let c = cos(pose.roll)
        let s = sin(pose.roll)
        let dx = (point.x - headCenter.x) * pose.scale
        let dy = (point.y - headCenter.y) * pose.scale
        return CallFacePoint(center.x + dx * c - dy * s, center.y + dx * s + dy * c)
    }

    public static func rigidNose(_ model: [CallFacePoint], _ live: [CallFacePoint], _ pose: CallFacePose, _ out: inout [CallFacePoint]) {
        let rigid = nose.map { projectPose(pose, model[$0]) }
        var sx = 0.0
        var sy = 0.0
        for (k, i) in nose.enumerated() {
            sx += live[i].x - rigid[k].x
            sy += live[i].y - rigid[k].y
        }
        sx /= Double(nose.count)
        sy /= Double(nose.count)
        for (k, i) in nose.enumerated() {
            let x = rigid[k].x + sx
            let y = rigid[k].y + sy
            out[i] = CallFacePoint(live[i].x + (x - live[i].x) * noseRigid, live[i].y + (y - live[i].y) * noseRigid)
        }
    }

    public static func jawTrust(_ model: [CallFacePoint], _ pose: CallFacePose) -> [Double] {
        let r3 = pose.rows[2]
        return jaw.map { i in
            let x = model[i].x
            let y = model[i].y
            let facing = dot3(r3, normalize3([x, CallFaceJS.max(0, y - 1) * 0.8, jawNormalZ]))
            return smoothstepValue(jawFacing.0, jawFacing.1, facing)
        }
    }

    struct Source {
        let i: Int
        let dx: Double
        let dy: Double
        let trust: Double
    }

    static func residualField(_ model: [CallFacePoint], _ sources: [Source], _ at: CallFacePoint) -> (Double, Double) {
        var wx = 0.0
        var wy = 0.0
        var weight = residualAnchor
        for source in sources {
            if !CallFaceJS.truthy(source.trust) { continue }
            let ex = at.x - model[source.i].x
            let ey = at.y - model[source.i].y
            let d2 = ex * ex + ey * ey
            let w = source.trust * exp(-d2 / (2 * residualSigma * residualSigma))
            wx += w * source.dx
            wy += w * source.dy
            weight += w
        }
        return (wx / weight, wy / weight)
    }

    struct MouthAxes {
        let mid: CallFacePoint
        let ux: CallFacePoint
        let half: Double

        func local(_ p: CallFacePoint) -> CallFacePoint {
            CallFacePoint(((p.x - mid.x) * ux.x + (p.y - mid.y) * ux.y) / half, ((p.x - mid.x) * -ux.y + (p.y - mid.y) * ux.x) / half)
        }

        func place(_ p: CallFacePoint) -> CallFacePoint {
            CallFacePoint(mid.x + (p.x * ux.x - p.y * ux.y) * half, mid.y + (p.x * ux.y + p.y * ux.x) * half)
        }
    }

    static func mouthAxes(_ points: [CallFacePoint]) -> MouthAxes? {
        let a = points[48]
        let b = points[54]
        let half = CallFaceJS.hypot(b.x - a.x, b.y - a.y) / 2
        if !(half > 1e-6) { return nil }
        let ux = CallFacePoint((b.x - a.x) / (2 * half), (b.y - a.y) / (2 * half))
        return MouthAxes(mid: CallFacePoint((a.x + b.x) / 2, (a.y + b.y) / 2), ux: ux, half: half)
    }

    static func lipCurve(_ local: [CallFacePoint?], _ indices: [Int]) -> [CallFacePoint] {
        CallFaceJS.sorted(indices.map { local[$0]! }) { $0.x - $1.x }
    }

    static func curveAt(_ curve: [CallFacePoint], _ s: Double) -> Double {
        if s <= curve[0].x { return curve[0].y }
        for k in 1..<curve.count {
            let s1 = curve[k].x
            let t1 = curve[k].y
            if s <= s1 {
                let s0 = curve[k - 1].x
                let t0 = curve[k - 1].y
                return s1 - s0 > 1e-9 ? t0 + ((t1 - t0) * (s - s0)) / (s1 - s0) : t1
            }
        }
        return curve[curve.count - 1].y
    }

    public static func regularMouth(_ reference: [CallFacePoint], _ points: [CallFacePoint]) -> [CallFacePoint] {
        guard let modelAxes = mouthAxes(reference), let liveAxes = mouthAxes(points) else { return points }
        var ref = [CallFacePoint?](repeating: nil, count: landmarkCount)
        var seen = [CallFacePoint?](repeating: nil, count: landmarkCount)
        for i in 48..<68 {
            ref[i] = modelAxes.local(reference[i])
            seen[i] = liveAxes.local(points[i])
        }
        let refUpper = lipCurve(ref, upperOuter)
        let refLower = lipCurve(ref, lowerOuter)
        let refInnerUpper = lipCurve(ref, upperInner)
        let refInnerLower = lipCurve(ref, lowerInner)
        let upper = lipCurve(seen, upperOuter)
        let lower = lipCurve(seen, lowerOuter)
        let innerUpper = lipCurve(seen, upperInner)
        let innerLower = lipCurve(seen, lowerInner)
        var placed = [CallFacePoint?](repeating: nil, count: landmarkCount)
        for i in [49, 50, 51, 52, 53] { placed[i] = CallFacePoint(ref[i]!.x, curveAt(upper, ref[i]!.x)) }
        for i in [55, 56, 57, 58, 59] { placed[i] = CallFacePoint(ref[i]!.x, curveAt(lower, ref[i]!.x)) }
        placed[48] = seen[48]
        placed[54] = seen[54]
        let top = lipCurve(placed, upperOuter)
        let bottom = lipCurve(placed, lowerOuter)
        let reach = CallFaceJS.max(abs(ref[60]!.x), abs(ref[64]!.x), 1e-3)
        let centerGap = CallFaceJS.max(0, curveAt(innerLower, 0) - curveAt(innerUpper, 0))
        func inner(_ s: Double) -> (Double, Double) {
            let roof = curveAt(top, s) + lipKeep * CallFaceJS.max(0, curveAt(refInnerUpper, s) - curveAt(refUpper, s))
            let floor = curveAt(bottom, s) - lipKeep * CallFaceJS.max(0, curveAt(refLower, s) - curveAt(refInnerLower, s))
            let seenUpper = curveAt(innerUpper, s)
            let seenLower = curveAt(innerLower, s)
            let a = CallFaceJS.max(seenUpper, roof)
            let b = CallFaceJS.min(seenLower, floor)
            if b <= a {
                let seam = clampValue((seenUpper + seenLower) / 2, CallFaceJS.min(roof, floor), CallFaceJS.max(roof, floor))
                return (seam, seam)
            }
            let middle = (a + b) / 2
            let ratio = s / reach
            let room = CallFaceJS.min(b - a, centerGap * pow(CallFaceJS.max(0, 1 - ratio * ratio), cornerTaper))
            return (middle - room / 2, middle + room / 2)
        }
        for (up, down) in [(61, 67), (62, 66), (63, 65)] {
            let s = (ref[up]!.x + ref[down]!.x) / 2
            let (a, b) = inner(s)
            placed[up] = CallFacePoint(s, a)
            placed[down] = CallFacePoint(s, b)
        }
        for i in [60, 64] {
            let s = ref[i]!.x
            let a = curveAt(top, s)
            let b = curveAt(bottom, s)
            placed[i] = CallFacePoint(s, clampValue(seen[i]!.y, CallFaceJS.min(a, b), CallFaceJS.max(a, b)))
        }
        var out = points
        for i in 48..<68 { out[i] = liveAxes.place(placed[i]!) }
        return out
    }

    public static func liveHead(_ model: [CallFacePoint], _ live: [CallFacePoint]) -> CallFaceLiveHead {
        let first = solvePose(model, live)
        let trust = jawTrust(model, first)
        let pose = solvePose(model, live, weights: poseWeights.enumerated().map { i, w in i < jaw.count ? w * trust[i] : w })
        let settled = jawTrust(model, pose)
        let start = model.count - extraCount
        let outlineEnd = ringStart + ringCount(model.count)
        let projected = model.enumerated().map { v, point in v >= ringStart && v < outlineEnd ? projectOutline(pose, point) : projectPose(pose, point) }
        func source(_ i: Int, _ trust: Double) -> Source {
            Source(i: i, dx: live[i].x - projected[i].x, dy: live[i].y - projected[i].y, trust: trust)
        }
        let jawSources = jaw.map { source($0, settled[$0] * jawFollow) }
        let field = fieldSources.map { source($0.0, $0.1) } + jawSources
        let cheekField = cheekSources.map { source($0.0, $0.1) } + jawSources
        var out = live[0..<landmarkCount].map(\.flat)
        for i in jaw {
            let follow = settled[i] * jawFollow
            let (fx, fy) = residualField(model, cheekField, model[i])
            out[i] = CallFacePoint(
                projected[i].x + follow * jawSources[i].dx + (1 - follow) * fx,
                projected[i].y + follow * jawSources[i].dy + (1 - follow) * fy
            )
        }
        rigidNose(model, live, pose, &out)
        let shaped = regularMouth(projected, out)
        for i in 48..<landmarkCount { out[i] = shaped[i] }
        for v in landmarkCount..<(start + cheekCount) {
            let (fx, fy) = residualField(model, v < start ? field : cheekField, model[v])
            out.append(CallFacePoint(projected[v].x + fx, projected[v].y + fy))
        }
        return CallFaceLiveHead(points: out + mouthExtras(out), pose: pose, jawTrust: settled)
    }

    public static func vertexDepths(_ model: [CallFacePoint], _ pose: CallFacePose) -> [Double] {
        let r3 = pose.rows[2]
        var depths = model.map { dot3(r3, [$0.x, $0.y, $0.depth]) }
        var jawDepth = Double.infinity
        for i in jaw { jawDepth = CallFaceJS.min(jawDepth, depths[i]) }
        var v = ringStart
        while v < depths.count - extraCount {
            depths[v] = CallFaceJS.min(depths[v], jawDepth - ringGap)
            v += 1
        }
        return depths
    }

    public static func smoothstepValue(_ low: Double, _ high: Double, _ value: Double) -> Double {
        let t = clampValue((value - low) / (high - low), 0, 1)
        return t * t * (3 - 2 * t)
    }

    static func dot3(_ a: [Double], _ b: [Double]) -> Double {
        a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    }

    static func normalize3(_ v: [Double]) -> [Double] {
        let length = CallFaceJS.or(CallFaceJS.hypot(v[0], v[1], v[2]), 1)
        return [v[0] / length, v[1] / length, v[2] / length]
    }

    static func invert3(_ m: [[Double]]) -> [[Double]] {
        let a = m[0][0], b = m[0][1], c = m[0][2]
        let d = m[1][0], e = m[1][1], f = m[1][2]
        let g = m[2][0], h = m[2][1], i = m[2][2]
        let A = e * i - f * h
        let B = -(d * i - f * g)
        let C = d * h - e * g
        let det = CallFaceJS.or(a * A + b * B + c * C, 1e-9)
        return [
            [A / det, -(b * i - c * h) / det, (b * f - c * e) / det],
            [B / det, (a * i - c * g) / det, -(a * f - c * d) / det],
            [C / det, -(a * h - b * g) / det, (a * e - b * d) / det],
        ]
    }

    static let neededRegions = ["faceContour", "leftEyebrow", "rightEyebrow", "leftEye", "rightEye", "outerLips", "innerLips"]

    public static func landmarksFromRegions(_ regions: CallFaceRegions?) -> [CallFacePoint]? {
        guard let regions else { return nil }
        if neededRegions.contains(where: { (regions[$0]?.count ?? 0) < 3 }) { return nil }
        let crest = (regions["noseCrest"]?.count ?? 0) >= 2 ? regions["noseCrest"] : regions["medianLine"]
        guard let crest, crest.count >= 2, let noseRegion = regions["nose"], noseRegion.count >= 3 else { return nil }
        let all = neededRegions.map { regions[$0]! } + [crest, noseRegion]
        let dims = all.allSatisfy { region in region.allSatisfy { $0.count > 2 && $0[2].isFinite } } ? 3 : 2
        func copy(_ point: [Double]) -> [Double] { Array(point.prefix(dims)) }
        var out = [[Double]](repeating: [], count: landmarkCount)
        var jawLine = regions["faceContour"]!.map(copy)
        if jawLine[0][0] > jawLine[jawLine.count - 1][0] { jawLine.reverse() }
        for (i, point) in resample(jawLine, 17).enumerated() { out[i] = point }
        let brows = CallFaceJS.sorted([regions["leftEyebrow"]!, regions["rightEyebrow"]!]) { meanX($0) - meanX($1) }
        for (i, point) in browLine(brows[0].map(copy)).enumerated() { out[17 + i] = point }
        for (i, point) in browLine(brows[1].map(copy)).enumerated() { out[22 + i] = point }
        let ridge = CallFaceJS.sorted(crest.map(copy)) { $0[1] - $1[1] }
        for (i, point) in resample(ridge, 4).enumerated() { out[27 + i] = point }
        let noseCopy = noseRegion.map(copy)
        let middle = noseCopy.reduce(0.0) { $0 + $1[1] } / Double(noseCopy.count)
        let base = CallFaceJS.sorted(noseCopy.filter { $0[1] >= middle }) { $0[0] - $1[0] }
        let noseBase = base.count >= 2 ? base : CallFaceJS.sorted(noseCopy, by: { $0[0] - $1[0] })
        for (i, point) in resample(noseBase, 5).enumerated() { out[31 + i] = point }
        let eyes = CallFaceJS.sorted([regions["leftEye"]!.map(copy), regions["rightEye"]!.map(copy)]) { meanX($0) - meanX($1) }
        for (side, loop) in eyes.enumerated() {
            let (upper, lower) = splitLoop(loop, 4, 4)
            let at = side == 1 ? 42 : 36
            out[at] = upper[0]
            out[at + 1] = upper[1]
            out[at + 2] = upper[2]
            out[at + 3] = upper[3]
            out[at + 4] = lower[2]
            out[at + 5] = lower[1]
        }
        let (lipTop, lipBottom) = splitLoop(regions["outerLips"]!.map(copy), 7, 7)
        for i in 0..<7 { out[48 + i] = lipTop[i] }
        for i in 1..<6 { out[60 - i] = lipBottom[i] }
        let (innerTop, innerBottom) = splitLoop(regions["innerLips"]!.map(copy), 5, 5)
        for i in 0..<5 { out[60 + i] = innerTop[i] }
        for i in 1..<4 { out[68 - i] = innerBottom[i] }
        return out.map(CallFacePoint.init)
    }

    public static func shapeRegions(_ points: [CallFacePoint]) -> CallFaceRegions {
        func pick(_ indices: [Int]) -> [[Double]] { indices.map { [points[$0].x, points[$0].y] } }
        return [
            "faceContour": pick(jaw),
            "leftEyebrow": pick([17, 18, 19, 20, 21]),
            "rightEyebrow": pick([22, 23, 24, 25, 26]),
            "noseCrest": pick([27, 28, 29, 30]),
            "nose": pick([31, 32, 33, 34, 35]),
            "leftEye": pick(leftEye),
            "rightEye": pick(rightEye),
            "outerLips": pick(outerLips),
            "innerLips": pick(innerLips),
        ]
    }

    public static func canonicalShape(_ points: [CallFacePoint]) -> [CallFacePoint] {
        guard var shaped = landmarksFromRegions(shapeRegions(points)) else { return points }
        for i in 27...35 { shaped[i] = points[i].flat }
        return shaped
    }

    static func meanX(_ points: [[Double]]) -> Double {
        points.reduce(0.0) { $0 + $1[0] } / Double(points.count)
    }

    static func browLine(_ points: [[Double]]) -> [[Double]] {
        let sorted = CallFaceJS.sorted(points) { $0[0] - $1[0] }
        let low = sorted[0][0]
        let high = sorted[sorted.count - 1][0]
        var out: [[Double]] = []
        for i in 0..<5 {
            let x = low + ((high - low) * Double(i)) / 4
            let near = CallFaceJS.sorted(sorted.map { (abs($0[0] - x), $0) }) { $0.0 - $1.0 }
                .prefix(Swift.min(2, sorted.count))
                .map(\.1)
            var point = [x, near.reduce(0.0) { $0 + $1[1] } / Double(near.count)]
            if near.allSatisfy({ $0.count > 2 && $0[2].isFinite }) { point.append(near.reduce(0.0) { $0 + $1[2] } / Double(near.count)) }
            out.append(point)
        }
        return out
    }

    static func splitLoop(_ loop: [[Double]], _ upperCount: Int, _ lowerCount: Int) -> ([[Double]], [[Double]]) {
        let points = loop
        var left = 0
        var right = 0
        for (i, point) in points.enumerated() {
            if point[0] < points[left][0] { left = i }
            if point[0] > points[right][0] { right = i }
        }
        func walk(_ from: Int, _ to: Int) -> [[Double]] {
            var path = [points[from]]
            var i = from
            while i != to {
                i = (i + 1) % points.count
                path.append(points[i])
            }
            return path
        }
        let one = walk(left, right)
        let two = Array(walk(right, left).reversed())
        func meanY(_ path: [[Double]]) -> Double { path.reduce(0.0) { $0 + $1[1] } / Double(path.count) }
        let (upper, lower) = meanY(one) <= meanY(two) ? (one, two) : (two, one)
        return (resample(upper, upperCount), resample(lower, lowerCount))
    }

    static func resample(_ points: [[Double]], _ count: Int) -> [[Double]] {
        if points.count == 1 { return [[Double]](repeating: points[0], count: count) }
        var lengths = [0.0]
        for i in 1..<points.count {
            lengths.append(lengths[i - 1] + CallFaceJS.hypot(points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1]))
        }
        let total = CallFaceJS.or(lengths[lengths.count - 1], 1)
        var out: [[Double]] = []
        var segment = 1
        for i in 0..<count {
            let at = (total * Double(i)) / Double(count - 1)
            while segment < points.count - 1 && lengths[segment] < at { segment += 1 }
            let span = CallFaceJS.or(lengths[segment] - lengths[segment - 1], 1)
            let t = clampValue((at - lengths[segment - 1]) / span, 0, 1)
            let a = points[segment - 1]
            let b = points[segment]
            out.append(a.enumerated().map { k, value in value + ((k < b.count ? b[k] : value) - value) * t })
        }
        return out
    }

    public static func resamplePolyline(_ points: [CallFacePoint], _ count: Int) -> [CallFacePoint] {
        resample(points.map(\.values), count).map(CallFacePoint.init)
    }

    public static func clampValue(_ value: Double, _ low: Double, _ high: Double) -> Double {
        CallFaceJS.min(high, CallFaceJS.max(low, value))
    }
}
