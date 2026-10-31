import Foundation

public final class CallFaceOneEuroFilter {
    public let minCutoff: Double
    public let beta: Double
    public let derivativeCutoff: Double
    public private(set) var value: Double?
    public private(set) var slope = 0.0
    public private(set) var time = 0.0

    public init(minCutoff: Double = 1.2, beta: Double = 0.02, derivativeCutoff: Double = 1) {
        self.minCutoff = minCutoff
        self.beta = beta
        self.derivativeCutoff = derivativeCutoff
    }

    public func reset() {
        value = nil
        slope = 0
    }

    public func filter(_ input: Double, time now: Double, scale: Double = 1) -> Double {
        guard let current = value, now > time else {
            value = input
            time = now
            return input
        }
        let elapsed = (now - time) / 1000
        time = now
        func smoothing(_ cutoff: Double) -> Double {
            let tau = 1 / (2 * Double.pi * cutoff)
            return 1 / (1 + tau / elapsed)
        }
        let rate = (input - current) / elapsed
        slope += smoothing(derivativeCutoff) * (rate - slope)
        let cutoff = minCutoff + (beta * abs(slope)) / scale
        let next = current + smoothing(cutoff) * (input - current)
        value = next
        return next
    }
}

public final class CallFacePointSmoother {
    private let filters: [CallFaceOneEuroFilter]

    public init(count: Int, minCutoff: Double = 1.2, beta: Double = 0.6) {
        filters = (0..<(count * 3)).map { _ in CallFaceOneEuroFilter(minCutoff: minCutoff, beta: beta) }
    }

    public func reset() {
        for filter in filters { filter.reset() }
    }

    public func smooth(_ points: [CallFacePoint], time: Double, scale: Double = 1) -> [CallFacePoint] {
        points.enumerated().map { i, point in
            let values = point.values.enumerated().map { k, value in
                i * 3 + k < filters.count ? filters[i * 3 + k].filter(value, time: time, scale: scale) : value
            }
            return CallFacePoint(values)
        }
    }
}

public final class CallFaceAnchorSmoother {
    private let eye = (0..<4).map { _ in CallFaceOneEuroFilter(minCutoff: 1.5, beta: 1.5) }
    private let mouth = (0..<2).map { _ in CallFaceOneEuroFilter(minCutoff: 1.5, beta: 1.5) }
    private let open = CallFaceOneEuroFilter(minCutoff: 3, beta: 0.5)
    private let yawFilter = CallFaceOneEuroFilter(minCutoff: 0.6, beta: 0.3)
    private var yaw = 0.0

    public init() {}

    public func reset() {
        for filter in eye + mouth + [open, yawFilter] { filter.reset() }
        yaw = 0
    }

    public func smooth(_ anchors: CallFaceAnchors, time: Double) -> CallFaceAnchors {
        let span = CallFaceJS.or(CallFaceJS.hypot(anchors.eyeR.x - anchors.eyeL.x, anchors.eyeR.y - anchors.eyeL.y), 1)
        if let value = anchors.yaw { yaw = yawFilter.filter(value, time: time) }
        let eyeL = CallFacePoint(eye[0].filter(anchors.eyeL.x, time: time, scale: span), eye[1].filter(anchors.eyeL.y, time: time, scale: span))
        let eyeR = CallFacePoint(eye[2].filter(anchors.eyeR.x, time: time, scale: span), eye[3].filter(anchors.eyeR.y, time: time, scale: span))
        let lips = anchors.mouth.map { CallFacePoint(mouth[0].filter($0.x, time: time, scale: span), mouth[1].filter($0.y, time: time, scale: span)) }
        let opened = open.filter(CallFaceJS.truthy(anchors.open) ? anchors.open! : 0, time: time)
        return CallFaceAnchors(eyeL: eyeL, eyeR: eyeR, mouth: lips, open: CallFaceGeometry.clampValue(opened, 0, 1), yaw: yaw)
    }
}

public final class CallFaceGroupFilter {
    public let minCutoff: Double
    public let beta: Double
    public let derivativeCutoff: Double
    public private(set) var values: [Double]?
    public private(set) var speed = 0.0
    public private(set) var time = 0.0

