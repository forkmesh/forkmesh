import Foundation

public struct CallFaceCustomTransform: Equatable, Sendable {
    public let scale: Double
    public let matrix: [Double]
}

public struct CallFaceCustomPolygons: Equatable, Sendable {
    public let face: [CallFacePoint]
    public let inner: [CallFacePoint]
    public let neck: [CallFacePoint]
}

public struct CallFaceSkinModel: Equatable, Sendable {
    public let mean: [Double]
    public let inverse: [Double]
}

public struct CallFaceCustomMatte: Equatable, Sendable {
    public let matte: [Float]
    public let skin: [Float]
    public let hair: [Float]
    public let head: [Double]
}

public enum CallFaceCustomError: Error, Equatable {
    case faceNotFound
    case faceTooSmall
}

public enum CallFaceCustom {
    public static let id = "custom"
    public static let label = "Your photo"
    public static let storageKey = "forkmesh.chat.callFaceImage"
    public static let side = 1024.0
    public static let size = 512
    static let eyeSpan = 118.0
    static let eyeMid = (256.0, 285.0)
    static let ring = 28

    public static func transform(_ points: [CallFacePoint]) -> CallFaceCustomTransform {
        let frame = CallFaceGeometry.faceFrame(points)
        let scale = eyeSpan / frame.span
        let c = cos(-frame.roll) * scale
        let s = sin(-frame.roll) * scale
        return CallFaceCustomTransform(
            scale: scale,
            matrix: [c, s, -s, c, eyeMid.0 - (c * frame.mid.x - s * frame.mid.y), eyeMid.1 - (s * frame.mid.x + c * frame.mid.y)]
        )
    }

    public static func apply(_ matrix: [Double], _ point: CallFacePoint) -> CallFacePoint {
        CallFacePoint(matrix[0] * point.x + matrix[2] * point.y + matrix[4], matrix[1] * point.x + matrix[3] * point.y + matrix[5])
    }

    public static func smoothstep(_ edge0: Double, _ edge1: Double, _ x: Double) -> Double {
        let t = CallFaceGeometry.clampValue((x - edge0) / (edge1 - edge0), 0, 1)
        return t * t * (3 - 2 * t)
    }

    public static func boxBlur(_ values: [Float], size: Int, radius: Int, passes: Int = 1) -> [Float] {
        var source = values
        let divisor = Double(2 * radius + 1)
        for _ in 0..<passes {
            var temp = [Float](repeating: 0, count: values.count)
            var out = [Float](repeating: 0, count: values.count)
            for y in 0..<size {
                var sum = 0.0
                let row = y * size
                for x in -radius...radius { sum += Double(source[row + Swift.min(Swift.max(x, 0), size - 1)]) }
                for x in 0..<size {
                    temp[row + x] = Float(sum / divisor)
                    sum += Double(source[row + Swift.min(size - 1, x + radius + 1)]) - Double(source[row + Swift.max(0, x - radius)])
                }
            }
            for x in 0..<size {
                var sum = 0.0
                for y in -radius...radius { sum += Double(temp[Swift.min(Swift.max(y, 0), size - 1) * size + x]) }
                for y in 0..<size {
                    out[y * size + x] = Float(sum / divisor)
                    sum += Double(temp[Swift.min(size - 1, y + radius + 1) * size + x]) - Double(temp[Swift.max(0, y - radius) * size + x])
                }
            }
            source = out
        }
        return source
    }

