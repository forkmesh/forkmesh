import Foundation

public enum CallDepthOrientation: Int, Sendable, CaseIterable {
    case up = 1
    case upMirrored = 2
    case down = 3
    case downMirrored = 4
    case leftMirrored = 5
    case right = 6
    case rightMirrored = 7
    case left = 8

    public var swapsAxes: Bool { rawValue >= 5 }

    public func uprightSize(width: Int, height: Int) -> (width: Int, height: Int) {
        swapsAxes ? (height, width) : (width, height)
    }

    public func sensor(u: Double, v: Double) -> (x: Double, y: Double) {
        switch self {
        case .up: return (u, v)
        case .upMirrored: return (1 - u, v)
        case .down: return (1 - u, 1 - v)
        case .downMirrored: return (u, 1 - v)
        case .leftMirrored: return (v, u)
        case .right: return (v, 1 - u)
        case .rightMirrored: return (1 - v, 1 - u)
        case .left: return (1 - v, u)
        }
    }
}

public struct CallDepthField: Sendable {
    public let width: Int
    public let height: Int
    public let values: [Float]
    public let orientation: CallDepthOrientation

    public init?(width: Int, height: Int, values: [Float], orientation: CallDepthOrientation) {
        guard width > 0, height > 0, values.count >= width * height else { return nil }
        self.width = width
        self.height = height
        self.values = values
        self.orientation = orientation
    }

    public func metres(u: Double, v: Double, radius: Int = CallFaceDepth.radius) -> Double? {
        guard u.isFinite, v.isFinite, (0...1).contains(u), (0...1).contains(v) else { return nil }
        let point = orientation.sensor(u: u, v: v)
        let cx = min(width - 1, max(0, Int(point.x * Double(width))))
        let cy = min(height - 1, max(0, Int(point.y * Double(height))))
        var found: [Double] = []
        for dy in -radius...radius {
            for dx in -radius...radius {
                let x = cx + dx
                let y = cy + dy
                guard x >= 0, y >= 0, x < width, y < height else { continue }
                let value = Double(values[y * width + x])
                if CallFaceDepth.usable(value) { found.append(value) }
            }
        }
        return CallFaceDepth.median(found)
    }
}

public enum CallFaceDepth {
    public static let nearest = 0.1
    public static let farthest = 5.0
    public static let faceReach = 0.15
    public static let minimumShare = 0.5
    public static let radius = 1

    public static func usable(_ metres: Double) -> Bool {
        metres.isFinite && metres > nearest && metres < farthest
    }

    public static func median(_ values: [Double]) -> Double? {
        guard !values.isEmpty else { return nil }
        let sorted = values.sorted()
        let middle = sorted.count / 2
        return sorted.count % 2 == 1 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2
    }

    public static func focalPixels(fx: Double, referenceWidth: Double, sensorWidth: Double) -> Double? {
        guard fx > 0, referenceWidth > 0, sensorWidth > 0 else { return nil }
        return fx * sensorWidth / referenceWidth
    }

    public static func focalPixels(fieldOfViewDegrees: Double, sensorWidth: Double) -> Double? {
        guard fieldOfViewDegrees > 0, fieldOfViewDegrees < 180, sensorWidth > 0 else { return nil }
        return sensorWidth / 2 / tan(fieldOfViewDegrees * .pi / 360)
    }

    public static func z(depth: Double, faceDepth: Double, focal: Double) -> Double {
        guard faceDepth > 0 else { return 0 }
        return (faceDepth - depth) * focal / faceDepth
    }

    public static func relief(
        _ regions: [String: [[Double]]], field: CallDepthField, width: Double, height: Double, focal: Double
    ) -> [String: [[Double]]]? {
        guard width > 0, height > 0, focal > 0, focal.isFinite else { return nil }
        var sampled: [String: [Double?]] = [:]
        var total = 0
        for (name, points) in regions {
            sampled[name] = points.map { point in
                guard point.count >= 2 else { return nil }
                return field.metres(u: point[0] / width, v: point[1] / height)
            }
            total += points.count
        }
        guard total > 0, let rough = median(sampled.values.flatMap { $0.compactMap { $0 } }) else { return nil }
        var kept = 0
        for (name, depths) in sampled {
            let trimmed = depths.map { depth in depth.flatMap { abs($0 - rough) <= faceReach ? $0 : nil } }
            kept += trimmed.compactMap { $0 }.count
            sampled[name] = trimmed
        }
        guard Double(kept) >= Double(total) * minimumShare,
            let face = median(sampled.values.flatMap { $0.compactMap { $0 } })
        else { return nil }
        var out: [String: [[Double]]] = [:]
        for (name, points) in regions {
            let filled = fill(sampled[name] ?? [], fallback: face)
            out[name] = zip(points, filled).map { point, depth in
                let x = point.count > 0 ? point[0] : 0
                let y = point.count > 1 ? point[1] : 0
                return [x, y, (z(depth: depth, faceDepth: face, focal: focal) * 100).rounded() / 100]
            }
        }
        return out
    }

    static func fill(_ depths: [Double?], fallback: Double) -> [Double] {
        depths.indices.map { index in
            if let depth = depths[index] { return depth }
            for step in 1..<max(2, depths.count) {
                if index - step >= 0, let depth = depths[index - step] { return depth }
                if index + step < depths.count, let depth = depths[index + step] { return depth }
            }
            return fallback
        }
    }
}