    public init(minCutoff: Double = 1.2, beta: Double = 0.6, derivativeCutoff: Double = 1) {
        self.minCutoff = minCutoff
        self.beta = beta
        self.derivativeCutoff = derivativeCutoff
    }

    public func reset() {
        values = nil
        speed = 0
    }

    public func filter(_ input: [Double], time now: Double, scale: Double = 1) -> [Double] {
        guard var current = values, current.count == input.count, now > time else {
            values = input
            time = now
            return input
        }
        let elapsed = (now - time) / 1000
        time = now
        func smoothing(_ cutoff: Double) -> Double {
            let tau = 1 / (2 * Double.pi * cutoff)
            return 1 / (1 + tau / elapsed)
        }
        var fastest = 0.0
        for i in input.indices { fastest = max(fastest, abs(input[i] - current[i]) / elapsed) }
        speed += smoothing(derivativeCutoff) * (fastest / CallFaceJS.or(scale, 1) - speed)
        let k = smoothing(minCutoff + beta * speed)
        for i in input.indices { current[i] += k * (input[i] - current[i]) }
        values = current
        return current
    }
}

public struct CallFaceSmoothGroup: Sendable {
    public let points: [Int]
    public let minCutoff: Double
    public let beta: Double
    public let derivativeCutoff: Double
}

public final class CallFaceLandmarkSmoother {
    public static let head = (minCutoff: 3.0, beta: 5.0, derivativeCutoff: 4.0)
    public static let groups = [
        CallFaceSmoothGroup(points: CallFaceGeometry.jaw, minCutoff: 1.2, beta: 2, derivativeCutoff: 2),
        CallFaceSmoothGroup(points: Array(17..<27), minCutoff: 2, beta: 3, derivativeCutoff: 3),
        CallFaceSmoothGroup(points: Array(27..<36), minCutoff: 1.5, beta: 2, derivativeCutoff: 2),
        CallFaceSmoothGroup(points: Array(36..<48), minCutoff: 2.5, beta: 4, derivativeCutoff: 3),
        CallFaceSmoothGroup(points: Array(48..<68), minCutoff: 2.5, beta: 6, derivativeCutoff: 4),
    ]

    private let eyes: CallFaceGroupFilter
    private let filters: [(points: [Int], filter: CallFaceGroupFilter)]

    public init() {
        let head = CallFaceLandmarkSmoother.head
        eyes = CallFaceGroupFilter(minCutoff: head.minCutoff, beta: head.beta, derivativeCutoff: head.derivativeCutoff)
        filters = CallFaceLandmarkSmoother.groups.map { group in
            (group.points, CallFaceGroupFilter(minCutoff: group.minCutoff, beta: group.beta, derivativeCutoff: group.derivativeCutoff))
        }
    }

    public func reset() {
        eyes.reset()
        for entry in filters { entry.filter.reset() }
    }

    public func smooth(_ points: [CallFacePoint], time: Double) -> [CallFacePoint] {
        guard points.count >= CallFaceGeometry.landmarkCount else { return points }
        let raw = CallFaceGeometry.faceFrame(points)
        let eye = eyes.filter([raw.left.x, raw.left.y, raw.right.x, raw.right.y], time: time, scale: raw.span)
        let steady = CallFaceGeometry.faceFrame(left: CallFacePoint(eye[0], eye[1]), right: CallFacePoint(eye[2], eye[3]))
        var out = points
        for (indices, filter) in filters {
            var flat: [Double] = []
            for i in indices {
                let local = CallFaceGeometry.toFrame(raw, points[i])
                flat.append(local.x / raw.span)
                flat.append(local.y / raw.span)
                if let z = points[i].z { flat.append(z / raw.span) }
            }
            let smooth = filter.filter(flat, time: time)
            var at = 0
            for i in indices {
                var point = CallFaceGeometry.fromFrame(steady, smooth[at] * steady.span, smooth[at + 1] * steady.span)
                at += 2
                if points[i].z != nil {
                    point.z = smooth[at] * steady.span
                    at += 1
                }
                out[i] = point
            }
        }
        return out
    }
}
