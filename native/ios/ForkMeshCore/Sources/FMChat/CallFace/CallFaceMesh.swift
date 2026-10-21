import Foundation

public struct CallFaceDetection: Equatable, Sendable {
    public let score: Double
    public let cx: Double
    public let cy: Double
    public let w: Double
    public let h: Double
    public let keypoints: [CallFacePoint]
}

public struct CallFaceLetterbox: Equatable, Sendable {
    public let scale: Double
    public let x: Double
    public let y: Double

    public init(scale: Double, x: Double, y: Double) {
        self.scale = scale
        self.x = x
        self.y = y
    }

    public init(width: Double, height: Double) {
        let side = CallFaceJS.max(width, height)
        self.init(scale: side, x: (side - width) / 2 / side, y: (side - height) / 2 / side)
    }
}

public struct CallFaceRoi: Equatable, Sendable {
    public let cx: Double
    public let cy: Double
    public let size: Double
    public let angle: Double

    public init(cx: Double, cy: Double, size: Double, angle: Double) {
        self.cx = cx
        self.cy = cy
        self.size = size
        self.angle = angle
    }
}

public enum CallFaceMesh {
    public static let detectSize = 128.0
    public static let meshSize = 192.0
    public static let detectScore = 0.5
    public static let faceScore = 0.5
    public static let roiScale = 1.5

    public static let regionNames = ["faceContour", "rightEyebrow", "leftEyebrow", "rightEye", "leftEye", "noseCrest", "nose", "outerLips", "innerLips"]
    public static let regions: [String: [Int]] = [
        "faceContour": [127, 234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454, 356],
        "rightEyebrow": [70, 63, 105, 66, 107],
        "leftEyebrow": [336, 296, 334, 293, 300],
        "rightEye": [33, 246, 161, 160, 159, 158, 157, 173, 133, 155, 154, 153, 145, 144, 163, 7],
        "leftEye": [263, 466, 388, 387, 386, 385, 384, 398, 362, 382, 381, 380, 374, 373, 390, 249],
        "noseCrest": [168, 6, 197, 195, 5, 4],
        "nose": [6, 197, 195, 5, 64, 98, 97, 2, 326, 327, 294],
        "outerLips": [61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291, 375, 321, 405, 314, 17, 84, 181, 91, 146],
        "innerLips": [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308, 324, 318, 402, 317, 14, 87, 178, 88, 95],
    ]

    public static func blazeAnchors() -> [CallFacePoint] {
        var anchors: [CallFacePoint] = []
        for (grid, count) in [(16, 2), (8, 6)] {
            for y in 0..<grid {
                for x in 0..<grid {
                    for _ in 0..<count { anchors.append(CallFacePoint((Double(x) + 0.5) / Double(grid), (Double(y) + 0.5) / Double(grid))) }
                }
            }
        }
        return anchors
    }

    public static func sigmoid(_ logit: Double, limit: Double) -> Double {
        1 / (1 + exp(-CallFaceJS.max(-limit, CallFaceJS.min(limit, logit))))
    }

    public static func decodeDetections(scores: [Float], boxes: [Float], anchors: [CallFacePoint]) -> CallFaceDetection? {
        var best: CallFaceDetection?
        var found: [CallFaceDetection] = []
        for i in 0..<anchors.count {
            let score = sigmoid(Double(scores[i]), limit: 100)
            if score < detectScore { continue }
            let r = (0..<16).map { Double(boxes[i * 16 + $0]) }
            let ax = anchors[i].x
            let ay = anchors[i].y
            let box = CallFaceDetection(
                score: score,
                cx: r[0] / detectSize + ax,
                cy: r[1] / detectSize + ay,
                w: r[2] / detectSize,
                h: r[3] / detectSize,
                keypoints: (0..<6).map { k in CallFacePoint(r[4 + k * 2] / detectSize + ax, r[5 + k * 2] / detectSize + ay) }
            )
            found.append(box)
            if best == nil || score > best!.score { best = box }
        }
        guard let best else { return nil }
        func overlap(_ a: CallFaceDetection, _ b: CallFaceDetection) -> Double {
            let x0 = CallFaceJS.max(a.cx - a.w / 2, b.cx - b.w / 2)
            let x1 = CallFaceJS.min(a.cx + a.w / 2, b.cx + b.w / 2)
            let y0 = CallFaceJS.max(a.cy - a.h / 2, b.cy - b.h / 2)
            let y1 = CallFaceJS.min(a.cy + a.h / 2, b.cy + b.h / 2)
            let inter = CallFaceJS.max(0, x1 - x0) * CallFaceJS.max(0, y1 - y0)
            return inter / CallFaceJS.or(a.w * a.h + b.w * b.h - inter, 1)
        }
        let group = found.filter { overlap($0, best) > 0.3 }
        let total = group.reduce(0.0) { $0 + $1.score }
        func mean(_ pick: (CallFaceDetection) -> Double) -> Double {
            group.reduce(0.0) { $0 + pick($1) * $1.score } / total
        }
        return CallFaceDetection(
            score: best.score,
            cx: mean(\.cx),
            cy: mean(\.cy),
            w: mean(\.w),
            h: mean(\.h),
            keypoints: best.keypoints.indices.map { k in CallFacePoint(mean { $0.keypoints[k].x }, mean { $0.keypoints[k].y }) }
        )
    }