    public static func headRing(_ matte: [Float], size: Int, mid: CallFacePoint, span: Double) -> [Double] {
        let center = CallFacePoint(mid.x, mid.y - 0.35 * span)
        var reaches: [Int] = []
        var directions: [CallFacePoint] = []
        let limit = Double(size - 1)
        for k in 0..<ring {
            let angle = (2 * Double.pi * Double(k)) / Double(ring) - Double.pi / 2
            let direction = CallFacePoint(cos(angle), sin(angle))
            var reach = 0
            var r = 0
            while true {
                let x = center.x + direction.x * Double(r)
                let y = center.y + direction.y * Double(r)
                if !(x >= 0 && x < limit && y >= 0 && y < limit) { break }
                if Double(matte[Int(y.rounded(.down)) * size + Int(x.rounded(.down))]) > 0.03 { reach = r }
                r += 1
            }
            reaches.append(reach)
            directions.append(direction)
        }
        func at(_ i: Int) -> Int { reaches[(i + ring) % ring] }
        let widened = reaches.indices.map { Swift.max(at($0 - 1), at($0), at($0 + 1)) }
        func wide(_ i: Int) -> Int { widened[(i + ring) % ring] }
        var out: [Double] = []
        for (i, reach) in reaches.enumerated() {
            let smooth = Double(wide(i - 2) + wide(i - 1) + wide(i) + wide(i + 1) + wide(i + 2)) / 5
            let radius = CallFaceJS.max(smooth, Double(reach), 0.9 * span) + 0.18 * span
            let x = CallFaceGeometry.clampValue(center.x + directions[i].x * radius, 0, Double(size))
            let y = CallFaceGeometry.clampValue(center.y + directions[i].y * radius, 0, Double(size))
            out.append(CallFaceJS.round(x * 10) / 10)
            out.append(CallFaceJS.round(y * 10) / 10)
        }
        return out
    }

    public static func polygons(_ p: [CallFacePoint]) -> CallFaceCustomPolygons {
        let left = CallFaceGeometry.centroid(p, [36, 37, 38, 39, 40, 41])
        let right = CallFaceGeometry.centroid(p, [42, 43, 44, 45, 46, 47])
        let span = CallFaceJS.or(CallFaceJS.hypot(right.x - left.x, right.y - left.y), eyeSpan)
        var forehead: [CallFacePoint] = []
        for i in stride(from: 26, through: 17, by: -1) { forehead.append(CallFacePoint(p[i].x, p[i].y - span * 1.2)) }
        let face = Array(p[0..<17]) + forehead
        let inner = Array(p[0..<17]) + p[17..<27].reversed().map { CallFacePoint($0.x, $0.y - span * 0.15) }
        let chin = p[8]
        let neckHalf = CallFaceJS.hypot(p[4].x - p[12].x, p[4].y - p[12].y) * 0.5
        let neck = [p[4], p[12], CallFacePoint(p[12].x + neckHalf * 0.08, chin.y + span * 1.1), CallFacePoint(p[4].x - neckHalf * 0.08, chin.y + span * 1.1)]
        return CallFaceCustomPolygons(face: face, inner: inner, neck: neck)
    }

