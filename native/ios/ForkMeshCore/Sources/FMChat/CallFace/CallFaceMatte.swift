import Foundation

public struct CallFaceSize: Equatable, Sendable {
    public let width: Int
    public let height: Int

    public init(_ width: Int, _ height: Int) {
        self.width = width
        self.height = height
    }
}

public enum CallFaceMatte {
    public static let regionWidth = 64
    public static let growRounds = 8
    public static let boxRadius = 4
    public static let eps = 0.0012
    public static let hold = 0.55
    public static let guideWidth = 320
    public static let minimumGuide = 96
    public static let hairWidth = 256
    public static let hairWarmRounds = 3
    public static let hairRate = 0.5
    public static let seedSpan = 0.8
    public static let depthNear = 0.35
    public static let depthFar = 0.7
    public static let depthAgree = 0.6
    public static let depthStaleMs = 150.0
    public static let hairDepthNear = 0.25
    public static let hairDepthFar = 0.55

    static func reduced(_ limit: Int, width: Int, height: Int, minimum: Int) -> CallFaceSize {
        let w = max(1, width)
        let small = min(limit, w)
        return CallFaceSize(small, max(minimum, Int(CallFaceJS.round(Double(small * max(1, height)) / Double(w)))))
    }

    static func turned(_ size: CallFaceSize, _ map: CallFaceFrameMap) -> CallFaceSize {
        map.sideways ? CallFaceSize(size.height, size.width) : size
    }

    public static func regionSize(rawWidth: Int, rawHeight: Int, map: CallFaceFrameMap) -> CallFaceSize {
        let upright = turned(CallFaceSize(rawWidth, rawHeight), map)
        let region = CallFaceSize(regionWidth, max(4, Int(CallFaceJS.round(Double(regionWidth * upright.height) / Double(max(1, upright.width))))))
        return turned(region, map)
    }

    public static func guideSize(_ guide: Int, map: CallFaceFrameMap) -> CallFaceSize {
        turned(reduced(guide, width: map.uprightWidth, height: map.uprightHeight, minimum: 2), map)
    }

    public static func hairSize(map: CallFaceFrameMap) -> CallFaceSize {
        reduced(hairWidth, width: map.uprightWidth, height: map.uprightHeight, minimum: 2)
    }

    public static func faceSeed(_ points: [CallFacePoint]?, width: Double, height: Double) -> [Double] {
        guard let points, points.count >= CallFaceGeometry.landmarkCount, width > 0, height > 0 else { return [0, 0, -1] }
        let frame = CallFaceGeometry.faceFrame(points)
        return [frame.mid.x / width, frame.mid.y / height, seedSpan * frame.span / height]
    }

    public static func compose(_ outer: [Double], _ inner: [Double]) -> [Double] {
        [
            outer[0] * inner[0] + outer[2] * inner[1],
            outer[1] * inner[0] + outer[3] * inner[1],
            outer[0] * inner[2] + outer[2] * inner[3],
            outer[1] * inner[2] + outer[3] * inner[3],
            outer[0] * inner[4] + outer[2] * inner[5] + outer[4],
            outer[1] * inner[4] + outer[3] * inner[5] + outer[5],
        ]
    }

    public static func uprightToBufferUv(_ map: CallFaceFrameMap) -> [Double] {
        let w = Double(max(1, map.bufferWidth))
        let h = Double(max(1, map.bufferHeight))
        return compose([1 / w, 0, 0, 1 / h, 0, 0], map.toBufferMatrix)
    }

    public static func depthUv(imageWidth: Int, imageHeight: Int, depthWidth: Int, depthHeight: Int) -> [Double] {
        let image = Double(max(1, imageWidth)) / Double(max(1, imageHeight))
        let depth = Double(max(1, depthWidth)) / Double(max(1, depthHeight))
        if abs(image - depth) < 0.01 { return [1, 0, 0, 1, 0, 0] }
        if depth > image {
            let span = image / depth
            return [span, 0, 0, 1, (1 - span) / 2, 0]
        }
        let span = depth / image
        return [1, 0, 0, span, 0, (1 - span) / 2]
    }

    public static func blurBackdrop(_ strength: Double) -> (sigma: Double, passes: Int) {
        CallFaceDrawPlan.haloSigma(strength)
    }

    public static func blurStrength(_ background: String) -> Double? {
        switch background {
        case "blur": return 0.4
        case "blur-strong": return 1
        default: return nil
        }
    }
}

public extension CallFaceFrameMap {
    var uprightSize: CallFaceSize { CallFaceSize(uprightWidth, uprightHeight) }

    var toUprightUvMatrix: [Double] {
        let w = Double(max(1, bufferWidth))
        let h = Double(max(1, bufferHeight))
        let uw = Double(max(1, uprightWidth))
        let uh = Double(max(1, uprightHeight))
        return Self.matrix { uv in
            let p = toUpright(CallFacePoint(uv.x * w, uv.y * h))
            return CallFacePoint(p.x / uw, p.y / uh)
        }
    }
}