    public static func roiFromDetection(_ detection: CallFaceDetection, _ letterbox: CallFaceLetterbox) -> CallFaceRoi {
        func toFrame(_ p: CallFacePoint) -> CallFacePoint {
            CallFacePoint((p.x - letterbox.x) * letterbox.scale, (p.y - letterbox.y) * letterbox.scale)
        }
        let center = toFrame(CallFacePoint(detection.cx, detection.cy))
        let right = toFrame(detection.keypoints[0])
        let left = toFrame(detection.keypoints[1])
        let angle = atan2(left.y - right.y, left.x - right.x)
        let size = CallFaceJS.max(detection.w, detection.h) * letterbox.scale * roiScale
        return CallFaceRoi(cx: center.x, cy: center.y, size: size, angle: angle)
    }

    public static func roiFromMesh(_ points: [CallFacePoint]) -> CallFaceRoi {
        let a = points[33]
        let b = points[263]
        let angle = atan2(b.y - a.y, b.x - a.x)
        let c = cos(-angle)
        let s = sin(-angle)
        var minX = Double.infinity
        var minY = Double.infinity
        var maxX = -Double.infinity
        var maxY = -Double.infinity
        let ox = points[1].x
        let oy = points[1].y
        for point in points {
            let rx = (point.x - ox) * c - (point.y - oy) * s
            let ry = (point.x - ox) * s + (point.y - oy) * c
            minX = CallFaceJS.min(minX, rx)
            maxX = CallFaceJS.max(maxX, rx)
            minY = CallFaceJS.min(minY, ry)
            maxY = CallFaceJS.max(maxY, ry)
        }
        let mx = (minX + maxX) / 2
        let my = (minY + maxY) / 2
        let ci = cos(angle)
        let si = sin(angle)
        return CallFaceRoi(
            cx: ox + mx * ci - my * si,
            cy: oy + mx * si + my * ci,
            size: CallFaceJS.max(maxX - minX, maxY - minY) * roiScale,
            angle: angle
        )
    }

    public static func meshPoints(raw: [Float], roi: CallFaceRoi) -> [CallFacePoint] {
        let c = cos(roi.angle)
        let s = sin(roi.angle)
        var points: [CallFacePoint] = []
        var i = 0
        while i + 2 < raw.count {
            let x = (Double(raw[i]) / meshSize - 0.5) * roi.size
            let y = (Double(raw[i + 1]) / meshSize - 0.5) * roi.size
            points.append(CallFacePoint(roi.cx + x * c - y * s, roi.cy + x * s + y * c, (Double(raw[i + 2]) / meshSize) * roi.size))
            i += 3
        }
        return points
    }

    public static func meshRegions(_ points: [CallFacePoint]) -> CallFaceRegions {
        var out: CallFaceRegions = [:]
        for name in regionNames {
            out[name] = regions[name]!.map { i in
                let p = points[i]
                if let z = p.z, z.isFinite { return [p.x, p.y, -z] }
                return [p.x, p.y]
            }
        }
        return out
    }

    public static func meshToLandmarks(_ points: [CallFacePoint]) -> [CallFacePoint]? {
        CallFaceGeometry.landmarksFromRegions(meshRegions(points))
    }
}