    public static func rasterize(_ polygon: [CallFacePoint], size: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: size * size)
        for y in 0..<size {
            for x in 0..<size where CallFaceGeometry.insidePolygon(polygon, CallFacePoint(Double(x) + 0.5, Double(y) + 0.5)) {
                out[y * size + x] = 255
            }
        }
        return out
    }

    static func chroma(_ r: Double, _ g: Double, _ b: Double) -> (Double, Double) {
        let total = r + g + b + 7.65
        return (r / total, g / total)
    }

    public static func skinModel(pixels: [UInt8], size: Int, points: [CallFacePoint], span: Double) -> CallFaceSkinModel? {
        var samples: [(Double, Double)] = []
        let radius = Int(CallFaceJS.max(2, CallFaceJS.round(span * 0.12)))
        for (a, b) in [(2, 31), (14, 35), (3, 48), (13, 54)] {
            let cx = Int(CallFaceJS.round((points[a].x + points[b].x) / 2))
            let cy = Int(CallFaceJS.round((points[a].y + points[b].y) / 2))
            for y in (cy - radius)..<(cy + radius) {
                for x in (cx - radius)..<(cx + radius) {
                    if x < 0 || y < 0 || x >= size || y >= size { continue }
                    let at = (y * size + x) * 4
                    samples.append(chroma(Double(pixels[at]), Double(pixels[at + 1]), Double(pixels[at + 2])))
                }
            }
        }
        if samples.count < 8 { return nil }
        let n = Double(samples.count)
        var mean = [0.0, 0.0]
        for (u, v) in samples {
            mean[0] += u / n
            mean[1] += v / n
        }
        var a = 2e-5
        var b = 0.0
        var d = 2e-5
        for (u, v) in samples {
            a += ((u - mean[0]) * (u - mean[0])) / n
            b += ((u - mean[0]) * (v - mean[1])) / n
            d += ((v - mean[1]) * (v - mean[1])) / n
        }
        let det = CallFaceJS.or(a * d - b * b, 1e-12)
        return CallFaceSkinModel(mean: mean, inverse: [d / det, -b / det, a / det])
    }

    public static func skinLikeness(pixels: [UInt8], size: Int, model: CallFaceSkinModel?) -> [Float] {
        var out = [Float](repeating: 0, count: size * size)
        guard let model else { return out }
        let ia = model.inverse[0], ib = model.inverse[1], id = model.inverse[2]
        for i in 0..<out.count {
            let r = Double(pixels[i * 4])
            let g = Double(pixels[i * 4 + 1])
            let bl = Double(pixels[i * 4 + 2])
            let (u, v) = chroma(r, g, bl)
            let du = u - model.mean[0]
            let dv = v - model.mean[1]
            let distance = du * du * ia + 2 * du * dv * ib + dv * dv * id
            out[i] = Float(exp(-0.5 * distance / 9) * smoothstep(0.03, 0.12, (r + g + bl) / 765))
        }
        return out
    }

    public static func maskValues(_ bytes: [UInt8]) -> [Float] {
        bytes.map { Float(Double($0) / 255) }
    }

    public static func personWeights(_ bytes: [UInt8]) -> [Float] {
        bytes.map { Float(smoothstep(0.2, 0.8, Double($0) / 255)) }
    }

    public static func buildMatte(pixels: [UInt8], size: Int = size, points p: [CallFacePoint], person: [Float]? = nil, faceMask: [UInt8], innerMask: [UInt8], neckMask: [UInt8]) -> CallFaceCustomMatte {
        let left = CallFaceGeometry.centroid(p, [36, 37, 38, 39, 40, 41])
        let right = CallFaceGeometry.centroid(p, [42, 43, 44, 45, 46, 47])
        let span = CallFaceJS.or(CallFaceJS.hypot(right.x - left.x, right.y - left.y), eyeSpan)
        let center = CallFacePoint((left.x + right.x) / 2, (left.y + right.y) / 2)
        let chin = p[8]
        let face = boxBlur(maskValues(faceMask), size: size, radius: 3, passes: 2)
        let inner = boxBlur(maskValues(innerMask), size: size, radius: 2, passes: 2)
        let neckRaw = boxBlur(maskValues(neckMask), size: size, radius: 5, passes: 2)
        let faceWide = boxBlur(face, size: size, radius: 6, passes: 2)
        let likeness = boxBlur(skinLikeness(pixels: pixels, size: size, model: skinModel(pixels: pixels, size: size, points: p, span: span)), size: size, radius: 2, passes: 1)
        var matte = [Float](repeating: 0, count: size * size)
        var neck = [Float](repeating: 0, count: size * size)
        let full = Double(size)
        for yi in 0..<size {
            let y = Double(yi)
            let below = smoothstep(chin.y - span * 0.1, chin.y + span * 0.2, y)
            let fadeNeck = 1 - smoothstep(chin.y + span * 0.1, chin.y + span * 0.7, y)
            let above = 1 - smoothstep(p[4].y, p[5].y + span * 0.25, y)
            let bottom = 1 - smoothstep(full - span * 0.55, full - span * 0.05, y)
            for xi in 0..<size {
                let x = Double(xi)
                let i = yi * size + xi
                neck[i] = Float(Double(neckRaw[i]) * fadeNeck)
                let reach = CallFaceJS.hypot((x - center.x) / (span * 1.75), (y - center.y + span * 0.4) / (span * 2.3))
                let near = 1 - smoothstep(0.85, 1.05, reach)
                let hairReach = CallFaceJS.hypot((x - center.x) / (span * 1.9), (y - center.y - span * 0.2) / (span * 2.9))
                let hairZone = (1 - smoothstep(0.85, 1.05, hairReach)) * (1 - smoothstep(chin.y, chin.y + span * 0.4, y))
                let oval = 1 - smoothstep(0.8, 1, CallFaceJS.hypot((x - center.x) / (span * 1.3), (y - center.y + span * 0.45) / (span * 1.95)))
                let region = person != nil
                    ? CallFaceJS.max(above * near, Double(face[i]), Double(neck[i]), hairZone)
                    : CallFaceJS.max(Double(face[i]), Double(neck[i]), oval)
                let who = person.map { Double($0[i]) } ?? 1
                var value = CallFaceGeometry.clampValue(who * region, 0, 1)
                let keepNeck = CallFaceJS.max(smoothstep(0.08, 0.35, Double(likeness[i])), Double(face[i]))
                value *= 1 - below + below * keepNeck
                matte[i] = Float(value * bottom)
            }
        }
        var soft = boxBlur(matte, size: size, radius: 1, passes: 1)
        var skin = [Float](repeating: 0, count: size * size)
        var hair = [Float](repeating: 0, count: size * size)
        for i in 0..<soft.count {
            let m = CallFaceJS.max(Double(soft[i]), Double(matte[i]) > 0.97 ? 1 : 0) * (Double(matte[i]) > 0.001 || Double(soft[i]) > 0.05 ? 1 : 0)
            soft[i] = Float(m)
            let skinLike = smoothstep(0.2, 0.55, Double(likeness[i]))
            skin[i] = Float(CallFaceGeometry.clampValue(m * CallFaceJS.max(skinLike * CallFaceJS.max(Double(faceWide[i]), Double(neck[i])), Double(inner[i]) * 0.85), 0, 1))
            hair[i] = Float(CallFaceGeometry.clampValue(m * (1 - skinLike) * (1 - Double(inner[i])), 0, 1))
        }
        return CallFaceCustomMatte(
            matte: soft,
            skin: boxBlur(skin, size: size, radius: 2, passes: 1),
            hair: boxBlur(hair, size: size, radius: 1, passes: 1),
            head: headRing(soft, size: size, mid: center, span: span)
        )
    }

    public static func matteRGBA(_ built: CallFaceCustomMatte) -> [UInt8] {
        var out = [UInt8](repeating: 255, count: built.matte.count * 4)
        func byte(_ v: Float) -> UInt8 { UInt8(CallFaceGeometry.clampValue(CallFaceJS.round(Double(v) * 255), 0, 255)) }
        for i in 0..<built.matte.count {
            out[i * 4] = byte(built.matte[i])
            out[i * 4 + 1] = byte(built.skin[i])
            out[i * 4 + 2] = byte(built.hair[i])
        }
        return out
    }

    public static func validate(_ points: [CallFacePoint]?) throws -> [CallFacePoint] {
        guard let points, points.count >= CallFaceGeometry.landmarkCount else { throw CallFaceCustomError.faceNotFound }
        if !(CallFaceGeometry.faceFrame(points).span >= 12) { throw CallFaceCustomError.faceTooSmall }
        return points
    }

    public static func face(placed: [CallFacePoint], head: [Double]) -> CallFaceModel {
        CallFaceModel(id: id, label: label, points: placed.flatMap { [CallFaceJS.round($0.x * 10) / 10, CallFaceJS.round($0.y * 10) / 10] }, head: head)
    }
}
