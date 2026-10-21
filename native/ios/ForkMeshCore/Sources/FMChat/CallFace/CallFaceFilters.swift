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
